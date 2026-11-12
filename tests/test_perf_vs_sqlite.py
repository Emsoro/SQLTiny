"""
SQLTiny vs SQLite 性能对比测试 (Release 版)

两种运行模式:
  --mode single  单进程: 一个进程内依次跑 SQLTiny 与 SQLite 的同一工作负载
  --mode multi   多进程: N 个 worker 并发,每个 worker 用独立库跑同一工作负载,
                 用于观察并发度提升时两引擎的吞吐变化
  --mode both    两种都跑(默认)

工作负载(两引擎完全同构,同 seed 生成同样数据):
  A. 批量 INSERT: users / orders,分 BATCH 行一次提交
  B. 查询各跑 --repeats 次取平均:
     B1 聚合 COUNT/SUM/AVG/MIN/MAX
     B2 GROUP BY + HAVING
     B3 INNER JOIN + 聚合
     B4 LEFT JOIN + COALESCE

  - SQLTiny 用 build/Release/sqltiny_server.exe,脚本自动在 --port 上启停一个
    专用 server(内存模式,避免与开发用 3306 冲突);--reuse-server 可复用已有实例
  - SQLite 每个 worker 用独立 :memory: 库
  - 架构差异: SQLTiny 是带 WAL/MVCC 的 TCP server,SQLite 是进程内库,
    对比目的是量级与并发扩展性,不代表同架构下的绝对优劣
  - 结果保存到 benchmark/:每次生成带时间戳的 JSON + CSV,并追加到 history.csv,
    同时更新 latest.json

示例:
  python tests/test_perf_vs_sqlite.py --mode both
  python tests/test_perf_vs_sqlite.py --mode multi --workers 8 --users 500 --orders 2000
  python tests/test_perf_vs_sqlite.py --reuse-server --port 3306
"""
import argparse
import csv
import json
import multiprocessing as mp
import platform
import random
import socket
import sqlite3
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

import pymysql

sys.stdout.reconfigure(errors="replace")

REPO = Path(__file__).resolve().parent.parent
DEFAULT_SERVER = REPO / "build" / "Release" / "sqltiny_server.exe"
DEFAULT_OUT = REPO / "benchmark"
PHASES = ["insert_users", "insert_orders", "agg_user", "groupby", "inner_join", "left_join"]


# ------------------------------------------------------------------ server
def wait_port(port, timeout=10.0):
    deadline = time.perf_counter() + timeout
    while time.perf_counter() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), 0.2):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def start_server(exe, port, data_dir=None):
    if not Path(exe).is_file():
        raise FileNotFoundError(f"Release server 不存在: {exe} (先执行 cmake --build build --config Release)")
    if wait_port(port, timeout=0.4):
        raise RuntimeError(f"端口 {port} 已被占用,换 --port 或加 --reuse-server")
    args = [str(exe), "--port", str(port), "--user", "root"]
    if data_dir:
        args += ["--data-dir", str(data_dir)]
    kw = {}
    if platform.system() == "Windows":
        kw["creationflags"] = subprocess.CREATE_NO_WINDOW
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, **kw)
    if not wait_port(port):
        proc.terminate()
        raise RuntimeError(f"server 启动超时 (port={port})")
    return proc


# ------------------------------------------------------------------ 数据
def gen_rows(seed, n_users, n_orders, batch):
    rnd = random.Random(seed)
    users, orders = [], []
    for b in range(0, n_users, batch):
        rows = []
        for i in range(min(batch, n_users - b)):
            n = b + i + 1
            rows.append((f"user_{n}", rnd.randint(18, 70), rnd.randint(1, 5),
                         round(rnd.uniform(0, 100), 2)))
        users.append(rows)
    for b in range(0, n_orders, batch):
        rows = []
        for i in range(min(batch, n_orders - b)):
            rows.append((rnd.randint(1, n_users), round(rnd.uniform(10, 500), 2),
                         rnd.choice(["paid", "pending", "shipped"])))
        orders.append(rows)
    return users, orders


