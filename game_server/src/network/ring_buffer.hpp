#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace net {
    template<std::size_t N>
    class RingBuffer {
        static_assert((N & (N - 1)) == 0, "N must be power of 2");

    public:
        void push(const uint8_t *data, std::size_t len) {
            for (std::size_t i = 0; i < len; ++i)
                buf_[(write_ + i) & mask_] = data[i];
            write_ += len;
        }

        void peek(uint8_t *dst, std::size_t len) const {
            for (std::size_t i = 0; i < len; ++i)
                dst[i] = buf_[(read_ + i) & mask_];
        }

        void consume(std::size_t len) { read_ += len; }

        std::size_t readable() const { return write_ - read_; }
        std::size_t writable() const { return N - readable(); }

        void reset() { read_ = write_ = 0; }

    private:
        static constexpr std::size_t mask_ = N - 1;
        std::array<uint8_t, N> buf_{};
        std::size_t read_ = 0;
        std::size_t write_ = 0;
    };
}
