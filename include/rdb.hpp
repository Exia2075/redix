#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace redix {

class Store;

class Rdb {
public:
    static std::expected<void, std::string> save(const Store& store, std::string_view path);
    static std::expected<void, std::string> load(Store& store, std::string_view path);
};

} // namespace redix
