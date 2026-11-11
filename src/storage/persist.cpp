#include "storage/persist.h"
#include "parser/ast.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>

namespace sqltiny::persist {

// ============ CRC32 (IEEE 802.3, 查表) ============
static uint32_t crc32_table[256];
static bool    crc32_table_init = false;
static void init_crc32_table() {
    if (crc32_table_init) return;
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_init = true;
}
static uint32_t crc32(const uint8_t* data, size_t len, uint32_t init = 0xFFFFFFFF) {
    init_crc32_table();
    uint32_t c = init;
    for (size_t i = 0; i < len; ++i) c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFF;
}
static uint32_t crc32(const std::vector<uint8_t>& v) {
    return crc32(v.data(), v.size());
}

// ============ LE helpers ============
static void put_le_u16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(v & 0xFF); out.push_back((v >> 8) & 0xFF);
}
static void put_le_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((v >> (i*8)) & 0xFF);
}
static void put_le_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back((v >> (i*8)) & 0xFF);
}
static uint16_t get_le_u16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
static uint32_t get_le_u32(const uint8_t* p) {
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) r |= uint32_t(p[i]) << (i*8);
    return r;
}
static uint64_t get_le_u64(const uint8_t* p) {
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) r |= uint64_t(p[i]) << (i*8);
    return r;
}

// ============ codec ============
namespace codec {

std::vector<uint8_t> encode_u64(uint64_t v) {
    std::vector<uint8_t> out;
    put_le_u64(out, v);
    return out;
}
uint64_t decode_u64(std::span<const uint8_t>& buf) {
    uint64_t v = get_le_u64(buf.data());
    buf = buf.subspan(8);
    return v;
}

std::vector<uint8_t> encode_string(const std::string& s) {
    std::vector<uint8_t> out;
    put_le_u32(out, static_cast<uint32_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
    return out;
}
std::optional<std::string> decode_string(std::span<const uint8_t>& buf) {
    if (buf.size() < 4) return std::nullopt;
    uint32_t len = get_le_u32(buf.data());
    buf = buf.subspan(4);
    if (buf.size() < len) return std::nullopt;
    std::string s(reinterpret_cast<const char*>(buf.data()), len);
    buf = buf.subspan(len);
    return s;
}

std::vector<uint8_t> encode_value(const types::Value& v) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>(v.kind));
    switch (v.kind) {
        case types::Value::Kind::Null: break;
        case types::Value::Kind::Bool:    out.push_back(v.v_bool ? 1 : 0); break;
        case types::Value::Kind::Int64:   put_le_u64(out, static_cast<uint64_t>(v.v_int)); break;
        case types::Value::Kind::Double: {
            uint64_t raw; memcpy(&raw, &v.v_double, 8); put_le_u64(out, raw); break;
        }
        case types::Value::Kind::String: {
            auto s = encode_string(v.v_str); out.insert(out.end(), s.begin(), s.end()); break;
        }
        case types::Value::Kind::Blob: {
            put_le_u32(out, static_cast<uint32_t>(v.v_blob.size()));
            out.insert(out.end(), v.v_blob.begin(), v.v_blob.end()); break;
        }
        case types::Value::Kind::Date: {
            put_le_u32(out, static_cast<uint32_t>(v.v_date.year));
            out.push_back(v.v_date.month); out.push_back(v.v_date.day); break;
        }
        case types::Value::Kind::Time: {
            out.push_back(v.v_time.hour); out.push_back(v.v_time.minute);
            out.push_back(v.v_time.second); put_le_u16(out, v.v_time.millis); break;
        }
        case types::Value::Kind::DateTime: {
            put_le_u32(out, static_cast<uint32_t>(v.v_dt.year));
            out.push_back(v.v_dt.month); out.push_back(v.v_dt.day);
            out.push_back(v.v_dt.hour); out.push_back(v.v_dt.minute);
            out.push_back(v.v_dt.second); put_le_u16(out, v.v_dt.millis); break;
        }
    }
    return out;
}
std::optional<types::Value> decode_value(std::span<const uint8_t>& buf) {
    if (buf.empty()) return std::nullopt;
    uint8_t kind_u8 = buf.front();
    buf = buf.subspan(1);
    auto kind = static_cast<types::Value::Kind>(kind_u8);
    types::Value v; v.kind = kind;
    switch (kind) {
        case types::Value::Kind::Null: break;
        case types::Value::Kind::Bool:
            if (buf.empty()) return std::nullopt;
            v.v_bool = buf.front() != 0; buf = buf.subspan(1); break;
        case types::Value::Kind::Int64:
            if (buf.size() < 8) return std::nullopt;
            v.v_int = static_cast<int64_t>(get_le_u64(buf.data())); buf = buf.subspan(8); break;
        case types::Value::Kind::Double: {
            if (buf.size() < 8) return std::nullopt;
            uint64_t raw = get_le_u64(buf.data()); buf = buf.subspan(8);
            memcpy(&v.v_double, &raw, 8); break;
        }
        case types::Value::Kind::String: {
            auto s = decode_string(buf); if (!s) return std::nullopt;
            v.v_str = std::move(*s); break;
        }
        case types::Value::Kind::Blob: {
            if (buf.size() < 4) return std::nullopt;
            uint32_t len = get_le_u32(buf.data()); buf = buf.subspan(4);
            if (buf.size() < len) return std::nullopt;
            v.v_blob.assign(buf.begin(), buf.begin() + len); buf = buf.subspan(len); break;
        }
        case types::Value::Kind::Date: {
            if (buf.size() < 6) return std::nullopt;
            v.v_date.year = static_cast<int32_t>(get_le_u32(buf.data())); buf = buf.subspan(4);
            v.v_date.month = buf[0]; v.v_date.day = buf[1]; buf = buf.subspan(2); break;
        }
        case types::Value::Kind::Time: {
            if (buf.size() < 5) return std::nullopt;
            v.v_time.hour = buf[0]; v.v_time.minute = buf[1]; v.v_time.second = buf[2];
            buf = buf.subspan(3);
            v.v_time.millis = get_le_u16(buf.data()); buf = buf.subspan(2); break;
        }
        case types::Value::Kind::DateTime: {
            if (buf.size() < 10) return std::nullopt;
            v.v_dt.year = static_cast<int32_t>(get_le_u32(buf.data())); buf = buf.subspan(4);
            v.v_dt.month = buf[0]; v.v_dt.day = buf[1]; v.v_dt.hour = buf[2]; v.v_dt.minute = buf[3]; v.v_dt.second = buf[4];
            buf = buf.subspan(5);
            v.v_dt.millis = get_le_u16(buf.data()); buf = buf.subspan(2); break;
        }
    }
    return v;
}

