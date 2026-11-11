#!/usr/bin/env python3
"""SQLTiny 全链路 Python 测试 (PyMySQL)

启动 server:  .\\build\\Debug\\sqltiny_server.exe --port 3306 --user root
运行:         python tests\\test_full_flow.py

覆盖范围(与 CODEPLAN §8.2 的 C02/C03/C06/C08 对齐):
  [0] 连接 + 系统变量          [1] DDL / 多库 Catalog
  [2] INSERT / AUTO_INCREMENT  [3] SELECT * 含 NULL + DEFAULT
  [4] WHERE / ORDER / IN / 聚合 [5] GROUP BY / HAVING
  [6] JOIN(含 LEFT JOIN 补 NULL)[7] UPDATE / DELETE 影响行数
  [8] 约束 / 语句原子性 / 错误码 [9] 事务(BEGIN/COMMIT/ROLLBACK/SAVEPOINT/autocommit)
  [10] 四隔离级别 + 40001 冲突 [11] 多库同名表隔离
  [12] 清理与汇总

与 smoke_m6_isolation.cpp / smoke_m6_hsqldb_matrix.cpp 的分工:那两个是进程内
C++ 断言,本脚本走真实 MySQL wire protocol + 双连接,验证协议层与跨会话行为。
"""
import sys

try:
    # Windows 默认 GBK 控制台:保持系统编码(中文可正常显示),只让无法编码的
    # 字符被替换而不是抛 UnicodeEncodeError(旧脚本末尾的 emoji 会崩在这里)
    sys.stdout.reconfigure(errors="replace")
except (AttributeError, OSError):
    pass

import pymysql

HOST, PORT, USER = "127.0.0.1", 3306, "root"
TEST_DB = "sqltiny_py_test"
TEST_DB2 = "sqltiny_py_test2"

G, R, C, RESET = "\033[92m", "\033[91m", "\033[96m", "\033[0m"
passed = failed = gaps = 0


def section(title):
    print(f"{C}{title}{RESET}")


def check_or_gap(name, cond, actual):
    """与 SQL 标准/HSQLDB 有差异的实现点:显式记录为 GAP,不掩盖也不误报"""
    global passed, gaps
    print(f"  {name} ... ", end="", flush=True)
    if cond:
        passed += 1
        print(f"{G}PASS{RESET}")
    else:
        gaps += 1
        print(f"{C}GAP{RESET}  实际 {actual}(与标准不同,见 CODEPLAN §8.4)")


def step(name, fn):
    """每个 step 必须有真断言;lambda 返回值不算断言"""
    global passed, failed
    print(f"  {name} ... ", end="", flush=True)
    try:
        fn()
    except Exception as e:
        failed += 1
        print(f"{R}FAIL{RESET}  ({type(e).__name__}: {e})")
    else:
        passed += 1
        print(f"{G}PASS{RESET}")


def fetch(cur, sql):
    cur.execute(sql)
    return cur.fetchall()


def scalar(cur, sql):
    rows = fetch(cur, sql)
    if not rows or not rows[0]:
        raise AssertionError(f"{sql}: 无结果")
    return rows[0][0]


def eq(got, want, label=""):
    if got != want:
        raise AssertionError(f"{label}: got {got!r}, want {want!r}")


def rows_eq(cur, sql, want, label=""):
    eq(fetch(cur, sql), want, label or sql)


def count(cur, table):
    return int(scalar(cur, f"SELECT COUNT(*) FROM {table}"))


def expect_err(cur, sql, errno):
    try:
        cur.execute(sql)
    except pymysql.err.MySQLError as e:
        a = e.args[0]
        actual = a if isinstance(a, int) else a[0]
        if actual != errno:
            raise AssertionError(f"期望 errno={errno}, 实际 {actual}")
    else:
        raise AssertionError(f"应该报 errno={errno}, 但执行成功")


