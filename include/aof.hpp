#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "fd.hpp"

namespace redix {

class Store;
struct Command;

class Aof {
public:
    std::expected<void, std::string> open(std::string_view path);
    void close();

    std::expected<void, std::string> append(const Command& cmd);
    std::expected<void, std::string> flush();

    static std::expected<void, std::string> replay(Store& store, std::string_view path);

    bool is_open() const noexcept;

private:
    FileDescriptor fd_;
    std::vector<std::byte> buf_;
    static constexpr std::size_t FLUSH_THRESHOLD = 4096;
};

} // namespace redix
