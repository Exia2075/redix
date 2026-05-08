#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <flat_map>
#include <flat_set>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace redix {

struct StringValue {
    std::string data;
};

struct ListValue {
    std::deque<std::string> data;
};

struct HashValue {
    std::flat_map<std::string, std::string> data;
};

struct SetValue {
    std::flat_set<std::string> data;
};

using RedisValue = std::variant<StringValue, ListValue, HashValue, SetValue>;

struct Entry {
    RedisValue value;
    std::uint64_t expires_at_ms{0};

    bool is_expired() const noexcept;
};

enum class StoreErrorKind {
    KeyNotFound,
    WrongType,
    NotAnInteger,
    OutOfRange,
};

struct StoreError {
    StoreErrorKind kind;
    std::string message;
};

class Store {
public:
    Store() = default;

    void set(std::string key, std::string value, std::uint64_t expires_at_ms = 0);
    std::expected<std::string, StoreError> get(std::string_view key);
    std::int64_t del(std::span<const std::string> keys);
    std::int64_t exists(std::span<const std::string> keys);
    std::expected<std::int64_t, StoreError> incr(std::string_view key);
    std::expected<std::int64_t, StoreError> incrby(std::string_view key, std::int64_t delta);
    std::expected<std::int64_t, StoreError> decr(std::string_view key);
    std::expected<std::int64_t, StoreError> decrby(std::string_view key, std::int64_t delta);
    std::expected<std::int64_t, StoreError> append(std::string_view key, std::string_view suffix);
    std::expected<std::int64_t, StoreError> strlen(std::string_view key);
    std::expected<std::string, StoreError> getset(std::string key, std::string value);
    void mset(std::span<const std::pair<std::string, std::string>> pairs);
    std::vector<std::optional<std::string>> mget(std::span<const std::string> keys);

    bool expire(std::string_view key, std::uint64_t seconds);
    bool pexpire(std::string_view key, std::uint64_t ms);
    bool persist(std::string_view key);
    std::int64_t ttl(std::string_view key);
    std::int64_t pttl(std::string_view key);

    std::int64_t lpush(std::string_view key, std::span<const std::string> values);
    std::int64_t rpush(std::string_view key, std::span<const std::string> values);
    std::expected<std::string, StoreError> lpop(std::string_view key);
    std::expected<std::string, StoreError> rpop(std::string_view key);
    std::expected<std::vector<std::string>, StoreError> lrange(std::string_view key,
                                                               std::int64_t start,
                                                               std::int64_t stop);
    std::expected<std::int64_t, StoreError> llen(std::string_view key);
    std::expected<std::string, StoreError> lindex(std::string_view key, std::int64_t index);

    std::int64_t hset(std::string_view key,
                      std::span<const std::pair<std::string, std::string>> fv_pairs);
    std::expected<std::string, StoreError> hget(std::string_view key, std::string_view field);
    std::int64_t hdel(std::string_view key, std::span<const std::string> fields);
    std::expected<std::vector<std::string>, StoreError> hgetall(std::string_view key);
    std::expected<bool, StoreError> hexists(std::string_view key, std::string_view field);
    std::expected<std::int64_t, StoreError> hlen(std::string_view key);
    std::expected<std::vector<std::string>, StoreError> hkeys(std::string_view key);
    std::expected<std::vector<std::string>, StoreError> hvals(std::string_view key);

    std::int64_t sadd(std::string_view key, std::span<const std::string> members);
    std::int64_t srem(std::string_view key, std::span<const std::string> members);
    std::expected<std::vector<std::string>, StoreError> smembers(std::string_view key);
    std::expected<bool, StoreError> sismember(std::string_view key, std::string_view member);
    std::expected<std::int64_t, StoreError> scard(std::string_view key);
    std::vector<std::string> sunion(std::span<const std::string> keys);
    std::vector<std::string> sinter(std::span<const std::string> keys);

    std::size_t dbsize() const;
    void flushdb();
    std::vector<std::string> keys(std::string_view pattern);
    std::string type(std::string_view key);
    bool rename(std::string_view src, std::string_view dst);

    void sweep_expired();

    const std::flat_map<std::string, Entry>& raw_data() const noexcept;
    std::shared_lock<std::shared_mutex> lock_shared() const;

private:
    mutable std::shared_mutex mutex_;
    std::flat_map<std::string, Entry> data_;
};

} // namespace redix
