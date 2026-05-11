#include "client.hpp"

#include "aof.hpp"
#include "command.hpp"
#include "store.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <utility>

namespace redix {

Client::Client(FileDescriptor fd, std::string remote_addr)
    : fd_{std::move(fd)}, remote_addr_{std::move(remote_addr)} {}

int Client::fd() const noexcept {
    return fd_.get();
}

const std::string& Client::remote_addr() const noexcept {
    return remote_addr_;
}

ClientState Client::state() const noexcept {
    return state_;
}

bool Client::on_readable(Store& store, Aof& aof, ServerContext& ctx) {
    std::array<std::byte, 4096> tmp{};

    while (true) {
        const ssize_t n = recv(fd_.get(), tmp.data(), tmp.size(), 0);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return false;
        }

        parser_.feed(std::span<const std::byte>{tmp.data(), static_cast<std::size_t>(n)});

        while (true) {
            ParseResult result = parser_.try_parse();
            if (result.status == ParseStatus::Incomplete) {
                break;
            }
            if (result.status == ParseStatus::Error) {
                parser_.reset();
                return false;
            }

            auto cmd_or_err = parse_command(result.value);
            if (!cmd_or_err) {
                enqueue_response(cmd_or_err.error());
                continue;
            }

            std::vector<std::byte> response = dispatch(*cmd_or_err, store, ctx);

            if (aof.is_open() && is_write_command(*cmd_or_err)) {
                if (auto appended = aof.append(*cmd_or_err); !appended) {
                    enqueue_response(resp_error(appended.error()));
                    continue;
                }
            }

            enqueue_response(std::move(response));
        }
    }

    return true;
}

bool Client::on_writable() {
    while (write_offset_ < write_buf_.size()) {
        const auto* data = write_buf_.data() + write_offset_;
        const std::size_t remaining = write_buf_.size() - write_offset_;
        const ssize_t n = send(fd_.get(), data, remaining, MSG_NOSIGNAL);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;
            }
            return false;
        }
        if (n == 0) {
            return true;
        }

        write_offset_ += static_cast<std::size_t>(n);
    }

    write_buf_.clear();
    write_offset_ = 0;
    state_ = ClientState::Reading;
    return true;
}

void Client::enqueue_response(std::vector<std::byte> data) {
    write_buf_.insert(write_buf_.end(), data.begin(), data.end());
    state_ = ClientState::Writing;
}

} // namespace redix
