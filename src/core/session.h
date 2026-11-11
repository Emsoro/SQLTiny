#pragma once

#include "catalog/catalog.h"
#include "executor/executor.h"
#include "parser/parser.h"
#include "storage/wal_manager.h"
#include "transaction.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>

namespace sqltiny::core {

/// 单例数据库(进程内)
class Database {
public:
    static Database& instance();

    catalog::Catalog& catalog() { return catalog_; }
    WalManager& wal() { return wal_; }

    /// 启动时调用:打开 WAL 并 replay 到 catalog
    /// data_dir 为空则持久化禁用
    bool open(const std::string& data_dir);

    /// 优雅关闭
    void shutdown();

private:
    Database() = default;
    catalog::Catalog catalog_;
    WalManager wal_;
    std::string wal_path_;  // 给 session.cpp::open 用
};

/// 会话 —— 一条 SQL 入口
class Session {
public:
    Session();
    ~Session();

    /// 执行一条 SQL,返回 ResultSet
    executor::ResultSet execute(const std::string& sql);

    /// 当前数据库(USE 用)
    std::string current_database = "default";

    /// 用户名(占位)
    std::string user = "root";

    // ============================================================
    // M6 事务相关
    // ============================================================

    /// 是否在事务中 (BEGIN 显式开启, 或 autocommit=false 时的隐式事务)
    bool isInTransaction() const { return transactionSCN != 0; }

    /// 显式开启事务 (autocommit=false 时也调用 begin)
    void beginTransaction();

    /// 提交事务 — 成功返回 true; 冲突返回 false (40001)
    bool commitTransaction();

    /// 回滚事务
    void rollbackTransaction();

    // 事务字段 (TransactionManager / Executor 会访问)
    bool                autocommit     = true;       // MySQL 默认
    uint64_t            transactionSCN = 0;          // begin 时分配, 0 = 不在事务中
    uint64_t            actionSCN       = 0;          // 最近一次写操作的 SCN
    std::vector<RowAction> row_actions_;              // 当前事务内所有写操作

    // RowAction 记录 helper — DML 在 Session 层调用
    void addRowAction(RowAction ra) { row_actions_.push_back(std::move(ra)); }

    // 行锁 helper — DML 在 Session 层调用
    bool tryLockRowWrite(storage::Table& tbl, int64_t rowId);
    void unlockRowWrite(storage::Table& tbl, int64_t rowId);

private:
    executor::Executor exec_{ Database::instance().catalog() };
};

} // namespace sqltiny::core
