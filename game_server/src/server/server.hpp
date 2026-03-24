#pragma once

#include "world.hpp"
#include "network/connection.hpp"
#include "network/packets.hpp"
#include "utils/logger.hpp"
#include "utils/thread_pool.hpp"

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
#include <memory>
#include <vector>
#include <span>

namespace server {
    inline constexpr int MAX_EVENTS = 256;
    inline constexpr int LISTEN_BACKLOG = 512;

    class Server {
    public:
        Server(int port, World &world, utils::ThreadPool &pool)
            : port_(port), world_(world), pool_(pool) {
        }

        ~Server() { stop(); }

        bool start() {
            if (!create_listen_socket()) return false;
            if (!create_epoll()) return false;

            if (!epoll_add(listen_fd_, EPOLLIN)) {
                LOG_ERROR("Failed to add listen_fd to epoll");
                return false;
            }

            running_.store(true, std::memory_order_release);
            LOG_INFO("Server listening on port %d", port_);
            return true;
        }

        void poll(int timeout_ms = 0) {
            epoll_event events[MAX_EVENTS];
            int n = epoll_wait(epoll_fd_, events, MAX_EVENTS, timeout_ms);
            if (n < 0) {
                if (errno != EINTR)
                    LOG_ERROR("epoll_wait: %s", strerror(errno));
                return;
            }

            for (int i = 0; i < n; ++i) {
                int fd = events[i].data.fd;

                if (fd == listen_fd_) {
                    accept_clients();
                    continue;
                }

                auto *conn = get_connection(fd);
                if (!conn) continue;

                if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                    disconnect(fd);
                    continue;
                }

                if (events[i].events & EPOLLIN) handle_recv(conn);
                if (events[i].events & EPOLLOUT) handle_send(conn);
            }
        }

        void send_to(int fd, std::span<const uint8_t> data) {
            auto *conn = get_connection(fd);
            if (!conn || !conn->alive) return;

            {
                std::lock_guard lock(send_mu_);
                net::SendChunk chunk;
                chunk.data.assign(data.begin(), data.end());
                conn->send_queue.push_back(std::move(chunk));
            }

            epoll_mod(fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET);
        }

        void broadcast(std::span<const uint8_t> data) {
            std::lock_guard lock(conn_mu_);
            for (auto &[fd, conn]: connections_) {
                if (!conn->alive) continue;
                {
                    std::lock_guard sl(send_mu_);
                    net::SendChunk chunk;
                    chunk.data.assign(data.begin(), data.end());
                    conn->send_queue.push_back(std::move(chunk));
                }
                epoll_mod(fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET);
            }
        }

        void stop() {
            running_.store(false, std::memory_order_release);
            {
                std::lock_guard lock(conn_mu_);
                for (auto &[fd, _]: connections_) close(fd);
                connections_.clear();
            }
            if (epoll_fd_ >= 0) {
                close(epoll_fd_);
                epoll_fd_ = -1;
            }
            if (listen_fd_ >= 0) {
                close(listen_fd_);
                listen_fd_ = -1;
            }
        }

        bool running() const { return running_.load(std::memory_order_acquire); }

    private:
        bool create_listen_socket() {
            listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (listen_fd_ < 0) {
                LOG_ERROR("socket: %s", strerror(errno));
                return false;
            }

            int opt = 1;
            setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(static_cast<uint16_t>(port_));

            if (bind(listen_fd_, (sockaddr *) &addr, sizeof(addr)) < 0) {
                LOG_ERROR("bind: %s", strerror(errno));
                return false;
            }
            if (listen(listen_fd_, LISTEN_BACKLOG) < 0) {
                LOG_ERROR("listen: %s", strerror(errno));
                return false;
            }
            return true;
        }

        bool create_epoll() {
            epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
            if (epoll_fd_ < 0) {
                LOG_ERROR("epoll_create1: %s", strerror(errno));
                return false;
            }
            return true;
        }

        bool epoll_add(int fd, uint32_t events) {
            epoll_event ev{};
            ev.events = events | EPOLLET;
            ev.data.fd = fd;
            return epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == 0;
        }

        bool epoll_mod(int fd, uint32_t events) {
            epoll_event ev{};
            ev.events = events | EPOLLET;
            ev.data.fd = fd;
            return epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == 0;
        }

