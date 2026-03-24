#pragma once
#include "server.hpp"
#include "world.hpp"
#include "utils/logger.hpp"

#include <thread>
#include <atomic>
#include <chrono>

namespace server {
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::duration<double>;
    using TimePoint = Clock::time_point;

    inline constexpr double TICK_RATE = 50.0;
    inline constexpr double TICK_INTERVAL = 1.0 / TICK_RATE;

    class GameLoop {
    public:
        GameLoop(World &world, Server &srv) : world_(world), srv_(srv) {
        }

        void start() {
            running_.store(true, std::memory_order_release);
            thread_ = std::thread([this] { loop(); });
        }

        void stop() {
            running_.store(false, std::memory_order_release);
            if (thread_.joinable()) thread_.join();
        }

    private:
        void loop() {
            using ns = std::chrono::nanoseconds;
            using dsec = std::chrono::duration<double>;

            const ns TICK_NS = std::chrono::duration_cast<ns>(dsec(TICK_INTERVAL));
            const ns SPIN_THRESHOLD{800'000}; // 800µs

            TimePoint next_tick = Clock::now();

            uint64_t total_ticks = 0;
            double accumulator = 0.0;

            while (running_.load(std::memory_order_acquire)) {
                TimePoint now = Clock::now();
                double elapsed = dsec(now - next_tick).count() + TICK_INTERVAL;

                accumulator += elapsed;

                constexpr double MAX_ACCUMULATOR = TICK_INTERVAL * 3;
                if (accumulator > MAX_ACCUMULATOR) {
                    LOG_WARN("GameLoop lagging — dropping %.1f ms",
                             (accumulator - TICK_INTERVAL) * 1000.0);
                    accumulator = TICK_INTERVAL;
                }

                while (accumulator >= TICK_INTERVAL) {
                    tick(static_cast<float>(TICK_INTERVAL));
                    accumulator -= TICK_INTERVAL;
                    ++total_ticks;
                }

                next_tick = now + TICK_NS;
                TimePoint wake_target = next_tick - SPIN_THRESHOLD;

                if (Clock::now() < wake_target)
                    std::this_thread::sleep_until(wake_target);

                while (Clock::now() < next_tick);
            }

            LOG_INFO("GameLoop stopped after %llu ticks", (unsigned long long)total_ticks);
        }

        void tick(float dt) {
            srv_.flush_inputs_to_world();
            world_.update(dt);
            auto snapshot = world_.build_snapshot();
            srv_.broadcast(std::span<const uint8_t>(snapshot.data(), snapshot.size()));
        }

        World &world_;
        Server &srv_;

        std::atomic<bool> running_{false};
        std::thread thread_;
    };
}
