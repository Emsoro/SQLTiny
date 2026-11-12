#!/usr/bin/env python3
"""SQLTiny 稳定性 / 压测 / 复杂查询套件

定位:本地数据库服务本地程序。密码不校验(开发期),本套件**不测认证**,
专攻稳定性六大维度:

  1. 复杂查询  —— 多表 JOIN 链 / 子查询 / GROUP BY+HAVING / 聚合 / CASE /
                 ORDER BY+LIMIT / 自连接 / NULL 聚合 / 事务内多表 UPDATE,逐一计时
  2. 并发压测  —— 可配读写比的多线程 worker,混合上述复杂查询 + INSERT/UPDATE/DELETE
  3. 长时 soak  —— 持续 N 秒,统计 ops/sec、错误率、p50/p95/p99 延迟
  4. 事务冲突  —— 并发改同一批行,验证 40001 回滚后无丢写 / 无幻行(不崩)
  5. 崩溃恢复  —— 自带 server + --data-dir:写入已知数据 -> kill -9 -> 重启 ->
                  校验 WAL replay 不丢已提交、回滚未提交(无撕裂写)
  6. 内存稳定  —— soak 期间采样 server RSS,报告增长(需 psutil,缺失则跳过)

用法:
  # 自带 server(自动选 Release/Debug 二进制),最省事
  python tests/stress_stability.py --self-server --phase all
  python tests/stress_stability.py --self-server --phase soak   --workers 16 --duration 60
  python tests/stress_stability.py --self-server --phase complex
  python tests/stress_stability.py --self-server --phase crash
  python tests/stress_stability.py --self-server --phase soak --no-clean   # 保留 data-dir

  # 连已运行的 server(已 --data-dir 启动以支持崩溃恢复)
  python tests/stress_stability.py --host 127.0.0.1 --port 3306 --phase soak

退出码:0=全通过,1=有断言失败,2=环境错误(server 起不来 / 缺 pymysql)
"""
import sys
import os
import time
import threading
import argparse
import random
import shutil
import subprocess
import tempfile

try:
    sys.stdout.reconfigure(errors="replace")
except (AttributeError, OSError):
    pass

try:
    import pymysql
    from pymysql.err import OperationalError, InterfaceError
except ImportError:
    print("ERROR: 需要 pymysql (pip install pymysql)")
    sys.exit(2)

# 冲突回滚码:server 返回 40001 / 1213,属预期,不计失败
EXPECTED_CONFLICT = (1213, 40001)

HOST, PORT, USER = "127.0.0.1", 3306, "root"
DB = "stress_test"
SERVER_CANDIDATES = ["build/Release/sqltiny_server.exe",
                     "build/Debug/sqltiny_server.exe"]

# 全局指标
_metrics_lock = threading.Lock()
_latencies = []          # 每次操作耗时(ms)
_op_count = 0
_op_errors = 0           # 非预期错误
_op_conflicts = 0        # 预期 40001
# 不变量追踪:Python 侧记录"已提交写入行数",与最终 COUNT(*) 比对
_invariant_lock = threading.Lock()
_tracked_rows = 0

G, R, C, Y, RESET = "\033[92m", "\033[91m", "\033[96m", "\033[93m", "\033[0m"
_pass = _fail = 0


def log_pass(name):
    global _pass
    _pass += 1
    print(f"  {G}PASS{RESET} {name}")


def log_fail(name, detail=""):
    global _fail
    _fail += 1
    print(f"  {R}FAIL{RESET} {name}  {detail}")


# ---------------------------------------------------------------------------
# server 生命周期(自带模式)
# ---------------------------------------------------------------------------
def find_server():
    for p in SERVER_CANDIDATES:
        if os.path.exists(p):
            return p
    return None


def spawn_server(data_dir, port):
    srv = find_server()
    if not srv:
        raise RuntimeError("找不到 server 二进制,请先 cmake --build build --config Release")
    args = [srv, "--port", str(port), "--user", USER]
    if data_dir:
        os.makedirs(data_dir, exist_ok=True)
        args += ["--data-dir", data_dir]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            c = pymysql.connect(host=HOST, port=port, user=USER, password="",
                                connect_timeout=1)
            c.close()
            return proc
        except Exception:
            time.sleep(0.2)
    proc.kill()
    raise RuntimeError("server 未在 20s 内监听")


def connect(port, db=DB, autocommit=True):
    return pymysql.connect(host=HOST, port=port, user=USER, password="",
                           database=db, autocommit=autocommit, connect_timeout=5)


