// Persistent, bounded batch protocol around upstream RIFE (see third_party).
#include <windows.h>
#include <objbase.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <map>
#include <memory>
#include <vector>
#include <cmath>
#include "gpu.h"
#include "rife.h"
#include "../video_frames/worker_images.h"
#include "../video_frames/worker_pipeline.h"

namespace {
std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> result;
    std::istringstream stream(line); std::string item;
    while (std::getline(stream, item, '\t')) result.push_back(item);
    return result;
}
struct Prepared {
    ncnn::Mat left, right; float time; std::string output;
    size_t bytes() const { return size_t(left.w) * left.h * 6; }
};
struct Result {
    ncnn::Mat image; std::string output;
    size_t bytes() const { return size_t(image.w) * image.h * 3; }
};
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--version") { std::cout << "FKW_RIFE_BRIDGE_1\n"; return 0; }
    if (argc != 6) { std::cerr << "Expected model-directory gpu-id spatial-tta temporal-tta uhd\n"; return 2; }
    if (!std::filesystem::is_regular_file(std::filesystem::path(argv[1]) / L"flownet.param") ||
        !std::filesystem::is_regular_file(std::filesystem::path(argv[1]) / L"flownet.bin")) return 3;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    fkw::frames::fastPng();
    ncnn::create_gpu_instance();
    int exitCode = 0;
    try {
        const int requested = _wtoi(argv[2]);
        const int device = requested < 0 ? ncnn::get_default_gpu_index() : requested;
        if (device < 0 || device >= ncnn::get_gpu_count()) throw std::runtime_error("No matching Vulkan GPU");
        std::cerr << "RIFE GPU " << device << ": " << ncnn::get_gpu_info(device).device_name() << '\n';
        RIFE rife(device, _wtoi(argv[3]) != 0, _wtoi(argv[4]) != 0, _wtoi(argv[5]) != 0, 1, false, true);
        if (rife.load(argv[1]) != 0) throw std::runtime_error("Cannot load RIFE model");
        std::cout << "READY\n" << std::flush;
        std::string command;
        while (std::getline(std::cin, command)) {
            if (!command.empty() && command.back() == '\r') command.pop_back();
            if (command == "QUIT") break;
            if (command.rfind("BATCH\t", 0) != 0) throw std::runtime_error("Unknown bridge command");
            std::ifstream manifest(std::filesystem::u8path(command.substr(6)));
            if (!manifest) throw std::runtime_error("Cannot read batch manifest");
            std::vector<std::vector<std::string>> jobs;
            std::string row;
            while (std::getline(manifest, row)) {
                if (!row.empty() && row.back() == '\r') row.pop_back();
                if (row.empty()) continue;
                auto job = fields(row);
                if (job.size() != 4 || jobs.size() >= 4096) throw std::runtime_error("Invalid or oversized interpolation batch");
                const float time = std::stof(job[0]);
                if (!std::isfinite(time) || time <= 0 || time >= 1) throw std::runtime_error("Invalid interpolation timestamp");
                if (!fkw::frames::MemoryFrame::isMemory(job[3]) && std::filesystem::exists(std::filesystem::u8path(job[3]))) throw std::runtime_error("Output already exists");
                jobs.push_back(std::move(job));
            }
            if (!manifest.eof()) throw std::runtime_error("Cannot finish reading manifest");
            std::map<std::string, ncnn::Mat> cache;
            fkw::frames::runBatch<Prepared, Result>(jobs.size(), 1,
                [&](size_t index) {
                    const auto& job = jobs[index];
                    for (auto it = cache.begin(); it != cache.end();) {
                        if (it->first != job[1] && it->first != job[2]) it = cache.erase(it); else ++it;
                    }
                    for (const auto& name : {job[1], job[2]}) if (!cache.count(name)) cache.emplace(name, fkw::frames::readBgr(name));
                    const auto& left = cache.at(job[1]); const auto& right = cache.at(job[2]);
                    if (left.w != right.w || left.h != right.h) throw std::runtime_error("Input sizes differ");
                    return Prepared{left, right, std::stof(job[0]), job[3]};
                },
                [&](Prepared input) {
                    Result output{ncnn::Mat(input.left.w, input.left.h, size_t(3), 3), input.output};
                    if (rife.process(input.left, input.right, input.time, output.image) || output.image.empty()) throw std::runtime_error("RIFE inference failed");
                    return output;
                },
                [&](Result output) { fkw::frames::storeBgr(output.output, output.image); fkw::frames::completedFrame(); });
            std::cout << "BATCH_DONE\n" << std::flush;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; std::cout << "ERROR\n" << std::flush; exitCode = 1;
    }
    ncnn::destroy_gpu_instance(); CoUninitialize(); return exitCode;
}

