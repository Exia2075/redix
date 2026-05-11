#include "macro.hpp"

#include "command.hpp"
#include "resp.hpp"
#include "store.hpp"

#include <criterion/criterion.h>

#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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

std::string dispatch_text(redix::Store& store,
                          std::string name,
                          std::vector<std::string> args = {}) {
    redix::ServerContext ctx;
    return text(redix::dispatch(redix::Command{std::move(name), std::move(args)}, store, ctx));
}

std::vector<std::string> strings(std::initializer_list<const char*> values) {
    std::vector<std::string> out;
    out.reserve(values.size());
    for (const char* value : values) {
        out.emplace_back(value);
    }
    return out;
}

} // namespace

Test(store_string, set_get_del_and_exists) {
    redix::Store store;
    store.set("foo", "bar");

    auto value = store.get("foo");
    cr_assert(value.has_value());
    cr_assert_str_eq(value->c_str(), "bar");

    std::vector<std::string> keys = strings({"foo", "missing"});
    cr_assert_eq(store.exists(keys), 1);
    cr_assert_eq(store.del(keys), 1);
    auto missing = store.get("foo");
    cr_assert(!missing.has_value());
    cr_assert_eq(missing.error().kind, redix::StoreErrorKind::KeyNotFound);
}

Test(store_string, integer_commands_and_type_errors) {
    redix::Store store;

    cr_assert_eq(*store.incr("n"), 1);
    cr_assert_eq(*store.incrby("n", 4), 5);
    cr_assert_eq(*store.decr("n"), 4);
    cr_assert_eq(*store.decrby("n", 10), -6);

    store.set("bad", "nope");
    auto bad = store.incr("bad");
    cr_assert(!bad.has_value());
    cr_assert_eq(bad.error().kind, redix::StoreErrorKind::NotAnInteger);

    std::vector<std::string> values = strings({"a"});
    store.lpush("list", values);
    auto wrong = store.get("list");
    cr_assert(!wrong.has_value());
    cr_assert_eq(wrong.error().kind, redix::StoreErrorKind::WrongType);
}

Test(store_string, append_strlen_getset_mset_and_mget) {
    redix::Store store;

    cr_assert_eq(*store.append("msg", "he"), 2);
    cr_assert_eq(*store.append("msg", "llo"), 5);
    cr_assert_eq(*store.strlen("msg"), 5);

    auto old = store.getset("msg", "bye");
    cr_assert(old.has_value());
    cr_assert_str_eq(old->c_str(), "hello");
    cr_assert_str_eq(store.get("msg")->c_str(), "bye");

    std::vector<std::pair<std::string, std::string>> pairs{{"a", "1"}, {"b", "2"}};
    store.mset(pairs);
    std::vector<std::string> keys = strings({"a", "missing", "b"});
    const auto values = store.mget(keys);
    cr_assert(values[0].has_value());
    cr_assert_str_eq(values[0]->c_str(), "1");
    cr_assert(!values[1].has_value());
    cr_assert(values[2].has_value());
    cr_assert_str_eq(values[2]->c_str(), "2");
}

Test(store_expiry, lazy_expiry_removes_key_on_access) {
    redix::Store store;
    store.set("temp", "value");
    cr_assert(store.expire("temp", 1));
    cr_assert_gt(store.ttl("temp"), 0);

    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto value = store.get("temp");
    cr_assert(!value.has_value());
    cr_assert_eq(value.error().kind, redix::StoreErrorKind::KeyNotFound);
    cr_assert_eq(store.ttl("temp"), -2);
}

Test(store_expiry, pexpire_persist_pttl_and_sweep) {
    redix::Store store;
    store.set("a", "1");
    store.set("b", "2");

    cr_assert(store.pexpire("a", 20));
    cr_assert_geq(store.pttl("a"), 0);
    cr_assert(store.persist("a"));
    cr_assert_eq(store.pttl("a"), -1);

    cr_assert(store.pexpire("b", 20));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    store.sweep_expired();
    auto expired = store.get("b");
    cr_assert(!expired.has_value());
    cr_assert_eq(expired.error().kind, redix::StoreErrorKind::KeyNotFound);
}

Test(store_list, push_pop_range_len_and_index) {
    redix::Store store;
    std::vector<std::string> left = strings({"a", "b"});
    std::vector<std::string> right = strings({"c"});

    cr_assert_eq(store.lpush("list", left), 2);
    cr_assert_eq(store.rpush("list", right), 3);
    cr_assert_eq(*store.llen("list"), 3);

    auto range = store.lrange("list", 0, -1);
    cr_assert(range.has_value());
    cr_assert_eq(range->size(), 3);
    cr_assert_str_eq((*range)[0].c_str(), "b");
    cr_assert_str_eq((*range)[1].c_str(), "a");
    cr_assert_str_eq((*range)[2].c_str(), "c");

    cr_assert_str_eq(store.lindex("list", -1)->c_str(), "c");
    cr_assert_str_eq(store.lpop("list")->c_str(), "b");
    cr_assert_str_eq(store.rpop("list")->c_str(), "c");
}

