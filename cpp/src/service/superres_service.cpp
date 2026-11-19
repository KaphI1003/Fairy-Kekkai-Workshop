#include "service/superres_service.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include "common/app_data.h"
#include "common/config.h"
#include "common/event_bus.h"
#include "common/logger.h"
#include "common/setting.h"
#include "common/task_status.h"
#include "service/superres_pipeline.h"
#include "service/video_frame_io.h"
#include "service/video_frame_analysis.h"
#include "service/video_gpu_gate.h"

namespace fkw {
namespace {
class PersistentSuperresEngine {
public:
    explicit PersistentSuperresEngine(const QSharedPointer<Logger>& logger) : logger_(logger) {}
    ~PersistentSuperresEngine() {
        if (process_.state() == QProcess::NotRunning) return;
        process_.write("QUIT\n"); process_.closeWriteChannel();
        if (!process_.waitForFinished(1000)) { process_.kill(); process_.waitForFinished(5000); }
    }
    static QString workerPath(const SuperresOptions& options) {
        if (qEnvironmentVariableIsSet("FKW_SUPERRES_LEGACY_ENGINE")) return {};
        const QFileInfo cli(options.enginePath);
        // Custom CLI implementations retain their original invocation contract.
        if (cli.fileName().compare(QStringLiteral("realesrgan-ncnn-vulkan.exe"), Qt::CaseInsensitive)) return {};
        const QString worker = cli.dir().filePath(QStringLiteral("fkw-superres-worker.exe"));
        return QFileInfo(worker).isFile() ? worker : QString();
    }
    bool infer(const QString& worker, const SuperresOptions& options, int scale,
               const SuperresBatch& batch, const std::function<void()>& pulse,
               const std::function<bool()>& abort, QString& error) {
        if (!started_) {
            started_ = true;
            const QStringList args{options.modelsPath, options.model, QString::number(scale),
                QString::number(options.gpu), QString::number(options.tile), options.tta ? QStringLiteral("1") : QStringLiteral("0")};
            logger_->info(QStringLiteral("[engine-session-start] ") + worker + QLatin1Char(' ') + args.join(QLatin1Char(' ')));
            process_.setWorkingDirectory(QFileInfo(worker).absolutePath());
            process_.start(worker, args);
            if (!process_.waitForStarted(10000)) { error = trText("无法启动常驻超分引擎：") + process_.errorString(); return false; }
            if (!waitFor(QByteArrayLiteral("READY"), 0, pulse, abort, error, 60000)) return false;
        }
        QFile manifest(QDir(batch.directory).filePath(QStringLiteral("inference.tsv")));
        QByteArray rows;
        for (auto it = batch.freshResults.cbegin(); it != batch.freshResults.cend(); ++it) {
            const QString input = batch.memoryInputs.contains(it.key())
                ? QString::fromStdString(batch.memoryInputs.value(it.key())->uri())
                : QDir(batch.inputDirectory).filePath(QFileInfo(it.value()).fileName());
            rows += input.toUtf8() + '\t' + it.value().toUtf8() + '\n';
        }
        if (!manifest.open(QIODevice::WriteOnly) || manifest.write(rows) != rows.size()) {
            error = trText("无法写入超分批次清单。"); return false;
        }
        manifest.close();
        const QByteArray command = QByteArrayLiteral("BATCH\t") + manifest.fileName().toUtf8() + '\n';
        if (process_.state() == QProcess::NotRunning || process_.write(command) != command.size()) {
            error = trText("常驻超分引擎已退出。"); return false;
        }
        return waitFor(QByteArrayLiteral("BATCH_DONE"), batch.freshResults.size(), pulse, abort, error);
    }
private:
    bool waitFor(const QByteArray& expected, int frames, const std::function<void()>& pulse,
                 const std::function<bool()>& abort, QString& error, int timeout = 0) {
        int completed = 0;
        QElapsedTimer elapsed; elapsed.start();
        while (!abort()) {
            process_.waitForReadyRead(50);
            errors_ += process_.readAllStandardError();
            if (errors_.size() > 16384) errors_ = errors_.right(16384);
            while (process_.canReadLine()) {
                const QByteArray line = process_.readLine().trimmed();
                if (line == QByteArrayLiteral("DONE")) { ++completed; continue; }
                if (line == expected && completed == frames) {
                    if (!errors_.isEmpty()) { logger_->info(QString::fromUtf8(errors_)); errors_.clear(); }
                    return true;
                }
                error = trText("常驻超分引擎返回无效结果：") + QString::fromUtf8(line) + QLatin1Char('\n') + QString::fromUtf8(errors_);
                return false;
            }
            pulse();
            if (process_.state() == QProcess::NotRunning || (timeout && elapsed.elapsed() > timeout) || process_.bytesAvailable() > 4096) {
                error = trText("常驻超分引擎意外退出或响应超时：") + QString::fromUtf8(errors_) +
                    QStringLiteral(" exit=%1 status=%2").arg(process_.exitCode()).arg(int(process_.exitStatus()));
                return false;
            }
        }
        // Cancellation and sibling-stage failure must stop inference immediately.
        process_.kill(); process_.waitForFinished(5000);
        return false;
    }
    QProcess process_;
    QSharedPointer<Logger> logger_;
    QByteArray errors_;
    bool started_ = false;
};
double fraction(const QString& value) {
    const auto parts = value.split(QLatin1Char('/'));
    const double numerator = parts.value(0).toDouble();
    const double denominator = parts.size() > 1 ? parts.at(1).toDouble() : 1;
    return denominator > 0 ? numerator / denominator : 0;
}
int even(double value) { return qMax(2, static_cast<int>(std::round(value / 2)) * 2); }
QString number(double value) { return QString::number(value, 'f', 9); }
QString tool(const QString& name) {
    return QDir(sourceRoot()).filePath(QStringLiteral("tools/") + name + executableSuffix());
}
QString resolveExecutable(const QString& path) {
    if (QFileInfo(path).isFile()) return QFileInfo(path).absoluteFilePath();
    return QStandardPaths::findExecutable(path);
}
bool reuseFrameFile(const QString& source, const QString& destination) {
    // QFile::link creates a shortcut on Windows. Use a real hard link instead,
    // falling back to a byte-for-byte copy on filesystems without hard links.
#ifdef Q_OS_WIN
    const std::filesystem::path from(QDir::toNativeSeparators(source).toStdWString());
    const std::filesystem::path to(QDir::toNativeSeparators(destination).toStdWString());
#else
    const std::filesystem::path from(source.toStdString()), to(destination.toStdString());
#endif
    std::error_code error;
    std::filesystem::create_hard_link(from, to, error);
    return !error || QFile::copy(source, destination);
}
}

qint64 superresMonotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void SuperresSpeedMeter::start(qint64 now) { samples_ = {{now, 0}}; }
double SuperresSpeedMeter::update(qint64 now, qint64 frames) {
    if (samples_.isEmpty()) return 0; // Analysis never contributes to render FPS.
    frames = qMax(frames, samples_.last().second);
    if (now <= samples_.last().first) samples_.last().second = frames;
    else samples_.append({now, frames});
    while (samples_.size() > 2 && samples_.at(1).first <= now - 8000) samples_.removeFirst();
    const qint64 elapsed = now - samples_.first().first;
    return elapsed >= 1000 ? (frames - samples_.first().second) * 1000.0 / elapsed : 0;
}
const QStringList& superresModelIds() {
    static const QStringList models{QStringLiteral("realesr-animevideov3"), QStringLiteral("realesrgan-x4plus"),
        QStringLiteral("realesrgan-x4plus-anime"), kAniSdSpanModel};
    return models;
}

SuperresOptions SuperresOptions::fromConfig() {
    SuperresOptions options;
    const auto& config = AppConfig::instance();
    const auto value = [&config](const char* name, const QJsonValue& fallback) {
        return config.value(QStringLiteral("Superres"), QString::fromLatin1(name), fallback);
    };
    options.sizeMode = value("SizeMode", options.sizeMode).toString();
    options.presetHeight = value("PresetHeight", options.presetHeight).toInt();
    options.scale = value("Scale", options.scale).toInt();
    options.width = value("Width", options.width).toInt();
    options.height = value("Height", options.height).toInt();
    options.lockRatio = value("LockRatio", options.lockRatio).toBool();
    options.fit = value("Fit", options.fit).toString();
    options.model = value("Model", options.model).toString();
    options.quality = value("Quality", options.quality).toString();
    options.codec = value("Codec", options.codec).toString();
    options.audio = value("Audio", options.audio).toString();
    options.container = value("Container", options.container).toString();
    options.enginePath = value("EnginePath", QDir(sourceRoot()).filePath(
        QStringLiteral("tools/realesrgan/realesrgan-ncnn-vulkan") + executableSuffix())).toString();
    options.modelsPath = value("ModelsPath", QDir(QFileInfo(options.enginePath).absolutePath()).filePath(QStringLiteral("models"))).toString();
    options.ffmpegPath = config.value(ConfigKeys::ffmpegPath, tool(QStringLiteral("ffmpeg"))).toString();
    const QString adjacentProbe = QDir(QFileInfo(options.ffmpegPath).absolutePath()).filePath(QStringLiteral("ffprobe") + executableSuffix());
    options.ffprobePath = value("FFprobePath", QFileInfo::exists(adjacentProbe) ? adjacentProbe : tool(QStringLiteral("ffprobe"))).toString();
    options.tempPath = value("TempPath", QDir(userDataFolder()).filePath(QStringLiteral("SuperresTemp"))).toString();
    options.gpu = value("Gpu", -1).toInt();
    options.tile = value("Tile", 0).toInt();
    options.batchFrames = value("BatchFrames", 16).toInt();
    options.tta = value("Tta", false).toBool();
    options.reuseIdenticalFrames = value("ReuseIdenticalFrames", true).toBool();
    return options;
}

QSize SuperresOptions::targetSize(const QSize& source) const {
    if (source.isEmpty()) return {};
    if (sizeMode == QStringLiteral("scale")) return source * scale;
    if (sizeMode == QStringLiteral("custom")) return {even(width), even(height)};
    const bool portrait = source.height() > source.width();
    return portrait ? QSize(even(presetHeight), even(presetHeight * double(source.height()) / source.width()))
                    : QSize(even(presetHeight * double(source.width()) / source.height()), even(presetHeight));
}

int SuperresOptions::modelScale(const QSize& source) const {
    if (model == kAniSdSpanModel) return 2;
    if (model != QStringLiteral("realesr-animevideov3")) return 4;
    const QSize target = targetSize(source);
    return qBound(2, static_cast<int>(std::ceil(qMax(double(target.width()) / source.width(),
                                                   double(target.height()) / source.height()))), 4);
}

int SuperresOptions::crf() const {
    return quality == QStringLiteral("high") ? 18 : quality == QStringLiteral("small") ? 25 : 21;
}

QString SuperresOptions::validate(const QSize& source) const {
    if (!QStringList{QStringLiteral("preset"), QStringLiteral("scale"), QStringLiteral("custom")}.contains(sizeMode) ||
        !QList<int>{720, 1080, 1440, 2160}.contains(presetHeight) || scale < 2 || scale > 4 ||
        width < 2 || height < 2 || width > 16384 || height > 16384)
        return trText("目标尺寸设置无效。");
    if (!superresModelIds().contains(model) ||
        !QStringList{QStringLiteral("libx264"), QStringLiteral("libx265")}.contains(codec) ||
        !QStringList{QStringLiteral("balanced"), QStringLiteral("high"), QStringLiteral("small")}.contains(quality) ||
        !QStringList{QStringLiteral("copy"), QStringLiteral("aac"), QStringLiteral("none")}.contains(audio) ||
        !QStringList{QStringLiteral("mp4"), QStringLiteral("mkv")}.contains(container) ||
        !QStringList{QStringLiteral("pad"), QStringLiteral("crop")}.contains(fit))
        return trText("超分参数无效，请重新选择。");
    if (gpu < -1 || tile < 0 || (tile > 0 && tile < 32) || tile > 4096 || batchFrames < 1 || batchFrames > 120)
        return trText("显卡、分块或缓存帧数设置无效。");
    if (!source.isEmpty()) {
        const QSize target = targetSize(source);
        if (target.width() > 16384 || target.height() > 16384 ||
            target.width() > source.width() * 4 || target.height() > source.height() * 4)
            return trText("单次超分最大支持 4 倍，且输出边长不能超过 16384 像素。");
        if (target.width() <= source.width() && target.height() <= source.height())
            return trText("目标尺寸没有放大，请选择更高的分辨率。");
    }
    if (resolveExecutable(enginePath).isEmpty()) return trText("未找到超分引擎，请在高级设置中选择 Real-ESRGAN 程序。");
    if (model == kAniSdSpanModel && PersistentSuperresEngine::workerPath(*this).isEmpty())
        return trText("AniSD SPAN 需要官方 CLI 旁支持此模型的 fkw-superres-worker.exe。");
    if (resolveExecutable(ffmpegPath).isEmpty()) return trText("未找到 FFmpeg，请在软件设置中检查路径。");
    if (resolveExecutable(ffprobePath).isEmpty()) return trText("未找到 ffprobe，请在高级设置中选择 ffprobe 程序。");
    const QString stem = model == QStringLiteral("realesr-animevideov3") ? model + QStringLiteral("-x2") : model;
    for (const QString& suffix : {QStringLiteral(".param"), QStringLiteral(".bin")})
        if (!QFileInfo(QDir(modelsPath).filePath(stem + suffix)).isFile())
            return trText("超分模型文件不完整，请在高级设置中检查模型目录。");
    return {};
}

SuperresVideoInfo parseSuperresVideoInfo(const QJsonObject& json) {
    SuperresVideoInfo info;
    for (const auto& item : json.value(QStringLiteral("streams")).toArray()) {
        const auto stream = item.toObject();
        if (stream.value(QStringLiteral("codec_type")) == QStringLiteral("audio")) info.hasAudio = true;
        if (stream.value(QStringLiteral("codec_type")) != QStringLiteral("video") || !info.size.isEmpty()) continue;
        if (stream.value(QStringLiteral("disposition")).toObject().value(QStringLiteral("attached_pic")).toInt()) continue;
        double sar = fraction(stream.value(QStringLiteral("sample_aspect_ratio")).toString().replace(QLatin1Char(':'), QLatin1Char('/')));
        if (sar <= 0) sar = 1;
        info.size = {even(stream.value(QStringLiteral("width")).toInt() * sar), even(stream.value(QStringLiteral("height")).toInt())};
        QSize decoded(stream.value(QStringLiteral("width")).toInt(), stream.value(QStringLiteral("height")).toInt());
        for (const auto& side : stream.value(QStringLiteral("side_data_list")).toArray()) {
            if (std::abs(side.toObject().value(QStringLiteral("rotation")).toInt()) % 180 == 90) {
                info.size.transpose(); decoded.transpose(); sar = 1 / sar;
            }
        }
        // Match the existing FFmpeg normalization: trunc(iw*sar/2)*2:ih.
        // Display-size rounding must not change the pixels sent to the model.
        info.decodedSize = {qMax(2, int(std::floor(decoded.width() * sar / 2)) * 2), decoded.height()};
        info.frameRate = stream.value(QStringLiteral("avg_frame_rate")).toString();
        info.fps = fraction(info.frameRate);
        const double nominal = fraction(stream.value(QStringLiteral("r_frame_rate")).toString());
        if (info.fps <= 0) { info.fps = nominal; info.frameRate = stream.value(QStringLiteral("r_frame_rate")).toString(); }
        info.variableFrameRate = nominal > 0 && info.fps > 0 && std::abs(nominal - info.fps) > .02;
        const QString transfer = stream.value(QStringLiteral("color_transfer")).toString();
        info.hdr = transfer == QStringLiteral("smpte2084") || transfer == QStringLiteral("arib-std-b67");
        info.duration = stream.value(QStringLiteral("duration")).toString().toDouble();
        info.frames = stream.value(QStringLiteral("nb_frames")).toString().toLongLong();
        info.exactFrames = info.frames > 0;
    }
    if (info.duration <= 0) info.duration = json.value(QStringLiteral("format")).toObject().value(QStringLiteral("duration")).toString().toDouble();
    if (info.frames <= 0 && info.duration > 0 && info.fps > 0) info.frames = qRound64(info.duration * info.fps);
    return info;
}

QStringList superresProbeArguments(const QString& input) {
    return {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_streams"),
            QStringLiteral("-show_format"), QStringLiteral("-of"), QStringLiteral("json"), input};
}

QString superresScaleFilter(const QSize& target, const QString& fit) {
    const QString size = QString::number(target.width()) + QLatin1Char(':') + QString::number(target.height());
    return QStringLiteral("scale=") + size + QStringLiteral(":flags=lanczos:force_original_aspect_ratio=") +
        (fit == QStringLiteral("crop") ? QStringLiteral("increase:out_color_matrix=bt709:out_range=tv,crop=") : QStringLiteral("decrease:force_divisible_by=2:out_color_matrix=bt709:out_range=tv,pad=")) +
        size + (fit == QStringLiteral("crop") ? QStringLiteral(":(iw-ow)/2:(ih-oh)/2,setsar=1") : QStringLiteral(":(ow-iw)/2:(oh-ih)/2,setsar=1"));
}

SuperresTask::SuperresTask(const QString& input, const QString& output, SuperresOptions settings)
    : TaskBase(input, output), options(std::move(settings)) {}

SuperresWorker::SuperresWorker(std::shared_ptr<SuperresTask> task) : task_(std::move(task)) {}
void SuperresWorker::cancel() { cancelled_.store(true); }
void SuperresWorker::cancelQueued() {
    cancel();
    emit completed(false, true, task_->outputPath, {});
    deleteLater();
}

bool SuperresWorker::execute(const QString& executable, const QStringList& args,
                            const std::function<void()>& poll, QByteArray* output, int timeoutMs,
                            const std::function<bool()>& abort) {
    if (cancelled_) return false;
    QProcess process;
    process.setWorkingDirectory(QFileInfo(resolveExecutable(executable)).absolutePath());
    logger_->info(executable + QLatin1Char(' ') + args.join(QLatin1Char(' ')));
    process.start(resolveExecutable(executable), args);
    if (!process.waitForStarted(10000)) { error_ = trText("无法启动处理程序：") + process.errorString(); return false; }
    QByteArray errors;
    QElapsedTimer timer;
    timer.start();
    do {
        process.waitForFinished(100);
        errors += process.readAllStandardError();
        const auto data = process.readAllStandardOutput();
        if (output) *output += data;
        if (errors.size() > 16384) errors = errors.right(16384);
        if (poll) poll();
        if (cancelled_ || (abort && abort()) || (timeoutMs > 0 && timer.elapsed() > timeoutMs)) {
            process.kill(); process.waitForFinished(5000);
            if (!cancelled_ && !(abort && abort())) error_ = trText("读取视频信息超时。");
            return false;
        }
    } while (process.state() != QProcess::NotRunning);
    logger_->info(QString::fromUtf8(errors));
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        error_ = trText("处理失败：") + QString::fromUtf8(errors.right(3000));
        return false;
    }
    return true;
}