        void accept_clients() {
            for (;;) {
                sockaddr_in caddr{};
                socklen_t clen = sizeof(caddr);
                int cfd = accept4(listen_fd_, (sockaddr *) &caddr, &clen,
                                  SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (cfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    LOG_ERROR("accept4: %s", strerror(errno));
                    break;
                }

                int flag = 1;
                setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

                auto conn = std::make_unique<net::Connection>(cfd, caddr);

                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
                LOG_INFO("New connection fd=%d from %s:%d", cfd, ip, ntohs(caddr.sin_port));

                ecs::Entity e = world_.spawn_player(cfd);
                conn->entity_id = e;

                net::PacketWriter pw(net::PacketId::WELCOME);
                net::WelcomePayload wp{e};
                pw.write(wp);
                auto welcome = pw.finalise();

                {
                    std::lock_guard sl(send_mu_);
                    net::SendChunk chunk;
                    chunk.data.assign(welcome.begin(), welcome.end());
                    conn->send_queue.push_back(std::move(chunk));
                }

                epoll_add(cfd, EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET);

                {
                    std::lock_guard lock(conn_mu_);
                    connections_[cfd] = std::move(conn);
                }
            }
        }

        void handle_recv(net::Connection *conn) {
            for (;;) {
                uint8_t tmp[4096];
                ssize_t n = recv(conn->fd, tmp, sizeof(tmp), MSG_DONTWAIT);
                if (n <= 0) {
                    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                        disconnect(conn->fd);
                    break;
                }

                if (conn->recv_buf.writable() < (std::size_t) n) {
                    LOG_WARN("recv buffer overflow fd=%d — disconnecting", conn->fd);
                    disconnect(conn->fd);
                    return;
                }

                conn->recv_buf.push(tmp, (std::size_t) n);
            }

            parse_packets(conn);
        }

        void parse_packets(net::Connection *conn) {
            for (;;) {
                if (conn->recv_buf.readable() < sizeof(net::PacketHeader)) break;

                net::PacketHeader hdr{};
                conn->recv_buf.peek(reinterpret_cast<uint8_t *>(&hdr), sizeof(hdr));

                if (hdr.length < sizeof(net::PacketHeader)) {
                    disconnect(conn->fd);
                    return;
                }
                if (conn->recv_buf.readable() < hdr.length) break;

                conn->recv_buf.consume(sizeof(net::PacketHeader));
                uint16_t payload_len = hdr.length - sizeof(net::PacketHeader);

                if (payload_len > 0) {
                    std::vector<uint8_t> payload(payload_len);
                    conn->recv_buf.peek(payload.data(), payload_len);
                    conn->recv_buf.consume(payload_len);

                    auto *conn_ptr = conn;
                    pool_.enqueue([this, conn_ptr, id = hdr.id,
                            pl = std::move(payload)]() mutable {
                            dispatch_packet(conn_ptr, id, pl.data(),
                                            static_cast<uint16_t>(pl.size()));
                        });
                } else {
                    conn->recv_buf.consume(0);
                    dispatch_packet(conn, hdr.id, nullptr, 0);
                }
            }
        }

        void dispatch_packet(net::Connection *conn, uint8_t id,
                             const uint8_t *payload, uint16_t len) {
            switch (static_cast<net::PacketId>(id)) {
                case net::PacketId::INPUT: {
                    if (len < sizeof(net::InputPayload)) break;
                    auto *inp = new net::InputPayload{};
                    std::memcpy(inp, payload, sizeof(net::InputPayload));
                    auto *prev = conn->pending_input.exchange(inp, std::memory_order_acq_rel);
                    delete prev;
                    break;
                }
                case net::PacketId::PING: {
                    net::PacketWriter pw(net::PacketId::PONG);
                    auto pong = pw.finalise();
                    send_to(conn->fd, pong);
                    break;
                }
                default:
                    break;
            }
        }

        void handle_send(net::Connection *conn) {
            std::lock_guard lock(send_mu_);

            while (!conn->send_queue.empty()) {
                auto &chunk = conn->send_queue.front();
                const uint8_t *ptr = chunk.data.data() + chunk.sent;
                std::size_t left = chunk.data.size() - chunk.sent;

                ssize_t n = send(conn->fd, ptr, left, MSG_DONTWAIT | MSG_NOSIGNAL);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    disconnect(conn->fd);
                    return;
                }
                chunk.sent += (std::size_t) n;
                if (chunk.sent >= chunk.data.size())
                    conn->send_queue.erase(conn->send_queue.begin());
            }

            if (conn->send_queue.empty())
                epoll_mod(conn->fd, EPOLLIN | EPOLLRDHUP | EPOLLET);
        }

        void disconnect(int fd) {
            net::Connection *conn = get_connection(fd);
            if (!conn || !conn->alive) return;

            conn->alive = false;
            LOG_INFO("Client disconnected fd=%d entity=%u", fd, conn->entity_id);

            if (conn->entity_id != UINT32_MAX)
                world_.despawn_player(conn->entity_id);

            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
            close(fd);

            std::lock_guard lock(conn_mu_);
            connections_.erase(fd);
        }

        net::Connection *get_connection(int fd) {
            std::lock_guard lock(conn_mu_);
            auto it = connections_.find(fd);
            return (it != connections_.end()) ? it->second.get() : nullptr;
        }

    public:
        void flush_inputs_to_world() {
            std::lock_guard lock(conn_mu_);
            for (auto &[fd, conn]: connections_) {
                if (!conn->alive || conn->entity_id == UINT32_MAX) continue;

                net::InputPayload *inp = conn->pending_input.exchange(
                    nullptr, std::memory_order_acq_rel);
                if (!inp) continue;

                world_.apply_input(conn->entity_id, *inp);
                delete inp;
            }
        }

    private:
        int port_;
        int listen_fd_ = -1;
        int epoll_fd_ = -1;
        World &world_;
        utils::ThreadPool &pool_;

        std::atomic<bool> running_{false};

        std::mutex conn_mu_;
        std::unordered_map<int, std::unique_ptr<net::Connection> > connections_;

        std::mutex send_mu_;
    };
} // namespace server
