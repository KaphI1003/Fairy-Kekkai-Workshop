#pragma once
#include "service/superres_service.h"
#include <QVector>
namespace fkw {
struct InterpolationOptions {
    QString rateMode = QStringLiteral("preset");
    QString presetRate = QStringLiteral("60/1");
    int multiplier = 2;
    int customRate = 60;
    QString model = QStringLiteral("rife-v4.6");
    bool reuseHolds = true;
    bool protectCuts = true;
    int sceneThreshold = 30;
    QString quality = QStringLiteral("balanced");
    QString codec = QStringLiteral("libx264");
    QString audio = QStringLiteral("copy");
    QString container = QStringLiteral("mp4");
    QString enginePath, modelsPath, ffmpegPath, ffprobePath, tempPath;
    int gpu = -1;
    int batchFrames = 16;
    bool spatialTta = false, temporalTta = false, uhd = false;
    static InterpolationOptions fromConfig();
    QString targetRate(const QString& sourceRate) const;
    QString validate(const SuperresVideoInfo& source = {}) const;
    int crf() const;
};
enum class InterpolationFrameKind { Original, Hold, Cut, Infer };
struct InterpolationFrame {
    qint64 left = 0, right = 0;
    double time = 0;
    InterpolationFrameKind kind = InterpolationFrameKind::Original;
};
struct InterpolationPlan {
    QVector<InterpolationFrame> frames;
    qint64 inference = 0, holds = 0, cuts = 0, originals = 0;
};
double interpolationRate(const QString& value);
bool interpolationSceneCut(const QByteArray& previous, const QByteArray& current, const QSize& size, int threshold);
SuperresVideoInfo parseInterpolationVideoInfo(const QJsonObject& json);
InterpolationPlan planInterpolation(qint64 sourceFrames, const QString& sourceRate, const QString& targetRate,
    const QVector<QByteArray>& hashes, const QVector<bool>& cuts, const InterpolationOptions& options);
struct InterpolationTask : TaskBase {
    InterpolationTask(const QString& input, const QString& output, InterpolationOptions settings);
    const InterpolationOptions options;
    bool preview = false;
    double previewStart = 0, previewDuration = 5;
};
class InterpolationWorker : public TaskWorker {
    Q_OBJECT
public:
    explicit InterpolationWorker(std::shared_ptr<InterpolationTask> task);
    void run() override;
    void cancel() override;
    void cancelQueued();
signals:
    void progress(int percent, const QString& stage, qint64 frames, qint64 total,
                  double fps, double elapsed, double remaining);
    void statistics(qint64 source, qint64 output, qint64 inference, qint64 holds, qint64 cuts);
    void completed(bool success, bool cancelled, const QString& output, const QString& error);
private:
    bool process();
    bool execute(const QString& program, const QStringList& args, QByteArray* output = nullptr, int timeout = 0);
    void report(const QString& stage, qint64 done = 0, bool complete = false);
    std::shared_ptr<InterpolationTask> task_;
    QString error_;
    QString summary_;
    QSharedPointer<class Logger> logger_;
    QElapsedTimer wall_, pulse_;
    SuperresSpeedMeter speed_;
    qint64 work_ = -1, done_ = 0;
};
}
