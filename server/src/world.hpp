#pragma once
#include <vector>
#include <cmath>
#include <cstdint>
#include <random>
#include <functional>
#include <shared_mutex>
#include "protocol.hpp"

inline float dist(float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    return std::sqrt(dx * dx + dy * dy);
}

inline void normalize(float &x, float &y) {
    float len = std::sqrt(x * x + y * y);
    if (len > 0.0001f) { x /= len; y /= len; }
}

struct Zombie {
    uint32_t id{0};
    float x{0.f}, y{0.f};
    int hp{Cfg::ZOMBIE_HP};
    bool alive{true};
    float attackTimer{0.f};
    float speed{Cfg::ZOMBIE_SPEED_BASE};

    void update(float dt, float px, float py) {
        if (!alive) return;
        attackTimer = std::max(0.f, attackTimer - dt);
        float dx = px - x, dy = py - y;
        float d = dist(x, y, px, py);
        if (d < 2.f) return;
        normalize(dx, dy);
        x += dx * speed * dt;
        y += dy * speed * dt;
        x = std::max(Cfg::ZOMBIE_RADIUS, std::min(Cfg::WORLD_W - Cfg::ZOMBIE_RADIUS, x));
        y = std::max(Cfg::ZOMBIE_RADIUS, std::min(Cfg::WORLD_H - Cfg::ZOMBIE_RADIUS, y));
    }

    bool canAttack(float px, float py) const {
        return alive && attackTimer <= 0.f &&
               dist(x, y, px, py) <= Cfg::ZOMBIE_ATTACK_R + Cfg::PLAYER_RADIUS;
    }
};

struct Player {
    float x{Cfg::WORLD_W * 0.5f}, y{Cfg::WORLD_H * 0.5f};
    int lives{3};
    bool alive{true};
    float attackTimer{0.f};
    uint8_t inputFlags{0};
    float aimX{0.f}, aimY{0.f};

    void update(float dt) {
        if (!alive) return;
        attackTimer = std::max(0.f, attackTimer - dt);
        float dx = 0.f, dy = 0.f;
        if (inputFlags & InputFlag::MOVE_UP)    dy -= 1.f;
        if (inputFlags & InputFlag::MOVE_DOWN)  dy += 1.f;
        if (inputFlags & InputFlag::MOVE_LEFT)  dx -= 1.f;
        if (inputFlags & InputFlag::MOVE_RIGHT) dx += 1.f;
        normalize(dx, dy);
        x += dx * Cfg::PLAYER_SPEED * dt;
        y += dy * Cfg::PLAYER_SPEED * dt;
        x = std::max(Cfg::PLAYER_RADIUS, std::min(Cfg::WORLD_W - Cfg::PLAYER_RADIUS, x));
        y = std::max(Cfg::PLAYER_RADIUS, std::min(Cfg::WORLD_H - Cfg::PLAYER_RADIUS, y));
    }

    bool canAttack() const { return alive && attackTimer <= 0.f; }
};

struct WorldEvent {
    EventKind kind;
    uint32_t entityId{0};
    float x{0.f}, y{0.f};
};

class World {
public:
    World() : rng_(std::random_device{}()) {
        respawnPlayer();
    }

    void update(float dt) {
        std::unique_lock<std::shared_mutex> lk(mtx_);
        pendingEvents_.clear();
        waveTimer_ += dt;
        if (waveTimer_ >= Cfg::WAVE_INTERVAL) {
            waveTimer_ = 0.f;
            ++wave_;
            spawnWave_locked();
            pendingEvents_.push_back({EventKind::WAVE_START, 0, 0.f, 0.f});
        }

        if (!player_.alive) return;

        player_.update(dt);

        bool didAttack = (player_.inputFlags & InputFlag::ATTACK) && player_.canAttack();
        if (didAttack) {
            player_.attackTimer = Cfg::ATTACK_COOLDOWN;
            resolveAttack_locked();
        }

        for (auto &z: zombies_) {
            z.update(dt, player_.x, player_.y);
            if (z.canAttack(player_.x, player_.y)) {
                z.attackTimer = Cfg::ZOMBIE_ATTACK_CD;
                if (--player_.lives <= 0) {
                    player_.alive = false;
                    pendingEvents_.push_back({EventKind::GAME_OVER, 0, player_.x, player_.y});
                } else {
                    pendingEvents_.push_back({EventKind::PLAYER_HIT, 0, player_.x, player_.y});
                }
            }
        }

        for (auto it = zombies_.begin(); it != zombies_.end();) {
            if (!it->alive) ++it->deadFrames_;
            it = (it->deadFrames_ > 2) ? zombies_.erase(it) : ++it;
        }

        ++tick_;
    }

    void applyInput(uint8_t flags, float aimX, float aimY) {
        std::unique_lock<std::shared_mutex> lk(mtx_);
        player_.inputFlags = flags;
        player_.aimX = aimX;
        player_.aimY = aimY;
    }

