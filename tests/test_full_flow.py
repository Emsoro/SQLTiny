#!/usr/bin/env python3
"""SQLTiny 全场景 Python 测试 (PyMySQL)
启动 server:  .\\build\\Debug\\sqltiny_server.exe --port 3306 --user root
运行:         python tests\\test_full_flow.py
"""
import sys
import pymysql

HOST, PORT, USER = "127.0.0.1", 3306, "root"
TEST_DB = "sqltiny_py_test"

G, R, C, RESET = "\033[92m", "\033[91m", "\033[96m", "\033[0m"
passed = failed = 0

def step(name, fn):
    global passed, failed
    print(f"  {name} ... ", end="", flush=True)
    try:
        fn()
        print(f"{G}PASS{RESET}"); passed += 1
    except Exception as e:
        print(f"{R}FAIL{RESET}  ({e})"); failed += 1

def expect_err(sql, errno):
    try:
        cur.execute(sql); raise AssertionError(f"应该报 errno={errno}")
    except pymysql.err.MySQLError as e:
        # args 可能是 (errno, msg) 或 ((errno, msg),)
        actual = e.args[0] if isinstance(e.args[0], int) else e.args[0][0]
        if actual != errno:
            raise AssertionError(f"期望 {errno}, 实际 {actual} (exc={type(e).__name__})")

print(f"{C}=== SQLTiny Python Full-Flow Test (PyMySQL) ==={RESET}\n")

# 单一 connection 跑到底
print(f"{C}[0] Connectivity{RESET}")
try:
    conn = pymysql.connect(host=HOST, port=PORT, user=USER,
                           charset="utf8mb4", autocommit=True)
    cur = conn.cursor()
    cur.execute("SELECT 1"); cur.fetchone()
    print(f"  connect ... {G}PASS{RESET}")
    passed += 1
except Exception as e:
    print(f"  connect ... {R}FAIL{RESET} ({e})"); sys.exit(1)

# ============ DDL ============
print(f"{C}[1] DDL 建库/建表{RESET}")
cur.execute(f"DROP DATABASE IF EXISTS `{TEST_DB}`")
cur.execute(f"CREATE DATABASE `{TEST_DB}`")
cur.execute(f"USE `{TEST_DB}`")
cur.execute("CREATE TABLE users(id BIGINT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(64) NOT NULL, age INT DEFAULT 18, dept_id INT)")
cur.execute("CREATE TABLE orders(id INT PRIMARY KEY AUTO_INCREMENT, user_id INT, total DECIMAL(10,2) NOT NULL, status VARCHAR(16) DEFAULT 'pending')")
cur.execute("SHOW TABLES")
print(f"  SHOW TABLES: {[r[0] for r in cur.fetchall()]} ... {G}PASS{RESET}"); passed += 1

# ============ INSERT ============
print(f"{C}[2] INSERT{RESET}")
cur.execute(f"USE `{TEST_DB}`")
cur.execute("INSERT INTO users (name, age, dept_id) VALUES "
            "('Alice', 28, 1), ('Bob', 32, 1), ('Charlie', 25, 2), "
            "('Diana', 40, 2), ('Eve', NULL, NULL)")
cur.execute("INSERT INTO orders (user_id, total, status) VALUES "
            "(1, 100.50, 'paid'), (1, 200.00, 'paid'), "
            "(2, 50.00, 'pending'), (3, 300.00, 'paid'), (4, 75.25, 'cancelled')")
cur.execute("SELECT COUNT(*) FROM users"); nu = int(cur.fetchone()[0])
cur.execute("SELECT COUNT(*) FROM orders"); no = int(cur.fetchone()[0])
assert nu == 5 and no == 5
print(f"  users=5 orders=5 ... {G}PASS{RESET}"); passed += 1

# ============ SELECT * 含 NULL ============
print(f"{C}[3] SELECT * 含 NULL{RESET}")
step("SELECT * FROM users", lambda: (
    cur.execute("SELECT * FROM users ORDER BY id"),
    (lambda rows: (
        len(rows) == 5,
        rows[4][1] == 'Eve',
        rows[4][2] is None  # Eve age NULL
    ))(cur.fetchall())
))

