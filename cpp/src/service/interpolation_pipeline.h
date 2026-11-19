#pragma once
#include "service/interpolation_service.h"
#include "common/logger.h"
#include <QProcess>
#include "service/video_frame_io.h"
namespace fkw {
class InterpolationDecoder {
public:
    InterpolationDecoder(const QString& input, const InterpolationOptions& options, const SuperresVideoInfo& info,
        qint64 first, qint64 count, const std::atomic<bool>& cancelled, std::function<bool()> stop = {});
    ~InterpolationDecoder();
    bool read(QByteArray& pixels, QString& error);
private:
    std::unique_ptr<VideoRawDecoder> decoder_;
    qint64 count_, read_ = 0;
};
bool analyzeInterpolationFrames(InterpolationDecoder& decoder, const QSize& size, const InterpolationOptions& options,
    QVector<QByteArray>& hashes, QVector<bool>& cuts, const std::atomic<bool>& cancelled,
    const std::function<void(qint64)>& progress, QString& error);
struct InterpolationBatch {
    qint64 sourceOffset = 0, outputOffset = 0;
    int outputCount = 0, inferenceCount = 0;
    int sourceCount = 0;
    QString directory, sequenceDirectory, segmentPath, manifest;
    QHash<qint64, VideoFramePtr> memoryInputs;
    QVector<VideoFramePtr> memorySequence;
};
class InterpolationPipeline {
public:
    InterpolationPipeline(const QString& input, const InterpolationOptions& options, const SuperresVideoInfo& info,
        const QString& rate, qint64 first, qint64 sourceFrames, const QVector<QByteArray>& hashes,
        const InterpolationPlan& plan, const QString& cache, const QSharedPointer<Logger>& logger, const std::atomic<bool>& cancelled,
        bool memoryFrames = false);
    ~InterpolationPipeline();
    bool next(InterpolationBatch& batch, const std::function<void()>& pulse);
    bool encode(InterpolationBatch batch, const std::function<void()>& pulse);
    bool finish(const std::function<void()>& pulse);
    bool failed() const;
    QString error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
