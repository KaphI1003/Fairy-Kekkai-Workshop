#include "service/superres_pipeline.h"
#include "service/video_frame_io.h"

#include "service/video_frame_analysis.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "common/app_data.h"
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace fkw {
namespace {
template<typename T> class BoundedQueue {
public:
    bool push(T value, const std::function<bool()>& stop, const std::function<void()>& pulse = {}) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!closed_ && !items_.empty() && !stop()) {
            changed_.wait_for(lock, std::chrono::milliseconds(50));
            lock.unlock(); if (pulse) pulse(); lock.lock();
        }
        if (closed_ || stop()) return false;
        items_.push_back(std::move(value)); changed_.notify_all(); return true;
    }
    bool pop(T& value, const std::function<bool()>& stop, const std::function<void()>& pulse = {}) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!closed_ && items_.empty() && !stop()) {
            changed_.wait_for(lock, std::chrono::milliseconds(50));
            lock.unlock(); if (pulse) pulse(); lock.lock();
        }
        if (stop() || items_.empty()) return false;
        value = std::move(items_.front()); items_.pop_front(); changed_.notify_all(); return true;
    }
    void close() { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; changed_.notify_all(); }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<T> items_;
    bool closed_ = false;
};
QString executable(const QString& path) {
    return QFileInfo(path).isFile() ? QFileInfo(path).absoluteFilePath() : QStandardPaths::findExecutable(path);
}
class ProcessGuard {
public:
    QProcess process;
    ~ProcessGuard() {
        if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(5000); }
    }
};
}

struct SuperresPipeline::Impl {
    QString input;
    SuperresOptions options;
    SuperresVideoInfo info;
    qint64 first;
    qint64 total;
    QVector<QByteArray> hashes;
    QVector<qint64> representatives;
    QString cache;
    QSharedPointer<Logger> logger;
    const std::atomic<bool>& cancelled;
    std::atomic<bool> stopping{false}, hasError{false}, encoderFinished{false};
    mutable std::mutex errorMutex;
    QString errorMessage;
    BoundedQueue<SuperresBatch> prepared, toEncode;
    std::thread prepareThread, encodeThread;
    bool memoryFrames;
    QSize modelSize;

    Impl(const QString& source, const SuperresOptions& settings, const SuperresVideoInfo& video,
         qint64 start, qint64 frames, const QVector<QByteArray>& signatures,
         const QVector<qint64>& refs, const QString& root, const QSharedPointer<Logger>& log,
         const std::atomic<bool>& cancellation, bool memory)
        : input(source), options(settings), info(video), first(start), total(frames), hashes(signatures),
          representatives(refs), cache(root), logger(log), cancelled(cancellation), memoryFrames(memory) {
        modelSize = info.decodedSize * options.modelScale(info.size);
        if (memoryFrames) options.batchFrames = videoMemoryBatch(options.batchFrames,
            frames::MemoryFrame::pixelBytes(info.decodedSize.width(), info.decodedSize.height()),
            frames::MemoryFrame::pixelBytes(modelSize.width(), modelSize.height()));
    }

    bool stop() const { return cancelled.load() || stopping.load() || hasError.load(); }
    void fail(const QString& message) {
        { std::lock_guard<std::mutex> lock(errorMutex); if (errorMessage.isEmpty()) errorMessage = message; }
        hasError.store(true); prepared.close(); toEncode.close();
    }
    void launch() {
        try {
            prepareThread = std::thread([this]() {
                try { prepare(); }
                catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
                prepared.close();
            });
            encodeThread = std::thread([this]() {
                try { encode(); }
                catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
                encoderFinished.store(true);
            });
        } catch (...) { shutdown(); throw; }
    }
    void shutdown() {
        stopping.store(true); prepared.close(); toEncode.close();
        if (prepareThread.joinable()) prepareThread.join();
        if (encodeThread.joinable()) encodeThread.join();
    }

