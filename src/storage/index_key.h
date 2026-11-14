#pragma once

#include "types/value.h"
#include <vector>
#include <cstdint>

namespace sqltiny::storage {

/// 键:单列值 或 多列组合(多列时 compare 按 tuple 语义逐列比较)
/// v2.0.0 起为 IndexAvl / IndexBPlusTree 共用的键类型
class Key {
public:
    Key() = default;
    explicit Key(sqltiny::types::Value v) : parts_{ std::move(v) } {}
    explicit Key(std::vector<sqltiny::types::Value> v) : parts_(std::move(v)) {}

    std::string to_string() const;
    const std::vector<sqltiny::types::Value>& parts() const { return parts_; }
    size_t size() const { return parts_.size(); }

    // 字典序比较
    int compare(const Key& o) const;
    bool operator==(const Key& o) const { return compare(o) == 0; }
    bool operator< (const Key& o) const { return compare(o) <  0; }

private:
    std::vector<sqltiny::types::Value> parts_;
};

} // namespace sqltiny::storage
