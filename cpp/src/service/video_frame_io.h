#pragma once
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QProcess>
#include <QElapsedTimer>
#include <QUuid>
#include <QHash>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include "common/app_data.h"
#include "../../video_frames/frame_memory.h"
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace fkw {
using VideoFramePtr = frames::FramePtr;
#ifdef Q_OS_WIN
// Read through a bounded OS pipe and wake on data, rather than polling FFmpeg's
// process handle. QProcess's asynchronous stdout reader has no backpressure.
class BoundedDecoderPipe {
public:
    ~BoundedDecoderPipe() {
        closeWriter();
        if (reader_ != INVALID_HANDLE_VALUE) CloseHandle(reader_);
        if (event_) CloseHandle(event_);
    }
    bool attach(QProcess& process) {
        const QString name = QStringLiteral("\\\\.\\pipe\\fkw-superres-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        reader_ = CreateNamedPipeW(reinterpret_cast<LPCWSTR>(name.utf16()),
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr);
        if (reader_ == INVALID_HANDLE_VALUE) return false;
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        writer_ = CreateFileW(reinterpret_cast<LPCWSTR>(name.utf16()), GENERIC_WRITE, 0, &security, OPEN_EXISTING, 0, nullptr);
        if (writer_ == INVALID_HANDLE_VALUE) { writer_ = nullptr; return false; }
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event_) return false;
        OVERLAPPED connection{}; connection.hEvent = event_;
        if (!ConnectNamedPipe(reader_, &connection) && GetLastError() != ERROR_PIPE_CONNECTED) return false;
        process.setCreateProcessArgumentsModifier([this](QProcess::CreateProcessArguments* args) {
            args->startupInfo->dwFlags |= STARTF_USESTDHANDLES;
            args->startupInfo->hStdOutput = writer_;
        });
        return true;
    }
    void closeWriter() { if (writer_) { CloseHandle(writer_); writer_ = nullptr; } }
    qint64 read(char* data, qint64 maximum) {
        OVERLAPPED operation{}; operation.hEvent = event_; ResetEvent(event_);
        DWORD received = 0;
        if (ReadFile(reader_, data, DWORD(qMin<qint64>(maximum, MAXDWORD)), &received, &operation)) return received;
        if (GetLastError() != ERROR_IO_PENDING) return -1;
        if (WaitForSingleObject(event_, 50) != WAIT_OBJECT_0) CancelIoEx(reader_, &operation);
        // Always settle the I/O before the caller reuses/frees its pixel buffer.
        if (!GetOverlappedResult(reader_, &operation, &received, TRUE))
            return GetLastError() == ERROR_OPERATION_ABORTED ? 0 : -1;
        return received;
    }
private:
    HANDLE reader_ = INVALID_HANDLE_VALUE, writer_ = nullptr, event_ = nullptr;
};
#endif


inline QStringList videoRgbDecodeArguments(const QString& input, const QSize& size, double fps, qint64 first, qint64 count) {
    QStringList args{"-hide_banner", "-loglevel", "error", "-nostdin", "-threads", "2", "-filter_threads", "1"};
    if (first > 0) args << "-ss" << QString::number((first - .25) / fps, 'f', 9);
    args << "-i" << input << "-map" << "0:v:0" << "-frames:v" << QString::number(count)
        << "-fps_mode" << "passthrough" << "-vf" << QStringLiteral("scale=%1:%2,setsar=1").arg(size.width()).arg(size.height())
        << "-pix_fmt" << "rgb24" << "-f" << "rawvideo" << "pipe:1";
    return args;
}

class VideoRawDecoder {
public:
    VideoRawDecoder(const QString& program, const QStringList& args, qsizetype bytes, qint64 total,
                    std::function<bool()> abort)
        : frameBytes_(bytes), total_(total), abort_(std::move(abort)) {
#ifdef Q_OS_WIN
        if (!output_.attach(process_)) { startError_ = trText("无法创建有界视频解码管道。"); return; }
#endif
        process_.setWorkingDirectory(QFileInfo(program).absolutePath());
        process_.start(program, args);
        if (!process_.waitForStarted(10000)) startError_ = trText("无法启动连续视频解码：") + process_.errorString();
#ifdef Q_OS_WIN
        output_.closeWriter();
#endif
    }
    ~VideoRawDecoder() { if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(5000); } }
    bool read(QByteArray& pixels, QString& error) {
        if (!startError_.isEmpty()) { error = startError_; return false; }
        pixels.resize(frameBytes_); qsizetype received = 0; QElapsedTimer noData; noData.start();
        while (received < frameBytes_ && !abort_()) {
#ifdef Q_OS_WIN
            const auto bytes = output_.read(pixels.data() + received, frameBytes_ - received);
#else
            const auto bytes = process_.read(pixels.data() + received, frameBytes_ - received);
#endif
            if (bytes > 0) { received += bytes; noData.restart(); continue; }
            collectErrors();
            if (process_.state() == QProcess::NotRunning) {
                error = trText("连续解码的视频帧数不完整：") +
                    QStringLiteral("frame=%1/%2 bytes=%3/%4 exit=%5 status=%6; ")
                        .arg(readFrames_).arg(total_).arg(received).arg(frameBytes_)
                        .arg(process_.exitCode()).arg(int(process_.exitStatus())) + QString::fromUtf8(errors_);
                return false;
            }
#ifdef Q_OS_WIN
            process_.waitForFinished(0);
#else
            process_.waitForReadyRead(50);
#endif
            if (noData.elapsed() > 30000) { error = trText("连续视频解码超时：") + QString::fromUtf8(errors_); return false; }
        }
        if (abort_()) return false;
        ++readFrames_; return true;
    }
    bool finish(QString& error) {
        QElapsedTimer elapsed; elapsed.start();
        while (process_.state() != QProcess::NotRunning && !abort_() && elapsed.elapsed() < 30000) {
            process_.waitForFinished(50); process_.readAllStandardOutput(); collectErrors();
        }
        collectErrors();
        if (abort_()) return false;
        if (process_.state() != QProcess::NotRunning || process_.exitStatus() != QProcess::NormalExit || process_.exitCode()) {
            error = trText("连续视频解码失败：") + QString::fromUtf8(errors_); return false;
        }
        return true;
    }
