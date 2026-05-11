#include "store.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace redix {

namespace {

using DataMap = std::flat_map<std::string, Entry>;

std::uint64_t now_ms() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

StoreError make_error(StoreErrorKind kind, std::string message) {
    return StoreError{kind, std::move(message)};
}

StoreError key_not_found() {
    return make_error(StoreErrorKind::KeyNotFound, "ERR no such key");
}

StoreError wrong_type() {
    return make_error(StoreErrorKind::WrongType,
                      "WRONGTYPE Operation against a key holding the wrong kind of value");
}

StoreError not_integer() {
    return make_error(StoreErrorKind::NotAnInteger,
                      "ERR value is not an integer or out of range");
}

StoreError out_of_range() {
    return make_error(StoreErrorKind::OutOfRange, "ERR index out of range");
}

DataMap::iterator find_live_mut(DataMap& data, std::string_view key) {
    auto it = data.find(std::string{key});
    if (it != data.end() && it->second.is_expired()) {
        data.erase(it);
        return data.end();
    }
    return it;
}

DataMap::const_iterator find_live(const DataMap& data, std::string_view key) {
    auto it = data.find(std::string{key});
    if (it != data.end() && it->second.is_expired()) {
        return data.end();
    }
    return it;
}

bool parse_i64(std::string_view text, std::int64_t& out) {
    if (text.empty()) {
        return false;
    }

    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

std::expected<std::int64_t, StoreError> checked_add(std::int64_t lhs, std::int64_t rhs) {
    const __int128 result = static_cast<__int128>(lhs) + static_cast<__int128>(rhs);
    if (result > std::numeric_limits<std::int64_t>::max() ||
        result < std::numeric_limits<std::int64_t>::min()) {
        return std::unexpected(not_integer());
    }
    return static_cast<std::int64_t>(result);
}

std::expected<std::int64_t, StoreError> checked_subtract(std::int64_t lhs, std::int64_t rhs) {
    const __int128 result = static_cast<__int128>(lhs) - static_cast<__int128>(rhs);
    if (result > std::numeric_limits<std::int64_t>::max() ||
        result < std::numeric_limits<std::int64_t>::min()) {
        return std::unexpected(not_integer());
    }
    return static_cast<std::int64_t>(result);
}

std::int64_t normalize_index(std::int64_t index, std::int64_t size) {
    return index < 0 ? size + index : index;
}

bool glob_match(std::string_view pattern, std::string_view text) {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t match = 0;

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++match;
        } else {
            return false;
        }
    }

    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }

    return p == pattern.size();
}

template <typename T>
std::string redis_type_name() {
    if constexpr (std::is_same_v<T, StringValue>) {
        return "string";
    } else if constexpr (std::is_same_v<T, ListValue>) {
        return "list";
    } else if constexpr (std::is_same_v<T, HashValue>) {
        return "hash";
    } else {
        return "set";
    }
}

} // namespace

bool Entry::is_expired() const noexcept {
    return expires_at_ms != 0 && expires_at_ms <= now_ms();
}

void Store::set(std::string key, std::string value, std::uint64_t expires_at_ms) {
    std::unique_lock lock{mutex_};
    data_[std::move(key)] = Entry{StringValue{std::move(value)}, expires_at_ms};
}

std::expected<std::string, StoreError> Store::get(std::string_view key) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    const auto* value = std::get_if<StringValue>(&it->second.value);
    if (value == nullptr) {
        return std::unexpected(wrong_type());
    }
    return value->data;
}

std::int64_t Store::del(std::span<const std::string> keys) {
    std::unique_lock lock{mutex_};
    std::int64_t removed = 0;
    for (const std::string& key : keys) {
        auto it = find_live_mut(data_, key);
        if (it != data_.end()) {
            data_.erase(it);
            ++removed;
        }
    }
    return removed;
}

std::int64_t Store::exists(std::span<const std::string> keys) {
    std::shared_lock lock{mutex_};
    std::int64_t count = 0;
    for (const std::string& key : keys) {
        if (find_live(data_, key) != data_.end()) {
            ++count;
        }
    }
    return count;
}

std::expected<std::int64_t, StoreError> Store::incr(std::string_view key) {
    return incrby(key, 1);
}