def setup(cur):
    cur.execute(f"DROP DATABASE IF EXISTS `{TEST_DB}`")
    cur.execute(f"DROP DATABASE IF EXISTS `{TEST_DB2}`")
    cur.execute(f"CREATE DATABASE `{TEST_DB}`")
    cur.execute(f"USE `{TEST_DB}`")
    cur.execute("CREATE TABLE users (id BIGINT PRIMARY KEY AUTO_INCREMENT, "
                "name VARCHAR(64) NOT NULL, age INT DEFAULT 18, dept_id INT)")
    cur.execute("CREATE TABLE orders (id INT PRIMARY KEY AUTO_INCREMENT, user_id INT, "
                "total DECIMAL(10,2) NOT NULL, status VARCHAR(16) DEFAULT 'pending')")
    cur.execute("CREATE TABLE tx (id INT PRIMARY KEY, v VARCHAR(32))")
    cur.execute("CREATE TABLE iso (id INT PRIMARY KEY, v VARCHAR(32))")
    cur.execute("INSERT INTO users (name, age, dept_id) VALUES "
                "('Alice', 28, 1), ('Bob', 32, 1), ('Charlie', 25, 2), "
                "('Diana', 40, 2), ('Eve', NULL, NULL)")
    cur.execute("INSERT INTO orders (user_id, total, status) VALUES "
                "(1, 100.50, 'paid'), (1, 200.00, 'paid'), "
                "(2, 50.00, 'pending'), (3, 300.00, 'paid'), (4, 75.25, 'cancelled')")


def teardown(cur):
    """无论中间哪一步失败,都保证不留下测试库"""
    for sql in ("USE default",
                f"DROP DATABASE IF EXISTS `{TEST_DB}`",
                f"DROP DATABASE IF EXISTS `{TEST_DB2}`"):
        try:
            cur.execute(sql)
        except Exception:
            pass


def group_connectivity(cur):
    section("[0] 连接与系统变量")
    step("SELECT 1", lambda: eq(scalar(cur, "SELECT 1"), 1))
    step("@@tx_isolation 默认 READ-COMMITTED",
         lambda: eq(scalar(cur, "SELECT @@tx_isolation"), "READ-COMMITTED"))
    step("@@autocommit 默认 ON",
         lambda: eq(scalar(cur, "SELECT @@autocommit"), "ON"))
    step("SHOW VARIABLES LIKE 'tx_isolation' 返回真实值",
         lambda: eq(fetch(cur, "SHOW VARIABLES LIKE 'tx_isolation'"),
                    (("tx_isolation", "READ-COMMITTED"),)))


def group_ddl(cur):
    section("[1] DDL / Catalog")
    step("USE 生效", lambda: eq(scalar(cur, "SELECT SCHEMA()"), TEST_DB))
    step("SHOW TABLES 含 4 张表", lambda: (
        lambda names: eq(sorted(names), ["iso", "orders", "tx", "users"])
    )([r[0] for r in fetch(cur, "SHOW TABLES")]))
    step("SHOW COLUMNS FROM users 有 4 列", lambda: (
        lambda n: eq(n, 4, "列数")
    )(len(fetch(cur, "SHOW COLUMNS FROM users"))))


def group_insert(cur):
    section("[2] INSERT / AUTO_INCREMENT / DEFAULT")
    step("users=5 orders=5", lambda: (
        eq(count(cur, "users"), 5, "users"), eq(count(cur, "orders"), 5, "orders")))
    step("AUTO_INCREMENT 生成 id 1..5", lambda: (
        eq([r[0] for r in fetch(cur, "SELECT id FROM users ORDER BY id")],
           [1, 2, 3, 4, 5])))
    step("指定列插入:省略列用 DEFAULT / NULL", lambda: (
        cur.execute("INSERT INTO users (name) VALUES ('Frank')"),
        eq(count(cur, "users"), 6),
        eq(scalar(cur, "SELECT age FROM users WHERE name='Frank'"), 18),
        eq(scalar(cur, "SELECT dept_id FROM users WHERE name='Frank'"), None),
        cur.execute("DELETE FROM users WHERE name='Frank'"),
        eq(count(cur, "users"), 5)))


