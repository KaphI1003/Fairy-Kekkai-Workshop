#pragma once
#include <QString>
#include <QProcessEnvironment>
namespace fkw {
struct PaddleOcrModelDirs {
    QString detection;
    QString recognition;
    QString classification;
};
PaddleOcrModelDirs resolveModelDirs(const QString& language, bool useServerModel,
                                    const QString& supportFilesPath = {});
// 显式设备路径优先，其次迁移旧的单引擎路径，最后使用当前代默认引擎。
QString selectPaddleOcrEngine(bool useGpu, const QString& devicePath,
                            const QString& legacyPath, const QString& defaultPath);
QString configuredPaddleOcrEngine(bool useGpu);
QProcessEnvironment paddleOcrEnvironment(bool useGpu);
}