    S2C_StatePacket buildStatePacket() const {
        std::shared_lock<std::shared_mutex> lk(mtx_);
        S2C_StatePacket pkt{};
        pkt.tick = tick_;
        pkt.payload.playerPos    = {player_.x, player_.y};
        pkt.payload.playerLives  = static_cast<int8_t>(player_.lives);
        pkt.payload.playerAlive  = player_.alive ? 1u : 0u;
        pkt.payload.score        = score_;
        pkt.payload.wave         = wave_;
        pkt.payload.killStreak   = killStreak_;
        uint8_t cnt = 0;
        for (const auto &z: zombies_) {
            if (cnt >= MAX_ZOMBIES) break;
            auto &s  = pkt.payload.zombies[cnt++];
            s.id     = z.id;
            s.pos    = {z.x, z.y};
            s.hp     = static_cast<uint8_t>(std::max(0, z.hp));
            s.alive  = z.alive ? 1u : 0u;
        }
        pkt.payload.zombieCount = cnt;
        return pkt;
    }

    std::vector<WorldEvent> drainEvents() {
        std::unique_lock<std::shared_mutex> lk(mtx_);
        std::vector<WorldEvent> out;
        out.swap(pendingEvents_);
        return out;
    }

    void respawnPlayer() {
        std::unique_lock<std::shared_mutex> lk(mtx_);
        player_ = Player{};
        zombies_.clear();
        wave_        = 1;
        score_       = 0;
        killStreak_  = 0;
        waveTimer_   = 0.f;
        tick_        = 0;
        spawnWave_locked();
    }

    bool isGameOver() const {
        std::shared_lock<std::shared_mutex> lk(mtx_);
        return !player_.alive;
    }

private:
    void spawnWave_locked() {
        int count = Cfg::ZOMBIES_PER_WAVE + static_cast<int>(wave_ - 1) * 2;
        count = std::min(count, MAX_ZOMBIES);
        float speedMult = 1.f + (wave_ - 1) * 0.08f;
        for (int i = 0; i < count; ++i) {
            ZombieExt z{};
            z.id    = ++nextZombieId_;
            z.speed = Cfg::ZOMBIE_SPEED_BASE * speedMult;
            spawnOnEdge_locked(z.x, z.y);
            zombies_.push_back(z);
        }
    }

    void spawnOnEdge_locked(float &ox, float &oy) {
        std::uniform_int_distribution<int> edge(0, 3);
        std::uniform_real_distribution<float> wx(0.f, Cfg::WORLD_W);
        std::uniform_real_distribution<float> wy(0.f, Cfg::WORLD_H);
        switch (edge(rng_)) {
            case 0: ox = wx(rng_); oy = 0.f;           break;
            case 1: ox = wx(rng_); oy = Cfg::WORLD_H;  break;
            case 2: ox = 0.f;      oy = wy(rng_);      break;
            default: ox = Cfg::WORLD_W; oy = wy(rng_); break;
        }
    }

    void resolveAttack_locked() {
        float aimDx = player_.aimX - player_.x;
        float aimDy = player_.aimY - player_.y;
        float aimLen = std::sqrt(aimDx * aimDx + aimDy * aimDy);
        if (aimLen < 0.001f) { aimDx = 1.f; aimDy = 0.f; }
        else                 { aimDx /= aimLen; aimDy /= aimLen; }

        const float cosHalf = std::cos((Cfg::ATTACK_ANGLE * 3.14159f) / 180.f);

        for (auto &z: zombies_) {
            if (!z.alive) continue;
            float d = dist(player_.x, player_.y, z.x, z.y);
            if (d > Cfg::ATTACK_RANGE + Cfg::ZOMBIE_RADIUS) continue;

            float zDx = z.x - player_.x, zDy = z.y - player_.y;
            float zLen = std::sqrt(zDx * zDx + zDy * zDy);
            if (zLen > 0.001f) { zDx /= zLen; zDy /= zLen; }
            if (aimDx * zDx + aimDy * zDy < cosHalf) continue;

            z.hp--;
            if (z.hp <= 0) {
                z.alive = false;
                score_ += 10 * wave_;
                ++killStreak_;
                pendingEvents_.push_back({EventKind::ZOMBIE_DIED, z.id, z.x, z.y});
                if (killStreak_ % Cfg::LIFE_DROP_KILLS == 0) {
                    std::uniform_real_distribution<float> roll(0.f, 1.f);
                    if (roll(rng_) < Cfg::LIFE_DROP_CHANCE &&
                        player_.lives < Cfg::PLAYER_MAX_LIVES) {
                        ++player_.lives;
                        pendingEvents_.push_back({EventKind::LIFE_DROP, 0, z.x, z.y});
                    }
                }
            }
        }
    }

    mutable std::shared_mutex mtx_;
    Player player_;

    struct ZombieExt : Zombie {
        int deadFrames_{0};
    };

    std::vector<ZombieExt> zombies_;
    uint32_t tick_{0};
    uint32_t wave_{1};
    uint32_t score_{0};
    uint32_t killStreak_{0};
    uint32_t nextZombieId_{0};
    float waveTimer_{0.f};
    std::mt19937 rng_;
    std::vector<WorldEvent> pendingEvents_;
};
