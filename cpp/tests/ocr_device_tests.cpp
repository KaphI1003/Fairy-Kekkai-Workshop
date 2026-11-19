#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <qtfluentwidgets.h>

#include "common/app_data.h"
#include "common/config.h"
#include "common/config_keys.h"
#include "common/setting.h"
#include "components/config_card.h"
#include "service/ocr_device_probe.h"
#include "service/ocr_migration.h"
#include "service/paddleocr.h"

namespace {
void writeFile(const QString& path, const QByteArray& contents = {}) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) qFatal("Could not create test fixture");
    file.write(contents);
}
}
class OcrDeviceTests : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        writeFile(QDir(fkw::sourceRoot()).filePath("PADDLEOCR"), "PaddleOCR-GPU-v3.7.0-CUDA-12.9");
    }
    void routeLegacyAndExplicitPaths() {
        QTemporaryDir temp;
        const QString gpu = temp.filePath("PaddleOCR-GPU-v3.7.0-CUDA-12.9/paddleocr.exe");
        const QString cpu = temp.filePath("PaddleOCR-CPU-v3.7.0/paddleocr.exe");
        writeFile(gpu);
        writeFile(cpu);
        QCOMPARE(fkw::selectPaddleOcrEngine(false, {}, gpu, cpu), cpu);
        QCOMPARE(fkw::selectPaddleOcrEngine(true, {}, cpu, gpu), gpu);
        QCOMPARE(fkw::selectPaddleOcrEngine(true, {}, gpu, "fallback"), gpu);
        QCOMPARE(fkw::selectPaddleOcrEngine(false, cpu, gpu, "fallback"), cpu);
        // 错误的显式路径不能静默换成别的引擎，否则设置显示与实际运行不一致。
        QCOMPARE(fkw::selectPaddleOcrEngine(false, "missing.exe", gpu, cpu), QString("missing.exe"));
    }
    void sameGenerationSurvivesCleanup() {
        const QString tools = QDir(fkw::sourceRoot()).filePath("tools");
        for (const QString& name : fkw::paddleOcrEngineNames(fkw::paddleOcrVersion()))
            writeFile(QDir(tools).filePath(name + "/paddleocr.exe"));
        writeFile(QDir(tools).filePath("PaddleOCR-GPU-v1.5.1-CUDA-12.9/paddleocr.exe"));
        const auto obsolete = fkw::ocr::scanObsoleteResources();
        QStringList names;
        for (const auto& item : obsolete) names << item.name;
        QVERIFY(names.contains("PaddleOCR-GPU-v1.5.1-CUDA-12.9"));
        for (const QString& name : fkw::paddleOcrEngineNames(fkw::paddleOcrVersion()))
            QVERIFY(!names.contains(name));
        QVERIFY(QFileInfo(fkw::paddleOcrCpuDefaultPath()).isFile());
        QVERIFY(QFileInfo(fkw::paddleOcrGpuDefaultPath()).isFile());
    }
    void independentModelPrecision() {
        for (const QString& lang : {QString("ch"), QString("chinese_cht"), QString("en"), QString("japan")}) {
            const auto smallModels = fkw::resolveModelDirs(lang, false, "models");
            const auto mediumModels = fkw::resolveModelDirs(lang, true, "models");
            QVERIFY(smallModels.detection.endsWith("PP-OCRv6_small_det"));
            QVERIFY(smallModels.recognition.endsWith("PP-OCRv6_small_rec"));
            QVERIFY(mediumModels.detection.endsWith("PP-OCRv6_medium_det"));
            QVERIFY(mediumModels.recognition.endsWith("PP-OCRv6_medium_rec"));
        }
        QVERIFY(fkw::resolveModelDirs("korean", true, "models").recognition.endsWith("korean_PP-OCRv5_mobile_rec"));
    }
    void settingsPersistAndLegacyMigration() {
        auto& cfg = fkw::AppConfig::instance();
        QVERIFY(cfg.set(fkw::ConfigKeys::paddleocrPath, fkw::paddleOcrGpuDefaultPath()));
        QVERIFY(cfg.set(fkw::ConfigKeys::useGpu, false));
        QVERIFY(!cfg.value(fkw::ConfigKeys::useGpu).toBool());
        QCOMPARE(fkw::configuredPaddleOcrEngine(false), fkw::paddleOcrCpuDefaultPath());
        QCOMPARE(fkw::configuredPaddleOcrEngine(true), fkw::paddleOcrGpuDefaultPath());
        QVERIFY(cfg.set(fkw::ConfigKeys::useGpu, true));
        QVERIFY(cfg.value(fkw::ConfigKeys::useGpu).toBool());
    }
    void explicitlyConfiguredEngineIsProtected() {
        const QString name = "PaddleOCR-CPU-v0.0.1";
        const QString engine = QDir(fkw::sourceRoot()).filePath("tools/" + name + "/paddleocr.exe");
        writeFile(engine);
        auto& cfg = fkw::AppConfig::instance();
        QVERIFY(cfg.set(fkw::ConfigKeys::cpuPaddleocrPath, engine));
        QCOMPARE(fkw::configuredPaddleOcrEngine(false), engine);
        for (const auto& item : fkw::ocr::scanObsoleteResources()) QVERIFY(item.name != name);
        QVERIFY(cfg.set(fkw::ConfigKeys::cpuPaddleocrPath, fkw::paddleOcrCpuDefaultPath()));
    }
    void cpuDoesNotLoadCuda() {
        QCOMPARE(fkw::paddleOcrEnvironment(false).value("CUDA_VISIBLE_DEVICES"), QString("-1"));
    }
    void missingEngineFailsOnce() {
        fkw::OcrDeviceProbe probe;
        QSignalSpy spy(&probe, &fkw::OcrDeviceProbe::finished);
        probe.start("does-not-exist.exe", true, {});
        QCOMPARE(spy.size(), 1);
        QVERIFY(!spy.front().front().toBool());
    }
    void failedProbeKeepsCpuSelected() {
        auto& cfg = fkw::AppConfig::instance();
        QVERIFY(cfg.set(fkw::ConfigKeys::useGpu, false));
        // 本测试程序拒绝 OCR 命令参数，用作可启动但不能完成 GPU 推理的引擎。
        QVERIFY(cfg.set(fkw::ConfigKeys::gpuPaddleocrPath, QCoreApplication::applicationFilePath()));
        fkw::OCRSettingInterface settings;
        auto* gpu = settings.findChild<qfw::SwitchSettingCard*>("ocrGpuSwitch");
        QVERIFY(gpu);
        QVERIFY(gpu->isEnabled());
        QTimer closeError;
        connect(&closeError, &QTimer::timeout, &settings, [&closeError] {
            for (QWidget* widget : QApplication::allWidgets()) {
                if (auto* dialog = qobject_cast<qfw::MessageDialog*>(widget)) {
                    closeError.stop();
                    dialog->QDialog::done(QDialog::Accepted);
                }
            }
        });
        closeError.start(50);
        gpu->setChecked(true);
        // 尚未检查成功，不应提前保存 GPU 设置。
        QVERIFY(!cfg.value(fkw::ConfigKeys::useGpu).toBool());
        QTRY_VERIFY_WITH_TIMEOUT(gpu->isEnabled(), 15000);
        QVERIFY(!cfg.value(fkw::ConfigKeys::useGpu).toBool());
        QVERIFY(!gpu->isChecked());
    }
};
int main(int argc, char** argv) {
    // 隔离测试配置，绝不读写用户的项目或设置。
    QTemporaryDir localData;
    qputenv("LOCALAPPDATA", localData.path().toUtf8());
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(resource);
    if (app.arguments().value(1) == "--settings-preview" || app.arguments().value(1) == "--switch-smoke") {
        const auto args = app.arguments();
        if (args.size() != 5) return 2;
        auto& cfg = fkw::AppConfig::instance();
        cfg.set(fkw::ConfigKeys::cpuPaddleocrPath, args[2]);
        cfg.set(fkw::ConfigKeys::gpuPaddleocrPath, args[3]);
        cfg.set(fkw::ConfigKeys::supportFilesPath, args[4]);
        cfg.set(fkw::ConfigKeys::useGpu, true);
        fkw::OCRSettingInterface settings;
        settings.setWindowTitle(QStringLiteral("OCR设备切换验证"));
        settings.resize(1050, 850);
        if (args[1] == "--switch-smoke") {
            auto* gpu = settings.findChild<qfw::SwitchSettingCard*>("ocrGpuSwitch");
            QObject::connect(&cfg, &fkw::AppConfig::valueChanged, &settings,
                [gpu, &app](const QString& group, const QString& key, const QJsonValue& value) {
                    if (group != "OCR" || key != "UseGpu") return;
                    if (!value.toBool()) {
                        qInfo() << "CPU_SWITCH_OK";
                        QTimer::singleShot(0, gpu, [gpu] { gpu->setChecked(true); });
                    } else {
                        qInfo() << "GPU_SWITCH_OK";
                        QTimer::singleShot(0, &app, [&app] { app.exit(0); });
                    }
                });
            QTimer::singleShot(0, gpu, [gpu] { gpu->setChecked(false); });
            QTimer::singleShot(45000, &app, [&app] { app.exit(1); });
        } else {
            settings.show();
        }
        return app.exec();
    }
    if (app.arguments().value(1) == "--probe") {
        const auto args = app.arguments();
        if (args.size() != 6) return 2;
        fkw::OcrDeviceProbe probe;
        QObject::connect(&probe, &fkw::OcrDeviceProbe::finished, &app,
            [&app](bool success, const QString& detail) {
                qInfo().noquote() << (success ? "PROBE_OK" : "PROBE_FAILED") << detail;
                QTimer::singleShot(0, &app, [&app, success] { app.exit(success ? 0 : 1); });
            });
        probe.start(args[2], args[3] == "gpu", fkw::resolveModelDirs("en", args[5] == "medium", args[4]));
        return app.exec();
    }
    OcrDeviceTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ocr_device_tests.moc"
