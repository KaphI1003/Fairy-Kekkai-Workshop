#include "service/ffmpeg_service.h"

#include <QDateTime>
#include <QDir>
#include <QTemporaryDir>
#include "common/app_data.h"
#include "common/setting.h"
#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <cstdio>
#endif
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QVariant>
#include <cmath>
#include <iterator>

#include "common/config.h"
#include "common/config_keys.h"
#include "common/event_bus.h"
#include "common/task_status.h"
#include "common/utils.h"

namespace fkw {
namespace {

// 解析 ffmpeg 输出（对齐 Easy-FFmpeg）
const QRegularExpression& timeRe() {
    static const QRegularExpression re(
        QStringLiteral(R"(time=(\d{2}):(\d{2}):(\d{2}\.\d+))"));
    return re;
}
const QRegularExpression& sizeRe() {
    static const QRegularExpression re(QStringLiteral(R"(size=\s*(\S+))"));
    return re;
}
const QRegularExpression& bitrateRe() {
    static const QRegularExpression re(QStringLiteral(R"(bitrate=\s*(\S+))"));
    return re;
}
const QRegularExpression& speedRe() {
    static const QRegularExpression re(QStringLiteral(R"(speed=\s*([\d.]+))"));
    return re;
}

QString ffmpegPath() {
    return AppConfig::instance().value(ConfigKeys::ffmpegPath).toString();
}
QString stringValue(ConfigKeys::Key key) {
    return AppConfig::instance().value(key).toString();
}
bool boolValue(ConfigKeys::Key key) {
    return AppConfig::instance().value(key).toBool();
}
int intValue(ConfigKeys::Key key) {
    return AppConfig::instance().value(key).toInt();
}
double doubleValue(ConfigKeys::Key key) {
    return AppConfig::instance().value(key).toDouble();
}

// os.path.dirname/basename 语义见 common/utils.h（与 task_base 共用）
double round2(double value) {
    return std::round(value * 100.0) / 100.0;
}

}  // namespace

FFmpegTask::FFmpegTask(const QString& videoPath, const QString& outputPath, const QString& subtitle)
    : TaskBase(videoPath, outputPath), saveFolder(pathDirname(outputPath)), subtitlePath(subtitle),
      preferGpu(boolValue(ConfigKeys::ffmpegGpuEncoding)) {}

QString adjustOutputFormat(const QString& outputFile) {
    const QString outputFormat = stringValue(ConfigKeys::ffmpegOutputFormat);
    if (outputFormat.isEmpty()) return outputFile;
    // os.path.splitext 语义：在最后一个路径组件内取最后一个非前导的点
    const qsizetype sep = lastSeparator(outputFile);
    const qsizetype dot = outputFile.lastIndexOf(QLatin1Char('.'));
    bool hasExtension = false;
    if (dot > sep) {
        for (qsizetype i = sep + 1; i < dot; ++i) {
            if (outputFile.at(i) != QLatin1Char('.')) {
                hasExtension = true;
                break;
            }
        }
    }
    const QString base = hasExtension ? outputFile.left(dot) : outputFile;
    return base + QLatin1Char('.') + outputFormat;
}

// Keep the source chroma sampling and the software encoder's output bit depth.
static QString nvencPixelFormat(const QString& codec, const QString& format, int quality) {
    if (quality == 0) return {};
    const bool eightBit420 = format == "yuv420p" || format == "nv12";
    if (codec == "libx265" && (eightBit420 || format == "yuv420p10le" || format == "p010le"))
        return "p010le";
    if (codec == "libx264" && eightBit420 && !boolValue(ConfigKeys::ffmpegUseAdvanced))
        return "yuv420p";
    return {};
}

static QString nvencPreset(const QString& preset) {
    static const QHash<QString, QString> presets{
        {"ultrafast", "p1"}, {"superfast", "p2"}, {"veryfast", "p3"}, {"faster", "p4"},
        {"fast", "p5"}, {"medium", "p6"}, {"slow", "p7"}, {"slower", "p7"}, {"veryslow", "p7"}
    };
    return presets.value(preset, "p6");
}

// NVENC -cq scale is noticeably more aggressive than x264/x265 -crf (especially 10-bit HEVC).
// Map the shared UI quality value so GPU encodes match CPU bitrate/quality expectations.
static int nvencQuality(const QString& codec, int quality) {
    const int offset = codec == "libx265" ? 6 : 4;
    return qBound(1, quality + offset, 51);
}

// Estimate an equivalent target average bitrate for 2-Pass VBR when converting
// across codecs (e.g. AV1 -> H.265/H.264) so high-efficiency sources are not starved.
static QString sourceTargetBitrate(const EncodeSource& src, const QString& codec) {
    if (src.videoBitrate <= 0) return {};
    const QString srcCodec = src.video["codec_name"].toString().toLower();
    double factor = 1.0;
    if (codec == "libx265") {
        if (srcCodec == "av1") factor = 1.30;
        else if (srcCodec == "hevc" || srcCodec == "vp9") factor = 1.10;
        else factor = 1.00;
    } else if (codec == "libx264") {
        if (srcCodec == "av1") factor = 1.70;
        else if (srcCodec == "hevc" || srcCodec == "vp9") factor = 1.35;
        else factor = 1.15;
    }
    const qint64 kbps = qMax<qint64>(1, qRound64(src.videoBitrate * factor / 1000.0));
    return QString::number(kbps) + 'k';
}

static QString passLogPrefix(const QString& workDir) {
    QString dir = QDir::toNativeSeparators(QFileInfo(workDir).absoluteFilePath());
#ifdef Q_OS_WIN
    wchar_t shortName[32768];
    const DWORD len = GetShortPathNameW(reinterpret_cast<LPCWSTR>(dir.utf16()),
                                         shortName, DWORD(std::size(shortName)));
    if (len && len < std::size(shortName)) dir = QString::fromWCharArray(shortName, int(len));
#endif
    return QDir(dir).filePath(QStringLiteral("ffmpeg2pass"));
}

static QStringList buildFfmpegCommand(const FFmpegTask& task, const EncodeSource& src,
                                      const SubtitleLayer& layer, const QString& destination,
                                      const QString& codec, int quality, const QString& gpuFormat,
                                      bool softwareDecode, const QString& targetBitrate = {},
                                      int pass = 0, const QString& passLog = {}) {
    QStringList cmd{"-nostdin", "-hide_banner", "-xerror"};
    const bool gpu = !gpuFormat.isEmpty();
    const bool twoPass = !gpu && pass > 0 && !targetBitrate.isEmpty();
    if (gpu) cmd << "-loglevel" << "level+info";
    const QString container = QFileInfo(destination).suffix().toLower();

    // 硬件加速
    if (!softwareDecode && boolValue(ConfigKeys::ffmpegUseHardwareAcceleration)) {
        const QString accelerator = stringValue(ConfigKeys::ffmpegHardwareAccelerator);
        if (accelerator != QStringLiteral("auto"))
            cmd << QStringLiteral("-hwaccel") << accelerator;
    }

    // 输入视频
    cmd << "-i" << task.inputPath;
    if (!layer.script.isEmpty()) cmd << "-f" << "avisynth" << "-i" << layer.script;
    const QString preset = stringValue(ConfigKeys::ffmpegPreset);
    if (gpu) {
        cmd << "-c:v" << (codec == "libx265" ? "hevc_nvenc" : "h264_nvenc")
            << "-pix_fmt" << gpuFormat << "-preset" << nvencPreset(preset) << "-tune" << "hq"
            << "-rc" << "vbr" << "-cq" << QString::number(nvencQuality(codec, quality)) << "-b:v" << "0"
            << "-multipass" << "fullres" << "-rc-lookahead" << "32" << "-g" << "300"
            << "-bf" << "4" << "-b_ref_mode" << "middle" << "-spatial-aq" << "1"
            << "-temporal-aq" << "1";
    } else {
        cmd << "-c:v" << codec;
        if (twoPass) {
            cmd << "-b:v" << targetBitrate << "-pass" << QString::number(pass)
                << "-passlogfile" << passLog;
        } else if (codec != "copy") {
            cmd << "-crf" << QString::number(quality);
        }
        if (codec == "libx264" || codec == "libx265") cmd << "-preset" << preset;
    }
    if (codec == "libx265") {
        if (pass != 1 && (container == "mp4" || container == "mov")) cmd << "-tag:v" << "hvc1";
        const QString fmt = src.video["pix_fmt"].toString();
        if (!gpu && (fmt == "yuv420p" || fmt == "yuv422p" || fmt == "yuv444p"))
            cmd << "-pix_fmt" << fmt + "10le";
    }
    if (codec == "libx264" && boolValue(ConfigKeys::ffmpegUseAdvanced)) {
        const QStringList x264Params{
            QStringLiteral("ref=") + QString::number(intValue(ConfigKeys::ffmpegRefFrames)),
            QStringLiteral("bframes=") + QString::number(intValue(ConfigKeys::ffmpegBFrames)),
            QStringLiteral("keyint=") + QString::number(intValue(ConfigKeys::ffmpegKeyint)),
            QStringLiteral("minkeyint=") + QString::number(intValue(ConfigKeys::ffmpegMinkeyint)),
            QStringLiteral("scenecut=") + QString::number(intValue(ConfigKeys::ffmpegScenecut)),
            QStringLiteral("qcomp=") + QString::number(doubleValue(ConfigKeys::ffmpegQcomp)),
            QStringLiteral("psy-rd=") + stringValue(ConfigKeys::ffmpegPsyRd),
            QStringLiteral("aq-mode=") + QString::number(intValue(ConfigKeys::ffmpegAqMode)),
            QStringLiteral("aq-strength=") + QString::number(doubleValue(ConfigKeys::ffmpegAqStrength)),
        };
        cmd << QStringLiteral("-x264-params") << x264Params.join(QLatin1Char(':'));
    }

    QString scale;
    // 视频缩放
    const QString scaleOption = stringValue(ConfigKeys::ffmpegScale);
    if (scaleOption != QStringLiteral("none")) {
        if (scaleOption == QStringLiteral("custom")) {
            const QString customScale = stringValue(ConfigKeys::ffmpegCustomScale);
            if (!customScale.isEmpty())
                scale = "scale=" + customScale;
        } else {
            static const QHash<QString, QString> resolutionMap{
                {QStringLiteral("720p"), QStringLiteral("1280:720")},
                {QStringLiteral("1080p"), QStringLiteral("1920:1080")},
                {QStringLiteral("1440p"), QStringLiteral("2560:1440")},
                {QStringLiteral("2160p"), QStringLiteral("3840:2160")},
            };
            const QString resolution = resolutionMap.value(scaleOption);
            if (!resolution.isEmpty())
                scale = "scale=" + resolution;
        }
    }

    if (layer.script.isEmpty()) {
        cmd << "-map" << QStringLiteral("0:%1").arg(src.video["index"].toInt());
        if (!scale.isEmpty()) cmd << "-vf" << scale;
    } else {
        QString graph = layer.graph;
        if (!scale.isEmpty()) graph += ',' + scale;
        cmd << "-filter_complex" << graph + "[video]" << "-map" << "[video]";
        cmd << layer.colorArgs;
    }

    if (pass == 1) {
        cmd << "-an";
    } else {
        const QString audioMode = stringValue(ConfigKeys::ffmpegAudioMode);
        const QString audioCodec = src.audio["codec_name"].toString();
        const bool compatible = container == "mkv" || ((container == "mp4" || container == "mov")
            && (audioCodec == "aac" || audioCodec == "mp3" || audioCodec == "ac3" || audioCodec == "eac3"))
            || (container == "webm" && (audioCodec == "opus" || audioCodec == "vorbis"));
        if (!src.audio.isEmpty() && audioMode != "none") {
            cmd << "-map" << QStringLiteral("0:%1").arg(src.audio["index"].toInt());
            const QString enc = audioMode == "copy" || (audioMode == "auto" && compatible)
                ? "copy" : stringValue(ConfigKeys::ffmpegAudioCodec);
            cmd << "-c:a" << enc;
            if (enc != "copy") cmd << "-b:a" << stringValue(ConfigKeys::ffmpegAudioBitrate);
        } else cmd << "-an";
    }

    // 帧率设置
    const QString fpsOption = stringValue(ConfigKeys::ffmpegFps);
    if (fpsOption != QStringLiteral("source"))
        cmd << QStringLiteral("-r") << fpsOption;
    else if (codec != "copy") cmd << "-fps_mode" << "passthrough" << "-enc_time_base:v" << "filter";

    if (codec == "libvpx-vp9") {
        const QString manualLimit = stringValue(ConfigKeys::ffmpegVideoBitrate).trimmed();
        cmd << "-b:v" << (manualLimit.isEmpty() ? QStringLiteral("0") : manualLimit);
    }
    if (pass == 1) {
#ifdef Q_OS_WIN
        cmd << "-f" << "null" << "NUL";
#else
        cmd << "-f" << "null" << "/dev/null";
#endif
    } else {
        cmd << "-y" << destination;
    }
    return cmd;
}

FFmpegWorker::FFmpegWorker(std::shared_ptr<FFmpegTask> task)
    : TaskWorker(nullptr), task_(std::move(task)) {
    // autoDelete 关闭与 QObject 归属由 TaskWorker 基类统一处理
}

void FFmpegWorker::run() {
    const QString currentTime =
        task_->createTime.toString(QStringLiteral("yyyy-MM-dd_hh-mm-ss"));
    taskLogger_ = Logger::get(
        QStringLiteral("Tasks/") + currentTime + QStringLiteral("_taskID-") +
            QString::number(task_->taskId),
        QStringLiteral("ffmpeg"));
    task_->logPath = taskLogger_->logFilePath();

    emit GlobalEventBus::instance().updateTaskStatusSig(
        task_->taskId, 0, QVariant(int(TaskStatus::Processing)),
        QStringLiteral("0KiB"), 0.0, QStringLiteral("0kbits/s"), 0.0);
    QString error;
    const bool success = !cancelled_.load() && encode(error);
    if (!error.isEmpty()) taskLogger_->error(error);
    finish(success);
    deleteLater();
}

bool FFmpegWorker::encode(QString& error) {
    const QFileInfo out(task_->outputPath);
    const bool overwrite = boolValue(ConfigKeys::ffmpegOverwriteOutput);
    if (out.exists() && (!overwrite || out == QFileInfo(task_->inputPath) || out == QFileInfo(task_->subtitlePath))) {
        error = QStringLiteral("输出路径与输入相同，或现有文件受覆盖设置保护：") + out.absoluteFilePath();
        return false;
    }
    QTemporaryDir work(out.dir().filePath(QStringLiteral(".fkw-encode-XXXXXX")));
    if (!work.isValid()) { error = work.errorString(); return false; }
    QString probe = QFileInfo(ffmpegPath()).dir().filePath("ffprobe" + executableSuffix());
    if (!QFileInfo(probe).isFile()) probe = QDir(sourceRoot()).filePath("tools/ffprobe" + executableSuffix());
    EncodeSource src;
    if (!probeEncodeSource(probe, task_->inputPath, src, error)) return false;
    duration_ = src.duration;
    const QString codec = stringValue(ConfigKeys::ffmpegVideoCodec);
    const int quality = intValue(ConfigKeys::ffmpegCrf);
    SubtitleLayer layer;
    if (!task_->subtitlePath.isEmpty()) {
        if (codec == "copy") {
            error = QStringLiteral("烧录字幕需要视频编码器，请在压制高级设置中选择 H.265 或 H.264。");
            return false;
        }
        const QString plugin = QDir(sourceRoot()).filePath("tools/vsfiltermod/VSFilterMod.dll");
        if (!makeSubtitleLayer(src, task_->subtitlePath, plugin, work.path(), layer, error)) return false;
        taskLogger_->info(layer.description);
    }
    const QString staged = work.filePath("output." + out.suffix());
    const QString manualLimit = stringValue(ConfigKeys::ffmpegVideoBitrate).trimmed();
    QString twoPassTarget;
    if (codec == "libx264" || codec == "libx265") {
        if (!manualLimit.isEmpty())
            twoPassTarget = manualLimit;
        else if (boolValue(ConfigKeys::ffmpegSourceRateLimit))
            twoPassTarget = sourceTargetBitrate(src, codec);
    }
    if (!twoPassTarget.isEmpty()) {
        taskLogger_->info(
            QStringLiteral("启用 2-Pass VBR 体积控制（CPU %1，目标平均码率 %2）").arg(codec, twoPassTarget));
        const QString passLog = passLogPrefix(work.path());
        const auto runPass = [&](int pass) {
            progressOffset_ = pass == 1 ? 0.0 : 50.0;
            progressScale_ = 0.5;
            lastEmit_ = 0;
            const auto args = buildFfmpegCommand(*task_, src, layer, staged, codec, quality, {},
                                                 false, twoPassTarget, pass, passLog);
            taskLogger_->info(ffmpegPath() + ' ' + args.join(' '));
            return !cancelled_.load() && runStage(args, nullptr);
        };
        if (!runPass(1) || !runPass(2)) return false;
    } else {
        progressOffset_ = 0.0;
        progressScale_ = 1.0;
        const QString gpuFormat = task_->preferGpu
            ? nvencPixelFormat(codec, src.video["pix_fmt"].toString(), quality) : QString();
        const auto attempt = [&](const QString& format, bool softwareDecode, bool* nvencFailed) {
            const auto args = buildFfmpegCommand(*task_, src, layer, staged, codec, quality, format, softwareDecode);
            taskLogger_->info(ffmpegPath() + ' ' + args.join(' '));
            return !cancelled_.load() && runStage(args, nvencFailed);
        };
        bool nvencFailed = false;
        if (task_->preferGpu && gpuFormat.isEmpty())
            taskLogger_->info(QStringLiteral("当前编码设置或片源像素格式使用原编码方式：") + codec);
        if (!attempt(gpuFormat, false, gpuFormat.isEmpty() ? nullptr : &nvencFailed)) {
            if (!nvencFailed || cancelled_.load()) return false;
            taskLogger_->warning(QStringLiteral("NVENC 不可用，使用 CPU 从头压制：") + codec);
            lastEmit_ = 0;
            if (!attempt({}, true, nullptr)) return false;
        }
    }
    if (cancelled_.load()) return false;
#ifdef Q_OS_WIN
    const bool moved = MoveFileExW(reinterpret_cast<LPCWSTR>(staged.utf16()),
        reinterpret_cast<LPCWSTR>(out.absoluteFilePath().utf16()), overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
#else
    const bool moved = overwrite ? std::rename(QFile::encodeName(staged), QFile::encodeName(out.absoluteFilePath())) == 0
                                 : QFile::rename(staged, out.absoluteFilePath());
#endif
    if (!moved) error = QStringLiteral("成片写入失败，目标文件可能正被占用：") + out.absoluteFilePath();
    return moved;
}

bool FFmpegWorker::runStage(const QStringList& args, bool* nvencFailed) {
    QProcess process;
#ifdef Q_OS_WIN
    // VSFilterMod uses the system ANSI code page for AviSynth paths.
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
        a->flags |= CREATE_NO_WINDOW;
    });
#endif
    if (!task_->subtitlePath.isEmpty())
        process.setWorkingDirectory(QFileInfo(task_->subtitlePath).absolutePath());
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("PATH", QDir(sourceRoot()).filePath("tools") + QDir::listSeparator() + env.value("PATH"));
    process.setProcessEnvironment(env);
    process.start(ffmpegPath(), args);
    if (!process.waitForStarted()) { taskLogger_->error(process.errorString()); return false; }
    // Only retry hardware initialization or encoding errors. Input, subtitle and disk
    // failures still report their original error without replaying the whole job.
    static const QRegularExpression gpuError(QStringLiteral(
        R"(\[(?:hevc|h264)_nvenc @[^\]]*\]\s*\[(?:error|fatal|panic)\]|)"
        R"(No device available for decoder|Device setup failed for decoder|)"
        R"(Unknown encoder '(?:hevc|h264)_nvenc'|)"
        R"(Unrecognized option '(?:rc|cq|multipass|rc-lookahead|spatial-aq|temporal-aq|b_ref_mode)')"),
        QRegularExpression::CaseInsensitiveOption);
    QString tail;
    do {
        if (cancelled_.load()) { process.kill(); process.waitForFinished(); return false; }
        process.waitForFinished(150);
        const QString data = QString::fromUtf8(process.readAllStandardError());
        if (!data.isEmpty()) {
            if (nvencFailed && !*nvencFailed) {
                tail += data;
                *nvencFailed = gpuError.match(tail).hasMatch();
                tail = tail.right(512);
            }
            parseProgress(data);
            taskLogger_->info(data);
        }
    } while (process.state() != QProcess::NotRunning);
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

void FFmpegWorker::cancel() {
    cancelled_.store(true);
}

void FFmpegWorker::parseProgress(const QString& data) {
    // 解析当前压制进度，节流到每秒最多 4 次
    if (duration_ <= 0) return;
    const double now = QDateTime::currentMSecsSinceEpoch() / 1000.0;
    if (now - lastEmit_ < 0.25) return;
    const auto match = timeRe().match(data);
    if (!match.hasMatch()) return;

    lastEmit_ = now;
    const double current = round2(match.captured(1).toInt() * 3600 +
                                  match.captured(2).toInt() * 60 +
                                  match.captured(3).toDouble());
    const int progress = qBound(
        0, int(progressOffset_ + (current / duration_ * 100.0) * progressScale_), 100);

    QString size;
    const auto sizeMatch = sizeRe().match(data);
    if (sizeMatch.hasMatch()) size = sizeMatch.captured(1);

    QString bitrate;
    const auto bitrateMatch = bitrateRe().match(data);
    if (bitrateMatch.hasMatch()) bitrate = bitrateMatch.captured(1);

    double speed = 0.0;
    const auto speedMatch = speedRe().match(data);
    if (speedMatch.hasMatch()) speed = round2(speedMatch.captured(1).toDouble());

    emit GlobalEventBus::instance().updateTaskStatusSig(
        task_->taskId, progress, QVariant(int(TaskStatus::Processing)), size,
        current, bitrate, speed);
}

void FFmpegWorker::finish(bool success) {
    // 任务结束统一处理：关日志、emit 完成信号（取消的任务不 emit）
    if (taskLogger_) taskLogger_->close();
    if (cancelled_.load()) return;
    emit GlobalEventBus::instance().finishTaskSig(task_->taskId, success,
                                                  task_->logPath);
}

}  // namespace fkw
