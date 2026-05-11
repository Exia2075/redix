#include "config.hpp"
#include "server.hpp"

#include <cstdio>
#include <cstdlib>
#include <print>

namespace {

void print_usage() {
    std::println(stderr,
                 "Usage: ./bin/redix [options]\n"
                 "Options:\n"
                 "  -h                  Print this help message\n"
                 "  -p port             TCP port to listen on. Default: 6379\n"
                 "  -b bind_addr        Address to bind to. Default: 127.0.0.1\n"
                 "  -d db_file          Path prefix for RDB/AOF files. Default: ./redix\n"
                 "  --aof               Enable AOF logging. Default: disabled\n"
                 "  --no-rdb            Disable RDB loading on startup\n"
                 "  --loglevel level    Log level: debug | info | warn | error. Default: info");
}

} // namespace

int main(int argc, char* argv[]) {
    auto cfg = redix::parse_config(argc, argv);
    if (!cfg) {
        print_usage();
        if (cfg.error() != "help") {
            std::println(stderr, "error: {}", cfg.error());
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    redix::Server server{*cfg};
    if (auto started = server.start(); !started) {
        std::println(stderr, "error: {}", started.error());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
