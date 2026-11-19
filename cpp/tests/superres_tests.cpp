#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QDialog>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>
#include "common/event_bus.h"
#include "service/superres_service.h"
#include "service/superres_pipeline.h"
#include "service/video_frame_io.h"
#include "service/video_frame_analysis.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif
#include "service/video_frame_service.h"
#include "service/video_preview.h"
#include "view/superres_interface.h"

using namespace fkw;
namespace {
QByteArray run(const QString& executable, const QStringList& arguments) {
    QProcess process;
    process.start(executable, arguments);
    if (!process.waitForFinished(60000) || process.exitCode() != 0) {
        qWarning().noquote() << process.readAllStandardError();
        return {};
    }
    return process.readAllStandardOutput();
}
SuperresVideoInfo inspect(const SuperresOptions& options, const QString& file) {
    return parseSuperresVideoInfo(QJsonDocument::fromJson(run(options.ffprobePath, superresProbeArguments(file))).object());
}
}

class SuperresTests : public QObject {
    Q_OBJECT
    QTemporaryDir localData_;
private slots:
    void initTestCase() {
        Q_INIT_RESOURCE(resource);
        QStandardPaths::setTestModeEnabled(true);
        qputenv("LOCALAPPDATA", localData_.path().toUtf8());
#ifdef Q_OS_WIN
        QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc"));
        qApp->setFont(QFont(QStringLiteral("Microsoft YaHei"), 10));
#endif
    }
    void resolutionAndModels() {
        SuperresOptions options;
        QCOMPARE(options.targetSize({1280, 720}), QSize(1920, 1080));
        QCOMPARE(options.targetSize({720, 1280}), QSize(1080, 1920));
        QCOMPARE(options.targetSize({960, 720}), QSize(1440, 1080));
        options.sizeMode = QStringLiteral("custom"); options.width = 1919; options.height = 1079;
        QCOMPARE(options.targetSize({1280, 720}), QSize(1920, 1080));
        options.sizeMode = QStringLiteral("scale"); options.scale = 3;
        QCOMPARE(options.targetSize({1280, 720}), QSize(3840, 2160));
        QCOMPARE(options.modelScale({1280, 720}), 3);
        options.model = QStringLiteral("realesrgan-x4plus");
        QCOMPARE(options.modelScale({1280, 720}), 4);
        options.model = kAniSdSpanModel;
        QCOMPARE(options.modelScale({1280, 720}), 2);
        QCOMPARE(options.targetSize({1280, 720}), QSize(3840, 2160));
        QVERIFY(superresScaleFilter({1920, 1080}, QStringLiteral("crop")).contains(QStringLiteral("(iw-ow)/2")));
    }
    void fractionalRateAndOrientation() {
        const auto json = QJsonDocument::fromJson(R"({"streams":[{"codec_type":"video","width":720,"height":480,"sample_aspect_ratio":"8:9","avg_frame_rate":"30000/1001","r_frame_rate":"30000/1001","duration":"2.002","nb_frames":"60","side_data_list":[{"rotation":90}]},{"codec_type":"audio"}]})").object();
        const auto info = parseSuperresVideoInfo(json);
        QCOMPARE(info.size, QSize(480, 640)); QCOMPARE(info.frameRate, QStringLiteral("30000/1001"));
        QCOMPARE(info.frames, 60); QVERIFY(info.hasAudio); QVERIFY(!info.variableFrameRate);
        QVERIFY(std::abs(info.fps - 29.97002997) < .000001);
    }
    void detectsVariableRate() {
        const auto info = parseSuperresVideoInfo(QJsonDocument::fromJson(R"({"streams":[{"codec_type":"video","width":1280,"height":720,"avg_frame_rate":"25/1","r_frame_rate":"30/1","duration":"1"}]})").object());
        QVERIFY(info.variableFrameRate);
    }
    void renderingSpeedExcludesScanAndUsesRecentWindow() {
        SuperresSpeedMeter meter;
        QCOMPARE(meter.update(96000, 0), 0.0);
        meter.start(96000); // A 96-second scan must not dilute render throughput.
        for (int second = 1; second <= 8; ++second)
            QCOMPARE(meter.update(96000 + second * 1000, second * 5), 5.0);
        for (int second = 9; second <= 17; ++second) meter.update(96000 + second * 1000, 40);
        QCOMPARE(meter.update(114000, 40), 0.0); // A real pipeline stall is visible.
    }
    void normalizationMatchesExistingPixels() {
        const auto info = parseSuperresVideoInfo(QJsonDocument::fromJson(R"({"streams":[{"codec_type":"video","width":854,"height":474,"sample_aspect_ratio":"2133:2135","avg_frame_rate":"30/1"}]})").object());
        QCOMPARE(info.size, QSize(854, 474));
        QCOMPARE(info.decodedSize, QSize(852, 474));
        QCOMPARE(SuperresOptions().targetSize(info.size), QSize(1946, 1080));
    }
    void decoderBackpressureAndCancellation() {
#ifdef Q_OS_WIN
        auto options = SuperresOptions::fromConfig();
        QTemporaryDir fixture;
        const QString source = fixture.filePath("long-solid.mp4");
        run(options.ffmpegPath, {"-v", "error", "-f", "lavfi", "-i", "color=red:size=160x90:rate=30",
            "-frames:v", "4000", "-c:v", "libx264", "-preset", "ultrafast", source});
        QVERIFY(QFileInfo::exists(source));
        const auto info = inspect(options, source);
        QCOMPARE(info.frames, 4000);
        options.reuseIdenticalFrames = false; options.batchFrames = 4;
        QVector<qint64> refs(info.frames);
        for (int i = 0; i < refs.size(); ++i) refs[i] = i;
        const auto memory = [] {
            PROCESS_MEMORY_COUNTERS_EX counters{};
            counters.cb = sizeof(counters);
            if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) return quint64(0);
            return quint64(counters.PrivateUsage);
        };
        const quint64 before = memory(); QVERIFY(before > 0);
        std::atomic<bool> cancelled{false};
        auto pipeline = std::make_unique<SuperresPipeline>(source, options, info, 0, info.frames,
            QVector<QByteArray>{}, refs, fixture.filePath("cache"), Logger::get("backpressure", "superres", false), cancelled);
        // Deliberately stop consuming: the decoder must block instead of buffering
        // the remaining 170 MB of raw video inside QProcess's Windows pipe reader.
        QTest::qSleep(3000);
        const quint64 after = memory();
        cancelled.store(true);
        QElapsedTimer shutdown; shutdown.start(); pipeline.reset();
        QVERIFY(after > 0);
        QVERIFY(shutdown.elapsed() < 5000);
        const qint64 growth = qint64(after) - qint64(before);
        qInfo() << "Stalled decoder private-memory growth:" << growth;
        QVERIFY2(growth < 48 * 1024 * 1024, qPrintable(QString::number(growth)));
#else
        QSKIP("Windows asynchronous pipe regression");
#endif
    }
    void shortDecoderReportsFrameAndExitStatus() {
        auto options = SuperresOptions::fromConfig();
        QTemporaryDir fixture;
        const QString source = fixture.filePath("short.mp4");
        run(options.ffmpegPath, {"-v", "error", "-f", "lavfi", "-i", "color=red:size=64x36:rate=30",
            "-frames:v", "9", "-c:v", "libx264", source});
        const auto info = inspect(options, source);
        QCOMPARE(info.frames, 9);
        options.reuseIdenticalFrames = false; options.batchFrames = 4;
        QVector<qint64> refs(10);
        for (int i = 0; i < refs.size(); ++i) refs[i] = i;
        std::atomic<bool> cancelled{false};
        SuperresPipeline pipeline(source, options, info, 0, 10, {}, refs,
            fixture.filePath("cache"), Logger::get("short-decoder", "superres", false), cancelled);
        SuperresBatch batch;
        while (pipeline.next(batch, {})) {}
        QVERIFY(pipeline.failed());
        QVERIFY2(pipeline.error().contains("frame=9/10"), qPrintable(pipeline.error()));
        QVERIFY(pipeline.error().contains("exit=0 status=0"));
    }
    void boundedDecoderWakesOnPixels() {
        auto options = SuperresOptions::fromConfig();
        QTemporaryDir fixture;
        const QString source = fixture.filePath("decode-throughput.mp4");
        run(options.ffmpegPath, {"-v", "error", "-f", "lavfi", "-i", "color=red:size=852x474:rate=30",
            "-frames:v", "48", "-c:v", "libx264", "-preset", "ultrafast", source});
        const auto info = inspect(options, source); QCOMPARE(info.frames, 48);
        options.reuseIdenticalFrames = false; options.batchFrames = 4;
        QVector<qint64> refs(info.frames);
        for (int i = 0; i < refs.size(); ++i) refs[i] = i;
        std::atomic<bool> cancelled{false};
        QElapsedTimer elapsed; elapsed.start();
        SuperresPipeline pipeline(source, options, info, 0, info.frames, {}, refs,
            fixture.filePath("cache"), Logger::get("decode-throughput", "superres", false), cancelled);
        SuperresBatch batch; qint64 count = 0;
        while (pipeline.next(batch, {})) { count += batch.count; QDir(batch.directory).removeRecursively(); }
        QVERIFY2(!pipeline.failed(), qPrintable(pipeline.error())); QCOMPARE(count, info.frames);
        QVERIFY(pipeline.finish({}));
        qInfo() << "48-frame 852x474 decode and PNG preparation ms:" << elapsed.elapsed();
        // The former 10-ms process-handle poll stalled on every small pipe read.
        QVERIFY(elapsed.elapsed() < 4000);
    }
    void persistentEngineMatchesCli() {
        if (!qEnvironmentVariableIsSet("FKW_SUPERRES_INTEGRATION")) QSKIP("Requires Vulkan engines");
        qputenv("FKW_VIDEO_PNG_TRANSPORT", "1"); // Compare the same segmented encoder; shared-frame pixels have a separate check.
        auto options = SuperresOptions::fromConfig();
        const QString worker = QFileInfo(options.enginePath).dir().filePath("fkw-superres-worker.exe");
        QVERIFY(QFileInfo::exists(worker));
        QCOMPARE(run(worker, {"--version"}).trimmed(), QByteArray("FKW_SUPERRES_WORKER_1"));
        QTemporaryDir fixture;
        const QString source = fixture.filePath(QStringLiteral("彩色 & 多批次.mp4"));
        run(options.ffmpegPath, {"-v", "error", "-f", "lavfi", "-i", "testsrc2=size=64x36:rate=30",
            "-frames:v", "12", "-c:v", "libx264", source});
        options.tempPath = fixture.filePath("cache"); options.sizeMode = "scale";
        options.scale = 2; options.tile = 32; options.batchFrames = 4; options.reuseIdenticalFrames = false;
        options.model = "realesr-animevideov3";
        QStringList outputs;
        QThreadPool pool;
        for (bool legacy : {true, false}) {
            if (legacy) qputenv("FKW_SUPERRES_LEGACY_ENGINE", "1"); else qunsetenv("FKW_SUPERRES_LEGACY_ENGINE");
            auto task = std::make_shared<SuperresTask>(source, fixture.filePath(legacy ? "cli.mp4" : "resident.mp4"), options);
            auto* workerTask = new SuperresWorker(task); QSignalSpy completed(workerTask, &SuperresWorker::completed);
            pool.start(workerTask); QVERIFY(completed.wait(60000)); pool.waitForDone();
            qunsetenv("FKW_SUPERRES_LEGACY_ENGINE");
            if (!completed.first().at(0).toBool()) {
                QFile diagnostic(task->logPath);
                if (diagnostic.open(QIODevice::ReadOnly)) qWarning().noquote() << diagnostic.readAll().right(8000);
            }
            QVERIFY2(completed.first().at(0).toBool(), qPrintable(completed.first().at(3).toString()));
            QFile log(task->logPath); QVERIFY(log.open(QIODevice::ReadOnly));
            const QByteArray contents = log.readAll();
            QCOMPARE(contents.count("[engine-session-start]"), legacy ? 0 : 1);
            QCOMPARE(contents.count("[inferred]"), 3);
            outputs.append(task->outputPath);
        }
        const auto hashes = [&](const QString& path) { return run(options.ffmpegPath, {"-v", "error", "-i", path,
            "-map", "0:v:0", "-pix_fmt", "rgb24", "-f", "framemd5", "-"}); };
        const QByteArray baseline = hashes(outputs.first()); QVERIFY(!baseline.isEmpty());
        QCOMPARE(hashes(outputs.last()), baseline);
        qunsetenv("FKW_VIDEO_PNG_TRANSPORT");
    }
    void sharedFrameLifecycleAndPixels() {
        auto frame = frames::MemoryFrame::create(64, 36);
        QVERIFY(!frame->ready());
        for (size_t i = 0; i < frame->bytes(); ++i) frame->data()[i] = static_cast<unsigned char>(i * 37);
        frame->publish();
        const auto uri = frame->uri(); auto opened = frames::MemoryFrame::open(uri);
        QCOMPARE(opened->width(), 64); QCOMPARE(opened->height(), 36); QVERIFY(opened->ready());
        QCOMPARE(QByteArray(reinterpret_cast<char*>(frame->data()), frame->bytes()), QByteArray(reinterpret_cast<char*>(opened->data()), opened->bytes()));
        frame.reset(); QVERIFY(opened->ready()); opened.reset();
        QVERIFY_EXCEPTION_THROWN(frames::MemoryFrame::open(uri), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(frames::MemoryFrame::create(0, 36), std::runtime_error);
        QCOMPARE(videoMemoryBatch(120, 3840ULL * 2160 * 3, 3840ULL * 2160 * 3), 2);
    }
    void memoryWorkerMatchesPngPixels() {
        if (!qEnvironmentVariableIsSet("FKW_SUPERRES_INTEGRATION")) QSKIP("Requires Vulkan engine");
        auto options = SuperresOptions::fromConfig(); QTemporaryDir fixture;
        QImage image(64, 36, QImage::Format_RGB888);
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x)
            image.setPixelColor(x, y, QColor((x * 7 + y) % 256, (y * 11 + x) % 256, (x * 3 + y * 5) % 256));
        const QString input = fixture.filePath(QStringLiteral("彩色 input.png")); QVERIFY(image.save(input));
        auto original = frames::MemoryFrame::create(64, 36);
        for (int y = 0; y < 36; ++y) memcpy(original->data() + y * 64 * 3, image.constScanLine(y), 64 * 3);
        original->publish();
        const QString worker = QFileInfo(options.enginePath).dir().filePath("fkw-superres-worker.exe");
        for (int scale : {2, 3, 4}) {
            auto output = frames::MemoryFrame::create(64 * scale, 36 * scale);
            const QString reference = fixture.filePath(QStringLiteral("reference-%1.png").arg(scale));
            QFile jobs(fixture.filePath(QStringLiteral("jobs-%1.tsv").arg(scale))); QVERIFY(jobs.open(QIODevice::WriteOnly));
            const QByteArray rows = input.toUtf8() + '\t' + reference.toUtf8() + '\n' + QByteArray::fromStdString(original->uri()) + '\t' + QByteArray::fromStdString(output->uri()) + '\n';
            QCOMPARE(jobs.write(rows), rows.size()); jobs.close();
            QProcess process; process.start(worker, {options.modelsPath, "realesr-animevideov3", QString::number(scale), "-1", "32", scale == 4 ? "1" : "0"});
            QVERIFY(process.waitForStarted(10000)); process.write("BATCH\t" + jobs.fileName().toUtf8() + "\nQUIT\n"); process.closeWriteChannel();
            QVERIFY(process.waitForFinished(60000)); QVERIFY2(process.exitCode() == 0, process.readAllStandardError().constData());
            QVERIFY(output->ready()); const QImage png = QImage(reference).convertToFormat(QImage::Format_RGB888); QVERIFY(!png.isNull());
            for (int y = 0; y < png.height(); ++y)
                QCOMPARE(QByteArray(reinterpret_cast<const char*>(png.constScanLine(y)), png.width() * 3),
                    QByteArray(reinterpret_cast<const char*>(output->data() + size_t(y) * png.width() * 3), png.width() * 3));
        }
    }
    void actualRenderAndPreview() {
        auto options = SuperresOptions::fromConfig();
        if (!qEnvironmentVariableIsSet("FKW_SUPERRES_INTEGRATION")) QSKIP("Set FKW_SUPERRES_INTEGRATION=1 to exercise the installed Vulkan engine");
        QVERIFY2(options.validate().isEmpty(), qPrintable(options.validate()));
        QTemporaryDir fixture;
        options.tempPath = fixture.filePath(QStringLiteral("超分缓存"));
        options.sizeMode = QStringLiteral("scale"); options.scale = 2;
        options.model = QStringLiteral("realesr-animevideov3"); options.batchFrames = 5;
        options.gpu = -1; options.tile = 32; options.audio = QStringLiteral("aac");
        const QString source = fixture.filePath(QStringLiteral("东方 MMD 测试 & 音频.mp4"));
        run(options.ffmpegPath, {"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=64x36:rate=30000/1001", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-frames:v", "18", "-t", "0.6006", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", source});
        QVERIFY(QFileInfo::exists(source));
        const auto original = inspect(options, source);
        QCOMPARE(original.frames, 18);
        auto task = std::make_shared<SuperresTask>(source, fixture.filePath(QStringLiteral("超分结果.mp4")), options);
        auto* worker = new SuperresWorker(task);
        QSignalSpy finished(worker, &SuperresWorker::completed);
        QSignalSpy progress(worker, &SuperresWorker::progress);
        QThreadPool pool; pool.start(worker);
        QVERIFY(finished.wait(120000)); pool.waitForDone();
        QVERIFY2(finished.at(0).at(0).toBool(), qPrintable(finished.at(0).at(3).toString()));
        const auto rendered = inspect(options, task->outputPath);
        QCOMPARE(rendered.size, QSize(128, 72)); QCOMPARE(rendered.frames, original.frames);
        QCOMPARE(rendered.frameRate, original.frameRate); QVERIFY(rendered.hasAudio);
        QVERIFY(std::abs(rendered.duration - original.duration) < .04);
        QVERIFY(progress.size() > 4); QCOMPARE(progress.last().at(0).toInt(), 100);
        for (int i = 0; i < progress.size() - 1; ++i) QVERIFY(progress.at(i).at(0).toInt() < 100);
        QVERIFY(QDir(options.tempPath).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

        QFile pipelineLog(task->logPath); QVERIFY(pipelineLog.open(QIODevice::ReadOnly));
        const QString pipelineText = QString::fromUtf8(pipelineLog.readAll());
        struct Interval { qint64 offset, begin, end; };
        const auto intervals = [&pipelineText](const QString& expression) {
            QList<Interval> values;
            auto matches = QRegularExpression(expression).globalMatch(pipelineText);
            while (matches.hasNext()) { const auto m = matches.next(); values.append({m.captured(1).toLongLong(), m.captured(2).toLongLong(), m.captured(3).toLongLong()}); }
            return values;
        };
        const auto inference = intervals(QStringLiteral("\\[inferred\\] offset=(\\d+) fresh=\\d+ begin_tick=(\\d+) end_tick=(\\d+)"));
        const auto encoding = intervals(QStringLiteral("\\[(?:encoded|encode-submit)\\] offset=(\\d+) frames=\\d+ elapsed_ms=\\d+ begin_tick=(\\d+) end_tick=(\\d+)"));
        const auto preparation = intervals(QStringLiteral("\\[prepare\\] offset=(\\d+) frames=\\d+ fresh=\\d+ elapsed_ms=\\d+ begin_tick=(\\d+) end_tick=(\\d+)"));
        QVERIFY(inference.size() >= 2); QVERIFY(preparation.size() >= 2);
        QVERIFY(preparation.at(1).end <= inference.first().end);
        bool overlapped = false;
        for (const auto& cpu : encoding) for (const auto& gpu : inference)
            overlapped |= cpu.offset != gpu.offset && cpu.begin < gpu.end && gpu.begin < cpu.end;
        QVERIFY2(overlapped, "Encoding should overlap inference instead of blocking the GPU pipeline");

        // A preview beginning at a fractional-rate frame boundary must not lose
        // or duplicate frames as it crosses several cache batches.
        auto preview = std::make_shared<SuperresTask>(source, fixture.filePath(QStringLiteral("preview.mp4")), options);
        preview->preview = true; preview->previewStart = 5.0 / original.fps; preview->previewDuration = 8.0 / original.fps;
        worker = new SuperresWorker(preview); QSignalSpy previewFinished(worker, &SuperresWorker::completed); pool.start(worker);
        QVERIFY(previewFinished.wait(120000)); pool.waitForDone();
        QVERIFY2(previewFinished.at(0).at(0).toBool(), qPrintable(previewFinished.at(0).at(3).toString()));
        const auto previewInfo = inspect(options, preview->outputPath);
        QCOMPARE(previewInfo.frames, 8); QCOMPARE(previewInfo.frameRate, original.frameRate); QVERIFY(previewInfo.hasAudio);

        // Cancel a queued/starting render. It must neither publish output nor
        // retain its image cache.
        auto cancelled = std::make_shared<SuperresTask>(source, fixture.filePath(QStringLiteral("cancelled.mp4")), options);
        worker = new SuperresWorker(cancelled); QSignalSpy cancelledFinished(worker, &SuperresWorker::completed);
        connect(worker, &SuperresWorker::progress, worker, [worker](int, const QString& stage) {
            if (stage == QStringLiteral("超分处理")) worker->cancel();
        }, Qt::DirectConnection);
        pool.start(worker); QVERIFY(cancelledFinished.wait(30000)); pool.waitForDone();
        QVERIFY(cancelledFinished.at(0).at(1).toBool()); QVERIFY(!QFileInfo::exists(cancelled->outputPath));
        QVERIFY(QDir(options.tempPath).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

        // Fail the encoder while preparation is looking ahead. All background
        // processes must stop and release their files before cache cleanup.
        auto failed = std::make_shared<SuperresTask>(source, fixture.filePath(QStringLiteral("encoder-failed.mp4")), options);
        worker = new SuperresWorker(failed); QSignalSpy failedFinished(worker, &SuperresWorker::completed);
        bool injected = false;
        connect(worker, &SuperresWorker::progress, worker, [&](int, const QString& stage) {
            if (injected || stage != QStringLiteral("超分处理")) return;
            const auto directories = QDir(options.tempPath).entryList({QStringLiteral("render-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
            if (directories.isEmpty()) return;
            QFile conflict(QDir(QDir(options.tempPath).filePath(directories.first())).filePath(QStringLiteral("segment-000000.mp4")));
            injected = conflict.open(QIODevice::WriteOnly); conflict.write("encoder conflict");
        }, Qt::DirectConnection);
        pool.start(worker); QVERIFY(failedFinished.wait(30000)); pool.waitForDone();
        QVERIFY(injected); QVERIFY(!failedFinished.first().at(0).toBool()); QVERIFY(!failedFinished.first().at(1).toBool());
        QVERIFY2(failedFinished.first().at(3).toString().contains(QStringLiteral("编码")), qPrintable(failedFinished.first().at(3).toString()));
        QVERIFY(!QFileInfo::exists(failed->outputPath));
        QVERIFY(QDir(options.tempPath).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

        // Existing files survive failed retries and output collisions.
        worker = new SuperresWorker(task); QSignalSpy collision(worker, &SuperresWorker::completed); pool.start(worker);
        QVERIFY(collision.wait(30000)); pool.waitForDone(); QVERIFY(!collision.at(0).at(0).toBool());
        QCOMPARE(inspect(options, task->outputPath).frames, original.frames);
    }
    void identicalFramesReuseAcrossBatches() {
        if (!qEnvironmentVariableIsSet("FKW_SUPERRES_INTEGRATION")) QSKIP("Requires the Vulkan engine");
        auto settings = SuperresOptions::fromConfig();
        settings.sizeMode = QStringLiteral("scale"); settings.scale = 2;
        settings.model = QStringLiteral("realesr-animevideov3"); settings.audio = QStringLiteral("aac");
        settings.tile = 32; settings.batchFrames = 5;
        QTemporaryDir fixture;
        settings.tempPath = fixture.filePath(QStringLiteral("重复帧缓存"));
        const QString images = fixture.filePath(QStringLiteral("frames")); QDir().mkpath(images);
        const QList<int> pattern{0,0,0,1,1,1,0,0,0,2,2,2,1,1,1,0,3,0};
        const QList<QColor> colors{QColor(180,20,40), QColor(20,50,180), QColor(40,160,35)};
        for (int index = 0; index < pattern.size(); ++index) {
            QImage image(64, 36, QImage::Format_RGB888);
            image.fill(colors.at(pattern.at(index) == 3 ? 0 : pattern.at(index)));
            // A single-channel difference of one in a single pixel must still
            // be inferred independently, rather than merged into the red hold.
            if (pattern.at(index) == 3) image.setPixelColor(32, 18, QColor(181,20,40));
            QVERIFY(image.save(QDir(images).filePath(QStringLiteral("%1.png").arg(index, 8, 10, QLatin1Char('0')))));
        }
        const QString source = fixture.filePath(QStringLiteral("定格 & 重复.mkv"));
        run(settings.ffmpegPath, {"-hide_banner", "-loglevel", "error", "-framerate", "30000/1001", "-i", QDir(images).filePath("%08d.png"),
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-frames:v", "18", "-t", "0.6006", "-c:v", "ffv1", "-level", "3", "-pix_fmt", "bgr0", "-c:a", "pcm_s16le", source});
        QVERIFY(QFileInfo::exists(source));
        const auto original = inspect(settings, source);
        QVERIFY(!original.variableFrameRate); QCOMPARE(original.frameRate, QStringLiteral("30000/1001"));
        QStringList outputs;
        QThreadPool pool;
        for (bool enabled : {true, false}) {
            settings.reuseIdenticalFrames = enabled;
            auto task = std::make_shared<SuperresTask>(source, fixture.filePath(enabled ? "reused.mp4" : "baseline.mp4"), settings);
            auto* worker = new SuperresWorker(task);
            QSignalSpy completed(worker, &SuperresWorker::completed);
            QSignalSpy statistics(worker, &SuperresWorker::frameReuseProgress);
            QSignalSpy progress(worker, &SuperresWorker::progress);
            QSignalSpy taskUpdates(&GlobalEventBus::instance(), &GlobalEventBus::updateTaskStatusSig);
            pool.start(worker); QVERIFY(completed.wait(45000)); pool.waitForDone();
            QVERIFY2(completed.first().at(0).toBool(), qPrintable(completed.first().at(3).toString()));
            QVERIFY(!statistics.isEmpty());
            const auto stats = statistics.last();
            QCOMPARE(stats.at(0).toLongLong(), enabled ? 4 : 18);
            QCOMPARE(stats.at(1).toLongLong(), enabled ? 14 : 0);
            QCOMPARE(stats.at(2).toLongLong(), enabled ? 4 : 18);
            QCOMPARE(stats.at(3).toLongLong(), 18);
            // Progress, speed and ETA describe model work, even when many
            // source occurrences reuse one result or an entire batch is reused.
            const qint64 expected = enabled ? 4 : 18;
            qint64 previous = 0;
            bool sawHalf = false, sawEstimate = false, sawEncoding = false, estimatorReady = false;
            double renderStart = -1;
            for (const auto& point : progress) {
                const auto done = point.at(2).toLongLong(), count = point.at(3).toLongLong();
                if (!count) continue; // Probe and signature analysis precede the fixed plan.
                if (renderStart < 0) renderStart = point.at(5).toDouble();
                QCOMPARE(count, expected); QVERIFY(done >= previous); QVERIFY(done <= count);
                previous = done;
                const int percent = point.at(0).toInt();
                QCOMPARE(percent, point.at(1).toString() == QStringLiteral("完成") ? 100 : qMin(99, int(done * 100 / expected)));
                sawHalf |= enabled && done == 2 && percent == 50;
                const double speed = point.at(4).toDouble(), remaining = point.at(6).toDouble();
                estimatorReady |= done > 0 && done < count && point.at(5).toDouble() - renderStart >= 1.0;
                if (done < count && speed > 0) {
                    QVERIFY(std::abs(remaining * speed - (count - done)) < .0001); sawEstimate = true;
                }
                if (point.at(1).toString() == QStringLiteral("编码收尾")) {
                    QCOMPARE(done, expected); QVERIFY(remaining < 0); QVERIFY(percent < 100); sawEncoding = true;
                }
            }
            QVERIFY(!enabled || sawHalf); QVERIFY(!estimatorReady || sawEstimate); QVERIFY(sawEncoding);
            QVERIFY(!taskUpdates.isEmpty());
            bool sawTaskHalf = false, sawTaskFinishing = false;
            for (const auto& update : taskUpdates) {
                if (update.at(0).toInt() != task->taskId) continue;
                sawTaskHalf |= enabled && update.at(1).toInt() == 50 &&
                    update.at(3).toString() == QStringLiteral("已处理 2/4 · 剩余 2 帧");
                sawTaskFinishing |= update.at(5).toString() == QStringLiteral("收尾中");
            }
            QVERIFY(!enabled || sawTaskHalf); QVERIFY(sawTaskFinishing);
            const auto output = inspect(settings, task->outputPath);
            QCOMPARE(output.size, QSize(128, 72)); QCOMPARE(output.frames, 18);
            QCOMPARE(output.frameRate, original.frameRate); QVERIFY(output.hasAudio);
            QVERIFY(std::abs(output.duration - 18.0 * 1001 / 30000) < .002);
            QVERIFY(QDir(settings.tempPath).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
            QFile log(task->logPath); QVERIFY(log.open(QIODevice::ReadOnly));
            const auto contents = log.readAll();
            QCOMPARE(contents.count("[inferred]"), enabled ? 3 : 4);
            if (contents.contains("[engine-session-start]")) QCOMPARE(contents.count("[engine-session-start]"), 1);
            else QCOMPARE(contents.count("realesrgan-ncnn-vulkan.exe -i"), enabled ? 3 : 4);
            outputs.append(task->outputPath);
        }
        // Reuse must produce the same complete decoded sequence as processing
        // every occurrence, including holds, later returns, and the one-pixel edit.
        const auto reused = run(settings.ffmpegPath, {"-v", "error", "-i", outputs.at(0), "-map", "0:v:0", "-pix_fmt", "rgb24", "-f", "framemd5", "-"});
        const auto baseline = run(settings.ffmpegPath, {"-v", "error", "-i", outputs.at(1), "-map", "0:v:0", "-pix_fmt", "rgb24", "-f", "framemd5", "-"});
        QVERIFY(!reused.isEmpty()); QCOMPARE(reused, baseline);
    }
    void mainPagePreviewAndTaskSnapshot() {
        if (!qEnvironmentVariableIsSet("FKW_SUPERRES_INTEGRATION")) QSKIP("Requires the Vulkan engine");
        auto settings = SuperresOptions::fromConfig();
        QTemporaryDir fixture;
        const QString source = fixture.filePath(QStringLiteral("MMD 示例.mp4"));
        run(settings.ffmpegPath, {"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=160x90:rate=6", "-vf", "fps=30", "-frames:v", "12", "-c:v", "ffv1", "-pix_fmt", "yuv420p", "-f", "matroska", source});
        QVERIFY(QFileInfo::exists(source));
        SuperresStackedInterfaces pages;
        pages.resize(960, 800); pages.show();
        auto* main = pages.findChild<SuperresInterface*>();
        auto* tasks = pages.findChild<SuperresTaskInterface*>();
        auto* mode = main->findChild<qfw::ComboBox*>(QStringLiteral("superresSizeMode"));
        auto* model = main->findChild<qfw::ComboBox*>(QStringLiteral("superresModel"));
        auto* button = main->findChild<qfw::PushButton*>(QStringLiteral("superresPreviewButton"));
        auto* bar = main->findChild<qfw::ProgressBar*>(QStringLiteral("superresPreviewProgress"));
        auto* status = main->findChild<qfw::BodyLabel*>(QStringLiteral("superresPreviewStatus"));
        auto* seek = main->findChild<qfw::Slider*>(QStringLiteral("superresSourceSlider"));
        auto* canvas = main->findChild<PreviewCanvas*>(QStringLiteral("superresSourcePreview"));
        auto* frames = main->findChild<VideoFrameService*>();
        auto* reuse = main->findChild<qfw::SwitchSettingCard*>(QStringLiteral("superresReuseFrames"));
        QVERIFY(reuse && reuse->isChecked());
        QVERIFY(main && tasks && mode && model && button && bar && status && seek && canvas && frames);
        QSignalSpy displayed(frames, &VideoFrameService::frameReady);
        mode->setCurrentIndex(1); model->setCurrentIndex(0);
        main->setInputPath(source);
        QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 15000);
        QCOMPARE(main->options().targetSize({160, 90}), QSize(320, 180));
        QCOMPARE(seek->maximum(), 11); QVERIFY(canvas->hasFrame());
        const auto firstFrame = canvas->grab().toImage();
        seek->setSliderDown(true);
        seek->setValue(1); QTest::qWait(20);
        seek->setValue(9); QTest::qWait(20);
        seek->setValue(5); seek->setSliderDown(false);
        QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 15000);
        QCOMPARE(displayed.last().at(0).toInt(), 5);
        QCOMPARE(main->previewStartSeconds(), 5.0 / 30);
        QVERIFY(canvas->grab().toImage() != firstFrame);
        const QString captures = qEnvironmentVariable("FKW_SUPERRES_CAPTURE_DIR");
        if (!captures.isEmpty()) { QDir().mkpath(captures); pages.grab().save(QDir(captures).filePath(QStringLiteral("main.png"))); }
        if (!captures.isEmpty()) {
            main->verticalScrollBar()->setValue(main->verticalScrollBar()->maximum());
            pages.grab().save(QDir(captures).filePath(QStringLiteral("source-selector.png")));
        }
        QString renderedPreview;
        connect(main, &SuperresInterface::previewRequested, &pages, [&](SuperresWorker* worker) {
            connect(worker, &SuperresWorker::completed, &pages, [&](bool success, bool, const QString& output, const QString&) {
                if (success) renderedPreview = output;
            });
        });
        bool sawRender = false, sawCompletion = false;
        QTimer observer;
        observer.setInterval(50);
        connect(&observer, &QTimer::timeout, &pages, [&]() {
            if (status->text().contains(QStringLiteral("超分处理")) && !status->text().contains(QStringLiteral("超分已完成"))) {
                sawRender = true;
                if (!captures.isEmpty()) { main->verticalScrollBar()->setValue(main->verticalScrollBar()->maximum()); pages.grab().save(QDir(captures).filePath(QStringLiteral("progress.png"))); }
            }
            for (auto* widget : QApplication::topLevelWidgets()) {
                if (auto* dialog = qobject_cast<QDialog*>(widget); dialog && dialog->windowTitle() == QStringLiteral("视频超分预览")) {
                    sawCompletion = true;
                    if (!captures.isEmpty()) dialog->grab().save(QDir(captures).filePath(QStringLiteral("comparison.png")));
                    dialog->accept();
                }
            }
        });
        observer.start(); button->click();
        QTRY_VERIFY_WITH_TIMEOUT(sawCompletion || status->text().startsWith(QStringLiteral("预览失败")), 120000);
        QVERIFY2(sawCompletion, qPrintable(status->text())); QVERIFY(sawRender); QCOMPARE(bar->value(), 100);
        const auto reuseText = main->findChild<qfw::CaptionLabel*>(QStringLiteral("superresPreviewReuseInfo"))->text();
        QVERIFY(reuseText.contains(QStringLiteral("原始 7 帧")));
        QVERIFY(reuseText.contains(QStringLiteral("去重后需超分 2 帧")));
        QVERIFY(reuseText.contains(QStringLiteral("可复用 5 帧")));
        QTRY_VERIFY_WITH_TIMEOUT(!renderedPreview.isEmpty(), 5000);
        // Starting at the displayed sixth frame of this 12-frame video leaves
        // exactly seven frames. A separate seconds field/default zero would fail.
        QCOMPARE(inspect(settings, renderedPreview).frames, 7);
        observer.stop();
        auto* add = main->findChild<qfw::PrimaryPushButton*>(QStringLiteral("tutorial-start"));
        QVERIFY(add); add->click();
        auto* card = tasks->findChild<TaskCard*>();
        QVERIFY(card);
        const auto submitted = std::static_pointer_cast<SuperresTask>(card->task());
        model->setCurrentIndex(1);
        reuse->setChecked(false);
        QCOMPARE(submitted->options.model, QStringLiteral("realesr-animevideov3"));
        QVERIFY(submitted->options.reuseIdenticalFrames); QVERIFY(!main->options().reuseIdenticalFrames);
        QTRY_COMPARE_WITH_TIMEOUT(card->status(), TaskStatus::Processing, 5000);
        auto* taskFrames = card->findChild<qfw::CaptionLabel*>(QStringLiteral("superresTaskFrames"));
        auto* taskSpeed = card->findChild<qfw::CaptionLabel*>(QStringLiteral("superresTaskSpeed"));
        QVERIFY(taskFrames && taskSpeed);
        QTRY_VERIFY_WITH_TIMEOUT(taskFrames->text().contains(QStringLiteral("/3")), 5000);
        QVERIFY(taskFrames->text().contains(QStringLiteral("剩余")));
        QVERIFY(taskSpeed->toolTip().contains(QStringLiteral("不包含复用帧")));
        if (!captures.isEmpty()) { pages.showPage(1); pages.grab().save(QDir(captures).filePath(QStringLiteral("tasks-progress.png"))); }
        auto queuedTask = std::make_shared<SuperresTask>(source, fixture.filePath(QStringLiteral("queued-preview.mp4")), submitted->options);
        queuedTask->preview = true;
        auto* queued = new SuperresWorker(queuedTask);
        QSignalSpy queuedFinished(queued, &SuperresWorker::completed);
        tasks->enqueuePreview(queued); tasks->cancelPreview(queued);
        QCOMPARE(queuedFinished.size(), 1); QVERIFY(queuedFinished.first().at(1).toBool());
        QVERIFY(!QFileInfo::exists(queuedTask->outputPath));
        QTRY_COMPARE_WITH_TIMEOUT(card->status(), TaskStatus::Succeeded, 120000);
        QVERIFY(QFileInfo::exists(submitted->outputPath));
        pages.showPage(1);
        if (!captures.isEmpty()) pages.grab().save(QDir(captures).filePath(QStringLiteral("tasks.png")));
        pages.showPage(2);
        if (!captures.isEmpty()) pages.grab().save(QDir(captures).filePath(QStringLiteral("advanced.png")));
        pages.showPage(0);
        const QString replacement = fixture.filePath(QStringLiteral("另一个视频.mp4"));
        QVERIFY(QFile::copy(source, replacement));
        seek->setValue(9);
        main->setInputPath(replacement);
        main->setInputPath(source);
        QCOMPARE(seek->value(), 0); QVERIFY(!canvas->hasFrame()); QVERIFY(!button->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 15000);
        QCOMPARE(displayed.last().at(0).toInt(), 0); QCOMPARE(main->previewStartSeconds(), 0.0);
    }
    void compareScanWithFfmpeg() {
        const QString source = qEnvironmentVariable("FKW_SUPERRES_SCAN_VIDEO");
        if (source.isEmpty()) QSKIP("Optional whole-video scan comparison");
        const auto options = SuperresOptions::fromConfig();
        const auto info = inspect(options, source);
        QVERIFY(info.exactFrames);
        const auto size = info.decodedSize;
        const auto bytes = qsizetype(size.width()) * size.height() * 3;
        QElapsedTimer timer; timer.start();
        QProcess reference;
        reference.start(options.ffmpegPath, {"-hide_banner", "-loglevel", "error", "-nostdin", "-i", source,
            "-map", "0:v:0", "-frames:v", QString::number(info.frames), "-fps_mode", "passthrough",
            "-vf", QStringLiteral("scale=%1:%2,setsar=1").arg(size.width()).arg(size.height()),
            "-pix_fmt", "rgb24", "-f", "framehash", "-hash", "sha256", "pipe:1"});
        QVERIFY(reference.waitForFinished(1200000));
        QCOMPARE(reference.exitCode(), 0);
        qInfo() << "FFmpeg scan ms" << timer.elapsed();
        QVector<QByteArray> expected;
        for (const auto& line : reference.readAllStandardOutput().split('\n')) {
            if (line.isEmpty() || line.startsWith('#')) continue;
            expected.append(QByteArray::fromHex(line.mid(line.lastIndexOf(',') + 1).trimmed()));
        }
        QCOMPARE(expected.size(), info.frames);
        const auto args = videoRgbDecodeArguments(source, size, info.fps, 0, info.frames);
        QVector<QByteArray> hashes(info.frames);
        std::atomic<bool> cancelled{false}; QString error;
        timer.restart();
        VideoRawDecoder decoder(options.ffmpegPath, args, bytes, info.frames, [&] { return cancelled.load(); });
        QVERIFY2(analyzeVideoFrames(decoder, size, hashes, cancelled, {}, [](qsizetype, bool) {}, error), qPrintable(error));
        QVERIFY2(decoder.finish(error), qPrintable(error));
        qInfo() << "parallel scan ms" << timer.elapsed() << "frames" << hashes.size();
        QVERIFY(hashes == expected);

        // Measure the second-pass verification cost on identical decoded pixels.
        const auto count = qMin<qint64>(240, info.frames);
        VideoRawDecoder sample(options.ffmpegPath, videoRgbDecodeArguments(source, size, info.fps, 0, count),
            bytes, count, [] { return false; });
        qint64 qtNs = 0, nativeNs = 0;
        for (qint64 i = 0; i < count; ++i) {
            QByteArray pixels; QVERIFY2(sample.read(pixels, error), qPrintable(error));
            for (int pass = 0; pass < 2; ++pass) {
                const bool native = (i + pass) % 2;
                timer.restart();
                const auto digest = native ? videoFrameHash(pixels) : QCryptographicHash::hash(pixels, QCryptographicHash::Sha256);
                (native ? nativeNs : qtNs) += timer.nsecsElapsed();
                QCOMPARE(digest, expected.at(i));
            }
        }
        QVERIFY2(sample.finish(error), qPrintable(error));
        qInfo() << "verify frames" << count << "old_ms" << qtNs / 1000000 << "new_ms" << nativeNs / 1000000;
    }
    void benchmarkMmdSample() {
        const QString source = qEnvironmentVariable("FKW_SUPERRES_BENCHMARK_VIDEO");
        if (source.isEmpty()) QSKIP("Optional representative MMD benchmark");
        const QString outputDirectory = qEnvironmentVariable("FKW_SUPERRES_BENCHMARK_DIR");
        QVERIFY(!outputDirectory.isEmpty()); QDir().mkpath(outputDirectory);
        const QString label = qEnvironmentVariable("FKW_SUPERRES_BENCHMARK_LABEL", "optimized");
        auto options = SuperresOptions::fromConfig();
        options.enginePath = qEnvironmentVariable("FKW_SUPERRES_BENCHMARK_ENGINE", options.enginePath);
        options.sizeMode = QStringLiteral("preset"); options.presetHeight = 1080;
        options.model = qEnvironmentVariable("FKW_SUPERRES_BENCHMARK_MODEL", options.model);
        options.batchFrames = 16;
        options.reuseIdenticalFrames = true;
        options.tempPath = QDir(outputDirectory).filePath(label + QStringLiteral("-cache"));
        auto task = std::make_shared<SuperresTask>(source, QDir(outputDirectory).filePath(label + QStringLiteral(".mp4")), options);
        task->preview = true; task->previewDuration = 5;
        task->previewStart = qEnvironmentVariable("FKW_SUPERRES_BENCHMARK_START").toDouble();
        const int requestedFrames = qEnvironmentVariableIntValue("FKW_SUPERRES_BENCHMARK_FRAMES");
        if (requestedFrames > 0) {
            const auto input = inspect(options, source);
            QVERIFY(input.fps > 0);
            task->previewDuration = requestedFrames / input.fps;
        }
        auto* worker = new SuperresWorker(task);
        QSignalSpy completed(worker, &SuperresWorker::completed);
        QElapsedTimer wall; wall.start();
        QThreadPool pool; pool.start(worker);
        QVERIFY(completed.wait(180000)); pool.waitForDone();
        const qint64 elapsed = wall.elapsed();
        QVERIFY2(completed.first().at(0).toBool(), qPrintable(completed.first().at(3).toString()));
        const auto video = inspect(options, task->outputPath);
        if (requestedFrames > 0) QCOMPARE(video.frames, qint64(requestedFrames));
        QJsonObject result{{"label", label}, {"elapsed_ms", elapsed}, {"frames", video.frames},
            {"fps", video.fps}, {"width", video.size.width()}, {"height", video.size.height()},
            {"output", task->outputPath}, {"log", task->logPath}};
        QFile report(QDir(outputDirectory).filePath(label + QStringLiteral(".json")));
        QVERIFY(report.open(QIODevice::WriteOnly)); report.write(QJsonDocument(result).toJson());
        QVERIFY(QFile::copy(task->logPath, QDir(outputDirectory).filePath(label + QStringLiteral(".log"))));
    }
};
QTEST_MAIN(SuperresTests)
#include "superres_tests.moc"
