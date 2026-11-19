#include "service/subtitle_translation.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStringConverter>

namespace fkw {
namespace {

const QRegularExpression assMarkup(QStringLiteral(R"(\{[^}]*\}|\\[Nnh{}])"));
const QRegularExpression assMarker(QStringLiteral(R"(\[\[ASS\d+\]\])"));
const QRegularExpression assLines(QStringLiteral(R"([^\r\n]+)"));

QString markerFor(qsizetype index) {
    return QStringLiteral("[[ASS%1]]").arg(index + 1);
}

struct AssTextPart {
    QStringView text;
    bool visible;
};

QList<AssTextPart> splitAssText(QStringView text) {
    // Keep drawing commands with their surrounding override blocks.
    static const QRegularExpression drawingTag(QStringLiteral(R"(\\p\s*([+-]?\d+))"));
    QList<AssTextPart> parts;
    qsizetype offset = 0;
    bool drawing = false;
    auto matches = assMarkup.globalMatchView(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (match.capturedStart() > offset)
            parts.append({text.sliced(offset, match.capturedStart() - offset), !drawing});
        const QStringView markup = match.capturedView();
        parts.append({markup, false});
        if (markup.startsWith(u'{')) {
            auto tags = drawingTag.globalMatchView(markup);
            while (tags.hasNext()) drawing = tags.next().capturedView(1).toInt() > 0;
        }
        offset = match.capturedEnd();
    }
    if (offset < text.size()) parts.append({text.sliced(offset), !drawing});
    return parts;
}

void applyTranslationFont(QString& content) {
    QList<QPair<qsizetype, qsizetype>> fields;
    bool inStyles = false;
    qsizetype fontColumn = 1;
    auto rows = assLines.globalMatch(content);
    while (rows.hasNext()) {
        const auto row = rows.next();
        const QStringView line = row.capturedView();
        const QStringView trimmed = line.trimmed();
        if (trimmed.startsWith(u'[') && trimmed.endsWith(u']')) {
            inStyles = trimmed.compare(u"[V4 Styles]", Qt::CaseInsensitive) == 0 ||
                       trimmed.compare(u"[V4+ Styles]", Qt::CaseInsensitive) == 0 ||
                       trimmed.compare(u"[V4++ Styles]", Qt::CaseInsensitive) == 0;
            fontColumn = 1;
            continue;
        }
        if (!inStyles) continue;
        const qsizetype colon = line.indexOf(u':');
        if (colon < 0) continue;
        const QStringView type = line.first(colon).trimmed();
        const auto columns = line.sliced(colon + 1).split(u',');
        if (type.compare(u"Format", Qt::CaseInsensitive) == 0) {
            fontColumn = -1;
            for (qsizetype i = 0; i < columns.size(); ++i) {
                if (columns.at(i).trimmed().compare(u"Fontname", Qt::CaseInsensitive) != 0) continue;
                fontColumn = i;
                break;
            }
        } else if (type.compare(u"Style", Qt::CaseInsensitive) == 0 &&
                   fontColumn >= 0 && fontColumn < columns.size()) {
            const QStringView font = columns.at(fontColumn).trimmed();
            if (!font.isEmpty())
                fields.append({row.capturedStart() + (font.data() - line.data()), font.size()});
        }
    }

    for (auto field = fields.crbegin(); field != fields.crend(); ++field)
        content.replace(field->first, field->second, QStringLiteral("Source Han Sans SC Medium"));
}

}  // namespace

SubtitleFormat subtitleFormatForPath(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("srt")) return SubtitleFormat::Srt;
    if (suffix == QStringLiteral("ass")) return SubtitleFormat::Ass;
    return SubtitleFormat::Unsupported;
}

