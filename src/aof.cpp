#include "aof.hpp"

namespace redix {

std::expected<void, std::string> Aof::open(std::string_view) {
    return {};
}

void Aof::close() {
    fd_ = FileDescriptor{};
    buf_.clear();
}

std::expected<void, std::string> Aof::append(const Command&) {
    return {};
}

std::expected<void, std::string> Aof::flush() {
    buf_.clear();
    return {};
}

std::expected<void, std::string> Aof::replay(Store&, std::string_view) {
    return {};
}

bool Aof::is_open() const noexcept {
    return static_cast<bool>(fd_);
}

} // namespace redix
