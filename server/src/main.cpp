#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <stdexcept>
#include "server.hpp"
#include "world.hpp"
#include "../../shared/include/logger.hpp"
#include "../../shared/include/thread_pool.hpp"

static std::atomic<bool> g_running{true};
static GameServer *g_server{nullptr};

void signalHandler(int sig) {
    LOG_INFO("Signal " + std::to_string(sig) + " received — shutting down");
    g_running = false;
    if (g_server) g_server->stop();
}

constexpr int TPS = 50;
constexpr double TICK_MS = 1000.0 / TPS;
constexpr float TICK_DT = 1.0f / TPS;

int main(int argc, char *argv[]) {
    uint16_t port = DEFAULT_PORT;
    if (argc >= 2) port = static_cast<uint16_t>(std::stoi(argv[1]));

    Logger::instance().init("server.log");
    LOG_INFO("Server start");

    GameServer server(port);
    g_server = &server;

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);

    try {
        server.start();
    } catch (const std::exception &e) {
        LOG_ERR("Invalid server start: " + std::string(e.what()));
        return 1;
    }
    World world;

    server.onInput = [&world](uint8_t flags, float ax, float ay) {
        world.applyInput(flags, ax, ay);
    };
    ThreadPool pool(2);

    auto netFuture = pool.enqueue([&] {
        while (g_running) {
            server.pollOnce(5);
        }
    });

    auto tickFuture = pool.enqueue([&] {
        using Clock = std::chrono::steady_clock;
        auto nextTick = Clock::now();

        while (g_running) {
            auto now = Clock::now();
            if (now < nextTick) {
                std::this_thread::sleep_until(nextTick);
            }
            nextTick += std::chrono::microseconds(static_cast<long>(TICK_MS * 1000));

            world.update(TICK_DT);

            auto events = world.drainEvents();
            for (const auto &ev: events) {
                S2C_EventPacket epkt{};
                epkt.kind = ev.kind;
                epkt.entityId = ev.entityId;
                epkt.x = ev.x;
                epkt.y = ev.y;
                server.broadcastEvent(epkt);

                if (ev.kind == EventKind::WAVE_START)
                    LOG_INFO("Wave started — clients: " + std::to_string(server.clientCount()));
                else if (ev.kind == EventKind::GAME_OVER)
                    LOG_INFO("Game over! Respawning in 3s…");
                else if (ev.kind == EventKind::LIFE_DROP)
                    LOG_INFO("Life dropped at (" + std::to_string((int)ev.x) + "," +
                    std::to_string((int)ev.y) + ")");
            }

            static int respawnCountdown = 0;
            if (world.isGameOver()) {
                ++respawnCountdown;
                if (respawnCountdown >= TPS * 3) {
                    world.respawnPlayer();
                    respawnCountdown = 0;
                    LOG_INFO("Player respawned — new game");
                }
            } else {
                respawnCountdown = 0;
            }

            auto state = world.buildStatePacket();
            server.broadcastState(state);
        }
    });

    LOG_INFO("Server running");
    netFuture.wait();
    tickFuture.wait();
    server.stop();
    Logger::instance().shutdown();
    std::cout << "Server closed.\n";
    return 0;
}
