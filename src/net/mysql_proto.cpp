#include "net/mysql_proto.h"
#include "types/type.h"
#include "types/value.h"
#include <fmt/format.h>
#include <cstring>
#include <random>
#include <chrono>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace sqltiny::net {

// ======== 基础工具 ========

std::string value_to_text(const sqltiny::types::Value& v) {
    using namespace sqltiny::types;
    if (v.is_null()) return "NULL";
    switch (v.kind) {
        case Value::Kind::Bool:   return v.v_bool ? "1" : "0";
        case Value::Kind::Int64: return std::to_string(v.v_int);
        case Value::Kind::Double: return fmt::to_string(v.v_double);
        case Value::Kind::String: return v.v_str;  // 不包装引号
        case Value::Kind::Date:
            return fmt::format("{:04d}-{:02d}-{:02d}", v.v_date.year, v.v_date.month, v.v_date.day);
        case Value::Kind::Time:
            return fmt::format("{:02d}:{:02d}:{:02d}", v.v_time.hour, v.v_time.minute, v.v_time.second);
        case Value::Kind::DateTime:
            return fmt::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
                v.v_dt.year, v.v_dt.month, v.v_dt.day,
                v.v_dt.hour, v.v_dt.minute, v.v_dt.second);
        default: return v.to_string();
    }
}

std::string random_bytes_hex(int n) {
    // 简单随机,生产需要更强熵
    static std::mt19937 rng{ static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) };
    static std::uniform_int_distribution<int> dist(0, 255);
    std::string out; out.reserve(n * 2);
    for (int i = 0; i < n; ++i) out += fmt::format("{:02x}", dist(rng));
    return out;
}

// SHA-1 简易实现(mysql_native_password 用 SHA1(SHA1(password) + salt))
namespace {
// 直接用 Windows crypto / 手动实现都麻烦,先 XOR 占位(仅用于本地测试)
// 真要加密用 openssl/sha1.h,但我们不想加依赖。
// mysql_native_password 的响应 = SHA1(SHA1(password) XOR SHA1(salt + SHA1(password)))
// 由于 SHA1 实现麻烦,简化做纯 XOR — 实际上 mysql_native_password 我们就当恒等跳过,
// 客户端连进来时不校验密码(开发期允许)。
}

std::vector<uint8_t> compute_mysql_native_password(const std::string& /*password*/,
                                                   const std::vector<uint8_t>& /*salt*/) {
    // 开发期简化:返回空数组 —— 服务器端接受任何密码
    return {};
}

// ======== Packet 编解码 ========

