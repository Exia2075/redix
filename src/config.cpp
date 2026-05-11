#include "config.hpp"

#include <charconv>
#include <string_view>
#include <system_error>

namespace redix {

namespace {

bool parse_port(std::string_view text, std::uint16_t& out) {
    int value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || value <= 0 || value > 65535) {
        return false;
    }

    out = static_cast<std::uint16_t>(value);
    return true;
}

bool is_loglevel(std::string_view level) {
    return level == "debug" || level == "info" || level == "warn" || level == "error";
}

} // namespace

std::expected<Config, std::string> parse_config(int argc, char* argv[]) {
    Config cfg;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};

        if (arg == "-h") {
            return std::unexpected(std::string{"help"});
        }
        if (arg == "--aof") {
            cfg.aof_enabled = true;
            continue;
        }
        if (arg == "--no-rdb") {
            cfg.rdb_enabled = false;
            continue;
        }

        if ((arg == "-p" || arg == "-b" || arg == "-d" || arg == "--loglevel") && i + 1 >= argc) {
            return std::unexpected("missing value for " + std::string{arg});
        }

        if (arg == "-p") {
            if (!parse_port(argv[++i], cfg.port)) {
                return std::unexpected(std::string{"invalid port"});
            }
        } else if (arg == "-b") {
            cfg.bind_addr = argv[++i];
        } else if (arg == "-d") {
            cfg.db_file_prefix = argv[++i];
        } else if (arg == "--loglevel") {
            cfg.loglevel = argv[++i];
            if (!is_loglevel(cfg.loglevel)) {
                return std::unexpected(std::string{"invalid loglevel"});
            }
        } else {
            return std::unexpected("unknown option: " + std::string{arg});
        }
    }

    return cfg;
}

} // namespace redix
