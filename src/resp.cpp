#include "resp.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace redix {

namespace {

constexpr std::string_view CRLF = "\r\n";

void append_ascii(std::vector<std::byte>& out, std::string_view s) {
    out.reserve(out.size() + s.size());
    for (const char c : s) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
}

char as_char(std::byte b) {
    return static_cast<char>(b);
}

std::size_t find_crlf(const std::vector<std::byte>& buf, std::size_t pos) {
    for (std::size_t i = pos; i + 1 < buf.size(); ++i) {
        if (as_char(buf[i]) == '\r' && as_char(buf[i + 1]) == '\n') {
            return i;
        }
    }
    return std::string::npos;
}

std::string read_string(const std::vector<std::byte>& buf, std::size_t start, std::size_t end) {
    std::string out;
    out.reserve(end - start);
    for (std::size_t i = start; i < end; ++i) {
        out.push_back(as_char(buf[i]));
    }
    return out;
}

bool parse_i64(std::string_view s, std::int64_t& out) {
    if (s.empty()) {
        return false;
    }

    const char* first = s.data();
    const char* last = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

ParseResult parse_at(const std::vector<std::byte>& buf, std::size_t pos) {
    if (pos >= buf.size()) {
        return {ParseStatus::Incomplete, {}, 0};
    }

    const char type = as_char(buf[pos]);
    const std::size_t payload_start = pos + 1;

    switch (type) {
    case '+': {
        const std::size_t line_end = find_crlf(buf, payload_start);
        if (line_end == std::string::npos) {
            return {ParseStatus::Incomplete, {}, 0};
        }
        return {ParseStatus::Complete,
                RespValue{read_string(buf, payload_start, line_end)},
                line_end + CRLF.size() - pos};
    }
    case '-': {
        const std::size_t line_end = find_crlf(buf, payload_start);
        if (line_end == std::string::npos) {
            return {ParseStatus::Incomplete, {}, 0};
        }
        return {ParseStatus::Complete,
                RespValue{RespError{read_string(buf, payload_start, line_end)}},
                line_end + CRLF.size() - pos};
    }
    case ':': {
        const std::size_t line_end = find_crlf(buf, payload_start);
        if (line_end == std::string::npos) {
            return {ParseStatus::Incomplete, {}, 0};
        }

        std::int64_t n = 0;
        const std::string line = read_string(buf, payload_start, line_end);
        if (!parse_i64(line, n)) {
            return {ParseStatus::Error, {}, 0};
        }
        return {ParseStatus::Complete, RespValue{n}, line_end + CRLF.size() - pos};
    }
    case '$': {
        const std::size_t line_end = find_crlf(buf, payload_start);
        if (line_end == std::string::npos) {
            return {ParseStatus::Incomplete, {}, 0};
        }

        std::int64_t len = 0;
        const std::string line = read_string(buf, payload_start, line_end);
        if (!parse_i64(line, len) || len < -1) {
            return {ParseStatus::Error, {}, 0};
        }
        if (len == -1) {
            return {ParseStatus::Complete, RespValue{RespNull{}}, line_end + CRLF.size() - pos};
        }

        const auto bulk_len = static_cast<std::size_t>(len);
        const std::size_t data_start = line_end + CRLF.size();
        const std::size_t data_end = data_start + bulk_len;
        if (data_end + CRLF.size() > buf.size()) {
            return {ParseStatus::Incomplete, {}, 0};
        }
        if (as_char(buf[data_end]) != '\r' || as_char(buf[data_end + 1]) != '\n') {
            return {ParseStatus::Error, {}, 0};
        }

        return {ParseStatus::Complete,
                RespValue{read_string(buf, data_start, data_end)},
                data_end + CRLF.size() - pos};
    }
    case '*': {
        const std::size_t line_end = find_crlf(buf, payload_start);
        if (line_end == std::string::npos) {
            return {ParseStatus::Incomplete, {}, 0};
        }

        std::int64_t len = 0;
        const std::string line = read_string(buf, payload_start, line_end);
        if (!parse_i64(line, len) || len < -1) {
            return {ParseStatus::Error, {}, 0};
        }
        if (len == -1) {
            return {ParseStatus::Complete, RespValue{RespNull{}}, line_end + CRLF.size() - pos};
        }

        RespValue::Array values;
        values.reserve(static_cast<std::size_t>(len));

        std::size_t cursor = line_end + CRLF.size();
        for (std::int64_t i = 0; i < len; ++i) {
            const ParseResult child = parse_at(buf, cursor);
            if (child.status != ParseStatus::Complete) {
                return {child.status, {}, 0};
            }
            cursor += child.consumed;
            values.push_back(child.value);
        }

        return {ParseStatus::Complete, RespValue{std::move(values)}, cursor - pos};
    }
    default:
        return {ParseStatus::Error, {}, 0};
    }
}

} // namespace

void RespParser::feed(std::span<const std::byte> data) {
    buf_.insert(buf_.end(), data.begin(), data.end());
}

ParseResult RespParser::try_parse() {
    ParseResult result = parse_at(buf_, 0);
    if (result.status == ParseStatus::Complete) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(result.consumed));
    }
    return result;
}

