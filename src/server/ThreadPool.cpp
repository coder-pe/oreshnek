// oreshnek/src/server/ThreadPool.cpp
#include "oreshnek/server/ThreadPool.h"
#include <iostream> // For debugging

namespace Oreshnek {
namespace Server {

ThreadPool::ThreadPool(size_t threads) : stop_(false) {
    if (threads == 0) {
        threads = 1; // At least one thread
    }
    for (size_t i = 0; i < threads; ++i) {
        workers_.emplace_back([this] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex_);
                    condition_.wait(lock, [this] { return stop_ || !tasks_.empty(); });

                    if (stop_ && tasks_.empty()) {
                        return; // Exit thread
                    }
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task(); // Execute the task
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_ = true;
    }
    condition_.notify_all(); // Wake up all waiting threads

    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join(); // Wait for each thread to finish
        }
    }
    std::cout << "ThreadPool shutdown complete." << std::endl;
}

} // namespace Server
} // namespace Oreshnek