std::vector<uint8_t> encode_row(const storage::Row& row) {
    std::vector<uint8_t> out;
    put_le_u32(out, static_cast<uint32_t>(row.size()));
    for (auto& v : row) {
        auto vb = encode_value(v);
        out.insert(out.end(), vb.begin(), vb.end());
    }
    return out;
}
std::optional<storage::Row> decode_row(std::span<const uint8_t>& buf) {
    if (buf.size() < 4) return std::nullopt;
    uint32_t n = get_le_u32(buf.data()); buf = buf.subspan(4);
    storage::Row row; row.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        auto v = decode_value(buf); if (!v) return std::nullopt;
        row.push_back(std::move(*v));
    }
    return row;
}

std::vector<uint8_t> encode_table_def(const storage::TableDef& def) {
    std::vector<uint8_t> out;
    // name
    auto nb = encode_string(def.name);
    out.insert(out.end(), nb.begin(), nb.end());
    // columns
    put_le_u32(out, static_cast<uint32_t>(def.columns.size()));
    for (auto& c : def.columns) {
        // ColumnDef: name + type info + flags + optional default (简化:default 直接 skip,序列化 Value)
        auto cb = encode_string(c.name); out.insert(out.end(), cb.begin(), cb.end());
        // TypeInfo: SqlType(1) + length(2) + scale(2) + nullable(1) + unsigned(1)
        out.push_back(static_cast<uint8_t>(c.type.type));
        put_le_u16(out, c.type.length);
        put_le_u16(out, c.type.scale);
        out.push_back(c.nullable ? 1 : 0);
        out.push_back(c.type.unsigned_ ? 1 : 0);
        // flags: pk, auto_inc
        uint8_t flags = 0;
        if (c.is_primary_key) flags |= 1;
        if (c.is_auto_increment) flags |= 2;
        out.push_back(flags);
        // default_value (optional — 只支持 Literal Expr,带 bool 前缀)
        bool has_def = c.default_value != nullptr;
        out.push_back(has_def ? 1 : 0);
        if (has_def) {
            auto* lit = c.default_value.get();
            if (lit->kind == ast::ExprKind::Literal) {
                auto vb = encode_value(lit->literal);
                out.insert(out.end(), vb.begin(), vb.end());
            } else {
                // 非 Literal 默认值 — 简化跳过
                out.pop_back();  // 撤回 has_def 标记
                out.push_back(0);
            }
        }
    }
    // primary_key_cols groups (简化:只保留第一组)
    put_le_u32(out, static_cast<uint32_t>(def.primary_key_cols.size()));
    for (auto& grp : def.primary_key_cols) {
        put_le_u32(out, static_cast<uint32_t>(grp.size()));
        for (auto& cn : grp) {
            auto sb = encode_string(cn); out.insert(out.end(), sb.begin(), sb.end());
        }
    }
    return out;
}
std::optional<storage::TableDef> decode_table_def(std::span<const uint8_t>& buf) {
    storage::TableDef def;
    auto name = decode_string(buf); if (!name) return std::nullopt;
    def.name = std::move(*name);
    if (buf.size() < 4) return std::nullopt;
    uint32_t ncols = get_le_u32(buf.data()); buf = buf.subspan(4);
    def.columns.reserve(ncols);
    for (uint32_t i = 0; i < ncols; ++i) {
        auto cname = decode_string(buf); if (!cname) return std::nullopt;
        ast::ColumnDef cd; cd.name = std::move(*cname);
        if (buf.size() < 7) return std::nullopt;
        cd.type.type = static_cast<types::SqlType>(buf[0]);
        cd.type.length = get_le_u16(buf.data() + 1);
        cd.type.scale  = get_le_u16(buf.data() + 3);
        cd.nullable    = buf[5] != 0;
        cd.type.unsigned_ = buf[6] != 0;
        buf = buf.subspan(7);
        if (buf.empty()) return std::nullopt;
        uint8_t flags = buf[0]; buf = buf.subspan(1);
        cd.is_primary_key = (flags & 1) != 0;
        cd.is_auto_increment = (flags & 2) != 0;
        if (buf.empty()) return std::nullopt;
        uint8_t has_def = buf[0]; buf = buf.subspan(1);
        if (has_def) {
            auto v = decode_value(buf); if (!v) return std::nullopt;
            // make_literal 返回 unique_ptr<Expression>,再包成 shared_ptr
            auto up = ast::Expression::make_literal(std::move(*v));
            cd.default_value = std::shared_ptr<ast::Expression>(std::move(up));
        }
        def.columns.push_back(std::move(cd));
    }
    if (buf.size() < 4) return std::nullopt;
    uint32_t ngroups = get_le_u32(buf.data()); buf = buf.subspan(4);
    def.primary_key_cols.reserve(ngroups);
    for (uint32_t g = 0; g < ngroups; ++g) {
        if (buf.size() < 4) return std::nullopt;
        uint32_t n = get_le_u32(buf.data()); buf = buf.subspan(4);
        std::vector<std::string> grp; grp.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            auto cn = decode_string(buf); if (!cn) return std::nullopt;
            grp.push_back(std::move(*cn));
        }
        def.primary_key_cols.push_back(std::move(grp));
    }
    return def;
}

} // namespace codec

