// Bounded, persistent batch protocol around the upstream Real-ESRGAN Vulkan core.
#include <windows.h>
#include <objbase.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>
#include <stdexcept>
#include <omp.h>
#include "realesrgan.h"
#include "../video_frames/worker_images.h"
#include "../video_frames/worker_pipeline.h"

namespace {
struct Job { std::filesystem::path input, output; };
struct Prepared {
    Job job; ncnn::Mat image;
    size_t bytes() const { return size_t(image.w) * image.h * 3; }
};
struct Result {
    Job job; ncnn::Mat image;
    size_t bytes() const { return size_t(image.w) * image.h * 3; }
};
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--version") { std::cout << "FKW_SUPERRES_WORKER_1\n"; return 0; }
    if (argc != 7) { std::cerr << "Expected models-directory model-name scale gpu-id tile tta\n"; return 2; }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) return 3;
    fkw::frames::fastPng();
    ncnn::create_gpu_instance();
    int status = 0;
    try {
        const int scale = std::stoi(argv[3]), requested = std::stoi(argv[4]), tile = std::stoi(argv[5]);
        const std::wstring model = argv[2];
        const bool animeVideo = model == L"realesr-animevideov3";
        const bool aniSd = model == L"2x_AniSD_G6i1_SPAN_215K";
        const bool realEsrgan = model == L"realesrgan-x4plus" || model == L"realesrgan-x4plus-anime";
        if (scale < 2 || scale > 4 || (tile != 0 && tile < 32) ||
            !(animeVideo || (aniSd && scale == 2) || (realEsrgan && scale == 4)))
            throw std::runtime_error("Invalid model parameters");
        const int device = requested < 0 ? ncnn::get_default_gpu_index() : requested;
        if (device < 0 || device >= ncnn::get_gpu_count()) throw std::runtime_error("No matching Vulkan GPU");
        std::cerr << "Superres GPU " << device << ": " << ncnn::get_gpu_info(device).device_name() << '\n';
        std::filesystem::path stem = std::filesystem::path(argv[1]) / (animeVideo ? model + L"-x" + std::to_wstring(scale) : model);
        auto param = stem; param += L".param"; auto weights = stem; weights += L".bin";
        if (!std::filesystem::is_regular_file(param) || !std::filesystem::is_regular_file(weights)) throw std::runtime_error("Model files missing");
        RealESRGAN engine(device, std::stoi(argv[6]) != 0);
        const uint32_t budget = ncnn::get_gpu_device(device)->get_heap_budget();
        engine.scale = scale;
        // SPAN has a longer receptive field than AnimeVideoV3. Give adjacent tiles enough overlap.
        engine.prepadding = aniSd ? 24 : 10;
        engine.tilesize = tile ? tile : budget > 1900 ? 200 : budget > 550 ? 100 : budget > 190 ? 64 : 32;
        if (engine.load(param.wstring(), weights.wstring()) != 0) throw std::runtime_error("Cannot load model or shaders");
        const int threads = std::min(2, int(ncnn::get_gpu_info(device).compute_queue_count()));
        std::cout << "READY\n" << std::flush;
        std::string command;
        while (std::getline(std::cin, command)) {
            if (!command.empty() && command.back() == '\r') command.pop_back();
            if (command == "QUIT") break;
            if (command.rfind("BATCH\t", 0) != 0) throw std::runtime_error("Unknown worker command");
            std::ifstream manifest(std::filesystem::u8path(command.substr(6)));
            if (!manifest) throw std::runtime_error("Cannot read batch manifest");
            std::vector<Job> jobs;
            std::string row;
            while (std::getline(manifest, row)) {
                if (!row.empty() && row.back() == '\r') row.pop_back();
                const size_t split = row.find('\t');
                if (split == std::string::npos || jobs.size() >= 256) throw std::runtime_error("Invalid or oversized batch");
                Job job{std::filesystem::u8path(row.substr(0, split)), std::filesystem::u8path(row.substr(split + 1))};
                if ((!fkw::frames::MemoryFrame::isMemory(job.input.u8string()) && !std::filesystem::is_regular_file(job.input)) || (!fkw::frames::MemoryFrame::isMemory(job.output.u8string()) && std::filesystem::exists(job.output))) throw std::runtime_error("Input missing or output already exists");
                jobs.push_back(std::move(job));
            }
            if (!manifest.eof() || jobs.empty()) throw std::runtime_error("Empty or unreadable batch");
            fkw::frames::runBatch<Prepared, Result>(jobs.size(), threads,
                [&](size_t index) { return Prepared{jobs[index], fkw::frames::readBgr(jobs[index].input.u8string())}; },
                [&](Prepared input) {
                    Result output{input.job, ncnn::Mat(input.image.w * scale, input.image.h * scale, size_t(3), 3)};
                    if (engine.process(input.image, output.image) || output.image.empty()) throw std::runtime_error("Superres inference failed");
                    return output;
                },
                [&](Result output) { fkw::frames::storeBgr(output.job.output.u8string(), output.image); fkw::frames::completedFrame(); });
            std::cout << "BATCH_DONE\n" << std::flush;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; std::cout << "ERROR\n" << std::flush; status = 1;
    }
    ncnn::destroy_gpu_instance(); CoUninitialize(); return status;
}

