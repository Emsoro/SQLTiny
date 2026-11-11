"""
Test 3: **多线程并发读写** (单 server 进程,多 PyMySQL 线程)
避免多进程各自开 WAL writer 互相覆盖 — SQLTiny server 本身是多线程处理每个连接。

5 线程:
  - 2 writer: INSERT 新行 + 偶尔 UPDATE
  - 2 reader: SELECT COUNT(*) + LEFT JOIN
  - 1 deleter: 随机 DELETE

跑 15 秒,主进程验证最终状态可查询。
"""
import threading as mt
import pymysql, time, random, os

DB_NAME = "ctest"
STOP = mt.Event()
COUNTER = 0
LOCK = mt.Lock()

def connect():
    return pymysql.connect(host="127.0.0.1", port=3306, user="root",
                           charset="utf8mb4", autocommit=True)

def writer(tid):
    global COUNTER
    conn = connect(); cur = conn.cursor()
    while not STOP.is_set():
        try:
            name = f"t{tid}_u_{random.randint(1,999)}"
            cur.execute(f"INSERT INTO {DB_NAME}.users(name,age,dept_id) VALUES ({name!r},{random.randint(18,70)},{random.randint(1,3)})")
            if random.random() < 0.1:  # 10% 概率 UPDATE 一行
                cur.execute(f"UPDATE {DB_NAME}.users SET age=age+1 WHERE id = {random.randint(1,100000)}")
            with LOCK: COUNTER += 1
        except Exception: pass
        time.sleep(random.uniform(0.002, 0.008))
    conn.close()

def reader(tid):
    conn = connect(); cur = conn.cursor()
    while not STOP.is_set():
        try:
            cur.execute(f"SELECT COUNT(*) FROM {DB_NAME}.users")
            cur.fetchone()
            cur.execute(f"SELECT u.name, o.total FROM {DB_NAME}.users u LEFT JOIN {DB_NAME}.orders o ON o.user_id = u.id LIMIT 10")
            cur.fetchall()
        except Exception: pass
        time.sleep(random.uniform(0.001, 0.005))
    conn.close()

def deleter(tid):
    conn = connect(); cur = conn.cursor()
    while not STOP.is_set():
        try:
            cur.execute(f"SELECT id FROM {DB_NAME}.users ORDER BY id DESC LIMIT 1")
            r = cur.fetchone()
            if r and r[0] > 100:  # 保留种子 100 行
                cur.execute(f"DELETE FROM {DB_NAME}.users WHERE id = {r[0]}")
        except Exception: pass
        time.sleep(random.uniform(0.01, 0.03))
    conn.close()

def main():
    print("=" * 60)
    print("  Test 3: 多线程并发读写 (PyMySQL 线程)")
    print("=" * 60)

    conn = connect(); cur = conn.cursor()
    cur.execute(f"DROP DATABASE IF EXISTS {DB_NAME}")
    cur.execute(f"CREATE DATABASE {DB_NAME}")
    cur.execute(f"""CREATE TABLE {DB_NAME}.users (
        id BIGINT PRIMARY KEY AUTO_INCREMENT,
        name VARCHAR(32), age INT, dept_id INT
    )""")
    cur.execute(f"""CREATE TABLE {DB_NAME}.orders (
        id BIGINT PRIMARY KEY AUTO_INCREMENT,
        user_id INT, total DOUBLE, status VARCHAR(16)
    )""")
    # 种子数据
    for i in range(100):
        cur.execute(f"INSERT INTO {DB_NAME}.users(name,age,dept_id) VALUES ('seed_{i}',{20+i},{i%3+1})")
    conn.close()
    print("  Schema + 100 种子 OK")

    threads = []
    for i in range(2): threads.append(mt.Thread(target=writer, args=(i,)))
    for i in range(2): threads.append(mt.Thread(target=reader, args=(i,)))
    threads.append(mt.Thread(target=deleter, args=(0,)))

    print(f"  启动 {len(threads)} 线程 (2W+2R+1D)...")
    t0 = time.time()
    for t in threads: t.start()

    time.sleep(15)
    STOP.set()
    for t in threads: t.join(timeout=5)
    t1 = time.time()

    # 最终验证
    conn = connect(); cur = conn.cursor()
    cur.execute(f"SELECT COUNT(*) FROM {DB_NAME}.users")
    n_users = cur.fetchone()[0]
    cur.execute(f"SELECT MIN(id), MAX(id) FROM {DB_NAME}.users")
    mn, mx = cur.fetchone()
    # 完整 SQL 扫描验证:能跑完整 SELECT * 不崩
    cur.execute(f"SELECT * FROM {DB_NAME}.users LIMIT 50")
    cur.fetchall()
    # LEFT JOIN 不崩
    cur.execute(f"SELECT u.id, u.name, o.id FROM {DB_NAME}.users u LEFT JOIN {DB_NAME}.orders o ON o.user_id = u.id")
    cur.fetchall()
    conn.close()

    print(f"\n  总耗时: {t1-t0:.2f}s")
    print(f"  成功 INSERT+UPDATE: {COUNTER}")
    print(f"  最终 users: {n_users} 行 (种子 100 + 写 - 删)")
    print(f"  id range: [{mn}, {mx}]")
    print(f"  吞吐: {COUNTER/(t1-t0):.0f} ops/s")

    print("\n  ✅ 并发测试通过 — 最终状态完整可查询")

if __name__ == "__main__":
    main()
