#include "server/world.hpp"
#include "server/server.hpp"
#include "server/game_loop.hpp"
#include "utils/logger.hpp"
#include "utils/thread_pool.hpp"

#include <csignal>
#include <atomic>
#include <cstdlib>
#include <cstdio>

static std::atomic<bool> g_shutdown{false};

static void sig_handler(int sig) {
    (void) sig;
    g_shutdown.store(true, std::memory_order_release);
}

int main(int argc, char *argv[]) {
    int port = 7777;
    if (argc >= 2) port = std::atoi(argv[1]);

    utils::Logger::instance().start();
    LOG_INFO("Server Working (port=%d)", port);

    struct sigaction sa{};
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    utils::ThreadPool pool(std::thread::hardware_concurrency());

    World world;

    server::Server srv(port, world, pool);
    server::GameLoop game_loop(world, srv);

    if (!srv.start()) {
        LOG_ERROR("Failed to start server");
        utils::Logger::instance().stop();
        return 1;
    }

    game_loop.start();

    LOG_INFO("Network loop");

    while (!g_shutdown.load(std::memory_order_acquire)) {
        srv.poll(/*timeout_ms=*/5);
    }

    LOG_INFO("Closed");

    game_loop.stop();
    srv.stop();
    pool.shutdown();

    LOG_INFO("Clean exit.");
    utils::Logger::instance().stop();
    return 0;
}