void RespParser::reset() {
    buf_.clear();
}

void resp_encode(const RespValue& value, std::vector<std::byte>& out) {
    std::visit(
        [&out](const auto& v) {
            using T = std::decay_t<decltype(v)>;

            if constexpr (std::is_same_v<T, RespNull>) {
                append_ascii(out, "$-1\r\n");
            } else if constexpr (std::is_same_v<T, std::string>) {
                append_ascii(out, "$");
                append_ascii(out, std::to_string(v.size()));
                append_ascii(out, "\r\n");
                append_ascii(out, v);
                append_ascii(out, "\r\n");
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                append_ascii(out, ":");
                append_ascii(out, std::to_string(v));
                append_ascii(out, "\r\n");
            } else if constexpr (std::is_same_v<T, RespError>) {
                append_ascii(out, "-");
                append_ascii(out, v.message);
                append_ascii(out, "\r\n");
            } else if constexpr (std::is_same_v<T, RespValue::Array>) {
                append_ascii(out, "*");
                append_ascii(out, std::to_string(v.size()));
                append_ascii(out, "\r\n");
                for (const RespValue& elem : v) {
                    resp_encode(elem, out);
                }
            }
        },
        value.value);
}

std::vector<std::byte> resp_ok() {
    std::vector<std::byte> out;
    append_ascii(out, "+OK\r\n");
    return out;
}

std::vector<std::byte> resp_error(std::string_view msg) {
    std::vector<std::byte> out;
    append_ascii(out, "-");
    append_ascii(out, msg);
    append_ascii(out, "\r\n");
    return out;
}

std::vector<std::byte> resp_integer(std::int64_t n) {
    std::vector<std::byte> out;
    append_ascii(out, ":");
    append_ascii(out, std::to_string(n));
    append_ascii(out, "\r\n");
    return out;
}

std::vector<std::byte> resp_bulk(std::string_view s) {
    std::vector<std::byte> out;
    append_ascii(out, "$");
    append_ascii(out, std::to_string(s.size()));
    append_ascii(out, "\r\n");
    append_ascii(out, s);
    append_ascii(out, "\r\n");
    return out;
}

std::vector<std::byte> resp_null_bulk() {
    std::vector<std::byte> out;
    append_ascii(out, "$-1\r\n");
    return out;
}

std::vector<std::byte> resp_array(const std::vector<RespValue>& elems) {
    std::vector<std::byte> out;
    append_ascii(out, "*");
    append_ascii(out, std::to_string(elems.size()));
    append_ascii(out, "\r\n");
    for (const RespValue& elem : elems) {
        resp_encode(elem, out);
    }
    return out;
}

} // namespace redix