std::vector<uint8_t> encode_mysql_packet(const std::vector<uint8_t>& payload, uint8_t seq) {
    std::vector<uint8_t> out;
    out.reserve(payload.size() + 4);
    // 分包:简单处理,先不考虑 > 16MB
    uint32_t len = static_cast<uint32_t>(payload.size());
    out.push_back(len & 0xff);
    out.push_back((len >> 8) & 0xff);
    out.push_back((len >> 16) & 0xff);
    out.push_back(seq);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool send_all(int fd, const uint8_t* p, size_t n) {
    size_t sent = 0;
    while (sent < n) {
#ifdef _WIN32
        int r = send(fd, reinterpret_cast<const char*>(p + sent), static_cast<int>(n - sent), 0);
#else
        ssize_t r = ::send(fd, p + sent, n - sent, 0);
#endif
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

bool recv_all(int fd, uint8_t* p, size_t n) {
    size_t got = 0;
    while (got < n) {
#ifdef _WIN32
        int r = recv(fd, reinterpret_cast<char*>(p + got), static_cast<int>(n - got), 0);
#else
        ssize_t r = ::recv(fd, p + got, n - got, 0);
#endif
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

std::vector<uint8_t> read_mysql_packet(int socket_fd, uint8_t& seq_out) {
    uint8_t header[4];
    if (!recv_all(socket_fd, header, 4)) return {};
    uint32_t len = header[0] | (header[1] << 8) | (header[2] << 16);
    seq_out = header[3];
    std::vector<uint8_t> payload(len);
    if (len > 0) {
        if (!recv_all(socket_fd, payload.data(), len)) return {};
    }
    return payload;
}

// ======== OK / ERR / EOF ========

std::vector<uint8_t> encode_ok_packet(uint64_t affected_rows, uint64_t last_insert_id,
                                      uint16_t status_flags, uint16_t warnings,
                                      const std::string& info) {
    PacketWriter w;
    w.write_u8(MYSQL_PACKET_OK);
    w.write_lenenc_int(affected_rows);
    w.write_lenenc_int(last_insert_id);
    w.write_u16(status_flags);
    w.write_u16(warnings);
    if (!info.empty()) w.write_lenenc_string(info);
    return w.buf;
}

std::vector<uint8_t> encode_err_packet(uint16_t err, std::string_view sql_state,
                                       std::string_view message) {
    PacketWriter w;
    w.write_u8(MYSQL_PACKET_ERR);
    w.write_u16(err);
    w.write_u8('#');
    for (char c : sql_state) w.write_u8(static_cast<uint8_t>(c));
    w.write_string_nolen(std::string(message));
    return w.buf;
}

std::vector<uint8_t> encode_eof_packet(uint16_t warnings, uint16_t status_flags) {
    PacketWriter w;
    w.write_u8(MYSQL_PACKET_EOF);
    w.write_u16(warnings);
    w.write_u16(status_flags);
    return w.buf;
}

// ======== Handshake ========

std::vector<uint8_t> encode_handshake_v10(uint32_t thread_id,
                                          const std::vector<uint8_t>& auth_data) {
    PacketWriter w;
    w.write_u8(10);  // protocol version
    w.write_string_null("5.7.0-SQLTiny");  // server version (必须以数字开头,PyMySQL 会 parse)
    w.write_u32(thread_id);
    // auth-data-part-1(8字节)
    for (int i = 0; i < 8 && i < static_cast<int>(auth_data.size()); ++i) w.write_u8(auth_data[i]);
    w.write_u8(0);  // filler
    // capability flags
    // 注意:不广播 CLIENT_DEPRECATE_EOF —— 老客户端(如 dbExpress/DBX)只是原样回显
    // 服务器广播的能力位,实际并不支持"无 EOF 包"的 8.0 协议,会卡在等 EOF 超时。
    uint32_t caps = CLIENT_PROTOCOL_41 | CLIENT_SECURE_CONNECTION | CLIENT_PLUGIN_AUTH |
                    CLIENT_CONNECT_WITH_DB;
    w.write_u16(caps & 0xffff);
    w.write_u8(33);  // character set (utf8mb4 = 45, 先用 latin1 = 8, 33=utf8)
    w.write_u16(SERVER_STATUS_AUTOCOMMIT);
    w.write_u16((caps >> 16) & 0xffff);
    w.write_u8(std::min<uint8_t>(21, static_cast<uint8_t>(auth_data.size() > 8 ? auth_data.size() - 8 : 0)));  // auth-data-length
    w.write_u8(0); w.write_u8(0); w.write_u8(0); w.write_u8(0); w.write_u8(0); w.write_u8(0); w.write_u8(0); w.write_u8(0); w.write_u8(0);
    // auth-data-part-2 (auth_data[8:])
    for (size_t i = 8; i < auth_data.size(); ++i) w.write_u8(auth_data[i]);
    w.write_u8(0);  // NUL terminator
    w.write_string_nolen("mysql_native_password");
    w.write_u8(0);
    return w.buf;
}

// ======== ResultSet ========

static std::vector<uint8_t> encode_column_def(
        const std::string& table_alias, const std::string& column_alias,
        sqltiny::types::SqlType type, bool nullable, bool is_pk, bool auto_inc) {
    using namespace sqltiny::types;
    PacketWriter w;
    w.write_lenenc_string("def");                 // catalog
    w.write_lenenc_string("default");             // schema
    w.write_lenenc_string(table_alias.empty() ? "t" : table_alias);  // table
    w.write_lenenc_string(table_alias.empty() ? "t" : table_alias);  // table_orig
    w.write_lenenc_string(column_alias);          // name
    w.write_lenenc_string(column_alias);          // name_orig
    w.write_lenenc_int(0x0c);                     // length of the rest of packet
    w.write_u16(0);                               // character set (binary = 63)
    w.write_u32(0);                               // column length (简化)
    w.write_u8(static_cast<uint8_t>(to_mysql_col_type(type)));
    uint16_t flags = 0;
    if (!nullable)    flags |= MYSQL_NOT_NULL_FLAG;
    if (is_pk)        flags |= MYSQL_PRI_KEY_FLAG;
    if (auto_inc)     flags |= MYSQL_AUTO_INCREMENT_FLAG;
    w.write_u16(flags);
    w.write_u8(0);                                // decimals
    w.write_u16(0);                               // reserved
    return w.buf;
}

// ======== 预编译语句 (binary protocol) ========

std::vector<uint8_t> encode_stmt_prepare_ok(uint32_t stmt_id, uint16_t num_columns,
                                            uint16_t num_params, uint16_t warnings) {
    PacketWriter w;
    w.write_u8(MYSQL_PACKET_OK);   // 0x00
    w.write_u32(stmt_id);
    w.write_u16(num_columns);
    w.write_u16(num_params);
    w.write_u8(0);                 // reserved
    w.write_u16(warnings);
    return w.buf;
}

std::vector<uint8_t> encode_stmt_column_def(const std::string& name,
                                            sqltiny::types::SqlType type) {
    return encode_column_def("", name, type, true, false, false);
}

namespace {
// SqlType → binary protocol 值编码类别
enum class BinKind { Int1, Int2, Int4, Int8, Float4, Float8, DateTime, Str };

BinKind binary_kind_of(sqltiny::types::SqlType t) {
    using sqltiny::types::SqlType;
    switch (t) {
        case SqlType::TinyInt:  return BinKind::Int1;
        case SqlType::SmallInt: return BinKind::Int2;
        case SqlType::Int:      return BinKind::Int4;
        case SqlType::BigInt:   return BinKind::Int8;
        case SqlType::Float:    return BinKind::Float4;
        case SqlType::Double:   return BinKind::Float8;
        case SqlType::Date:
        case SqlType::Time:
        case SqlType::DateTime:
        case SqlType::Timestamp:return BinKind::DateTime;
        default:                return BinKind::Str;   // Varchar/Decimal/Json/Blob/Boolean → lenenc 字符串
    }
}

void write_datetime(PacketWriter& w, const sqltiny::types::Value& v) {
    // DATE/DATETIME/TIMESTAMP 的 binary 编码:长度字节 + 字段
    int32_t year = 0; uint8_t month = 0, day = 0, hour = 0, minute = 0, second = 0; uint16_t millis = 0;
    if (v.kind == sqltiny::types::Value::Kind::Date) {
        year = v.v_date.year; month = v.v_date.month; day = v.v_date.day;
    } else if (v.kind == sqltiny::types::Value::Kind::Time) {
        hour = v.v_time.hour; minute = v.v_time.minute; second = v.v_time.second; millis = v.v_time.millis;
    } else {
        year = v.v_dt.year; month = v.v_dt.month; day = v.v_dt.day;
        hour = v.v_dt.hour; minute = v.v_dt.minute; second = v.v_dt.second; millis = v.v_dt.millis;
    }
    uint16_t us = static_cast<uint16_t>(millis) * 1000;
    if (hour == 0 && minute == 0 && second == 0 && us == 0) {
        w.write_u8(4);   // date only
        w.write_u16(static_cast<uint16_t>(year));
        w.write_u8(month); w.write_u8(day);
    } else if (us == 0) {
        w.write_u8(7);
        w.write_u16(static_cast<uint16_t>(year));
        w.write_u8(month); w.write_u8(day);
        w.write_u8(hour); w.write_u8(minute); w.write_u8(second);
    } else {
        w.write_u8(11);
        w.write_u16(static_cast<uint16_t>(year));
        w.write_u8(month); w.write_u8(day);
        w.write_u8(hour); w.write_u8(minute); w.write_u8(second);
        w.write_u32(us);
    }
}
} // anonymous

std::vector<uint8_t> encode_binary_row(const storage::Row& row,
                                       const std::vector<sqltiny::types::SqlType>& col_types,
                                       uint16_t status_flags) {
    PacketWriter w;
    w.write_u8(0x00);   // binary row header
    size_t n = row.size();
    size_t null_bytes = (n + 9) / 8;
    size_t bm_pos = w.buf.size();
    for (size_t i = 0; i < null_bytes; ++i) w.write_u8(0);
    for (size_t i = 0; i < n; ++i) {
        auto& v = row[i];
        if (v.is_null()) {
            size_t bit = i + 2;
            w.buf[bm_pos + bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
            continue;
        }
        BinKind bk = binary_kind_of(i < col_types.size() ? col_types[i] : sqltiny::types::SqlType::Varchar);
        switch (bk) {
            case BinKind::Int1: {
                int64_t x = 0; v.try_to_int(x);
                w.write_u8(static_cast<uint8_t>(x));
                break;
            }
            case BinKind::Int2: {
                int64_t x = 0; v.try_to_int(x);
                w.write_u16(static_cast<uint16_t>(x));
                break;
            }
            case BinKind::Int4: {
                int64_t x = 0; v.try_to_int(x);
                w.write_u32(static_cast<uint32_t>(x));
                break;
            }
            case BinKind::Int8:
                w.write_i64(v.v_int);
                break;
            case BinKind::Float4: {
                double d = 0; v.try_to_double(d);
                float f = static_cast<float>(d);
                w.write_u32(0);   // placeholder to reserve
                std::memcpy(&w.buf[w.buf.size() - 4], &f, 4);
                break;
            }
            case BinKind::Float8: {
                double d = 0; v.try_to_double(d);
                w.write_u64(0);
                std::memcpy(&w.buf[w.buf.size() - 8], &d, 8);
                break;
            }
            case BinKind::DateTime:
                write_datetime(w, v);
                break;
            case BinKind::Str:
                w.write_lenenc_string(value_to_text(v));
                break;
        }
    }
    (void)status_flags;
    return w.buf;
}

std::vector<std::vector<uint8_t>> encode_result_set(
        const executor::ResultSet& rs, bool deprecate_eof, uint16_t status_flags) {
    std::vector<std::vector<uint8_t>> packets;

    if (!rs.ok) {
        packets.push_back(encode_err_packet(rs.mysql_errno, rs.sql_state, rs.error_message));
        return packets;
    }

    // OK packet (DML 无结果集)
    if (rs.column_names.empty()) {
        packets.push_back(encode_ok_packet(rs.affected_rows, rs.last_insert_id,
            status_flags, rs.warnings));
        return packets;
    }

    // Column count
    PacketWriter count_pkt;
    count_pkt.write_lenenc_int(rs.column_names.size());
    packets.push_back(count_pkt.buf);

    // Column defs
    for (size_t i = 0; i < rs.column_names.size(); ++i) {
        auto type = (i < rs.column_types.size()) ? rs.column_types[i] : types::SqlType::Varchar;
        packets.push_back(encode_column_def("", rs.column_names[i], type, true, false, false));
    }

    // EOF after columns(如果没有 deprecate_eof)
    if (!deprecate_eof) packets.push_back(encode_eof_packet(0, status_flags));

    // Rows
    for (size_t row_idx = 0; row_idx < rs.rows.size(); ++row_idx) {
        auto& row = rs.rows[row_idx];
        PacketWriter w;
        for (size_t i = 0; i < rs.column_names.size(); ++i) {
            if (i >= row.size()) break;
            auto& v = row[i];
            if (v.is_null()) {
                w.write_null();
            } else {
                std::string txt = value_to_text(v);
                w.write_lenenc_string(txt);
            }
        }
        packets.push_back(w.buf);
    }

    // Last packet: OK 或 EOF 标记结束
    if (deprecate_eof) {
        packets.push_back(encode_ok_packet(0, 0, status_flags, 0));
    } else {
        packets.push_back(encode_eof_packet(0, status_flags));
    }
    return packets;
}

// ======== 工具:发送完整 MySQL 响应 ========

bool send_mysql_response(int fd, const executor::ResultSet& rs, uint8_t& seq,
                         bool deprecate_eof, uint16_t status_flags) {
    auto packets = encode_result_set(rs, deprecate_eof, status_flags);
    for (auto& p : packets) {
        auto wrapped = encode_mysql_packet(p, seq++);
        if (!send_all(fd, wrapped.data(), wrapped.size())) return false;
    }
    return true;
}

// ======== 握手后:处理 HandshakeResponse(结构体定义在 mysql_proto.h) ========

std::optional<HandshakeResponse> parse_handshake_response(const uint8_t* data, size_t n) {
    PacketReader r(data, n);
    HandshakeResponse hr;
    if (r.len < 32) return std::nullopt;
    hr.client_cap = r.u32();
    hr.max_packet_size = r.u32();
    hr.charset = r.u8();
    // 23 bytes reserved
    for (int i = 0; i < 23; ++i) r.u8();
    hr.user = r.null_string();
    if (hr.client_cap & CLIENT_SECURE_CONNECTION) {
        uint64_t len = r.lenenc_int();
        for (uint64_t i = 0; i < len; ++i) hr.auth_response.push_back(r.u8());
    } else {
        hr.auth_response = {};
        while (!r.eof() && r.u8() != 0) {}
    }
    if (hr.client_cap & CLIENT_CONNECT_WITH_DB) {
        hr.database = r.null_string();
    }
    if (hr.client_cap & CLIENT_PLUGIN_AUTH && !r.eof()) {
        hr.auth_plugin = r.null_string();
    }
    return hr;
}

} // namespace sqltiny::net
