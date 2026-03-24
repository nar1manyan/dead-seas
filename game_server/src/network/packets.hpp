#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <span>
#include <cmath>

namespace net {
    enum class PacketId : uint8_t {
        INPUT = 0x01,
        SNAPSHOT = 0x10,
        WELCOME = 0x11,
        PING = 0xFE,
        PONG = 0xFF,
    };

#pragma pack(push, 1)

    struct PacketHeader {
        uint16_t length;
        uint8_t id;
    };

    struct InputPayload {
        float dx;
        float dy;
        uint8_t action;
        uint32_t seq;
    };

    struct EntityState {
        uint32_t entity_id;
        float x, y;
        int16_t hp;
        uint8_t flags;
    };

    struct SnapshotHeader {
        uint32_t tick;
        uint16_t entity_count;
    };

    struct WelcomePayload {
        uint32_t assigned_entity_id;
    };

#pragma pack(pop)

    class PacketWriter {
    public:
        explicit PacketWriter(PacketId id) {
            buf_.resize(sizeof(PacketHeader));
            buf_[2] = static_cast<uint8_t>(id);
        }

        template<typename T>
        PacketWriter &write(const T &val) {
            static_assert(std::is_trivially_copyable_v<T>);
            const auto *p = reinterpret_cast<const uint8_t *>(&val);
            buf_.insert(buf_.end(), p, p + sizeof(T));
            return *this;
        }

        std::span<const uint8_t> finalise() {
            uint16_t len = static_cast<uint16_t>(buf_.size());
            std::memcpy(buf_.data(), &len, 2);
            return {buf_.data(), buf_.size()};
        }

    private:
        std::vector<uint8_t> buf_;
    };

    class PacketReader {
    public:
        explicit PacketReader(const uint8_t *data, uint16_t size)
            : ptr_(data), end_(data + size) {
        }

        template<typename T>
        bool read(T &out) {
            static_assert(std::is_trivially_copyable_v<T>);
            if (ptr_ + sizeof(T) > end_) return false;
            std::memcpy(&out, ptr_, sizeof(T));
            ptr_ += sizeof(T);
            return true;
        }

        bool empty() const { return ptr_ >= end_; }

    private:
        const uint8_t *ptr_;
        const uint8_t *end_;
    };
}
