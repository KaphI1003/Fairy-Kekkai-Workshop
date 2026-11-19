#include "service/paddleocr.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <stdexcept>

#include "common/app_data.h"
#include "common/setting.h"
#include "common/config.h"
#include "common/config_keys.h"

namespace fkw {
QString selectPaddleOcrEngine(bool useGpu, const QString& devicePath,
                            const QString& legacyPath, const QString& defaultPath) {
    if (!devicePath.trimmed().isEmpty()) return QDir::cleanPath(devicePath);
    // 不把旧 GPU exe 当 CPU 用，反之亦然。未知的自定义引擎需通过新路径明确指定。
    const QString folder = QFileInfo(legacyPath).dir().dirName();
    if (folder.contains(useGpu ? QStringLiteral("-GPU-") : QStringLiteral("-CPU-"),
                        Qt::CaseInsensitive) && QFileInfo(legacyPath).isFile())
        return QDir::cleanPath(legacyPath);
    return defaultPath;
}
QString configuredPaddleOcrEngine(bool useGpu) {
    const auto& cfg = AppConfig::instance();
    // snapshot 区分用户指定路径和动态默认值，便于迁移旧配置。
    const QJsonObject section = cfg.snapshot().value(QStringLiteral("OCR")).toObject();
    return selectPaddleOcrEngine(useGpu,
        section.value(useGpu ? QStringLiteral("GpuPaddleocrPath")
                             : QStringLiteral("CpuPaddleocrPath")).toString(),
        section.value(QStringLiteral("PaddleocrPath")).toString(),
        useGpu ? paddleOcrGpuDefaultPath() : paddleOcrCpuDefaultPath());
}
QProcessEnvironment paddleOcrEnvironment(bool useGpu) {
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
    env.remove(QStringLiteral("PYTHONHOME"));
    env.remove(QStringLiteral("PYTHONPATH"));
    const QString cacheKey = QStringLiteral("PADDLE_PDX_CACHE_HOME");
    if (!env.contains(cacheKey))
        env.insert(cacheKey, QDir(userDataFolder()).filePath(QStringLiteral("paddlex")));
    const QString sourceCheckKey = QStringLiteral("PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK");
    if (!env.contains(sourceCheckKey)) env.insert(sourceCheckKey, QStringLiteral("True"));
#ifdef Q_OS_WIN
    const QString fontKey = QStringLiteral("PADDLE_PDX_LOCAL_FONT_FILE_PATH");
    const QDir fonts(QDir(env.value(QStringLiteral("SystemRoot"))).filePath(QStringLiteral("Fonts")));
    const QString font = QFileInfo::exists(fonts.filePath(QStringLiteral("msyh.ttc")))
        ? fonts.filePath(QStringLiteral("msyh.ttc")) : fonts.filePath(QStringLiteral("segoeui.ttf"));
    if (!env.contains(fontKey) && QFileInfo(font).isFile()) env.insert(fontKey, font);
#endif
    if (!useGpu) env.insert(QStringLiteral("CUDA_VISIBLE_DEVICES"), QStringLiteral("-1"));
    return env;
}
PaddleOcrModelDirs resolveModelDirs(const QString& language, bool useServerModel,
                                    const QString& supportFilesPath) {
    static const QJsonObject languages = [] {
        QFile file(QStringLiteral(":/app/paddleocr_langs.json"));
        if (!file.open(QIODevice::ReadOnly)) return QJsonObject{};
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    const QString base = supportFilesPath.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).filePath(paddleOcrSupportFilesName())
        : supportFilesPath;
    const QString v6Mode = useServerModel ? QStringLiteral("medium")
                                          : QStringLiteral("small");
    const QString legacyMode = useServerModel ? QStringLiteral("server")
                                              : QStringLiteral("mobile");
    const bool isV6Supported =
        QStringList{QStringLiteral("ch"), QStringLiteral("chinese_cht"),
                    QStringLiteral("en"), QStringLiteral("japan")}.contains(language)
        || (languages.value(QStringLiteral("latin")).toArray().contains(language)
            && language != QStringLiteral("pi"));
    QString detection;
    QString recognition;
    if (isV6Supported) {
        detection = QStringLiteral("PP-OCRv6_") + v6Mode + QStringLiteral("_det");
        recognition = QStringLiteral("PP-OCRv6_") + v6Mode + QStringLiteral("_rec");
    } else {
        detection = language == QStringLiteral("ka")
            ? QStringLiteral("PP-OCRv3_mobile_det")
            : QStringLiteral("PP-OCRv5_") + legacyMode + QStringLiteral("_det");
        for (const QString& group : {QStringLiteral("latin"), QStringLiteral("arabic"),
                                     QStringLiteral("eslav"), QStringLiteral("cyrillic"),
                                     QStringLiteral("devanagari")}) {
            if (languages.value(group).toArray().contains(language)) {
                recognition = group + QStringLiteral("_PP-OCRv5_mobile_rec");
                break;
            }
        }
        if (recognition.isEmpty() &&
            QStringList{QStringLiteral("korean"), QStringLiteral("th"),
                        QStringLiteral("el"), QStringLiteral("te"),
                        QStringLiteral("ta")}.contains(language))
            recognition = language + QStringLiteral("_PP-OCRv5_mobile_rec");
        if (language == QStringLiteral("ka"))
            recognition = QStringLiteral("ka_PP-OCRv3_mobile_rec");
    }
    if (recognition.isEmpty())
        throw std::invalid_argument("Unsupported PaddleOCR language");
    const QDir directory(base);
    return {directory.filePath(QStringLiteral("det/") + detection),
            directory.filePath(QStringLiteral("rec/") + recognition),
            directory.filePath(QStringLiteral("cls/PP-LCNet_x1_0_textline_ori"))};
}
}  // namespace fkw
