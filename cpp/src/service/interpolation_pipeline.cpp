#include "service/interpolation_pipeline.h"
#include "service/video_frame_analysis.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QSet>
#include <QStandardPaths>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <cmath>
#include "common/app_data.h"

namespace fkw {
namespace {
QString program(const QString& value) { return QFileInfo(value).isFile() ? QFileInfo(value).absoluteFilePath() : QStandardPaths::findExecutable(value); }
template<typename T> class Queue {
public:
    bool push(T item, const std::function<bool()>& stop, const std::function<void()>& pulse = {}) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!closed_ && !items_.empty() && !stop()) {
            changed_.wait_for(lock, std::chrono::milliseconds(50)); lock.unlock(); if (pulse) pulse(); lock.lock();
        }
        if (closed_ || stop()) return false;
        items_.push_back(std::move(item)); changed_.notify_all(); return true;
    }
    bool pop(T& item, const std::function<bool()>& stop, const std::function<void()>& pulse = {}) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!closed_ && items_.empty() && !stop()) {
            changed_.wait_for(lock, std::chrono::milliseconds(50)); lock.unlock(); if (pulse) pulse(); lock.lock();
        }
        if (stop() || items_.empty()) return false;
        item = std::move(items_.front()); items_.pop_front(); changed_.notify_all(); return true;
    }
    void close() { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; changed_.notify_all(); }
