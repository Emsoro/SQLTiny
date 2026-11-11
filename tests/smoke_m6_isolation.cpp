// M6 · 四个隔离级别 + 事务语义冒烟测试 (进程内, 无需 server)
//
// 参考 HSQLDB 2.7.4:
//   SessionInterface.TX_*  = 1/2/4/8
//   Session.setIsolation()  READ UNCOMMITTED → READ COMMITTED (+只读)
//                           REPEATABLE READ  → SERIALIZABLE
//   RowAction.canRead()     READ COMMITTED = 语句级 SCN, RR/SER = 事务级 SCN
//   TransactionManagerMVCC  RR/SER 写冲突直接 40001 中止
//   sessions-chapt.html     四个级别都不允许脏读
// Python 版与 SQLite 对比见 tests/test_txn_vs_sqlite.py

#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>
#include "core/session.h"
#include "executor/executor.h"

using namespace sqltiny;
static int g_pass = 0, g_fail = 0;

static void expect(bool c, const std::string& l) {
    if (c) { ++g_pass; printf("  [PASS] %s\n", l.c_str()); }
    else   { ++g_fail; printf("  [FAIL] %s\n", l.c_str()); }
}
static void expect_i(int64_t got, int64_t want, const std::string& l) {
    if (got == want) { ++g_pass; printf("  [PASS] %s = %lld\n", l.c_str(), (long long)got); }
    else { ++g_fail; printf("  [FAIL] %s: got %lld want %lld\n", l.c_str(),
                            (long long)got, (long long)want); }
}
static void expect_s(const std::string& got, const std::string& want, const std::string& l) {
    if (got == want) { ++g_pass; printf("  [PASS] %s = %s\n", l.c_str(), got.c_str()); }
    else { ++g_fail; printf("  [FAIL] %s: got '%s' want '%s'\n", l.c_str(),
                            got.c_str(), want.c_str()); }
}
static executor::ResultSet q(core::Session& s, const std::string& sql) { return s.execute(sql); }
// to_string() 会把字符串值加引号,测试里比较的是原始值
static std::string plain(const types::Value& v) {
    return v.kind == types::Value::Kind::String ? v.v_str : v.to_string();
}
static void ok(core::Session& s, const std::string& sql) {
    auto r = q(s, sql);
    if (r.ok) ++g_pass;
    else { ++g_fail; printf("  [FAIL] %s -> errno=%u %s\n", sql.c_str(),
                            r.mysql_errno, r.error_message.c_str()); }
}
static void bad(core::Session& s, const std::string& sql, uint16_t eno, const std::string& l) {
    auto r = q(s, sql);
    if (!r.ok && r.mysql_errno == eno) { ++g_pass; printf("  [PASS] %s (errno=%u)\n", l.c_str(), eno); }
    else { ++g_fail; printf("  [FAIL] %s: ok=%d errno=%u want=%u msg=%s\n", l.c_str(),
                            r.ok, r.mysql_errno, eno, r.error_message.c_str()); }
}
static int64_t si(core::Session& s, const std::string& sql) {
    auto r = q(s, sql);
    if (!r.ok || r.rows.empty() || r.rows[0].empty()) return -999;
    int64_t v = 0; r.rows[0][0].try_to_int(v); return v;
}
static std::string ss(core::Session& s, const std::string& sql) {
    auto r = q(s, sql);
    if (!r.ok || r.rows.empty() || r.rows[0].empty()) return "<error>";
    return plain(r.rows[0][0]);
}
static std::string ids(core::Session& s) {
    auto r = q(s, "SELECT id FROM t ORDER BY id");
    if (!r.ok) return "<error>";
    std::string out;
    for (auto& row : r.rows) { if (!out.empty()) out += ","; out += plain(row[0]); }
    return out;
}
static void level(core::Session& s, const char* lvl) {
    ok(s, std::string("SET SESSION TRANSACTION ISOLATION LEVEL ") + lvl);
}
static void reset(core::Session& a, core::Session& b) {
    ok(a, "ROLLBACK"); ok(b, "ROLLBACK");
    level(a, "READ COMMITTED"); level(b, "READ COMMITTED");
    ok(a, "SET AUTOCOMMIT = 1"); ok(b, "SET AUTOCOMMIT = 1");
    ok(a, "USE default"); ok(a, "DROP DATABASE IF EXISTS iso");
    ok(a, "CREATE DATABASE iso"); ok(a, "USE iso");
    ok(a, "CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(32))");
}
static const char* kLevels[] = {"READ UNCOMMITTED", "READ COMMITTED",
                                "REPEATABLE READ", "SERIALIZABLE"};
