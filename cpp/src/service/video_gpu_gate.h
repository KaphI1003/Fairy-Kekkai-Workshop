#pragma once
#include <QSemaphore>
#include <atomic>
#include <functional>
namespace fkw {
// Serializes GPU model ownership across interpolation and super-resolution.
class VideoGpuLease {
public:
    VideoGpuLease() = default;
    VideoGpuLease(const VideoGpuLease&) = delete;
    VideoGpuLease& operator=(const VideoGpuLease&) = delete;
    ~VideoGpuLease() { if (held_) gate().release(); }
    bool acquire(const std::atomic<bool>& cancelled, const std::function<void()>& pulse) {
        while (!cancelled.load()) {
            if (gate().tryAcquire(1, 50)) { held_ = true; return true; }
            pulse();
        }
        return false;
    }
private:
    static QSemaphore& gate() { static QSemaphore value(1); return value; }
    bool held_ = false;
};
}
