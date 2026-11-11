#pragma once

#include "types/value.h"
#include "storage/table.h"
#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <span>

namespace sqltiny::persist {

/// WAL record type. Values are stable — 不要在中间插新类型.
enum class RecordType : uint8_t {
    // --- Transaction (M5.6 MVCC) ---
    BeginTxn     = 0x01,   // payload: txn_id uint64
    CommitTxn    = 0x02,   // payload: txn_id uint64, scn uint64
    AbortTxn     = 0x03,   // payload: txn_id uint64

    // --- Catalog ---
    CreateDatabase = 0x10, // payload: db_name string
    DropDatabase   = 0x11, // payload: db_name string
    UseDatabase    = 0x12, // payload: db_name string (server 级别记录, replay 时设 current_database)

    // --- Table DDL ---
    CreateTable   = 0x20, // payload: db_name | serialized TableDef
    DropTable     = 0x21, // payload: db_name | table_name
    AlterTable    = 0x22, // payload: db_name | table_name | op_code | op_payload (M5.6+)

    // --- Row DML ---
    InsertRow     = 0x30, // payload: db | table | row_id | Row
    DeleteRow     = 0x31, // payload: db | table | pk_values Row
    UpdateRow     = 0x32, // payload: db | table | pk_values Row | new Row
    TruncateTable = 0x33, // payload: db | table

    // --- Internal state ---
    AutoIncUpdate = 0x40, // payload: db | table | new_cursor uint64

    // --- Checkpoint ---
    CheckpointStart = 0x50, // payload: checkpoint_seq uint64
    CheckpointEnd   = 0x51, // payload: checkpoint_seq uint64

    // --- Terminal (M5.7 前不强制写,但读时用来判断完整/截断) ---
    EndOfFile     = 0xFF,  // payload: empty, CRC=0 表示干净关闭;CRC 有效但中途停 = 崩溃
};

/// WAL 文件头 (固定 64 字节)
struct WalHeader {
    char     magic[8]       = {'S','Q','T','W','A','L','\0','\0'};
    uint32_t version        = 1;
    uint64_t checkpoint_seq = 0;   // 最近一次完成的 checkpoint 编号, 0=尚未 checkpoint
    uint64_t checkpoint_end_offset = 0;  // checkpoint 结束时的文件偏移
    char     reserved[40]   = {};
};

/// WAL 单条 record 序列化帮助
/// Layout: [type(1)] [flags(1)] [payload_len(4) LE] [payload(N)] [crc32(4) of type+flags+len+payload]
struct WalRecord {
    RecordType type;
    uint8_t    flags;
    std::vector<uint8_t> payload;

    /// 编码成 wire bytes (返回完整 record)
    std::vector<uint8_t> encode() const;

    /// 从 wire bytes 解码, crc 校验 (坏记录返回 nullopt)
    static std::optional<WalRecord> decode(std::span<const uint8_t> bytes);

    /// 估算编码后字节数 (不含长度前缀)
    size_t wire_size() const { return 6 + payload.size() + 4; }
};

/// 简单二进制序列化/反序列化 Value / Row / TableDef
namespace codec {
    /// 序列化一个 Value (带 Kind tag)
    std::vector<uint8_t> encode_value(const types::Value& v);
    /// 反序列化
    std::optional<types::Value> decode_value(std::span<const uint8_t>& buf);

    /// 序列化 Row = vector<Value>
    std::vector<uint8_t> encode_row(const storage::Row& row);
    /// 反序列化
    std::optional<storage::Row> decode_row(std::span<const uint8_t>& buf);

    /// 序列化 TableDef (列定义 + PK 组,**不含** indexes 简化)
    std::vector<uint8_t> encode_table_def(const storage::TableDef& def);
    /// 反序列化
    std::optional<storage::TableDef> decode_table_def(std::span<const uint8_t>& buf);

    /// 带长度前缀的 string
    std::vector<uint8_t> encode_string(const std::string& s);
    std::optional<std::string> decode_string(std::span<const uint8_t>& buf);

    /// uint64 LE
    std::vector<uint8_t> encode_u64(uint64_t v);
    uint64_t decode_u64(std::span<const uint8_t>& buf);
}

/// WAL writer — 纯追加文件
class WalWriter {
public:
    explicit WalWriter(const std::string& path);
    ~WalWriter();

    /// 打开/创建 WAL 文件,写 header (如果新建);现有文件只做头部校验
    bool open();
    void close();
    bool is_open() const { return fh_ != nullptr; }

    /// 追加一条 record, 立即 fsync (M5.1 简化:每次 append 就 sync,后续可批量 sync)
    bool append(const WalRecord& rec);

    /// 追加时同时 sync (默认 true)
    void set_sync_on_append(bool v) { sync_on_append_ = v; }

private:
    std::string path_;
    FILE*       fh_ = nullptr;
    bool        sync_on_append_ = true;
    bool        header_written_ = false;
};

/// WAL reader — 顺序扫描所有 record, CRC 校验,遇坏 record 停止(崩溃残留)
class WalReader {
public:
    explicit WalReader(const std::string& path);
    ~WalReader();

    bool open();
    void close();
    bool is_open() const { return fh_ != nullptr; }

    /// 顺序读下一条 record. 返回 nullopt 表示 EOF 或遇到坏 record (EOF=正常结束,坏=中途崩溃)
    /// out_truncated=true 时说明读到了坏 record / CRC 错误 —— 上一条之后的都是崩溃残留,应丢弃
    std::optional<WalRecord> read_next(bool* out_truncated = nullptr);

    /// 回到头部 (重新 replay 用)
    void seek_to_beginning();

private:
    std::string path_;
    FILE*       fh_ = nullptr;
};

/// 便捷宏 — 构造带 payload 的 WAL record
#define WAL_RECORD(type) (::sqltiny::persist::WalRecord{ type, 0, {} })

} // namespace sqltiny::persist
