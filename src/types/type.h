#pragma once

#include <cstdint>

namespace sqltiny::types {

/// SQL 类型枚举,覆盖 MySQL 常用 + HSQLDB 核心
enum class SqlType : uint8_t {
    Null,
    Boolean,
    TinyInt,    // 1 字节
    SmallInt,   // 2 字节
    Int,        // 4 字节
    BigInt,     // 8 字节
    Decimal,    // 定点数,带 precision/scale
    Float,      // 4 字节浮点
    Double,     // 8 字节浮点
    Char,       // 定长字符
    Varchar,    // 变长字符
    Text,       // 大文本
    Binary,     // 定长字节
    Varbinary,  // 变长字节
    Blob,       // 大二进制
    Date,       // YYYY-MM-DD
    Time,       // HH:MM:SS[.sss]
    DateTime,   // YYYY-MM-DD HH:MM:SS[.sss]
    Timestamp,  // 同 DateTime 但带时区语义(第一版存 UTC)
    Interval,   // HSQLDB 风格间隔(可选,M3 前不重点支持)
    Json,
    Array,      // SQL ARRAY(可选)
};

/// precision/scale 附加信息,DECIMAL / VARCHAR / CHAR / TIME/DateTime 精度用
struct TypeInfo {
    SqlType  type    = SqlType::Null;
    uint16_t length  = 0;   // VARCHAR(64) 的 64;DECIMAL(10,2) 的 10
    uint16_t scale   = 0;   // DECIMAL(10,2) 的 2
    bool     nullable = true;
    bool     unsigned_ = false;  // MySQL TINYINT UNSIGNED 等
};

inline bool is_integer(SqlType t) {
    return t == SqlType::TinyInt || t == SqlType::SmallInt ||
           t == SqlType::Int    || t == SqlType::BigInt;
}
inline bool is_numeric(SqlType t) {
    return is_integer(t) || t == SqlType::Decimal ||
           t == SqlType::Float || t == SqlType::Double;
}
inline bool is_string(SqlType t) {
    return t == SqlType::Char || t == SqlType::Varchar ||
           t == SqlType::Text || t == SqlType::Json;
}
inline bool is_binary(SqlType t) {
    return t == SqlType::Binary || t == SqlType::Varbinary || t == SqlType::Blob;
}
inline bool is_datetime(SqlType t) {
    return t == SqlType::Date || t == SqlType::Time ||
           t == SqlType::DateTime || t == SqlType::Timestamp;
}

/// 转换为 MySQL 客户端协议 Field Type(里程碑 2.5 用)
/// 参考: https://dev.mysql.com/doc/dev/mysql-server/latest/group__group__cs__column__type.html
enum class MysqlColumnType : uint8_t {
    Decimal    = 0,
    Tiny       = 1,
    Short      = 2,
    Long       = 3,
    Float      = 4,
    Double     = 5,
    Null       = 6,
    Timestamp  = 7,
    Longlong   = 8,
    Int24      = 9,
    Date       = 10,
    Time       = 11,
    Datetime   = 12,
    Year       = 13,
    VarChar    = 15,
    Bit        = 16,
    Json       = 245,
    NewDecimal = 246,
    Enum       = 247,
    Set        = 248,
    Blob       = 252,
    VarString  = 253,
    String     = 254,
    Geometry   = 255,
};

inline MysqlColumnType to_mysql_col_type(SqlType t) {
    switch (t) {
        case SqlType::Null:       return MysqlColumnType::Null;
        case SqlType::Boolean:    return MysqlColumnType::Tiny;
        case SqlType::TinyInt:    return MysqlColumnType::Tiny;
        case SqlType::SmallInt:   return MysqlColumnType::Short;
        case SqlType::Int:        return MysqlColumnType::Long;
        case SqlType::BigInt:     return MysqlColumnType::Longlong;
        case SqlType::Decimal:    return MysqlColumnType::NewDecimal;
        case SqlType::Float:      return MysqlColumnType::Float;
        case SqlType::Double:     return MysqlColumnType::Double;
        case SqlType::Char:       return MysqlColumnType::String;
        case SqlType::Varchar:    return MysqlColumnType::VarChar;
        case SqlType::Text:       return MysqlColumnType::Blob;
        case SqlType::Binary:     return MysqlColumnType::String;
        case SqlType::Varbinary:  return MysqlColumnType::VarString;
        case SqlType::Blob:       return MysqlColumnType::Blob;
        case SqlType::Date:       return MysqlColumnType::Date;
        case SqlType::Time:       return MysqlColumnType::Time;
        case SqlType::DateTime:   return MysqlColumnType::Datetime;
        case SqlType::Timestamp:  return MysqlColumnType::Timestamp;
        case SqlType::Json:       return MysqlColumnType::Json;
        default:                  return MysqlColumnType::Blob;
    }
}

} // namespace sqltiny::types
