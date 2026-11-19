#include "service/subtitle_encode.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <iterator>
#include <cmath>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace fkw {
namespace {
const QStringList subtitleTypes{"ass", "ssa", "srt"};

QString avsPath(const QString& path) {
    const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    wchar_t shortName[32768];
    const DWORD len = GetShortPathNameW(reinterpret_cast<LPCWSTR>(native.utf16()),
                                         shortName, DWORD(std::size(shortName)));
    if (len && len < std::size(shortName)) return QString::fromWCharArray(shortName, int(len));
#endif
    return native;
}

bool hasModTags(const QByteArray& text) {
    // Extended tags are useful for the log. Every ASS uses the Mod renderer,
    // including scripts with newer or less common override tags.
    static const QRegularExpression mod(QStringLiteral(
        R"(\\(?:jitter|[1-4](?:vc|va|img)|distort|rnd[xyz]?|mover|moves[234]?|fsvp|frs)(?=[(\d+.-]))"),
        QRegularExpression::CaseInsensitiveOption);
    return mod.match(QString::fromLatin1(text).remove(QChar::Null)).hasMatch();
}
}

QString findEncodeSubtitle(const QString& video, bool* ambiguous) {
    if (ambiguous) *ambiguous = false;
    const QFileInfo info(video);
    if (!info.isFile() || info.suffix().compare("avs", Qt::CaseInsensitive) == 0) return {};
    const QString stem = info.completeBaseName();
    if (stem.startsWith(QStringLiteral("熟肉"))) return {};
    QStringList names{stem};
    if (stem == QStringLiteral("生肉"))
        names << QStringLiteral("译文") << QStringLiteral("简体中文字幕") << "subs";
    const bool projectRaw = stem == QStringLiteral("生肉");
    const bool onlyVideo = info.dir().entryList({"*.mp4", "*.mkv", "*.flv", "*.avi", "*.mov", "*.webm",
        "*.wmv", "*.mpg", "*.m4v", "*.ts", "*.m2ts", "*.m4s", "*.avs"}, QDir::Files).size() == 1;
    for (const QString& ext : subtitleTypes) {
        for (const QString& name : names) {
            const QFileInfo sub(info.dir().filePath(name + '.' + ext));
            if (sub.isFile()) return sub.absoluteFilePath();
        }
        // A single language-qualified sidecar is unambiguous (episode.zh-Hans.ass).
        const QRegularExpression localized("^" + QRegularExpression::escape(stem)
            + R"(\.[a-z]{2,3}(?:[-.][a-z]{2,4})?\.)" + ext + "$",
            QRegularExpression::CaseInsensitiveOption);
        QStringList matches;
        for (const QString& file : info.dir().entryList({"*." + ext}, QDir::Files))
            if (localized.match(file).hasMatch()) matches << file;
        if (matches.size() == 1) return info.dir().absoluteFilePath(matches.first());
        if (matches.isEmpty() && (projectRaw || onlyVideo)) {
            static const QRegularExpression ignored(
                QStringLiteral(R"(^(?:原文|熟肉)|(?:autosave|backup|\.bak\.|\.tmp\.|副本))"),
                QRegularExpression::CaseInsensitiveOption);
            for (const QString& file : info.dir().entryList({"*." + ext}, QDir::Files)) {
                if (!ignored.match(file).hasMatch()) matches << file;
            }
            if (matches.size() == 1) return info.dir().absoluteFilePath(matches.first());
        }
        if (matches.size() > 1) {
            if (ambiguous) *ambiguous = true;
            return {};
        }
    }
    return {};
}

QString projectEncodeInput(const QString& folder) {
    const QDir dir(folder);
    const QString raw = dir.filePath(QStringLiteral("生肉.mp4"));
    if (!findEncodeSubtitle(raw).isEmpty()) return raw;
    const QString cooked = dir.filePath(QStringLiteral("熟肉.mp4"));
    return QFileInfo(cooked).isFile() ? cooked : QString();
}

QString projectEncodeOutput(const QString& input) {
    const QFileInfo file(input);
    const QString cooked = file.dir().filePath(QStringLiteral("熟肉.mp4"));
    return file.completeBaseName() == QStringLiteral("生肉") && !QFileInfo::exists(cooked)
        ? cooked : file.dir().filePath(QStringLiteral("熟肉_压制.mp4"));
}

