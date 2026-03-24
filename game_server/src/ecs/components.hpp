#pragma once
#include <cstdint>

namespace components {
    struct Position {
        float x = 0.f, y = 0.f;
    };

    struct Velocity {
        float vx = 0.f, vy = 0.f;
    };

    struct Health {
        int hp = 100;
        int max_hp = 100;
        bool dead() const { return hp <= 0; }
    };

    struct PlayerTag {
        uint8_t _pad = 0;
    };

    struct ZombieTag {
        uint8_t _pad = 0;
    };

    struct NetworkId {
        int fd = -1;
    };
}
