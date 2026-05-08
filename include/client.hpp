#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "fd.hpp"
#include "resp.hpp"

namespace redix {

class Aof;
class Store;
struct ServerContext;

enum class ClientState {
    Reading,
    Writing,
    Closing,
};

class Client {
public:
    explicit Client(FileDescriptor fd, std::string remote_addr);

    int fd() const noexcept;
    const std::string& remote_addr() const noexcept;
    ClientState state() const noexcept;

    bool on_readable(Store& store, Aof& aof, ServerContext& ctx);
    bool on_writable();
    void enqueue_response(std::vector<std::byte> data);

private:
    FileDescriptor fd_;
    std::string remote_addr_;
    ClientState state_{ClientState::Reading};
    RespParser parser_;
    std::vector<std::byte> write_buf_;
    std::size_t write_offset_{0};
};

} // namespace redix
