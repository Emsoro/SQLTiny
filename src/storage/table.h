#pragma once

#include "parser/ast.h"
#include "types/value.h"
#include "storage/index_avl.h"
#include <string>
#include <vector>
#include <optional>
#include <map>
#include <memory>

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
/// M2 版本:所有索引都存 Value 完整拷贝,row 本身也是一份拷贝(内存表,先简单)
class Table {
public:
    explicit Table(TableDef def);

    const TableDef& def() const { return def_; }

    /// 插入一行。成功返回 true;主键冲突返回 false
    bool insert(const Row& row, int64_t* out_row_id = nullptr);

    /// 按主键删除
    bool delete_by_pk(const Row& pk_values);

    /// 按主键查找
    std::optional<int64_t> find_row_id_by_pk(const Row& pk_values) const;

    /// 迭代器回调:返回 false 停止
    using RowCallback = std::function<bool(int64_t row_id, const Row& row)>;
    void for_each(const RowCallback& cb) const;

    /// 所有行 id
    std::vector<int64_t> all_row_ids() const;

    size_t row_count() const { return row_map_.size(); }

    /// 取一行的完整值
    std::optional<Row> get_row(int64_t row_id) const;

    /// 自增主键下一个值(仅整数 PK)
    int64_t next_auto_increment();

    /// M5.5 Snapshot 重建 + AUTO_INCREMENT 收尾:恢复 cursor
    void set_auto_inc_cursor(int64_t v) { auto_inc_cursor_ = v; }
    int64_t auto_inc_cursor() const { return auto_inc_cursor_; }

private:
    TableDef def_;
    // 行存储:row_id → Row
    std::map<int64_t, Row> row_map_;
    int64_t next_row_id_ = 1;
    int64_t auto_inc_cursor_ = 0;

    // 主键 AVL 索引 —— key 是 PK 列值按顺序拼接
    std::unique_ptr<IndexAvl> pk_index_;

    Row pk_from_row(const Row& row) const;
    Key pk_key(const Row& pk) const;
};

} // namespace sqltiny::storage
