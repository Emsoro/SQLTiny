#include "core/transaction.h"
#include "core/session.h"
#include "storage/wal_manager.h"
#include "storage/table.h"
#include <mutex>
#include <algorithm>
#include <limits>
#include <utility>

namespace sqltiny::core {

// ============================================================
// 单例
// ============================================================
TransactionManager& TransactionManager::instance() {
    static TransactionManager txm;
    return txm;
}

// ============================================================
// 行锁表管理
// ============================================================
TransactionManager::TableLockEntry* TransactionManager::get_or_create_lock_entry(storage::Table* tbl) {
    std::lock_guard<std::mutex> g(locks_map_mu_);
    auto it = table_locks_.find(tbl);
    if (it != table_locks_.end()) return it->second.get();
    auto entry = std::make_unique<TableLockEntry>();
    TableLockEntry* raw = entry.get();
    table_locks_[tbl] = std::move(entry);
    return raw;
}

// ============================================================
// try_lock_row_write / unlock_row_write
// ============================================================
bool TransactionManager::try_lock_row_write(storage::Table& tbl, int64_t row_id, Session* s) {
    auto* entry = get_or_create_lock_entry(&tbl);
    std::lock_guard<std::mutex> g(entry->mu);
    auto& lk = entry->locks[row_id];
    if (lk.state == RowLock::FREE) {
        lk.state = RowLock::EXCLUSIVE_WRITE;
        lk.owner = s;
        return true;
    }
    if (lk.is_exclusive_owned_by(s)) return true;   // 重入
    return false;   // 冲突,立即 abort
}

void TransactionManager::unlock_row_write(storage::Table& tbl, int64_t row_id, Session* s) {
    auto* entry = get_or_create_lock_entry(&tbl);
    std::lock_guard<std::mutex> g(entry->mu);
    auto it = entry->locks.find(row_id);
    if (it == entry->locks.end()) return;
    auto& lk = it->second;
    if (lk.is_exclusive_owned_by(s)) {
        lk.state = RowLock::FREE;
        lk.owner = nullptr;
    }
}

void TransactionManager::release_all_locks_for_session(Session* s) {
    std::lock_guard<std::mutex> g(locks_map_mu_);
    for (auto& [tbl, entry] : table_locks_) {
        std::lock_guard<std::mutex> eg(entry->mu);
        for (auto& [rid, lk] : entry->locks) {
            if (lk.state == RowLock::EXCLUSIVE_WRITE && lk.owner == s) {
                lk.state = RowLock::FREE;
                lk.owner = nullptr;
            }
        }
    }
}

// ============================================================
// begin_transaction
// ============================================================
uint64_t TransactionManager::begin_transaction(Session* s) {
    std::lock_guard<std::mutex> g(writeLock_);
    (void)s;
    // SCN 只有 WAL 一套计数器,避免两套 SCN 造成可见性错乱
    uint64_t scn = Database::instance().wal().next_scn();
    live_tx_scns_.push_back(scn);
    return scn;
}

// ============================================================
// 快照隔离的提交时冲突检测
// ============================================================
bool TransactionManager::has_snapshot_conflict(Session* s) const {
    // HSQLDB 2.7.4:REPEATABLE READ / SERIALIZABLE 在 MVCC 下等价 (SNAPSHOT ISOLATION),
    // 冲突时不等待重做,直接 X_40501 中止事务。READ COMMITTED 不做这个检查。
    if (s->isolationLevel != ast::IsolationLevel::RepeatableRead &&
        s->isolationLevel != ast::IsolationLevel::Serializable) return false;
    for (auto& ctxn : committed_queue_) {
        if (ctxn.owner == s || ctxn.commitSCN <= s->transactionSCN) continue;
        for (auto& other : ctxn.actions)
            for (auto& mine : s->row_actions_)
                // rowId 是表内局部编号,必须同时比较库/表,避免跨表误判
                if (other.rowId == mine.rowId &&
                    other.dbName == mine.dbName &&
                    other.tableName == mine.tableName) return true;
    }
    return false;
}

// ============================================================
// commit_transaction
// ============================================================
bool TransactionManager::commit_transaction(Session* s) {
    std::lock_guard<std::mutex> g(writeLock_);

    // 1. 冲突检测(返回 false 由 Session 转成 40001 并整体回滚)
    if (has_snapshot_conflict(s)) return false;

    // 2. 分配 commitSCN —— 行版本只有到这一步才对其它会话可见
    uint64_t commitSCN = Database::instance().wal().next_scn();

    // 3. finalize 行版本 + 记录已提交事务(供后续快照冲突检测)
    for (auto& a : s->row_actions_) a.commitSCN = commitSCN;
    apply_commit_to_tables(s->row_actions_, commitSCN);
    if (!s->row_actions_.empty()) {
        committed_queue_.push_back(CommittedTxn{commitSCN, s, s->row_actions_});
    }

    // 4. 释放行锁 + 从 live 队列移除
    release_all_locks_for_session(s);
    end_transaction(s);
    return true;
}

// ============================================================
// rollback_transaction / rollback_to_savepoint
// ============================================================
void TransactionManager::rollback_transaction(Session* s) {
    std::lock_guard<std::mutex> g(writeLock_);
    apply_rollback_to_tables(s->row_actions_);
    release_all_locks_for_session(s);
    end_transaction(s);
}

void TransactionManager::rollback_to_savepoint(Session* s, size_t action_index) {
    std::lock_guard<std::mutex> g(writeLock_);
    if (action_index >= s->row_actions_.size()) return;
    std::vector<RowAction> tail(s->row_actions_.begin() + action_index,
                                s->row_actions_.end());
    apply_rollback_to_tables(tail);   // 逆序撤销
    s->row_actions_.erase(s->row_actions_.begin() + action_index,
                          s->row_actions_.end());
    // 释放被撤销行上的锁(简化:其它仍存活的写锁随后由本事务继续持有)
    for (auto& a : tail) {
        auto* tbl = Database::instance().catalog().get_table_in_db(a.dbName, a.tableName);
        if (tbl) unlock_row_write(*tbl, a.rowId, s);
    }
}

// ============================================================
// apply_commit_to_tables / apply_rollback_to_tables
// ============================================================
void TransactionManager::apply_commit_to_tables(const std::vector<RowAction>& actions,
                                               uint64_t commitSCN) {
    auto& db = Database::instance();
    for (auto& a : actions) {
        storage::Table* tbl = db.catalog().get_table_in_db(a.dbName, a.tableName);
        if (!tbl) continue;
        bool is_delete = (a.type == RowAction::DELETE || a.type == RowAction::UPDATE_DELETE);
        tbl->finalize_commit(a.rowId, commitSCN, is_delete);
    }
}

void TransactionManager::apply_rollback_to_tables(const std::vector<RowAction>& actions) {
    auto& db = Database::instance();
    // 必须逆序:同一行可能先 INSERT 再 DELETE,或 UPDATE 产生一删一插
    for (auto it = actions.rbegin(); it != actions.rend(); ++it) {
        auto& a = *it;
        storage::Table* tbl = db.catalog().get_table_in_db(a.dbName, a.tableName);
        if (!tbl) continue;
        if (a.type == RowAction::INSERT || a.type == RowAction::UPDATE_INSERT) {
            tbl->erase_row(a.rowId);        // 未提交插入 → 物理移除(含主键索引)
        } else {
            tbl->restore_row(a.rowId);      // 未提交删除 → 恢复可见 + 主键索引
        }
    }
}

// ============================================================
// end_transaction + merge_expired
// ============================================================
void TransactionManager::end_transaction(Session* s) {
    auto tx_scn = s->transactionSCN;
    auto it = std::find(live_tx_scns_.begin(), live_tx_scns_.end(), tx_scn);
    if (it != live_tx_scns_.end()) live_tx_scns_.erase(it);
    merge_expired_transactions();
}

void TransactionManager::merge_expired_transactions() {
    // 没有活跃事务时,所有已提交 action 都不再需要为快照冲突检测保留
    uint64_t min_live = live_tx_scns_.empty()
                      ? std::numeric_limits<uint64_t>::max()
                      : live_tx_scns_.front();
    while (!committed_queue_.empty() && committed_queue_.front().commitSCN < min_live) {
        committed_queue_.pop_front();
    }
}

} // namespace sqltiny::core
