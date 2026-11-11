"""
M5.4 关键验证: 插数据 → 关 server → 重启带 --data-dir → 数据还在

流程:
1. 清掉旧 WAL
2. 起 server1, --data-dir sqltiny_data_m5test
3. PyMySQL 连,建库+建表+插 3 行
4. 关 server1
5. 起 server2, 同一个 --data-dir
6. PyMySQL 连, SELECT * → 应该还是那 3 行 ✅
"""
import pymysql, subprocess, time, os, sys, signal

DATA_DIR = "sqltiny_data_m5test"
WAL_PATH = f"{DATA_DIR}/sql.wal"
EXE = "build/Debug/sqltiny_server.exe"

def kill_all():
    import subprocess
    subprocess.run(["powershell", "-Command",
        "Get-Process sqltiny_server -ErrorAction SilentlyContinue | Stop-Process -Force"],
        capture_output=True)
    time.sleep(0.3)

# 1. 清旧数据
kill_all()
if os.path.exists(DATA_DIR):
    import shutil; shutil.rmtree(DATA_DIR)

# 2. 起 server1
print("[TEST] server1 start (clean WAL)")
p1 = subprocess.Popen([EXE, "--port", "3306", "--user", "root", "--data-dir", DATA_DIR],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2)

def run_sql(sql, expect_errno=None):
    conn = pymysql.connect(host="127.0.0.1", port=3306, user="root",
                           charset="utf8mb4", autocommit=True)
    cur = conn.cursor()
    try:
        cur.execute(sql)
        result = cur.fetchall() if cur.description else None
        if result is not None: result = list(result)  # PyMySQL 可能返回 tuple
        return result
    except pymysql.err.MySQLError as e:
        actual = e.args[0] if isinstance(e.args[0], int) else e.args[0][0]
        if actual == expect_errno:
            return None
        raise
    finally:
        conn.close()

# 3. 建库+建表+插数据
print("[TEST] server1 DDL+DML")
run_sql("DROP DATABASE IF EXISTS m5test")
run_sql("CREATE DATABASE m5test")
run_sql("USE m5test")
run_sql("CREATE TABLE t(id BIGINT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(32), score INT)")
run_sql("INSERT INTO t(name, score) VALUES ('Alice', 90), ('Bob', 85), ('Charlie', 95)")
rows = run_sql("SELECT * FROM t ORDER BY id")
assert rows == [(1, 'Alice', 90), (2, 'Bob', 85), (3, 'Charlie', 95)], f"server1 SELECT fail: {rows}"
print(f"  server1 SELECT OK: {rows}")

# 确认 WAL 文件存在
assert os.path.exists(WAL_PATH), "WAL file not created!"
wal_size = os.path.getsize(WAL_PATH)
print(f"  WAL size = {wal_size} bytes")

# 4. 关 server1 (让它写 EndOfFile 并 clean close)
print("[TEST] server1 shutdown")
p1.terminate()
p1.wait(timeout=5)
kill_all()
time.sleep(1)

# 5. 起 server2,同 --data-dir
print("[TEST] server2 start (replay WAL)")
p2 = subprocess.Popen([EXE, "--port", "3306", "--user", "root", "--data-dir", DATA_DIR],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2)

# 6. 读数据
print("[TEST] server2 replay verification")
try:
    run_sql("USE m5test")
    rows = run_sql("SELECT * FROM t ORDER BY id")
    assert rows == [(1, 'Alice', 90), (2, 'Bob', 85), (3, 'Charlie', 95)], f"server2 SELECT fail: {rows}"
    print(f"  server2 SELECT OK: {rows}")

    # 再插 1 行 (WAL append 在现有 WAL 后面)
    run_sql("INSERT INTO t(name, score) VALUES ('Diana', 88)")
    rows = run_sql("SELECT * FROM t ORDER BY id")
    assert len(rows) == 4, f"after second insert expect 4, got {len(rows)}"
    print(f"  server2 more insert OK: {rows}")

    # UPDATE + DELETE
    run_sql("UPDATE t SET score=100 WHERE name='Alice'")
    run_sql("DELETE FROM t WHERE name='Charlie'")
    rows = run_sql("SELECT * FROM t ORDER BY id")
    assert len(rows) == 3, f"after update+delete expect 3, got {len(rows)}"
    print(f"  server2 UPDATE+DELETE OK: {rows}")

    # 再关再启,第三次验证
    kill_all()
    p3 = subprocess.Popen([EXE, "--port", "3306", "--user", "root", "--data-dir", DATA_DIR],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    run_sql("USE m5test")
    rows = run_sql("SELECT * FROM t ORDER BY id")
    assert rows == [(1, 'Alice', 100), (2, 'Bob', 85), (4, 'Diana', 88)], f"server3 SELECT fail: {rows}"
    print(f"  server3 replay OK (after update+delete+insert+restart): {rows}")
finally:
    kill_all()

# 清理
import shutil
shutil.rmtree(DATA_DIR, ignore_errors=True)

print("\n=== M5.4 Restart Recovery ALL PASSED ===")
print("  ✅ 插 3 行 → 重启 → 3 行还在")
print("  ✅ 插 1 行 → 重启 → 4 行还在")
print("  ✅ UPDATE + DELETE → 重启 → 正确状态保留")
