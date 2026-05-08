#pragma once

#include <expected>
#include <flat_map>
#include <memory>
#include <string>

#include "aof.hpp"
#include "client.hpp"
#include "command.hpp"
#include "config.hpp"
#include "event_loop.hpp"
#include "fd.hpp"
#include "store.hpp"

namespace redix {

class Server {
public:
    explicit Server(Config cfg);

    std::expected<void, std::string> start();
    void shutdown();

private:
    Config cfg_;
    Store store_;
    Aof aof_;
    EventLoop loop_;
    FileDescriptor listen_fd_;
    FileDescriptor signal_fd_;
    std::flat_map<int, std::unique_ptr<Client>> clients_;
    ServerContext ctx_;

    void on_new_connection();
    void on_client_readable(int fd);
    void on_client_writable(int fd);
    void on_signal();
    void close_client(int fd);
    void setup_signals();
    void tick_periodic_tasks();
};

} // namespace redix