// ============ WalRecord ============
std::vector<uint8_t> WalRecord::encode() const {
    std::vector<uint8_t> out;
    out.reserve(6 + payload.size() + 4);
    out.push_back(static_cast<uint8_t>(type));
    out.push_back(flags);
    put_le_u32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    uint32_t c = crc32(out.data(), out.size());
    put_le_u32(out, c);
    return out;
}

std::optional<WalRecord> WalRecord::decode(std::span<const uint8_t> bytes) {
    if (bytes.size() < 10) return std::nullopt; // 最少 10 字节 (type+flags+len=0+crc)
    uint32_t stored_crc = get_le_u32(bytes.data() + bytes.size() - 4);
    auto content = bytes.subspan(0, bytes.size() - 4);
    uint32_t calc_crc = crc32(content.data(), content.size());
    if (stored_crc != calc_crc) return std::nullopt;
    WalRecord r;
    r.type  = static_cast<RecordType>(content[0]);
    r.flags = content[1];
    uint32_t len = get_le_u32(content.data() + 2);
    content = content.subspan(6);
    if (content.size() != len) return std::nullopt;
    r.payload.assign(content.begin(), content.end());
    return r;
}

// ============ WalWriter ============
WalWriter::WalWriter(const std::string& path) : path_(path) {}
WalWriter::~WalWriter() { close(); }

