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

/// 保存点 —— 记录事务内写操作与 WAL 缓冲的位置
struct Savepoint {
    std::string name;
    size_t action_index = 0;
    size_t wal_count    = 0;
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

    /// 本会话唯一事务 id —— Table 用它区分"自己的未提交写"和"别人的未提交写"
    uint64_t txnId() const { return txn_id_; }

    // ============================================================
    // 隔离级别 (参考 HSQLDB 2.7.4 Session.java / SessionInterface.java)
    //   TX_READ_UNCOMMITTED → 提升为 TX_READ_COMMITTED (下一事务级还附加只读)
    //   TX_REPEATABLE_READ  → 提升为 TX_SERIALIZABLE (MVCC 下即快照隔离)
    //   所有级别都不会读到未提交数据
    // ============================================================
    ast::IsolationLevel isolationLevelDefault = ast::IsolationLevel::ReadCommitted;
    ast::IsolationLevel isolationLevel        = ast::IsolationLevel::ReadCommitted;
    bool                isReadOnlyIsolation  = false;

    /// 设置隔离级别。sessionDefault=true 影响会话默认值(SET SESSION ...),
    /// false 只影响下一个事务(SET TRANSACTION ...)
    void setIsolation(ast::IsolationLevel level, bool sessionDefault);

    /// 对外可见的隔离级别名(MySQL 风格,返回的是"提升后"的级别)
    std::string isolationName() const;

    /// 本语句使用的读 SCN:
    ///  READ COMMITTED      → 语句级最新已提交 SCN
    ///  REPEATABLE READ / SERIALIZABLE → 事务开始时的 SCN (快照隔离)
    uint64_t readSCN() const;

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

    /// 回滚到保存点(不结束事务)
    bool rollbackToSavepoint(const std::string& name);

    // 事务字段 (TransactionManager / Executor 会访问)
    bool                autocommit     = true;       // MySQL 默认
    uint64_t            transactionSCN = 0;          // begin 时分配, 0 = 不在事务中
    uint64_t            actionSCN       = 0;          // 最近一次写操作的 SCN
    std::vector<RowAction> row_actions_;              // 当前事务内所有写操作
    std::vector<Savepoint> savepoints_;
    std::vector<persist::WalRecord> wal_buffer_;      // 未提交事务的行 DML 日志

    // RowAction 记录 helper — DML 在 Session 层调用
    void addRowAction(RowAction ra) { row_actions_.push_back(std::move(ra)); }

    // 行锁 helper — DML 在 Session 层调用
    bool tryLockRowWrite(storage::Table& tbl, int64_t rowId);
    void unlockRowWrite(storage::Table& tbl, int64_t rowId);

private:
    executor::ResultSet applySet(const ast::Statement& stmt);
    executor::ResultSet applySavepoint(const ast::Statement& stmt);
    void finishTransaction(bool committed);

    executor::Executor exec_{ Database::instance().catalog() };
    uint64_t txn_id_ = 0;
};

} // namespace sqltiny::core