Q_AGG = "SELECT COUNT(*), SUM(age), AVG(age), MIN(age), MAX(age) FROM users"
Q_GROUP = ("SELECT dept_id, COUNT(*), AVG(age) FROM users "
           "GROUP BY dept_id HAVING COUNT(*)>10 ORDER BY dept_id")
Q_INNER = ("SELECT u.dept_id, COUNT(*), SUM(o.total) FROM users u "
           "INNER JOIN orders o ON o.user_id = u.id "
           "GROUP BY u.dept_id ORDER BY u.dept_id")
Q_LEFT = ("SELECT u.name, COUNT(o.id), COALESCE(SUM(o.total),0) FROM users u "
          "LEFT JOIN orders o ON o.user_id = u.id WHERE u.id <= 100 "
          "GROUP BY u.id ORDER BY u.id")


# ------------------------------------------------------------------ workload
def sqltiny_workload(wid, cfg):
    conn = pymysql.connect(host="127.0.0.1", port=cfg["port"], user="root",
                           charset="utf8mb4", autocommit=True,
                           read_timeout=600, write_timeout=600)
    cur = conn.cursor()
    db = f"pbench_{wid}"
    cur.execute(f"DROP DATABASE IF EXISTS {db}")
    cur.execute(f"CREATE DATABASE {db}")
    cur.execute(f"USE {db}")
    cur.execute("CREATE TABLE users (id BIGINT PRIMARY KEY AUTO_INCREMENT, "
                "name VARCHAR(32), age INT, dept_id INT, score DOUBLE)")
    cur.execute("CREATE TABLE orders (id BIGINT PRIMARY KEY AUTO_INCREMENT, "
                "user_id INT, total DOUBLE, status VARCHAR(16))")

    users, orders = gen_rows(cfg["seed"] + wid, cfg["users"], cfg["orders"], cfg["batch"])
    t = {}
    t0 = time.perf_counter()
    for rows in users:
        ph = ",".join(["(%s,%s,%s,%s)"] * len(rows))
        cur.execute(f"INSERT INTO users(name,age,dept_id,score) VALUES {ph}",
                    [v for row in rows for v in row])
    t["insert_users"] = (time.perf_counter() - t0) * 1000

    t0 = time.perf_counter()
    for rows in orders:
        ph = ",".join(["(%s,%s,%s)"] * len(rows))
        cur.execute(f"INSERT INTO orders(user_id,total,status) VALUES {ph}",
                    [v for row in rows for v in row])
    t["insert_orders"] = (time.perf_counter() - t0) * 1000

    for key, sql in [("agg_user", Q_AGG), ("groupby", Q_GROUP),
                     ("inner_join", Q_INNER), ("left_join", Q_LEFT)]:
        samples = []
        for _ in range(cfg["repeats"]):
            t0 = time.perf_counter()
            cur.execute(sql)
            cur.fetchall()
            samples.append((time.perf_counter() - t0) * 1000)
        t[key] = sum(samples) / len(samples)
    conn.close()
    return t


def sqlite_workload(wid, cfg):
    conn = sqlite3.connect(":memory:")
    conn.execute("CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                 "name TEXT, age INTEGER, dept_id INTEGER, score REAL)")
    conn.execute("CREATE TABLE orders (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                 "user_id INTEGER, total REAL, status TEXT)")
    users, orders = gen_rows(cfg["seed"] + wid, cfg["users"], cfg["orders"], cfg["batch"])
    t = {}
    t0 = time.perf_counter()
    for rows in users:
        conn.executemany("INSERT INTO users(name,age,dept_id,score) VALUES (?,?,?,?)", rows)
    conn.commit()
    t["insert_users"] = (time.perf_counter() - t0) * 1000

    t0 = time.perf_counter()
    for rows in orders:
        conn.executemany("INSERT INTO orders(user_id,total,status) VALUES (?,?,?)", rows)
    conn.commit()
    t["insert_orders"] = (time.perf_counter() - t0) * 1000

    for key, sql in [("agg_user", Q_AGG), ("groupby", Q_GROUP),
                     ("inner_join", Q_INNER), ("left_join", Q_LEFT)]:
        samples = []
        for _ in range(cfg["repeats"]):
            t0 = time.perf_counter()
            conn.execute(sql).fetchall()
            samples.append((time.perf_counter() - t0) * 1000)
        t[key] = sum(samples) / len(samples)
    conn.close()
    return t