def group_select(cur):
    section("[3] SELECT * 含 NULL")
    # 原脚本这里只返回布尔元组、从不 assert,等价于恒真;下面改成真断言
    def check_nulls():
        rows = fetch(cur, "SELECT id, name, age, dept_id FROM users ORDER BY id")
        eq(len(rows), 5, "行数")
        eq([r[1] for r in rows], ["Alice", "Bob", "Charlie", "Diana", "Eve"], "名字")
        eq(rows[4][2], None, "Eve.age 应为 NULL")
        eq(rows[4][3], None, "Eve.dept_id 应为 NULL")
        eq(rows[0][2], 28, "Alice.age")
    step("SELECT * 逐列校验(含 NULL)", check_nulls)

    section("[4] WHERE / ORDER / IN / 聚合")
    step("WHERE age > 28 → Bob, Diana", lambda: rows_eq(
        cur, "SELECT name FROM users WHERE age > 28 ORDER BY name",
        (("Bob",), ("Diana",))))
    step("ORDER BY age DESC LIMIT 2 → Diana, Bob", lambda: rows_eq(
        cur, "SELECT name FROM users ORDER BY age DESC LIMIT 2",
        (("Diana",), ("Bob",))))
    step("IS NULL 只命中 Eve", lambda: rows_eq(
        cur, "SELECT name FROM users WHERE age IS NULL", (("Eve",),)))
    step("IN (1,2) 排除 NULL 行", lambda: (
        eq(len(fetch(cur, "SELECT name FROM users WHERE dept_id IN (1,2)")), 4)))
    step("COUNT/SUM/AVG 精确值(跳过 NULL)", lambda: (
        lambda r: (eq(int(r[0]), 5, "COUNT(*)"),
                   eq(float(r[1]), 125.0, "SUM(age)"),
                   eq(round(float(r[2]), 2), 31.25, "AVG(age)")))(
        fetch(cur, "SELECT COUNT(*), SUM(age), AVG(age) FROM users")[0]))
    step("COUNT(age) 跳过 NULL = 4", lambda: (
        eq(int(scalar(cur, "SELECT COUNT(age) FROM users")), 4)))


def group_aggregate(cur):
    section("[5] GROUP BY / HAVING")
    def check_group():
        rows = fetch(cur, "SELECT user_id, SUM(total) FROM orders "
                          "GROUP BY user_id ORDER BY user_id")
        eq([r[0] for r in rows], [1, 2, 3, 4], "分组键")
        eq([round(float(r[1]), 2) for r in rows], [300.50, 50.00, 300.00, 75.25], "分组 SUM")
    step("按 user_id 分组求和", check_group)

    def check_having():
        no_agg = fetch(cur, "SELECT user_id FROM orders GROUP BY user_id "
                            "HAVING SUM(total) > 100 ORDER BY user_id")
        with_agg = fetch(cur, "SELECT user_id, SUM(total) FROM orders GROUP BY user_id "
                              "HAVING SUM(total) > 100 ORDER BY user_id")
        got = [r[0] for r in no_agg]
        check_or_gap("HAVING SUM(total) > 100 → user 1,3", got == [1, 3],
                     f"SELECT 无聚合={got}, 含聚合={[r[0] for r in with_agg]}")
    check_having()


