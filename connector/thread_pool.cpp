#include "thread_pool.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>
#include <vector>

class ThreadPoolManagerImpl final {
   public:
    ThreadPoolManagerImpl() {
        const std::size_t worker_count =
            std::max<std::size_t>(1, std::thread::hardware_concurrency());
        workers_.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i) {
            workers_.emplace_back([this]() { workerLoop(); });
        }
    }

    ~ThreadPoolManagerImpl() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

   private:
    void workerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this]() {
                    return stopping_ || !tasks_.empty();
                });
                if (stopping_ && tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
};

ThreadPoolManager::ThreadPoolManager()
    : impl_(std::make_unique<ThreadPoolManagerImpl>()) {}

ThreadPoolManager::~ThreadPoolManager() = default;

void ThreadPoolManager::post(std::function<void()> task) {
    impl_->post(std::move(task));
}