static bool is_rc(const char* l) {
    return std::string(l) == "READ UNCOMMITTED" || std::string(l) == "READ COMMITTED";
}

// 1. 隔离级别设置 / HSQLDB 提升规则 / 系统变量真实反映
static void t1_settings(core::Session& a, core::Session& b) {
    printf("\n[1] 隔离级别设置与 HSQLDB 提升规则\n");
    expect_s(ss(b, "SELECT @@tx_isolation"), "READ-COMMITTED", "默认 READ COMMITTED");
    expect_s(ss(b, "SELECT @@autocommit"), "ON", "默认 autocommit ON");
    level(b, "READ UNCOMMITTED");
    expect_s(ss(b, "SELECT @@tx_isolation"), "READ-COMMITTED", "RU 提升为 RC");
    level(b, "REPEATABLE READ");
    expect_s(ss(b, "SELECT @@tx_isolation"), "SERIALIZABLE", "RR 提升为 SERIALIZABLE");
    level(b, "SERIALIZABLE");
    auto vr = q(b, "SHOW VARIABLES LIKE 'tx_isolation'");
    expect(vr.ok && vr.rows.size() == 1 && plain(vr.rows[0][1]) == "SERIALIZABLE",
           "SHOW VARIABLES 返回真实隔离级别");
    ok(b, "SET SESSION tx_isolation = 'REPEATABLE-READ'");
    expect_s(ss(b, "SELECT @@session.tx_isolation"), "SERIALIZABLE",
             "SET SESSION tx_isolation= 生效");
    level(b, "READ COMMITTED");
    // SET TRANSACTION 只作用于"下一个事务",中间不能再插 SELECT(会消耗掉它)
    ok(b, "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED");
    bad(b, "INSERT INTO t VALUES (99,'x')", 25006,
        "下一事务级 RU 提升为 RC 且只读,拒绝写");
    expect_s(ss(b, "SELECT @@tx_isolation"), "READ-COMMITTED", "事务结束恢复会话默认级别");
    ok(b, "BEGIN"); ok(b, "INSERT INTO t VALUES (98,'y')");
    bad(b, "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE", 25001,
        "事务中(已写)改隔离级别被拒绝 X_25001");
    ok(b, "ROLLBACK"); ok(b, "DELETE FROM t");
    (void)a;
}

// 2. 四个隔离级别都不允许脏读
static void t2_dirty(core::Session& a, core::Session& b) {
    printf("\n[2] 四个隔离级别 — 无脏读 / 未提交删除不影响其它会话\n");
    for (const char* L : kLevels) {
        reset(a, b); level(b, L); std::string t(L);
        ok(a, "INSERT INTO t VALUES (1,'base')");
        ok(a, "BEGIN"); ok(a, "INSERT INTO t VALUES (2,'dirty')");
        expect_i(si(b, "SELECT COUNT(*) FROM t"), 1, t + " 看不到未提交 INSERT");
        expect_i(si(a, "SELECT COUNT(*) FROM t"), 2, t + " 自己的 INSERT 可见");
        ok(a, "DELETE FROM t WHERE id=1");
        expect_s(ss(b, "SELECT v FROM t WHERE id=1"), "base", t + " 未提交 DELETE 不生效");
        expect_i(si(a, "SELECT COUNT(*) FROM t"), 1, t + " 自己的 DELETE 生效");
        ok(a, "ROLLBACK");
        expect_i(si(b, "SELECT COUNT(*) FROM t"), 1, t + " 回滚后其它会话见 1 行");
        expect_s(ss(a, "SELECT v FROM t WHERE id=1"), "base", t + " 回滚后原行完好");
    }
}

// 3. 不可重复读: RC 允许, RR/SER 事务级快照
static void t3_repeatable(core::Session& a, core::Session& b) {
    printf("\n[3] 不可重复读 (RC 看到新提交 / RR,SER 保持快照)\n");
    for (const char* L : kLevels) {
        reset(a, b); level(b, L); std::string t(L);
        ok(a, "INSERT INTO t VALUES (1,'base')");
        ok(b, "BEGIN");
        expect_i(si(b, "SELECT COUNT(*) FROM t"), 1, t + " 第一次读");
        ok(a, "INSERT INTO t VALUES (2,'new')");
        expect_i(si(b, "SELECT COUNT(*) FROM t"), is_rc(L) ? 2 : 1, t + " 第二次读");
        ok(b, "COMMIT");
        expect_i(si(b, "SELECT COUNT(*) FROM t"), 2, t + " 新事务读到 2 行");
    }
}

