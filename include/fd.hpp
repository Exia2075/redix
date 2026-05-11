#pragma once

#include <expected>
#include <string>

namespace redix {

class FileDescriptor {
public:
    constexpr FileDescriptor() noexcept = default;
    [[nodiscard]] explicit FileDescriptor(int fd) noexcept;
    ~FileDescriptor();

    FileDescriptor(FileDescriptor&&) noexcept;
    FileDescriptor& operator=(FileDescriptor&&) noexcept;

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] int release() noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;

    std::expected<void, std::string> set_nonblocking();

private:
    int fd_{-1};
};

} // namespace redix