bool readSubtitleForTranslation(const QString& path, QString& content, QString& error) {
    const auto format = subtitleFormatForPath(path);
    if (format == SubtitleFormat::Unsupported) {
        error = QStringLiteral("请选择 SRT 或 ASS 字幕文件");
        return false;
    }

    QFile file(path);
    const auto mode = format == SubtitleFormat::Ass ? QIODevice::ReadOnly
                                                     : QIODevice::ReadOnly | QIODevice::Text;
    if (!file.open(mode)) {
        error = file.errorString();
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        error = file.errorString();
        return false;
    }
    if (format == SubtitleFormat::Ass) {
        const auto encoding = QStringConverter::encodingForData(bytes).value_or(QStringConverter::Utf8);
        QStringDecoder decoder(encoding, QStringConverter::Flag::Stateless);
        content = decoder(bytes);
        if (decoder.hasError()) {
            error = QStringLiteral("无法读取 ASS 字幕编码，请将文件保存为 UTF-8 后重试");
            return false;
        }
    } else {
        content = QString::fromUtf8(bytes);
    }
    return true;
}

QString projectTranslationSource(const QDir& dir, SubtitleFormat format) {
    QStringList names;
    if (format == SubtitleFormat::Ass)
        names = {QStringLiteral("原文.ass")};
    else if (format == SubtitleFormat::Srt)
        names = {QStringLiteral("原文.srt"), QStringLiteral("原文_OCR.srt"),
                 QStringLiteral("原文_Whisper.srt")};
    for (const QString& name : names) {
        const QFileInfo source(dir.filePath(name));
        if (source.isFile()) return source.absoluteFilePath();
    }
    return {};
}

QList<SrtItem> parseSrt(const QString& content) {
    QList<SrtItem> items;
    static const QRegularExpression separator(QStringLiteral("\\n\\s*\\n"));
    const QStringList blocks = content.trimmed().split(separator, Qt::SkipEmptyParts);
    for (const QString& rawBlock : blocks) {
        const QStringList lines = rawBlock.trimmed().split(QLatin1Char('\n'));
        if (lines.size() < 3) continue;
        items.append({lines.at(0).trimmed(), lines.at(1).trimmed(),
                      lines.mid(2).join(QLatin1Char('\n')).trimmed()});
    }
    return items;
}

QString assembleSrt(const QList<SrtItem>& items) {
    QStringList blocks;
    blocks.reserve(items.size());
    for (const SrtItem& item : items)
        blocks << item.index + QLatin1Char('\n') + item.time + QLatin1Char('\n') + item.text;
    return blocks.join(QStringLiteral("\n\n"));
}

