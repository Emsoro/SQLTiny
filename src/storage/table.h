#pragma once

#include "parser/ast.h"
#include "types/value.h"
#include "storage/index_avl.h"
#include <string>
#include <vector>
#include <optional>
#include <map>
#include <memory>
#include <mutex>

namespace sqltiny::storage {

/// 一行 —— 就是 Value 列表,按列顺序对齐
using Row = std::vector<types::Value>;

/// 表定义(元数据,跨 session 共享)
struct TableDef {
    std::string name;
    std::vector<ast::ColumnDef> columns;
    std::vector<std::vector<std::string>> primary_key_cols;  // 主键列名组
    std::map<std::string, ast::CreateTableStmt::IdxDef> indexes;  // secondary index
    bool if_not_exists_handled = false;

    // 方便查找
    int column_index(const std::string& col_name) const {
        for (size_t i = 0; i < columns.size(); ++i) {
            if (_stricmp(columns[i].name.c_str(), col_name.c_str()) == 0) return static_cast<int>(i);
        }
        return -1;
    }
    bool has_column(const std::string& col_name) const { return column_index(col_name) >= 0; }
};

/// 表实例 —— 持有主索引 + 所有 secondary 索引 + 行存储
class Table {
public:
    explicit Table(TableDef def);

    const TableDef& def() const { return def_; }

    /// 插入一行。成功返回 true;主键冲突返回 false
    /// createSCN = 0 表示"立即可见" (M1-M4 兼容,隐式 active)
    bool insert(const Row& row, int64_t* out_row_id = nullptr, uint64_t createSCN = 0);

    /// 按主键删除 (deleteSCN = 0 表示"立即物理删除" —— M1-M4 兼容)
    bool delete_by_pk(const Row& pk_values, uint64_t deleteSCN = 0);

    /// 按主键查找
    std::optional<int64_t> find_row_id_by_pk(const Row& pk_values) const;

    /// 迭代器回调:返回 false 停止
    using RowCallback = std::function<bool(int64_t row_id, const Row& row)>;
    /// 遍历所有行 (包含标记 deleted 但没真正物理删的版本)
    void for_each(const RowCallback& cb) const;

    /// M5.6 MVCC:按 querySCN 只遍历 querySCN 可见的版本
    /// querySCN=0 时退化为 for_each() (兼容旧调用)
    void for_each_visible(uint64_t querySCN, const RowCallback& cb) const;

    /// 所有行 id (只返回物理存在且可见的)
    std::vector<int64_t> all_row_ids(uint64_t querySCN = 0) const;

    size_t row_count() const { return row_map_.size(); }

    /// 取一行的完整值
    std::optional<Row> get_row(int64_t row_id) const;

    /// 自增主键下一个值(仅整数 PK)
    int64_t next_auto_increment();

    /// M5.5 Snapshot 重建 + AUTO_INCREMENT 收尾:恢复 cursor
    void set_auto_inc_cursor(int64_t v) { auto_inc_cursor_ = v; }
    int64_t auto_inc_cursor() const { return auto_inc_cursor_; }

    /// M5.6 MVCC:行版本元信息读写
    struct VersionInfo { uint64_t createSCN = 0; uint64_t deleteSCN = 0; };
    VersionInfo get_version(int64_t row_id) const;
    void set_version(int64_t row_id, VersionInfo vi);

private:
    TableDef def_;
    // 行存储:row_id → Row
    std::map<int64_t, Row> row_map_;
    int64_t next_row_id_ = 1;
    int64_t auto_inc_cursor_ = 0;

    // M5.6 MVCC:row_id → 版本元信息
    std::map<int64_t, VersionInfo> versions_;

    // M6: 表级 mutex, 保护 row_map_ / versions_ / AVL 索引并发访问
    mutable std::mutex mu_;

    // 主键 AVL 索引 —— key 是 PK 列值按顺序拼接
    std::unique_ptr<IndexAvl> pk_index_;

    Row pk_from_row(const Row& row) const;
    Key pk_key(const Row& pk) const;
};

} // namespace sqltiny::storage
