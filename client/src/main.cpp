#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <string>
#include <csignal>
#include "net_client.hpp"
#include "renderer.hpp"
#include "../../shared/include/logger.hpp"
#include "../../shared/include/protocol.hpp"

struct SharedState {
    std::mutex mtx;
    StatePayload latest{};
    bool hasState{false};
    std::vector<S2C_EventPacket> events;
    bool welcomed{false};
    bool connected{false};
};

static std::atomic<bool> g_running{true};

void sigHandler(int) { g_running = false; }

//network
void networkThread(NetClient &client, SharedState &shared,
                   const std::string &host, uint16_t port, const char *name) {
    while (g_running) {
        if (!client.connect(host, port)) {
            LOG_WARN("Invalid connection. Retrying…");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        client.sendJoin(name);
        {
            std::lock_guard<std::mutex> lk(shared.mtx);
            shared.connected = true;
        }

        ServerMessages msgs;
        while (g_running && client.isConnected()) {
            msgs.events.clear();
            msgs.hasState = false;
            msgs.welcomed = false;

            if (!client.poll(msgs)) break;

            std::lock_guard<std::mutex> lk(shared.mtx);
            if (msgs.welcomed) shared.welcomed = true;
            if (msgs.hasState) {
                shared.latest = msgs.state.payload;
                shared.hasState = true;
            }
            for (auto &ev: msgs.events)
                shared.events.push_back(ev);

            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }

        {
            std::lock_guard<std::mutex> lk(shared.mtx);
            shared.connected = false;
        }
        client.disconnect();
        LOG_WARN("Disconnected. Reconnecting in 2s…");
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

int main(int argc, char *argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = DEFAULT_PORT;
    const char *pname = "Pirate";

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = static_cast<uint16_t>(std::stoi(argv[2]));
    if (argc >= 4) pname = argv[3];

    Logger::instance().init("client.log");
    LOG_INFO("Client started");

    std::signal(SIGINT, sigHandler);
    std::signal(SIGTERM, sigHandler);
    std::signal(SIGPIPE, SIG_IGN);

    SharedState shared;
    NetClient client;

    //network start
    std::thread netThr(networkThread, std::ref(client), std::ref(shared),
                       host, port, pname);

    //render
    // #TODO Check render deps for performance

    Renderer renderer;
    if (!renderer.init(static_cast<unsigned>(Cfg::WORLD_W),
                       static_cast<unsigned>(Cfg::WORLD_H),
                       "Pirate vs Zombies")) {
        LOG_ERR("Failed to init renderer");
        g_running = false;
        netThr.join();
        return 1;
    }

    using Clock = std::chrono::steady_clock;
    auto prevTime = Clock::now();
    float accumDt = 0.f;
    constexpr float TICK_DT = 1.f / 50.f;

    //screen
    {
        bool welcomed = false;
        while (renderer.isOpen() && g_running && !welcomed) {
            sf::Event ev;
            while (renderer.window().pollEvent(ev)) {
                if (ev.type == sf::Event::Closed) {
                    g_running = false;
                    break;
                }
            }
            renderer.window().clear(sf::Color(20, 10, 30));
            sf::Text t;
            renderer.window().display();
            {
                std::lock_guard<std::mutex> lk(shared.mtx);
                welcomed = shared.welcomed;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    //game loop
    while (renderer.isOpen() && g_running) {
        sf::Event ev;
        while (renderer.window().pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) {
                g_running = false;
                break;
            }
            if (ev.type == sf::Event::KeyPressed &&
                ev.key.code == sf::Keyboard::Escape) {
                g_running = false;
                break;
            }
        }
        if (!g_running) break;

        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - prevTime).count();
        prevTime = now;
        dt = std::min(dt, 0.1f);

        std::lock_guard<std::mutex> lk(shared.mtx);
        if (shared.hasState) {
            renderer.updateFromState(shared.latest);
            shared.hasState = false;
        }
        for (const auto &sev: shared.events) {
            switch (sev.kind) {
                case EventKind::ZOMBIE_DIED:
                    renderer.addNotification("+10", sf::Color(255, 220, 60), sev.x, sev.y);
                    break;
                case EventKind::PLAYER_HIT:
                    renderer.addNotification("Uaaaaa!", sf::Color(255, 80, 80),
                                             Cfg::WORLD_W / 2.f - 30.f, 80.f);
                    break;
                case EventKind::LIFE_DROP:
                    renderer.addNotification("+1 LIFE!", sf::Color(60, 255, 120),
                                             sev.x, sev.y - 20.f);
                    break;
                case EventKind::WAVE_START:
                    renderer.addNotification("WAVE " + std::to_string(shared.latest.wave) + "!",
                                             sf::Color(255, 160, 30),
                                             Cfg::WORLD_W / 2.f - 50.f, Cfg::WORLD_H / 2.f - 40.f);
                    break;
                case EventKind::GAME_OVER:
                    renderer.addNotification("GAME OVER", sf::Color(220, 40, 40),
                                             Cfg::WORLD_W / 2.f - 60.f, Cfg::WORLD_H / 2.f + 60.f);
                    break;
                default: break;
            }
        }
        shared.events.clear();
        accumDt += dt;
        float alpha = std::min(1.f, accumDt / TICK_DT);
        if (accumDt >= TICK_DT) accumDt -= TICK_DT;
        renderer.setInterpolationAlpha(alpha);
        float aimX, aimY;
        uint8_t inputFlags = renderer.collectInput(aimX, aimY);
        client.sendInput(inputFlags, aimX, aimY);
        renderer.render(dt);
    }

    LOG_INFO("Client closed");
    g_running = false;
    client.disconnect();
    netThr.join();
    Logger::instance().shutdown();
    return 0;
}
