#include "command.hpp"

#include "resp.hpp"
#include "store.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <flat_map>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace redix {

namespace {

using Handler = std::function<std::vector<std::byte>(const Command&, Store&, ServerContext&)>;
using DispatchTable = std::flat_map<std::string, Handler>;

std::uint64_t now_ms() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string uppercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
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

bool parse_u64(std::string_view s, std::uint64_t& out) {
    if (s.empty() || s.front() == '-') {
        return false;
    }

    const char* first = s.data();
    const char* last = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

std::vector<std::byte> wrong_arity() {
    return resp_error("wrong number of arguments");
}

std::vector<std::byte> syntax_error() {
    return resp_error("ERR syntax error");
}

std::vector<std::byte> integer_error() {
    return resp_error("ERR value is not an integer or out of range");
}

std::vector<std::byte> simple_string(std::string_view value) {
    std::vector<std::byte> out;
    out.reserve(value.size() + 3);
    out.push_back(static_cast<std::byte>('+'));
    for (const char c : value) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    out.push_back(static_cast<std::byte>('\r'));
    out.push_back(static_cast<std::byte>('\n'));
    return out;
}

std::vector<std::byte> store_error(const StoreError& error) {
    return resp_error(error.message);
}

std::vector<std::byte> wrong_type_error() {
    return resp_error("WRONGTYPE Operation against a key holding the wrong kind of value");
}

std::vector<std::byte> string_array(const std::vector<std::string>& values) {
    std::vector<RespValue> elems;
    elems.reserve(values.size());
    for (const std::string& value : values) {
        elems.emplace_back(value);
    }
    return resp_array(elems);
}

std::vector<std::byte> optional_string_array(const std::vector<std::optional<std::string>>& values) {
    std::vector<RespValue> elems;
    elems.reserve(values.size());
    for (const auto& value : values) {
        if (value.has_value()) {
            elems.emplace_back(*value);
        } else {
            elems.emplace_back(RespNull{});
        }
    }
    return resp_array(elems);
}

bool exact(const Command& cmd, std::size_t n) {
    return cmd.args.size() == n;
}

bool at_least(const Command& cmd, std::size_t n) {
    return cmd.args.size() >= n;
}

std::span<const std::string> tail_args(const Command& cmd, std::size_t offset) {
    return std::span<const std::string>{cmd.args.data() + offset, cmd.args.size() - offset};
}

bool is_key_not_found(const StoreError& error) {
    return error.kind == StoreErrorKind::KeyNotFound;
}

bool is_out_of_range(const StoreError& error) {
    return error.kind == StoreErrorKind::OutOfRange;
}

bool key_is_wrong_type_for_set_op(Store& store, const std::string& key) {
    const std::string type = store.type(key);
    return type != "none" && type != "set";
}

std::vector<std::byte> handle_set(const Command& cmd, Store& store, ServerContext&) {
    if (!at_least(cmd, 2)) {
        return wrong_arity();
    }

    std::uint64_t expires_at = 0;
    bool nx = false;
    bool xx = false;

    for (std::size_t i = 2; i < cmd.args.size(); ++i) {
        const std::string option = uppercase(cmd.args[i]);
        if (option == "EX" || option == "PX") {
            if (i + 1 >= cmd.args.size()) {
                return syntax_error();
            }

            std::uint64_t amount = 0;
            if (!parse_u64(cmd.args[++i], amount) || amount == 0) {
                return integer_error();
            }
            expires_at = now_ms() + (option == "EX" ? amount * 1000 : amount);
        } else if (option == "NX") {
            if (xx) {
                return syntax_error();
            }
            nx = true;
        } else if (option == "XX") {
            if (nx) {
                return syntax_error();
            }
            xx = true;
        } else {
            return syntax_error();
        }
    }

    const std::vector<std::string> key{cmd.args[0]};
    const bool exists = store.exists(key) > 0;
    if ((nx && exists) || (xx && !exists)) {
        return resp_null_bulk();
    }

    store.set(cmd.args[0], cmd.args[1], expires_at);
    return resp_ok();
}

std::vector<std::byte> handle_get(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }

    auto value = store.get(cmd.args[0]);
    if (!value) {
        return is_key_not_found(value.error()) ? resp_null_bulk() : store_error(value.error());
    }
    return resp_bulk(*value);
}

std::vector<std::byte> handle_del(const Command& cmd, Store& store, ServerContext&) {
    if (!at_least(cmd, 1)) {
        return wrong_arity();
    }
    return resp_integer(store.del(cmd.args));
}

std::vector<std::byte> handle_exists(const Command& cmd, Store& store, ServerContext&) {
    if (!at_least(cmd, 1)) {
        return wrong_arity();
    }
    return resp_integer(store.exists(cmd.args));
}

std::vector<std::byte> handle_incr(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }

