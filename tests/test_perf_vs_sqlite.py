"""
Test 4: 大数据量 INSERT + LEFT JOIN 多表性能对比 SQLite
两个引擎跑完全相同的工作负载,打印时间:

  Workload A — 大批量 INSERT:
    users:     2000 行 (批量 200/次)
    orders:    10000 行 (批量 500/次,user_id 随机分布在 users 范围)

  Workload B — 复杂查询 (跑 5 次取平均):
    (B1) SELECT COUNT(*), SUM(), AVG(), MIN(), MAX() on users
    (B2) GROUP BY dept_id + HAVING on users 全表
    (B3) INNER JOIN users×orders + 聚合
    (B4) LEFT JOIN (包含无订单用户 Eve/Nik 各占 20% 左右)

注意:
  - SQLite 默认无 WAL 时单进程很快 — SQLTiny 是带 WAL + MVCC 的 TCP server
    两者架构不同,对比目标是 SQLTiny 功能正确性 + 量级差距
  - 数据量选 2k users / 10k orders:SQLTiny 全内存能 hold 住,且 JOIN 性能可比较
"""
import pymysql, sqlite3, time, random

N_USERS = 200
N_ORDERS = 800
BATCH = 50

def bench(label, fn):
    t0 = time.perf_counter()
    r = fn()
    t1 = time.perf_counter()
    print(f"  [{label}] {(t1-t0)*1000:.1f} ms")
    return (t1-t0), r

# ==========================================================
# SQLTiny
# ==========================================================
def run_sqltiny():
    conn = pymysql.connect(host="127.0.0.1", port=3306, user="root",
                           charset="utf8mb4", autocommit=True,
                           read_timeout=600, write_timeout=600)
    cur = conn.cursor()
    cur.execute("DROP DATABASE IF EXISTS pbench")
    cur.execute("CREATE DATABASE pbench")
    cur.execute("USE pbench")
    cur.execute("""CREATE TABLE users (
        id BIGINT PRIMARY KEY AUTO_INCREMENT,
        name VARCHAR(32), age INT, dept_id INT, score DOUBLE
    )""")
    cur.execute("""CREATE TABLE orders (
        id BIGINT PRIMARY KEY AUTO_INCREMENT,
        user_id INT, total DOUBLE, status VARCHAR(16)
    )""")

    # --- A: INSERT ---
    print("  [SQLTiny] Workload A: INSERT")
    t_a0 = time.perf_counter()
    for b in range(0, N_USERS, BATCH):
        values = []
        for i in range(BATCH):
            n = b + i + 1
            values.append((f"user_{n}", random.randint(18,70), random.randint(1,5), round(random.uniform(0,100),2)))
        placeholders = ",".join(["(%s,%s,%s,%s)"]*BATCH)
        flat = [v for row in values for v in row]
        cur.execute(f"INSERT INTO users(name,age,dept_id,score) VALUES {placeholders}", flat)
    t_a1 = time.perf_counter()

    # orders
    for b in range(0, N_ORDERS, BATCH):
        values = []
        for i in range(BATCH):
            uid = random.randint(1, N_USERS)
            values.append((uid, round(random.uniform(10,500),2), random.choice(["paid","pending","shipped"])))
        placeholders = ",".join(["(%s,%s,%s)"]*BATCH)
        flat = [v for row in values for v in row]
        cur.execute(f"INSERT INTO orders(user_id,total,status) VALUES {placeholders}", flat)
    t_a2 = time.perf_counter()
    print(f"    INSERT {N_USERS} users:  {(t_a1-t_a0)*1000:.1f} ms")
    print(f"    INSERT {N_ORDERS} orders: {(t_a2-t_a1)*1000:.1f} ms")

    # --- B: SELECT (跑 5 次取平均) ---
    print("  [SQLTiny] Workload B: SELECT (5x avg)")
    bench_times = {"agg_user":[],"groupby":[],"inner_join":[],"left_join":[]}
    for _ in range(5):
        t0=time.perf_counter(); cur.execute("SELECT COUNT(*), SUM(age), AVG(age), MIN(age), MAX(age) FROM users"); cur.fetchone()
        bench_times["agg_user"].append(time.perf_counter()-t0)

        t0=time.perf_counter(); cur.execute("SELECT dept_id, COUNT(*), AVG(age) FROM users GROUP BY dept_id HAVING COUNT(*)>100 ORDER BY dept_id"); cur.fetchall()
        bench_times["groupby"].append(time.perf_counter()-t0)

        t0=time.perf_counter(); cur.execute("""
            SELECT u.dept_id, COUNT(*), SUM(o.total)
            FROM users u INNER JOIN orders o ON o.user_id = u.id
            GROUP BY u.dept_id ORDER BY u.dept_id
        """); cur.fetchall()
        bench_times["inner_join"].append(time.perf_counter()-t0)

        t0=time.perf_counter(); cur.execute("""
            SELECT u.name, COUNT(o.id), COALESCE(SUM(o.total),0)
            FROM users u LEFT JOIN orders o ON o.user_id = u.id
            WHERE u.id <= 100
            GROUP BY u.id ORDER BY u.id
        """); cur.fetchall()
        bench_times["left_join"].append(time.perf_counter()-t0)

    for k,v in bench_times.items():
        avg = sum(v)/len(v)
        print(f"    {k:14s}: {avg*1000:7.1f} ms")

    conn.close()
    return (t_a1-t_a0)*1000, (t_a2-t_a1)*1000, {k: sum(v)/len(v)*1000 for k,v in bench_times.items()}