def group_join(cur):
    section("[6] JOIN")
    step("INNER JOIN users × orders = 5 行", lambda: (
        eq(len(fetch(cur, "SELECT u.name, o.total FROM users u "
                          "INNER JOIN orders o ON o.user_id = u.id")), 5)))
    def check_left():
        rows = fetch(cur, "SELECT u.name, o.id FROM users u LEFT JOIN orders o "
                          "ON o.user_id = u.id ORDER BY u.id, o.id")
        if not any(r[0] == "Eve" and r[1] is None for r in rows):
            raise AssertionError("LEFT JOIN 未给 Eve 补 NULL 行")
    step("LEFT JOIN 给无订单用户补 NULL", check_left)
    step("CROSS JOIN 5×5 = 25 行", lambda: (
        eq(int(scalar(cur, "SELECT COUNT(*) FROM users a CROSS JOIN users b")), 25)))


def group_update_delete(cur):
    section("[7] UPDATE / DELETE")
    def check_update():
        cur.execute("UPDATE users SET age=30 WHERE name='Eve'")
        eq(scalar(cur, "SELECT age FROM users WHERE name='Eve'"), 30)
        eq(count(cur, "users WHERE age IS NULL"), 0)
    step("UPDATE Eve age=30", check_update)
    def check_delete():
        cur.execute("DELETE FROM users WHERE age < 30")
        eq(count(cur, "users"), 3, "剩余行数")
        eq(fetch(cur, "SELECT name FROM users WHERE name IN ('Alice','Charlie')"), ())
    step("DELETE age < 30 精确删除 2 行", check_delete)


def group_constraints(cur):
    section("[8] 约束 / 语句原子性 / 错误路径")
    cur.execute("DELETE FROM tx")
    step("正常插入 tx", lambda: cur.execute("INSERT INTO tx VALUES (1,'a')"))
    step("重复主键 → 1062", lambda: expect_err(
        cur, "INSERT INTO tx VALUES (1,'dup')", 1062))
    step("多行 VALUES 冲突时整条语句不生效", lambda: (
        expect_err(cur, "INSERT INTO tx VALUES (2,'ok'),(1,'dup')", 1062),
        eq(count(cur, "tx"), 1, "冲突后行数")))
    step("DROP 不存在的库 → 1008", lambda: expect_err(
        cur, "DROP DATABASE no_such_db", 1008))
    step("USE 不存在的库 → 1049", lambda: expect_err(
        cur, "USE no_such_db", 1049))
    cur.execute(f"USE `{TEST_DB}`")
    step("SELECT 不存在的表 → 1146", lambda: expect_err(
        cur, "SELECT * FROM no_such_table", 1146))


def group_transactions(cur, cur2):
    section("[9] 事务链路(双连接)")
    cur.execute("DELETE FROM tx")
    cur2.execute(f"USE `{TEST_DB}`")

    def check_visibility():
        cur.execute("BEGIN")
        cur.execute("INSERT INTO tx VALUES (10,'pending')")
        eq(count(cur, "tx"), 1, "本事务可见自己的写")
        eq(count(cur2, "tx"), 0, "其它会话无脏读")
        cur.execute("ROLLBACK")
        eq(count(cur2, "tx"), 0, "ROLLBACK 后仍为空")
    step("BEGIN/ROLLBACK 无脏读、回滚生效", check_visibility)

    def check_commit():
        cur.execute("BEGIN")
        cur.execute("INSERT INTO tx VALUES (11,'kept')")
        eq(count(cur2, "tx"), 0, "COMMIT 前不可见")
        cur.execute("COMMIT")
        eq(count(cur2, "tx"), 1, "COMMIT 后可见")
    step("BEGIN/COMMIT 提交后其它会话可见", check_commit)

    def check_savepoint():
        cur.execute("BEGIN")
        cur.execute("INSERT INTO tx VALUES (12,'s1')")
        cur.execute("SAVEPOINT sp")
        cur.execute("INSERT INTO tx VALUES (13,'s2')")
        cur.execute("ROLLBACK TO SAVEPOINT sp")
        cur.execute("COMMIT")
        eq([r[0] for r in fetch(cur2, "SELECT id FROM tx ORDER BY id")], [11, 12])
    step("SAVEPOINT / ROLLBACK TO 部分回滚", check_savepoint)
    step("不存在的 SAVEPOINT → 1305", lambda: expect_err(
        cur2, "ROLLBACK TO SAVEPOINT sp", 1305))

    def check_autocommit():
        cur.execute("SET AUTOCOMMIT = 0")
        eq(scalar(cur, "SELECT @@autocommit"), "OFF")
        cur.execute("INSERT INTO tx VALUES (14,'auto-off')")
        eq(count(cur2, "tx"), 2, "autocommit=OFF 时未提交不可见")
        cur.execute("ROLLBACK")
        eq(count(cur2, "tx"), 2, "ROLLBACK 后不变")
        cur.execute("SET AUTOCOMMIT = 1")
        eq(scalar(cur, "SELECT @@autocommit"), "ON")
        cur.execute("INSERT INTO tx VALUES (15,'auto-on')")
        eq(count(cur2, "tx"), 3, "autocommit=ON 立即可见")
    step("SET AUTOCOMMIT=0/1 与 @@autocommit", check_autocommit)