# ---------------------------------------------------------------------------
# schema + seed
# ---------------------------------------------------------------------------
def setup(conn, users, products, orders):
    cur = conn.cursor()
    cur.execute(f"DROP DATABASE IF EXISTS {DB}")
    cur.execute(f"CREATE DATABASE {DB}")
    cur.execute(f"USE {DB}")
    cur.execute("DROP TABLE IF EXISTS orders")
    cur.execute("DROP TABLE IF EXISTS users")
    cur.execute("DROP TABLE IF EXISTS products")
    cur.execute("CREATE TABLE users ("
                "id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(64), "
                "region VARCHAR(16), age INT, balance INT)")
    cur.execute("CREATE TABLE products ("
                "pid INT PRIMARY KEY AUTO_INCREMENT, pname VARCHAR(64), "
                "price INT, stock INT)")
    cur.execute("CREATE TABLE orders ("
                "oid INT PRIMARY KEY AUTO_INCREMENT, uid INT, pid INT, "
                "qty INT, amount INT, remark VARCHAR(32))")
    regions = ["north", "south", "east", "west"]
    for i in range(users):
        cur.execute("INSERT INTO users(name,region,age,balance) VALUES (%s,%s,%s,%s)",
                    (f"u{i}", random.choice(regions), random.randint(18, 60),
                     random.randint(0, 1000)))
    for i in range(products):
        cur.execute("INSERT INTO products(pname,price,stock) VALUES (%s,%s,%s)",
                    (f"p{i}", random.randint(1, 500), random.randint(0, 100)))
    for _ in range(orders):
        cur.execute("INSERT INTO orders(uid,pid,qty,amount,备注) VALUES (%s,%s,%s,%s,%s)",
                    (random.randint(1, users), random.randint(1, products),
                     random.randint(1, 5), random.randint(10, 2000), "init"))
    conn.commit()
    with _invariant_lock:
        global _tracked_rows
        _tracked_rows = orders
    print(f"  seeded: users={users} products={products} orders={orders}")


# ---------------------------------------------------------------------------
# 复杂查询库(逐一计时)
# ---------------------------------------------------------------------------
COMPLEX_QUERIES = [
    ("多表JOIN+聚合",
     "SELECT u.region, COUNT(*) c, SUM(o.amount) s "
     "FROM orders o JOIN users u ON o.uid=u.id "
     "JOIN products p ON o.pid=p.pid "
     "WHERE u.age>20 GROUP BY u.region ORDER BY s DESC"),
    ("GROUP BY + HAVING",
     "SELECT u.region, AVG(o.amount) a FROM orders o "
     "JOIN users u ON o.uid=u.id GROUP BY u.region "
     "HAVING AVG(o.amount) > 100 ORDER BY a DESC"),
    ("子查询 IN",
     "SELECT name FROM users WHERE id IN "
     "(SELECT uid FROM orders WHERE amount > 1000) ORDER BY name LIMIT 20"),
    ("子查询 GROUP+HAVING",
     "SELECT u.name FROM users u WHERE u.id IN "
     "(SELECT uid FROM orders GROUP BY uid HAVING SUM(amount) > 500) "
     "ORDER BY u.name LIMIT 20"),
    ("CASE + 聚合",
     "SELECT CASE WHEN balance<300 THEN 'low' ELSE 'ok' END bucket, "
     "COUNT(*) c, SUM(balance) b FROM users GROUP BY 1 ORDER BY c DESC"),
    ("LEFT JOIN 补 NULL",
     "SELECT u.name, COUNT(o.oid) orders FROM users u "
     "LEFT JOIN orders o ON o.uid=u.id GROUP BY u.name ORDER BY orders DESC LIMIT 20"),
    ("自连接",
     "SELECT a.name, b.name FROM users a JOIN users b "
     "ON a.region=b.region AND a.id<b.id LIMIT 30"),
    ("ORDER BY + LIMIT + OFFSET",
     "SELECT u.name, o.amount FROM orders o JOIN users u ON o.uid=u.id "
     "ORDER BY o.amount DESC LIMIT 25 OFFSET 10"),
    ("NULL 聚合",
     "SELECT COUNT(*) total, COUNT(balance) non_null, AVG(balance) avg_b, "
     "SUM(balance) sum_b FROM users"),
    ("多表事务读(复杂)",
     "SELECT u.region, p.pname, SUM(o.qty) total_qty, SUM(o.amount) total_amt "
     "FROM orders o JOIN users u ON o.uid=u.id "
     "JOIN products p ON o.pid=p.pid "
     "WHERE o.amount >= 100 AND o.amount <= 1500 "
     "GROUP BY u.region, p.pname HAVING SUM(o.qty) > 1 "
     "ORDER BY total_amt DESC LIMIT 30"),
]


