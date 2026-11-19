#pragma once

#include <QObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

#include "service/paddleocr.h"

namespace fkw {
// 后台执行真实识别，不能仅用 exe 文件名或 nvidia-smi 判断 GPU 可用。
class OcrDeviceProbe : public QObject {
    Q_OBJECT
public:
    explicit OcrDeviceProbe(QObject* parent = nullptr);
    ~OcrDeviceProbe() override;
    void start(const QString& enginePath, bool useGpu, const PaddleOcrModelDirs& models);
signals:
    void finished(bool success, const QString& detail);
private:
    void finishOnce(bool success, const QString& detail);
    QProcess process_;
    QTimer timeout_;
    QTemporaryDir temp_;
    QString output_;
    bool done_ = false;
};
}