std::expected<std::int64_t, StoreError> Store::incrby(std::string_view key, std::int64_t delta) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, key);
    if (it == data_.end()) {
        data_[std::string{key}] = Entry{StringValue{std::to_string(delta)}, 0};
        return delta;
    }

    auto* value = std::get_if<StringValue>(&it->second.value);
    if (value == nullptr) {
        return std::unexpected(wrong_type());
    }

    std::int64_t current = 0;
    if (!parse_i64(value->data, current)) {
        return std::unexpected(not_integer());
    }

    auto next = checked_add(current, delta);
    if (!next) {
        return std::unexpected(next.error());
    }
    value->data = std::to_string(*next);
    return *next;
}

std::expected<std::int64_t, StoreError> Store::decr(std::string_view key) {
    return decrby(key, 1);
}

std::expected<std::int64_t, StoreError> Store::decrby(std::string_view key, std::int64_t delta) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, key);
    if (it == data_.end()) {
        auto initial = checked_subtract(0, delta);
        if (!initial) {
            return std::unexpected(initial.error());
        }
        data_[std::string{key}] = Entry{StringValue{std::to_string(*initial)}, 0};
        return *initial;
    }

    auto* value = std::get_if<StringValue>(&it->second.value);
    if (value == nullptr) {
        return std::unexpected(wrong_type());
    }

    std::int64_t current = 0;
    if (!parse_i64(value->data, current)) {
        return std::unexpected(not_integer());
    }

    auto next = checked_subtract(current, delta);
    if (!next) {
        return std::unexpected(next.error());
    }
    value->data = std::to_string(*next);
    return *next;
}

std::expected<std::int64_t, StoreError> Store::append(std::string_view key,
                                                       std::string_view suffix) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, key);
    if (it == data_.end()) {
        std::string value{suffix};
        const auto len = static_cast<std::int64_t>(value.size());
        data_[std::string{key}] = Entry{StringValue{std::move(value)}, 0};
        return len;
    }

    auto* value = std::get_if<StringValue>(&it->second.value);
    if (value == nullptr) {
        return std::unexpected(wrong_type());
    }

    value->data.append(suffix);
    return static_cast<std::int64_t>(value->data.size());
}

std::expected<std::int64_t, StoreError> Store::strlen(std::string_view key) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return 0;
    }

    const auto* value = std::get_if<StringValue>(&it->second.value);
    if (value == nullptr) {
        return std::unexpected(wrong_type());
    }
    return static_cast<std::int64_t>(value->data.size());
}

std::expected<std::string, StoreError> Store::getset(std::string key, std::string value) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, key);
    if (it == data_.end()) {
        data_[std::move(key)] = Entry{StringValue{std::move(value)}, 0};
        return std::unexpected(key_not_found());
    }

    auto* old = std::get_if<StringValue>(&it->second.value);
    if (old == nullptr) {
        return std::unexpected(wrong_type());
    }

    std::string previous = std::move(old->data);
    it->second = Entry{StringValue{std::move(value)}, 0};
    return previous;
}

void Store::mset(std::span<const std::pair<std::string, std::string>> pairs) {
    std::unique_lock lock{mutex_};
    for (const auto& [key, value] : pairs) {
        data_[key] = Entry{StringValue{value}, 0};
    }
}

std::vector<std::optional<std::string>> Store::mget(std::span<const std::string> keys) {
    std::shared_lock lock{mutex_};
    std::vector<std::optional<std::string>> values;
    values.reserve(keys.size());
    for (const std::string& key : keys) {
        auto it = find_live(data_, key);
        if (it == data_.end()) {
            values.push_back(std::nullopt);
            continue;
        }

        const auto* value = std::get_if<StringValue>(&it->second.value);
        values.push_back(value == nullptr ? std::nullopt : std::optional<std::string>{value->data});
    }
    return values;
}

bool Store::expire(std::string_view key, std::uint64_t seconds) {
    return pexpire(key, seconds * 1000);
}

bool Store::pexpire(std::string_view key, std::uint64_t ms) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, key);
    if (it == data_.end()) {
        return false;
    }
    it->second.expires_at_ms = now_ms() + ms;
    return true;
}

bool Store::persist(std::string_view key) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, key);
    if (it == data_.end() || it->second.expires_at_ms == 0) {
        return false;
    }
    it->second.expires_at_ms = 0;
    return true;
}

std::int64_t Store::ttl(std::string_view key) {
    const std::int64_t remaining = pttl(key);
    if (remaining <= 0) {
        return remaining;
    }
    return (remaining + 999) / 1000;
}

