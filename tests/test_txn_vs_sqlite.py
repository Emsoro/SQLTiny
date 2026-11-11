"""
事务 / 四个隔离级别 —— SQLTiny vs SQLite 结果对比

参考 HSQLDB 2.7.4 的隔离级别语义:
  READ UNCOMMITTED  → 提升为 READ COMMITTED (MVCC 下不允许脏读)
  READ COMMITTED    → 语句级快照
  REPEATABLE READ   → 提升为 SERIALIZABLE (事务级快照 / SNAPSHOT ISOLATION)
  SERIALIZABLE      → 事务级快照 + 写冲突 40001

SQLite 只有两档实际语义:
  autocommit 下每条 SELECT 是独立事务  ⇔ READ COMMITTED(语句级快照)
  显式 BEGIN ... COMMIT 持有一个快照    ⇔ SERIALIZABLE(事务级快照)
  因此本测试用这两种模式分别作为 SQLTiny 四个级别的对照基准,逐项比对结果。
  READ UNCOMMITTED 一节另外展示 SQLite shared-cache read_uncommitted 的真实脏读,
  说明 HSQLDB 的提升规则带来的"预期不一致"。

运行: python tests/test_txn_vs_sqlite.py   (自动起停 sqltiny_server, 不占用 3306)
"""
import os
import sys
import time
import socket
import shutil
import sqlite3
import subprocess
import tempfile
from pathlib import Path

try:
    import pymysql
except ImportError:
    print("需要 pymysql: pip install pymysql")
    sys.exit(2)

ROOT = Path(__file__).resolve().parent.parent
EXE = ROOT / "build" / "Debug" / "sqltiny_server.exe"
if not EXE.exists():
    EXE = ROOT / "build" / "sqltiny_server"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def norm_err(exc):
    return "ERR"


class Conn:
    """统一的 exec/query 门面,把驱动异常归一化成 'ERR'"""

    def __init__(self, cur):
        self.cur = cur

    def exec(self, sql):
        try:
            self.cur.execute(sql)
            return "OK"
        except Exception:
            return norm_err(None)

    def query(self, sql):
        try:
            self.cur.execute(sql)
            return tuple(tuple(r) for r in self.cur.fetchall())
        except Exception:
            return norm_err(None)


class TinyConn(Conn):
    def __init__(self, port):
        self.conn = pymysql.connect(host="127.0.0.1", port=port, user="root",
                                    charset="utf8mb4", autocommit=True)
        super().__init__(self.conn.cursor())

    def close(self):
        self.conn.close()


class LiteConn(Conn):
    def __init__(self, path):
        self.conn = sqlite3.connect(path, isolation_level=None, timeout=1.0)
        self.conn.execute("PRAGMA busy_timeout = 300")
        super().__init__(self.conn.cursor())

    def close(self):
        self.conn.close()


class Harness:
    """SQLTiny 用隔离级别控制读快照;SQLite 用是否显式 BEGIN 控制读快照"""

    def __init__(self, engine, level, port, lite_path):
        self.engine = engine
        self.level = level
        self.snapshot = level in ("REPEATABLE READ", "SERIALIZABLE")
        if engine == "SQLTiny":
            self.c1 = TinyConn(port)
            self.c2 = TinyConn(port)
            self.c1.exec("USE default")
            self.c1.exec("DROP DATABASE IF EXISTS txncmp")
            self.c1.exec("CREATE DATABASE txncmp")
            self.c1.exec("USE txncmp")
            self.c1.exec("CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(32))")
        else:
            self.c1 = LiteConn(lite_path)
            self.c2 = LiteConn(lite_path)
            self.c1.exec("PRAGMA journal_mode=WAL")
            self.c1.exec("DROP TABLE IF EXISTS t")
            self.c1.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)")
        self.set_level(level)

    def set_level(self, level):
        if self.engine == "SQLTiny":
            self.c2.exec("SET SESSION TRANSACTION ISOLATION LEVEL " + level)
            self.c1.exec("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED")
        # SQLite:无需设置,靠 reader_begin/reader_end 模拟

    def reset(self):
        self.c1.exec("DELETE FROM t")
        self.c2.exec("ROLLBACK")

    def reader_begin(self):
        if self.engine == "SQLTiny" or self.snapshot:
            self.c2.exec("BEGIN")

    def reader_end(self):
        if self.engine == "SQLTiny" or self.snapshot:
            self.c2.exec("COMMIT")

    def close(self):
        self.c1.close()
        self.c2.close()


