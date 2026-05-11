#include "fd.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace redix {

FileDescriptor::FileDescriptor(int fd) noexcept : fd_(fd) {}

FileDescriptor::~FileDescriptor() {
    if (fd_ >= 0) {
        close(fd_);
    }
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)) {}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            close(fd_);
        }
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

int FileDescriptor::get() const noexcept {
    return fd_;
}

int FileDescriptor::release() noexcept {
    return std::exchange(fd_, -1);
}

FileDescriptor::operator bool() const noexcept {
    return fd_ >= 0;
}

std::expected<void, std::string> FileDescriptor::set_nonblocking() {
    if (fd_ < 0) {
        return std::unexpected(std::string{"invalid file descriptor"});
    }

    const int flags = fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
        return std::unexpected(std::string{"fcntl(F_GETFL): "} + std::strerror(errno));
    }

    if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        return std::unexpected(std::string{"fcntl(F_SETFL): "} + std::strerror(errno));
    }

    return {};
}

} // namespace redix