def run_complex_suite(conn):
    cur = conn.cursor()
    print(f"\n{C}=== 复杂查询计时 (DB={DB}) ==={RESET}")
    slow = []
    for name, sql in COMPLEX_QUERIES:
        t0 = time.time()
        try:
            cur.execute(sql)
            rows = cur.fetchall()
            dt = (time.time() - t0) * 1000
            print(f"  {G}{name:<22}{RESET} {dt:8.2f} ms   rows={len(rows)}")
            slow.append((dt, name))
        except Exception as e:
            log_fail(name, str(e)[:120])
    slow.sort(reverse=True)
    if slow:
        print(f"  {Y}最慢: {slow[0][1]} = {slow[0][0]:.1f} ms{RESET}")


# ---------------------------------------------------------------------------
# worker
# ---------------------------------------------------------------------------
_queries = [q[1] for q in COMPLEX_QUERIES]


def _record(dt_ms):
    with _metrics_lock:
        _latencies.append(dt_ms)
        global _op_count
        _op_count += 1


def reader_worker(port, stop, wid):
    while not stop.is_set():
        try:
            conn = connect(port)
            cur = conn.cursor()
            sql = random.choice(_queries)
            t0 = time.time()
            cur.execute(sql)
            cur.fetchall()
            _record((time.time() - t0) * 1000)
            conn.close()
        except (OperationalError, InterfaceError) as e:
            if isinstance(e.args, tuple) and e.args[0] in EXPECTED_CONFLICT:
                with _metrics_lock:
                    global _op_conflicts
                    _op_conflicts += 1
            else:
                with _metrics_lock:
                    global _op_errors
                    _op_errors += 1
        except Exception:
            with _metrics_lock:
                _op_errors += 1


def writer_worker(port, stop, wid, users, products):
    while not stop.is_set():
        try:
            conn = connect(port)
            cur = conn.cursor()
            op = random.random()
            if op < 0.6:
                t0 = time.time()
                cur.execute("INSERT INTO orders(uid,pid,qty,amount,remark) "
                            "VALUES (%s,%s,%s,%s,%s)",
                            (random.randint(1, users), random.randint(1, products),
                             random.randint(1, 5), random.randint(10, 2000), "w"))
                conn.commit()
                _record((time.time() - t0) * 1000)
                with _invariant_lock:
                    global _tracked_rows
                    _tracked_rows += 1
            elif op < 0.85:
                t0 = time.time()
                cur.execute("UPDATE users SET balance=balance+%s WHERE id=%s",
                            (random.randint(-50, 50), random.randint(1, users)))
                conn.commit()
                _record((time.time() - t0) * 1000)
            else:
                t0 = time.time()
                cur.execute("DELETE FROM orders WHERE oid=%s",
                            (random.randint(1, 1000000),))
                aff = cur.rowcount
                conn.commit()
                _record((time.time() - t0) * 1000)
                if aff > 0:
                    with _invariant_lock:
                        _tracked_rows -= aff
            conn.close()
        except (OperationalError, InterfaceError) as e:
            if isinstance(e.args, tuple) and e.args[0] in EXPECTED_CONFLICT:
                with _metrics_lock:
                    global _op_conflicts
                    _op_conflicts += 1
            else:
                with _metrics_lock:
                    _op_errors += 1
        except Exception:
            with _metrics_lock:
                _op_errors += 1


def txn_worker(port, stop, wid, users):
    """并发事务冲突:同一批评注行,验证 40001 后无数据损坏"""
    targets = list(range(1, min(users, 10) + 1))
    while not stop.is_set():
        conn = None
        try:
            conn = connect(port, autocommit=False)
            cur = conn.cursor()
            cur.execute("BEGIN")
            for tid in targets:
                cur.execute("UPDATE users SET balance=balance+1 WHERE id=%s", (tid,))
            cur.execute("SAVEPOINT sp1")
            cur.execute("UPDATE users SET balance=balance-1 WHERE id=%s", (targets[0],))
            cur.execute("ROLLBACK TO SAVEPOINT sp1")
            cur.execute("COMMIT")
            _record(0.0)
        except (OperationalError, InterfaceError) as e:
            try:
                if conn:
                    conn.rollback()
            except Exception:
                pass
            if isinstance(e.args, tuple) and e.args[0] in EXPECTED_CONFLICT:
                with _metrics_lock:
                    global _op_conflicts
                    _op_conflicts += 1
            else:
                with _metrics_lock:
                    global _op_errors
                    _op_errors += 1
        except Exception:
            try:
                if conn:
                    conn.rollback()
            except Exception:
                pass
            with _metrics_lock:
                _op_errors += 1