QList<AssCue> parseAss(const QString& content) {
    QList<AssCue> cues;
    bool inEvents = false;
    qsizetype startColumn = 1, endColumn = 2, textColumn = 9;
    auto rows = assLines.globalMatch(content);
    while (rows.hasNext()) {
        const auto row = rows.next();
        const QStringView line = row.capturedView();
        const QStringView trimmed = line.trimmed();
        if (trimmed.startsWith(u'[') && trimmed.endsWith(u']')) {
            inEvents = trimmed.compare(u"[Events]", Qt::CaseInsensitive) == 0;
            continue;
        }
        if (!inEvents) continue;

        const qsizetype colon = line.indexOf(u':');
        if (colon < 0) continue;
        const QStringView type = line.first(colon).trimmed();
        const qsizetype fieldsStart = colon + 1;
        if (type.compare(u"Format", Qt::CaseInsensitive) == 0) {
            QStringList columns = line.sliced(fieldsStart).toString().toLower().split(u',');
            for (QString& column : columns) column = column.trimmed();
            startColumn = columns.indexOf(QStringLiteral("start"));
            endColumn = columns.indexOf(QStringLiteral("end"));
            textColumn = columns.endsWith(QStringLiteral("text")) ? columns.size() - 1 : -1;
            continue;
        }
        if (type.compare(u"Dialogue", Qt::CaseInsensitive) != 0 ||
            startColumn < 0 || endColumn < 0 || textColumn <= qMax(startColumn, endColumn)) continue;

        AssCue cue;
        QStringView start, end;
        qsizetype field = fieldsStart;
        for (qsizetype column = 0; column < textColumn; ++column) {
            const qsizetype comma = line.indexOf(u',', field);
            if (comma < 0) {
                field = -1;
                break;
            }
            if (column == startColumn) start = line.sliced(field, comma - field).trimmed();
            if (column == endColumn) end = line.sliced(field, comma - field).trimmed();
            field = comma + 1;
        }
        if (field < 0 || start.isEmpty() || end.isEmpty()) continue;

        const QStringView text = line.sliced(field);
        const auto parts = splitAssText(text);
        qsizetype first = 0, last = parts.size();
        const auto hasWords = [](const AssTextPart& part) {
            return part.visible && !part.text.trimmed().isEmpty();
        };
        while (first < last && !hasWords(parts.at(first))) ++first;
        if (first == last) continue;
        while (!hasWords(parts.at(last - 1))) --last;

        cue.offset = row.capturedStart() + field;
        cue.length = text.size();
        cue.time = start.toString() + QStringLiteral(" --> ");
        cue.time += end;
        for (qsizetype i = 0; i < first; ++i) cue.prefix += parts.at(i).text;
        for (qsizetype i = last; i < parts.size(); ++i) cue.suffix += parts.at(i).text;
        for (qsizetype i = 0; i < parts.size(); ++i) {
            const auto& part = parts.at(i);
            if (part.visible) cue.previewText += part.text;
            else if (part.text == u"\\N" || part.text == u"\\n") cue.previewText += QStringLiteral("\\n");
            else if (part.text == u"\\h") cue.previewText += u' ';
        }
        for (qsizetype i = first; i < last;) {
            if (parts.at(i).visible) {
                cue.modelText += parts.at(i++).text;
                continue;
            }
            QString protectedPart;
            do {
                protectedPart += parts.at(i++).text;
            } while (i < last && !parts.at(i).visible);
            cue.modelText += markerFor(cue.protectedText.size());
            cue.protectedText.append(protectedPart);
        }
        cues.append(std::move(cue));
    }
    return cues;
}

bool AssCue::restoreTranslation(const QString& translation, QString& text) const {
    static const QRegularExpression newlines(QStringLiteral(R"([\r\n]+)"));
    QString cleaned = translation.trimmed();
    cleaned.replace(newlines, QStringLiteral(" "));
    if (cleaned.isEmpty() || assMarkup.match(cleaned).hasMatch()) return false;

    QString restored = prefix;
    qsizetype index = 0, offset = 0;
    bool hasText = false;
    auto markers = assMarker.globalMatch(cleaned);
    while (markers.hasNext()) {
        const auto marker = markers.next();
        if (index >= protectedText.size() || marker.capturedView() != markerFor(index)) return false;
        const auto plain = QStringView(cleaned).sliced(offset, marker.capturedStart() - offset);
        hasText |= !plain.trimmed().isEmpty();
        restored += plain;
        restored += protectedText.at(index++);
        offset = marker.capturedEnd();
    }
    const auto tail = QStringView(cleaned).sliced(offset);
    if (index != protectedText.size() || (!hasText && tail.trimmed().isEmpty())) return false;
    restored += tail;
    restored += suffix;
    text = std::move(restored);
    return true;
}

QString assembleAss(const QString& content, const QList<AssCue>& cues,
                    const QStringList& translations) {
    QString result;
    result.reserve(content.size());
    const QStringView original(content);
    qsizetype offset = 0;
    for (qsizetype i = 0; i < cues.size() && i < translations.size(); ++i) {
        if (translations.at(i).isEmpty()) continue;
        const AssCue& cue = cues.at(i);
        result += original.sliced(offset, cue.offset - offset);
        result += translations.at(i);
        offset = cue.offset + cue.length;
    }
    result += original.sliced(offset);
    applyTranslationFont(result);
    return result;
}

}  // namespace fkw