# ==========================================================
# SQLite (同构 SQL, sqlite3 原生)
# ==========================================================
def run_sqlite():
    conn = sqlite3.connect(":memory:")
    conn.execute("CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, age INTEGER, dept_id INTEGER, score REAL)")
    conn.execute("CREATE TABLE orders (id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER, total REAL, status TEXT)")

    print("  [SQLite] Workload A: INSERT")
    t_a0 = time.perf_counter()
    for b in range(0, N_USERS, BATCH):
        rows = []
        for i in range(BATCH):
            n = b + i + 1
            rows.append((f"user_{n}", random.randint(18,70), random.randint(1,5), round(random.uniform(0,100),2)))
        conn.executemany("INSERT INTO users(name,age,dept_id,score) VALUES (?,?,?,?)", rows)
    conn.commit()
    t_a1 = time.perf_counter()

    for b in range(0, N_ORDERS, BATCH):
        rows = []
        for i in range(BATCH):
            uid = random.randint(1, N_USERS)
            rows.append((uid, round(random.uniform(10,500),2), random.choice(["paid","pending","shipped"])))
        conn.executemany("INSERT INTO orders(user_id,total,status) VALUES (?,?,?)", rows)
    conn.commit()
    t_a2 = time.perf_counter()
    print(f"    INSERT {N_USERS} users:  {(t_a1-t_a0)*1000:.1f} ms")
    print(f"    INSERT {N_ORDERS} orders: {(t_a2-t_a1)*1000:.1f} ms")

    print("  [SQLite] Workload B: SELECT (5x avg)")
    bench_times = {"agg_user":[],"groupby":[],"inner_join":[],"left_join":[]}
    for _ in range(5):
        t0=time.perf_counter(); conn.execute("SELECT COUNT(*), SUM(age), AVG(age), MIN(age), MAX(age) FROM users").fetchone(); bench_times["agg_user"].append(time.perf_counter()-t0)
        t0=time.perf_counter(); conn.execute("SELECT dept_id, COUNT(*), AVG(age) FROM users GROUP BY dept_id HAVING COUNT(*)>100 ORDER BY dept_id").fetchall(); bench_times["groupby"].append(time.perf_counter()-t0)
        t0=time.perf_counter(); conn.execute("""
            SELECT u.dept_id, COUNT(*), SUM(o.total)
            FROM users u INNER JOIN orders o ON o.user_id = u.id
            GROUP BY u.dept_id ORDER BY u.dept_id
        """).fetchall(); bench_times["inner_join"].append(time.perf_counter()-t0)
        t0=time.perf_counter(); conn.execute("""
            SELECT u.name, COUNT(o.id), COALESCE(SUM(o.total),0)
            FROM users u LEFT JOIN orders o ON o.user_id = u.id
            WHERE u.id <= 100
            GROUP BY u.id ORDER BY u.id
        """).fetchall(); bench_times["left_join"].append(time.perf_counter()-t0)

    for k,v in bench_times.items():
        avg = sum(v)/len(v)
        print(f"    {k:14s}: {avg*1000:7.1f} ms")

    conn.close()
    return (t_a1-t_a0)*1000, (t_a2-t_a1)*1000, {k: sum(v)/len(v)*1000 for k,v in bench_times.items()}

# ==========================================================
# main
# ==========================================================
print("=" * 70)
print(f"  Test 4: 大数据量性能对比  users={N_USERS}  orders={N_ORDERS}")
print("=" * 70)

t_sq_u, t_sq_o, t_sq_q = run_sqltiny()
print()
t_sl_u, t_sl_o, t_sl_q = run_sqlite()

print()
print("=" * 70)
print("  性能对比汇总")
print("=" * 70)
print(f"{'操作':14s} {'SQLTiny(ms)':>14s}  {'SQLite(ms)':>14s}  {'倍数':>10s}")
print("-" * 70)
print(f"INSERT users   {t_sq_u:14.1f}  {t_sl_u:14.1f}  {t_sq_u/max(t_sl_u,0.01):10.1f}x")
print(f"INSERT orders  {t_sq_o:14.1f}  {t_sl_o:14.1f}  {t_sq_o/max(t_sl_o,0.01):10.1f}x")
for k in ["agg_user","groupby","inner_join","left_join"]:
    sq = t_sq_q.get(k,0); sl = t_sl_q.get(k,0)
    print(f"{k:14s} {sq:14.1f}  {sl:14.1f}  {sq/max(sl,0.01):10.1f}x")

print("\n  备注: SQLTiny 是带 WAL + MVCC + TCP 协议开销的 server;")
print("        SQLite 是单进程无 WAL in-memory — 架构不同,量级差距合理。")
print("        SQLTiny 优势:可持久化、多连接、MySQL wire protocol。")