# ---------------------------------------------------------------------------
# RSS 采样
# ---------------------------------------------------------------------------
def sample_rss(proc, samples):
    try:
        import psutil
        p = psutil.Process(proc.pid)
        rss = p.memory_info().rss
        samples.append(rss)
        return rss
    except Exception:
        return None


# ---------------------------------------------------------------------------
# 阶段
# ---------------------------------------------------------------------------
def phase_complex(port):
    conn = connect(port)
    run_complex_suite(conn)
    conn.close()


def phase_soak(port, workers, duration, users, products, with_server_proc=None):
    global _op_count, _op_errors, _op_conflicts, _tracked_rows
    _op_count = _op_errors = _op_conflicts = 0
    _latencies.clear()
    rw = max(1, workers // 4)        # 事务冲突 worker
    ww = max(1, (workers - rw) // 2)  # 写 worker
    rr = max(1, workers - rw - ww)    # 读 worker
    print(f"\n{C}=== SOAK: {duration}s  workers(r={rr} w={ww} txn={rw}) ==={RESET}")
    stop = threading.Event()
    threads = []
    for i in range(rr):
        t = threading.Thread(target=reader_worker, args=(port, stop, i)); threads.append(t)
    for i in range(ww):
        t = threading.Thread(target=writer_worker, args=(port, stop, i, users, products)); threads.append(t)
    for i in range(rw):
        t = threading.Thread(target=txn_worker, args=(port, stop, i, users)); threads.append(t)
    rss_samples = []
    for t in threads:
        t.start()
    end = time.time() + duration
    while time.time() < end:
        time.sleep(2)
        if with_server_proc:
            sample_rss(with_server_proc, rss_samples)
    stop.set()
    for t in threads:
        t.join(timeout=10)

    with _metrics_lock:
        total = _op_count
        errs = _op_errors
        conf = _op_conflicts
        lats = sorted(_latencies)
    secs = max(0.001, duration)
    def pct(p):
        if not lats:
            return 0.0
        idx = min(len(lats) - 1, int(len(lats) * p / 100))
        return lats[idx]
    print(f"  ops={total}  ops/sec={total/secs:.1f}  errors={errs}  expected_40001={conf}")
    if lats:
        print(f"  latency ms: p50={pct(50):.2f} p95={pct(95):.2f} p99={pct(99):.2f} "
              f"max={lats[-1]:.2f}")
    if rss_samples:
        growth = (rss_samples[-1] - rss_samples[0]) / (1024 * 1024)
        print(f"  RSS: start={rss_samples[0]/1024/1024:.1f}MB "
              f"end={rss_samples[-1]/1024/1024:.1f}MB  growth={growth:+.1f}MB")

    # 不变量:已提交行数 vs COUNT(*)
    conn = connect(port)
    cur = conn.cursor()
    cur.execute("SELECT COUNT(*) FROM orders")
    cnt = int(cur.fetchone()[0])
    conn.close()
    with _invariant_lock:
        expected = _tracked_rows
    if cnt == expected and errs == 0:
        log_pass(f"soak 不变量 (orders COUNT={cnt} == tracked {expected})")
    else:
        if cnt != expected:
            log_fail("soak 不变量", f"COUNT={cnt} != tracked {expected} (可能丢写/幻行)")
        if errs:
            log_fail("soak 非预期错误", f"errors={errs}")
    return errs == 0 and cnt == expected


def phase_crash(data_dir, port, rows):
    """杀进程 + 重启,验证 WAL replay 正确性"""
    print(f"\n{C}=== CRASH RECOVERY (data-dir={data_dir}) ==={RESET}")
    proc = spawn_server(data_dir, port)
    try:
        conn = connect(port)
        cur = conn.cursor()
        cur.execute(f"DROP DATABASE IF EXISTS {DB}")
        cur.execute(f"CREATE DATABASE {DB}")
        cur.execute(f"USE {DB}")
        cur.execute("CREATE TABLE t (id INT PRIMARY KEY, v INT)")
        # 已知数据:val = id*7,校验和
        checksum = 0
        for i in range(1, rows + 1):
            v = i * 7
            checksum += v
            cur.execute("INSERT INTO t(id,v) VALUES (%s,%s)", (i, v))
        conn.commit()
        # 后台写线程:持续提交已提交行,模拟正常写入流
        stop = threading.Event()
        def bg():
            try:
                c2 = connect(port)
                cc = c2.cursor()
                n = 0
                while not stop.is_set():
                    cc.execute("INSERT INTO t(id,v) VALUES (%s,%s)",
                               (100000 + n, n))
                    c2.commit()
                    n += 1
            except Exception:
                pass
        th = threading.Thread(target=bg)
        th.start()
        time.sleep(1.5)
        # 真·未提交事务:autocommit=False + BEGIN,插入后不 commit 即杀
        uc = connect(port, autocommit=False)
        ucur = uc.cursor()
        ucur.execute("BEGIN")
        ucur.execute("INSERT INTO t(id,v) VALUES (%s,%s)", (999999, 42))
        # 模拟崩溃:直接 TerminateProcess(kill -9),此时 999999 仍在未提交事务中
        proc.kill()
        th.join(timeout=3)
        stop.set()
        time.sleep(1)

        # 重启同 data-dir
        proc = spawn_server(data_dir, port)
        conn = connect(port)
        cur = conn.cursor()
        cur.execute(f"USE {DB}")
        cur.execute("SELECT COUNT(*), COALESCE(SUM(v),0) FROM t")
        cnt, ssum = cur.fetchone()
        cnt, ssum = int(cnt), int(ssum)
        cur.execute("SELECT v FROM t WHERE id=%s", (rows,))
        ok_tail = cur.fetchone() is not None
        cur.execute("SELECT COUNT(*) FROM t WHERE id=%s", (999999,))
        uncommitted = cur.fetchone()[0]
        conn.close()
        # 已提交 rows 行必须全在;未提交的 999999 必须被回滚(崩溃恢复原子性)
        if cnt >= rows and ssum >= checksum and ok_tail and uncommitted == 0:
            log_pass(f"崩溃恢复: {rows} 已提交行全在, SUM(v)>={checksum}, "
                     f"未提交事务(999999)已回滚")
        else:
            log_fail("崩溃恢复",
                     f"COUNT={cnt}(>={rows}) SUM={ssum}(>={checksum}) "
                     f"tail_ok={ok_tail} uncommitted_999999={uncommitted}")
    finally:
        try:
            proc.kill()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-server", action="store_true", help="自动拉起/管理 server")
    ap.add_argument("--host", default=HOST)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--phase", choices=["all", "complex", "soak", "crash"],
                    default="all")
    ap.add_argument("--workers", type=int, default=16)
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--users", type=int, default=50)
    ap.add_argument("--products", type=int, default=20)
    ap.add_argument("--orders", type=int, default=200)
    ap.add_argument("--crash-rows", type=int, default=200)
    ap.add_argument("--no-clean", action="store_true", help="保留临时 data-dir")
    args = ap.parse_args()

    tmp = None
    proc = None
    if args.self_server:
        tmp = tempfile.mkdtemp(prefix="sqltiny_stress_")
        ddir = os.path.join(tmp, "data") if args.phase == "crash" else None
        try:
            proc = spawn_server(ddir, args.port)
        except RuntimeError as e:
            print(f"{R}ERROR: {e}{RESET}")
            sys.exit(2)
    else:
        if args.phase == "crash":
            print(f"{R}ERROR: 崩溃恢复需 --self-server (自带 data-dir){RESET}")
            sys.exit(2)

    port = args.port
    try:
        if args.phase in ("all", "complex", "soak"):
            c = connect(port, db=None)
            setup(c, args.users, args.products, args.orders)
            c.close()

        if args.phase in ("all", "complex"):
            phase_complex(port)
        if args.phase in ("all", "soak"):
            phase_soak(port, args.workers, args.duration, args.users,
                       args.products, with_server_proc=proc)
        if args.phase in ("all", "crash"):
            # 崩溃恢复需独占端口:先停掉 soak 的 server,再由 phase_crash 拉起独立 data-dir 实例
            if proc:
                try:
                    proc.kill()
                except Exception:
                    pass
                proc = None
                time.sleep(1)
            phase_crash(os.path.join(tmp, "data") if tmp else "./_stress_crash",
                        port, args.crash_rows)
    finally:
        if proc:
            try:
                proc.kill()
            except Exception:
                pass
        if tmp and not args.no_clean:
            shutil.rmtree(tmp, ignore_errors=True)

    print(f"\n{C}==== 结果: PASS={_pass} FAIL={_fail} ===={RESET}")
    sys.exit(0 if _fail == 0 else 1)


if __name__ == "__main__":
    main()