    auto n = store.incr(cmd.args[0]);
    return n ? resp_integer(*n) : store_error(n.error());
}

std::vector<std::byte> handle_incrby(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 2)) {
        return wrong_arity();
    }

    std::int64_t delta = 0;
    if (!parse_i64(cmd.args[1], delta)) {
        return integer_error();
    }

    auto n = store.incrby(cmd.args[0], delta);
    return n ? resp_integer(*n) : store_error(n.error());
}

std::vector<std::byte> handle_decr(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }

    auto n = store.decr(cmd.args[0]);
    return n ? resp_integer(*n) : store_error(n.error());
}

std::vector<std::byte> handle_decrby(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 2)) {
        return wrong_arity();
    }

    std::int64_t delta = 0;
    if (!parse_i64(cmd.args[1], delta)) {
        return integer_error();
    }

    auto n = store.decrby(cmd.args[0], delta);
    return n ? resp_integer(*n) : store_error(n.error());
}

std::vector<std::byte> handle_append(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 2)) {
        return wrong_arity();
    }

    auto n = store.append(cmd.args[0], cmd.args[1]);
    return n ? resp_integer(*n) : store_error(n.error());
}

std::vector<std::byte> handle_strlen(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }

    auto n = store.strlen(cmd.args[0]);
    return n ? resp_integer(*n) : store_error(n.error());
}

std::vector<std::byte> handle_getset(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 2)) {
        return wrong_arity();
    }

    auto old = store.getset(cmd.args[0], cmd.args[1]);
    if (!old) {
        return is_key_not_found(old.error()) ? resp_null_bulk() : store_error(old.error());
    }
    return resp_bulk(*old);
}

std::vector<std::byte> handle_mset(const Command& cmd, Store& store, ServerContext&) {
    if (cmd.args.size() < 2 || cmd.args.size() % 2 != 0) {
        return wrong_arity();
    }

    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve(cmd.args.size() / 2);
    for (std::size_t i = 0; i < cmd.args.size(); i += 2) {
        pairs.emplace_back(cmd.args[i], cmd.args[i + 1]);
    }

    store.mset(pairs);
    return resp_ok();
}

std::vector<std::byte> handle_mget(const Command& cmd, Store& store, ServerContext&) {
    if (!at_least(cmd, 1)) {
        return wrong_arity();
    }
    return optional_string_array(store.mget(cmd.args));
}

std::vector<std::byte> handle_expire(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 2)) {
        return wrong_arity();
    }

    std::uint64_t amount = 0;
    if (!parse_u64(cmd.args[1], amount)) {
        return integer_error();
    }
    return resp_integer(store.expire(cmd.args[0], amount) ? 1 : 0);
}

std::vector<std::byte> handle_pexpire(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 2)) {
        return wrong_arity();
    }

    std::uint64_t amount = 0;
    if (!parse_u64(cmd.args[1], amount)) {
        return integer_error();
    }
    return resp_integer(store.pexpire(cmd.args[0], amount) ? 1 : 0);
}

std::vector<std::byte> handle_ttl(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }
    return resp_integer(store.ttl(cmd.args[0]));
}

std::vector<std::byte> handle_pttl(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }
    return resp_integer(store.pttl(cmd.args[0]));
}

std::vector<std::byte> handle_persist(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }
    return resp_integer(store.persist(cmd.args[0]) ? 1 : 0);
}

std::vector<std::byte> handle_lpush(const Command& cmd, Store& store, ServerContext&) {
    if (!at_least(cmd, 2)) {
        return wrong_arity();
    }

    const std::int64_t len = store.lpush(cmd.args[0], tail_args(cmd, 1));
    return len < 0 ? wrong_type_error() : resp_integer(len);
}

std::vector<std::byte> handle_rpush(const Command& cmd, Store& store, ServerContext&) {
    if (!at_least(cmd, 2)) {
        return wrong_arity();
    }

    const std::int64_t len = store.rpush(cmd.args[0], tail_args(cmd, 1));
    return len < 0 ? wrong_type_error() : resp_integer(len);
}

