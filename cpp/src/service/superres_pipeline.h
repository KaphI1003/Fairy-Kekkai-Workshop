#pragma once

#include <QHash>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>
#include "common/logger.h"
#include "service/superres_service.h"
#include "../../video_frames/frame_memory.h"

namespace fkw {
struct SuperresBatch {
    qint64 offset = 0;
    int count = 0;
    QString directory;
    QString inputDirectory;
    QString resultDirectory;
    QString sequenceDirectory;
    QString segmentPath;
    QVector<qint64> representatives;
    QHash<qint64, QString> freshResults;
    QHash<qint64, frames::FramePtr> memoryInputs, memoryResults;
    QVector<frames::FramePtr> memorySequence;
};

// A single sequential decoder prepares a bounded lookahead while an independent
// encoder consumes completed batches. The caller owns the GPU and reuse cache.
class SuperresPipeline {
public:
    SuperresPipeline(const QString& input, const SuperresOptions& options,
                     const SuperresVideoInfo& info, qint64 first, qint64 total,
                     const QVector<QByteArray>& hashes, const QVector<qint64>& representatives,
                     const QString& cache, const QSharedPointer<Logger>& logger,
                     const std::atomic<bool>& cancelled, bool memoryFrames = false);
    ~SuperresPipeline();
    bool next(SuperresBatch& batch, const std::function<void()>& pulse);
    bool encode(SuperresBatch batch, const std::function<void()>& pulse);
    bool finish(const std::function<void()>& pulse);
    bool failed() const;
    QString error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace fkw