private:
    void collectErrors() { errors_ += process_.readAllStandardError(); if (errors_.size() > 8192) errors_ = errors_.right(8192); }
#ifdef Q_OS_WIN
    BoundedDecoderPipe output_;
#endif
    QProcess process_;
    qsizetype frameBytes_; qint64 total_, readFrames_ = 0;
    std::function<bool()> abort_; QString startError_; QByteArray errors_;
};

inline VideoFramePtr videoFrameFromRgb(const QByteArray& pixels, const QSize& size) {
    auto result = frames::MemoryFrame::create(size.width(), size.height());
    if (pixels.size() != qsizetype(result->bytes())) throw std::runtime_error("Invalid decoded frame size");
    memcpy(result->data(), pixels.constData(), result->bytes()); result->publish(); return result;
}
inline int videoMemoryBatch(int requested, uint64_t inputBytes, uint64_t outputBytes) {
    const uint64_t cost = qMax<uint64_t>(1, inputBytes + outputBytes);
    return qMax(1, qMin(requested, int((128ULL * 1024 * 1024) / cost)));
}
inline bool videoMemoryTransport() {
#ifdef Q_OS_WIN
    return !qEnvironmentVariableIsSet("FKW_VIDEO_PNG_TRANSPORT");
#else
    return false;
#endif
}

// FFmpeg receives a bounded raw RGB stream. At most 256 KiB can be queued in
// QProcess; every wait checks cancellation and the other pipeline stages.
inline QStringList videoRawEncodeArguments(const QSize& size, const QString& rate, qint64 count,
        const QString& filter, const QString& codec, int crf, const QString& output) {
    return {"-hide_banner", "-loglevel", "error", "-nostdin", "-f", "rawvideo", "-pixel_format", "rgb24",
        "-video_size", QStringLiteral("%1x%2").arg(size.width()).arg(size.height()), "-framerate", rate,
        "-i", "pipe:0", "-frames:v", QString::number(count), "-vf", filter, "-c:v", codec, "-crf", QString::number(crf),
        "-preset", "medium", "-pix_fmt", "yuv420p", "-colorspace", "bt709", "-color_primaries", "bt709",
        "-color_trc", "bt709", "-color_range", "tv", "-an", "-n", output};
}
class VideoRawEncoder {
public:
    ~VideoRawEncoder() { if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(5000); } }
    bool start(const QString& program, const QStringList& args, const QString& output, QString& error) {
        if (QFileInfo::exists(output)) { error = trText("视频编码目标已存在。"); return false; }
        output_ = output; process_.start(program, args);
        if (!process_.waitForStarted(10000)) { error = trText("无法启动视频编码：") + process_.errorString(); return false; }
        return true;
    }
    bool write(const VideoFramePtr& frame, const std::function<bool()>& stop, QString& error) {
        if (!frame || !frame->ready()) { error = trText("内存视频帧尚未完成。"); return false; }
        size_t offset = 0; QElapsedTimer noProgress; noProgress.start();
        while ((offset < frame->bytes() || process_.bytesToWrite()) && !stop()) {
            process_.readAllStandardOutput(); collectErrors();
            if (process_.state() == QProcess::NotRunning) { error = trText("视频编码失败：") + QString::fromUtf8(errors_); return false; }
            if (offset < frame->bytes() && process_.bytesToWrite() < 256 * 1024) {
                const qint64 count = qMin<size_t>(65536, frame->bytes() - offset);
                if (process_.write(reinterpret_cast<const char*>(frame->data() + offset), count) != count) {
                    error = trText("无法发送视频像素：") + process_.errorString(); return false;
                }
                offset += size_t(count); noProgress.restart();
            }
            if (process_.bytesToWrite()) {
                if (process_.waitForBytesWritten(50)) noProgress.restart();
                if (noProgress.elapsed() > 30000) { error = trText("视频编码输入超时。"); return false; }
            }
        }
        return !stop();
    }
    bool finish(const std::function<bool()>& stop, QString& error) {
        process_.closeWriteChannel(); QElapsedTimer timeout; timeout.start();
        do {
            process_.waitForFinished(50); collectErrors(); process_.readAllStandardOutput();
            if (stop()) return false;
            if (timeout.elapsed() > 120000) { error = trText("视频编码收尾超时。"); return false; }
        } while (process_.state() != QProcess::NotRunning);
        if (process_.exitStatus() != QProcess::NormalExit || process_.exitCode() || QFileInfo(output_).size() <= 0) {
            error = trText("视频编码失败：") + QString::fromUtf8(errors_); return false;
        }
        return true;
    }