def run_worker(task):
    """顶层函数,可被 multiprocessing pickle"""
    engine, wid, cfg = task
    try:
        t = sqltiny_workload(wid, cfg) if engine == "sqltiny" else sqlite_workload(wid, cfg)
        return {"engine": engine, "worker": wid, "timings_ms": t, "error": None}
    except Exception as e:
        return {"engine": engine, "worker": wid, "timings_ms": {}, "error": repr(e)}


# ------------------------------------------------------------------ 汇总 / 输出
def summarize(engine, runs, wall_ms, cfg):
    ok = [r for r in runs if not r["error"]]
    out = {"engine": engine, "workers": len(runs), "ok": len(ok), "wall_ms": round(wall_ms, 2)}
    for p in PHASES:
        vals = [r["timings_ms"][p] for r in ok if p in r["timings_ms"]]
        out[p + "_avg_ms"] = round(sum(vals) / len(vals), 3) if vals else None
        out[p + "_max_ms"] = round(max(vals), 3) if vals else None
    rows = (cfg["users"] + cfg["orders"]) * len(ok)
    out["rows"] = rows
    out["rows_per_s"] = round(rows / (wall_ms / 1000), 1) if wall_ms > 0 else None
    out["errors"] = [r["error"] for r in runs if r["error"]]
    return out


def print_table(title, summaries, cfg):
    print(f"\n=== {title} (users={cfg['users']} orders={cfg['orders']} "
          f"repeats={cfg['repeats']} workers={cfg['workers']}) ===")
    print(f"{'phase':18s} {'SQLTiny(ms)':>13s} {'SQLite(ms)':>13s} {'倍数':>9s}")
    print("-" * 58)
    sq = next(s for s in summaries if s["engine"] == "sqltiny")
    sl = next(s for s in summaries if s["engine"] == "sqlite")
    for p in PHASES:
        a, b = sq[p + "_avg_ms"], sl[p + "_avg_ms"]
        ratio = f"{a / b:8.1f}x" if a is not None and b not in (None, 0) else "     n/a"
        print(f"{p:18s} {a if a is not None else float('nan'):13.2f} "
              f"{b if b is not None else float('nan'):13.2f} {ratio}")
    print("-" * 58)
    print(f"{'wall_ms':18s} {sq['wall_ms']:13.2f} {sl['wall_ms']:13.2f} "
          f"{sq['wall_ms'] / sl['wall_ms']:8.1f}x")
    print(f"{'rows/s':18s} {sq['rows_per_s'] or 0:13.1f} {sl['rows_per_s'] or 0:13.1f}")
    if sq["errors"] or sl["errors"]:
        print(f"[WARN] errors: sqltiny={sq['errors']} sqlite={sl['errors']}")


