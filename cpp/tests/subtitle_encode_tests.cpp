#include "service/subtitle_encode.h"
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using namespace fkw;
class SubtitleEncodeTests : public QObject {
    Q_OBJECT
    static bool save(const QString& path, const QByteArray& data = {}) {
        QFile out(path);
        return out.open(QIODevice::WriteOnly) && out.write(data) == data.size();
    }

private slots:
    void projectAndManualSelection() {
        QTemporaryDir dir;
        const QString video = dir.filePath(QStringLiteral("生肉.mp4"));
        const QString translated = dir.filePath(QStringLiteral("译文.ass"));
        const QString cooked = dir.filePath(QStringLiteral("熟肉.mp4"));
        QVERIFY(save(video));
        QVERIFY(save(dir.filePath(QStringLiteral("原文_OCR.ass"))));
        QVERIFY(findEncodeSubtitle(video).isEmpty());
        QVERIFY(save(dir.filePath(QStringLiteral("译文.srt"))));
        QVERIFY(save(translated));
        QCOMPARE(findEncodeSubtitle(video), translated);
        QCOMPARE(projectEncodeInput(dir.path()), video);
        QCOMPARE(projectEncodeOutput(video), cooked);
        QVERIFY(save(cooked));
        QVERIFY(findEncodeSubtitle(cooked).isEmpty());
        QVERIFY(QFile::remove(translated));
        QVERIFY(save(dir.filePath("effects.ass")));
        QCOMPARE(findEncodeSubtitle(video), dir.filePath("effects.ass"));
        QVERIFY(save(dir.filePath("alternative.ass")));
        bool ambiguous = false;
        QVERIFY(findEncodeSubtitle(video, &ambiguous).isEmpty());
        QVERIFY(ambiguous);
    }

    void renderColor_data() {
        QTest::addColumn<QString>("matrix");
        QTest::addColumn<QString>("range");
        QTest::addColumn<QString>("fmt");
        QTest::newRow("HD limited") << QString("bt709") << QString("tv") << QString("yuv420p");
        QTest::newRow("HD full") << QString("bt709") << QString("pc") << QString("yuv420p");
        QTest::newRow("SD 10-bit") << QString("smpte170m") << QString("tv") << QString("yuv420p10le");
    }

    void renderColor() {
        QFETCH(QString, matrix);
        QFETCH(QString, range);
        QFETCH(QString, fmt);
        QTemporaryDir dir;
        const QDir tools(qEnvironmentVariable("FKW_SUBTITLE_TEST_TOOLS", QStringLiteral(FKW_SUBTITLE_TOOLS)));
        const auto ffmpeg = tools.filePath("ffmpeg.exe");
        const auto plugin = tools.filePath("vsfiltermod/VSFilterMod.dll");
        if (!QFileInfo::exists(ffmpeg) || !QFileInfo::exists(plugin))
            QSKIP("FFmpeg and the Windows VSFilterMod runtime are needed for rendering");
        const QString sub = dir.filePath(QStringLiteral("特效字幕.ass"));
        QVERIFY(save(sub, R"([Script Info]
ScriptType: v4.00+
PlayResX: 128
PlayResY: 96
[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,18,&H00F2FA84,&H00000000,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1
[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.50,0:00:01.00,Default,,0,0,0,,{\pos(0,0)\p1}m 32 8 l 96 8 96 28 32 28
)"));
        EncodeSource src;
        src.duration = 1;
        src.video = {{"index", 0}, {"width", 128}, {"height", 96}, {"pix_fmt", fmt},
                     {"color_space", matrix}, {"color_range", range}, {"r_frame_rate", "2/1"}};
        SubtitleLayer layer;
        QString error;
        QVERIFY2(makeSubtitleLayer(src, sub, plugin, dir.path(), layer, error), qPrintable(error));
        const QString pattern = "testsrc2=s=128x96:r=2:d=1,format=" + fmt
            + ",setparams=colorspace=" + matrix + ":range=" + range;
        auto frames = [&](const QStringList& filters, const QString& pixelFormat) {
            QProcess p;
            p.setWorkingDirectory(dir.path());
            p.start(ffmpeg, QStringList{"-v","error","-f","lavfi","-i",pattern}
                + filters + QStringList{"-f","rawvideo","-pix_fmt",pixelFormat,"-"});
            if (!p.waitForFinished(30000) || p.exitCode()) {
                qWarning() << p.readAllStandardError();
                return QByteArray();
            }
            return p.readAllStandardOutput();
        };
        const auto reference = frames({}, fmt);
        const QStringList mask{"-i", layer.script, "-filter_complex"};
        const auto composite = frames(mask + QStringList{layer.graph + "[v]","-map","[v]"}, fmt);
        QVERIFY(!reference.isEmpty());
        QCOMPARE(composite.size(), reference.size());
        QCOMPARE(composite.first(composite.size()/2), reference.first(reference.size()/2));
        const QString toRgb = ",scale=in_range=" + range + ":out_range=pc:in_color_matrix="
            + (matrix == "bt709" ? "bt709" : "bt601") + ",format=rgb24[v]";
        const auto rgb = frames(mask + QStringList{layer.graph + toRgb,"-map","[v]"}, "rgb24");
        QCOMPARE(rgb.size(), 2 * 128 * 96 * 3);
        const int center = (128 * 96 + 16 * 128 + 64) * 3;
        const int target[] = {132, 250, 242};
        for (int c = 0; c < 3; ++c)
            QVERIFY(qAbs(int(uchar(rgb[center + c])) - target[c]) <= 4);
    }
};
QTEST_GUILESS_MAIN(SubtitleEncodeTests)
#include "subtitle_encode_tests.moc"
