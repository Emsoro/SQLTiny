#pragma once

#include "catalog/catalog.h"
#include <string>
#include <cstdio>

namespace sqltiny::persist {

/// 快照文件头 (固定 64B)
struct SnapshotHeader {
    char     magic[8]    = {'S','Q','T','S','N','A','P','\0'};
    uint32_t version     = 1;
    uint64_t scn         = 0;   // checkpoint 时的 SCN (M5.6 用)
    uint64_t wal_offset  = 0;  // checkpoint 完成时 WAL 文件偏移 — replay 从这里之后开始
    uint64_t db_count    = 0;
    uint64_t record_count = 0; // 总 row 数
    char     reserved[32] = {};
};

/// Catalog 快照序列化 — 遍历所有 db/table/row,写一个完整 .data 文件
/// 文件结构:
///   [SnapshotHeader 64B]
///   [Database count 4B]
///   [Database N]: [name len(4)+name bytes] [Table count 4B]
///     [Table N]:   [TableDef 完整] [row_count 4B] [Row N]...
///                  [auto_inc_cursor 8B]
///   [CRC32 of everything before 4B]
class WalSnapshot {
public:
    explicit WalSnapshot(const std::string& data_path);

    /// 写快照:遍历 catalog 所有 db/table/row
    /// 返回 true=成功
    bool write(const catalog::Catalog& catalog, uint64_t wal_offset, uint64_t scn = 0);

    /// 读快照:重建 catalog (**清空后重填**)
    /// 返回 record_count (总 row 数),失败返回 -1
    int64_t read_and_rebuild(catalog::Catalog& out_catalog, uint64_t* out_wal_offset = nullptr);

    bool file_exists() const;
    bool is_valid() const;  // 校验 header magic

    /// 原子替换:先写 .tmp,然后 rename 覆盖 .data
    static bool atomic_replace(const std::string& final_path);

private:
    std::string path_;
    std::string tmp_path() const { return path_ + ".tmp"; }

    bool _write_to(catalog::Catalog& catalog, FILE* fh, uint64_t wal_offset, uint64_t scn);
    int64_t _read_from(catalog::Catalog& catalog, FILE* fh, uint64_t* out_wal_offset);
};

} // namespace sqltiny::persist
