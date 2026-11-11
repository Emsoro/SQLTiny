#pragma once

#include "storage/table.h"
#include <string>
#include <map>
#include <memory>
#include <optional>

namespace sqltiny::catalog {

/// Catalog —— 多库模型:databases_[db].tables_[table]
class Catalog {
public:
    Catalog();

    // ---------- Database ----------
    bool create_database(const std::string& name, bool if_not_exists = false);
    bool drop_database(const std::string& name, bool if_exists = false);
    bool database_exists(const std::string& name) const;
    bool switch_database(const std::string& name);   // USE db —— 失败返回 false
    std::string current_database() const { return current_database_; }
    std::vector<std::string> list_databases() const;

    // ---------- Table ----------
    bool create_table(const ast::CreateTableStmt& stmt);
    bool drop_table(const std::string& name, bool if_exists);
    bool table_exists(const std::string& name) const;

    storage::Table* get_table(const std::string& name);
    const storage::Table* get_table(const std::string& name) const;

    /// M6: 指定库名拿表 (跨库查询 / TransactionManager 用)
    storage::Table* get_table_in_db(const std::string& db_name, const std::string& table_name);
    const storage::Table* get_table_in_db(const std::string& db_name, const std::string& table_name) const;

    std::vector<std::string> table_names() const;

    /// M5.5 Snapshot 用:遍历所有库 (顺序)
    void for_each_database(const std::function<void(const std::string& db_name)>& cb) const;
    /// M5.5 Snapshot 用:遍历指定库的所有表
    void for_each_table_in(const std::string& db_name,
                           const std::function<void(const std::string& tname, const storage::Table*)>& cb) const;
    /// M5.5 Snapshot 重建前清空 (保留 default)
    void clear_all_except_default();

private:
    std::string current_database_ = "default";
    /// 每个 database 持有自己的 table map
    struct DbTables {
        std::map<std::string, std::unique_ptr<storage::Table>> tables;
    };
    std::map<std::string, DbTables> databases_;
    mutable std::mutex mu_;  // M6: 保护 Catalog 所有多线程访问

    /// 取当前库的 tables map
    DbTables* current_db();
    const DbTables* current_db() const;

    /// 按 db 名拿 DbTables(没找到返回 nullptr)
    DbTables* get_db(const std::string& name);
    const DbTables* get_db(const std::string& name) const;

    /// lowercased name
    static std::string lower_name(const std::string& s);
};

} // namespace sqltiny::catalog
