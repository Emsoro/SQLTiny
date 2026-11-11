#pragma once

#include "executor/executor.h"
#include <vector>
#include <cstdint>
#include <string>
#include <cstring>

namespace sqltiny::net {

// ---------- 协议常量 ----------

// capability flags
constexpr uint32_t CLIENT_PROTOCOL_41       = 0x00000200;
constexpr uint32_t CLIENT_SECURE_CONNECTION  = 0x00008000;
constexpr uint32_t CLIENT_PLUGIN_AUTH       = 0x00080000;
constexpr uint32_t CLIENT_CONNECT_WITH_DB   = 0x00000008;
constexpr uint32_t CLIENT_DEPRECATE_EOF     = 0x01000000;

// command ids
constexpr uint8_t COM_SLEEP                 = 0x00;
constexpr uint8_t COM_QUIT                  = 0x01;
constexpr uint8_t COM_INIT_DB               = 0x02;
constexpr uint8_t COM_QUERY                 = 0x03;
constexpr uint8_t COM_FIELD_LIST            = 0x04;
constexpr uint8_t COM_PING                  = 0x0e;
constexpr uint8_t COM_STMT_PREPARE          = 0x16;
constexpr uint8_t COM_STMT_EXECUTE          = 0x17;

// packet types sent by server
constexpr uint8_t MYSQL_PACKET_OK           = 0x00;
constexpr uint8_t MYSQL_PACKET_ERR          = 0xff;
constexpr uint8_t MYSQL_PACKET_EOF          = 0xfe;
constexpr uint8_t MYSQL_PACKET_TEXT_RESULT  = 0x01;

// server status flags
constexpr uint16_t SERVER_STATUS_AUTOCOMMIT = 0x0002;

// field flags
constexpr uint16_t MYSQL_NOT_NULL_FLAG      = 0x0001;
constexpr uint16_t MYSQL_PRI_KEY_FLAG       = 0x0002;
constexpr uint16_t MYSQL_UNIQUE_KEY_FLAG    = 0x0004;
constexpr uint16_t MYSQL_MULT_KEY_FLAG      = 0x0008;
constexpr uint16_t MYSQL_AUTO_INCREMENT_FLAG= 0x0040;

// ---------- Packet 读写 ----------

class PacketWriter {
public:
    std::vector<uint8_t> buf;

    void write_u8(uint8_t v) { buf.push_back(v); }
    void write_u16(uint16_t v) { write_u8(v & 0xff); write_u8((v >> 8) & 0xff); }
    void write_u32(uint32_t v) {
        write_u16(v & 0xffff); write_u16((v >> 16) & 0xffff);
    }
    void write_u64(uint64_t v) {
        write_u32(v & 0xffffffff); write_u32((v >> 32) & 0xffffffff);
    }
    void write_i64(int64_t v) { write_u64(static_cast<uint64_t>(v)); }

    void write_string_null(const std::string& s) {
        for (char c : s) buf.push_back(static_cast<uint8_t>(c));
        buf.push_back(0);
    }
    void write_string_nolen(const std::string& s) {
        for (char c : s) buf.push_back(static_cast<uint8_t>(c));
    }
    void write_lenenc_int(uint64_t v) {
        if (v < 251) { write_u8(static_cast<uint8_t>(v)); }
        else if (v < 1 << 16) { write_u8(0xfc); write_u16(static_cast<uint16_t>(v)); }
        else if (v < 1ULL << 24) {
            write_u8(0xfd);
            write_u16(static_cast<uint16_t>(v & 0xffff));
            write_u8(static_cast<uint8_t>((v >> 16) & 0xff));
        } else {
            write_u8(0xfe);
            write_u64(v);
        }
    }
    void write_lenenc_string(const std::string& s) {
        write_lenenc_int(s.size());
        write_string_nolen(s);
    }
    void write_null() { write_lenenc_int(0xfb); }
    void write_bytes(const uint8_t* p, size_t n) {
        buf.insert(buf.end(), p, p + n);
    }
};

class PacketReader {
public:
    const uint8_t* data;
    size_t len;
    size_t pos = 0;

