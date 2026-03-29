#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>
#include <functional>
#include <string>
#include <stdexcept>
#include "../../shared/include/protocol.hpp"
#include "../../shared/include/logger.hpp"

struct ServerMessages {
    bool welcomed{false};
    S2C_WelcomePacket welcome{};
    bool hasState{false};
    S2C_StatePacket state{};
    std::vector<S2C_EventPacket> events;
};

class NetClient {
public:
    NetClient() = default;

    ~NetClient() { disconnect(); }

    bool connect(const std::string &host, uint16_t port) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) return false;

        if (::connect(fd_, (sockaddr *) &addr, sizeof(addr)) < 0) {
            close(fd_);
            fd_ = -1;
            return false;
        }

        int opt = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        int flags = fcntl(fd_, F_GETFL, 0);
        fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

        connected_ = true;
        LOG_INFO("Connected " + host + ":" + std::to_string(port));
        return true;
    }

    void disconnect() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        connected_ = false;
    }

    bool isConnected() const { return connected_ && fd_ >= 0; }

    bool sendJoin(const char *name) {
        C2S_JoinPacket pkt{};
        strncpy(pkt.name, name, 15);
        return sendPacket(fd_, pkt);
    }

    bool sendInput(uint8_t flags, float aimX, float aimY) {
        if (!isConnected()) return false;
        C2S_InputPacket pkt{};
        pkt.flags = flags;
        pkt.aimX = aimX;
        pkt.aimY = aimY;
        return sendPacket(fd_, pkt);
    }

    bool poll(ServerMessages &out) {
        if (!isConnected()) return false;
        uint8_t tmp[8192];
        ssize_t n;
        while ((n = recv(fd_, tmp, sizeof(tmp), MSG_DONTWAIT)) > 0) {
            recvBuf_.insert(recvBuf_.end(), tmp, tmp + n);
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            LOG_WARN("Server disconnected");
            disconnect();
            return false;
        }
        size_t pos = 0;
        while (pos + 4 <= recvBuf_.size()) {
            uint32_t pktLen;
            memcpy(&pktLen, recvBuf_.data() + pos, 4);
            if (pktLen == 0 || pktLen > 65536) {
                disconnect();
                return false;
            }
            if (pos + 4 + pktLen > recvBuf_.size()) break;
            const uint8_t *payload = recvBuf_.data() + pos + 4;
            sendNewPacket(payload, pktLen, out);
            pos += 4 + pktLen;
        }
        if (pos > 0) {
            recvBuf_.erase(recvBuf_.begin(), recvBuf_.begin() + static_cast<long>(pos));
        }
        return true;
    }

private:
    void sendNewPacket(const uint8_t *data, uint32_t len, ServerMessages &out) {
        if (len == 0) return;
        auto type = static_cast<PacketType>(data[0]);
        switch (type) {
            case PacketType::S2C_WELCOME:
                if (len >= sizeof(S2C_WelcomePacket)) {
                    memcpy(&out.welcome, data, sizeof(S2C_WelcomePacket));
                    out.welcomed = true;
                }
                break;
            case PacketType::S2C_STATE:
                if (len >= sizeof(S2C_StatePacket)) {
                    memcpy(&out.state, data, sizeof(S2C_StatePacket));
                    out.hasState = true;
                }
                break;
            case PacketType::S2C_EVENT: {
                if (len >= sizeof(S2C_EventPacket)) {
                    S2C_EventPacket ev{};
                    memcpy(&ev, data, sizeof(ev));
                    out.events.push_back(ev);
                }
                break;
            }
            default: break;
        }
    }

    int fd_{-1};
    bool connected_{false};
    std::vector<uint8_t> recvBuf_;
};
