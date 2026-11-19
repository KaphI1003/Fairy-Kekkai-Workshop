#pragma once

#include <QJsonObject>
#include <QElapsedTimer>
#include <QList>
#include <QSize>
#include <QSharedPointer>
#include <QStringList>
#include <functional>
#include <memory>
#include "service/task_base.h"

namespace fkw {
inline const QString kAniSdSpanModel = QStringLiteral("2x_AniSD_G6i1_SPAN_215K");
const QStringList& superresModelIds();
qint64 superresMonotonicMs();
class SuperresSpeedMeter {
public:
    void start(qint64 now);
    double update(qint64 now, qint64 frames);
private:
    QList<QPair<qint64, qint64>> samples_;
};
struct SuperresVideoInfo {
    QSize size;
    QSize decodedSize;
    QString frameRate;
    double fps = 0;
    double duration = 0;
    qint64 frames = 0;
    bool exactFrames = false;
    bool hasAudio = false;
    bool variableFrameRate = false;
    bool hdr = false;
};

struct SuperresOptions {
    QString sizeMode = QStringLiteral("preset");
    int presetHeight = 1080;
    int scale = 2;
    int width = 1920;
    int height = 1080;
    bool lockRatio = true;
    QString fit = QStringLiteral("pad");
    QString model = QStringLiteral("realesr-animevideov3");
    QString quality = QStringLiteral("balanced");
    QString codec = QStringLiteral("libx264");
    QString audio = QStringLiteral("copy");
    QString container = QStringLiteral("mp4");
    QString enginePath;
    QString modelsPath;
    QString ffmpegPath;
    QString ffprobePath;
    QString tempPath;
    int gpu = -1;
    int tile = 0;
    int batchFrames = 16;
    bool tta = false;
    bool reuseIdenticalFrames = true;

    static SuperresOptions fromConfig();
    QSize targetSize(const QSize& source) const;
    int modelScale(const QSize& source) const;
    int crf() const;
    QString validate(const QSize& source = {}) const;
};

SuperresVideoInfo parseSuperresVideoInfo(const QJsonObject& json);
QStringList superresProbeArguments(const QString& input);
QString superresScaleFilter(const QSize& target, const QString& fit);

struct SuperresTask : TaskBase {
    SuperresTask(const QString& input, const QString& output, SuperresOptions options);
    const SuperresOptions options;
    bool preview = false;
    double previewStart = 0;
    double previewDuration = 5;
};

class SuperresWorker : public TaskWorker {
    Q_OBJECT
public:
    explicit SuperresWorker(std::shared_ptr<SuperresTask> task);
    void run() override;
    void cancel() override;
    void cancelQueued();
signals:
    void progress(int percent, const QString& stage, qint64 frames, qint64 total,
                  double fps, double elapsed, double remaining);
    void completed(bool success, bool cancelled, const QString& output, const QString& error);
    void frameReuseProgress(qint64 inferred, qint64 reused, qint64 unique, qint64 total);
private:
    bool execute(const QString& executable, const QStringList& args,
                 const std::function<void()>& poll = {}, QByteArray* standardOutput = nullptr,
                 int timeoutMs = 0, const std::function<bool()>& abort = {});
    void report(const QString& stage, qint64 frames, qint64 total, bool complete = false);
    bool process();
    std::shared_ptr<SuperresTask> task_;
    QString error_;
    QSharedPointer<class Logger> logger_;
    qint64 startedAt_ = 0;
    qint64 sourceFrames_ = 0;
    qint64 uniqueFrames_ = 0;
    qint64 inferredFrames_ = 0;
    qint64 reusedFrames_ = 0;
    QElapsedTimer wallTimer_;
    SuperresSpeedMeter speedMeter_;
};
} // namespace fkw