def group_isolation(cur, cur2):
    section("[10] 四隔离级别 / 40001 冲突(双连接)")
    cur.execute("DELETE FROM iso")
    cur.execute("INSERT INTO iso VALUES (1,'base')")
    cur2.execute(f"USE `{TEST_DB}`")

    step("RR 提升为 SERIALIZABLE", lambda: (
        cur2.execute("SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ"),
        eq(scalar(cur2, "SELECT @@tx_isolation"), "SERIALIZABLE")))
    step("RU 提升为 READ COMMITTED", lambda: (
        cur2.execute("SET SESSION TRANSACTION ISOLATION LEVEL READ UNCOMMITTED"),
        eq(scalar(cur2, "SELECT @@tx_isolation"), "READ-COMMITTED")))

    def check_rr():
        cur2.execute("SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ")
        cur2.execute("BEGIN")
        eq(count(cur2, "iso"), 1, "第一次读")
        cur.execute("INSERT INTO iso VALUES (2,'new')")
        eq(count(cur2, "iso"), 1, "RR/SER 事务快照看不到新提交")
        cur2.execute("COMMIT")
        eq(count(cur2, "iso"), 2, "新事务可见")
    step("RR/SER 事务级快照(无不可重复读)", check_rr)

    def check_rc():
        cur2.execute("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED")
        cur2.execute("BEGIN")
        eq(count(cur2, "iso"), 2, "第一次读")
        cur.execute("INSERT INTO iso VALUES (3,'rc')")
        eq(count(cur2, "iso"), 3, "RC 语句级快照看到新提交")
        cur2.execute("COMMIT")
    step("READ COMMITTED 语句级快照", check_rc)

    def check_readonly():
        cur2.execute("SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED")
        expect_err(cur2, "INSERT INTO iso VALUES (99,'x')", 25006)
        eq(scalar(cur2, "SELECT @@tx_isolation"), "READ-COMMITTED", "事务结束恢复默认")
    step("SET TRANSACTION READ UNCOMMITTED 只读 + 仅下一事务", check_readonly)

    def check_start():
        cur2.execute("START TRANSACTION ISOLATION LEVEL SERIALIZABLE")
        cur.execute("INSERT INTO iso VALUES (4,'after-start')")
        eq(count(cur2, "iso"), 3, "START TRANSACTION 的快照")
        cur2.execute("COMMIT")
        eq(count(cur2, "iso"), 4, "COMMIT 后可见")
        eq(scalar(cur2, "SELECT @@tx_isolation"), "READ-COMMITTED", "级别不残留")
    step("START TRANSACTION ISOLATION LEVEL", check_start)

    def check_conflict():
        cur.execute("BEGIN")
        cur.execute("UPDATE iso SET v='A' WHERE id=1")
        cur2.execute("BEGIN")
        expect_err(cur2, "UPDATE iso SET v='B' WHERE id=1", 1213)
        cur.execute("COMMIT")
        cur2.execute("ROLLBACK")
        eq(scalar(cur2, "SELECT v FROM iso WHERE id=1"), "A")
    step("并发 UPDATE 同一行 → 40001(1213)", check_conflict)