void SuperresWorker::report(const QString& stage, qint64 frames, qint64 total, bool complete) {
    const double elapsed = wallTimer_.elapsed() / 1000.0;
    const double fps = speedMeter_.update(wallTimer_.elapsed(), frames);
    const double remaining = fps > 0 && frames < total ? (total - frames) / fps : -1;
    const int percent = complete ? 100 : total > 0 ? qBound(0, int(frames * 100 / total), 99) : 0;
    if (uniqueFrames_ > 0) emit frameReuseProgress(frames, reusedFrames_, uniqueFrames_, sourceFrames_);
    emit progress(percent, stage, frames, total, fps, elapsed, remaining);
    if (!task_->preview) {
        auto& bus = GlobalEventBus::instance();
        const QString reuseSummary = QStringLiteral("%1 %2 %3，%4 %5 %3")
            .arg(trText("超分")).arg(inferredFrames_).arg(trText("帧")).arg(trText("复用")).arg(reusedFrames_);
        const QString sourceSummary = QStringLiteral("%1 %2 %3 · %4 %5 %3")
            .arg(trText("原始")).arg(sourceFrames_).arg(trText("帧"))
            .arg(trText("可复用")).arg(sourceFrames_ - uniqueFrames_);
        const QString taskStage = complete ? reuseSummary : stage +
            (uniqueFrames_ > 0 ? QStringLiteral(" · ") + sourceSummary : QString());
        emit bus.taskStageChangedSig(task_->taskId, taskStage);
        const QString detail = total > 0 ? QStringLiteral("%1 %2/%3 · %4 %5 %6")
            .arg(trText("已处理")).arg(frames).arg(total).arg(trText("剩余")).arg(qMax<qint64>(0, total - frames)).arg(trText("帧"))
            : trText("统计中");
        const QString estimate = total > 0 && frames >= total ? trText("收尾中") :
            trText("超分剩余") + QLatin1Char(' ') + (remaining >= 0 ? QStringLiteral("%1s").arg(int(std::ceil(remaining))) : trText("估算中"));
        emit bus.updateTaskStatusSig(task_->taskId, percent, int(TaskStatus::Processing), detail,
                                     elapsed, estimate, fps);
    }
}