std::vector<std::byte> handle_lpop(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }

    auto value = store.lpop(cmd.args[0]);
    if (!value) {
        return is_key_not_found(value.error()) ? resp_null_bulk() : store_error(value.error());
    }
    return resp_bulk(*value);
}

std::vector<std::byte> handle_rpop(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }

    auto value = store.rpop(cmd.args[0]);
    if (!value) {
        return is_key_not_found(value.error()) ? resp_null_bulk() : store_error(value.error());
    }
    return resp_bulk(*value);
}

std::vector<std::byte> handle_lrange(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 3)) {
        return wrong_arity();
    }

    std::int64_t start = 0;
    std::int64_t stop = 0;
    if (!parse_i64(cmd.args[1], start) || !parse_i64(cmd.args[2], stop)) {
        return integer_error();
    }

    auto values = store.lrange(cmd.args[0], start, stop);
    if (!values) {
        return is_key_not_found(values.error()) ? string_array({}) : store_error(values.error());
    }
    return string_array(*values);
}

std::vector<std::byte> handle_llen(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }

    auto len = store.llen(cmd.args[0]);
    if (!len) {
        return is_key_not_found(len.error()) ? resp_integer(0) : store_error(len.error());
    }
    return resp_integer(*len);
}

std::vector<std::byte> handle_lindex(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 2)) {
        return wrong_arity();
    }

    std::int64_t index = 0;
    if (!parse_i64(cmd.args[1], index)) {
        return integer_error();
    }

    auto value = store.lindex(cmd.args[0], index);
    if (!value) {
        return is_key_not_found(value.error()) || is_out_of_range(value.error())
                   ? resp_null_bulk()
                   : store_error(value.error());
    }
    return resp_bulk(*value);
}

std::vector<std::byte> handle_hset(const Command& cmd, Store& store, ServerContext&) {
    if (cmd.args.size() < 3 || cmd.args.size() % 2 == 0) {
        return wrong_arity();
    }

    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve((cmd.args.size() - 1) / 2);
    for (std::size_t i = 1; i < cmd.args.size(); i += 2) {
        pairs.emplace_back(cmd.args[i], cmd.args[i + 1]);
    }

    const std::int64_t added = store.hset(cmd.args[0], pairs);
    return added < 0 ? wrong_type_error() : resp_integer(added);
}

std::vector<std::byte> handle_hget(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 2)) {
        return wrong_arity();
    }

    auto value = store.hget(cmd.args[0], cmd.args[1]);
    if (!value) {
        return is_key_not_found(value.error()) ? resp_null_bulk() : store_error(value.error());
    }
    return resp_bulk(*value);
}

std::vector<std::byte> handle_hdel(const Command& cmd, Store& store, ServerContext&) {
    if (!at_least(cmd, 2)) {
        return wrong_arity();
    }

    const std::int64_t removed = store.hdel(cmd.args[0], tail_args(cmd, 1));
    return removed < 0 ? wrong_type_error() : resp_integer(removed);
}

std::vector<std::byte> handle_hgetall(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }

    auto values = store.hgetall(cmd.args[0]);
    if (!values) {
        return is_key_not_found(values.error()) ? string_array({}) : store_error(values.error());
    }
    return string_array(*values);
}

std::vector<std::byte> handle_hexists(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 2)) {
        return wrong_arity();
    }

    auto exists = store.hexists(cmd.args[0], cmd.args[1]);
    if (!exists) {
        return is_key_not_found(exists.error()) ? resp_integer(0) : store_error(exists.error());
    }
    return resp_integer(*exists ? 1 : 0);
}

std::vector<std::byte> handle_hlen(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }

    auto len = store.hlen(cmd.args[0]);
    if (!len) {
        return is_key_not_found(len.error()) ? resp_integer(0) : store_error(len.error());
    }
    return resp_integer(*len);
}

std::vector<std::byte> handle_hkeys(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }

    auto values = store.hkeys(cmd.args[0]);
    if (!values) {
        return is_key_not_found(values.error()) ? string_array({}) : store_error(values.error());
    }
    return string_array(*values);
}

std::vector<std::byte> handle_hvals(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }

    auto values = store.hvals(cmd.args[0]);
    if (!values) {
        return is_key_not_found(values.error()) ? string_array({}) : store_error(values.error());
    }
    return string_array(*values);
}

