#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

static constexpr uint16_t PROTOCOL_VERSION = 1;
static constexpr uint16_t DEFAULT_PORT = 54321;
static constexpr int MAX_ZOMBIES = 64;

namespace Cfg {
    constexpr float WORLD_W = 800.f;
    constexpr float WORLD_H = 600.f;
    constexpr float PLAYER_SPEED = 180.f;
    constexpr float PLAYER_RADIUS = 18.f;
    constexpr float ZOMBIE_SPEED_BASE = 70.f;
    constexpr float ZOMBIE_RADIUS = 16.f;
    constexpr float ATTACK_RANGE = 120.f;
    constexpr float ATTACK_ANGLE = 60.f; // degrees half-arc
    constexpr float ATTACK_COOLDOWN = 0.45f;
    constexpr float ZOMBIE_ATTACK_CD = 1.2f;
    constexpr float ZOMBIE_ATTACK_R = 30.f;
    constexpr int PLAYER_MAX_LIVES = 5;
    constexpr int ZOMBIE_HP = 2;
    constexpr int LIFE_DROP_KILLS = 4;
    constexpr float LIFE_DROP_CHANCE = 0.35f;
    constexpr float WAVE_INTERVAL = 18.f;
    constexpr int ZOMBIES_PER_WAVE = 5;
}

enum class PacketType : uint8_t {
    C2S_JOIN = 0x01,
    C2S_INPUT = 0x02,
    C2S_LEAVE = 0x03,

    S2C_WELCOME = 0x10,
    S2C_STATE = 0x11,
    S2C_EVENT = 0x12,
    S2C_REJECT = 0x13,
};

namespace InputFlag {
    constexpr uint8_t MOVE_UP = 1 << 0;
    constexpr uint8_t MOVE_DOWN = 1 << 1;
    constexpr uint8_t MOVE_LEFT = 1 << 2;
    constexpr uint8_t MOVE_RIGHT = 1 << 3;
    constexpr uint8_t ATTACK = 1 << 4;
}

enum class EventKind : uint8_t {
    ZOMBIE_DIED = 0x01,
    PLAYER_HIT = 0x02,
    LIFE_DROP = 0x03,
    WAVE_START = 0x04,
    GAME_OVER = 0x05,
};

#pragma pack(push, 1)

struct Vec2f {
    float x{0.f};
    float y{0.f};
};

struct ZombieSnap {
    uint32_t id{0};
    Vec2f pos{};
    uint8_t hp{0};
    uint8_t alive{1};
};

struct StatePayload {
    Vec2f playerPos{};
    int8_t playerLives{3};
    uint8_t playerAlive{1};
    uint32_t score{0};
    uint32_t wave{1};
    uint32_t killStreak{0};
    uint8_t zombieCount{0};
    ZombieSnap zombies[MAX_ZOMBIES]{};
};

struct C2S_JoinPacket {
    uint8_t type{static_cast<uint8_t>(PacketType::C2S_JOIN)};
    uint16_t version{PROTOCOL_VERSION};
    char name[16]{"Player"};
};

struct C2S_InputPacket {
    uint8_t type{static_cast<uint8_t>(PacketType::C2S_INPUT)};
    uint8_t flags{0};
    float aimX{0.f};
    float aimY{0.f};
};

struct S2C_WelcomePacket {
    uint8_t type{static_cast<uint8_t>(PacketType::S2C_WELCOME)};
    uint32_t playerId{0};
    float worldW{800.f};
    float worldH{600.f};
};

struct S2C_StatePacket {
    uint8_t type{static_cast<uint8_t>(PacketType::S2C_STATE)};
    uint32_t tick{0};
    StatePayload payload{};
};

struct S2C_EventPacket {
    uint8_t type{static_cast<uint8_t>(PacketType::S2C_EVENT)};
    EventKind kind{EventKind::ZOMBIE_DIED};
    uint32_t entityId{0};
    float x{0.f};
    float y{0.f};
};

struct S2C_RejectPacket {
    uint8_t type{static_cast<uint8_t>(PacketType::S2C_REJECT)};
    char reason[64]{"Server full"};
};

#pragma pack(pop)

inline bool sendAll(int fd, const void *buf, size_t len) {
    const char *p = reinterpret_cast<const char *>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

inline bool recvAll(int fd, void *buf, size_t len) {
    char *p = reinterpret_cast<char *>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, p + got, len - got, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

template<typename T>
bool sendPacket(int fd, const T &pkt) {
    uint32_t len = static_cast<uint32_t>(sizeof(T));
    if (!sendAll(fd, &len, 4)) return false;
    return sendAll(fd, &pkt, sizeof(T));
}

inline bool recvFrame(int fd, std::vector<uint8_t> &out) {
    uint32_t len = 0;
    if (!recvAll(fd, &len, 4)) return false;
    if (len == 0 || len > 65536) return false;
    out.resize(len);
    return recvAll(fd, out.data(), len);
}
