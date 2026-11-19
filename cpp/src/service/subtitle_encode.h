#pragma once

#include <QJsonObject>
#include <QStringList>

namespace fkw {

// Project aliases apply only to the original episode video.
QString findEncodeSubtitle(const QString& video, bool* ambiguous = nullptr);
QString projectEncodeInput(const QString& folder);
QString projectEncodeOutput(const QString& input);

struct EncodeSource {
    QJsonObject video;
    QJsonObject audio;
    double duration = 0;
    qint64 videoBitrate = 0;
};

bool probeEncodeSource(const QString& probe, const QString& input, EncodeSource& source,
                       QString& error);
struct SubtitleLayer {
    QString script;
    QString graph;
    QStringList colorArgs;
    QString description;
};

bool makeSubtitleLayer(const EncodeSource& source, const QString& subtitle,
                       const QString& plugin, const QString& workDir,
                       SubtitleLayer& layer, QString& error);

} // namespace fkw