bool WalWriter::open() {
    // 确保父目录存在
    auto sep = path_.find_last_of("\\/");
    if (sep != std::string::npos) {
        std::string dir = path_.substr(0, sep);
        if (!dir.empty()) {
#ifdef _WIN32
            std::string sys_cmd = "mkdir \"" + dir + "\" 2>NUL";
            (void)std::system(sys_cmd.c_str());
#else
            std::string sys_cmd = "mkdir -p \"" + dir + "\"";
            (void)std::system(sys_cmd.c_str());
#endif
        }
    }
    fh_ = std::fopen(path_.c_str(), "r+b");
    if (!fh_) {
        fh_ = std::fopen(path_.c_str(), "w+b");
        if (!fh_) return false;
        // 写 header
        WalHeader hdr;
        std::fseek(fh_, 0, SEEK_SET);
        if (std::fwrite(&hdr, sizeof(hdr), 1, fh_) != 1) { std::fclose(fh_); fh_ = nullptr; return false; }
        header_written_ = true;
    } else {
        header_written_ = true;
        // 校验 header magic
        WalHeader hdr;
        std::fseek(fh_, 0, SEEK_SET);
        if (std::fread(&hdr, sizeof(hdr), 1, fh_) != 1) { std::fclose(fh_); fh_ = nullptr; return false; }
        if (std::memcmp(hdr.magic, "SQTWAL", 6) != 0 || hdr.version != 1) {
            // 不是合法 WAL — 截断重建
            std::fclose(fh_); fh_ = nullptr;
            fh_ = std::fopen(path_.c_str(), "w+b");
            if (!fh_) return false;
            WalHeader new_hdr;
            if (std::fwrite(&new_hdr, sizeof(new_hdr), 1, fh_) != 1) { std::fclose(fh_); fh_ = nullptr; return false; }
        }
    }
    return true;
}

void WalWriter::close() {
    if (fh_) { std::fclose(fh_); fh_ = nullptr; }
}

bool WalWriter::append(const WalRecord& rec) {
    if (!fh_) return false;
    auto bytes = rec.encode();
    // 跳到文件尾追加
    std::fseek(fh_, 0, SEEK_END);
    if (std::fwrite(bytes.data(), 1, bytes.size(), fh_) != bytes.size()) return false;
    if (sync_on_append_) std::fflush(fh_);
    return true;
}

// ============ WalReader ============
WalReader::WalReader(const std::string& path) : path_(path) {}
WalReader::~WalReader() { close(); }

bool WalReader::open() {
    fh_ = std::fopen(path_.c_str(), "rb");
    if (!fh_) return false;
    WalHeader hdr;
    if (std::fread(&hdr, sizeof(hdr), 1, fh_) != 1) return false;
    if (std::memcmp(hdr.magic, "SQTWAL", 6) != 0 || hdr.version != 1) return false;
    return true;
}

void WalReader::close() {
    if (fh_) { std::fclose(fh_); fh_ = nullptr; }
}

void WalReader::seek_to_beginning() {
    if (fh_) {
        std::fseek(fh_, sizeof(WalHeader), SEEK_SET);
    }
}

std::optional<WalRecord> WalReader::read_next(bool* out_truncated) {
    if (!fh_) return std::nullopt;
    if (out_truncated) *out_truncated = false;
    // 先读 type + flags + len
    uint8_t hdr[6];
    if (std::fread(hdr, 1, 6, fh_) != 6) {
        // EOF
        return std::nullopt;
    }
    uint32_t payload_len = get_le_u32(hdr + 2);
    // 读 payload + crc
    size_t rec_total = 6 + payload_len + 4;
    std::vector<uint8_t> buf(rec_total);
    std::memcpy(buf.data(), hdr, 6);
    if (payload_len > 0 && std::fread(buf.data() + 6, 1, payload_len, fh_) != payload_len) {
        if (out_truncated) *out_truncated = true;
        return std::nullopt;
    }
    uint8_t crc[4];
    if (std::fread(crc, 1, 4, fh_) != 4) {
        if (out_truncated) *out_truncated = true;
        return std::nullopt;
    }
    std::memcpy(buf.data() + 6 + payload_len, crc, 4);
    auto rec = WalRecord::decode(buf);
    if (!rec) {
        if (out_truncated) *out_truncated = true;
        return std::nullopt;
    }
    return rec;
}

} // namespace sqltiny::persist