std::int64_t Store::pttl(std::string_view key) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return -2;
    }
    if (it->second.expires_at_ms == 0) {
        return -1;
    }

    const auto now = now_ms();
    if (it->second.expires_at_ms <= now) {
        return -2;
    }

    return static_cast<std::int64_t>(it->second.expires_at_ms - now);
}

std::int64_t Store::lpush(std::string_view key, std::span<const std::string> values) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, key);
    if (it == data_.end()) {
        ListValue list;
        for (const std::string& value : values) {
            list.data.push_front(value);
        }
        const auto size = static_cast<std::int64_t>(list.data.size());
        data_[std::string{key}] = Entry{std::move(list), 0};
        return size;
    }

    auto* list = std::get_if<ListValue>(&it->second.value);
    if (list == nullptr) {
        return -1;
    }
    for (const std::string& value : values) {
        list->data.push_front(value);
    }
    return static_cast<std::int64_t>(list->data.size());
}

std::int64_t Store::rpush(std::string_view key, std::span<const std::string> values) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, key);
    if (it == data_.end()) {
        ListValue list;
        for (const std::string& value : values) {
            list.data.push_back(value);
        }
        const auto size = static_cast<std::int64_t>(list.data.size());
        data_[std::string{key}] = Entry{std::move(list), 0};
        return size;
    }

    auto* list = std::get_if<ListValue>(&it->second.value);
    if (list == nullptr) {
        return -1;
    }
    for (const std::string& value : values) {
        list->data.push_back(value);
    }
    return static_cast<std::int64_t>(list->data.size());
}

std::expected<std::string, StoreError> Store::lpop(std::string_view key) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    auto* list = std::get_if<ListValue>(&it->second.value);
    if (list == nullptr) {
        return std::unexpected(wrong_type());
    }
    if (list->data.empty()) {
        data_.erase(it);
        return std::unexpected(key_not_found());
    }

    std::string value = std::move(list->data.front());
    list->data.pop_front();
    if (list->data.empty()) {
        data_.erase(it);
    }
    return value;
}

std::expected<std::string, StoreError> Store::rpop(std::string_view key) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    auto* list = std::get_if<ListValue>(&it->second.value);
    if (list == nullptr) {
        return std::unexpected(wrong_type());
    }
    if (list->data.empty()) {
        data_.erase(it);
        return std::unexpected(key_not_found());
    }

    std::string value = std::move(list->data.back());
    list->data.pop_back();
    if (list->data.empty()) {
        data_.erase(it);
    }
    return value;
}

std::expected<std::vector<std::string>, StoreError> Store::lrange(std::string_view key,
                                                                  std::int64_t start,
                                                                  std::int64_t stop) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    const auto* list = std::get_if<ListValue>(&it->second.value);
    if (list == nullptr) {
        return std::unexpected(wrong_type());
    }

    const auto size = static_cast<std::int64_t>(list->data.size());
    std::int64_t first = normalize_index(start, size);
    std::int64_t last = normalize_index(stop, size);

    if (first < 0) {
        first = 0;
    }
    if (last >= size) {
        last = size - 1;
    }

    std::vector<std::string> out;
    if (size == 0 || first > last || first >= size || last < 0) {
        return out;
    }

    out.reserve(static_cast<std::size_t>(last - first + 1));
    for (std::int64_t i = first; i <= last; ++i) {
        out.push_back(list->data[static_cast<std::size_t>(i)]);
    }
    return out;
}

std::expected<std::int64_t, StoreError> Store::llen(std::string_view key) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    const auto* list = std::get_if<ListValue>(&it->second.value);
    if (list == nullptr) {
        return std::unexpected(wrong_type());
    }
    return static_cast<std::int64_t>(list->data.size());
}

std::expected<std::string, StoreError> Store::lindex(std::string_view key, std::int64_t index) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    const auto* list = std::get_if<ListValue>(&it->second.value);
    if (list == nullptr) {
        return std::unexpected(wrong_type());
    }

    const auto size = static_cast<std::int64_t>(list->data.size());
    const std::int64_t normalized = normalize_index(index, size);
    if (normalized < 0 || normalized >= size) {
        return std::unexpected(out_of_range());
    }
    return list->data[static_cast<std::size_t>(normalized)];
}

