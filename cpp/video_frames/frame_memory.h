#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fkw::frames {
// All transport pixels are tightly packed RGB24. The named mapping owns no
// files; each process holds a handle until its last consumer releases the frame.
class MemoryFrame {
public:
    using Ptr = std::shared_ptr<MemoryFrame>;
    static bool isMemory(const std::string& uri) { return uri.rfind("fkwframe:", 0) == 0; }
    static uint64_t pixelBytes(int width, int height) {
        if (width <= 0 || height <= 0 || width > 32768 || height > 32768 || uint64_t(width) * height * 3 > 512ULL * 1024 * 1024)
            throw std::runtime_error("Invalid or oversized video frame");
        return uint64_t(width) * height * 3;
    }
    static Ptr create(int width, int height) {
#ifdef _WIN32
        const auto bytes = pixelBytes(width, height);
        static std::atomic<uint64_t> sequence{0};
        const std::string name = "Local\\fkw-frame-" + std::to_string(GetCurrentProcessId()) + "-" +
            std::to_string(GetTickCount64()) + "-" + std::to_string(++sequence);
        Ptr result(new MemoryFrame);
        const uint64_t size = sizeof(Header) + bytes;
        result->handle_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(size >> 32), DWORD(size), name.c_str());
        if (!result->handle_ || GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("Cannot create unique frame mapping");
        result->header_ = static_cast<Header*>(MapViewOfFile(result->handle_, FILE_MAP_ALL_ACCESS, 0, 0, SIZE_T(size)));
        if (!result->header_) throw std::runtime_error("Cannot map frame pixels");
        *result->header_ = Header{0x32464b57, 2, uint32_t(width), uint32_t(height), uint32_t(width * 3), 0, bytes, {}};
        result->uri_ = "fkwframe:" + name;
        return result;
#else
        throw std::runtime_error("Shared video frames require Windows");
#endif
    }
    static Ptr open(const std::string& uri) {
#ifdef _WIN32
        if (!isMemory(uri) || uri.size() > 240) throw std::runtime_error("Invalid frame mapping name");
        Ptr result(new MemoryFrame);
        result->handle_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, uri.substr(9).c_str());
        if (!result->handle_) throw std::runtime_error("Frame mapping is no longer available");
        auto* header = static_cast<Header*>(MapViewOfFile(result->handle_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Header)));
        if (!header) throw std::runtime_error("Cannot read frame header");
        const Header copy = *header; UnmapViewOfFile(header);
        if (copy.magic != 0x32464b57 || copy.version != 2 || copy.stride != copy.width * 3 ||
            copy.bytes != pixelBytes(int(copy.width), int(copy.height))) throw std::runtime_error("Invalid frame mapping header");
        result->header_ = static_cast<Header*>(MapViewOfFile(result->handle_, FILE_MAP_ALL_ACCESS, 0, 0, SIZE_T(sizeof(Header) + copy.bytes)));
        if (!result->header_) throw std::runtime_error("Truncated frame mapping");
        result->uri_ = uri; return result;
#else
        throw std::runtime_error("Shared video frames require Windows");
#endif
    }
    ~MemoryFrame() {
#ifdef _WIN32
        if (header_) UnmapViewOfFile(header_);
        if (handle_) CloseHandle(handle_);
#endif
    }
    MemoryFrame(const MemoryFrame&) = delete;
    MemoryFrame& operator=(const MemoryFrame&) = delete;
    int width() const { return int(header_->width); }
    int height() const { return int(header_->height); }
    size_t bytes() const { return size_t(header_->bytes); }
    unsigned char* data() const { return reinterpret_cast<unsigned char*>(header_ + 1); }
    const std::string& uri() const { return uri_; }
    void publish() {
#ifdef _WIN32
        InterlockedExchange(&header_->ready, 1);
#endif
    }
    bool ready() const {
#ifdef _WIN32
        return InterlockedCompareExchange(&header_->ready, 1, 1) == 1;
#else
        return false;
#endif
    }
private:
    MemoryFrame() = default;
    struct Header {
        uint32_t magic, version, width, height, stride;
#ifdef _WIN32
        volatile LONG ready;
#else
        int32_t ready;
#endif
        uint64_t bytes;
        unsigned char reserved[32];
    };
    static_assert(sizeof(Header) == 64, "Stable transport header");
    Header* header_ = nullptr;
    std::string uri_;
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#endif
};
using FramePtr = MemoryFrame::Ptr;
}