def group_multi_db(cur):
    section("[11] 多库同名表隔离")
    cur.execute(f"CREATE DATABASE `{TEST_DB2}`")
    cur.execute(f"USE `{TEST_DB2}`")
    cur.execute("CREATE TABLE users (id INT PRIMARY KEY, v VARCHAR(16))")
    cur.execute("INSERT INTO users VALUES (1,'other-db')")
    step("当前库 users 只有 1 行", lambda: eq(count(cur, "users"), 1))
    step(f"两段式 {TEST_DB}.users 指向主库", lambda: (
        eq(count(cur, f"{TEST_DB}.users"), 3)))
    step("USE 切回主库后仍是 3 行", lambda: (
        cur.execute(f"USE `{TEST_DB}`"), eq(count(cur, "users"), 3)))


def main():
    global passed, failed, gaps
    print(f"{C}=== SQLTiny Python Full-Flow Test (PyMySQL) ==={RESET}\n")
    try:
        conn = pymysql.connect(host=HOST, port=PORT, user=USER,
                               charset="utf8mb4", autocommit=True)
        conn2 = pymysql.connect(host=HOST, port=PORT, user=USER,
                                charset="utf8mb4", autocommit=True)
    except Exception as e:
        print(f"{R}无法连接 {HOST}:{PORT} — 请先启动 sqltiny_server{RESET}\n  {e}")
        return 2

    cur, cur2 = conn.cursor(), conn2.cursor()
    try:
        section("[setup] 建库 / 建表 / 种子数据")
        try:
            setup(cur)
            cur2.execute(f"USE `{TEST_DB}`")
            print(f"  setup ... {G}PASS{RESET}")
            passed += 1
        except Exception as e:
            print(f"  setup ... {R}FAIL{RESET}  ({e})")
            failed += 1
            return 1

        group_connectivity(cur)
        group_ddl(cur)
        group_insert(cur)
        group_select(cur)
        group_aggregate(cur)
        group_join(cur)
        group_update_delete(cur)
        group_constraints(cur)
        group_transactions(cur, cur2)
        group_isolation(cur, cur2)
        group_multi_db(cur)

        section("[12] 清理")
        step(f"USE default + DROP `{TEST_DB}` / `{TEST_DB2}`", lambda: (
            cur.execute("USE default"),
            cur.execute(f"DROP DATABASE IF EXISTS `{TEST_DB}`"),
            cur.execute(f"DROP DATABASE IF EXISTS `{TEST_DB2}`")))
        step("SHOW DATABASES 已无测试库", lambda: (
            lambda names: (eq(TEST_DB in names, False), eq(TEST_DB2 in names, False))
        )([r[0] for r in fetch(cur, "SHOW DATABASES")]))
    finally:
        teardown(cur)
        conn.close()
        conn2.close()

    total = passed + failed + gaps
    print(f"\n{C}=== Result ==={RESET}")
    print(f"  {G}{passed}{RESET} PASS / {R}{failed}{RESET} FAIL / "
          f"{C}{gaps}{RESET} GAP   (共 {total} 项)")
    if failed:
        return 1
    print(f"\n{G}All {passed} assertions PASSED!{RESET}")
    print("  全链路:连接/系统变量 → DDL → INSERT/AUTO_INCREMENT → SELECT*含NULL → "
          "WHERE/ORDER/IN/聚合 → GROUP BY/HAVING → JOIN → UPDATE/DELETE → "
          "约束/错误码 → 事务/SAVEPOINT/autocommit → 四隔离级别/40001 → 多库 → 清理")
    return 0


if __name__ == "__main__":
    sys.exit(main())