std::int64_t Store::hset(std::string_view key,
                         std::span<const std::pair<std::string, std::string>> fv_pairs) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, key);
    if (it == data_.end()) {
        HashValue hash;
        std::int64_t added = 0;
        for (const auto& [field, value] : fv_pairs) {
            auto [_, inserted] = hash.data.insert_or_assign(field, value);
            if (inserted) {
                ++added;
            }
        }
        data_[std::string{key}] = Entry{std::move(hash), 0};
        return added;
    }

    auto* hash = std::get_if<HashValue>(&it->second.value);
    if (hash == nullptr) {
        return -1;
    }

    std::int64_t added = 0;
    for (const auto& [field, value] : fv_pairs) {
        auto [_, inserted] = hash->data.insert_or_assign(field, value);
        if (inserted) {
            ++added;
        }
    }
    return added;
}

std::expected<std::string, StoreError> Store::hget(std::string_view key, std::string_view field) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    const auto* hash = std::get_if<HashValue>(&it->second.value);
    if (hash == nullptr) {
        return std::unexpected(wrong_type());
    }

    auto field_it = hash->data.find(std::string{field});
    if (field_it == hash->data.end()) {
        return std::unexpected(key_not_found());
    }
    return field_it->second;
}

std::int64_t Store::hdel(std::string_view key, std::span<const std::string> fields) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, key);
    if (it == data_.end()) {
        return 0;
    }

    auto* hash = std::get_if<HashValue>(&it->second.value);
    if (hash == nullptr) {
        return -1;
    }

    std::int64_t removed = 0;
    for (const std::string& field : fields) {
        removed += static_cast<std::int64_t>(hash->data.erase(field));
    }
    if (hash->data.empty()) {
        data_.erase(it);
    }
    return removed;
}

std::expected<std::vector<std::string>, StoreError> Store::hgetall(std::string_view key) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    const auto* hash = std::get_if<HashValue>(&it->second.value);
    if (hash == nullptr) {
        return std::unexpected(wrong_type());
    }

    std::vector<std::string> out;
    out.reserve(hash->data.size() * 2);
    for (const auto& [field, value] : hash->data) {
        out.push_back(field);
        out.push_back(value);
    }
    return out;
}

std::expected<bool, StoreError> Store::hexists(std::string_view key, std::string_view field) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    const auto* hash = std::get_if<HashValue>(&it->second.value);
    if (hash == nullptr) {
        return std::unexpected(wrong_type());
    }
    return hash->data.contains(std::string{field});
}

std::expected<std::int64_t, StoreError> Store::hlen(std::string_view key) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    const auto* hash = std::get_if<HashValue>(&it->second.value);
    if (hash == nullptr) {
        return std::unexpected(wrong_type());
    }
    return static_cast<std::int64_t>(hash->data.size());
}

std::expected<std::vector<std::string>, StoreError> Store::hkeys(std::string_view key) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    const auto* hash = std::get_if<HashValue>(&it->second.value);
    if (hash == nullptr) {
        return std::unexpected(wrong_type());
    }

    std::vector<std::string> out;
    out.reserve(hash->data.size());
    for (const auto& [field, _] : hash->data) {
        out.push_back(field);
    }
    return out;
}

std::expected<std::vector<std::string>, StoreError> Store::hvals(std::string_view key) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    const auto* hash = std::get_if<HashValue>(&it->second.value);
    if (hash == nullptr) {
        return std::unexpected(wrong_type());
    }

    std::vector<std::string> out;
    out.reserve(hash->data.size());
    for (const auto& [_, value] : hash->data) {
        out.push_back(value);
    }
    return out;
}

std::int64_t Store::sadd(std::string_view key, std::span<const std::string> members) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, key);
    if (it == data_.end()) {
        SetValue set;
        std::int64_t added = 0;
        for (const std::string& member : members) {
            if (set.data.insert(member).second) {
                ++added;
            }
        }
        data_[std::string{key}] = Entry{std::move(set), 0};
        return added;
    }

    auto* set = std::get_if<SetValue>(&it->second.value);
    if (set == nullptr) {
        return -1;
    }

    std::int64_t added = 0;
    for (const std::string& member : members) {
        if (set->data.insert(member).second) {
            ++added;
        }
    }
    return added;
}

std::int64_t Store::srem(std::string_view key, std::span<const std::string> members) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, key);
    if (it == data_.end()) {
        return 0;
    }

    auto* set = std::get_if<SetValue>(&it->second.value);
    if (set == nullptr) {
        return -1;
    }

    std::int64_t removed = 0;
    for (const std::string& member : members) {
        removed += static_cast<std::int64_t>(set->data.erase(member));
    }
    if (set->data.empty()) {
        data_.erase(it);
    }
    return removed;
}

