#pragma once
#include "ecs/registry.hpp"
#include "ecs/components.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace systems {
    using namespace components;
    using namespace ecs;

    inline void movement_system(Registry &reg, float dt) {
        reg.view<Position, Velocity>([&](Entity, Position &pos, Velocity &vel) {
            pos.x += vel.vx * dt;
            pos.y += vel.vy * dt;
            pos.x = std::clamp(pos.x, 0.f, 2048.f);
            pos.y = std::clamp(pos.y, 0.f, 2048.f);
        });
    }

    inline constexpr float ZOMBIE_SPEED = 60.f;
    inline constexpr float ZOMBIE_STOP_RANGE = 16.f;

    inline void ai_system(Registry &reg, float /*dt*/) {
        struct PlayerPos {
            float x, y;
        };
        std::vector<PlayerPos> players;
        players.reserve(64);

        reg.view<Position, PlayerTag, Health>(
            [&](Entity, Position &p, PlayerTag &, Health &h) {
                if (!h.dead()) players.push_back({p.x, p.y});
            });

        if (players.empty()) {
            reg.view<Velocity, ZombieTag>([](Entity, Velocity &v, ZombieTag &) {
                v.vx = v.vy = 0.f;
            });
            return;
        }

        reg.view<Position, Velocity, ZombieTag, Health>(
            [&](Entity, Position &pos, Velocity &vel, ZombieTag &, Health &h) {
                if (h.dead()) {
                    vel.vx = vel.vy = 0.f;
                    return;
                }

                float best_dist2 = std::numeric_limits<float>::max();
                float tx = 0.f, ty = 0.f;

                for (auto &pp: players) {
                    float dx = pp.x - pos.x;
                    float dy = pp.y - pos.y;
                    float d2 = dx * dx + dy * dy;
                    if (d2 < best_dist2) {
                        best_dist2 = d2;
                        tx = pp.x;
                        ty = pp.y;
                    }
                }

                float dist = std::sqrt(best_dist2);
                if (dist < ZOMBIE_STOP_RANGE) {
                    vel.vx = vel.vy = 0.f;
                } else {
                    float inv = ZOMBIE_SPEED / dist;
                    vel.vx = (tx - pos.x) * inv;
                    vel.vy = (ty - pos.y) * inv;
                }
            });
    }

    inline constexpr float MELEE_RANGE = 20.f;
    inline constexpr int ZOMBIE_DAMAGE = 5;
    inline constexpr float ATTACK_COOLDOWN = 1.0f;

#include <unordered_map>

    inline void combat_system(Registry &reg, float dt) {
        static std::unordered_map<Entity, float> cooldowns;

        struct PData {
            Entity e;
            float x, y;
        };
        std::vector<PData> players;

        reg.view<Position, PlayerTag, Health>(
            [&](Entity e, Position &p, PlayerTag &, Health &h) {
                if (!h.dead()) players.push_back({e, p.x, p.y});
            });

        reg.view<Position, ZombieTag, Health>(
            [&](Entity ze, Position &zp, ZombieTag &, Health &zh) {
                if (zh.dead()) return;

                float &cd = cooldowns[ze];
                cd -= dt;
                if (cd > 0.f) return;

                for (auto &pp: players) {
                    float dx = pp.x - zp.x;
                    float dy = pp.y - zp.y;
                    if (dx * dx + dy * dy < MELEE_RANGE * MELEE_RANGE) {
                        auto &ph = reg.get<Health>(pp.e);
                        ph.hp -= ZOMBIE_DAMAGE;
                        cd = ATTACK_COOLDOWN;
                        break;
                    }
                }
            });
    }
}
