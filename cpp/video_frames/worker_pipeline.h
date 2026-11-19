#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace fkw::frames {
template<class T> class WorkerQueue {
public:
    bool push(T item, size_t bytes, const std::atomic<bool>& stop) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return closed_ || stop.load() ||
            (items_.size() < 2 && (items_.empty() || bytes_ + bytes <= 128 * 1024 * 1024)); });
        if (closed_ || stop.load()) return false;
        bytes_ += bytes; items_.emplace_back(std::move(item), bytes); changed_.notify_all(); return true;
    }
    bool pop(T& item, const std::atomic<bool>& stop) {
        std::unique_lock<std::mutex> lock(mutex_);
        changed_.wait(lock, [&] { return closed_ || stop.load() || !items_.empty(); });
        if (stop.load() || items_.empty()) return false;
        item = std::move(items_.front().first); bytes_ -= items_.front().second;
        items_.pop_front(); changed_.notify_all(); return true;
    }
    void close() { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; changed_.notify_all(); }
private:
    std::mutex mutex_; std::condition_variable changed_;
    std::deque<std::pair<T,size_t>> items_;
    size_t bytes_ = 0; bool closed_ = false;
};

// Input preparation, GPU work and result storage run independently. Each queue
// has both a frame count and byte limit. A failure wakes every producer/consumer.
template<class Input, class Output, class Read, class Infer, class Store>
void runBatch(size_t count, int gpuThreads, Read read, Infer infer, Store store) {
    WorkerQueue<Input> inputs; WorkerQueue<Output> outputs;
    std::atomic<bool> stopped{false}; std::atomic<int> computing{gpuThreads};
    std::mutex errorMutex; std::exception_ptr error;
    const auto fail = [&] {
        { std::lock_guard<std::mutex> lock(errorMutex); if (!error) error = std::current_exception(); }
        stopped.store(true); inputs.close(); outputs.close();
    };
    std::vector<std::thread> threads;
    const auto launch = [&](auto operation) {
        threads.emplace_back([&, operation] { try { operation(); } catch (...) { fail(); } });
    };
    try {
        launch([&] {
            for (size_t index = 0; index < count && !stopped.load(); ++index) {
                Input item = read(index); const size_t bytes = item.bytes();
                if (!inputs.push(std::move(item), bytes, stopped)) break;
            }
            inputs.close();
        });
        for (int i = 0; i < gpuThreads; ++i) launch([&] {
            Input input;
            while (inputs.pop(input, stopped)) {
                Output output = infer(std::move(input)); const size_t bytes = output.bytes();
                if (!outputs.push(std::move(output), bytes, stopped)) break;
            }
            if (--computing == 0) outputs.close();
        });
        for (int i = 0; i < 3; ++i) launch([&] { Output item; while (outputs.pop(item, stopped)) store(std::move(item)); });
    } catch (...) { fail(); }
    for (auto& thread : threads) if (thread.joinable()) thread.join();
    if (error) std::rethrow_exception(error);
}
}