Test(store_hash, hset_hget_hdel_and_enumeration) {
    redix::Store store;
    std::vector<std::pair<std::string, std::string>> pairs{{"name", "Alice"}, {"age", "30"}};

    cr_assert_eq(store.hset("user", pairs), 2);
    cr_assert_eq(store.hset("user", pairs), 0);
    cr_assert_str_eq(store.hget("user", "name")->c_str(), "Alice");
    cr_assert(*store.hexists("user", "age"));
    cr_assert_eq(*store.hlen("user"), 2);

    auto all = store.hgetall("user");
    cr_assert(all.has_value());
    cr_assert_eq(all->size(), 4);

    auto keys = store.hkeys("user");
    auto vals = store.hvals("user");
    cr_assert(keys.has_value());
    cr_assert(vals.has_value());
    cr_assert_eq(keys->size(), 2);
    cr_assert_eq(vals->size(), 2);

    std::vector<std::string> fields = strings({"age", "missing"});
    cr_assert_eq(store.hdel("user", fields), 1);
    auto age_exists = store.hexists("user", "age");
    cr_assert(age_exists.has_value());
    cr_assert(!*age_exists);
}

Test(store_set, sadd_srem_membership_cardinality_and_set_ops) {
    redix::Store store;
    std::vector<std::string> a = strings({"b", "a", "a"});
    std::vector<std::string> b = strings({"b", "c"});

    cr_assert_eq(store.sadd("a", a), 2);
    cr_assert_eq(store.sadd("b", b), 2);
    cr_assert(*store.sismember("a", "a"));
    cr_assert_eq(*store.scard("a"), 2);

    auto members = store.smembers("a");
    cr_assert(members.has_value());
    cr_assert_eq(members->size(), 2);
    cr_assert_str_eq((*members)[0].c_str(), "a");
    cr_assert_str_eq((*members)[1].c_str(), "b");

    std::vector<std::string> keys = strings({"a", "b"});
    auto unioned = store.sunion(keys);
    cr_assert_eq(unioned.size(), 3);
    cr_assert_str_eq(unioned[0].c_str(), "a");
    cr_assert_str_eq(unioned[1].c_str(), "b");
    cr_assert_str_eq(unioned[2].c_str(), "c");

    auto intersected = store.sinter(keys);
    cr_assert_eq(intersected.size(), 1);
    cr_assert_str_eq(intersected[0].c_str(), "b");

    std::vector<std::string> remove = strings({"a", "missing"});
    cr_assert_eq(store.srem("a", remove), 1);
}

Test(command_dispatch, set_options_get_and_missing_values) {
    redix::Store store;

    cr_assert_str_eq(dispatch_text(store, "SET", strings({"k", "v", "NX"})).c_str(), "+OK\r\n");
    cr_assert_str_eq(dispatch_text(store, "SET", strings({"k", "new", "NX"})).c_str(), "$-1\r\n");
    cr_assert_str_eq(dispatch_text(store, "GET", strings({"k"})).c_str(), "$1\r\nv\r\n");

    cr_assert_str_eq(dispatch_text(store, "SET", strings({"missing", "x", "XX"})).c_str(), "$-1\r\n");
    cr_assert_str_eq(dispatch_text(store, "SET", strings({"k", "new", "XX"})).c_str(), "+OK\r\n");
    cr_assert_str_eq(dispatch_text(store, "GET", strings({"k"})).c_str(), "$3\r\nnew\r\n");

    cr_assert_str_eq(dispatch_text(store, "SET", strings({"short", "life", "PX", "20"})).c_str(),
                     "+OK\r\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    cr_assert_str_eq(dispatch_text(store, "GET", strings({"short"})).c_str(), "$-1\r\n");
}

Test(command_dispatch, string_expiry_and_collection_responses) {
    redix::Store store;

    cr_assert_str_eq(dispatch_text(store, "MSET", strings({"a", "1", "b", "2"})).c_str(), "+OK\r\n");
    cr_assert_str_eq(dispatch_text(store, "MGET", strings({"a", "missing", "b"})).c_str(),
                     "*3\r\n$1\r\n1\r\n$-1\r\n$1\r\n2\r\n");
    cr_assert_str_eq(dispatch_text(store, "EXISTS", strings({"a", "b", "missing"})).c_str(),
                     ":2\r\n");
    cr_assert_str_eq(dispatch_text(store, "DEL", strings({"a"})).c_str(), ":1\r\n");

    cr_assert_str_eq(dispatch_text(store, "LPUSH", strings({"list", "a", "b"})).c_str(), ":2\r\n");
    cr_assert_str_eq(dispatch_text(store, "LRANGE", strings({"list", "0", "-1"})).c_str(),
                     "*2\r\n$1\r\nb\r\n$1\r\na\r\n");

    cr_assert_str_eq(dispatch_text(store, "HSET", strings({"user", "name", "Ada"})).c_str(),
                     ":1\r\n");
    cr_assert_str_eq(dispatch_text(store, "HGET", strings({"user", "name"})).c_str(),
                     "$3\r\nAda\r\n");

    cr_assert_str_eq(dispatch_text(store, "SADD", strings({"tags", "cpp", "redis", "cpp"})).c_str(),
                     ":2\r\n");
    cr_assert_str_eq(dispatch_text(store, "SMEMBERS", strings({"tags"})).c_str(),
                     "*2\r\n$3\r\ncpp\r\n$5\r\nredis\r\n");
}
