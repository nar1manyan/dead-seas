#pragma once
#include "ecs/registry.hpp"
#include "ecs/components.hpp"
#include "systems/systems.hpp"
#include "network/packets.hpp"
#include "utils/logger.hpp"
#include <algorithm>
#include <cstdlib>
#include <cmath>

class World {
public:
    World() { spawn_zombies(20); }

    ecs::Registry &registry() { return reg_; }

    void update(float dt) {
        systems::ai_system(reg_, dt);
        systems::movement_system(reg_, dt);
        systems::combat_system(reg_, dt);
        cleanup_dead();
        ++tick_;
    }

    void apply_input(ecs::Entity e, const net::InputPayload &inp) {
        if (!reg_.has<components::Velocity>(e)) return;
        if (!reg_.has<components::Health>(e)) return;
        if (reg_.get<components::Health>(e).dead()) return;

        auto &vel = reg_.get<components::Velocity>(e);

        float dx = std::clamp(inp.dx, -1.f, 1.f);
        float dy = std::clamp(inp.dy, -1.f, 1.f);

        float len2 = dx * dx + dy * dy;
        if (len2 > 1.f) {
            float inv = 1.f / std::sqrt(len2);
            dx *= inv;
            dy *= inv;
        }

        constexpr float PLAYER_SPEED = 120.f;
        vel.vx = dx * PLAYER_SPEED;
        vel.vy = dy * PLAYER_SPEED;
    }

    ecs::Entity spawn_player(int client_fd) {
        ecs::Entity e = reg_.create();
        reg_.emplace<components::Position>(e, 512.f + (float) (rand() % 256), 512.f + (float) (rand() % 256));
        reg_.emplace<components::Velocity>(e);
        reg_.emplace<components::Health>(e);
        reg_.emplace<components::PlayerTag>(e);
        reg_.emplace<components::NetworkId>(e, client_fd);
        LOG_INFO("Player entity %u spawned (fd=%d)", e, client_fd);
        return e;
    }

    void despawn_player(ecs::Entity e) {
        reg_.destroy(e);
        LOG_INFO("Player entity %u despawned", e);
    }

    uint32_t tick() const { return tick_; }

    std::vector<uint8_t> build_snapshot() const {
        std::vector<net::EntityState> states;
        states.reserve(256);

        auto collect = [&]<typename TagT>(uint8_t flag_bit) {
            auto &r = const_cast<ecs::Registry &>(reg_);
            r.view<components::Position, components::Health, TagT>(
                [&](ecs::Entity e, components::Position &p,
                    components::Health &h, TagT &) {
                    net::EntityState s{};
                    s.entity_id = e;
                    s.x = p.x;
                    s.y = p.y;
                    s.hp = static_cast<int16_t>(std::clamp(h.hp, -32768, 32767));
                    s.flags = flag_bit;
                    if (h.dead()) s.flags |= 0x04;
                    states.push_back(s);
                });
        };

        collect.template operator()<components::PlayerTag>(0x01);
        collect.template operator()<components::ZombieTag>(0x02);

        net::PacketWriter pw(net::PacketId::SNAPSHOT);
        net::SnapshotHeader hdr{};
        hdr.tick = tick_;
        hdr.entity_count = static_cast<uint16_t>(states.size());
        pw.write(hdr);
        for (auto &s: states) pw.write(s);

        auto view = pw.finalise();
        return {view.begin(), view.end()};
    }

private:
    void spawn_zombies(int count) {
        for (int i = 0; i < count; ++i) {
            ecs::Entity e = reg_.create();
            reg_.emplace<components::Position>(e,
                                               (float) (rand() % 2000 + 24),
                                               (float) (rand() % 2000 + 24));
            reg_.emplace<components::Velocity>(e);
            reg_.emplace<components::Health>(e, 50, 50);
            reg_.emplace<components::ZombieTag>(e);
        }
    }

    void cleanup_dead() {
        std::vector<ecs::Entity> to_remove;
        reg_.view<components::Health, components::ZombieTag>(
            [&](ecs::Entity e, components::Health &h, components::ZombieTag &) {
                if (h.dead()) to_remove.push_back(e);
            });
        for (ecs::Entity e: to_remove) {
            reg_.destroy(e);
            spawn_zombies(1);
        }
    }

    ecs::Registry reg_;
    uint32_t tick_ = 0;
};
