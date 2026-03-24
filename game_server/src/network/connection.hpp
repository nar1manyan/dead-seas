#pragma once
#include "ring_buffer.hpp"
#include "packets.hpp"
#include <cstdint>
#include <atomic>
#include <string>
#include <vector>
#include <netinet/in.h>

namespace net {
    struct SendChunk {
        std::vector<uint8_t> data;
        std::size_t sent = 0;
    };

    struct Connection {
        int fd = -1;
        bool alive = false;

        uint32_t entity_id = UINT32_MAX;

        alignas(64) std::atomic<InputPayload *> pending_input{nullptr};

        RingBuffer<65536> recv_buf;

        std::vector<SendChunk> send_queue;

        sockaddr_in addr{};

        explicit Connection(int fd_, sockaddr_in addr_)
            : fd(fd_), alive(true), addr(addr_) {
        }

        ~Connection() {
            delete pending_input.load(std::memory_order_acquire);
        }

        Connection(const Connection &) = delete;

        Connection &operator=(const Connection &) = delete;
    };
}
