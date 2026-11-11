#pragma once

#include "storage/table.h"
#include <string>
#include <map>
#include <memory>
#include <optional>

namespace sqltiny::catalog {

/// Catalog —— 持有所有表实例(分 database)
class Catalog {
public:
    Catalog() = default;

    /// 创建表。已存在返回 false(除非 if_not_exists)
    bool create_table(const ast::CreateTableStmt& stmt);

    /// 删除表
    bool drop_table(const std::string& name, bool if_exists);

    /// 查是否存在
    bool table_exists(const std::string& name) const;

    /// 取表(可变)
    storage::Table* get_table(const std::string& name);
    const storage::Table* get_table(const std::string& name) const;

    /// 所有表名列表
    std::vector<std::string> table_names() const;

private:
    std::map<std::string, std::unique_ptr<storage::Table>> tables_;
};

} // namespace sqltiny::catalog
