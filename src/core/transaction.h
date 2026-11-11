#pragma once

// ============================================================
// M6 P0 · 行锁 + 事务管理器
// 参考: HSQLDB TransactionManagerMVCC / RowAction / TransactionManagerCommon
//       Derby LockManager (精简版)
// ============================================================

#include "storage/table.h"
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace sqltiny::core {

// —— 前向声明 ——
class TransactionManager;
class Session;

// ============================================================
// RowLock — 表级行锁 (参考 Derby LockManager 精简版)
// ============================================================
struct RowLock {
    enum State { FREE, SHARED_READ, EXCLUSIVE_WRITE };

    State    state   = FREE;
    Session* owner   = nullptr;   // EXCLUSIVE 时持有
    int      sharedCount = 0;     // SHARED 时计数

    // 是否 owner 持有 EXCLUSIVE (重入判断)
    bool is_exclusive_owned_by(Session* s) const {
        return state == EXCLUSIVE_WRITE && owner == s;
    }
};

// ============================================================
// RowAction — 每行的事务操作记录 (参考 HSQLDB RowAction.java)
//
// SELECT 不创建 RowAction (MVCC 快照隔离的关键优势!)
// 只有 INSERT / DELETE / UPDATE 的写操作才创建
// ============================================================
struct RowAction {
    enum Type {
        NONE,
        INSERT,            // 新行插入
        DELETE,            // 旧行标记删除
        UPDATE_DELETE,     // UPDATE = DELETE旧
        UPDATE_INSERT      // UPDATE = INSERT新
    };

    Type      type       = NONE;
    Session*  owner      = nullptr;
    uint64_t  actionSCN  = 0;        // 动作发生时分配的 SCN
    uint64_t  commitSCN  = 0;        // 提交后分配, 0 = 还没提交
    int64_t   rowId      = 0;
    std::string dbName;
    std::string tableName;
    storage::Row row;                // 操作涉及的行

    // —— 构造 helper ——
    static RowAction make_insert(Session* o, uint64_t ascn, int64_t rid,
                                  std::string db, std::string tbl, storage::Row r) {
        return {INSERT, o, ascn, 0, rid, std::move(db), std::move(tbl), std::move(r)};
    }
    static RowAction make_delete(Session* o, uint64_t ascn, int64_t rid,
                                  std::string db, std::string tbl, storage::Row r) {
        return {DELETE, o, ascn, 0, rid, std::move(db), std::move(tbl), std::move(r)};
    }

    // —— 冲突检测 ——
    // 同 rowId 下, 其他 Session 已提交的写操作会造成冲突
    bool conflicts_with(const RowAction& other) const {
        if (commitSCN != 0) return false;  // 自己已提交, 不管
        if (other.commitSCN == 0) return false;  // 对方还没提交, 暂不冲突
        if (other.rowId != rowId) return false;
        // 同 rowId 双方都是写 (INSERT/DELETE/UPDATE_*)
        return is_write(type) && is_write(other.type);
    }
    static bool is_write(Type t) {
        return t == INSERT || t == DELETE || t == UPDATE_DELETE || t == UPDATE_INSERT;
    }

    // —— 可见性判断 ——
    // querySCN 下这行是否可见 (commitSCN=0 永远不可见)
    bool is_visible(uint64_t querySCN) const {
        if (commitSCN == 0) return false;
        if (type == INSERT || type == UPDATE_INSERT)
            return commitSCN <= querySCN;
        if (type == DELETE || type == UPDATE_DELETE)
            return commitSCN > querySCN;   // 删除提交得比查询早 => 不可见
        return false;
    }
};

// ============================================================
// TransactionManager — 全局单例事务管理器
//
// 参考: HSQLDB TransactionManagerMVCC + TransactionManagerCommon
// 简化: 没有 latch 等待队列, 行锁冲突直接 abort (40001)
// ============================================================
class TransactionManager {
public:
    static TransactionManager& instance();

    // —— SCN 分配 ——
    uint64_t next_scn() { return next_scn_.fetch_add(1) + 1; }
    uint64_t current_scn() const { return next_scn_.load(); }
    void set_scn(uint64_t v) { next_scn_.store(v); }

    // —— 事务生命周期 ——
    // 在 writeLock_ 保护下分配 SCN, 加入 live_tx_scns_
    uint64_t begin_transaction(Session* s);

    // 冲突检测 + 分配 commitSCN + 更新 Table 版本链 + WAL 追加 +
    // 放到 committedQueue + mergeExpired
    // 返回 true = 成功; false = 冲突 (40001 Serialization Failure)
    bool commit_transaction(Session* s);

    // 回滚: 未提交 INSERT → 物理删除; 未提交 DELETE → 恢复
    void rollback_transaction(Session* s);

    // —— 行锁 ——
    // 让 Table 调用: 对指定 row_id 拿 EXCLUSIVE 锁
    // 返回 true = 成功; false = 冲突 (立即 abort 模式, 不等待)
    bool try_lock_row_write(storage::Table& tbl, int64_t row_id, Session* s);
    void unlock_row_write(storage::Table& tbl, int64_t row_id, Session* s);

private:
    TransactionManager() = default;
    ~TransactionManager() = default;

    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;

    // —— 内部: 处理 Table 版本链 ——
    void apply_commit_to_tables(const std::vector<RowAction>& actions, uint64_t commitSCN);
    void apply_rollback_to_tables(const std::vector<RowAction>& actions);
    void release_all_locks_for_session(Session* s);

    std::mutex              writeLock_;          // 事务边界串行化
    std::deque<uint64_t>    live_tx_scns_;       // 未结束事务 SCN (有序)
    std::atomic<uint64_t>   next_scn_{1};

    // —— 行锁表: Table* → (row_id → RowLock) ——
    // 用一个全局 table-level mutex 保护, 避免 Table.h 头文件依赖 TransactionManager
    struct TableLockEntry {
        std::mutex mu;
        std::map<int64_t, RowLock> locks;
    };
    std::mutex                      locks_map_mu_;
    std::map<storage::Table*, std::unique_ptr<TableLockEntry>> table_locks_;

    TableLockEntry* get_or_create_lock_entry(storage::Table* tbl);

    // —— 已提交事务 action 队列 (等 live tx SCN 过期后 finalize) ——
    std::deque<std::vector<RowAction>> committed_queue_;

    // 合并已经没有 live transaction 能看到的 committed actions
    void merge_expired_transactions();
    // 从 live 队列移除 + 检查 merge 条件
    void end_transaction(Session* s);
};

} // namespace sqltiny::core
