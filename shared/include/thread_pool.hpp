#pragma once
#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <stdexcept>
#include <atomic>

class ThreadPool {
public:
    explicit ThreadPool(size_t threads) : stop_(false) {
        for (size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lk(mtx_);
                        cv_.wait(lk, [this] { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
    }

    template<class F, class... Args>
    auto enqueue(F &&f, Args &&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type> {
        using RetType = typename std::invoke_result<F, Args...>::type;
        auto task = std::make_shared<std::packaged_task<RetType()> >(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<RetType> res = task->get_future();
        {
            std::unique_lock<std::mutex> lk(mtx_);
            if (stop_) throw std::runtime_error("ThreadPool stopped");
            tasks_.emplace([task] { (*task)(); });
        }
        cv_.notify_one();
        return res;
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &w: workers_) w.join();
    }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()> > tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_;
};
