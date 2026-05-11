#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace redix {

namespace {

std::string syscall_error(std::string_view op) {
    std::string msg{op};
    msg.append(": ");
    msg.append(std::strerror(errno));
    return msg;
}

std::string remote_addr_string(const sockaddr_storage& addr) {
    char ip[INET6_ADDRSTRLEN]{};
    std::uint16_t port = 0;

    if (addr.ss_family == AF_INET) {
        const auto* in = reinterpret_cast<const sockaddr_in*>(&addr);
        inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
        port = ntohs(in->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof(ip));
        port = ntohs(in6->sin6_port);
    } else {
        return "unknown";
    }

    std::string out{ip};
    out.push_back(':');
    out.append(std::to_string(port));
    return out;
}

std::uint64_t now_ms() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace

Server::Server(Config cfg) : cfg_{std::move(cfg)} {
    ctx_.bind_addr = cfg_.bind_addr;
    ctx_.port = cfg_.port;
    ctx_.aof_enabled = cfg_.aof_enabled;
    ctx_.db_file_prefix = cfg_.db_file_prefix;
    ctx_.started_at_ms = now_ms();
    ctx_.connected_clients = 0;
}

std::expected<void, std::string> Server::start() {
    setup_signals();
    if (!signal_fd_) {
        return std::unexpected(std::string{"failed to set up signal handling"});
    }

    FileDescriptor fd{socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)};
    if (!fd) {
        return std::unexpected(syscall_error("socket"));
    }

    int yes = 1;
    if (setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
        return std::unexpected(syscall_error("setsockopt(SO_REUSEADDR)"));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.port);
    if (inet_pton(AF_INET, cfg_.bind_addr.c_str(), &addr.sin_addr) != 1) {
        return std::unexpected("invalid bind address: " + cfg_.bind_addr);
    }

    if (bind(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        return std::unexpected(syscall_error("bind"));
    }
    if (listen(fd.get(), SOMAXCONN) < 0) {
        return std::unexpected(syscall_error("listen"));
    }

    listen_fd_ = std::move(fd);
    auto added = loop_.add(listen_fd_.get(), EPOLLIN, [this](std::uint32_t events) {
        if ((events & (EPOLLERR | EPOLLHUP)) != 0) {
            shutdown();
            return;
        }
        on_new_connection();
    });
    if (!added) {
        return std::unexpected(added.error());
    }

    loop_.run();
    return {};
}

void Server::shutdown() {
    aof_.close();

    std::vector<int> client_fds;
    client_fds.reserve(clients_.size());
    for (const auto& [fd, _] : clients_) {
        client_fds.push_back(fd);
    }
    for (int fd : client_fds) {
        close_client(fd);
    }

    if (listen_fd_) {
        loop_.remove(listen_fd_.get());
        listen_fd_ = FileDescriptor{};
    }
    if (signal_fd_) {
        loop_.remove(signal_fd_.get());
        signal_fd_ = FileDescriptor{};
    }

    loop_.stop();
}

void Server::on_new_connection() {
    while (true) {
        sockaddr_storage addr{};
        socklen_t addr_len = sizeof(addr);
        FileDescriptor client_fd{accept4(listen_fd_.get(),
                                         reinterpret_cast<sockaddr*>(&addr),
                                         &addr_len,
                                         SOCK_NONBLOCK | SOCK_CLOEXEC)};
        if (!client_fd) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            break;
        }

        const int fd = client_fd.get();
        auto client = std::make_unique<Client>(std::move(client_fd), remote_addr_string(addr));
        clients_[fd] = std::move(client);
        ++ctx_.connected_clients;

        auto added = loop_.add(fd, EPOLLIN, [this, fd](std::uint32_t events) {
            if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
                close_client(fd);
                return;
            }
            if ((events & EPOLLIN) != 0) {
                on_client_readable(fd);
            }
            if (clients_.find(fd) != clients_.end() && (events & EPOLLOUT) != 0) {
                on_client_writable(fd);
            }
        });
        if (!added) {
            close_client(fd);
        }
    }
}

void Server::on_client_readable(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return;
    }

    if (!it->second->on_readable(store_, aof_, ctx_)) {
        close_client(fd);
        return;
    }

    if (it->second->state() == ClientState::Writing) {
        auto modified = loop_.modify(fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP, [this, fd](std::uint32_t events) {
            if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
                close_client(fd);
                return;
            }
            if ((events & EPOLLIN) != 0) {
                on_client_readable(fd);
            }
            if (clients_.find(fd) != clients_.end() && (events & EPOLLOUT) != 0) {
                on_client_writable(fd);
            }
        });
        if (!modified) {
            close_client(fd);
        }
    }
}

void Server::on_client_writable(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return;
    }

    if (!it->second->on_writable()) {
        close_client(fd);
        return;
    }

    if (it->second->state() == ClientState::Reading) {
        auto modified = loop_.modify(fd, EPOLLIN | EPOLLRDHUP, [this, fd](std::uint32_t events) {
            if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
                close_client(fd);
                return;
            }
            if ((events & EPOLLIN) != 0) {
                on_client_readable(fd);
            }
            if (clients_.find(fd) != clients_.end() && (events & EPOLLOUT) != 0) {
                on_client_writable(fd);
            }
        });
        if (!modified) {
            close_client(fd);
        }
    }
}

void Server::on_signal() {
    while (true) {
        signalfd_siginfo info{};
        const ssize_t n = read(signal_fd_.get(), &info, sizeof(info));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            break;
        }
        if (n == 0) {
            break;
        }

        if (info.ssi_signo == SIGINT || info.ssi_signo == SIGTERM) {
            shutdown();
            break;
        }
    }
}

void Server::close_client(int fd) {
    auto it = clients_.find(fd);
    if (it == clients_.end()) {
        return;
    }

    loop_.remove(fd);
    clients_.erase(it);
    if (ctx_.connected_clients > 0) {
        --ctx_.connected_clients;
    }
}

void Server::setup_signals() {
    sigset_t mask{};
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    if (sigprocmask(SIG_BLOCK, &mask, nullptr) < 0) {
        return;
    }

    signal_fd_ = FileDescriptor{signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC)};
    if (!signal_fd_) {
        return;
    }

    auto added = loop_.add(signal_fd_.get(), EPOLLIN, [this](std::uint32_t events) {
        if ((events & (EPOLLERR | EPOLLHUP)) != 0) {
            shutdown();
            return;
        }
        on_signal();
    });
    if (!added) {
        signal_fd_ = FileDescriptor{};
    }
}

void Server::tick_periodic_tasks() {
    store_.sweep_expired();
    if (aof_.is_open()) {
        (void)aof_.flush();
    }
}

} // namespace redix
