#pragma once
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <functional>
#include <stdexcept>
#include <string>
#include "../../shared/include/protocol.hpp"
#include "../../shared/include/logger.hpp"
#include "world.hpp"

struct ClientSession {
    int fd{-1};
    uint32_t id{0};
    char name[16]{"?"};
    bool active{false};
    std::vector<uint8_t> readBuf;
    uint32_t readNeed{0};
    bool haveLen{false};
    uint8_t lenBuf[4]{};
    int lenGot{0};
};

class GameServer {
public:
    explicit GameServer(uint16_t port) : port_(port) {
    }

    void start() {
        setupSocket();
        setupEpoll();
        LOG_INFO("Server running on port " + std::to_string(port_));
        running_ = true;
    }

    void stop() {
        running_ = false;
        if (epollFd_ >= 0) {
            close(epollFd_);
            epollFd_ = -1;
        }
        if (listenFd_ >= 0) {
            close(listenFd_);
            listenFd_ = -1;
        }
        std::lock_guard<std::mutex> lk(clientsMtx_);
        for (auto &[fd, sess]: clients_) {
            close(fd);
        }
        clients_.clear();
        LOG_INFO("Server stopped");
    }

    void pollOnce(int timeoutMs = 5) {
        constexpr int MAX_EV = 32;
        epoll_event events[MAX_EV];
        int n = epoll_wait(epollFd_, events, MAX_EV, timeoutMs);
        if (n < 0) {
            if (errno == EINTR) return;
            return;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == listenFd_) {
                acceptClient();
            } else {
                handleClientData(fd);
            }
        }
    }

    void broadcastState(const S2C_StatePacket &pkt) {
        std::lock_guard<std::mutex> lk(clientsMtx_);
        for (auto &[fd, sess]: clients_) {
            if (!sess.active) continue;
            if (!sendPacket(fd, pkt)) {
                LOG_WARN("Broadcast failed to fd=" + std::to_string(fd));
            }
        }
    }

    void broadcastEvent(const S2C_EventPacket &pkt) {
        std::lock_guard<std::mutex> lk(clientsMtx_);
        for (auto &[fd, sess]: clients_) {
            if (!sess.active) continue;
            sendPacket(fd, pkt);
        }
    }

    bool isRunning() const { return running_; }

    int clientCount() const {
        std::lock_guard<std::mutex> lk(clientsMtx_);
        return static_cast<int>(clients_.size());
    }

    std::function<void(uint8_t flags, float ax, float ay)> onInput;

