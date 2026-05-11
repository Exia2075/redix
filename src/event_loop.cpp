#include "event_loop.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <sys/epoll.h>
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

} // namespace

EventLoop::EventLoop() : epoll_fd_{epoll_create1(EPOLL_CLOEXEC)} {}

EventLoop::~EventLoop() {
    stop();
}

std::expected<void, std::string> EventLoop::add(int fd, std::uint32_t events, EventCallback cb) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
        return std::unexpected(syscall_error("epoll_ctl(ADD)"));
    }

    callbacks_[fd] = std::move(cb);
    return {};
}

std::expected<void, std::string> EventLoop::modify(int fd,
                                                   std::uint32_t events,
                                                   EventCallback cb) {
    epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &ev) < 0) {
        return std::unexpected(syscall_error("epoll_ctl(MOD)"));
    }

    callbacks_[fd] = std::move(cb);
    return {};
}

void EventLoop::remove(int fd) {
    callbacks_.erase(fd);
    epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr);
}

int EventLoop::run_once(int timeout_ms) {
    std::vector<epoll_event> events(MAX_EVENTS);
    const int n = epoll_wait(epoll_fd_.get(), events.data(), MAX_EVENTS, timeout_ms);
    if (n < 0) {
        return errno == EINTR ? 0 : -1;
    }

    for (int i = 0; i < n; ++i) {
        const int fd = events[static_cast<std::size_t>(i)].data.fd;
        auto it = callbacks_.find(fd);
        if (it == callbacks_.end()) {
            continue;
        }

        EventCallback cb = it->second;
        cb(events[static_cast<std::size_t>(i)].events);
    }

    return n;
}

void EventLoop::run() {
    running_ = true;
    while (running_) {
        if (run_once(100) < 0) {
            stop();
        }
    }
}

void EventLoop::stop() {
    running_ = false;
}

} // namespace redix