std::vector<std::byte> handle_sadd(const Command& cmd, Store& store, ServerContext&) {
    if (!at_least(cmd, 2)) {
        return wrong_arity();
    }

    const std::int64_t added = store.sadd(cmd.args[0], tail_args(cmd, 1));
    return added < 0 ? wrong_type_error() : resp_integer(added);
}

std::vector<std::byte> handle_srem(const Command& cmd, Store& store, ServerContext&) {
    if (!at_least(cmd, 2)) {
        return wrong_arity();
    }

    const std::int64_t removed = store.srem(cmd.args[0], tail_args(cmd, 1));
    return removed < 0 ? wrong_type_error() : resp_integer(removed);
}

std::vector<std::byte> handle_smembers(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }

    auto values = store.smembers(cmd.args[0]);
    if (!values) {
        return is_key_not_found(values.error()) ? string_array({}) : store_error(values.error());
    }
    return string_array(*values);
}

std::vector<std::byte> handle_sismember(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 2)) {
        return wrong_arity();
    }

    auto exists = store.sismember(cmd.args[0], cmd.args[1]);
    if (!exists) {
        return is_key_not_found(exists.error()) ? resp_integer(0) : store_error(exists.error());
    }
    return resp_integer(*exists ? 1 : 0);
}

std::vector<std::byte> handle_scard(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }

    auto count = store.scard(cmd.args[0]);
    if (!count) {
        return is_key_not_found(count.error()) ? resp_integer(0) : store_error(count.error());
    }
    return resp_integer(*count);
}

std::vector<std::byte> handle_sunion(const Command& cmd, Store& store, ServerContext&) {
    if (!at_least(cmd, 1)) {
        return wrong_arity();
    }

    for (const std::string& key : cmd.args) {
        if (key_is_wrong_type_for_set_op(store, key)) {
            return wrong_type_error();
        }
    }
    return string_array(store.sunion(cmd.args));
}

std::vector<std::byte> handle_sinter(const Command& cmd, Store& store, ServerContext&) {
    if (!at_least(cmd, 1)) {
        return wrong_arity();
    }

    for (const std::string& key : cmd.args) {
        if (key_is_wrong_type_for_set_op(store, key)) {
            return wrong_type_error();
        }
    }
    return string_array(store.sinter(cmd.args));
}

std::vector<std::byte> handle_ping(const Command& cmd, Store&, ServerContext&) {
    if (cmd.args.empty()) {
        return simple_string("PONG");
    }
    if (cmd.args.size() == 1) {
        return resp_bulk(cmd.args[0]);
    }
    return wrong_arity();
}

std::vector<std::byte> handle_echo(const Command& cmd, Store&, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }
    return resp_bulk(cmd.args[0]);
}

std::vector<std::byte> handle_dbsize(const Command& cmd, Store& store, ServerContext&) {
    if (!cmd.args.empty()) {
        return wrong_arity();
    }
    return resp_integer(static_cast<std::int64_t>(store.dbsize()));
}

std::vector<std::byte> handle_flushdb(const Command& cmd, Store& store, ServerContext&) {
    if (!cmd.args.empty()) {
        return wrong_arity();
    }
    store.flushdb();
    return resp_ok();
}

std::vector<std::byte> handle_keys(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }
    return string_array(store.keys(cmd.args[0]));
}

std::vector<std::byte> handle_type(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 1)) {
        return wrong_arity();
    }
    return simple_string(store.type(cmd.args[0]));
}

std::vector<std::byte> handle_rename(const Command& cmd, Store& store, ServerContext&) {
    if (!exact(cmd, 2)) {
        return wrong_arity();
    }
    return store.rename(cmd.args[0], cmd.args[1]) ? resp_ok() : resp_error("ERR no such key");
}