def matrix(h):
    """同一套事务脚本在两个引擎上跑,返回逐项观测值"""
    c1, c2, out = h.c1, h.c2, {}
    h.reset()
    c1.exec("INSERT INTO t VALUES (1,'base')")

    # --- 脏读 ---
    c1.exec("BEGIN")
    c1.exec("INSERT INTO t VALUES (2,'dirty')")
    out["dirty_other"] = c2.query("SELECT COUNT(*) FROM t")
    out["dirty_self"] = c1.query("SELECT COUNT(*) FROM t")
    c1.exec("ROLLBACK")
    out["after_rollback"] = c2.query("SELECT COUNT(*) FROM t")

    # --- 不可重复读 / 幻读 ---
    h.reader_begin()
    out["repeat1"] = c2.query("SELECT COUNT(*) FROM t")
    c1.exec("INSERT INTO t VALUES (3,'new')")
    out["repeat2"] = c2.query("SELECT COUNT(*) FROM t")
    h.reader_end()
    out["repeat3"] = c2.query("SELECT COUNT(*) FROM t")

    # --- 已提交 UPDATE 的可见性 ---
    h.reader_begin()
    out["update1"] = c2.query("SELECT v FROM t WHERE id=1")
    c1.exec("UPDATE t SET v='changed' WHERE id=1")
    out["update2"] = c2.query("SELECT v FROM t WHERE id=1")
    h.reader_end()
    out["update3"] = c2.query("SELECT v FROM t WHERE id=1")

    # --- 提交 / 回滚 ---
    c1.exec("BEGIN")
    c1.exec("INSERT INTO t VALUES (4,'rb')")
    c1.exec("UPDATE t SET v='temp' WHERE id=1")
    c1.exec("ROLLBACK")
    out["rollback_value"] = c2.query("SELECT v FROM t WHERE id=1")
    out["rollback_count"] = c2.query("SELECT COUNT(*) FROM t")
    c1.exec("BEGIN")
    c1.exec("INSERT INTO t VALUES (5,'cm')")
    c1.exec("COMMIT")
    out["commit_count"] = c2.query("SELECT COUNT(*) FROM t")

    # --- SAVEPOINT ---
    c1.exec("BEGIN")
    c1.exec("INSERT INTO t VALUES (10,'s1')")
    c1.exec("SAVEPOINT sp")
    c1.exec("INSERT INTO t VALUES (11,'s2')")
    c1.exec("ROLLBACK TO sp")
    c1.exec("COMMIT")
    out["savepoint_ids"] = c2.query("SELECT id FROM t WHERE id >= 10 ORDER BY id")

    # --- 写-写冲突 ---
    c1.exec("BEGIN")
    out["ww_c1"] = c1.exec("UPDATE t SET v='A' WHERE id=1")
    c2.exec("BEGIN")
    out["ww_c2"] = c2.exec("UPDATE t SET v='B' WHERE id=1")
    c1.exec("COMMIT")
    c2.exec("ROLLBACK")
    out["ww_final"] = c2.query("SELECT v FROM t WHERE id=1")
    return out


TOTAL = [0, 0]


def check(label, tiny, lite):
    ok = tiny == lite
    TOTAL[0 if ok else 1] += 1
    print(f"  {'PASS' if ok else 'FAIL'} {label}\n"
          f"        SQLTiny={tiny}  SQLite={lite}")
    return ok