// 4. 已提交 UPDATE 的可见性
static void t4_update_visibility(core::Session& a, core::Session& b) {
    printf("\n[4] 已提交 UPDATE 的可见性\n");
    for (const char* L : kLevels) {
        reset(a, b); level(b, L); std::string t(L);
        ok(a, "INSERT INTO t VALUES (1,'base')");
        ok(b, "BEGIN");
        expect_s(ss(b, "SELECT v FROM t WHERE id=1"), "base", t + " 初始 v");
        ok(a, "UPDATE t SET v='changed' WHERE id=1");
        expect_s(ss(b, "SELECT v FROM t WHERE id=1"), is_rc(L) ? "changed" : "base",
                 t + " 其它会话提交后读到的 v");
        ok(b, "COMMIT");
        expect_s(ss(b, "SELECT v FROM t WHERE id=1"), "changed", t + " 新事务读到 changed");
    }
}

// 5. 写-写冲突 (行锁) 与同事务重入
static void t5_write_conflict(core::Session& a, core::Session& b) {
    printf("\n[5] 写-写冲突 / 行锁重入\n");
    reset(a, b);
    ok(a, "INSERT INTO t VALUES (1,'v0')");
    ok(a, "BEGIN"); ok(a, "UPDATE t SET v='A' WHERE id=1");
    ok(b, "BEGIN");
    bad(b, "UPDATE t SET v='B' WHERE id=1", 1213, "并发 UPDATE 同一行被 40001 中止");
    ok(a, "UPDATE t SET v='A2' WHERE id=1");   // 同事务内重入
    ok(a, "COMMIT"); ok(b, "ROLLBACK");
    expect_s(ss(b, "SELECT v FROM t WHERE id=1"), "A2", "最终值为 A2");
}

// 6. 快照隔离的陈旧写冲突 (HSQLDB X_40501)
static void t6_snapshot_conflict(core::Session& a, core::Session& b) {
    printf("\n[6] 快照隔离陈旧写冲突\n");
    reset(a, b); level(b, "SERIALIZABLE");
    ok(a, "INSERT INTO t VALUES (1,'v0')");
    ok(b, "BEGIN");
    expect_s(ss(b, "SELECT v FROM t WHERE id=1"), "v0", "B 读到 v0");
    ok(a, "UPDATE t SET v='A' WHERE id=1");
    bad(b, "UPDATE t SET v='B' WHERE id=1", 1213,
        "SERIALIZABLE 快照之后被改的行 → 40001");
    ok(b, "ROLLBACK");
    expect_s(ss(b, "SELECT v FROM t WHERE id=1"), "A", "回滚后保留 A 的更新");

    reset(a, b); level(b, "READ COMMITTED");
    ok(a, "INSERT INTO t VALUES (1,'v0')");
    ok(b, "BEGIN");
    expect_s(ss(b, "SELECT v FROM t WHERE id=1"), "v0", "RC B 读到 v0");
    ok(a, "UPDATE t SET v='A' WHERE id=1");
    ok(b, "UPDATE t SET v='B' WHERE id=1");
    ok(b, "COMMIT");
    expect_s(ss(b, "SELECT v FROM t WHERE id=1"), "B", "RC 允许后写覆盖(最后提交者胜)");
}

// 7. autocommit / BEGIN / COMMIT / ROLLBACK
static void t7_autocommit(core::Session& a, core::Session& b) {
    printf("\n[7] autocommit 与显式事务\n");
    reset(a, b);
    ok(b, "SET AUTOCOMMIT = 0");
    expect_s(ss(b, "SELECT @@autocommit"), "OFF", "autocommit OFF");
    ok(b, "INSERT INTO t VALUES (3,'pending')");
    expect_i(si(a, "SELECT COUNT(*) FROM t"), 0, "OFF 时其它会话看不到未提交行");
    expect_i(si(b, "SELECT COUNT(*) FROM t"), 1, "OFF 时自己能看到");
    ok(b, "ROLLBACK");
    expect_i(si(b, "SELECT COUNT(*) FROM t"), 0, "ROLLBACK 后为空");
    ok(b, "INSERT INTO t VALUES (4,'kept')");
    expect_i(si(a, "SELECT COUNT(*) FROM t"), 0, "COMMIT 前仍不可见");
    ok(b, "COMMIT");
    expect_i(si(a, "SELECT COUNT(*) FROM t"), 1, "COMMIT 后可见");
    ok(b, "SET AUTOCOMMIT = 1");
    expect_s(ss(b, "SELECT @@autocommit"), "ON", "autocommit ON");
    ok(b, "INSERT INTO t VALUES (5,'auto')");
    expect_i(si(a, "SELECT COUNT(*) FROM t"), 2, "ON 时立即提交可见");
}

