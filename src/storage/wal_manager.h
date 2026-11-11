#pragma once

#include "storage/persist.h"
#include <memory>
#include <string>
#include <mutex>
#include <functional>

namespace sqltiny {

// 前向
namespace catalog { class Catalog; }
namespace storage { class Table; }

/// 持久化层: WAL Writer + Replay
/// 单例挂在 Database 上。线程安全:内部互斥锁。
class WalManager {
public:
    WalManager() = default;
    ~WalManager() { close(); }

    WalManager(const WalManager&) = delete;
    WalManager& operator=(const WalManager&) = delete;

    /// 打开/创建 WAL 文件并 replay 到 catalog。
    /// wal_path: WAL 文件完整路径 (例如 "sqltiny_data/sql.wal")。
    /// 若 wal_path 为空字符串,则持久化层禁用 (内存模式,M1-M4 兼容)。
    /// 返回 true = 成功(或禁用模式直接返回 true)
    bool open(const std::string& wal_path, catalog::Catalog& catalog);

    /// 优雅关闭:写 EndOfFile record,fsync,close
    void close();
    bool is_open() const { return writer_ && writer_->is_open(); }
    bool enabled() const { return !wal_path_.empty(); }

    // ============ Append helpers(Executor/Table 调用) ============

    // Catalog
    bool append_create_database(const std::string& db_name);
    bool append_drop_database(const std::string& db_name);
    bool append_use_database(const std::string& db_name);

    // Table DDL (TableDef 完整序列化)
    bool append_create_table(const std::string& db_name, const storage::TableDef& def);
    bool append_drop_table(const std::string& db_name, const std::string& table_name);

    // Row DML
    bool append_insert_row(const std::string& db_name, const std::string& table_name,
                           int64_t row_id, const storage::Row& row);
    bool append_delete_row(const std::string& db_name, const std::string& table_name,
                           const storage::Row& pk_values);
    bool append_update_row(const std::string& db_name, const std::string& table_name,
                           const storage::Row& pk_values, const storage::Row& new_row);

    // 内部状态
    bool append_auto_inc_update(const std::string& db_name, const std::string& table_name, uint64_t new_cursor);

    // 通用 append(上面的 helper 最终都走这里)
    bool append(const persist::WalRecord& rec);

    // ============ Checkpoint (M5.5) ============
    void request_checkpoint() { checkpoint_pending_.store(true); }
    bool checkpoint_pending() const { return checkpoint_pending_.load(); }

private:
    std::string wal_path_;
    std::unique_ptr<persist::WalWriter> writer_;
    std::mutex mu_;       // 所有 append 共享一把锁
    std::atomic<bool> checkpoint_pending_{false};

    /// Replay WAL 到 catalog。返回实际应用的 record 数。
    /// is_truncated = true 表示 WAL 中途损坏/崩溃 —— 前面的有效,后面的丢
    size_t replay(catalog::Catalog& catalog, bool& out_truncated);
};

} // namespace sqltiny
