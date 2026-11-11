"""
M6 P1 端到端验证: per-session current db (CODEPLAN §8.4 #6/#7/#8)

覆盖:
  #6  连接握手 database 字段生效 —— database=X 连接后 SELECT DATABASE() 返回 X;
      连不存在的库 → 1049 拒绝连接
  #7  多连接 current db 互相隔离 —— 连接 A 的 USE 不影响连接 B
  #8  允许 DROP DATABASE 当前连接所在的库;删除后该库引用失效

自带 server 生命周期管理(随机端口)。
"""
import pymysql, subprocess, time, os, sys, shutil, socket

EXE = "build/Debug/sqltiny_server.exe"
PORT = 33310

TOTAL = [0, 0]
def ok(label, cond, detail=""):
    TOTAL[0 if cond else 1] += 1
    mark = "✅ PASS" if cond else "❌ FAIL"
    print(f"  {mark} {label}" + (f"  ({detail})" if detail and not cond else ""))
    return cond

def connect(port=PORT, database=None):
    kw = dict(host="127.0.0.1", port=port, user="root", charset="utf8mb4", autocommit=True)
    if database is not None:
        kw["database"] = database
    return pymysql.connect(**kw)

def scalar(cur, sql):
    cur.execute(sql)
    return cur.fetchone()[0]

def start_server(port=PORT):
    p = subprocess.Popen([EXE, "--port", str(port), "--user", "root"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)
    return p

def main():
    p = start_server()
    try:
        # ---------- #6 握手 database 字段 ----------
        print("\n[#6] 握手 database 字段")
        try:
            c = connect(database="nosuchdb_xyz")
            ok("连接不存在的库 → 拒绝", False, "居然连上了")
            c.close()
        except pymysql.err.MySQLError as e:
            ok("连接不存在的库 → 拒绝", e.args[0] == 1049, f"errno={e.args[0]}")

        c = connect()          # 不带 database
        cur = c.cursor()
        ok("未带 database 连接 → default", scalar(cur, "SELECT DATABASE()") == "default")

        cur.execute("CREATE DATABASE hdb")
        cur.execute("CREATE TABLE hdb.t(id INT PRIMARY KEY)")
        c2 = connect(database="hdb")
        cur2 = c2.cursor()
        ok("带 database=hdb 连接 → DATABASE()=hdb", scalar(cur2, "SELECT DATABASE()") == "hdb")
        cur2.execute("INSERT INTO t VALUES (1)")
        cur2.execute("SELECT COUNT(*) FROM t")
        ok("带 database 连接直接可操作库内表", cur2.fetchone()[0] == 1)
        # COM_INIT_DB 路径(pymysql 会发 USE)
        cur2.execute("USE default")
        ok("USE default 切换生效", scalar(cur2, "SELECT DATABASE()") == "default")
        try:
            cur2.execute("USE hdb")
        except Exception:
            pass
        cur2.execute("SELECT COUNT(*) FROM t")
        ok("USE hdb 后可查 hdb.t", cur2.fetchone()[0] == 1)
        cur2.execute("USE default")
        cur2.execute("DROP DATABASE hdb")

        # ---------- #7 多连接 current db 隔离 ----------
        print("\n[#7] 多连接 current db 隔离")
        cur.execute("CREATE DATABASE pdb")
        cur.execute("USE pdb")
        cur.execute("CREATE TABLE t(id INT PRIMARY KEY, v INT)")
        cur.execute("INSERT INTO t VALUES (1, 10)")
        # 另一个全新连接(未 USE),应仍停在 default
        c3 = connect()
        cur3 = c3.cursor()
        ok("B 连接不受 A 的 USE 影响 → default", scalar(cur3, "SELECT DATABASE()") == "default")
        try:
            cur3.execute("SELECT * FROM t")
            ok("B 看不到 A 库里的表", False, "居然查到了")
        except pymysql.err.MySQLError as e:
            ok("B 看不到 A 库里的表", e.args[0] == 1146, f"errno={e.args[0]}")
        # A 的 USE 未被 B 干扰
        ok("A 的 current db 保持 pdb", scalar(cur, "SELECT DATABASE()") == "pdb")
        # B 在 default 建同名表,A 不受影响
        cur3.execute("CREATE TABLE t(id INT PRIMARY KEY, w INT)")
        cur3.execute("INSERT INTO t VALUES (100, 7)")
        cur.execute("SELECT COUNT(*) FROM t")
        ok("A 的 t 仍只有 1 行", cur.fetchone()[0] == 1)
        cur3.execute("DROP TABLE t")
        cur.execute("DROP TABLE t")
        cur.execute("DROP DATABASE pdb")

        # ---------- #8 DROP 当前连接所在的库 ----------
        print("\n[#8] DROP 当前连接所在的库")
        cur.execute("CREATE DATABASE dcur")
        cur.execute("USE dcur")
        cur.execute("CREATE TABLE t(id INT PRIMARY KEY)")
        try:
            cur.execute("DROP DATABASE dcur")
            ok("可以 DROP 当前连接所在的库", True)
        except pymysql.err.MySQLError as e:
            ok("可以 DROP 当前连接所在的库", False, f"errno={e.args[0]}")
        try:
            cur.execute("SELECT * FROM t")
            ok("DROP 后引用失效 → 1146", False, "居然查到了")
        except pymysql.err.MySQLError as e:
            ok("DROP 后引用失效 → 1146", e.args[0] == 1146, f"errno={e.args[0]}")
        cur3.close(); c3.close()
        cur2.close(); c2.close()
        cur.close(); c.close()

        print("\n" + "=" * 60)
        print(f"  {TOTAL[0]} PASS / {TOTAL[1]} FAIL")
        print("=" * 60)
        return 0 if TOTAL[1] == 0 else 1
    finally:
        p.terminate()
        try: p.wait(timeout=3)
        except Exception: p.kill()

if __name__ == "__main__":
    sys.exit(main())