// 8. INSERT/UPDATE/DELETE 的提交与回滚 (含主键索引恢复)
static void t8_dml_rollback(core::Session& a, core::Session& b) {
    printf("\n[8] DML 提交 / 回滚 / 主键索引恢复\n");
    reset(a, b);
    ok(a, "INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c')");
    ok(a, "BEGIN");
    ok(a, "INSERT INTO t VALUES (4,'d')");
    ok(a, "UPDATE t SET v='A' WHERE id=1");
    ok(a, "DELETE FROM t WHERE id=2");
    expect_s(ids(a), "1,3,4", "事务内可见集合");
    expect_s(ss(a, "SELECT v FROM t WHERE id=1"), "A", "事务内读到自己的 UPDATE");
    ok(a, "ROLLBACK");
    expect_s(ids(a), "1,2,3", "回滚恢复全部行");
    expect_s(ss(a, "SELECT v FROM t WHERE id=1"), "a", "回滚恢复原值");
    bad(a, "INSERT INTO t VALUES (2,'dup')", 1062, "回滚后主键索引已恢复(重复主键被拒)");
    ok(a, "DELETE FROM t WHERE id=2");
    ok(a, "INSERT INTO t VALUES (2,'again')");
    expect_s(ids(a), "1,2,3", "删除后可重新插入同主键");

    reset(a, b);
    ok(a, "INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c')");
    ok(a, "BEGIN");
    ok(a, "INSERT INTO t VALUES (4,'d')");
    ok(a, "UPDATE t SET v='A' WHERE id=1");
    ok(a, "DELETE FROM t WHERE id=2");
    ok(a, "COMMIT");
    expect_s(ids(a), "1,3,4", "提交后可见集合");
    expect_s(ss(b, "SELECT v FROM t WHERE id=1"), "A", "提交后其它会话读到新值");
    expect_i(si(a, "SELECT COUNT(*) FROM t WHERE id=2"), 0, "提交删除仍不可见");
}

// 9. SAVEPOINT
static void t9_savepoint(core::Session& a, core::Session& b) {
    printf("\n[9] SAVEPOINT / ROLLBACK TO / RELEASE\n");
    reset(a, b);
    ok(a, "BEGIN");
    ok(a, "INSERT INTO t VALUES (1,'a')");
    ok(a, "SAVEPOINT s1");
    ok(a, "INSERT INTO t VALUES (2,'b')");
    ok(a, "SAVEPOINT s2");
    ok(a, "INSERT INTO t VALUES (3,'c')");
    ok(a, "ROLLBACK TO SAVEPOINT s1");
    expect_s(ids(a), "1", "回滚到 s1 只保留第一行");
    ok(a, "COMMIT");
    expect_s(ids(b), "1", "提交后其它会话也只看到第一行");
    bad(a, "ROLLBACK TO SAVEPOINT s1", 1305, "事务结束后保存点失效");
    ok(a, "BEGIN");
    ok(a, "SAVEPOINT sp"); ok(a, "RELEASE SAVEPOINT sp"); ok(a, "COMMIT");
    bad(a, "ROLLBACK TO SAVEPOINT sp", 1305, "RELEASE 后保存点不存在");
}

// 10. START TRANSACTION ISOLATION LEVEL (仅下一个事务)
static void t10_start_transaction(core::Session& a, core::Session& b) {
    printf("\n[10] START TRANSACTION ISOLATION LEVEL\n");
    reset(a, b);
    ok(b, "START TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    ok(a, "INSERT INTO t VALUES (1,'x')");
    expect_i(si(b, "SELECT COUNT(*) FROM t"), 0, "SERIALIZABLE 事务看不到之后提交的行");
    ok(b, "COMMIT");
    expect_i(si(b, "SELECT COUNT(*) FROM t"), 1, "COMMIT 后新事务可见");
    expect_s(ss(b, "SELECT @@tx_isolation"), "READ-COMMITTED",
             "START TRANSACTION 的隔离级别不残留到后续事务");
}

int main() {
    printf("=== SQLTiny M6 · 四隔离级别 + 事务冒烟测试 ===\n");
    core::Session a, b;
    reset(a, b);
    t1_settings(a, b);
    t2_dirty(a, b);
    t3_repeatable(a, b);
    t4_update_visibility(a, b);
    t5_write_conflict(a, b);
    t6_snapshot_conflict(a, b);
    t7_autocommit(a, b);
    t8_dml_rollback(a, b);
    t9_savepoint(a, b);
    t10_start_transaction(a, b);
    ok(a, "ROLLBACK"); ok(b, "ROLLBACK");
    ok(a, "USE default"); ok(a, "DROP DATABASE IF EXISTS iso");
    printf("\n=== %d PASS / %d FAIL ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
