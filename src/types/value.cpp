#include "types/value.h"
#include <fmt/format.h>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace sqltiny::types {

bool Value::try_to_int(int64_t& out) const {
    switch (kind) {
        case Kind::Int64:   out = v_int; return true;
        case Kind::Double:  out = static_cast<int64_t>(v_double); return true;
        case Kind::Bool:    out = v_bool ? 1 : 0; return true;
        case Kind::String: {
            try { out = std::stoll(v_str); return true; }
            catch (...) { return false; }
        }
        default: return false;
    }
}

bool Value::try_to_double(double& out) const {
    switch (kind) {
        case Kind::Int64:   out = static_cast<double>(v_int); return true;
        case Kind::Double:  out = v_double; return true;
        case Kind::Bool:    out = v_bool ? 1.0 : 0.0; return true;
        case Kind::String: {
            try { out = std::stod(v_str); return true; }
            catch (...) { return false; }
        }
        default: return false;
    }
}

std::string Value::to_string() const {
    switch (kind) {
        case Kind::Null:     return "NULL";
        case Kind::Bool:     return v_bool ? "TRUE" : "FALSE";
        case Kind::Int64:    return fmt::to_string(v_int);
        case Kind::Double:   return fmt::to_string(v_double);
        case Kind::String:   return fmt::format("'{}'", v_str);
        case Kind::Blob:     return fmt::format("<blob:{} bytes>", v_blob.size());
        case Kind::Date:
            return fmt::format("{:04d}-{:02d}-{:02d}", v_date.year, v_date.month, v_date.day);
        case Kind::Time:
            return fmt::format("{:02d}:{:02d}:{:02d}{}",
                v_time.hour, v_time.minute, v_time.second,
                v_time.millis ? fmt::format(".{:03d}", v_time.millis) : "");
        case Kind::DateTime:
            return fmt::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}{}",
                v_dt.year, v_dt.month, v_dt.day,
                v_dt.hour, v_dt.minute, v_dt.second,
                v_dt.millis ? fmt::format(".{:03d}", v_dt.millis) : "");
    }
    return "<unknown>";
}

int Value::compare(const Value& other) const {
    // NULL 相等 / 与任何值比较都"未知",这里按 SQL = 语义处理,调用方决定
    if (is_null() || other.is_null()) return 0;

    // 数值类统一提升到 double 比较
    if (is_numeric() && other.is_numeric()) {
        double a, b;
        try_to_double(a); other.try_to_double(b);
        if (a < b) return -1;
        if (a > b) return  1;
        return 0;
    }

    // 同类直接比较
    if (kind != other.kind) {
        // 不同类型:按 kind 编号兜底(只保证弱序一致)
        if (kind < other.kind) return -1;
        if (kind > other.kind) return  1;
        return 0;
    }
    switch (kind) {
        case Kind::Bool:    return v_bool == other.v_bool ? 0 : (v_bool ? 1 : -1);
        case Kind::Int64:   return (v_int < other.v_int) ? -1 : (v_int > other.v_int ? 1 : 0);
        case Kind::Double: {
            if (v_double < other.v_double) return -1;
            if (v_double > other.v_double) return  1;
            return 0;
        }
        case Kind::String:  return v_str.compare(other.v_str);
        case Kind::Blob:    return v_blob < other.v_blob ? -1 : (v_blob > other.v_blob ? 1 : 0);
        case Kind::Date:    return v_date < other.v_date ? -1 : (v_date == other.v_date ? 0 : 1);
        case Kind::Time:    return v_time < other.v_time ? -1 : (v_time == other.v_time ? 0 : 1);
        case Kind::DateTime:return v_dt < other.v_dt ? -1 : (v_dt == other.v_dt ? 0 : 1);
        case Kind::Null:    return 0;
    }
    return 0;
}

bool Value::sql_equals(const Value& other) const {
    if (is_null() || other.is_null()) return false;   // NULL = NULL 也未知
    return compare(other) == 0;
}

size_t Value::hash() const {
    size_t h = std::hash<uint8_t>{}(static_cast<uint8_t>(kind));
    switch (kind) {
        case Kind::Bool:     h ^= std::hash<bool>{}(v_bool); break;
        case Kind::Int64:    h ^= std::hash<int64_t>{}(v_int); break;
        case Kind::Double:   h ^= std::hash<double>{}(v_double); break;
        case Kind::String:   h ^= std::hash<std::string>{}(v_str); break;
        case Kind::Date:
            h ^= std::hash<int32_t>{}(v_date.year) << 1;
            h ^= std::hash<uint8_t>{}(v_date.month) << 2;
            h ^= std::hash<uint8_t>{}(v_date.day)   << 3;
            break;
        case Kind::Time:
            h ^= std::hash<uint32_t>{}(
                (v_time.hour << 24) | (v_time.minute << 16) | (v_time.second << 8) | v_time.millis);
            break;
        case Kind::DateTime: {
            uint64_t packed = (uint64_t(v_dt.year) << 40) |
                              (uint64_t(v_dt.month)  << 36) |
                              (uint64_t(v_dt.day)    << 32) |
                              (uint64_t(v_dt.hour)   << 24) |
                              (uint64_t(v_dt.minute) << 16) |
                              (uint64_t(v_dt.second) << 8)  |
                               uint64_t(v_dt.millis);
            h ^= std::hash<uint64_t>{}(packed);
            break;
        }
        default: break;
    }
    return h;
}

} // namespace sqltiny::types