bool SuperresWorker::process() {
    const auto& options = task_->options;
    error_ = options.validate();
    if (!error_.isEmpty()) return false;
    if (!QFileInfo(task_->inputPath).isFile() || QFileInfo::exists(task_->outputPath)) {
        error_ = trText("输入视频不存在或输出文件已经存在，请选择新的输出路径。"); return false;
    }
    report(trText("准备片段"), 0, 0);
    QByteArray json;
    if (!execute(options.ffprobePath, superresProbeArguments(task_->inputPath), {}, &json, 15000)) return false;
    auto info = parseSuperresVideoInfo(QJsonDocument::fromJson(json).object());
    if (info.size.isEmpty() || info.fps <= 0 || info.frames <= 0) { error_ = trText("无法读取视频尺寸、帧率或时长。"); return false; }
    if (info.variableFrameRate) { error_ = trText("当前超分支持固定帧率视频。此视频检测为可变帧率，请先在压制中转换为固定帧率。"); return false; }
    if (info.hdr) { error_ = trText("当前超分支持 SDR 视频，请先将 HDR 视频转换为 SDR。"); return false; }
    if (!info.exactFrames) {
        QByteArray counts;
        if (!execute(options.ffprobePath, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-count_packets"),
                QStringLiteral("-select_streams"), QStringLiteral("v:0"), QStringLiteral("-show_entries"), QStringLiteral("stream=nb_read_packets"),
                QStringLiteral("-of"), QStringLiteral("json"), task_->inputPath}, {}, &counts, 60000)) return false;
        const auto streams = QJsonDocument::fromJson(counts).object().value(QStringLiteral("streams")).toArray();
        const qint64 count = streams.isEmpty() ? 0 : streams.first().toObject().value(QStringLiteral("nb_read_packets")).toString().toLongLong();
        if (count <= 0) { error_ = trText("无法确定视频总帧数。"); return false; }
        info.frames = count;
    }
    error_ = options.validate(info.size);
    if (!error_.isEmpty()) return false;
    const QSize target = options.targetSize(info.size);
    const int scale = options.modelScale(info.size);
    if (options.model == QStringLiteral("realesr-animevideov3")) {
        const QString stem = options.model + QStringLiteral("-x%1").arg(scale);
        for (const auto& ext : {QStringLiteral(".bin"), QStringLiteral(".param")})
            if (!QFileInfo::exists(QDir(options.modelsPath).filePath(stem + ext))) { error_ = trText("所选倍率的模型文件缺失。"); return false; }
    }
    const qint64 first = task_->preview ? qMax<qint64>(0, std::floor(task_->previewStart * info.fps + 1e-6)) : 0;
    const qint64 total = qMin(info.frames - first, task_->preview ? qint64(std::ceil(task_->previewDuration * info.fps - 1e-6)) : info.frames);
    if (total <= 0) { error_ = trText("预览起点已经超出视频时长。"); return false; }
    if (!QDir().mkpath(options.tempPath) || !QDir().mkpath(QFileInfo(task_->outputPath).absolutePath())) {
        error_ = trText("无法创建临时目录或输出目录。"); return false;
    }
    QTemporaryDir cache(QDir(options.tempPath).filePath(QStringLiteral("render-XXXXXX")));
    if (!cache.isValid()) { error_ = trText("无法创建超分缓存。"); return false; }
    const QDir dir(cache.path());
    const QString reuseDirectory = dir.filePath(QStringLiteral("reused"));
    if (!QDir().mkpath(reuseDirectory)) { error_ = trText("无法创建相同帧缓存。"); return false; }
    QVector<QByteArray> frameHashes;
    QVector<qint64> representatives(total), lastUse(total, -1);
    QHash<qint64, QString> renderedFrames;
    qint64 uniqueFrames = total;
    if (options.reuseIdenticalFrames) {
        report(trText("检测相同帧"), 0, 0);
        const auto args = videoRgbDecodeArguments(task_->inputPath, info.decodedSize, info.fps, first, total);
        VideoRawDecoder decoder(resolveExecutable(options.ffmpegPath), args,
            qsizetype(info.decodedSize.width()) * info.decodedSize.height() * 3, total, [&] { return cancelled_.load(); });
        frameHashes.resize(total);
        QElapsedTimer progress; progress.start();
        if (!analyzeVideoFrames(decoder, info.decodedSize, frameHashes, cancelled_, {}, [&](qsizetype index, bool) {
            if (progress.elapsed() >= 200 || index + 1 == total) {
                progress.restart();
                report(trText("检测相同帧") + QStringLiteral(" %1/%2").arg(index + 1).arg(total), 0, 0);
            }
        }, error_) || !decoder.finish(error_)) return false;
    }
    {
        QHash<QByteArray, qint64> firstOccurrences;
        for (qint64 index = 0; index < total; ++index) {
            const qint64 representative = options.reuseIdenticalFrames ? firstOccurrences.value(frameHashes.at(index), index) : index;
            representatives[index] = representative; lastUse[representative] = index;
            if (options.reuseIdenticalFrames && representative == index) firstOccurrences.insert(frameHashes.at(index), index);
        }
        if (options.reuseIdenticalFrames) uniqueFrames = firstOccurrences.size();
    }
    logger_->info(QStringLiteral("Frame reuse plan: total=%1 unique=%2 reusable=%3 analysis_ms=%4")
        .arg(total).arg(uniqueFrames).arg(total - uniqueFrames).arg(wallTimer_.elapsed()));
    sourceFrames_ = total; uniqueFrames_ = uniqueFrames;
    VideoGpuLease gpu;
    QElapsedTimer gpuWait; gpuWait.start();
    if (!gpu.acquire(cancelled_, [&]() {
        if (gpuWait.elapsed() >= 200) { gpuWait.restart(); report(trText("等待显卡资源"), 0, uniqueFrames); }
    })) return false;
    speedMeter_.start(wallTimer_.elapsed());
    report(trText("准备片段"), 0, uniqueFrames);
    const QString persistentWorker = PersistentSuperresEngine::workerPath(options);
    const bool memoryFrames = !persistentWorker.isEmpty() && videoMemoryTransport();
    logger_->info(QStringLiteral("[transport] ") + (memoryFrames ? QStringLiteral("shared-rgb24") : QStringLiteral("png")));
    SuperresPipeline pipeline(task_->inputPath, options, info, first, total, frameHashes,
                              representatives, cache.path(), logger_, cancelled_, memoryFrames);
    PersistentSuperresEngine engine(logger_);
    std::unique_ptr<VideoReuseCache> reuseCache;
    if (memoryFrames) reuseCache = std::make_unique<VideoReuseCache>();
    const auto pipelineFailed = [&]() {
        error_ = pipeline.error();
        if (error_.isEmpty() && !cancelled_) error_ = trText("视频处理流水线已停止。");
        return false;
    };
    QStringList manifest;
    qint64 processed = 0;
    while (processed < total) {
        if (cancelled_) return false;
        if (reuseCache && !reuseCache->error().isEmpty()) { error_ = reuseCache->error(); return false; }
        QElapsedTimer pulse; pulse.start();
        const auto preparing = [&]() {
            if (pulse.elapsed() < 200) return;
            pulse.restart(); report(trText("准备片段"), inferredFrames_, uniqueFrames);
        };
        SuperresBatch batch;
        QElapsedTimer inputWait; inputWait.start();
        if (!pipeline.next(batch, preparing)) return pipelineFailed();
        logger_->info(QStringLiteral("[gpu-input-wait] offset=%1 elapsed_ms=%2").arg(batch.offset).arg(inputWait.elapsed()));
        if (!batch.freshResults.isEmpty()) {
            report(trText("超分处理"), inferredFrames_, uniqueFrames);
            const qint64 began = superresMonotonicMs();
            QStringList upscale{QStringLiteral("-i"), batch.inputDirectory, QStringLiteral("-o"), batch.resultDirectory,
                QStringLiteral("-m"), options.modelsPath, QStringLiteral("-n"), options.model,
                QStringLiteral("-s"), QString::number(scale), QStringLiteral("-t"), QString::number(options.tile),
                QStringLiteral("-f"), QStringLiteral("png")};
            if (options.gpu >= 0) upscale << QStringLiteral("-g") << QString::number(options.gpu);
            if (options.tta) upscale << QStringLiteral("-x");
            QElapsedTimer throttle; throttle.start();
            const auto inferenceProgress = [&]() {
                if (throttle.elapsed() < 200) return;
                throttle.restart();
                int ready = 0;
                // Each new result contributes once, regardless of how many
                // source frames will reuse it within or after this batch.
                if (memoryFrames) { for (const auto& result : batch.memoryResults) if (result->ready()) ++ready; }
                else for (const QString& result : batch.freshResults) if (QFileInfo(result).size() > 0) ++ready;
                report(trText("超分处理"), inferredFrames_ + ready, uniqueFrames);
            };
            const bool inferred = persistentWorker.isEmpty()
                ? execute(options.enginePath, upscale, inferenceProgress, nullptr, 0, [&]() { return pipeline.failed(); })
                : engine.infer(persistentWorker, options, scale, batch, inferenceProgress,
                    [&]() { return cancelled_.load() || pipeline.failed(); }, error_);
            if (!inferred) {
                if (pipeline.failed()) return pipelineFailed();
                return false;
            }
            logger_->info(QStringLiteral("[inferred] offset=%1 fresh=%2 begin_tick=%3 end_tick=%4")
                .arg(batch.offset).arg(batch.freshResults.size()).arg(began).arg(superresMonotonicMs()));
            const bool complete = memoryFrames
                ? std::all_of(batch.memoryResults.cbegin(), batch.memoryResults.cend(), [](const auto& frame) { return frame->ready(); })
                : QDir(batch.resultDirectory).entryList({QStringLiteral("*.png")}, QDir::Files).size() == batch.freshResults.size();
            if (!complete) {
                error_ = trText("超分输出帧不完整，请查看日志或尝试减小分块大小。"); return false;
            }
        }
        QHash<qint64, VideoFramePtr> loaded;
        const auto abortCache = [&] { return cancelled_.load() || pipeline.failed(); };
        for (int index = 0; index < batch.count; ++index) {
            if (cancelled_) return false;
            const qint64 representative = batch.representatives.at(index);
            if (memoryFrames) {
                auto result = batch.memoryResults.value(representative, loaded.value(representative));
                if (!result) {
                    result = reuseCache->load(renderedFrames.value(representative), abortCache, preparing);
                    if (!result) { error_ = reuseCache->error(); if (error_.isEmpty()) error_ = trText("无法读取重复帧缓存。"); return false; }
                    loaded.insert(representative, result);
                }
                batch.memorySequence.append(result); continue;
            }
            const QString result = batch.freshResults.value(representative, renderedFrames.value(representative));
            const QString name = QStringLiteral("%1.png").arg(index, 8, 10, QLatin1Char('0'));
            if (result.isEmpty() || !reuseFrameFile(result, QDir(batch.sequenceDirectory).filePath(name))) {
                error_ = trText("无法复用已超分的视频帧。"); return false;
            }
        }
        const qint64 lastInBatch = processed + batch.count - 1;
        for (auto it = batch.freshResults.cbegin(); it != batch.freshResults.cend(); ++it) {
            if (lastUse.at(it.key()) <= lastInBatch) continue;
            const QString retained = QDir(reuseDirectory).filePath(QStringLiteral("%1.png").arg(it.key(), 12, 10, QLatin1Char('0')));
            if (memoryFrames) {
                if (!reuseCache->put(retained, batch.memoryResults.value(it.key()), abortCache, preparing)) {
                    error_ = reuseCache->error(); return false;
                }
            } else if (!reuseFrameFile(it.value(), retained)) { error_ = trText("无法保存待复用的超分帧。"); return false; }
            renderedFrames.insert(it.key(), retained);
        }
        for (auto it = renderedFrames.begin(); it != renderedFrames.end();) {
            if (lastUse.at(it.key()) <= lastInBatch) {
                const bool removed = memoryFrames ? reuseCache->drop(it.value(), abortCache, preparing) : QFile::remove(it.value());
                if (!removed) { error_ = trText("无法清理已完成复用的视频帧。"); return false; }
                it = renderedFrames.erase(it);
            } else ++it;
        }
        inferredFrames_ += batch.freshResults.size(); reusedFrames_ += batch.count - batch.freshResults.size();
        report(batch.freshResults.isEmpty() ? trText("处理复用帧") : trText("超分处理"), inferredFrames_, uniqueFrames);
        logger_->info(QStringLiteral("Frame reuse progress: resolved=%1/%2 inferred=%3 reused=%4")
            .arg(processed + batch.count).arg(total).arg(inferredFrames_).arg(reusedFrames_));
        if (!memoryFrames || manifest.isEmpty()) manifest << QStringLiteral("file '") + QFileInfo(batch.segmentPath).fileName() +
            QStringLiteral("'\nduration ") + number((memoryFrames ? total : batch.count) / info.fps);
        batch.memoryInputs.clear();
        const int count = batch.count;
        pulse.restart();
        if (!pipeline.encode(std::move(batch), [&]() {
            if (pulse.elapsed() < 200) return;
            pulse.restart(); report(trText("等待编码片段"), inferredFrames_, uniqueFrames);
        })) return pipelineFailed();
        processed += count;
    }
    report(trText("编码收尾"), inferredFrames_, uniqueFrames);
    QElapsedTimer draining; draining.start();
    if (!pipeline.finish([&]() {
        if (draining.elapsed() < 200) return;
        draining.restart(); report(trText("编码收尾"), inferredFrames_, uniqueFrames);
    })) return pipelineFailed();

    QFile list(dir.filePath(QStringLiteral("segments.txt")));
    if (!list.open(QIODevice::WriteOnly) || list.write(manifest.join(QLatin1Char('\n')).toUtf8()) < 0) {
        error_ = trText("无法写入片段清单。"); return false;
    }
    list.close();
    report(task_->preview ? trText("合成预览") : trText("合成输出"), inferredFrames_, uniqueFrames);
    QTemporaryDir destination(QFileInfo(task_->outputPath).dir().filePath(QStringLiteral(".fkw-superres-XXXXXX")));
    if (!destination.isValid()) { error_ = trText("无法在输出目录创建临时文件。"); return false; }
    const QString staged = QDir(destination.path()).filePath(QStringLiteral("result.") + options.container);
    QStringList mux{QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-nostdin"),
        QStringLiteral("-f"), QStringLiteral("concat"), QStringLiteral("-safe"), QStringLiteral("1"),
        QStringLiteral("-i"), list.fileName()};
    if (first > 0) mux << QStringLiteral("-ss") << number(first / info.fps);
    mux << QStringLiteral("-i") << task_->inputPath << QStringLiteral("-map") << QStringLiteral("0:v:0");
    if (options.audio != QStringLiteral("none") && info.hasAudio)
        mux << QStringLiteral("-map") << QStringLiteral("1:a?") << QStringLiteral("-c:a")
            << (task_->preview || options.audio == QStringLiteral("aac") ? QStringLiteral("aac") : QStringLiteral("copy"));
    else mux << QStringLiteral("-an");
    mux << QStringLiteral("-c:v") << QStringLiteral("copy") << QStringLiteral("-t") << number(total / info.fps)
        << QStringLiteral("-map_metadata") << QStringLiteral("1") << QStringLiteral("-metadata:s:v:0") << QStringLiteral("rotate=0");
    if (options.container == QStringLiteral("mp4")) mux << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    mux << QStringLiteral("-n") << staged;
    if (!execute(options.ffmpegPath, mux)) return false;
    QByteArray resultJson;
    if (!execute(options.ffprobePath, superresProbeArguments(staged), {}, &resultJson, 15000)) return false;
    const auto result = parseSuperresVideoInfo(QJsonDocument::fromJson(resultJson).object());
    if (result.size != target || (result.exactFrames && result.frames != total) || result.fps <= 0 ||
        std::abs(result.fps - info.fps) > .02 || (options.audio != QStringLiteral("none") && info.hasAudio && !result.hasAudio)) {
        logger_->error(QStringLiteral("Output validation expected=%1x%2 frames=%3 fps=%4 audio=%5; actual=%6x%7 frames=%8 fps=%9 audio=%10; probe=%11")
            .arg(target.width()).arg(target.height()).arg(total).arg(info.fps, 0, 'g', 12).arg(info.hasAudio)
            .arg(result.size.width()).arg(result.size.height()).arg(result.frames).arg(result.fps, 0, 'g', 12).arg(result.hasAudio)
            .arg(QString::fromUtf8(resultJson)));
        error_ = trText("输出视频校验失败，未保存结果。请查看日志。"); return false;
    }
    if (cancelled_) return false;
    // QFile::rename refuses to overwrite an existing file, including a file created
    // after this task started. Partial/cancelled work never appears at the final path.
    if (!QFile::rename(staged, task_->outputPath)) { error_ = trText("无法保存结果，输出路径可能已存在或没有写入权限。"); return false; }
    report(trText("完成"), inferredFrames_, uniqueFrames, true);
    return true;
}

void SuperresWorker::run() {
    wallTimer_.start();
    startedAt_ = QDateTime::currentMSecsSinceEpoch();
    logger_ = Logger::get(QStringLiteral("superres-%1-%2.log").arg(task_->taskId).arg(startedAt_), QStringLiteral("superres"), false);
    task_->logPath = logger_->logFilePath();
    bool success = false;
    try { if (!cancelled_) success = process(); }
    catch (const std::exception& exception) { error_ = QString::fromUtf8(exception.what()); }
    if (!error_.isEmpty()) {
        logger_->error(error_);
        if (!task_->preview) emit GlobalEventBus::instance().taskStageChangedSig(task_->taskId, error_.left(140));
    }
    logger_->close();
    emit completed(success, cancelled_.load(), task_->outputPath, error_);
    if (!task_->preview && !cancelled_)
        emit GlobalEventBus::instance().finishTaskSig(task_->taskId, success, task_->logPath);
    deleteLater();
}
} // namespace fkw
