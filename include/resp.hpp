#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace redix {

struct RespNull {};

struct RespError {
    std::string message;
};

struct RespValue {
    using Array = std::vector<RespValue>;
    using Variant = std::variant<RespNull, std::string, std::int64_t, RespError, Array>;

    Variant value{RespNull{}};

    RespValue() = default;
    RespValue(RespNull v) : value(v) {}
    RespValue(std::string v) : value(std::move(v)) {}
    RespValue(std::int64_t v) : value(v) {}
    RespValue(RespError v) : value(std::move(v)) {}
    RespValue(Array v) : value(std::move(v)) {}
};

enum class ParseStatus {
    Complete,
    Incomplete,
    Error,
};

struct ParseResult {
    ParseStatus status{ParseStatus::Incomplete};
    RespValue value{};
    std::size_t consumed{0};
};

class RespParser {
public:
    void feed(std::span<const std::byte> data);
    ParseResult try_parse();
    void reset();

private:
    std::vector<std::byte> buf_;
};

void resp_encode(const RespValue& value, std::vector<std::byte>& out);

std::vector<std::byte> resp_ok();
std::vector<std::byte> resp_error(std::string_view msg);
std::vector<std::byte> resp_integer(std::int64_t n);
std::vector<std::byte> resp_bulk(std::string_view s);
std::vector<std::byte> resp_null_bulk();
std::vector<std::byte> resp_array(const std::vector<RespValue>& elems);

} // namespace redix