const DispatchTable DISPATCH_TABLE = [] {
    DispatchTable t;

    t.emplace("APPEND", Handler{handle_append});
    t.emplace("DBSIZE", Handler{handle_dbsize});
    t.emplace("DECR", Handler{handle_decr});
    t.emplace("DECRBY", Handler{handle_decrby});
    t.emplace("DEL", Handler{handle_del});
    t.emplace("ECHO", Handler{handle_echo});
    t.emplace("EXISTS", Handler{handle_exists});
    t.emplace("EXPIRE", Handler{handle_expire});
    t.emplace("FLUSHDB", Handler{handle_flushdb});
    t.emplace("GET", Handler{handle_get});
    t.emplace("GETSET", Handler{handle_getset});
    t.emplace("HDEL", Handler{handle_hdel});
    t.emplace("HEXISTS", Handler{handle_hexists});
    t.emplace("HGET", Handler{handle_hget});
    t.emplace("HGETALL", Handler{handle_hgetall});
    t.emplace("HKEYS", Handler{handle_hkeys});
    t.emplace("HLEN", Handler{handle_hlen});
    t.emplace("HSET", Handler{handle_hset});
    t.emplace("HVALS", Handler{handle_hvals});
    t.emplace("INCR", Handler{handle_incr});
    t.emplace("INCRBY", Handler{handle_incrby});
    t.emplace("KEYS", Handler{handle_keys});
    t.emplace("LINDEX", Handler{handle_lindex});
    t.emplace("LLEN", Handler{handle_llen});
    t.emplace("LPOP", Handler{handle_lpop});
    t.emplace("LPUSH", Handler{handle_lpush});
    t.emplace("LRANGE", Handler{handle_lrange});
    t.emplace("MGET", Handler{handle_mget});
    t.emplace("MSET", Handler{handle_mset});
    t.emplace("PERSIST", Handler{handle_persist});
    t.emplace("PEXPIRE", Handler{handle_pexpire});
    t.emplace("PING", Handler{handle_ping});
    t.emplace("PTTL", Handler{handle_pttl});
    t.emplace("RENAME", Handler{handle_rename});
    t.emplace("RPOP", Handler{handle_rpop});
    t.emplace("RPUSH", Handler{handle_rpush});
    t.emplace("SADD", Handler{handle_sadd});
    t.emplace("SCARD", Handler{handle_scard});
    t.emplace("SET", Handler{handle_set});
    t.emplace("SINTER", Handler{handle_sinter});
    t.emplace("SISMEMBER", Handler{handle_sismember});
    t.emplace("SMEMBERS", Handler{handle_smembers});
    t.emplace("SREM", Handler{handle_srem});
    t.emplace("STRLEN", Handler{handle_strlen});
    t.emplace("SUNION", Handler{handle_sunion});
    t.emplace("TTL", Handler{handle_ttl});
    t.emplace("TYPE", Handler{handle_type});

    return t;
}();

} // namespace

std::expected<Command, std::vector<std::byte>> parse_command(const RespValue& resp) {
    const auto* array = std::get_if<RespValue::Array>(&resp.value);
    if (array == nullptr) {
        return std::unexpected(resp_error("ERR Protocol error: expected array"));
    }
    if (array->empty()) {
        return std::unexpected(resp_error("ERR Protocol error: expected command"));
    }

    Command cmd;
    cmd.args.reserve(array->size() - 1);

    for (std::size_t i = 0; i < array->size(); ++i) {
        const auto* bulk = std::get_if<std::string>(&(*array)[i].value);
        if (bulk == nullptr) {
            return std::unexpected(resp_error("ERR Protocol error: expected bulk string"));
        }

        std::string text = *bulk;
        if (i == 0) {
            cmd.name = uppercase(std::move(text));
        } else {
            cmd.args.push_back(std::move(text));
        }
    }

    if (cmd.name.empty()) {
        return std::unexpected(resp_error("ERR Protocol error: empty command"));
    }

    return cmd;
}

std::vector<std::byte> dispatch(const Command& cmd, Store& store, ServerContext& ctx) {
    const auto it = DISPATCH_TABLE.find(cmd.name);
    if (it == DISPATCH_TABLE.end()) {
        std::string msg = "ERR unknown command '";
        msg.append(cmd.name);
        msg.push_back('\'');
        return resp_error(msg);
    }

    return it->second(cmd, store, ctx);
}

bool is_write_command(const Command& cmd) {
    static const std::vector<std::string> write_commands{
        "SET",     "DEL",     "INCR",    "INCRBY", "DECR",    "DECRBY", "APPEND",
        "GETSET",  "MSET",    "EXPIRE",  "PEXPIRE", "PERSIST", "LPUSH",  "RPUSH",
        "LPOP",    "RPOP",    "HSET",    "HDEL",   "SADD",    "SREM",   "FLUSHDB",
        "RENAME",
    };

    return std::find(write_commands.begin(), write_commands.end(), cmd.name) != write_commands.end();
}

} // namespace redix
