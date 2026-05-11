#include "command.hpp"
#include "resp.hpp"
#include "store.hpp"

#include <criterion/criterion.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string text(std::span<const std::byte> data) {
    std::string out;
    out.reserve(data.size());
    for (const std::byte b : data) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

std::string dispatch_text(redix::Command cmd) {
    redix::Store store;
    redix::ServerContext ctx;
    return text(redix::dispatch(cmd, store, ctx));
}

void assert_bytes_eq(const std::vector<std::byte>& actual, const char* expected) {
    const std::string actual_text = text(actual);
    cr_assert_str_eq(actual_text.c_str(), expected);
}

} // namespace

Test(command_parse, accepts_array_of_bulk_strings_and_uppercases_name) {
    redix::RespValue resp{redix::RespValue::Array{
        redix::RespValue{std::string{"set"}},
        redix::RespValue{std::string{"key"}},
        redix::RespValue{std::string{"value"}},
    }};

    auto cmd = redix::parse_command(resp);

    cr_assert(cmd.has_value());
    cr_assert_str_eq(cmd->name.c_str(), "SET");
    cr_assert_eq(cmd->args.size(), 2);
    cr_assert_str_eq(cmd->args[0].c_str(), "key");
    cr_assert_str_eq(cmd->args[1].c_str(), "value");
}

Test(command_parse, rejects_non_array_values) {
    auto cmd = redix::parse_command(redix::RespValue{std::string{"PING"}});

    cr_assert(!cmd.has_value());
    assert_bytes_eq(cmd.error(), "-ERR Protocol error: expected array\r\n");
}

Test(command_parse, rejects_non_bulk_array_elements) {
    redix::RespValue resp{redix::RespValue::Array{
        redix::RespValue{std::string{"GET"}},
        redix::RespValue{std::int64_t{7}},
    }};

    auto cmd = redix::parse_command(resp);

    cr_assert(!cmd.has_value());
    assert_bytes_eq(cmd.error(), "-ERR Protocol error: expected bulk string\r\n");
}

Test(command_dispatch, uses_generic_arity_error_from_table_handler) {
    cr_assert_str_eq(dispatch_text(redix::Command{"GET", {}}).c_str(),
                     "-wrong number of arguments\r\n");
}

Test(command_dispatch, uses_dispatch_table_for_known_and_unknown_commands) {
    cr_assert_str_eq(dispatch_text(redix::Command{"PING", {}}).c_str(), "+PONG\r\n");
    cr_assert_str_eq(dispatch_text(redix::Command{"NOPE", {}}).c_str(),
                     "-ERR unknown command 'NOPE'\r\n");
}