bool probeEncodeSource(const QString& probe, const QString& input, EncodeSource& source,
                       QString& error) {
    QProcess p;
    p.start(probe, {"-v", "error", "-show_streams", "-show_format", "-of", "json", input});
    if (!p.waitForFinished(30000) || p.exitStatus() != QProcess::NormalExit || p.exitCode()) {
        error = QStringLiteral("读取片源失败：%1 %2").arg(p.errorString(), QString::fromUtf8(p.readAllStandardError()));
        return false;
    }
    const auto root = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
    const auto format = root["format"].toObject();
    double otherBitrate = 0;
    for (const auto item : root["streams"].toArray()) {
        const auto s = item.toObject();
        const auto disp = s["disposition"].toObject();
        const auto type = s["codec_type"].toString();
        if (type == "video" && !disp["attached_pic"].toInt() && source.video.isEmpty())
            source.video = s;
        else {
            if (type == "audio" && (source.audio.isEmpty() || disp["default"].toInt()))
                source.audio = s;
            otherBitrate += s["bit_rate"].toString().toDouble();
        }
    }
    if (source.video.isEmpty()) {
        error = QStringLiteral("片源没有可压制的视频流。");
        return false;
    }
    source.duration = format["duration"].toString().toDouble();
    if (source.duration <= 0) source.duration = source.video["duration"].toString().toDouble();
    source.videoBitrate = source.video["bit_rate"].toString().toLongLong();
    if (format["format_name"].toString() == "avisynth") {
        source.videoBitrate = 0; // The AVS file size is unrelated to the decoded video.
    } else if (source.videoBitrate <= 0)
        source.videoBitrate = qMax(0.0, format["bit_rate"].toString().toDouble() - otherBitrate);
    return true;
}