    PacketReader(const uint8_t* d, size_t n) : data(d), len(n) {}

    bool eof() const { return pos >= len; }
    uint8_t u8() { return data[pos++]; }
    uint16_t u16() {
        uint16_t v = data[pos] | (data[pos + 1] << 8); pos += 2; return v;
    }
    uint32_t u32() {
        uint32_t v = u16() | (static_cast<uint32_t>(u16()) << 16); return v;
    }
    uint64_t u64() {
        uint64_t lo = u32(); uint64_t hi = u32(); return lo | (hi << 32);
    }
    std::string null_string() {
        std::string s;
        while (pos < len && data[pos] != 0) { s.push_back(static_cast<char>(data[pos++])); }
        if (pos < len) pos++;
        return s;
    }
    std::string lenenc_string() {
        uint64_t n = lenenc_int();
        std::string s;
        for (uint64_t i = 0; i < n && pos < len; ++i) s.push_back(static_cast<char>(data[pos++]));
        return s;
    }
    uint64_t lenenc_int() {
        uint8_t first = u8();
        if (first < 0xfb) return first;
        if (first == 0xfc) return u16();
        if (first == 0xfd) return u16() | (static_cast<uint64_t>(u8()) << 16);
        if (first == 0xfe) return u64();
        return 0;
    }
};

// ---------- 编解码 ----------

/// 读一个 MySQL packet(带 4 字节 header:3 字节 len + 1 字节 seq)
std::vector<uint8_t> read_mysql_packet(int socket_fd, uint8_t& seq_out);

/// 把 payload 按 MySQL packet 分包(每包最多 0xffffff 字节),返回完整字节流
std::vector<uint8_t> encode_mysql_packet(const std::vector<uint8_t>& payload, uint8_t seq);

// 高层包
std::vector<uint8_t> encode_handshake_response(uint32_t client_cap,
                                               const std::string& user,
                                               const std::vector<uint8_t>& auth_response);

struct HandshakeResponse {
    uint32_t client_cap = 0;
    uint32_t max_packet_size = 0;
    uint8_t  charset = 0;
    std::string user;
    std::vector<uint8_t> auth_response;
    std::string database;
    std::string auth_plugin;
};
std::optional<HandshakeResponse> parse_handshake_response(const uint8_t* data, size_t n);
std::vector<uint8_t> encode_ok_packet(uint64_t affected_rows, uint64_t last_insert_id,
                                      uint16_t status_flags, uint16_t warnings,
                                      const std::string& info = "");
std::vector<uint8_t> encode_err_packet(uint16_t err, std::string_view sql_state,
                                       std::string_view message);
std::vector<uint8_t> encode_eof_packet(uint16_t warnings, uint16_t status_flags);

/// ResultSet 编码:text result protocol
std::vector<std::vector<uint8_t>> encode_result_set(const executor::ResultSet& rs, bool deprecate_eof);

// 辅助
std::string value_to_text(const sqltiny::types::Value& v);
std::string random_bytes_hex(int n);
std::vector<uint8_t> compute_mysql_native_password(const std::string& password,
                                                   const std::vector<uint8_t>& salt);

struct HandshakeResponse;
std::optional<HandshakeResponse> parse_handshake_response(const uint8_t* data, size_t n);

// 低层 packet 工具(实现于 mysql_server.cpp)
bool send_all(int fd, const uint8_t* p, size_t n);
bool recv_all(int fd, uint8_t* p, size_t n);
std::vector<uint8_t> read_mysql_packet(int socket_fd, uint8_t& seq_out);
std::vector<uint8_t> encode_mysql_packet(const std::vector<uint8_t>& payload, uint8_t seq);
std::vector<uint8_t> encode_handshake_v10(uint32_t tid, const std::vector<uint8_t>& auth);
bool send_mysql_response(int fd, const executor::ResultSet& rs, uint8_t& seq, bool deprecate_eof);

} // namespace sqltiny::net