def expect_diff(label, tiny, lite, note):
    ok = tiny != lite
    TOTAL[0 if ok else 1] += 1
    print(f"  {'PASS' if ok else 'FAIL'} {label} (预期不同)\n"
          f"        SQLTiny={tiny}  SQLite={lite}\n        {note}")
    return ok


def main():
    if not EXE.exists():
        print(f"找不到 server: {EXE}; 先执行 cmake --build build --config Debug")
        return 2

    port = free_port()
    tmp = Path(tempfile.mkdtemp(prefix="sqltiny_txncmp_"))
    data_dir = tmp / "data"
    lite_path = tmp / "cmp.db"
    proc = None

    def start_server():
        nonlocal proc
        proc = subprocess.Popen([str(EXE), "--port", str(port), "--user", "root",
                                 "--data-dir", str(data_dir)],
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        for _ in range(60):
            try:
                s = socket.create_connection(("127.0.0.1", port), 0.3)
                s.close()
                return True
            except OSError:
                time.sleep(0.15)
        return False

    def stop_server():
        nonlocal proc
        if proc and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    try:
        if not start_server():
            print("server 启动失败")
            return 2
        print("=" * 68)
        print("  SQLTiny vs SQLite — 事务 / 四隔离级别结果对比")
        print("=" * 68)

        # ---------- 1. 四个隔离级别逐项比对 ----------
        for level in ("READ UNCOMMITTED", "READ COMMITTED",
                      "REPEATABLE READ", "SERIALIZABLE"):
            print(f"\n[{level}]  (SQLite 基准: "
                  f"{'显式 BEGIN 快照' if level in ('REPEATABLE READ', 'SERIALIZABLE') else 'autocommit 语句级'})")
            ht = Harness("SQLTiny", level, port, str(lite_path))
            hl = Harness("SQLite", level, port, str(lite_path))
            try:
                t, l = matrix(ht), matrix(hl)
                for key in t:
                    check(f"{level} / {key}", t[key], l[key])
            finally:
                ht.close()
                hl.close()

        # ---------- 2. 同一事务里 RC 与 SQLite 快照语义的已知差异 ----------
        print("\n[已知语义差异] SQLTiny READ COMMITTED 事务内 vs SQLite 显式事务")
        ht = Harness("SQLTiny", "READ COMMITTED", port, str(lite_path))
        hl = Harness("SQLite", "READ COMMITTED", port, str(lite_path))
        try:
            ht.reset(); hl.reset()
            for h in (ht, hl):
                h.c1.exec("INSERT INTO t VALUES (1,'base')")
                h.c2.exec("BEGIN")
                h.c2.query("SELECT COUNT(*) FROM t")
                h.c1.exec("INSERT INTO t VALUES (2,'new')")
            t = ht.c2.query("SELECT COUNT(*) FROM t")
            l = hl.c2.query("SELECT COUNT(*) FROM t")
            expect_diff("事务内第二次读", t, l,
                        "SQLTiny READ COMMITTED 每条语句取新快照;SQLite 显式事务固定快照")
            ht.c2.exec("COMMIT"); hl.c2.exec("COMMIT")
        finally:
            ht.close(); hl.close()

        # ---------- 3. READ UNCOMMITTED 提升规则 vs SQLite 真实脏读 ----------
        print("\n[提升规则] SQLTiny READ UNCOMMITTED (HSQLDB 提升) vs SQLite read_uncommitted")
        ht = Harness("SQLTiny", "READ UNCOMMITTED", port, str(lite_path))
        try:
            ht.reset()
            ht.c1.exec("BEGIN")
            ht.c1.exec("INSERT INTO t VALUES (1,'dirty')")
            tiny_dirty = ht.c2.query("SELECT COUNT(*) FROM t")
            ht.c1.exec("ROLLBACK")
        finally:
            ht.close()

        lite_dirty = "SKIP"
        shared = tmp / "shared.db"
        try:
            uri = f"file:{shared.as_posix()}?cache=shared"
            a = sqlite3.connect(uri, uri=True, isolation_level=None)
            b = sqlite3.connect(uri, uri=True, isolation_level=None)
            a.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)")
            b.execute("PRAGMA read_uncommitted=1")
            a.execute("BEGIN")
            a.execute("INSERT INTO t VALUES (1,'dirty')")
            lite_dirty = b.execute("SELECT COUNT(*) FROM t").fetchall()
            lite_dirty = tuple(tuple(r) for r in lite_dirty)
            a.execute("ROLLBACK")
            a.close(); b.close()
        except Exception as e:
            print(f"  (SQLite shared-cache 不可用: {e})")

        if lite_dirty != "SKIP":
            expect_diff("未提交 INSERT 是否被其它会话读到", tiny_dirty, lite_dirty,
                        "HSQLDB 把 READ UNCOMMITTED 提升为 READ COMMITTED,永不允许脏读")
        else:
            check("READ UNCOMMITTED 无脏读 (HSQLDB 基准)", tiny_dirty, ((0,),))

        # ---------- 4. 回滚/提交的持久化 (WAL 事务缓冲) ----------
        print("\n[WAL] 未提交事务不得落盘 — 重启后结果对比")
        tconn = TinyConn(port)
        tconn.exec("USE txncmp")
        tconn.exec("DROP TABLE IF EXISTS w")
        tconn.exec("CREATE TABLE w (id INT PRIMARY KEY, v VARCHAR(32))")
        tconn.exec("INSERT INTO w VALUES (1,'keep')")
        tconn.exec("BEGIN")
        tconn.exec("INSERT INTO w VALUES (2,'rolled_back')")
        tconn.exec("ROLLBACK")
        tconn.exec("BEGIN")
        tconn.exec("INSERT INTO w VALUES (3,'committed')")
        tconn.exec("COMMIT")
        tconn.exec("UPDATE w SET v='updated' WHERE id=1")
        tconn.close()

        lconn = LiteConn(str(lite_path))
        lconn.exec("DROP TABLE IF EXISTS w")
        lconn.exec("CREATE TABLE w (id INTEGER PRIMARY KEY, v TEXT)")
        lconn.exec("INSERT INTO w VALUES (1,'keep')")
        lconn.exec("BEGIN")
        lconn.exec("INSERT INTO w VALUES (2,'rolled_back')")
        lconn.exec("ROLLBACK")
        lconn.exec("BEGIN")
        lconn.exec("INSERT INTO w VALUES (3,'committed')")
        lconn.exec("COMMIT")
        lconn.exec("UPDATE w SET v='updated' WHERE id=1")
        lite_before = lconn.query("SELECT id, v FROM w ORDER BY id")
        lconn.close()
        reopened = LiteConn(str(lite_path))
        lite_after = reopened.query("SELECT id, v FROM w ORDER BY id")
        reopened.close()

        # 关闭所有连接后重启 server,验证未提交事务没有落进 WAL
        stop_server()
        time.sleep(0.3)
        if not start_server():
            print("server 重启失败")
            return 2
        t2 = TinyConn(port)
        t2.exec("USE txncmp")
        tiny_after = t2.query("SELECT id, v FROM w ORDER BY id")
        t2.close()

        check("重启前 SQLite 状态", lite_before, ((1, "updated"), (3, "committed")))
        check("重启后 SQLite 状态", lite_after, ((1, "updated"), (3, "committed")))
        check("重启后 SQLTiny 状态 (= SQLite)", tiny_after, lite_after)
        check("回滚行未复活", tiny_after, ((1, "updated"), (3, "committed")))

        print("\n" + "=" * 68)
        print(f"  {TOTAL[0]} PASS / {TOTAL[1]} FAIL")
        print("=" * 68)
        return 0 if TOTAL[1] == 0 else 1
    finally:
        stop_server()
        time.sleep(0.2)
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