std::expected<std::vector<std::string>, StoreError> Store::smembers(std::string_view key) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    const auto* set = std::get_if<SetValue>(&it->second.value);
    if (set == nullptr) {
        return std::unexpected(wrong_type());
    }
    return std::vector<std::string>{set->data.begin(), set->data.end()};
}

std::expected<bool, StoreError> Store::sismember(std::string_view key, std::string_view member) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    const auto* set = std::get_if<SetValue>(&it->second.value);
    if (set == nullptr) {
        return std::unexpected(wrong_type());
    }
    return set->data.contains(std::string{member});
}

std::expected<std::int64_t, StoreError> Store::scard(std::string_view key) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return std::unexpected(key_not_found());
    }

    const auto* set = std::get_if<SetValue>(&it->second.value);
    if (set == nullptr) {
        return std::unexpected(wrong_type());
    }
    return static_cast<std::int64_t>(set->data.size());
}

std::vector<std::string> Store::sunion(std::span<const std::string> keys) {
    std::shared_lock lock{mutex_};
    std::flat_set<std::string> result;
    for (const std::string& key : keys) {
        auto it = find_live(data_, key);
        if (it == data_.end()) {
            continue;
        }

        const auto* set = std::get_if<SetValue>(&it->second.value);
        if (set == nullptr) {
            continue;
        }
        result.insert(set->data.begin(), set->data.end());
    }
    return std::vector<std::string>{result.begin(), result.end()};
}

std::vector<std::string> Store::sinter(std::span<const std::string> keys) {
    std::shared_lock lock{mutex_};
    if (keys.empty()) {
        return {};
    }

    std::flat_set<std::string> result;
    bool initialized = false;

    for (const std::string& key : keys) {
        auto it = find_live(data_, key);
        if (it == data_.end()) {
            return {};
        }

        const auto* set = std::get_if<SetValue>(&it->second.value);
        if (set == nullptr) {
            return {};
        }

        if (!initialized) {
            result = set->data;
            initialized = true;
            continue;
        }

        std::flat_set<std::string> next;
        for (const std::string& member : result) {
            if (set->data.contains(member)) {
                next.insert(member);
            }
        }
        result = std::move(next);
        if (result.empty()) {
            return {};
        }
    }

    return std::vector<std::string>{result.begin(), result.end()};
}

std::size_t Store::dbsize() const {
    std::shared_lock lock{mutex_};
    std::size_t count = 0;
    for (const auto& [_, entry] : data_) {
        if (!is_expired(entry)) {
            ++count;
        }
    }
    return count;
}

void Store::flushdb() {
    std::unique_lock lock{mutex_};
    data_.clear();
}

std::vector<std::string> Store::keys(std::string_view pattern) {
    std::shared_lock lock{mutex_};
    std::vector<std::string> out;
    const DataMap& data = data_;

    for (auto it = data.begin(); it != data.end(); ++it) {
        if (is_expired(it->second)) {
            continue;
        }
        if (glob_match(pattern, it->first)) {
            out.push_back(it->first);
        }
    }
    return out;
}

std::string Store::type(std::string_view key) {
    std::shared_lock lock{mutex_};
    auto it = find_live(data_, key);
    if (it == data_.end()) {
        return "none";
    }
    return std::visit([](const auto& value) { return redis_type_name<std::decay_t<decltype(value)>>(); },
                      it->second.value);
}

bool Store::rename(std::string_view src, std::string_view dst) {
    std::unique_lock lock{mutex_};
    auto it = find_live_mut(data_, src);
    if (it == data_.end()) {
        return false;
    }

    Entry entry = std::move(it->second);
    data_.erase(it);
    data_[std::string{dst}] = std::move(entry);
    return true;
}

void Store::sweep_expired() {
    std::unique_lock lock{mutex_};
    std::size_t visited = 0;
    for (auto it = data_.begin(); it != data_.end() && visited < 20; ++visited) {
        if (is_expired(it->second)) {
            it = data_.erase(it);
        } else {
            ++it;
        }
    }
}

const std::flat_map<std::string, Entry>& Store::raw_data() const noexcept {
    return data_;
}

std::shared_lock<std::shared_mutex> Store::lock_shared() const {
    return std::shared_lock{mutex_};
}

bool Store::is_expired(const Entry& entry) noexcept {
    return entry.is_expired();
}

} // namespace redix