private:
    void setupSocket() {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) throw std::runtime_error("socket() failed");

        int opt = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(listenFd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        setNonBlocking(listenFd_);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(listenFd_, (sockaddr *) &addr, sizeof(addr)) < 0)
            throw std::runtime_error("bind() failed: " + std::string(strerror(errno)));
        if (listen(listenFd_, SOMAXCONN) < 0)
            throw std::runtime_error("listen() failed");
    }

    void setupEpoll() {
        epollFd_ = epoll_create1(0);
        if (epollFd_ < 0) throw std::runtime_error("epoll_create1() failed");
        addToEpoll(listenFd_, EPOLLIN);
    }

    void addToEpoll(int fd, uint32_t events) {
        epoll_event ev{};
        ev.events = events | EPOLLET;
        ev.data.fd = fd;
        epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev);
    }

    void setNonBlocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    void acceptClient() {
        for (;;) {
            sockaddr_in caddr{};
            socklen_t caddrLen = sizeof(caddr);
            int cfd = accept4(listenFd_, (sockaddr *) &caddr, &caddrLen, SOCK_NONBLOCK);
            if (cfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                LOG_WARN("accept4 error: " + std::string(strerror(errno)));
                break;
            }
            int opt = 1;
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
            addToEpoll(cfd, EPOLLIN | EPOLLRDHUP | EPOLLERR);

            uint32_t newId = ++nextClientId_;
            {
                std::lock_guard<std::mutex> lk(clientsMtx_);
                ClientSession sess{};
                sess.fd = cfd;
                sess.id = newId;
                sess.active = false;
                clients_[cfd] = sess;
            }
            LOG_INFO("New connection fd=" + std::to_string(cfd) +
                " from " + inet_ntoa(caddr.sin_addr));
        }
    }

    void handleClientData(int fd) {
        uint8_t rawBuf[4096];
        ssize_t n;
        while ((n = recv(fd, rawBuf, sizeof(rawBuf), MSG_DONTWAIT)) > 0) {
            appendToSession(fd, rawBuf, static_cast<size_t>(n));
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            removeClient(fd);
        }
    }

    void appendToSession(int fd, const uint8_t *data, size_t len) {
        std::lock_guard<std::mutex> lk(clientsMtx_);
        auto it = clients_.find(fd);
        if (it == clients_.end()) return;
        ClientSession &sess = it->second;

        size_t pos = 0;
        while (pos < len) {
            if (!sess.haveLen) {
                while (pos < len && sess.lenGot < 4) {
                    sess.lenBuf[sess.lenGot++] = data[pos++];
                }
                if (sess.lenGot == 4) {
                    uint32_t pktLen;
                    memcpy(&pktLen, sess.lenBuf, 4);
                    if (pktLen == 0 || pktLen > 65536) {
                        removeClientLocked(fd);
                        return;
                    }
                    sess.readNeed = pktLen;
                    sess.readBuf.clear();
                    sess.readBuf.reserve(pktLen);
                    sess.haveLen = true;
                    sess.lenGot = 0;
                }
            } else {
                size_t want = sess.readNeed - sess.readBuf.size();
                size_t take = std::min(want, len - pos);
                sess.readBuf.insert(sess.readBuf.end(), data + pos, data + pos + take);
                pos += take;
                if (sess.readBuf.size() == sess.readNeed) {
                    processPacket(sess, sess.readBuf);
                    sess.haveLen = false;
                    sess.readBuf.clear();
                }
            }
        }
    }

    void processPacket(ClientSession &sess, const std::vector<uint8_t> &buf) {
        if (buf.empty()) return;
        auto type = static_cast<PacketType>(buf[0]);

        if (type == PacketType::C2S_JOIN && buf.size() >= sizeof(C2S_JoinPacket)) {
            C2S_JoinPacket join{};
            memcpy(&join, buf.data(), sizeof(join));
            memcpy(sess.name, join.name, 16);
            sess.name[15] = '\0';
            sess.active = true;

            S2C_WelcomePacket welcome{};
            welcome.playerId = sess.id;
            welcome.worldW = Cfg::WORLD_W;
            welcome.worldH = Cfg::WORLD_H;
            sendPacket(sess.fd, welcome);
            LOG_INFO("Player " + std::string(sess.name) + " joined (id=" +
                std::to_string(sess.id) + ")");
        } else if (type == PacketType::C2S_INPUT && buf.size() >= sizeof(C2S_InputPacket)) {
            C2S_InputPacket inp{};
            memcpy(&inp, buf.data(), sizeof(inp));
            if (onInput) onInput(inp.flags, inp.aimX, inp.aimY);
        } else if (type == PacketType::C2S_LEAVE) {
            removeClientLocked(sess.fd);
        }
    }

    void removeClient(int fd) {
        std::lock_guard<std::mutex> lk(clientsMtx_);
        removeClientLocked(fd);
    }

    void removeClientLocked(int fd) {
        epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        clients_.erase(fd);
        LOG_INFO("Client fd=" + std::to_string(fd) + " disconnected");
    }


    uint16_t port_;
    int listenFd_{-1};
    int epollFd_{-1};
    std::atomic<bool> running_{false};
    mutable std::mutex clientsMtx_;
    std::unordered_map<int, ClientSession> clients_;
    std::atomic<uint32_t> nextClientId_{0};
};
