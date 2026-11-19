#include <QApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QProcess>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QtTest>
#include "service/interpolation_service.h"
#include "service/video_gpu_gate.h"
#include "service/video_preview.h"
#include "service/interpolation_pipeline.h"
#include "service/video_frame_analysis.h"
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif
#include "view/interpolation_interface.h"

using namespace fkw;
namespace {
QByteArray command(const QString& program, const QStringList& args) {
    QProcess process; process.start(program, args);
    if (!process.waitForFinished(20000) || process.exitCode() != 0) { qWarning().noquote() << process.readAllStandardError(); return {}; }
    return process.readAllStandardOutput();
}
SuperresVideoInfo inspect(const InterpolationOptions& options, const QString& path) {
    return parseSuperresVideoInfo(QJsonDocument::fromJson(command(options.ffprobePath, superresProbeArguments(path))).object());
}
QString source(const InterpolationOptions& options, QTemporaryDir& directory, bool cut = false) {
    const QString images = directory.filePath("images"); QDir().mkpath(images);
    const QList<int> pattern = cut ? QList<int>{0,0,1,1} : QList<int>{0,0,1,2,2,3,0,0};
    for (int index = 0; index < pattern.size(); ++index) {
        QImage image(96, 64, QImage::Format_RGB888);
        image.fill(cut ? (pattern.at(index) == 0 ? QColor(190,20,30) : QColor(20,30,190)) : QColor(20,20,30));
        if (!cut) { QPainter paint(&image); paint.fillRect(4 + pattern.at(index) * 12, 20, 18, 18, QColor(200,50,80)); }
        if (!image.save(QDir(images).filePath(QStringLiteral("%1.png").arg(index, 8, 10, QLatin1Char('0'))))) return {};
    }
    const QString path = directory.filePath(QStringLiteral("东方 MMD & 测试.mkv"));
    command(options.ffmpegPath, {"-hide_banner", "-loglevel", "error", "-framerate", "24", "-i", QDir(images).filePath("%08d.png"),
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000", "-frames:v", QString::number(pattern.size()),
        "-t", QString::number(pattern.size() / 24.0, 'f', 9), "-c:v", "ffv1", "-pix_fmt", "bgr0", "-c:a", "pcm_s16le", path});
    return QFileInfo::exists(path) ? path : QString();
}
struct Run { bool success = false, cancelled = false; QString error, log; QList<QList<QVariant>> progress, stats; };
Run render(const std::shared_ptr<InterpolationTask>& task, const std::function<void(InterpolationWorker*, const QString&)>& hook = {}) {
    auto* worker = new InterpolationWorker(task); QSignalSpy finished(worker, &InterpolationWorker::completed);
    QSignalSpy progress(worker, &InterpolationWorker::progress), stats(worker, &InterpolationWorker::statistics);
    if (hook) QObject::connect(worker, &InterpolationWorker::progress, worker, [worker, hook](int, const QString& stage) { hook(worker, stage); }, Qt::DirectConnection);
    QThreadPool pool; pool.start(worker); const bool completed = finished.wait(45000);
    if (!completed) worker->cancel(); pool.waitForDone();
    Run result; result.progress = progress; result.stats = stats;
    if (completed) { result.success = finished.first().at(0).toBool(); result.cancelled = finished.first().at(1).toBool(); result.error = finished.first().at(3).toString(); }
    else result.error = "Timed out";
    QFile log(task->logPath); if (log.open(QIODevice::ReadOnly)) result.log = QString::fromUtf8(log.readAll());
    return result;
}
}
class InterpolationTests : public QObject {
    Q_OBJECT
    QTemporaryDir localData_;
private slots:
    void initTestCase() {
        Q_INIT_RESOURCE(resource); QStandardPaths::setTestModeEnabled(true); qputenv("LOCALAPPDATA", localData_.path().toUtf8());
        QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc"); qApp->setFont(QFont("Microsoft YaHei", 10));
    }
    void exactTimelineAndProtectionPlan() {
        const auto metadata = parseInterpolationVideoInfo(QJsonDocument::fromJson(R"({"streams":[{"codec_type":"video","width":854,"height":474,"avg_frame_rate":"8089000/269633","r_frame_rate":"30/1"}]})").object());
        QCOMPARE(metadata.frameRate, QString("30/1")); QCOMPARE(metadata.fps, 30.0);
        InterpolationOptions settings; settings.rateMode = "multiply";
        QCOMPARE(settings.targetRate("30000/1001"), QString("60000/1001"));
        settings.rateMode = "custom"; settings.customRate = 60; QCOMPARE(settings.targetRate("24/1"), QString("60/1"));
        QVector<QByteArray> hashes; for (int i = 0; i < 13; ++i) hashes.append(QByteArray::number(i));
        hashes[1] = hashes[0]; QVector<bool> cuts(13, false); cuts[2] = true;
        const auto plan = planInterpolation(12, "24/1", "60/1", hashes, cuts, settings);
        QCOMPARE(plan.frames.size(), 30); QCOMPARE(plan.holds, 2); QCOMPARE(plan.cuts, 2); QCOMPARE(plan.inference, 20);
        QCOMPARE(plan.frames.at(1).left, 0); QVERIFY(std::abs(plan.frames.at(1).time - .4) < 1e-9);
        QCOMPARE(plan.frames.at(3).left, 1); QVERIFY(std::abs(plan.frames.at(3).time - .2) < 1e-9);
        QCOMPARE(plan.frames.at(5).left, 2); QCOMPARE(plan.frames.at(5).kind, InterpolationFrameKind::Original);
        QCOMPARE(plan.frames.last().right, 12); // A clipped preview still sees its following source frame.
        const auto fractional = planInterpolation(12, "30000/1001", "60/1", hashes, cuts, settings);
        QCOMPARE(fractional.frames.size(), 25);
    }
    void originalFramesOnRoundedCfrGrid() {
        QJsonObject stream{{"codec_type", "video"}, {"width", 1920}, {"height", 1080},
            {"r_frame_rate", "909753/30325"}, {"avg_frame_rate", "909753/30325"}, {"nb_frames", "6065"}};
        const auto probe = [&] { return parseInterpolationVideoInfo(QJsonObject{{"streams", QJsonArray{stream}}}); };
        const auto info = probe();
        QCOMPARE(info.frameRate, QString("30/1"));
        const auto plan = planInterpolation(info.frames, info.frameRate, "60/1",
            QVector<QByteArray>(info.frames), QVector<bool>(info.frames, false), {});
        QCOMPARE(plan.frames.size(), 12130);
        QCOMPARE(plan.inference, 6064);
        for (int i = 0; i < info.frames; ++i) {
            const auto& original = plan.frames.at(2 * i);
            QCOMPARE(original.kind, InterpolationFrameKind::Original);
            QCOMPARE(original.left, i);
            const auto& next = plan.frames.at(2 * i + 1);
            QCOMPARE(next.left, i);
            if (i + 1 < info.frames) {
                QCOMPARE(next.kind, InterpolationFrameKind::Infer);
                QCOMPARE(next.right, i + 1);
                QCOMPARE(next.time, .5);
            } else {
                QCOMPARE(next.kind, InterpolationFrameKind::Original);
            }
        }
        stream["nb_frames"] = "606500";
        QCOMPARE(probe().frameRate, QString("909753/30325"));
        stream["r_frame_rate"] = "30000/1001";
        stream["avg_frame_rate"] = "30000/1001";
        QCOMPARE(probe().frameRate, QString("30000/1001"));
        stream["r_frame_rate"] = "30/1";
        QVERIFY(probe().variableFrameRate);
    }
    void cameraMotionAndHardCuts() {
        const QSize size(160, 96);
        QByteArray first(size.width() * size.height() * 3, Qt::Uninitialized);
        QByteArray moved = first, nextShot = first;
        QRandomGenerator random(7919);
        for (auto& channel : first) channel = char(random.bounded(256));
        for (auto& channel : nextShot) channel = char(random.bounded(256));
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                const int offset = (y * size.width() + x) * 3;
                const int from = (y * size.width() + (x + 13) % size.width()) * 3;
                for (int c = 0; c < 3; ++c) {
                    moved[offset + c] = first[from + c];
                    // A logo in one corner persists across a genuine edit.
                    if (x < 42 && y < 24) nextShot[offset + c] = first[offset + c];
                }
            }
        }
        QVERIFY(!interpolationSceneCut({}, first, size, 30));
        QVERIFY(!interpolationSceneCut(first, first, size, 30));
        QVERIFY(!interpolationSceneCut(first, moved, size, 30));
        QVERIFY(interpolationSceneCut(first, nextShot, size, 30));
        QVERIFY(interpolationSceneCut(QByteArray(first.size(), '\0'), QByteArray(first.size(), char(255)), size, 30));
    }
    void exactFrameHashes() {
        QByteArray pixels(1920 * 1080 * 3, Qt::Uninitialized);
        for (qsizetype i = 0; i < pixels.size(); ++i) pixels[i] = char(i * 37);
        for (int length : {0, 1, 55, 56, 63, 64, 65, 4097, int(pixels.size())}) {
            const auto buffer = QByteArray::fromRawData(pixels.constData(), length);
            QCOMPARE(videoFrameHash(buffer), QCryptographicHash::hash(buffer, QCryptographicHash::Sha256));
        }
    }
    void profileFrameAnalysis() {
        const QString input = qEnvironmentVariable("FKW_INTERPOLATION_ANALYSIS_INPUT");
        if (input.isEmpty()) QSKIP("Optional scan comparison on a supplied video");
        const auto options = InterpolationOptions::fromConfig();
        const auto info = inspect(options, input);
        const bool fullVideo = qEnvironmentVariableIsSet("FKW_INTERPOLATION_ANALYSIS_FULL");
        const qint64 first = fullVideo ? 0 : qRound64(29 * info.fps);
        const qint64 count = fullVideo ? info.frames : qMin<qint64>(qRound64(8 * info.fps), info.frames - first);
        QVERIFY(count > 0);
        std::atomic<bool> cancelled{false};
        QByteArray reference;
        const QList<bool> runs = fullVideo ? QList<bool>{false, true} : QList<bool>{false, true, true, false};
        for (const bool parallel : runs) {
            QElapsedTimer timer; timer.start();
            InterpolationDecoder decoder(input, options, info, first, count, cancelled);
            qint64 readNs = 0, hashNs = 0, cutNs = 0;
            QByteArray previous, decisions;
            int cuts = 0;
            if (parallel) {
                QVector<QByteArray> hashes(count);
                QVector<bool> detectedCuts(count);
                QString error;
                QVERIFY2(analyzeInterpolationFrames(decoder, info.size, options, hashes, detectedCuts, cancelled,
                    [](qint64) {}, error), qPrintable(error));
                qInfo() << "optimized frames" << count << "wall_ms" << timer.elapsed();
                for (qint64 i = 0; i < count; ++i) {
                    decisions += hashes[i]; decisions += char(detectedCuts[i]);
                }
                QCOMPARE(decisions, reference);
                continue;
            }
            for (qint64 i = 0; i < count; ++i) {
                QByteArray pixels; QString error;
                const auto start = timer.nsecsElapsed();
                QVERIFY2(decoder.read(pixels, error), qPrintable(error));
                const auto read = timer.nsecsElapsed();
                decisions += QCryptographicHash::hash(pixels, QCryptographicHash::Sha256);
                const auto hashed = timer.nsecsElapsed();
                const bool cut = interpolationSceneCut(previous, pixels, info.size, 30);
                decisions += char(cut);
                cuts += cut;
                readNs += read - start; hashNs += hashed - read; cutNs += timer.nsecsElapsed() - hashed;
                previous = std::move(pixels);
            }
            qInfo() << "baseline frames" << count << "cuts" << cuts
                    << "wall_ms" << timer.elapsed() << "read_ms" << readNs / 1000000
                    << "hash_ms" << hashNs / 1000000 << "cut_ms" << cutNs / 1000000;
            if (reference.isEmpty()) reference = decisions;
            else QCOMPARE(decisions, reference);
        }
    }
    void stopAnalysisWithPendingFrames() {
        const auto options = InterpolationOptions::fromConfig();
        QTemporaryDir fixture;
        const QString input = source(options, fixture);
        const auto info = inspect(options, input);
        QCOMPARE(info.frames, 8);
        for (bool cancel : {true, false}) {
            std::atomic<bool> cancelled{false};
            const qint64 count = cancel ? info.frames : info.frames + 1;
            InterpolationDecoder decoder(input, options, info, 0, count, cancelled);
            QVector<QByteArray> hashes(count);
            QVector<bool> cuts(count);
            QString error;
            int processed = 0;
            QVERIFY(!analyzeInterpolationFrames(decoder, info.size, options, hashes, cuts, cancelled, [&](qint64 done) {
                processed = int(done);
                if (cancel) cancelled.store(true);
            }, error));
            if (cancel) QCOMPARE(processed, 1);
            else QVERIFY(!error.isEmpty());
        }
    }
    void decoderBackpressureAndFailureStop() {
#ifdef Q_OS_WIN
        auto options = InterpolationOptions::fromConfig(); QTemporaryDir fixture;
        const QString input = fixture.filePath("long.mp4");
        command(options.ffmpegPath, {"-v", "error", "-f", "lavfi", "-i", "color=red:size=160x90:rate=30", "-frames:v", "4000", "-c:v", "libx264", "-preset", "ultrafast", input});
        const auto info = inspect(options, input); QCOMPARE(info.frames, 4000);
        const auto memory = [] { PROCESS_MEMORY_COUNTERS_EX value{}; value.cb = sizeof(value);
            return K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&value), sizeof(value)) ? quint64(value.PrivateUsage) : quint64(0); };
        const auto before = memory(); QVERIFY(before > 0);
        std::atomic<bool> cancelled{false}, stageFailed{false};
        auto decoder = std::make_unique<InterpolationDecoder>(input, options, info, 0, 4000, cancelled, [&] { return stageFailed.load(); });
        QByteArray pixels; QString error; QVERIFY2(decoder->read(pixels, error), qPrintable(error));
        QTest::qSleep(3000); const auto after = memory();
        stageFailed.store(true); QElapsedTimer shutdown; shutdown.start();
        QVERIFY(!decoder->read(pixels, error)); decoder.reset(); QVERIFY(shutdown.elapsed() < 5000);
        QVERIFY(after > 0); const auto growth = qint64(after) - qint64(before);
        qInfo() << "Stalled interpolation decoder memory growth:" << growth;
        QVERIFY(growth < 48 * 1024 * 1024);
