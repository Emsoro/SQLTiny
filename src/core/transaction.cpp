#include "core/transaction.h"
#include "core/session.h"
#include "storage/wal_manager.h"
#include "storage/table.h"
#include <mutex>
#include <algorithm>

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
    uint64_t scn = next_scn();    // 用内部 SCN, 与 WAL 的 next_scn 独立 (可以先保持 WAL 也能拿到)
    // 实际 SQLTiny 第一版: 让 TransactionManager 分配 SCN, WAL 从同源头取
    (void)s;
    // 先保持 WAL 作为 SCN 源头, 避免两套 SCN 计数器
    uint64_t wal_scn = sqltiny::core::Database::instance().wal().next_scn();
    live_tx_scns_.push_back(wal_scn);
    return wal_scn;
}

// ============================================================
// commit_transaction
// ============================================================
bool TransactionManager::commit_transaction(Session* s) {
    std::lock_guard<std::mutex> g(writeLock_);

    // 1. 冲突检测: 检查已提交事务里有没有同 rowId 的写操作冲突
    //    由于 SQLTiny 当前没有持久化 RowAction 队列, 第一版简化 —
    //    RowAction 的 rowId 冲突检测只查 committed_queue_
    //    实际上 writeLock_ 串行化了 begin/commit, 所以两个事务不会同时 commit
    //    行锁冲突已经在 try_lock_row_write 里解决过了
    (void)s;

    // 2. 分配 commitSCN
    uint64_t commitSCN = sqltiny::core::Database::instance().wal().next_scn();

    // 3. 把 session 的 row_actions_ 打上 commitSCN, 应用到 Table 版本链
    //    注意: session 在 header 里声明了但 transaction.h 只前向声明,
    //    我们用友元或直接在实现里 include session.h
    apply_commit_to_tables(s->row_actions_, commitSCN);

    // 4. WAL 追加已有逻辑(Executor 已经在写 WAL 了, commit 时不需要额外 WAL record
    //    第一版简化 — commit 不写额外 WAL record)

    // 5. 放到 committedQueue, 等过期 merge
    if (!s->row_actions_.empty()) {
        committed_queue_.push_back(s->row_actions_);
    }

    // 6. 释放所有 session 的行锁
    //    (在 writeLock_ 外释放, 避免死锁)

    // 7. 从 live queue 移除 + merge 过期
    end_transaction(s);

    // 8. 释放行锁 — 挪到 writeLock_ 外做
    // 这里先释放, 简化
    release_all_locks_for_session(s);

    return true;
}

// ============================================================
// rollback_transaction
// ============================================================
void TransactionManager::rollback_transaction(Session* s) {
    std::lock_guard<std::mutex> g(writeLock_);

    // 回滚版本链:
    apply_rollback_to_tables(s->row_actions_);

    // rollback 不进 committed_queue (它没 commitSCN)
    end_transaction(s);

    // 释放锁
    release_all_locks_for_session(s);
}

// ============================================================
// apply_commit_to_tables / apply_rollback_to_tables
// ============================================================
void TransactionManager::apply_commit_to_tables(const std::vector<RowAction>& actions, uint64_t commitSCN) {
    auto& db = sqltiny::core::Database::instance();
    for (auto& a : actions) {
        storage::Table* tbl = db.catalog().get_table_in_db(a.dbName, a.tableName);
        if (!tbl) continue;

        if (a.type == RowAction::DELETE || a.type == RowAction::UPDATE_DELETE) {
            // 标记该行 deleteSCN = commitSCN
            // 先找 row_id 对应的版本信息
            auto vi = tbl->get_version(a.rowId);
            vi.deleteSCN = commitSCN;
            tbl->set_version(a.rowId, vi);
        }
        if (a.type == RowAction::INSERT || a.type == RowAction::UPDATE_INSERT) {
            // 标记该行 createSCN = commitSCN
            auto vi = tbl->get_version(a.rowId);
            vi.createSCN = commitSCN;
            tbl->set_version(a.rowId, vi);
        }
    }
}

void TransactionManager::apply_rollback_to_tables(const std::vector<RowAction>& actions) {
    auto& db = sqltiny::core::Database::instance();
    for (auto& a : actions) {
        storage::Table* tbl = db.catalog().get_table_in_db(a.dbName, a.tableName);
        if (!tbl) continue;

        if (a.type == RowAction::INSERT || a.type == RowAction::UPDATE_INSERT) {
            // 未提交的 INSERT: 从 row_map_ 物理删除
            // Table 没有直接按 row_id 删除的接口 — 我们用 delete_by_pk + 设 deleteSCN=actionSCN
            // 但更简单: 直接调 delete_by_pk(传入 pk 列, deleteSCN=0 表示物理删)
            // 第一版: delete_by_pk 里如果 deleteSCN=0 则直接 erase
            // 这里我们把 commitSCN 设成 actionSCN - 1 使得这行永远不可见
            tbl->delete_by_pk(a.row, /*deleteSCN=*/0);  // 物理删(对于 rollback 的 INSERT)
        }
        if (a.type == RowAction::DELETE || a.type == RowAction::UPDATE_DELETE) {
            // 未提交的 DELETE: 恢复可见性 (deleteSCN 设回 0)
            auto vi = tbl->get_version(a.rowId);
            vi.deleteSCN = 0;   // 恢复可见
            tbl->set_version(a.rowId, vi);
        }
    }
}

// ============================================================
// end_transaction + merge_expired
// ============================================================
void TransactionManager::end_transaction(Session* s) {
    // 从 live 队列移除对应 SCN
    // 注意: 我们不知道这个 session 的 transactionSCN 是多少
    // 简化: live_tx_scns_ 是每个 session begin 时 push 的, 我们需要能找到它
    // 解决方案: 在 Session 里存 transactionSCN, 然后这里查找
    auto tx_scn = s->transactionSCN;
    auto it = std::find(live_tx_scns_.begin(), live_tx_scns_.end(), tx_scn);
    if (it != live_tx_scns_.end()) live_tx_scns_.erase(it);

    merge_expired_transactions();
}

void TransactionManager::merge_expired_transactions() {
    // 所有 commitSCN < 当前最小 live_tx_scn 的 committed actions 都可以 finalize 了
    // 因为没有还在等那个 SCN 快照的事务了
    while (!committed_queue_.empty() && !live_tx_scns_.empty()) {
        uint64_t min_live = live_tx_scns_.front();
        uint64_t commit_scn = committed_queue_.front()[0].commitSCN;
        if (commit_scn < min_live) {
            // finalize 这些 actions — 实际上 commit 时已经 apply 过了
            // 这里只是从 committed_queue_ 移除
            committed_queue_.pop_front();
        } else {
            break;
        }
    }
}

} // namespace sqltiny::core
