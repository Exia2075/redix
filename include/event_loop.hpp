#pragma once

#include <cstdint>
#include <expected>
#include <flat_map>
#include <functional>
#include <string>

#include "fd.hpp"

namespace redix {

using EventCallback = std::function<void(std::uint32_t events)>;

class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    std::expected<void, std::string> add(int fd, std::uint32_t events, EventCallback cb);
    std::expected<void, std::string> modify(int fd, std::uint32_t events, EventCallback cb);
    void remove(int fd);

    int run_once(int timeout_ms = 100);
    void run();
    void stop();

private:
    FileDescriptor epoll_fd_;
    std::flat_map<int, EventCallback> callbacks_;
    bool running_{false};
    static constexpr int MAX_EVENTS = 1024;
};

} // namespace redix