#endif
    }
    void memoryWorkerMatchesPngPixels() {
        if (!qEnvironmentVariableIsSet("FKW_INTERPOLATION_INTEGRATION")) QSKIP("Requires Vulkan engine");
        auto options = InterpolationOptions::fromConfig(); QTemporaryDir fixture;
        auto left = frames::MemoryFrame::create(64, 36), right = frames::MemoryFrame::create(64, 36), result = frames::MemoryFrame::create(64, 36);
        QImage first(64,36,QImage::Format_RGB888), second(64,36,QImage::Format_RGB888);
        for (int y = 0; y < 36; ++y) for (int x = 0; x < 64; ++x) {
            first.setPixelColor(x,y,QColor((x*7+y)%256,(y*11+x)%256,(x*3+y*5)%256));
            second.setPixelColor(x,y,QColor((x*5+y*2)%256,(y*7+x*3)%256,(x*2+y*9)%256));
        }
        for (int y = 0; y < 36; ++y) { memcpy(left->data()+y*64*3,first.constScanLine(y),64*3); memcpy(right->data()+y*64*3,second.constScanLine(y),64*3); }
        left->publish(); right->publish();
        const QString pngLeft=fixture.filePath(QStringLiteral("左.png")), pngRight=fixture.filePath(QStringLiteral("右.png")), reference=fixture.filePath("reference.png");
        QVERIFY(first.save(pngLeft)); QVERIFY(second.save(pngRight));
        QFile jobs(fixture.filePath("jobs.tsv")); QVERIFY(jobs.open(QIODevice::WriteOnly));
        const QByteArray rows = "0.5\t"+pngLeft.toUtf8()+'\t'+pngRight.toUtf8()+'\t'+reference.toUtf8()+'\n'+
            "0.5\t"+QByteArray::fromStdString(left->uri())+'\t'+QByteArray::fromStdString(right->uri())+'\t'+QByteArray::fromStdString(result->uri())+'\n';
        QCOMPARE(jobs.write(rows), rows.size()); jobs.close();
        QProcess worker; worker.start(options.enginePath,{QDir(options.modelsPath).filePath(options.model),"-1","0","0","0"});
        QVERIFY(worker.waitForStarted(10000)); worker.write("BATCH\t"+jobs.fileName().toUtf8()+"\nQUIT\n"); worker.closeWriteChannel();
        QVERIFY(worker.waitForFinished(60000)); QVERIFY2(worker.exitCode()==0,worker.readAllStandardError().constData());
        QVERIFY(result->ready()); const QImage png=QImage(reference).convertToFormat(QImage::Format_RGB888); QVERIFY(!png.isNull());
        for(int y=0;y<36;++y) QCOMPARE(QByteArray(reinterpret_cast<const char*>(png.constScanLine(y)),64*3),
            QByteArray(reinterpret_cast<const char*>(result->data()+y*64*3),64*3));
    }
    void actualRenderAndLookahead() {
        if (!qEnvironmentVariableIsSet("FKW_INTERPOLATION_INTEGRATION")) QSKIP("Optional Vulkan test");
        auto options = InterpolationOptions::fromConfig(); QTemporaryDir fixture;
        options.tempPath = fixture.filePath("cache"); options.batchFrames = 2; options.audio = "aac"; options.protectCuts = false;
        const QString input = source(options, fixture); QVERIFY(!input.isEmpty());
        auto task = std::make_shared<InterpolationTask>(input, fixture.filePath("result.mp4"), options);
        const auto result = render(task); QVERIFY2(result.success, qPrintable(result.error + result.log.right(2000)));
        const auto video = inspect(options, task->outputPath);
        QCOMPARE(video.frames, 20); QCOMPARE(video.size, QSize(96,64)); QCOMPARE(video.frameRate, QString("60/1")); QVERIFY(video.hasAudio);
        QVERIFY(std::abs(video.duration - 8.0 / 24) < 1.0 / 60 + .002);
        QCOMPARE(result.log.count("[engine-start]"), 1); QVERIFY(result.log.count("[inferred]") >= 3); QVERIFY(result.log.contains("RIFE GPU"));
        QVERIFY(!result.stats.isEmpty()); const auto stats = result.stats.last();
        QCOMPARE(stats.at(0).toLongLong(), 8); QCOMPARE(stats.at(1).toLongLong(), 20); QVERIFY(stats.at(2).toLongLong() > 0); QVERIFY(stats.at(3).toLongLong() > 0);
        qint64 previous = 0; bool sawFinishing = false;
        for (const auto& point : result.progress) {
            const auto done = point.at(2).toLongLong(), total = point.at(3).toLongLong(); if (!total) continue;
            QCOMPARE(total, stats.at(2).toLongLong()); QVERIFY(done >= previous); QVERIFY(done <= total); previous = done;
            if (point.at(1).toString() == "编码收尾") { QCOMPARE(done, total); QVERIFY(point.at(6).toDouble() < 0); QVERIFY(point.at(0).toInt() < 100); sawFinishing = true; }
        }
        QVERIFY(sawFinishing); QCOMPARE(result.progress.last().at(0).toInt(), 100);
        QVERIFY(QDir(options.tempPath).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        auto preview = std::make_shared<InterpolationTask>(input, fixture.filePath("preview.mp4"), options);
        preview->preview = true; preview->previewStart = 3.0 / 24; preview->previewDuration = 2.0 / 24;
        const auto clip = render(preview); QVERIFY2(clip.success, qPrintable(clip.error + clip.log.right(1500)));
        QCOMPARE(inspect(options, preview->outputPath).frames, 5);
        QCOMPARE(clip.stats.last().at(2).toLongLong(), 2); QCOMPARE(clip.stats.last().at(3).toLongLong(), 2);
    }
    void cutsHoldsCancellationAndFailure() {
        if (!qEnvironmentVariableIsSet("FKW_INTERPOLATION_INTEGRATION")) QSKIP("Optional Vulkan test");
        auto options = InterpolationOptions::fromConfig(); QTemporaryDir fixture;
        options.tempPath = fixture.filePath("cache"); options.rateMode = "multiply"; options.batchFrames = 1; options.audio = "aac";
        const QString input = source(options, fixture, true); QVERIFY(!input.isEmpty());
        auto task = std::make_shared<InterpolationTask>(input, fixture.filePath("protected.mp4"), options);
        const auto result = render(task); QVERIFY2(result.success, qPrintable(result.error + result.log));
        QCOMPARE(result.stats.last().at(2).toLongLong(), 0); QCOMPARE(result.stats.last().at(3).toLongLong(), 2); QCOMPARE(result.stats.last().at(4).toLongLong(), 1);
        QCOMPARE(result.log.count("[engine-start]"), 0); QCOMPARE(inspect(options, task->outputPath).frames, 8);
        const auto pixels = command(options.ffmpegPath, {"-v", "error", "-i", task->outputPath, "-vf", "select=eq(n\\,3)", "-frames:v", "1", "-pix_fmt", "rgb24", "-f", "rawvideo", "pipe:1"});
        QVERIFY(pixels.size() >= 3); QVERIFY(uchar(pixels.at(0)) > 150); QVERIFY(uchar(pixels.at(2)) < 60);
        const auto collision = render(task); QVERIFY(!collision.success); QCOMPARE(inspect(options, task->outputPath).frames, 8);
        options.protectCuts = false;
        auto cancelled = std::make_shared<InterpolationTask>(input, fixture.filePath("cancelled.mp4"), options);
        const auto cancellation = render(cancelled, [](InterpolationWorker* worker, const QString& stage) { if (stage == "补帧处理") worker->cancel(); });
        QVERIFY(cancellation.cancelled); QVERIFY(!QFileInfo::exists(cancelled->outputPath));
        VideoGpuLease occupied; std::atomic<bool> noCancel{false}; QVERIFY(occupied.acquire(noCancel, []() {}));
        auto waiting = std::make_shared<InterpolationTask>(input, fixture.filePath("waiting.mp4"), options);
        const auto queued = render(waiting, [](InterpolationWorker* worker, const QString& stage) { if (stage == "等待显卡资源") worker->cancel(); });
        QVERIFY(queued.cancelled); QVERIFY(!QFileInfo::exists(waiting->outputPath));
        options.protectCuts = true; // No GPU needed while the test holds the lease.
        auto failed = std::make_shared<InterpolationTask>(input, fixture.filePath("failed.mp4"), options); bool injected = false;
        const auto failure = render(failed, [&](InterpolationWorker*, const QString& stage) {
            if (injected || stage != "复用原始画面") return;
            const auto roots = QDir(options.tempPath).entryList({"render-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
            if (roots.isEmpty()) return;
            QFile file(QDir(QDir(options.tempPath).filePath(roots.first())).filePath("batch-000000.mp4"));
            injected = file.open(QIODevice::WriteOnly); file.write("conflict");
        });
        QVERIFY(injected); QVERIFY(!failure.success); QVERIFY2(failure.error.contains("编码"), qPrintable(failure.error));
        QVERIFY(!QFileInfo::exists(failed->outputPath)); QVERIFY(QDir(options.tempPath).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }
    void pagePreviewAndTaskSnapshot() {
        if (!qEnvironmentVariableIsSet("FKW_INTERPOLATION_INTEGRATION")) QSKIP("Optional Vulkan test");
        auto settings = InterpolationOptions::fromConfig(); QTemporaryDir fixture; const QString input = source(settings, fixture); QVERIFY(!input.isEmpty());
        InterpolationStackedInterfaces pages; pages.resize(960,800); pages.show();
        auto* main = pages.findChild<InterpolationInterface*>(); auto* tasks = pages.findChild<InterpolationTaskInterface*>(); QVERIFY(main && tasks);
        auto* seek = main->findChild<qfw::Slider*>("interpolationSourceSlider");
        auto* button = main->findChild<qfw::PushButton*>("interpolationPreviewButton");
        auto* status = main->findChild<qfw::BodyLabel*>("interpolationPreviewStatus");
        auto* rate = main->findChild<qfw::ComboBox*>("interpolationRateMode");
        auto* model = main->findChild<qfw::ComboBox*>("interpolationModel");
        auto* hold = main->findChild<qfw::SwitchSettingCard*>("interpolationReuseHolds");
        QVERIFY(seek && button && status && rate && model && hold);
        main->setInputPath(input); QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 10000); QCOMPARE(seek->maximum(), 7);
        rate->setCurrentIndex(2); main->findChild<qfw::SpinBox*>("interpolationCustomRate")->setValue(60);
        seek->setValue(3); QTRY_VERIFY_WITH_TIMEOUT(button->isEnabled(), 10000); QCOMPARE(main->previewStartSeconds(), 3.0 / 24);
        const QString captures = qEnvironmentVariable("FKW_INTERPOLATION_CAPTURE_DIR"); if (!captures.isEmpty()) { QDir().mkpath(captures); pages.grab().save(QDir(captures).filePath("main.png")); }
        bool sawRender = false, sawResult = false, matchedTime = false; QString output;
        connect(main, &InterpolationInterface::previewRequested, &pages, [&](InterpolationWorker* worker) {
            connect(worker, &InterpolationWorker::completed, &pages, [&](bool success, bool, const QString& path, const QString&) { if (success) output = path; });
            connect(worker, &InterpolationWorker::progress, &pages, [&](int, const QString& stage) {
                if (stage != "补帧处理") return;
                sawRender = true;
                if (!captures.isEmpty()) { main->verticalScrollBar()->setValue(main->verticalScrollBar()->maximum()); pages.grab().save(QDir(captures).filePath("progress.png")); }
            });
        });
        QTimer observer; observer.setInterval(50);
        connect(&observer, &QTimer::timeout, &pages, [&]() {
            if (status->text().contains("补帧处理")) {
                sawRender = true;
                if (!captures.isEmpty()) { main->verticalScrollBar()->setValue(main->verticalScrollBar()->maximum()); pages.grab().save(QDir(captures).filePath("progress.png")); }
            }
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* dialog = qobject_cast<QDialog*>(widget); if (!dialog || dialog->windowTitle() != "视频补帧预览") continue;
                auto* slider = dialog->findChild<qfw::Slider*>("interpolationComparisonSlider");
                auto* before = dialog->findChild<QLabel*>("interpolationComparisonSource");
                if (slider && before) { slider->setValue(3); matchedTime = before->property("frameIndex").toLongLong() == 4; }
                sawResult = true; if (!captures.isEmpty()) dialog->grab().save(QDir(captures).filePath("comparison.png")); dialog->accept();
            }
        });
        observer.start(); button->click(); QTRY_VERIFY_WITH_TIMEOUT(sawResult || status->text().startsWith("预览失败"), 30000);
        QVERIFY2(sawResult, qPrintable(status->text())); QVERIFY(sawRender); QVERIFY(matchedTime);
        QTRY_VERIFY_WITH_TIMEOUT(!output.isEmpty(), 5000); QCOMPARE(inspect(settings, output).frames, 13); observer.stop();
        auto* add = main->findChild<qfw::PrimaryPushButton*>("tutorial-start"); QVERIFY(add); add->click();
        auto* card = tasks->findChild<TaskCard*>(); QVERIFY(card); auto submitted = std::static_pointer_cast<InterpolationTask>(card->task());
        model->setCurrentIndex(1); hold->setChecked(false); QCOMPARE(submitted->options.model, QString("rife-v4.6")); QVERIFY(submitted->options.reuseHolds);
        QTRY_COMPARE_WITH_TIMEOUT(card->status(), TaskStatus::Processing, 5000);
        auto* frames = card->findChild<qfw::CaptionLabel*>("interpolationTaskFrames"); QVERIFY(frames); QTRY_VERIFY_WITH_TIMEOUT(frames->text().contains('/'), 5000);
        pages.showPage(1); if (!captures.isEmpty()) pages.grab().save(QDir(captures).filePath("tasks-progress.png"));
        auto queuedTask = std::make_shared<InterpolationTask>(input, fixture.filePath("queued.mp4"), submitted->options);
        queuedTask->preview = true;
        auto* queuedWorker = new InterpolationWorker(queuedTask); QSignalSpy cancelled(queuedWorker, &InterpolationWorker::completed);
        tasks->enqueuePreview(queuedWorker); tasks->cancelPreview(queuedWorker); QCOMPARE(cancelled.size(), 1); QVERIFY(cancelled.first().at(1).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(card->status(), TaskStatus::Succeeded, 30000);
        if (!captures.isEmpty()) pages.grab().save(QDir(captures).filePath("tasks.png"));
        pages.showPage(2); if (!captures.isEmpty()) pages.grab().save(QDir(captures).filePath("advanced.png"));
    }
    void mmdShortPreview() {
        const QString input = qEnvironmentVariable("FKW_INTERPOLATION_MMD"); if (input.isEmpty()) QSKIP("Optional 2-second MMD sample");
        auto settings = InterpolationOptions::fromConfig();
        settings.enginePath = qEnvironmentVariable("FKW_INTERPOLATION_BENCHMARK_ENGINE", settings.enginePath);
        const QString folder = qEnvironmentVariable("FKW_INTERPOLATION_OUTPUT"); QVERIFY(!folder.isEmpty()); QDir().mkpath(folder); settings.tempPath = QDir(folder).filePath("cache");
        auto task = std::make_shared<InterpolationTask>(input, QDir(folder).filePath("mmd-60fps.mp4"), settings);
        task->preview = true; task->previewStart = qEnvironmentVariable("FKW_INTERPOLATION_SAMPLE_START", "442.73").toDouble(); task->previewDuration = 2;
        QElapsedTimer timer; timer.start(); const auto result = render(task); QVERIFY2(result.success, qPrintable(result.error + result.log.right(2000)));
        QFile log(QDir(folder).filePath("mmd-run.log")); QVERIFY(log.open(QIODevice::WriteOnly)); log.write(result.log.toUtf8());
        QFile benchmark(QDir(folder).filePath("benchmark.json")); QVERIFY(benchmark.open(QIODevice::WriteOnly));
        benchmark.write(QJsonDocument(QJsonObject{{"elapsed_ms",timer.elapsed()},{"output",task->outputPath}}).toJson());
        const auto video = inspect(settings, task->outputPath); QCOMPARE(video.frameRate, QString("60/1")); QVERIFY(video.hasAudio); QVERIFY(timer.elapsed() < 55000);
    }
};
QTEST_MAIN(InterpolationTests)
#include "interpolation_tests.moc"
