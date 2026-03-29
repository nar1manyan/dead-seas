#pragma once
#include <string>
#include <fstream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>

class Logger {
public:
    static Logger &instance() {
        static Logger inst;
        return inst;
    }

    void init(const std::string &filename) {
        file_.open(filename, std::ios::app);
        worker_ = std::thread(&Logger::workerLoop, this);
    }

    void shutdown() {
        running_ = false;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        if (file_.is_open()) file_.close();
    }

    enum class Level { DEBUG, INFO, WARN, ERR };

    void log(Level lvl, const std::string &msg) {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << "[" << std::put_time(std::localtime(&t), "%H:%M:%S") << "] "
                << levelStr(lvl) << " " << msg;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push(oss.str());
        }
        cv_.notify_one();
    }

    // log
    void info(const std::string &m) { log(Level::INFO, m); }
    void warn(const std::string &m) { log(Level::WARN, m); }
    void error(const std::string &m) { log(Level::ERR, m); }
    void debug(const std::string &m) { log(Level::DEBUG, m); }

private:
    Logger() = default;

    ~Logger() { if (running_) shutdown(); }

    void workerLoop() {
        while (running_ || !queue_.empty()) {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&] { return !queue_.empty() || !running_; });
            while (!queue_.empty()) {
                std::string line = queue_.front();
                queue_.pop();
                lk.unlock();
                if (file_.is_open()) {
                    file_ << line << "\n";
                    file_.flush();
                }
                puts(line.c_str());
                lk.lock();
            }
        }
    }

    static const char *levelStr(Level l) {
        switch (l) {
            case Level::DEBUG: return "[DBG]";
            case Level::INFO: return "[INFO]";
            case Level::WARN: return "[WARN]";
            case Level::ERR: return "[ERR]";
        }
        return "[invalid type]";
    }

    std::ofstream file_;
    std::queue<std::string> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> running_{true};
};

#define LOG_INFO(msg)  Logger::instance().info(msg)
#define LOG_WARN(msg)  Logger::instance().warn(msg)
#define LOG_ERR(msg)   Logger::instance().error(msg)
#define LOG_DBG(msg)   Logger::instance().debug(msg)
