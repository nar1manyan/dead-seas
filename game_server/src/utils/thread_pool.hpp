#pragma once
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

namespace utils {
    class ThreadPool {
    public:
        explicit ThreadPool(std::size_t n) {
            workers_.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                workers_.emplace_back([this] { worker_loop(); });
            }
        }

        ~ThreadPool() { shutdown(); }

        void enqueue(std::function<void()> task) {
            {
                std::lock_guard lock(mu_);
                tasks_.push(std::move(task));
            }
            cv_.notify_one();
        }

        void shutdown() {
            {
                std::lock_guard lock(mu_);
                stop_ = true;
            }
            cv_.notify_all();
            for (auto &t: workers_)
                if (t.joinable()) t.join();
            workers_.clear();
        }

    private:
        void worker_loop() {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock lock(mu_);
                    cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                    if (stop_ && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        }

        std::vector<std::thread> workers_;
        std::queue<std::function<void()> > tasks_;
        std::mutex mu_;
        std::condition_variable cv_;
        bool stop_ = false;
    };
}