private:
    std::mutex mutex_; std::condition_variable changed_; std::deque<T> items_; bool closed_ = false;
};
bool linkFrame(const QString& input, const QString& output) {
#ifdef Q_OS_WIN
    const std::filesystem::path from(input.toStdWString()), to(output.toStdWString());
#else
    const std::filesystem::path from(input.toStdString()), to(output.toStdString());
#endif
    std::error_code error; std::filesystem::create_hard_link(from, to, error);
    return !error || QFile::copy(input, output);
}
class Process {
public:
    QProcess value;
    ~Process() { if (value.state() != QProcess::NotRunning) { value.kill(); value.waitForFinished(5000); } }
};
}
InterpolationDecoder::InterpolationDecoder(const QString& input, const InterpolationOptions& options,
    const SuperresVideoInfo& info, qint64 first, qint64 count, const std::atomic<bool>& cancelled, std::function<bool()> stop)
    : count_(count) {
    const auto args = videoRgbDecodeArguments(input, info.size, info.fps, first, count);
    decoder_ = std::make_unique<VideoRawDecoder>(program(options.ffmpegPath), args,
        qsizetype(info.size.width()) * info.size.height() * 3, count,
        [&cancelled, stop] { return cancelled.load() || (stop && stop()); });
}
InterpolationDecoder::~InterpolationDecoder() = default;
bool InterpolationDecoder::read(QByteArray& pixels, QString& error) {
    if (!decoder_->read(pixels, error)) return false;
    return ++read_ != count_ || decoder_->finish(error);
}
bool analyzeInterpolationFrames(InterpolationDecoder& decoder, const QSize& size, const InterpolationOptions& options,
    QVector<QByteArray>& hashes, QVector<bool>& cuts, const std::atomic<bool>& cancelled,
    const std::function<void(qint64)>& progress, QString& error) {
    VideoCutDetector detectCut;
    if (options.protectCuts) {
        detectCut = [&](const QByteArray& previous, const QByteArray& current) {
            return interpolationSceneCut(previous, current, size, options.sceneThreshold);
        };
    }
    return analyzeVideoFrames(decoder, size, hashes, cancelled, detectCut, [&](qsizetype index, bool cut) {
        cuts[index] = cut;
        progress(index + 1);
    }, error);
}
struct InterpolationPipeline::Impl {
    QString input, rate, cache; InterpolationOptions options; SuperresVideoInfo info;
    qint64 first, sourceFrames; QVector<QByteArray> hashes; InterpolationPlan plan;
    QSharedPointer<Logger> logger; const std::atomic<bool>& cancelled;
    std::atomic<bool> stopping{false}, failed{false}, encoded{false};
    std::mutex errorMutex; QString error;
    Queue<InterpolationBatch> prepared, encoding;
    std::thread producer, encoder;
    bool memoryFrames;
    Impl(const QString& in, const InterpolationOptions& opt, const SuperresVideoInfo& video, const QString& fps,
        qint64 start, qint64 count, const QVector<QByteArray>& signatures, const InterpolationPlan& jobs,
        const QString& root, const QSharedPointer<Logger>& log, const std::atomic<bool>& cancel, bool memory)
        : input(in), rate(fps), cache(root), options(opt), info(video), first(start), sourceFrames(count), hashes(signatures), plan(jobs), logger(log), cancelled(cancel), memoryFrames(memory) {
        if (memoryFrames) {
            const auto bytes = frames::MemoryFrame::pixelBytes(info.size.width(), info.size.height());
            options.batchFrames = videoMemoryBatch(options.batchFrames, bytes * 2, uint64_t(std::ceil(interpolationRate(rate) / info.fps)) * bytes);
        }
    }
    bool stop() const { return stopping.load() || cancelled.load() || failed.load(); }
    void fail(const QString& message) {
        { std::lock_guard<std::mutex> lock(errorMutex); if (error.isEmpty()) error = message; }
        failed.store(true); prepared.close(); encoding.close();
    }
    void start() {
        try {
            producer = std::thread([this]() { try { prepare(); } catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); } prepared.close(); });
            encoder = std::thread([this]() { try { encode(); } catch (const std::exception& e) { fail(QString::fromUtf8(e.what())); } encoded.store(true); });
        } catch (...) { shutdown(); throw; }
    }
    void shutdown() {
        stopping.store(true); prepared.close(); encoding.close();
        if (producer.joinable()) producer.join(); if (encoder.joinable()) encoder.join();
    }
    void prepare() {
        InterpolationDecoder decoder(input, options, info, first, hashes.size(), cancelled, [this] { return stop(); });
        qint64 nextFrame = 0, output = 0; QByteArray lookahead;
        for (qint64 offset = 0; offset < sourceFrames && !stop(); offset += options.batchFrames) {
            const qint64 began = superresMonotonicMs();
            InterpolationBatch batch; batch.sourceOffset = offset; batch.outputOffset = output;
            const qint64 end = qMin<qint64>(sourceFrames, offset + options.batchFrames);
            batch.sourceCount = int(end - offset);
            const QString stem = QStringLiteral("batch-%1").arg(offset / options.batchFrames, 6, 10, QLatin1Char('0'));
            batch.directory = QDir(cache).filePath(stem); batch.sequenceDirectory = QDir(batch.directory).filePath("frames");
            batch.segmentPath = QDir(cache).filePath(stem + QStringLiteral(".mp4"));
            batch.manifest = QDir(batch.directory).filePath("jobs.tsv");
            const QString inputs = QDir(batch.directory).filePath("input");
            if (!QDir().mkpath(inputs) || !QDir().mkpath(batch.sequenceDirectory)) { fail(trText("无法创建补帧缓存。")); return; }
            QSet<qint64> needed;
            while (output < plan.frames.size() && plan.frames.at(output).left < end) {
                const auto& frame = plan.frames.at(output++); needed.insert(frame.left);
                if (frame.kind == InterpolationFrameKind::Infer) needed.insert(frame.right);
            }
            batch.outputCount = int(output - batch.outputOffset);
            QHash<qint64, QString> paths; QHash<QByteArray, QString> same;
            QHash<QString, VideoFramePtr> shared;
            const qint64 last = qMin<qint64>(end, hashes.size() - 1);
            for (qint64 index = offset; index <= last && !stop(); ++index) {
                QByteArray pixels; QString error;
                const auto& hash = hashes.at(index);
                if (index < nextFrame) pixels = lookahead;
                else {
                    if (!decoder.read(pixels, error)) { if (!stop()) fail(error); return; }
                    if (!hash.isEmpty() && videoFrameHash(pixels) != hash) {
                        fail(trText("补帧解码与分析画面不一致，已停止处理。")); return;
                    }
                    ++nextFrame;
                }
                if (index == last) lookahead = pixels;
                if (!needed.contains(index)) continue;
                if (!hash.isEmpty() && same.contains(hash)) {
                    paths.insert(index, same.value(hash));
                    if (memoryFrames) batch.memoryInputs.insert(index, shared.value(same.value(hash)));
                    continue;
                }
                if (memoryFrames) {
                    auto original = videoFrameFromRgb(pixels, info.size);
                    const QString path = QString::fromStdString(original->uri());
                    batch.memoryInputs.insert(index, original); paths.insert(index, path); shared.insert(path, original);
                    if (!hash.isEmpty()) same.insert(hash, path);
                    continue;
                }
                const QString path = QDir(inputs).filePath(QStringLiteral("%1.png").arg(index, 10, 10, QLatin1Char('0')));
                const QImage image(reinterpret_cast<const uchar*>(pixels.constData()), info.size.width(), info.size.height(), info.size.width() * 3, QImage::Format_RGB888);
                QImageWriter writer(path, "png"); writer.setCompression(1);
                if (!writer.write(image)) { fail(trText("无法保存补帧输入：") + writer.errorString()); return; }
                paths.insert(index, path); if (!hash.isEmpty()) same.insert(hash, path);
            }
            QByteArray jobs;
            for (int index = 0; index < batch.outputCount && !stop(); ++index) {
                const auto& frame = plan.frames.at(batch.outputOffset + index);
                QString path = QDir(batch.sequenceDirectory).filePath(QStringLiteral("%1.png").arg(index, 8, 10, QLatin1Char('0')));
                if (frame.kind == InterpolationFrameKind::Infer) {
                    if (memoryFrames) {
                        auto result = frames::MemoryFrame::create(info.size.width(), info.size.height());
                        path = QString::fromStdString(result->uri()); batch.memorySequence.append(result);
                    }
                    jobs += (QString::number(frame.time, 'f', 9) + QLatin1Char('\t') + paths.value(frame.left) + QLatin1Char('\t') + paths.value(frame.right) + QLatin1Char('\t') + path + QLatin1Char('\n')).toUtf8();
                    ++batch.inferenceCount;
                } else if (memoryFrames) batch.memorySequence.append(batch.memoryInputs.value(frame.left));
                else if (!linkFrame(paths.value(frame.left), path)) { fail(trText("无法复用补帧画面。")); return; }
            }
            QFile manifest(batch.manifest);
            if (!manifest.open(QIODevice::WriteOnly) || manifest.write(jobs) != jobs.size()) { fail(trText("无法写入补帧任务清单。")); return; }
            manifest.close();
            logger->info(QStringLiteral("[prepare] source=%1 output=%2 jobs=%3 begin_tick=%4 end_tick=%5")
                .arg(offset).arg(batch.outputCount).arg(batch.inferenceCount).arg(began).arg(superresMonotonicMs()));
            if (!prepared.push(std::move(batch), [this]() { return stop(); })) return;
        }
    }
    void encode() {
        if (memoryFrames) { encodeMemory(); return; }
        InterpolationBatch batch;
        while (encoding.pop(batch, [this]() { return stop(); })) {
            const qint64 began = superresMonotonicMs();
            if (QFileInfo::exists(batch.segmentPath)) { fail(trText("补帧编码目标已存在。")); return; }
            Process guard; auto& process = guard.value;
            QStringList args{"-hide_banner", "-loglevel", "error", "-nostdin", "-framerate", rate, "-start_number", "0", "-i",
                QDir(batch.sequenceDirectory).filePath("%08d.png"), "-frames:v", QString::number(batch.outputCount), "-vf", superresScaleFilter(info.size, "pad"),
                "-c:v", options.codec, "-crf", QString::number(options.crf()), "-preset", "medium", "-pix_fmt", "yuv420p",
                "-colorspace", "bt709", "-color_primaries", "bt709", "-color_trc", "bt709", "-color_range", "tv", "-an", "-n", batch.segmentPath};
            process.start(program(options.ffmpegPath), args);
            if (!process.waitForStarted(10000)) { fail(trText("无法启动补帧编码：") + process.errorString()); return; }
            QByteArray errors;
            do {
                process.waitForFinished(50); errors += process.readAllStandardError(); process.readAllStandardOutput();
                if (errors.size() > 8192) errors = errors.right(8192);
                if (stop()) return;
            } while (process.state() != QProcess::NotRunning);
            if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || QFileInfo(batch.segmentPath).size() <= 0) {
                fail(trText("补帧编码失败：") + QString::fromUtf8(errors)); return;
            }
            logger->info(QStringLiteral("[encoded] source=%1 begin_tick=%2 end_tick=%3").arg(batch.sourceOffset).arg(began).arg(superresMonotonicMs()));
            if (!QDir(batch.directory).removeRecursively()) { fail(trText("无法清理补帧片段缓存。")); return; }
        }
    }
    void encodeMemory() {
        VideoRawEncoder encoder; InterpolationBatch batch; bool started = false; qint64 submitted = 0;
        while (encoding.pop(batch, [this] { return stop(); })) {
            const qint64 began = superresMonotonicMs();
            if (!started) {
                const auto args = videoRawEncodeArguments(info.size, rate, plan.frames.size(), superresScaleFilter(info.size, "pad"),
                    options.codec, options.crf(), batch.segmentPath);
                QString error; logger->info("[encode-memory] " + options.ffmpegPath + ' ' + args.join(' '));
                if (!encoder.start(program(options.ffmpegPath), args, batch.segmentPath, error)) { fail(error); return; }
                started = true;
            }
            if (batch.memorySequence.size() != batch.outputCount) { fail(trText("内存补帧序列不完整。")); return; }
            for (const auto& frame : batch.memorySequence) {
                QString error;
                if (!frame || frame->width() != info.size.width() || frame->height() != info.size.height() ||
                    !encoder.write(frame, [this] { return stop(); }, error)) { if (!stop()) fail(error.isEmpty() ? trText("内存补帧尺寸不一致。") : error); return; }
                ++submitted;
            }
            batch.memorySequence.clear(); batch.memoryInputs.clear();
            logger->info(QStringLiteral("[encode-submit] source=%1 begin_tick=%2 end_tick=%3").arg(batch.sourceOffset).arg(began).arg(superresMonotonicMs()));
            if (!QDir(batch.directory).removeRecursively()) { fail(trText("无法清理补帧内存批次清单。")); return; }
        }
        if (stop()) return;
        QString error;
        if (!started || submitted != plan.frames.size() || !encoder.finish([this] { return stop(); }, error)) {
            if (!stop()) fail(error.isEmpty() ? trText("连续补帧编码帧数不完整。") : error);
        } else logger->info(QStringLiteral("[encoded-stream] frames=%1").arg(submitted));
    }
};
InterpolationPipeline::InterpolationPipeline(const QString& input, const InterpolationOptions& options, const SuperresVideoInfo& info,
    const QString& rate, qint64 first, qint64 sourceFrames, const QVector<QByteArray>& hashes, const InterpolationPlan& plan,
    const QString& cache, const QSharedPointer<Logger>& logger, const std::atomic<bool>& cancelled, bool memoryFrames)
    : impl_(std::make_unique<Impl>(input, options, info, rate, first, sourceFrames, hashes, plan, cache, logger, cancelled, memoryFrames)) { impl_->start(); }
InterpolationPipeline::~InterpolationPipeline() { impl_->shutdown(); }
bool InterpolationPipeline::next(InterpolationBatch& batch, const std::function<void()>& pulse) { return impl_->prepared.pop(batch, [this]() { return impl_->stop(); }, pulse); }
bool InterpolationPipeline::encode(InterpolationBatch batch, const std::function<void()>& pulse) { return impl_->encoding.push(std::move(batch), [this]() { return impl_->stop(); }, pulse); }
bool InterpolationPipeline::finish(const std::function<void()>& pulse) {
    impl_->encoding.close();
    while (!impl_->encoded.load() && !impl_->stop()) { pulse(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    if (impl_->producer.joinable()) impl_->producer.join(); if (impl_->encoder.joinable()) impl_->encoder.join();
    return !impl_->stop();
}
bool InterpolationPipeline::failed() const { return impl_->failed.load(); }
QString InterpolationPipeline::error() const { std::lock_guard<std::mutex> lock(impl_->errorMutex); return impl_->error; }
}