# ============ 单表查询 ============
print(f"{C}[4] 单表查询{RESET}")
step("WHERE age > 28 → ['Bob','Diana']", lambda: (
    cur.execute("SELECT name FROM users WHERE age > 28"),
    None if [r[0] for r in cur.fetchall()] == ['Bob', 'Diana'] else (_ for _ in ()).throw(AssertionError("wrong rows"))
))
step("ORDER BY age DESC LIMIT 2", lambda: (
    cur.execute("SELECT name FROM users ORDER BY age DESC LIMIT 2"),
    None if [r[0] for r in cur.fetchall()] == ['Diana', 'Bob'] else (_ for _ in ()).throw(AssertionError("wrong"))
))
step("COUNT=5 SUM AVG≈31.25", lambda: (
    cur.execute("SELECT COUNT(*), SUM(age), AVG(age) FROM users"),
    (lambda r: None if (int(r[0]) == 5 and abs(float(r[2]) - 31.25) < 0.01) else (_ for _ in ()).throw(AssertionError(f"r={r}")))(cur.fetchone())
))
step("GROUP BY + HAVING SUM(total) > 100", lambda: (
    cur.execute("SELECT user_id, SUM(total) FROM orders GROUP BY user_id HAVING SUM(total) > 100 ORDER BY user_id"),
    (lambda rows: None if len(rows) >= 2 else (_ for _ in ()).throw(AssertionError("too few rows")))(cur.fetchall())
))

# ============ JOIN ============
print(f"{C}[5] JOIN{RESET}")
step("INNER JOIN users×orders = 5 行", lambda: (
    cur.execute("SELECT u.name, o.total FROM users u INNER JOIN orders o ON o.user_id = u.id"),
    (lambda rows: None if len(rows) == 5 else (_ for _ in ()).throw(AssertionError(f"rows={len(rows)}")))(cur.fetchall())
))
step("LEFT JOIN 含 NULL 行", lambda: (
    cur.execute("SELECT u.name, o.id, o.total FROM users u LEFT JOIN orders o ON o.user_id = u.id ORDER BY u.id, o.id"),
    (lambda rows: None if (len(rows) >= 4 and any(r[0] == 'Eve' and r[1] is None for r in rows)) else (_ for _ in ()).throw(AssertionError(f"rows={rows}")))(cur.fetchall())
))

# ============ UPDATE ============
print(f"{C}[6] UPDATE{RESET}")
step("UPDATE Eve age=30", lambda: (
    cur.execute("UPDATE users SET age=30 WHERE name='Eve'"),
    cur.execute("SELECT age FROM users WHERE name='Eve'"),
    (lambda r: None if r[0] == 30 else (_ for _ in ()).throw(AssertionError(f"age={r[0]}")))(cur.fetchone())
))

# ============ DELETE ============
print(f"{C}[7] DELETE{RESET}")
step("DELETE FROM users WHERE age < 30", lambda: (
    cur.execute("DELETE FROM users WHERE age < 30"),
    cur.execute("SELECT COUNT(*) FROM users"),
    (lambda r: None if int(r[0]) < 5 else (_ for _ in ()).throw(AssertionError("not deleted")))(cur.fetchone())
))

# ============ 错误路径 ============
print(f"{C}[8] 错误路径{RESET}")
step("DROP 不存在 → 1008", lambda: expect_err("DROP DATABASE no_such_db", 1008))
step("USE 不存在 → 1049", lambda: expect_err("USE no_such_db", 1049))
cur.execute(f"USE `{TEST_DB}`")  # 回到测试库
step("SELECT 不存在表 → 1146", lambda: expect_err("SELECT * FROM no_such_table", 1146))

# ============ 清理 ============
print(f"{C}[9] 清理 DROP{RESET}")
# SQLTiny/MySQL 都不允许 DROP 当前正在使用的库
cur.execute("USE default")
step(f"DROP DATABASE `{TEST_DB}`", lambda: cur.execute(f"DROP DATABASE `{TEST_DB}`"))
cur.execute("SHOW DATABASES")
for r in cur.fetchall():
    assert r[0] != TEST_DB, f"DROP 后仍有 {TEST_DB}"
print(f"  SHOW DATABASES 已清理 ... {G}PASS{RESET}"); passed += 1

conn.close()

# ============ 汇总 ============
total = passed + failed
print(f"\n{C}=== Result ==={RESET}")
print(f"  {G}{passed}{RESET}/{total} PASS")
if failed: print(f"  {R}{failed}{RESET}/{total} FAIL"); sys.exit(1)
print(f"\n{G}All {total} tests PASSED!{RESET} 🔥")
print(f"  全场景:建库→建表→插入→SELECT*含NULL→WHERE/ORDER/聚合/HAVING→INNER JOIN→LEFT JOIN含NULL→更新→删除→错误路径→删库")