private:
    void collectErrors() { errors_ += process_.readAllStandardError(); if (errors_.size() > 8192) errors_ = errors_.right(8192); }
    QProcess process_; QString output_; QByteArray errors_;
};

// Only representatives used again in a later batch are saved. PNG work runs on
// its own CPU thread; pending pixels are capped by bytes, and no history of raw
// frames is retained in RAM. Disk files survive only until their last reuse.
class VideoReuseCache {
public:
    VideoReuseCache() : writer_([this] { run(); }) {}
    ~VideoReuseCache() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; changed_.notify_all(); }
        if (writer_.joinable()) writer_.join();
    }
    bool put(const QString& path, VideoFramePtr frame, const std::function<bool()>& abort, const std::function<void()>& pulse) {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_ && error_.isEmpty() && pendingBytes_ && pendingBytes_ + frame->bytes() > 64 * 1024 * 1024) {
            changed_.wait_for(lock, std::chrono::milliseconds(50)); lock.unlock(); pulse(); const bool stop = abort(); lock.lock();
            if (stop) return false;
        }
        if (stop_ || !error_.isEmpty() || abort()) return false;
        auto state = std::make_shared<State>(); state->path = path; state->frame = std::move(frame);
        pendingBytes_ += state->frame->bytes(); states_.insert(path, state); queue_.push_back(state); changed_.notify_all(); return true;
    }
    VideoFramePtr load(const QString& path, const std::function<bool()>& abort, const std::function<void()>& pulse) {
        if (!wait(path, abort, pulse)) return {};
        const QImage image = QImage(path).convertToFormat(QImage::Format_RGB888);
        if (image.isNull()) return {};
        auto result = frames::MemoryFrame::create(image.width(), image.height());
        for (int row = 0; row < image.height(); ++row) memcpy(result->data() + size_t(row) * image.width() * 3, image.constScanLine(row), size_t(image.width()) * 3);
        result->publish(); return result;
    }
    bool drop(const QString& path, const std::function<bool()>& abort, const std::function<void()>& pulse) {
        if (!wait(path, abort, pulse)) return false;
        { std::lock_guard<std::mutex> lock(mutex_); states_.remove(path); }
        return QFile::remove(path);
    }
    QString error() { std::lock_guard<std::mutex> lock(mutex_); return error_; }
private:
    struct State { QString path; VideoFramePtr frame; bool done = false; };
    bool wait(const QString& path, const std::function<bool()>& abort, const std::function<void()>& pulse) {
        std::unique_lock<std::mutex> lock(mutex_); auto state = states_.value(path);
        if (!state) return false;
        while (!state->done && !stop_ && error_.isEmpty()) {
            changed_.wait_for(lock, std::chrono::milliseconds(50)); lock.unlock(); pulse(); const bool stop = abort(); lock.lock();
            if (stop) return false;
        }
        return state->done && error_.isEmpty();
    }
    void run() {
        for (;;) {
            std::shared_ptr<State> state;
            { std::unique_lock<std::mutex> lock(mutex_); changed_.wait(lock, [&] { return stop_ || !queue_.empty(); });
              if (stop_) return; state = queue_.front(); queue_.pop_front(); }
            bool ok = false;
            try {
                const QImage image(state->frame->data(), state->frame->width(), state->frame->height(), state->frame->width() * 3, QImage::Format_RGB888);
                QImageWriter writer(state->path, "png"); writer.setCompression(1); ok = writer.write(image);
            } catch (...) {}
            { std::lock_guard<std::mutex> lock(mutex_);
              pendingBytes_ -= state->frame->bytes(); state->frame.reset(); state->done = true;
              if (!ok) error_ = trText("无法保存重复帧缓存。"); changed_.notify_all(); }
        }
    }
    std::mutex mutex_; std::condition_variable changed_;
    QHash<QString, std::shared_ptr<State>> states_; std::deque<std::shared_ptr<State>> queue_;
    size_t pendingBytes_ = 0; bool stop_ = false; QString error_;
    std::thread writer_;
};

} // namespace fkw

