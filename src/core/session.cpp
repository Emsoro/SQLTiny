#include "core/session.h"
#include "core/transaction.h"
#include <fmt/format.h>
#include <cstdio>
#include <string>
#include <mutex>

namespace sqltiny::core {

// 全局执行锁: server 多线程时串行化 SQL 执行
// M6 P0 完成后: 这把锁将退化为 fallback, 事务边界用 TransactionManager.writeLock_
static std::mutex g_execute_mutex;

Database& Database::instance() {
    static Database db;
    return db;
}

bool Database::open(const std::string& data_dir) {
    if (data_dir.empty()) {
        fmt::print("[SQLTiny] data-dir not set — running in memory-only mode (no persistence)\n");
        wal_path_.clear();
        return true;
    }
#ifdef _WIN32
    std::string sys_cmd = "mkdir \"" + data_dir + "\" 2>NUL";
    (void)std::system(sys_cmd.c_str());
#else
    std::string sys_cmd = "mkdir -p \"" + data_dir + "\"";
    (void)std::system(sys_cmd.c_str());
#endif
    wal_path_ = data_dir + "/sql.wal";
    return wal_.open(wal_path_, catalog_);
}

void Database::shutdown() {
    if (wal_.enabled()) {
        fmt::print("[SQLTiny] shutting down — checkpointing...\n");
        wal_.do_checkpoint(catalog_);
    }
    wal_.close();
}

Session::Session() {
    // M6: 把自己 bind 给 Executor — DML 时拿行锁 + 记录 RowAction
    exec_.bind_session(this);
}

Session::~Session() {
    // 还在事务中(比如 client 断连) — 自动 rollback
    if (isInTransaction()) {
        rollbackTransaction();
    }
}

// ============================================================
// M6 事务方法
// ============================================================
void Session::beginTransaction() {
    if (isInTransaction()) {
        // 已经在事务中 — 先 rollback 再重新 begin (MySQL 行为)
        rollbackTransaction();
    }
    transactionSCN = TransactionManager::instance().begin_transaction(this);
    autocommit = false;
}

bool Session::commitTransaction() {
    if (!isInTransaction()) return true;  // 不在事务中 — 空操作
    bool ok = TransactionManager::instance().commit_transaction(this);
    row_actions_.clear();
    transactionSCN = 0;
    return ok;
}

void Session::rollbackTransaction() {
    if (!isInTransaction()) return;
    TransactionManager::instance().rollback_transaction(this);
    row_actions_.clear();
    transactionSCN = 0;
}

bool Session::tryLockRowWrite(storage::Table& tbl, int64_t rowId) {
    return TransactionManager::instance().try_lock_row_write(tbl, rowId, this);
}

void Session::unlockRowWrite(storage::Table& tbl, int64_t rowId) {
    TransactionManager::instance().unlock_row_write(tbl, rowId, this);
}

// ============================================================
// Session::execute — M6 改 autocommit + 事务边界
// ============================================================
executor::ResultSet Session::execute(const std::string& sql) {
    std::lock_guard<std::mutex> lock(g_execute_mutex);

    executor::ResultSet last;
    try {
        parser::Parser p(sql);
        auto stmts = p.parse_all();

        // autocommit 模式 + 不在显式事务中 => 每条 SQL 隐式 begin
        // autocommit=false 或显式 BEGIN 后 => 保持事务边界
        bool implicitBegin = autocommit && !isInTransaction();
        if (implicitBegin) {
            beginTransaction();  // 隐式开启
        }

        for (auto& s : stmts) {
            // —— M6: BEGIN / COMMIT / ROLLBACK 拦截 ——
            if (s->kind == ast::StmtKind::Begin) {
                beginTransaction();
                executor::ResultSet ok; ok.ok = true; ok.affected_rows = 0;
                // BEGIN 显式开启后 autocommit 仍保持 false (MySQL 语义)
                // 但 implicitBegin 已经 true => 不再隐式 commit
                implicitBegin = false;
                return ok;
            }
            if (s->kind == ast::StmtKind::Commit) {
                bool committed = commitTransaction();
                executor::ResultSet ok; ok.ok = committed; ok.affected_rows = 0;
                if (committed) {
                    // autocommit 保持用户之前设置的值(BEGIN 显式设 false,这里不改回)
                    implicitBegin = autocommit;  // 下一条看 autocommit
                }
                return ok;
            }
            if (s->kind == ast::StmtKind::Rollback) {
                rollbackTransaction();
                executor::ResultSet ok; ok.ok = true; ok.affected_rows = 0;
                implicitBegin = autocommit;
                return ok;
            }

            // —— 执行语句 ——
            last = exec_.execute(*s);
            if (!last.ok) {
                // 出错: 隐式事务回滚,显式事务保持(MySQL 行为更复杂,先简化)
                if (implicitBegin) rollbackTransaction();
                return last;
            }

            // —— USE: 同步 Catalog + WAL ——
            if (s->kind == ast::StmtKind::Use) {
                auto& name = s->use_->database_name;
                if (!exec_.catalog_ref().switch_database(name)) {
                    executor::ResultSet u;
                    u.ok = false; u.mysql_errno = 1049; u.sql_state = "42000";
                    u.error_message = fmt::format("Unknown database '{}'", name);
                    if (implicitBegin) rollbackTransaction();
                    return u;
                }
                current_database = name;
                Database::instance().wal().append_use_database(name);
                executor::ResultSet u;
                u.ok = true; u.affected_rows = 0;
                last = u;
            }
        }

        // autocommit 模式: 最后隐式 commit
        if (implicitBegin) {
            bool committed = commitTransaction();
            if (!committed) {
                executor::ResultSet fail; fail.ok = false;
                fail.mysql_errno = 1213;     // Deadlock found when trying to get lock
                fail.sql_state = "40001";
                fail.error_message = "Serialization failure — row lock conflict";
                return fail;
            }
        }
        return last;
    } catch (const parser::ParseError& e) {
        executor::ResultSet r;
        r.ok = false;
        r.mysql_errno = 1064;
        r.sql_state = "42000";
        r.error_message = fmt::format("You have an error in your SQL syntax: {}", e.what());
        // 出错回滚
        if (isInTransaction()) rollbackTransaction();
        return r;
    } catch (const std::exception& e) {
        executor::ResultSet r;
        r.ok = false;
        r.mysql_errno = 1105;
        r.sql_state = "HY000";
        r.error_message = e.what();
        if (isInTransaction()) rollbackTransaction();
        return r;
    }
}

} // namespace sqltiny::core
