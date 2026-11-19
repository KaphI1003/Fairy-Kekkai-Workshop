#pragma once
#include <QByteArray>
#include <QCryptographicHash>
#include <QSize>
#include <QThreadPool>
#include <QVector>
#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <bcrypt.h>
#endif

namespace fkw {
inline QByteArray videoFrameHash(const QByteArray& pixels) {
#ifdef BCRYPT_SHA256_ALG_HANDLE
    if (quint64(pixels.size()) <= std::numeric_limits<ULONG>::max()) {
        QByteArray digest(32, Qt::Uninitialized);
        auto* data = reinterpret_cast<PUCHAR>(const_cast<char*>(pixels.constData()));
        if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, data, ULONG(pixels.size()),
                       reinterpret_cast<PUCHAR>(digest.data()), ULONG(digest.size())) == 0)
            return digest;
    }
#endif
    return QCryptographicHash::hash(pixels, QCryptographicHash::Sha256);
}

using VideoCutDetector = std::function<bool(const QByteArray&, const QByteArray&)>;

template<class Decoder>
bool analyzeVideoFrames(Decoder& decoder, const QSize& size, QVector<QByteArray>& hashes,
    const std::atomic<bool>& cancelled, const VideoCutDetector& detectCut,
    const std::function<void(qsizetype, bool)>& frameReady, QString& error) {
    // Consecutive pairs share their boundary frame. Reserve room for that extra frame.
    const qint64 bytesPerFrame = qint64(size.width()) * size.height() * 3;
    const int byMemory = int(qMax<qint64>(1, (64 * 1024 * 1024) / bytesPerFrame - 1));
    QThreadPool pool;
    const int depth = std::min({4, byMemory, qMax(1, pool.maxThreadCount())});
    pool.setMaxThreadCount(depth);
    struct Result { QByteArray hash; bool cut; };
    std::deque<std::future<Result>> pending;
    QByteArray previous;
    qsizetype next = 0;
    for (qsizetype index = 0; index < hashes.size(); ++index) {
        while (next < hashes.size() && pending.size() < size_t(depth) && !cancelled.load()) {
            QByteArray pixels;
            if (!decoder.read(pixels, error)) return false;
            std::packaged_task<Result()> job([pixels, previous, &detectCut] {
                return Result{videoFrameHash(pixels),
                    detectCut && detectCut(previous, pixels)};
            });
            pending.push_back(job.get_future());
            pool.start(std::move(job));
            if (detectCut) previous = std::move(pixels);
            ++next;
        }
        if (cancelled.load()) return false;
        auto frame = pending.front().get();
        pending.pop_front();
        hashes[index] = std::move(frame.hash);
        frameReady(index, frame.cut);
    }
    return !cancelled.load();
}
} // namespace fkw
