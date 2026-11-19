#pragma once
#include "frame_memory.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <vector>
#include "mat.h"
#include "wic_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "../../third_party/realesrgan-ncnn-vulkan/stb_image_write.h"

namespace fkw::frames {
class ComScope {
public:
    ComScope() { status_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED); if (FAILED(status_)) throw std::runtime_error("Cannot initialize image codec"); }
    ~ComScope() { CoUninitialize(); }
private:
    HRESULT status_;
};
inline ncnn::Mat readBgr(const std::string& path) {
    if (MemoryFrame::isMemory(path)) {
        const auto frame = MemoryFrame::open(path);
        if (!frame->ready()) throw std::runtime_error("Input frame is incomplete");
        ncnn::Mat result(frame->width(), frame->height(), size_t(3), 3);
        auto* dest = static_cast<unsigned char*>(result.data); const auto* src = frame->data();
        for (size_t i = 0; i < frame->bytes(); i += 3) { dest[i] = src[i+2]; dest[i+1] = src[i+1]; dest[i+2] = src[i]; }
        return result;
    }
    ComScope com;
    int width = 0, height = 0, channels = 0;
    std::unique_ptr<unsigned char, decltype(&free)> pixels(wic_decode_image(std::filesystem::u8path(path).c_str(), &width, &height, &channels), &free);
    if (!pixels || width <= 0 || height <= 0 || (channels != 3 && channels != 4)) throw std::runtime_error("Cannot decode input image");
    ncnn::Mat result(width, height, size_t(3), 3);
    auto* dst = static_cast<unsigned char*>(result.data);
    for (size_t i = 0; i < size_t(width) * height; ++i) memcpy(dst + i * 3, pixels.get() + i * channels, 3);
    return result;
}
inline void storeBgr(const std::string& path, const ncnn::Mat& image) {
    const size_t bytes = MemoryFrame::pixelBytes(image.w, image.h);
    const auto* src = static_cast<const unsigned char*>(image.data);
    FramePtr memory;
    std::vector<unsigned char> temporary;
    if (MemoryFrame::isMemory(path)) {
        memory = MemoryFrame::open(path);
        if (memory->width() != image.w || memory->height() != image.h || memory->ready()) throw std::runtime_error("Invalid result mapping");
    } else temporary.resize(bytes);
    auto* rgb = memory ? memory->data() : temporary.data();
    for (size_t i = 0; i < bytes; i += 3) { rgb[i] = src[i+2]; rgb[i+1] = src[i+1]; rgb[i+2] = src[i]; }
    if (memory) { memory->publish(); return; }
    std::ofstream output(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot create result PNG");
    // Configured once before starting worker threads; pixels remain lossless.
    const auto write = [](void* context, void* data, int size) { static_cast<std::ofstream*>(context)->write(static_cast<const char*>(data), size); };
    if (!stbi_write_png_to_func(write, &output, image.w, image.h, 3, rgb, image.w * 3)) throw std::runtime_error("PNG encoding failed");
    output.close(); if (!output) throw std::runtime_error("PNG write failed");
}
inline void fastPng() { stbi_write_png_compression_level = 1; stbi_write_force_png_filter = 0; }
inline void completedFrame() { static std::mutex io; std::lock_guard<std::mutex> lock(io); std::cout << "DONE\n" << std::flush; }
}
