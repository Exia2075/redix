#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace redix {

struct Config {
    std::string bind_addr{"127.0.0.1"};
    std::uint16_t port{6379};
    std::string db_file_prefix{"./redix"};
    bool aof_enabled{false};
    bool rdb_enabled{true};
    std::string loglevel{"info"};
};

std::expected<Config, std::string> parse_config(int argc, char* argv[]);

} // namespace redix