    void prepare() {
        const auto args = videoRgbDecodeArguments(input, info.decodedSize, info.fps, first, total);
        logger->info(QStringLiteral("[decode] ") + options.ffmpegPath + QLatin1Char(' ') + args.join(QLatin1Char(' ')));
        const qsizetype frameBytes = qsizetype(info.decodedSize.width()) * info.decodedSize.height() * 3;
        VideoRawDecoder decoder(executable(options.ffmpegPath), args, frameBytes, total, [this] { return stop(); });
        QByteArray pixels;
        for (qint64 offset = 0; offset < total && !stop(); offset += options.batchFrames) {
            const qint64 began = superresMonotonicMs();
            QElapsedTimer preparation; preparation.start();
            SuperresBatch batch;
            batch.offset = offset; batch.count = int(qMin<qint64>(options.batchFrames, total - offset));
            const QString stem = QStringLiteral("batch-%1").arg(offset / options.batchFrames, 6, 10, QLatin1Char('0'));
            batch.directory = QDir(cache).filePath(stem);
            batch.inputDirectory = QDir(batch.directory).filePath(QStringLiteral("unique-input"));
            batch.resultDirectory = QDir(batch.directory).filePath(QStringLiteral("unique-output"));
            batch.sequenceDirectory = QDir(batch.directory).filePath(QStringLiteral("enhanced"));
            batch.segmentPath = QDir(cache).filePath(QStringLiteral("segment-%1.mp4").arg(offset / options.batchFrames, 6, 10, QLatin1Char('0')));
            for (const auto& directory : {batch.inputDirectory, batch.resultDirectory, batch.sequenceDirectory})
                if (!QDir().mkpath(directory)) { fail(trText("无法创建预读帧目录。")); return; }
            for (int index = 0; index < batch.count && !stop(); ++index) {
                QString decodeError;
                if (!decoder.read(pixels, decodeError)) { if (!stop()) fail(decodeError); return; }
                const qint64 position = offset + index;
                if (options.reuseIdenticalFrames && videoFrameHash(pixels) != hashes.at(position)) {
                    fail(trText("连续解码与相同帧检测的画面不一致，已停止处理以避免复用错误画面。")); return;
                }
                const qint64 representative = representatives.at(position);
                batch.representatives.append(representative);
                if (representative != position) continue;
                if (memoryFrames) {
                    auto original = videoFrameFromRgb(pixels, info.decodedSize);
                    auto result = frames::MemoryFrame::create(modelSize.width(), modelSize.height());
                    batch.memoryInputs.insert(representative, original); batch.memoryResults.insert(representative, result);
                    batch.freshResults.insert(representative, QString::fromStdString(result->uri()));
                    continue;
                }
                const QString name = QStringLiteral("%1.png").arg(index, 8, 10, QLatin1Char('0'));
                const QImage image(reinterpret_cast<const uchar*>(pixels.constData()), info.decodedSize.width(), info.decodedSize.height(), info.decodedSize.width() * 3, QImage::Format_RGB888);
                QImageWriter writer(QDir(batch.inputDirectory).filePath(name), QByteArrayLiteral("png"));
                writer.setCompression(1);
                if (!writer.write(image)) { fail(trText("无法保存待超分的预读帧：") + writer.errorString()); return; }
                batch.freshResults.insert(representative, QDir(batch.resultDirectory).filePath(name));
            }
            if (stop()) return;
            logger->info(QStringLiteral("[prepare] offset=%1 frames=%2 fresh=%3 elapsed_ms=%4 begin_tick=%5 end_tick=%6")
                .arg(offset).arg(batch.count).arg(batch.freshResults.size()).arg(preparation.elapsed()).arg(began).arg(superresMonotonicMs()));
            if (!prepared.push(std::move(batch), [this]() { return stop(); })) return;
        }
        QString decodeError;
        if (!decoder.finish(decodeError) && !stop()) fail(decodeError);
    }

