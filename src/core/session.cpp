#include "core/session.h"
#include "core/transaction.h"
#include <fmt/format.h>
#include <cstdio>
#include <string>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cctype>

namespace sqltiny::core {

static std::atomic<uint64_t> g_next_session_id{1};

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

static bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

static executor::ResultSet make_ok(bool ok = true) {
    executor::ResultSet r;
    r.ok = ok;
    return r;
}

static executor::ResultSet make_err(uint16_t eno, std::string state, std::string msg) {
    executor::ResultSet r;
    r.ok = false;
    r.mysql_errno = eno;
    r.sql_state = std::move(state);
    r.error_message = std::move(msg);
    return r;
}

static bool is_write_stmt(ast::StmtKind k) {
    using ast::StmtKind;
    return k == StmtKind::Insert || k == StmtKind::Update || k == StmtKind::Delete ||
           k == StmtKind::CreateTable || k == StmtKind::DropTable ||
           k == StmtKind::CreateDatabase || k == StmtKind::DropDatabase;
}

Session::Session() : txn_id_(g_next_session_id.fetch_add(1)) {
    exec_.bind_session(this);
}

Session::~Session() {
    // 还在事务中(比如 client 断连) — 自动 rollback
    if (isInTransaction()) rollbackTransaction();
    if (WalManager::thread_txn_buffer() == &wal_buffer_)
        WalManager::set_thread_txn_buffer(nullptr);
}

/// 保证"哪条 Session 在执行,哪条 Session 的 WAL 缓冲生效"。
/// 进程内多个 Session 交错调用时,不能只靠 thread_local 记住上一个事务。
struct WalBufferScope {
    std::vector<persist::WalRecord>* saved;
    explicit WalBufferScope(Session* s)
        : saved(WalManager::thread_txn_buffer()) {
        WalManager::set_thread_txn_buffer(s->isInTransaction() ? &s->wal_buffer_ : nullptr);
    }
    ~WalBufferScope() { WalManager::set_thread_txn_buffer(saved); }
};

// ============================================================
// 隔离级别 (HSQLDB 2.7.4 Session.setIsolation / setIsolationDefault)
// ============================================================
void Session::setIsolation(ast::IsolationLevel level, bool sessionDefault) {
    bool read_only = false;
    if (level == ast::IsolationLevel::ReadUncommitted) {
        level = ast::IsolationLevel::ReadCommitted;   // 提升:MVCC 下 READ UNCOMMITTED → READ COMMITTED
        read_only = !sessionDefault;                   // 下一事务级还附加"只读"
    }
    if (level == ast::IsolationLevel::RepeatableRead) {
        level = ast::IsolationLevel::Serializable;     // 提升:REPEATABLE READ → SERIALIZABLE
    }
    if (sessionDefault) {
        isolationLevelDefault = level;
        isolationLevel = level;
        isReadOnlyIsolation = false;
    } else {
        isolationLevel = level;
        isReadOnlyIsolation = read_only;
    }
}

std::string Session::isolationName() const {
    switch (isolationLevel) {
        case ast::IsolationLevel::ReadUncommitted: return "READ-UNCOMMITTED";
        case ast::IsolationLevel::ReadCommitted:   return "READ-COMMITTED";
        case ast::IsolationLevel::RepeatableRead:  return "REPEATABLE-READ";
        case ast::IsolationLevel::Serializable:    return "SERIALIZABLE";
    }
    return "READ-COMMITTED";
}

uint64_t Session::readSCN() const {
    uint64_t latest = Database::instance().wal().current_scn();
    if (!isInTransaction()) return latest;
    if (isolationLevel == ast::IsolationLevel::Serializable ||
        isolationLevel == ast::IsolationLevel::RepeatableRead) {
        return transactionSCN;    // 事务级快照 (HSQLDB RowAction.canRead)
    }
    return latest;               // READ COMMITTED:语句级快照
}

// ============================================================
// 事务方法
// ============================================================
void Session::beginTransaction() {
    if (isInTransaction()) commitTransaction();   // MySQL:新 BEGIN 隐式提交当前事务
    row_actions_.clear();
    savepoints_.clear();
    wal_buffer_.clear();
    transactionSCN = TransactionManager::instance().begin_transaction(this);
    actionSCN = transactionSCN;
    WalManager::set_thread_txn_buffer(&wal_buffer_);
}

void Session::finishTransaction(bool committed) {
    if (committed) Database::instance().wal().flush_txn_buffer(wal_buffer_);
    else           wal_buffer_.clear();
    if (WalManager::thread_txn_buffer() == &wal_buffer_)
        WalManager::set_thread_txn_buffer(nullptr);
    row_actions_.clear();
    savepoints_.clear();
    transactionSCN = 0;
    actionSCN = 0;
    // HSQLDB Session.endTransaction():恢复会话默认隔离级别(SET TRANSACTION 只对下一事务生效)
    isolationLevel = isolationLevelDefault;
    isReadOnlyIsolation = false;
}

bool Session::commitTransaction() {
    if (!isInTransaction()) return true;
    if (!TransactionManager::instance().commit_transaction(this)) {
        // 40001 serialization failure — 整个事务回滚
        rollbackTransaction();
        return false;
    }
    finishTransaction(true);
    return true;
}

void Session::rollbackTransaction() {
    if (!isInTransaction()) return;
    TransactionManager::instance().rollback_transaction(this);
    finishTransaction(false);
}

bool Session::rollbackToSavepoint(const std::string& name) {
    size_t pos = savepoints_.size();
    for (size_t i = savepoints_.size(); i-- > 0;) {
        if (ieq(savepoints_[i].name, name)) { pos = i; break; }
    }
    if (pos == savepoints_.size()) return false;
    TransactionManager::instance().rollback_to_savepoint(this, savepoints_[pos].action_index);
    wal_buffer_.resize(savepoints_[pos].wal_count);
    savepoints_.erase(savepoints_.begin() + static_cast<ptrdiff_t>(pos) + 1,
                      savepoints_.end());
    return true;
}

bool Session::tryLockRowWrite(storage::Table& tbl, int64_t rowId) {
    // 快照隔离:若目标行在本事务开始之后被其它事务改过,直接 40001 中止
    // (参考 HSQLDB TransactionManagerMVCC 对 RR/SERIALIZABLE 的冲突处理)
    if (isInTransaction() &&
        (isolationLevel == ast::IsolationLevel::Serializable ||
         isolationLevel == ast::IsolationLevel::RepeatableRead)) {
        auto vi = tbl.get_version(rowId);
        if (vi.pendingTxn != txnId() &&
            ((vi.createSCN != 0 && vi.createSCN > transactionSCN) ||
             (vi.deleteSCN != 0 && vi.deleteSCN > transactionSCN))) {
            return false;
        }
    }
    return TransactionManager::instance().try_lock_row_write(tbl, rowId, this);
}

void Session::unlockRowWrite(storage::Table& tbl, int64_t rowId) {
    TransactionManager::instance().unlock_row_write(tbl, rowId, this);
}

// ============================================================
// SET / SAVEPOINT 语句
// ============================================================
executor::ResultSet Session::applySet(const ast::Statement& stmt) {
    auto& s = *stmt.set_;
    switch (s.kind) {
        case ast::SetStmt::Kind::Ignored:
            return make_ok();
        case ast::SetStmt::Kind::AutoCommit:
            if (s.autocommit_value) {
                if (isInTransaction()) commitTransaction();   // MySQL:SET AUTOCOMMIT=1 提交当前事务
                autocommit = true;
            } else {
                autocommit = false;
                if (!isInTransaction()) beginTransaction();   // MySQL:SET AUTOCOMMIT=0 开启事务
            }
            return make_ok();
        case ast::SetStmt::Kind::IsolationLevel:
            if (isInTransaction() && !row_actions_.empty()) {
                return make_err(25001, "25001",
                    "cannot set transaction isolation level in mid transaction");
            }
            setIsolation(static_cast<ast::IsolationLevel>(s.isolation_level),
                         s.session_default);
            return make_ok();
    }
    return make_ok();
}

executor::ResultSet Session::applySavepoint(const ast::Statement& stmt) {
    switch (stmt.kind) {
        case ast::StmtKind::Savepoint:
            if (isInTransaction()) {
                savepoints_.push_back(Savepoint{stmt.savepoint_name,
                                                row_actions_.size(), wal_buffer_.size()});
            }
            return make_ok();
        case ast::StmtKind::ReleaseSavepoint: {
            auto it = std::find_if(savepoints_.begin(), savepoints_.end(),
                [&](const Savepoint& sp) { return ieq(sp.name, stmt.savepoint_name); });
            if (it == savepoints_.end())
                return make_err(1305, "42000",
                    fmt::format("SAVEPOINT {} does not exist", stmt.savepoint_name));
            savepoints_.erase(it);
            return make_ok();
        }
        case ast::StmtKind::RollbackToSavepoint:
            if (!isInTransaction() || !rollbackToSavepoint(stmt.savepoint_name))
                return make_err(1305, "42000",
                    fmt::format("SAVEPOINT {} does not exist", stmt.savepoint_name));
            return make_ok();
        default:
            return make_ok();
    }
}

// ============================================================
// Session::execute — 事务边界 / 隔离级别 / autocommit
// ============================================================
executor::ResultSet Session::execute(const std::string& sql) {
    executor::ResultSet last;
    bool implicit_active = false;
    WalBufferScope wal_scope(this);
    try {
        parser::Parser p(sql);
        auto stmts = p.parse_all();

        for (auto& st : stmts) {
            auto kind = st->kind;

            // —— 会话控制语句:在隐式事务边界之外处理 ——
            if (kind == ast::StmtKind::Set) {
                last = applySet(*st);
                if (!last.ok) return last;
                continue;
            }
            if (kind == ast::StmtKind::Begin || kind == ast::StmtKind::StartTransaction) {
                if (implicit_active) { commitTransaction(); implicit_active = false; }
                // START TRANSACTION ISOLATION LEVEL ... 只作用于这个事务
                if (kind == ast::StmtKind::StartTransaction && st->set_ &&
                    st->set_->kind == ast::SetStmt::Kind::IsolationLevel) {
                    setIsolation(static_cast<ast::IsolationLevel>(st->set_->isolation_level), false);
                }
                beginTransaction();
                last = make_ok();
                continue;
            }
            if (kind == ast::StmtKind::Commit) {
                bool ok = commitTransaction();
                implicit_active = false;
                last = ok ? make_ok() : make_err(1213, "40001",
                    "Serialization failure — snapshot write conflict");
                if (!last.ok) return last;
                continue;
            }
            if (kind == ast::StmtKind::Rollback) {
                rollbackTransaction();
                implicit_active = false;
                last = make_ok();
                continue;
            }
            if (kind == ast::StmtKind::Savepoint ||
                kind == ast::StmtKind::RollbackToSavepoint ||
                kind == ast::StmtKind::ReleaseSavepoint) {
                last = applySavepoint(*st);
                if (!last.ok) return last;
                continue;
            }

            // —— 普通语句:autocommit 时每条语句隐式 begin+commit;
            //    autocommit=OFF 时隐式 begin 但一直保持到显式 COMMIT/ROLLBACK ——
            bool need_txn = !isInTransaction();
            bool implicit_commit = need_txn && autocommit;
            if (need_txn) beginTransaction();
            if (implicit_commit) implicit_active = true;

            // READ UNCOMMITTED 提升为 READ COMMITTED + 只读 (HSQLDB)
            if (isReadOnlyIsolation && is_write_stmt(kind)) {
                if (implicit_active) { rollbackTransaction(); implicit_active = false; }
                return make_err(25006, "25006", "transaction is read-only at this isolation level");
            }

            last = exec_.execute(*st);
            if (!last.ok) {
                if (implicit_active) { rollbackTransaction(); implicit_active = false; }
                return last;
            }

            // —— USE: 只改本会话的 current_database (M6 P1 #7: per-session db) ——
            if (kind == ast::StmtKind::Use) {
                auto& name = st->use_->database_name;
                if (!exec_.catalog_ref().database_exists(name)) {
                    if (implicit_active) { rollbackTransaction(); implicit_active = false; }
                    return make_err(1049, "42000", fmt::format("Unknown database '{}'", name));
                }
                current_database = name;
                Database::instance().wal().append_use_database(name);
                last = make_ok();
            }

            if (implicit_commit) {
                bool ok = commitTransaction();
                implicit_active = false;
                if (!ok) return make_err(1213, "40001",
                    "Serialization failure — snapshot write conflict");
            }
        }

        if (implicit_active) {
            bool ok = commitTransaction();
            if (!ok) return make_err(1213, "40001",
                "Serialization failure — snapshot write conflict");
        }
        return last;
    } catch (const parser::ParseError& e) {
        if (implicit_active) rollbackTransaction();
        return make_err(1064, "42000",
            fmt::format("You have an error in your SQL syntax: {}", e.what()));
    } catch (const std::exception& e) {
        if (implicit_active) rollbackTransaction();
        return make_err(1105, "HY000", e.what());
    }
}

} // namespace sqltiny::core
