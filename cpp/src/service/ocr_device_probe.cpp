#include "service/ocr_device_probe.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>

#include "common/app_data.h"

namespace fkw {
OcrDeviceProbe::OcrDeviceProbe(QObject* parent) : QObject(parent) {
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, [this] {
        process_.kill();
        finishOnce(false, trText("设备检查超时，请检查 OCR 引擎和显卡驱动。"));
    });
    process_.setProcessChannelMode(QProcess::MergedChannels);
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        output_ += QString::fromUtf8(process_.readAllStandardOutput());
        output_ = output_.right(12000);
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finishOnce(false, process_.errorString());
    });
    connect(&process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        output_ += QString::fromUtf8(process_.readAllStandardOutput());
        if (code != 0 || status != QProcess::NormalExit) {
            finishOnce(false, output_.isEmpty() ? process_.errorString() : output_.right(3000));
            return;
        }
        QDirIterator files(temp_.path(), {QStringLiteral("*_res.json")},
                           QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            QFile file(files.next());
            if (!file.open(QIODevice::ReadOnly)) continue;
            const auto result = QJsonDocument::fromJson(file.readAll()).object();
            if (!result.value(QStringLiteral("rec_texts")).toArray().isEmpty()) {
                finishOnce(true, {});
                return;
            }
        }
        finishOnce(false, trText("引擎没有返回有效识别结果。") + QLatin1Char('\n')
                              + output_.right(2500));
    });
}
OcrDeviceProbe::~OcrDeviceProbe() {
    process_.disconnect(this);
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(2000);
    }
}
void OcrDeviceProbe::finishOnce(bool success, const QString& detail) {
    if (done_) return;
    done_ = true;
    timeout_.stop();
    emit finished(success, detail);
}
void OcrDeviceProbe::start(const QString& enginePath, bool useGpu,
                            const PaddleOcrModelDirs& models) {
    if (!temp_.isValid() || !QFileInfo(enginePath).isFile()) {
        finishOnce(false, trText("缺少 OCR 引擎，请在高级设置中选择对应的 CPU 或 GPU 引擎。"));
        return;
    }
    QImage image(480, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        painter.setPen(Qt::black);
        painter.setFont(QFont(QStringLiteral("Arial"), 28));
        painter.drawText(image.rect(), Qt::AlignCenter, QStringLiteral("OCR TEST 123"));
    }
    const QString input = QDir(temp_.path()).filePath(QStringLiteral("probe.png"));
    if (!image.save(input)) {
        finishOnce(false, trText("无法创建设备检查图片。"));
        return;
    }
    process_.setProgram(enginePath);
    process_.setWorkingDirectory(QFileInfo(enginePath).absolutePath());
    process_.setProcessEnvironment(paddleOcrEnvironment(useGpu));
    process_.setArguments({QStringLiteral("ocr"), QStringLiteral("--input"), input,
        QStringLiteral("--device"), useGpu ? QStringLiteral("gpu") : QStringLiteral("cpu"),
        QStringLiteral("--use_doc_orientation_classify"), QStringLiteral("false"),
        QStringLiteral("--use_doc_unwarping"), QStringLiteral("false"),
        QStringLiteral("--use_textline_orientation"), QStringLiteral("false"),
        QStringLiteral("--text_detection_model_dir"), models.detection,
        QStringLiteral("--text_detection_model_name"), QDir(models.detection).dirName(),
        QStringLiteral("--text_recognition_model_dir"), models.recognition,
        QStringLiteral("--text_recognition_model_name"), QDir(models.recognition).dirName(),
        QStringLiteral("--save_path"), temp_.path()});
    timeout_.start(120000);
    process_.start();
}
}
