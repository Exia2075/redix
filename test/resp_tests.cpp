#include "macro.hpp"

#include "resp.hpp"

#include <criterion/criterion.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

std::vector<std::byte> bytes(std::string_view s) {
    std::vector<std::byte> out;
    out.reserve(s.size());
    for (const char c : s) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string text(std::span<const std::byte> data) {
    std::string out;
    out.reserve(data.size());
    for (const std::byte b : data) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

void feed(redix::RespParser& parser, std::string_view input) {
    const std::vector<std::byte> data = bytes(input);
    parser.feed(data);
}

void assert_bytes_eq(const std::vector<std::byte>& actual, const char* expected) {
    const std::string actual_text = text(actual);
    cr_assert_str_eq(actual_text.c_str(), expected);
}

const std::string& as_string(const redix::RespValue& value) {
    cr_assert(std::holds_alternative<std::string>(value.value));
    return std::get<std::string>(value.value);
}

std::int64_t as_integer(const redix::RespValue& value) {
    cr_assert(std::holds_alternative<std::int64_t>(value.value));
    return std::get<std::int64_t>(value.value);
}

const redix::RespValue::Array& as_array(const redix::RespValue& value) {
    cr_assert(std::holds_alternative<redix::RespValue::Array>(value.value));
    return std::get<redix::RespValue::Array>(value.value);
}

} // namespace

Test(resp_serialiser, encodes_simple_values) {
    assert_bytes_eq(redix::resp_ok(), "+OK\r\n");
    assert_bytes_eq(redix::resp_error("ERR nope"), "-ERR nope\r\n");
    assert_bytes_eq(redix::resp_integer(42), ":42\r\n");
    assert_bytes_eq(redix::resp_integer(-7), ":-7\r\n");
    assert_bytes_eq(redix::resp_bulk("foo"), "$3\r\nfoo\r\n");
    assert_bytes_eq(redix::resp_null_bulk(), "$-1\r\n");
}

Test(resp_serialiser, encodes_arrays_with_bulk_string_elements) {
    const std::vector<redix::RespValue> elems{
        redix::RespValue{std::string{"SET"}},
        redix::RespValue{std::string{"foo"}},
        redix::RespValue{std::string{"bar"}},
    };

    assert_bytes_eq(redix::resp_array(elems), "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
}

Test(resp_parser, parses_complete_single_message) {
    redix::RespParser parser;
    feed(parser, "$3\r\nfoo\r\n");

    const redix::ParseResult result = parser.try_parse();

    cr_assert_eq(result.status, redix::ParseStatus::Complete);
    cr_assert_eq(result.consumed, 9);
    cr_assert_str_eq(as_string(result.value).c_str(), "foo");
    cr_assert_eq(parser.try_parse().status, redix::ParseStatus::Incomplete);
}

Test(resp_parser, parses_message_split_at_every_possible_point) {
    const std::string wire = "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";

    for (std::size_t split = 0; split <= wire.size(); ++split) {
        redix::RespParser parser;
        feed(parser, std::string_view{wire}.substr(0, split));

        const redix::ParseResult first = parser.try_parse();
        if (split < wire.size()) {
            cr_assert_eq(first.status, redix::ParseStatus::Incomplete,
                         "expected incomplete parse at split %zu", split);
        }

        feed(parser, std::string_view{wire}.substr(split));
        const redix::ParseResult result = split == wire.size() ? first : parser.try_parse();

        cr_assert_eq(result.status, redix::ParseStatus::Complete,
                     "expected complete parse at split %zu", split);
        const redix::RespValue::Array& arr = as_array(result.value);
        cr_assert_eq(arr.size(), 3);
        cr_assert_str_eq(as_string(arr[0]).c_str(), "SET");
        cr_assert_str_eq(as_string(arr[1]).c_str(), "foo");
        cr_assert_str_eq(as_string(arr[2]).c_str(), "bar");
    }
}

Test(resp_parser, parses_array_of_bulk_strings_command_format) {
    redix::RespParser parser;
    feed(parser, "*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n");

    const redix::ParseResult result = parser.try_parse();

    cr_assert_eq(result.status, redix::ParseStatus::Complete);
    const redix::RespValue::Array& arr = as_array(result.value);
    cr_assert_eq(arr.size(), 2);
    cr_assert_str_eq(as_string(arr[0]).c_str(), "GET");
    cr_assert_str_eq(as_string(arr[1]).c_str(), "key");
}

Test(resp_parser, parses_integer_and_error_values) {
    redix::RespParser parser;
    feed(parser, ":100\r\n-ERR bad\r\n");

    redix::ParseResult result = parser.try_parse();
    cr_assert_eq(result.status, redix::ParseStatus::Complete);
    cr_assert_eq(as_integer(result.value), 100);

    result = parser.try_parse();
    cr_assert_eq(result.status, redix::ParseStatus::Complete);
    cr_assert_str_eq(std::get<redix::RespError>(result.value.value).message.c_str(), "ERR bad");
}

Test(resp_parser, reports_malformed_type_byte) {
    redix::RespParser parser;
    feed(parser, "~nope\r\n");

    const redix::ParseResult result = parser.try_parse();

    cr_assert_eq(result.status, redix::ParseStatus::Error);
}
