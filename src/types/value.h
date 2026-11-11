#pragma once

#include "types/type.h"
#include <cstdint>
#include <string>
#include <variant>
#include <vector>
#include <functional>

namespace sqltiny::types {

/// 运行时日期:毫秒级精度,存储为 std::tm 风格 + 可选毫秒
struct DateValue {
    int32_t year = 0;
    uint8_t month = 1;   // 1-12
    uint8_t day = 1;
    bool operator==(const DateValue& o) const { return year==o.year && month==o.month && day==o.day; }
    bool operator<(const DateValue& o) const {
        if (year != o.year) return year < o.year;
        if (month != o.month) return month < o.month;
        return day < o.day;
    }
};

struct TimeValue {
    uint8_t hour = 0, minute = 0, second = 0;
    uint16_t millis = 0;
    bool operator==(const TimeValue& o) const {
        return hour==o.hour && minute==o.minute && second==o.second && millis==o.millis;
    }
    bool operator<(const TimeValue& o) const {
        if (hour != o.hour) return hour < o.hour;
        if (minute != o.minute) return minute < o.minute;
        if (second != o.second) return second < o.second;
        return millis < o.millis;
    }
};

struct DateTimeValue {
    int32_t year = 0;
    uint8_t month = 1, day = 1;
    uint8_t hour = 0, minute = 0, second = 0;
    uint16_t millis = 0;
    bool operator==(const DateTimeValue& o) const {
        return year==o.year && month==o.month && day==o.day &&
               hour==o.hour && minute==o.minute && second==o.second && millis==o.millis;
    }
    bool operator<(const DateTimeValue& o) const {
        if (year != o.year) return year < o.year;
        if (month != o.month) return month < o.month;
        if (day != o.day) return day < o.day;
        if (hour != o.hour) return hour < o.hour;
        if (minute != o.minute) return minute < o.minute;
        if (second != o.second) return second < o.second;
        return millis < o.millis;
    }
};

using BlobValue = std::vector<uint8_t>;

/// 运行时值 —— 用 std::variant 表达所有可能的列值类型
/// Null 用 std::monostate + 外部 is_null() 标志一起表达
/// (这样 Value 本身可比较,不用 std::optional 套一层)
struct Value {
    enum class Kind : uint8_t {
        Null,
        Bool,
        Int64,
        Double,
        String,
        Blob,
        Date,
        Time,
        DateTime,
    };

    Kind kind = Kind::Null;

    // 直接成员,比 variant 访问更快
    bool            v_bool   = false;
    int64_t         v_int    = 0;
    double          v_double = 0.0;
    std::string     v_str;
    BlobValue       v_blob;
    DateValue       v_date;
    TimeValue       v_time;
    DateTimeValue   v_dt;

    // ---------- 构造 ----------
    Value() = default;
    static Value make_null()           { Value v; v.kind = Kind::Null;   return v; }
    static Value make_bool(bool b)      { Value v; v.kind = Kind::Bool;   v.v_bool = b; return v; }
    static Value make_int(int64_t i)    { Value v; v.kind = Kind::Int64;  v.v_int = i; return v; }
    static Value make_double(double d)  { Value v; v.kind = Kind::Double; v.v_double = d; return v; }
    static Value make_string(std::string s) {
        Value v; v.kind = Kind::String; v.v_str = std::move(s); return v;
    }
    static Value make_blob(BlobValue b) {
        Value v; v.kind = Kind::Blob; v.v_blob = std::move(b); return v;
    }
    static Value make_date(DateValue d) { Value v; v.kind = Kind::Date;     v.v_date = d; return v; }
    static Value make_time(TimeValue t) { Value v; v.kind = Kind::Time;     v.v_time = t; return v; }
    static Value make_datetime(DateTimeValue dt) {
        Value v; v.kind = Kind::DateTime; v.v_dt = dt; return v;
    }

    bool is_null()     const { return kind == Kind::Null; }
    bool is_int()      const { return kind == Kind::Int64; }
    bool is_double()    const { return kind == Kind::Double; }
    bool is_numeric()  const { return kind == Kind::Int64 || kind == Kind::Double; }
    bool is_string()   const { return kind == Kind::String; }

    // ---------- 类型转换 ----------
    /// 尝试转为 int64;失败返回 false + out 未定义
    bool try_to_int(int64_t& out) const;
    /// 尝试转为 double;失败返回 false
    bool try_to_double(double& out) const;
    /// 统一数值比较辅助:把两种数值都提升到 double
    bool numeric_equals(double a, double b) const;  // 未使用,占位

    /// 转为字符串表示(用于 SHOW / 日志 / MySQL 文本协议)
    std::string to_string() const;

    // ---------- 比较 ----------
    /// 三值比较: <0 / 0 / >0;任一为 Null 返回 0(由调用方用 IS DISTINCT 处理)
    int compare(const Value& other) const;

    /// SQL = 语义,处理 NULL 特殊情况
    bool sql_equals(const Value& other) const;

    // ---------- 哈希(用于 AVL / 哈希表,可以只是 compare 的退化) ----------
    size_t hash() const;
};

// 便于 unordered
struct ValueHash {
    size_t operator()(const Value& v) const { return v.hash(); }
};

} // namespace sqltiny::types