def save_results(out_dir, cfg, modes, server_exe):
    out_dir.mkdir(parents=True, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    payload = {
        "meta": {
            "timestamp": datetime.now().isoformat(timespec="seconds"),
            "python": sys.version.split()[0],
            "platform": platform.platform(),
            "server_exe": str(server_exe),
            "server_config": "Release / memory-only" if not cfg["reuse_server"] else "reuse existing",
            "port": cfg["port"],
            "modes": modes,
            "cfg": cfg,
        },
        "results": {},
    }
    for mode, (summaries, runs) in modes.items():
        payload["results"][mode] = {"summary": summaries, "runs": runs}

    json_path = out_dir / f"perf_{ts}.json"
    json_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    (out_dir / "latest.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")

    csv_path = out_dir / f"perf_{ts}.csv"
    history = out_dir / "history.csv"
    fields = ["timestamp", "mode", "engine", "workers", "wall_ms", "rows", "rows_per_s"] + \
             [p + "_avg_ms" for p in PHASES] + ["errors"]
    rows = []
    for mode, (summaries, _) in modes.items():
        for s in summaries:
            rows.append({
                "timestamp": payload["meta"]["timestamp"], "mode": mode,
                "engine": s["engine"], "workers": s["workers"], "wall_ms": s["wall_ms"],
                "rows": s["rows"], "rows_per_s": s["rows_per_s"],
                **{p + "_avg_ms": s[p + "_avg_ms"] for p in PHASES},
                "errors": ";".join(s["errors"]) if s["errors"] else "",
            })
    with csv_path.open("w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(rows)
    need_header = not history.exists()
    with history.open("a", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        if need_header:
            w.writeheader()
        w.writerows(rows)
    return json_path, csv_path, history


# ------------------------------------------------------------------ main
def run_mode(mode, cfg):
    n = 1 if mode == "single" else cfg["workers"]
    results = {}
    for engine in ("sqltiny", "sqlite"):
        tasks = [(engine, wid, cfg) for wid in range(n)]
        t0 = time.perf_counter()
        if mode == "single":
            runs = [run_worker(t) for t in tasks]
        else:
            with mp.Pool(processes=n) as pool:
                runs = pool.map(run_worker, tasks)
        wall = (time.perf_counter() - t0) * 1000
        results[engine] = (runs, wall)
    summaries = [summarize(eng, runs, wall, cfg)
                 for eng, (runs, wall) in results.items()]
    runs = [r for eng in ("sqltiny", "sqlite") for r in results[eng][0]]
    print_table(mode, summaries, cfg)
    return summaries, runs


def main():
    ap = argparse.ArgumentParser(description="SQLTiny(Release) vs SQLite 性能对比")
    ap.add_argument("--mode", choices=["single", "multi", "both"], default="both")
    ap.add_argument("--workers", type=int, default=4, help="multi 模式并发 worker 数")
    ap.add_argument("--users", type=int, default=200)
    ap.add_argument("--orders", type=int, default=800)
    ap.add_argument("--batch", type=int, default=50)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--seed", type=int, default=20261112)
    ap.add_argument("--port", type=int, default=3307)
    ap.add_argument("--server", default=str(DEFAULT_SERVER))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--data-dir", default=None, help="SQLTiny WAL 目录,缺省内存模式")
    ap.add_argument("--reuse-server", action="store_true", help="复用已运行的 server,不自动启停")
    args = ap.parse_args()

    cfg = {"port": args.port, "users": args.users, "orders": args.orders,
           "batch": args.batch, "repeats": args.repeats, "seed": args.seed,
           "workers": args.workers, "reuse_server": args.reuse_server}
    modes = ["single", "multi"] if args.mode == "both" else [args.mode]

    proc = None
    try:
        if not args.reuse_server:
            print(f"[server] 启动 Release: {args.server} (port={args.port})")
            proc = start_server(args.server, args.port, args.data_dir)
        elif not wait_port(args.port, timeout=1.0):
            raise RuntimeError(f"--reuse-server 但端口 {args.port} 无 server")

        collected = {}
        for mode in modes:
            collected[mode] = run_mode(mode, cfg)

        j, c, h = save_results(Path(args.out), cfg, collected, args.server)
        print(f"\n[benchmark] 已保存:\n  {j}\n  {c}\n  {h} (追加)\n  {Path(args.out) / 'latest.json'}")
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
            print("[server] 已停止")


if __name__ == "__main__":
    main()