    void encode() {
        if (memoryFrames) { encodeMemory(); return; }
        SuperresBatch batch;
        while (toEncode.pop(batch, [this]() { return stop(); })) {
            const qint64 began = superresMonotonicMs();
            QElapsedTimer elapsed; elapsed.start();
            // Some packaged FFmpeg builds return zero when -n refuses an
            // existing target. Detect collisions independently of the exit code.
            if (QFileInfo::exists(batch.segmentPath)) { fail(trText("视频片段编码目标已存在，已停止处理。")); return; }
            ProcessGuard guard;
            auto& process = guard.process;
            process.setWorkingDirectory(QFileInfo(executable(options.ffmpegPath)).absolutePath());
            QStringList args{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-nostdin"),
                QStringLiteral("-framerate"), info.frameRate, QStringLiteral("-start_number"), QStringLiteral("0"),
                QStringLiteral("-i"), QDir(batch.sequenceDirectory).filePath(QStringLiteral("%08d.png")), QStringLiteral("-frames:v"), QString::number(batch.count),
                QStringLiteral("-vf"), superresScaleFilter(options.targetSize(info.size), options.fit), QStringLiteral("-c:v"), options.codec,
                QStringLiteral("-crf"), QString::number(options.crf()), QStringLiteral("-preset"), QStringLiteral("medium"),
                QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-colorspace"), QStringLiteral("bt709"),
                QStringLiteral("-color_primaries"), QStringLiteral("bt709"), QStringLiteral("-color_trc"), QStringLiteral("bt709"),
                QStringLiteral("-color_range"), QStringLiteral("tv"), QStringLiteral("-an"), QStringLiteral("-n"), batch.segmentPath};
            logger->info(QStringLiteral("[encode] ") + options.ffmpegPath + QLatin1Char(' ') + args.join(QLatin1Char(' ')));
            process.start(executable(options.ffmpegPath), args);
            if (!process.waitForStarted(10000)) { fail(trText("无法启动片段编码：") + process.errorString()); return; }
            QByteArray errors;
            do {
                process.waitForFinished(50); errors += process.readAllStandardError(); process.readAllStandardOutput();
                if (errors.size() > 8192) errors = errors.right(8192);
                if (stop()) return;
            } while (process.state() != QProcess::NotRunning);
            if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || QFileInfo(batch.segmentPath).size() <= 0) {
                fail(trText("视频片段编码失败：") + QString::fromUtf8(errors)); return;
            }
            logger->info(QStringLiteral("[encoded] offset=%1 frames=%2 elapsed_ms=%3 begin_tick=%4 end_tick=%5")
                .arg(batch.offset).arg(batch.count).arg(elapsed.elapsed()).arg(began).arg(superresMonotonicMs()));
            // All retained representatives have separate hard links outside this
            // batch, so its files can be removed while later inference proceeds.
            if (!QDir(batch.directory).removeRecursively()) { fail(trText("无法清理已编码的片段缓存。")); return; }
        }
    }

    void encodeMemory() {
        VideoRawEncoder encoder;
        SuperresBatch batch; qint64 submitted = 0; bool started = false;
        while (toEncode.pop(batch, [this] { return stop(); })) {
            const qint64 began = superresMonotonicMs();
            if (!started) {
                const auto args = videoRawEncodeArguments(modelSize, info.frameRate, total,
                    superresScaleFilter(options.targetSize(info.size), options.fit), options.codec, options.crf(), batch.segmentPath);
                QString error;
                logger->info(QStringLiteral("[encode-memory] ") + options.ffmpegPath + ' ' + args.join(' '));
                if (!encoder.start(executable(options.ffmpegPath), args, batch.segmentPath, error)) { fail(error); return; }
                started = true;
            }
            if (batch.memorySequence.size() != batch.count) { fail(trText("内存视频帧数不完整。")); return; }
            for (const auto& frame : batch.memorySequence) {
                QString error;
                if (!frame || frame->width() != modelSize.width() || frame->height() != modelSize.height() ||
                    !encoder.write(frame, [this] { return stop(); }, error)) { if (!stop()) fail(error.isEmpty() ? trText("内存帧尺寸不一致。") : error); return; }
                ++submitted;
            }
            batch.memorySequence.clear(); batch.memoryInputs.clear(); batch.memoryResults.clear();
            logger->info(QStringLiteral("[encode-submit] offset=%1 frames=%2 elapsed_ms=%3 begin_tick=%4 end_tick=%5")
                .arg(batch.offset).arg(batch.count).arg(superresMonotonicMs() - began).arg(began).arg(superresMonotonicMs()));
            if (!QDir(batch.directory).removeRecursively()) { fail(trText("无法清理内存批次清单。")); return; }
        }
        if (stop()) return;
        QString error;
        if (!started || submitted != total || !encoder.finish([this] { return stop(); }, error)) {
            if (!stop()) fail(error.isEmpty() ? trText("连续视频编码帧数不完整。") : error);
        } else logger->info(QStringLiteral("[encoded-stream] frames=%1").arg(submitted));
    }
};

SuperresPipeline::SuperresPipeline(const QString& input, const SuperresOptions& options,
    const SuperresVideoInfo& info, qint64 first, qint64 total, const QVector<QByteArray>& hashes,
    const QVector<qint64>& representatives, const QString& cache, const QSharedPointer<Logger>& logger,
    const std::atomic<bool>& cancelled, bool memoryFrames)
    : impl_(std::make_unique<Impl>(input, options, info, first, total, hashes, representatives, cache, logger, cancelled, memoryFrames)) { impl_->launch(); }
SuperresPipeline::~SuperresPipeline() { impl_->shutdown(); }
bool SuperresPipeline::next(SuperresBatch& batch, const std::function<void()>& pulse) {
    return impl_->prepared.pop(batch, [this]() { return impl_->stop(); }, pulse);
}
bool SuperresPipeline::encode(SuperresBatch batch, const std::function<void()>& pulse) {
    return impl_->toEncode.push(std::move(batch), [this]() { return impl_->stop(); }, pulse);
}
bool SuperresPipeline::finish(const std::function<void()>& pulse) {
    impl_->toEncode.close();
    while (!impl_->encoderFinished.load() && !impl_->stop()) { if (pulse) pulse(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    // A short/failed decode may be reported just after publishing its last batch.
    if (impl_->prepareThread.joinable()) impl_->prepareThread.join();
    if (impl_->encodeThread.joinable()) impl_->encodeThread.join();
    return !impl_->stop();
}
bool SuperresPipeline::failed() const { return impl_->hasError.load(); }
QString SuperresPipeline::error() const { std::lock_guard<std::mutex> lock(impl_->errorMutex); return impl_->errorMessage; }
} // namespace fkw

