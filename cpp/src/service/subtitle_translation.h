#pragma once

#include <QList>
#include <QStringList>

class QDir;

namespace fkw {

enum class SubtitleFormat { Unsupported, Srt, Ass };

SubtitleFormat subtitleFormatForPath(const QString& path);
bool readSubtitleForTranslation(const QString& path, QString& content, QString& error);
QString projectTranslationSource(const QDir& dir, SubtitleFormat format);

struct SrtItem {
    QString index;
    QString time;
    QString text;
};

QList<SrtItem> parseSrt(const QString& content);
QString assembleSrt(const QList<SrtItem>& items);

struct AssCue {
    QString time;
    QString previewText;
    QString modelText;
    bool restoreTranslation(const QString& translation, QString& text) const;

private:
    friend QList<AssCue> parseAss(const QString& content);
    friend QString assembleAss(const QString& content, const QList<AssCue>& cues,
                               const QStringList& translations);

    qsizetype offset = 0;
    qsizetype length = 0;
    QString prefix;
    QString suffix;
    QStringList protectedText;
};

QList<AssCue> parseAss(const QString& content);
QString assembleAss(const QString& content, const QList<AssCue>& cues,
                    const QStringList& translations);

}  // namespace fkw
