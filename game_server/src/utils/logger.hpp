#pragma once
#include <atomic>
#include <thread>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <memory>

namespace utils {
    struct LogNode {
        std::string msg;
        LogNode *next = nullptr;
    };

    class Logger {
    public:
        static Logger &instance() {
            static Logger inst;
            return inst;
        }

        void start() {
            running_.store(true, std::memory_order_release);
            thread_ = std::thread([this] { drain_loop(); });
        }

        void stop() {
            running_.store(false, std::memory_order_release);
            if (thread_.joinable()) thread_.join();
            flush();
        }

        void log(const char *level, const char *fmt, ...) {
            char buf[512];
            va_list args;
            va_start(args, fmt);
            vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);

            char ts[32];
            timestamp(ts);

            auto *node = new LogNode{};
            node->msg.reserve(256);
            node->msg = std::string(ts) + " [" + level + "] " + buf + "\n";

            push(node);
        }

    private:
        Logger() {
            auto *dummy = new LogNode{};
            head_.store(dummy, std::memory_order_relaxed);
            tail_ = dummy;
        }

        ~Logger() { stop(); }

        void push(LogNode *node) {
            node->next = nullptr;
            LogNode *prev = head_.exchange(node, std::memory_order_acq_rel);
            prev->next = node;
        }

        void flush() {
            LogNode *cur = tail_->next;
            while (cur) {
                fputs(cur->msg.c_str(), stdout);
                LogNode *tmp = cur;
                cur = cur->next;
                tail_ = tmp;
            }
            fflush(stdout);
        }

        void drain_loop() {
            while (running_.load(std::memory_order_acquire)) {
                flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            flush();
        }

        void timestamp(char *buf) {
            timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            time_t sec = ts.tv_sec;
            tm tm_info;
            localtime_r(&sec, &tm_info);
            snprintf(buf, 32, "%02d:%02d:%02d.%03ld",
                     tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
                     ts.tv_nsec / 1000000);
        }

        alignas(64) std::atomic<LogNode *> head_;
        alignas(64) LogNode *tail_;

        std::atomic<bool> running_{false};
        std::thread thread_;
    };
}

#define LOG_INFO(fmt,  ...) utils::Logger::instance().log("INFO ", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt,  ...) utils::Logger::instance().log("WARN ", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) utils::Logger::instance().log("ERROR", fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) utils::Logger::instance().log("DEBUG", fmt, ##__VA_ARGS__)
