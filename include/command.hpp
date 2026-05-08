#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace redix {

struct RespValue;
class Store;

struct Command {
    std::string name;
    std::vector<std::string> args;
};

struct ServerContext {
    std::string bind_addr;
    std::uint16_t port{6379};
    bool aof_enabled{false};
    std::string db_file_prefix{"./redix"};
    std::uint64_t started_at_ms{0};
    std::size_t connected_clients{0};
};

std::expected<Command, std::vector<std::byte>> parse_command(const RespValue& resp);

std::vector<std::byte> dispatch(const Command& cmd, Store& store, ServerContext& ctx);

bool is_write_command(const Command& cmd);

} // namespace redix
