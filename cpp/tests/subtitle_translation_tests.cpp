#include "service/subtitle_translation.h"

#include <QDir>
#include <QFile>
#include <QStringEncoder>
#include <QTemporaryDir>
#include <QtTest>

using namespace fkw;

class SubtitleTranslationTests : public QObject {
    Q_OBJECT

private slots:
    void replacesDialogueTextAndSetsTheDefaultFont() {
        const QString source = QStringLiteral(
            "[Script Info]\r\nTitle: <thinking>Original title</thinking>\r\n"
            "[V4+ Styles]\r\nStyle: Default,Arial,42\r\n"
            "[Events]\r\nFormat: Style, End, Name, Start, Text\r\n"
            "Comment: Default,0:00:02.00,Editor,0:00:01.00,Leave this alone\r\n"
            "Dialogue: Default,0:00:02.00,Actor,0:00:01.00,{\\pos(10,20)}Hello, friend\\N"
            "{\\i1}How\\hare you?{\\i0}\r\n"
            "Dialogue: Default,0:00:02.00,,0:00:01.00,{\\p1}m 0 0 l 10 10{\\p0}\r\n"
            "[Fonts]\r\nfontname: fake.ttf\r\nend of font");
        const auto cues = parseAss(source);
        QCOMPARE(cues.size(), 1);
        const auto& cue = cues.first();
        QCOMPARE(cue.time, QStringLiteral("0:00:01.00 --> 0:00:02.00"));
        QCOMPARE(cue.previewText, QStringLiteral("Hello, friend\\nHow are you?"));
        QCOMPARE(cue.modelText, QStringLiteral("Hello, friend[[ASS1]]How[[ASS2]]are you?"));

        QString restored;
        QVERIFY(cue.restoreTranslation(QStringLiteral("你好，朋友[[ASS1]]近来[[ASS2]]好吗？"), restored));
        QString expected = source;
        expected.replace(QStringLiteral("Style: Default,Arial,42"),
                         QStringLiteral("Style: Default,Source Han Sans SC Medium,42"));
        expected.replace(QStringLiteral("Hello, friend\\N{\\i1}How\\hare you?"),
                         QStringLiteral("你好，朋友\\N{\\i1}近来\\h好吗？"));
        QCOMPARE(assembleAss(source, cues, {restored}), expected);
    }

    void setsOnlyFontnameInStylesWithCustomFieldOrder() {
        const QString source = QStringLiteral(
            "[V4+ Styles]\n"
            "Format: Name, Fontsize, FONTNAME, PrimaryColour\n"
            "Style: Default,42, Arial ,&H00F0F0F0\n"
            "Style: Speaker,58, Yu Gothic UI ,&H00FF0000\n"
            "[Events]\n"
            "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,32,,{\\fnArial\\fs36\\1c&H112233&}日本語");
        QString expected = source;
        expected.replace(QStringLiteral("42, Arial ,"),
                         QStringLiteral("42, Source Han Sans SC Medium ,"));
        expected.replace(QStringLiteral("58, Yu Gothic UI ,"),
                         QStringLiteral("58, Source Han Sans SC Medium ,"));
        QCOMPARE(assembleAss(source, parseAss(source), {}), expected);
    }

    void protectsInlineDrawingAndRejectsChangedMarkers() {
        const QString source = QStringLiteral(
            "[Events]\n"
            "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,"
            "Before{\\p1}m 0 0 l 4 0 4 4{\\p0}middle\\nafter\n"
            "Dialogue: 0,0:00:02.00,0:00:03.00,Default,,0,0,0,,Second cue");
        const auto cues = parseAss(source);
        QCOMPARE(cues.size(), 2);
        const auto& cue = cues.first();
        QCOMPARE(cue.modelText, QStringLiteral("Before[[ASS1]]middle[[ASS2]]after"));
        QString restored;
        QVERIFY(!cue.restoreTranslation(QStringLiteral("Missing [[ASS1]]"), restored));
        QVERIFY(!cue.restoreTranslation(QStringLiteral("Wrong [[ASS2]] order [[ASS1]]"), restored));
        QVERIFY(!cue.restoreTranslation(QStringLiteral("Duplicate [[ASS1]][[ASS1]][[ASS2]]"), restored));
        QVERIFY(!cue.restoreTranslation(QStringLiteral("Injected {\\an8} [[ASS1]] [[ASS2]] tag"), restored));
        QVERIFY(!cue.restoreTranslation(QStringLiteral("[[ASS1]][[ASS2]]"), restored));
        QVERIFY(cue.restoreTranslation(QStringLiteral("前[[ASS1]]中[[ASS2]]后"), restored));
        QCOMPARE(restored, QStringLiteral("前{\\p1}m 0 0 l 4 0 4 4{\\p0}中\\n后"));

        QVERIFY(cues.last().restoreTranslation(QStringLiteral("第二句"), restored));
        QString expected = source;
        expected.replace(QStringLiteral("Second cue"), restored);
        QCOMPARE(assembleAss(source, cues, {QString(), restored}), expected);
    }

    void readsUnicodeAssAndFindsProjectSources() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QDir project(temp.path());
        QFile ass(project.filePath(QStringLiteral("原文.ass")));
        const QString unicode = QStringLiteral("[Events]\r\nDialogue: 0,0:00:01.00,0:00:02.00,"
                                               "Default,,0,0,0,,日本語\r\n");
        QStringEncoder encoder(QStringEncoder::Utf16LE, QStringConverter::Flag::WriteBom);
        const QByteArray bytes = encoder(unicode);
        QVERIFY(ass.open(QIODevice::WriteOnly));
        QCOMPARE(ass.write(bytes), bytes.size());
        ass.close();
        QString contents, error;
        QVERIFY2(readSubtitleForTranslation(ass.fileName(), contents, error), qPrintable(error));
        QCOMPARE(contents, unicode);
        QCOMPARE(parseAss(contents).first().modelText, QStringLiteral("日本語"));
        QCOMPARE(subtitleFormatForPath(QStringLiteral("TEST.ASS")), SubtitleFormat::Ass);
        QCOMPARE(projectTranslationSource(project, SubtitleFormat::Ass), ass.fileName());
        QVERIFY(projectTranslationSource(project, SubtitleFormat::Srt).isEmpty());

        QFile srt(project.filePath(QStringLiteral("原文_Whisper.srt")));
        const QByteArray original("3\r\n00:00:01,000 --> 00:00:02,000\r\nHello\r\nagain");
        QVERIFY(srt.open(QIODevice::WriteOnly));
        QCOMPARE(srt.write(original), original.size());
        srt.close();
        QCOMPARE(projectTranslationSource(project, SubtitleFormat::Srt), srt.fileName());
        QVERIFY(readSubtitleForTranslation(srt.fileName(), contents, error));
        QCOMPARE(assembleSrt(parseSrt(contents)),
                 QStringLiteral("3\n00:00:01,000 --> 00:00:02,000\nHello\nagain"));
    }
};

QTEST_GUILESS_MAIN(SubtitleTranslationTests)
#include "subtitle_translation_tests.moc"