bool makeSubtitleLayer(const EncodeSource& source, const QString& subtitle,
                       const QString& plugin, const QString& workDir,
                       SubtitleLayer& layer, QString& error) {
    if (!QFileInfo(subtitle).isFile() || !QFileInfo(plugin).isFile()) {
        error = QStringLiteral("找不到字幕或 FKW 的 VSFilterMod.dll：%1 / %2").arg(subtitle, plugin);
        return false;
    }
    const auto& v = source.video;
    int w = v["width"].toInt(), h = v["height"].toInt();
    for (auto data : v["side_data_list"].toArray()) {
        if (qAbs(data.toObject()["rotation"].toInt()) % 180 == 90) qSwap(w, h);
    }
    QString fmt = v["pix_fmt"].toString();
    static const QRegularExpression yuv(R"(^yuv(j?)(420|422|444)p(10le)?$)");
    const auto pixel = yuv.match(fmt);
    const QString trc = v["color_transfer"].toString();
    QString matrix = v["color_space"].toString();
    const bool assumed = matrix.isEmpty() || matrix == "unknown";
    if (assumed) matrix = (v["height"].toInt() > 576 || v["width"].toInt() >= 1280)
        ? "bt709" : "smpte170m";
    const QString primaries = v["color_primaries"].toString();
    if (!pixel.hasMatch() || (matrix != "bt709" && matrix != "smpte170m" && matrix != "bt470bg")
        || trc == "smpte2084" || trc == "arib-std-b67" || primaries == "bt2020") {
        error = QStringLiteral("当前字幕合成支持 BT.601 / BT.709 的 8 或 10 位 SDR YUV。片源为 %1 / %2 / %3。")
            .arg(fmt, matrix, trc);
        return false;
    }
    const bool full = v["color_range"].toString() == "pc" || !pixel.captured(1).isEmpty();
    const QString range = full ? "pc" : "tv";
    const QString sampling = pixel.captured(2);
    const bool tenBit = !pixel.captured(3).isEmpty();
    QStringList rate = v["r_frame_rate"].toString().split('/');
    int num = rate.value(0).toInt(), den = rate.value(1).toInt();
    if (num <= 0 || den <= 0 || double(num) / den > 240) {
        num = 60;
        den = 1;
    }
    const double fps = double(num) / den;
    // Framesync follows the original video's timestamps, including variable frame rates.
    // A finite subtitle source also allows seeking/probing without decoding the video in AVS.
    const double frames = std::ceil((source.duration + 2) * fps);
    if (source.duration <= 0 || frames > 2147483646 || w <= 0 || h <= 0) {
        error = QStringLiteral("无法确定视频尺寸或字幕时间轴。");
        return false;
    }
    // The legacy plugin reads ANSI filenames and treats unmarked text as ANSI.
    // An ASCII filename relative to the AVS script also works in Unicode directories.
    QFile sourceText(subtitle);
    if (!sourceText.open(QIODevice::ReadOnly)) { error = sourceText.errorString(); return false; }
    QByteArray payload = sourceText.readAll();
    if (sourceText.error() != QFile::NoError) { error = sourceText.errorString(); return false; }
    const QString extension = QFileInfo(subtitle).suffix().toLower();
    if (!subtitleTypes.contains(extension)) { error = QStringLiteral("请选择 ASS、SSA 或 SRT 字幕。"); return false; }
    if (!payload.startsWith("\xEF\xBB\xBF") && payload.isValidUtf8())
        payload.prepend("\xEF\xBB\xBF");
    const QString name = "text." + extension;
    QFile snapshot(QDir(workDir).filePath(name));
    if (!snapshot.open(QIODevice::WriteOnly) || snapshot.write(payload) != payload.size() || !snapshot.flush()) {
        error = snapshot.errorString();
        return false;
    }
    QString dll = avsPath(plugin);
    if (QString::fromLocal8Bit(dll.toLocal8Bit()) != dll) {
        dll = "VSFilterMod.dll";
        if (!QFile::copy(plugin, QDir(workDir).filePath(dll))) { error = QStringLiteral("无法暂存 VSFilterMod。"); return false; }
    }
    // MaskSubMod writes its RGBA rows top-down into a bottom-up AviSynth RGB32 clip.
    // Restore native TextSubMod orientation before FFmpeg decodes the layer.
    const QString script = QStringLiteral(
        "LoadPlugin(\"%1\")\n"
        "MaskSubMod(\"%2\", %3, %4, %5.0 / %6, %7).KillAudio().FlipVertical().AssumeFPS(%5, %6)\n")
        .arg(dll, name)
        .arg(w).arg(h).arg(num).arg(den).arg(int(frames));
    const QByteArray bytes = script.toLocal8Bit();
    layer.script = QDir(workDir).filePath("subtitle.avs");
    QFile avs(layer.script);
    if (!avs.open(QIODevice::WriteOnly) || avs.write(bytes) != bytes.size() || !avs.flush()) {
        error = avs.errorString();
        return false;
    }
    const QString alphaFormat = "yuva" + sampling + "p" + (tenBit ? "10le" : "");
    const QString overlayFormat = "yuv" + sampling + (tenBit ? "p10" : "");
    layer.graph = QStringLiteral(
        "[0:%1]setparams=colorspace=%2:range=%3[base];"
        "[1:v]scale=iw:ih:in_range=pc:out_range=%3:out_color_matrix=%4,format=%5,"
        "setparams=colorspace=%2:range=%3[sub];"
        "[base][sub]overlay=format=%6:alpha=straight:eof_action=pass:ts_sync_mode=nearest")
        .arg(v["index"].toInt()).arg(matrix, range, matrix == "bt709" ? "bt709" : "bt601",
                                     alphaFormat, overlayFormat);
    layer.colorArgs = {"-colorspace", matrix, "-color_range", range};
    if (!trc.isEmpty() && trc != "unknown") layer.colorArgs << "-color_trc" << trc;
    if (!primaries.isEmpty() && primaries != "unknown") layer.colorArgs << "-color_primaries" << primaries;
    layer.description = QStringLiteral("%1 · VSFilterMod%2 · %3 %4%5")
        .arg(subtitle, hasModTags(payload) ? QStringLiteral("（检测到扩展特效）") : QString(),
             matrix, range, assumed ? QStringLiteral("（按尺寸推断矩阵）") : QString());
    return true;
}
} // namespace fkw
