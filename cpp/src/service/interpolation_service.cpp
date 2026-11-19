#include "service/interpolation_service.h"
#include "service/interpolation_pipeline.h"
#include "service/video_gpu_gate.h"
#include "common/app_data.h"
#include "common/config.h"
#include "common/event_bus.h"
#include "common/logger.h"
#include "common/setting.h"
#include "common/task_status.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <algorithm>
#include <opencv2/imgproc.hpp>

namespace fkw {
namespace {
QString executable(const QString& path) { return QFileInfo(path).isFile() ? QFileInfo(path).absoluteFilePath() : QStandardPaths::findExecutable(path); }
QString number(double value) { return QString::number(value, 'f', 9); }
double sceneScore(const QByteArray& previous, const QByteArray& current, const QSize& size) {
    if (previous.isEmpty()) return 0;
    qint64 difference = 0, samples = 0;
    const auto* a = reinterpret_cast<const uchar*>(previous.constData());
    const auto* b = reinterpret_cast<const uchar*>(current.constData());
    for (int y = 0; y < size.height(); y += qMax(1, size.height() / 36))
        for (int x = 0; x < size.width(); x += qMax(1, size.width() / 64)) {
            const qsizetype p = (qsizetype(y) * size.width() + x) * 3;
            for (int c = 0; c < 3; ++c) { difference += std::abs(int(a[p + c]) - int(b[p + c])); ++samples; }
        }
    return qMin(1.0, difference * 3.0 / (samples * 255));
}
cv::Mat sceneThumbnail(const QByteArray& pixels, const QSize& size, const QSize& thumbnail) {
    const cv::Mat source(size.height(), size.width(), CV_8UC3, const_cast<char*>(pixels.constData()));
    cv::Mat result;
    cv::resize(source, result, cv::Size(thumbnail.width(), thumbnail.height()), 0, 0, cv::INTER_AREA);
    return result;
}

bool matchesAcrossMotion(const cv::Mat& previous, const cv::Mat& current) {
    const int width = current.cols, height = current.rows;
    const int patchSize = std::clamp(std::min(width, height) / 6, 8, 18);
    const int radiusX = width / 6, radiusY = height / 6;
    const cv::Rect bounds(0, 0, width, height);
    int textured = 0, matched = 0;
    bool left = false, right = false, top = false, bottom = false;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 5; ++column) {
            const int x = (width - patchSize) * column / 4;
            const int y = (height - patchSize) * row / 3;
            const cv::Mat patch = current(cv::Rect(x, y, patchSize, patchSize));
            cv::Scalar mean, deviation;
            cv::meanStdDev(patch, mean, deviation);
            const double contrast = std::sqrt(deviation.dot(deviation) / 3);
            // Flat patches can match unrelated backgrounds by color alone.
            if (contrast < 10) continue;
            ++textured;
            const cv::Rect search = cv::Rect(x - radiusX, y - radiusY,
                patchSize + 2 * radiusX, patchSize + 2 * radiusY) & bounds;
            cv::Mat errors;
            cv::matchTemplate(previous(search), patch, errors, cv::TM_SQDIFF);
            double best = 0;
            cv::minMaxLoc(errors, &best);
            const double tolerance = std::min(24.0, 4 + contrast * .65);
            if (best > tolerance * tolerance * patch.total() * 3) continue;
            ++matched;
            left |= column < 2; right |= column > 2;
            top |= row < 2; bottom |= row >= 2;
        }
    }
    // A fixed logo or a single persistent object is insufficient evidence of continuity.
    return matched >= 4 && matched * 2 >= textured && left && right && top && bottom;
}
class EngineSession {
public:
    QProcess process;
    QByteArray lines, errors;
    QSharedPointer<Logger> logger;
    explicit EngineSession(const QSharedPointer<Logger>& log) : logger(log) {}
    ~EngineSession() { if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(5000); } flush(); }
    void flush() { if (process.isOpen()) errors += process.readAllStandardError(); if (!errors.isEmpty()) logger->info(QString::fromUtf8(errors)); errors.clear(); }
    bool wait(const QByteArray& expected, const std::function<bool()>& stopped, const std::function<void(int)>& pulse, int count = 0) {
        QElapsedTimer noProgress; noProgress.start(); int done = 0;
        for (;;) {
            lines += process.readAllStandardOutput(); errors += process.readAllStandardError();
            if (errors.size() > 16384) { logger->info(QString::fromUtf8(errors)); errors.clear(); }
            qsizetype end;
            while ((end = lines.indexOf('\n')) >= 0) {
                const QByteArray line = lines.left(end).trimmed(); lines.remove(0, end + 1);
                if (line == "DONE") { ++done; if (done > count) return false; noProgress.restart(); pulse(done); }
                else if (line == expected) { flush(); return done == count; }
                else if (line == "ERROR") return false;
                else return false;
            }
            if (stopped() || process.state() == QProcess::NotRunning || noProgress.elapsed() > 120000) return false;
            pulse(done); process.waitForReadyRead(50);
        }
    }
};
}
bool interpolationSceneCut(const QByteArray& previous, const QByteArray& current, const QSize& size, int threshold) {
    if (sceneScore(previous, current, size) < threshold / 100.0) return false;
    const QSize thumbnail = size.scaled(size.boundedTo(QSize(192, 192)), Qt::KeepAspectRatio);
    if (std::min(thumbnail.width(), thumbnail.height()) < 24) return true;
    return !matchesAcrossMotion(sceneThumbnail(previous, size, thumbnail), sceneThumbnail(current, size, thumbnail));
}
double interpolationRate(const QString& value) {
    const auto parts = value.split('/'); const double denominator = parts.size() == 2 ? parts.at(1).toDouble() : 1;
    return denominator > 0 ? parts.first().toDouble() / denominator : 0;
}
SuperresVideoInfo parseInterpolationVideoInfo(const QJsonObject& json) {
    auto info = parseSuperresVideoInfo(json);
    for (const auto& value : json.value("streams").toArray()) {
        const auto stream = value.toObject();
        if (stream.value("codec_type") != "video" || stream.value("disposition").toObject().value("attached_pic").toInt()) continue;
        const QString nominal = stream.value("r_frame_rate").toString(); const double rate = interpolationRate(nominal);
        // Some CFR MP4s shorten their last packet, slightly perturbing avg FPS.
        // Preserve the nominal rational grid instead of needlessly inferring
        // every original frame at a nearly-zero fractional timestep.
        if (!info.variableFrameRate && rate > 0 && std::abs(rate - info.fps) <= .0001) { info.frameRate = nominal; info.fps = rate; }
        // A CFR transcode can copy the rounded average into both rate fields.
        // Bound the timing change over the entire input, including for previews.
        if (!info.variableFrameRate && info.exactFrames && info.fps > 0) {
            for (const QString& grid : {QStringLiteral("24/1"), QStringLiteral("25/1"), QStringLiteral("30/1"),
                     QStringLiteral("48/1"), QStringLiteral("50/1"), QStringLiteral("60/1"),
                     QStringLiteral("100/1"), QStringLiteral("120/1"), QStringLiteral("240/1"),
                     QStringLiteral("24000/1001"), QStringLiteral("30000/1001"), QStringLiteral("48000/1001"),
                     QStringLiteral("60000/1001"), QStringLiteral("120000/1001")}) {
                const double fps = interpolationRate(grid);
                if (std::abs(info.fps - fps) <= .0001 &&
                    std::abs(info.frames / info.fps - info.frames / fps) <= .001) {
                    info.frameRate = grid;
                    info.fps = fps;
                    break;
                }
            }
        }
        break;
    }
    return info;
}
InterpolationOptions InterpolationOptions::fromConfig() {
    InterpolationOptions result; const auto& config = AppConfig::instance();
    const auto get = [&config](const char* name, const QJsonValue& fallback) { return config.value("Interpolation", QString::fromLatin1(name), fallback); };
    result.rateMode = get("RateMode", result.rateMode).toString(); result.presetRate = get("PresetRate", result.presetRate).toString();
    result.multiplier = get("Multiplier", result.multiplier).toInt(); result.customRate = get("CustomRate", result.customRate).toInt();
    result.model = get("Model", result.model).toString(); result.reuseHolds = get("ReuseHolds", true).toBool(); result.protectCuts = get("ProtectCuts", true).toBool();
    result.sceneThreshold = get("SceneThreshold", 30).toInt(); result.gpu = get("Gpu", -1).toInt(); result.batchFrames = get("BatchFrames", 16).toInt();
    result.spatialTta = get("SpatialTta", false).toBool(); result.temporalTta = get("TemporalTta", false).toBool(); result.uhd = get("Uhd", false).toBool();
    result.quality = get("Quality", result.quality).toString(); result.codec = get("Codec", result.codec).toString();
    result.audio = get("Audio", result.audio).toString(); result.container = get("Container", result.container).toString();
    const auto video = SuperresOptions::fromConfig(); result.ffmpegPath = video.ffmpegPath;
    result.ffprobePath = get("FFprobePath", video.ffprobePath).toString();
    result.enginePath = get("EnginePath", QDir(sourceRoot()).filePath("tools/rife/fkw-rife-bridge" + executableSuffix())).toString();
    result.modelsPath = get("ModelsPath", QDir(QFileInfo(result.enginePath).absolutePath()).filePath("models")).toString();
    result.tempPath = get("TempPath", QDir(userDataFolder()).filePath("InterpolationTemp")).toString(); return result;
}
QString InterpolationOptions::targetRate(const QString& source) const {
    if (rateMode == "custom") return QString::number(customRate) + "/1";
    if (rateMode != "multiply") return presetRate;
    const auto parts = source.split('/'); qint64 numerator = parts.value(0).toLongLong() * multiplier;
    qint64 denominator = parts.size() == 2 ? parts.at(1).toLongLong() : 1;
    if (numerator <= 0 || denominator <= 0) return {};
    const auto divisor = std::gcd(numerator, denominator); return QString::number(numerator / divisor) + '/' + QString::number(denominator / divisor);
}
int InterpolationOptions::crf() const { return quality == "high" ? 18 : quality == "small" ? 25 : 21; }
QString InterpolationOptions::validate(const SuperresVideoInfo& source) const {
    if (!QStringList{"preset", "multiply", "custom"}.contains(rateMode) || !QStringList{"60/1", "60000/1001", "120/1", "120000/1001", "50/1", "30/1"}.contains(presetRate) ||
        multiplier < 2 || multiplier > 4 || customRate < 1 || customRate > 240 || !QStringList{"rife-v4.6", "rife-v4"}.contains(model) ||
        !QStringList{"balanced", "high", "small"}.contains(quality) || !QStringList{"libx264", "libx265"}.contains(codec) ||
        !QStringList{"copy", "aac", "none"}.contains(audio) || !QStringList{"mp4", "mkv"}.contains(container) ||
        gpu < -1 || gpu > 15 || batchFrames < 1 || batchFrames > 120 || sceneThreshold < 5 || sceneThreshold > 80)
        return trText("补帧参数无效，请重新选择。");
    if (source.fps > 0) {
        const double rate = interpolationRate(targetRate(source.frameRate));
        if (rate <= source.fps + .000001) return trText("目标帧率必须高于原视频帧率。");
        if (rate > 240.000001 || rate > source.fps * 16 + .000001) return trText("目标最高为 240 fps，单次最多提高到原帧率的 16 倍。");
    }
    if (executable(enginePath).isEmpty()) return trText("未找到补帧引擎，请在高级设置中选择 fkw-rife-bridge 程序。");
    if (executable(ffmpegPath).isEmpty() || executable(ffprobePath).isEmpty()) return trText("未找到 FFmpeg 或 ffprobe，请检查工具路径。");
    for (const auto& suffix : {"flownet.param", "flownet.bin"})
        if (!QFileInfo(QDir(modelsPath).filePath(model + '/' + suffix)).isFile()) return trText("补帧模型文件不完整，请检查模型目录。");
    return {};
}
InterpolationPlan planInterpolation(qint64 count, const QString& source, const QString& target,
    const QVector<QByteArray>& hashes, const QVector<bool>& cuts, const InterpolationOptions& options) {
    const long double ratio = static_cast<long double>(interpolationRate(source)) / interpolationRate(target);
    if (count <= 0 || ratio <= 0 || !std::isfinite(ratio) || hashes.size() < count) throw std::runtime_error("Invalid interpolation timeline");
    const qint64 outputs = qint64(std::ceil(count / ratio - 1e-9L));
    InterpolationPlan plan; plan.frames.reserve(outputs);
    for (qint64 index = 0; index < outputs; ++index) {
        long double position = index * ratio;
        const long double nearest = std::round(position);
        if (nearest < count && std::abs(position - nearest) < 1e-8L) position = nearest;
        InterpolationFrame frame; frame.left = qMin<qint64>(count - 1, qint64(std::floor(position)));
        frame.right = qMin<qint64>(hashes.size() - 1, frame.left + 1); frame.time = double(position - frame.left);
        if (frame.time < 1e-8 || frame.left == frame.right) { frame.kind = InterpolationFrameKind::Original; ++plan.originals; }
        else if (options.reuseHolds && !hashes.at(frame.left).isEmpty() && hashes.at(frame.left) == hashes.at(frame.right)) { frame.kind = InterpolationFrameKind::Hold; ++plan.holds; }
        else if (options.protectCuts && frame.right < cuts.size() && cuts.at(frame.right)) { frame.kind = InterpolationFrameKind::Cut; ++plan.cuts; }
        else { frame.kind = InterpolationFrameKind::Infer; ++plan.inference; }
        plan.frames.append(frame);
    }
    return plan;
}
InterpolationTask::InterpolationTask(const QString& input, const QString& output, InterpolationOptions settings) : TaskBase(input, output), options(std::move(settings)) {}
InterpolationWorker::InterpolationWorker(std::shared_ptr<InterpolationTask> task) : task_(std::move(task)) {}
void InterpolationWorker::cancel() { cancelled_.store(true); }
void InterpolationWorker::cancelQueued() { cancel(); emit completed(false, true, task_->outputPath, {}); deleteLater(); }
void InterpolationWorker::report(const QString& stage, qint64 done, bool complete) {
    done_ = qMax(done_, done);
    const double elapsed = wall_.elapsed() / 1000.0, fps = speed_.update(wall_.elapsed(), done_);
    const double remaining = fps > 0 && done_ < work_ ? (work_ - done_) / fps : -1;
    const int percent = complete ? 100 : work_ > 0 ? qMin(99, int(done_ * 100 / work_)) : work_ == 0 ? 99 : 0;
    emit progress(percent, stage, done_, qMax<qint64>(0, work_), fps, elapsed, remaining);
    if (!task_->preview) {
        auto& bus = GlobalEventBus::instance(); emit bus.taskStageChangedSig(task_->taskId, stage + (summary_.isEmpty() ? QString() : " · " + summary_));
        const QString detail = work_ < 0 ? trText("统计中") : work_ == 0 ? trText("无需模型补帧") :
            QStringLiteral("%1 %2/%3 · %4 %5 %6").arg(trText("已处理")).arg(done_).arg(work_).arg(trText("剩余")).arg(qMax<qint64>(0, work_ - done_)).arg(trText("帧"));
        const QString estimate = work_ >= 0 && done_ >= work_ ? trText("收尾中") : trText("补帧剩余") + QLatin1Char(' ') +
            (remaining >= 0 ? QStringLiteral("%1s").arg(int(std::ceil(remaining))) : trText("估算中"));
        emit bus.updateTaskStatusSig(task_->taskId, percent, int(TaskStatus::Processing), detail, elapsed, estimate, fps);
    }
}
bool InterpolationWorker::execute(const QString& file, const QStringList& args, QByteArray* output, int timeout) {
    if (cancelled_) return false;
    QProcess process; logger_->info(file + ' ' + args.join(' ')); process.start(executable(file), args);
    if (!process.waitForStarted(10000)) { error_ = trText("无法启动视频工具：") + process.errorString(); return false; }
    QElapsedTimer timer; timer.start(); QByteArray errors;
    do {
        process.waitForFinished(50); if (output) *output += process.readAllStandardOutput(); else process.readAllStandardOutput();
        errors += process.readAllStandardError(); if (errors.size() > 16384) errors = errors.right(16384);
        if (cancelled_ || (timeout > 0 && timer.elapsed() > timeout)) { process.kill(); process.waitForFinished(5000); if (!cancelled_) error_ = trText("视频工具运行超时。"); return false; }
    } while (process.state() != QProcess::NotRunning);
    logger_->info(QString::fromUtf8(errors));
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) { error_ = trText("视频处理失败：") + QString::fromUtf8(errors.right(3000)); return false; }
    return true;
}
bool InterpolationWorker::process() {
    const auto& options = task_->options; error_ = options.validate(); if (!error_.isEmpty()) return false;
    if (!QFileInfo(task_->inputPath).isFile() || QFileInfo::exists(task_->outputPath)) { error_ = trText("输入视频不存在或输出文件已经存在。"); return false; }
    report(trText("读取视频信息")); QByteArray json;
    if (!execute(options.ffprobePath, superresProbeArguments(task_->inputPath), &json, 15000)) return false;
    auto info = parseInterpolationVideoInfo(QJsonDocument::fromJson(json).object());
    if (info.fps <= 0 || info.frames <= 0 || info.size.isEmpty()) { error_ = trText("无法读取视频信息。"); return false; }
    if (info.variableFrameRate || info.hdr) { error_ = trText("当前补帧支持固定帧率 SDR 视频，请先在压制中转换。"); return false; }
    if (!info.exactFrames) {
        QByteArray counts;
        if (!execute(options.ffprobePath, {"-v", "error", "-count_packets", "-select_streams", "v:0", "-show_entries", "stream=nb_read_packets", "-of", "json", task_->inputPath}, &counts, 60000)) return false;
        const auto streams = QJsonDocument::fromJson(counts).object().value("streams").toArray();
        info.frames = streams.isEmpty() ? 0 : streams.first().toObject().value("nb_read_packets").toString().toLongLong();
        if (info.frames <= 0) { error_ = trText("无法确定视频帧数。"); return false; }
    }
    error_ = options.validate(info); if (!error_.isEmpty()) return false;
    const QString rate = options.targetRate(info.frameRate);
    const qint64 first = task_->preview ? qMax<qint64>(0, qint64(std::floor(task_->previewStart * info.fps + 1e-6))) : 0;
    const qint64 count = qMin(info.frames - first, task_->preview ? qint64(std::ceil(task_->previewDuration * info.fps - 1e-6)) : info.frames);
    if (count <= 0) { error_ = trText("预览起点已经超出视频时长。"); return false; }
    const qint64 available = count + (first + count < info.frames ? 1 : 0);
    QVector<QByteArray> hashes(available); QVector<bool> cuts(available, false);
    if (options.reuseHolds || options.protectCuts) {
        QElapsedTimer analysisTime; analysisTime.start();
        InterpolationDecoder decoder(task_->inputPath, options, info, first, available, cancelled_);
        report(trText("分析画面"));
        if (!analyzeInterpolationFrames(decoder, info.size, options, hashes, cuts, cancelled_, [&](qint64 done) {
            if (pulse_.elapsed() > 200 || done == available) {
                pulse_.restart(); report(trText("分析画面") + QStringLiteral(" %1/%2").arg(done).arg(available));
            }
        }, error_)) return false;
        logger_->info(QStringLiteral("[analysis] frames=%1 elapsed_ms=%2").arg(available).arg(analysisTime.elapsed()));
    }
    const auto plan = planInterpolation(count, info.frameRate, rate, hashes, cuts, options); work_ = plan.inference;
    summary_ = QStringLiteral("%1 %2 → %3 %4 · %5 %6 %4 · %7 %8 %4")
        .arg(trText("原始")).arg(count).arg(plan.frames.size()).arg(trText("帧"))
        .arg(trText("静止复用")).arg(plan.holds).arg(trText("切镜保护")).arg(plan.cuts);
    logger_->info(QStringLiteral("Interpolation plan: source=%1 output=%2 infer=%3 holds=%4 cuts=%5 rate=%6 direct=%7 source_rate=%8")
        .arg(count).arg(plan.frames.size()).arg(work_).arg(plan.holds).arg(plan.cuts).arg(rate).arg(plan.originals).arg(info.frameRate));
    emit statistics(count, plan.frames.size(), work_, plan.holds, plan.cuts);
    if (!QDir().mkpath(options.tempPath) || !QDir().mkpath(QFileInfo(task_->outputPath).absolutePath())) { error_ = trText("无法创建补帧输出或临时目录。"); return false; }
    QTemporaryDir cache(QDir(options.tempPath).filePath("render-XXXXXX")); if (!cache.isValid()) { error_ = trText("无法创建补帧缓存。"); return false; }
    VideoGpuLease gpu;
    if (work_ > 0 && !gpu.acquire(cancelled_, [&]() { if (pulse_.elapsed() > 200) { pulse_.restart(); report(trText("等待显卡资源")); } })) return false;
    speed_.start(wall_.elapsed());
    const bool memoryFrames = videoMemoryTransport();
    logger_->info(QStringLiteral("[transport] ") + (memoryFrames ? QStringLiteral("shared-rgb24") : QStringLiteral("png")));
    InterpolationPipeline pipeline(task_->inputPath, options, info, rate, first, count, hashes, plan, cache.path(), logger_, cancelled_, memoryFrames);
    EngineSession engine(logger_);
    const auto stopped = [&]() { return cancelled_.load() || pipeline.failed(); };
    const auto fail = [&]() { error_ = pipeline.failed() ? pipeline.error() : trText("补帧引擎运行失败：") + QString::fromUtf8(engine.errors).right(2000); return false; };
    if (work_ > 0) {
        report(trText("加载补帧模型"));
        auto environment = QProcessEnvironment::systemEnvironment(); environment.insert("PATH", QCoreApplication::applicationDirPath() + ';' + sourceRoot() + ';' + environment.value("PATH"));
        engine.process.setProcessEnvironment(environment);
        const QStringList arguments{QDir(options.modelsPath).filePath(options.model), QString::number(options.gpu), QString::number(options.spatialTta), QString::number(options.temporalTta), QString::number(options.uhd)};
        logger_->info("[engine-start] " + options.enginePath + ' ' + arguments.join(' '));
        engine.process.start(executable(options.enginePath), arguments);
        if (!engine.process.waitForStarted(10000) || !engine.wait("READY", stopped, [&](int) { if (pulse_.elapsed() > 200) { pulse_.restart(); report(trText("加载补帧模型")); } })) return fail();
    }
    QStringList segments; qint64 finishedWork = 0;
    for (qint64 processed = 0; processed < count;) {
        InterpolationBatch batch;
        if (!pipeline.next(batch, [&]() { if (pulse_.elapsed() > 200) { pulse_.restart(); report(trText("准备补帧片段"), finishedWork); } })) return fail();
        if (batch.inferenceCount > 0) {
            report(trText("补帧处理"), finishedWork); const qint64 begin = superresMonotonicMs();
            const QByteArray command = "BATCH\t" + batch.manifest.toUtf8() + '\n';
            if (engine.process.write(command) != command.size() || !engine.wait("BATCH_DONE", stopped, [&](int ready) {
                if (pulse_.elapsed() > 200 || ready == batch.inferenceCount) { pulse_.restart(); report(trText("补帧处理"), finishedWork + ready); }
            }, batch.inferenceCount)) return fail();
            logger_->info(QStringLiteral("[inferred] source=%1 jobs=%2 begin_tick=%3 end_tick=%4").arg(batch.sourceOffset).arg(batch.inferenceCount).arg(begin).arg(superresMonotonicMs()));
        }
        finishedWork += batch.inferenceCount;
        const bool complete = memoryFrames
            ? batch.memorySequence.size() == batch.outputCount && std::all_of(batch.memorySequence.cbegin(), batch.memorySequence.cend(), [](const auto& frame) { return frame && frame->ready(); })
            : QDir(batch.sequenceDirectory).entryList({"*.png"}, QDir::Files).size() == batch.outputCount;
        if (!complete) { error_ = trText("补帧结果帧数不完整。"); return false; }
        report(batch.inferenceCount > 0 ? trText("补帧处理") : trText("复用原始画面"), finishedWork);
        if (!memoryFrames || segments.isEmpty()) segments << "file '" + QFileInfo(batch.segmentPath).fileName() + "'\nduration " + number((memoryFrames ? plan.frames.size() : batch.outputCount) / interpolationRate(rate));
        processed += batch.sourceCount; batch.memoryInputs.clear();
        if (!pipeline.encode(std::move(batch), [&]() { if (pulse_.elapsed() > 200) { pulse_.restart(); report(trText("等待编码片段"), finishedWork); } })) return fail();
    }
    report(trText("编码收尾"), finishedWork);
    if (!pipeline.finish([&]() { if (pulse_.elapsed() > 200) { pulse_.restart(); report(trText("编码收尾"), finishedWork); } })) return fail();
    if (engine.process.state() != QProcess::NotRunning) { engine.process.write("QUIT\n"); if (!engine.process.waitForFinished(5000)) { engine.process.kill(); engine.process.waitForFinished(5000); } engine.flush(); }
    QFile list(QDir(cache.path()).filePath("segments.txt")); const QByteArray manifest = segments.join('\n').toUtf8();
    if (!list.open(QIODevice::WriteOnly) || list.write(manifest) != manifest.size()) { error_ = trText("无法保存补帧片段清单。"); return false; } list.close();
    report(task_->preview ? trText("合成预览") : trText("合成输出"), finishedWork);
    QTemporaryDir destination(QFileInfo(task_->outputPath).dir().filePath(".fkw-interpolation-XXXXXX"));
    if (!destination.isValid()) { error_ = trText("无法创建输出临时目录。"); return false; }
    const QString staged = QDir(destination.path()).filePath("result." + options.container);
    QStringList mux{"-hide_banner", "-loglevel", "error", "-nostdin", "-f", "concat", "-safe", "1", "-i", list.fileName()};
    if (first > 0) mux << "-ss" << number(first / info.fps);
    mux << "-i" << task_->inputPath << "-map" << "0:v:0";
    if (options.audio != "none" && info.hasAudio) mux << "-map" << "1:a?" << "-c:a" << (task_->preview || options.audio == "aac" ? "aac" : "copy"); else mux << "-an";
    mux << "-c:v" << "copy" << "-t" << number(count / info.fps) << "-map_metadata" << "1" << "-metadata:s:v:0" << "rotate=0";
    if (options.container == "mp4") mux << "-movflags" << "+faststart";
    mux << "-n" << staged;
    if (!execute(options.ffmpegPath, mux)) return false;
    QByteArray verified; if (!execute(options.ffprobePath, superresProbeArguments(staged), &verified, 15000)) return false;
    const auto result = parseSuperresVideoInfo(QJsonDocument::fromJson(verified).object());
    if (result.size != info.size || std::abs(result.fps - interpolationRate(rate)) > .02 ||
        (result.exactFrames && result.frames != plan.frames.size()) || std::abs(result.duration - count / info.fps) > 1 / interpolationRate(rate) + .05 ||
        (info.hasAudio && options.audio != "none" && !result.hasAudio)) { error_ = trText("补帧输出校验失败，未保存结果。"); return false; }
    if (cancelled_) return false;
    if (!QFile::rename(staged, task_->outputPath)) { error_ = trText("无法保存补帧输出，目标可能已经存在。"); return false; }
    report(trText("完成"), finishedWork, true); return true;
}
void InterpolationWorker::run() {
    wall_.start(); pulse_.start();
    logger_ = Logger::get(QStringLiteral("interpolation-%1-%2").arg(task_->taskId).arg(QDateTime::currentMSecsSinceEpoch()), "interpolation", false);
    task_->logPath = logger_->logFilePath(); bool success = false;
    try { if (!cancelled_) success = process(); } catch (const std::exception& exception) { error_ = QString::fromUtf8(exception.what()); }
    if (!error_.isEmpty() && !cancelled_) { logger_->error(error_); if (!task_->preview) emit GlobalEventBus::instance().taskStageChangedSig(task_->taskId, error_.left(140)); }
    logger_->close(); emit completed(success, cancelled_.load(), task_->outputPath, error_);
    if (!task_->preview && !cancelled_) emit GlobalEventBus::instance().finishTaskSig(task_->taskId, success, task_->logPath);
    deleteLater();
}
}
