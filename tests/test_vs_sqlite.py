"""
Test 2: SQLTiny vs SQLite 用户全场景对比测试
同一个逻辑流程在两个引擎上跑,用 assert 验证行为一致:
  DDL (CREATE DATABASE/TABLE) → INSERT → SELECT (单表/JOIN/聚合) → UPDATE → DELETE → DROP
"""
import pymysql, sqlite3, time, os, subprocess

PASS = "\033[92m✅\033[0m"
FAIL = "\033[91m❌\033[0m"

# ==========================================================
# 两个引擎的 facade — 提供统一的 execute/fetchall/fetchone/commit
# ==========================================================

class SQLEngine:
    def __init__(self, label): self.label = label
    def setup_schema(self): raise NotImplementedError
    def teardown(self): raise NotImplementedError
    def exec(self, sql, params=None): raise NotImplementedError

class SQLTiny(SQLEngine):
    def __init__(self):
        super().__init__("SQLTiny")
        self.conn = pymysql.connect(host="127.0.0.1", port=3306, user="root",
                                    charset="utf8mb4", autocommit=True)
        self.cur = self.conn.cursor()
        self.exec("USE default")
        self.exec("DROP DATABASE IF EXISTS cmp")
        self.exec("CREATE DATABASE cmp")
        self.exec("USE cmp")
    def exec(self, sql, params=None):
        try:
            if params: self.cur.execute(sql, params)
            else:      self.cur.execute(sql)
        except Exception as e:
            print(f"  [{self.label}] ERROR {sql} → {e}")
            raise
    def setup_schema(self):
        self.exec("""CREATE TABLE users (
            id BIGINT PRIMARY KEY AUTO_INCREMENT,
            name VARCHAR(32) NOT NULL, age INT DEFAULT 18, dept_id INT
        )""")
        self.exec("""CREATE TABLE orders (
            id BIGINT PRIMARY KEY AUTO_INCREMENT,
            user_id INT, total DECIMAL(10,2) NOT NULL, status VARCHAR(16) DEFAULT 'pending'
        )""")
    def insert_users(self, rows):
        for r in rows: self.exec(f"INSERT INTO users(name,age,dept_id) VALUES({r[0]!r},{r[1]},{r[2]})")
    def insert_orders(self, rows):
        for r in rows: self.exec(f"INSERT INTO orders(user_id,total,status) VALUES({r[0]},{r[1]},{r[2]!r})")
    def query(self, sql, params=None):
        self.exec(sql, params)
        return [tuple(r) for r in self.cur.fetchall()]
    def teardown(self):
        self.exec("USE default")
        self.exec("DROP DATABASE cmp")
        self.conn.close()

class SQLite(SQLEngine):
    def __init__(self, path=":memory:"):
        super().__init__("SQLite")
        self.conn = sqlite3.connect(path)
        self.conn.execute("PRAGMA foreign_keys = ON")
        self.cur = self.conn.cursor()
    def exec(self, sql, params=None):
        try:
            if params: self.cur.execute(sql, params)
            else:      self.cur.execute(sql)
        except Exception as e:
            print(f"  [{self.label}] ERROR {sql} → {e}")
            raise
    def setup_schema(self):
        self.exec("""CREATE TABLE users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL, age INTEGER DEFAULT 18, dept_id INTEGER
        )""")
        self.exec("""CREATE TABLE orders (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id INTEGER, total REAL NOT NULL, status TEXT DEFAULT 'pending'
        )""")
    def insert_users(self, rows):
        for r in rows: self.exec("INSERT INTO users(name,age,dept_id) VALUES (?,?,?)", r)
        self.conn.commit()
    def insert_orders(self, rows):
        for r in rows: self.exec("INSERT INTO orders(user_id,total,status) VALUES (?,?,?)", r)
        self.conn.commit()
    def query(self, sql, params=None):
        self.exec(sql, params)
        return [tuple(r) for r in self.cur.fetchall()]
    def teardown(self):
        self.conn.close()

# ==========================================================
# 对比 helper
# ==========================================================
def check(label, sqltiny_val, sqlite_val, tolerance=0.001):
    """两个引擎返回值应该一致 (float 用 tolerance 比较)"""
    def approx(a, b):
        if isinstance(a, float) or isinstance(b, float):
            return abs(float(a) - float(b)) <= tolerance
        return a == b
    ok = True
    if isinstance(sqltiny_val, list):
        if len(sqltiny_val) != len(sqlite_val): ok = False
        else:
            for i in range(len(sqltiny_val)):
                if isinstance(sqltiny_val[i], tuple):
                    if len(sqltiny_val[i]) != len(sqlite_val[i]): ok = False; break
                    for j in range(len(sqltiny_val[i])):
                        if not approx(sqltiny_val[i][j], sqlite_val[i][j]): ok = False; break
                else:
                    if not approx(sqltiny_val[i], sqlite_val[i]): ok = False
    else:
        ok = approx(sqltiny_val, sqlite_val)
    tag = PASS if ok else FAIL
    if ok: print(f"  {tag} {label}")
    else:  print(f"  {tag} {label}  SQLTiny={sqltiny_val}  SQLite={sqlite_val}")
    return ok

# ==========================================================
# 主流程
# ==========================================================
print("=" * 60)
print("  SQLTiny vs SQLite — 用户全场景对比")
print("=" * 60)

sql = SQLTiny()
lite = SQLite()
all_ok = True

# --- Setup ---
print("\n[Setup] DDL")
sql.setup_schema(); lite.setup_schema()
print("  两张表创建 OK")

# --- Insert ---
print("\n[Insert] 5 users + 5 orders")
users = [("Alice",28,1),("Bob",32,1),("Charlie",25,2),("Diana",40,2),("Eve",None,None)]
orders = [(1,100.0,"paid"),(1,50.0,"pending"),(2,200.0,"paid"),(3,75.0,"paid"),(4,120.0,"paid")]
sql.insert_users(users); sql.insert_orders(orders)
lite.insert_users(users); lite.insert_orders(orders)
check("INSERT 总数 users=5", len(sql.query("SELECT * FROM users")), 5)
check("INSERT 总数 orders=5", len(sql.query("SELECT * FROM orders")), 5)
check("Eve age=NULL",        sql.query("SELECT age FROM users WHERE name='Eve'")[0][0], None)

# --- 单表 SELECT ---
print("\n[SELECT] 单表查询")
all_ok &= check("WHERE age > 28", sql.query("SELECT name FROM users WHERE age > 28 ORDER BY name"),
                                    lite.query("SELECT name FROM users WHERE age > 28 ORDER BY name"))
all_ok &= check("WHERE dept_id IS NULL", sql.query("SELECT name FROM users WHERE dept_id IS NULL ORDER BY name"),
                                         lite.query("SELECT name FROM users WHERE dept_id IS NULL ORDER BY name"))
all_ok &= check("LIMIT 2 ORDER BY age DESC",
                sql.query("SELECT name,age FROM users ORDER BY age DESC LIMIT 2"),
                lite.query("SELECT name,age FROM users ORDER BY age DESC LIMIT 2"))

# --- 聚合 ---
print("\n[SELECT] 聚合")
all_ok &= check("COUNT(*)", sql.query("SELECT COUNT(*) FROM users")[0][0],
                            lite.query("SELECT COUNT(*) FROM users")[0][0])
all_ok &= check("SUM(age)", sql.query("SELECT SUM(age) FROM users")[0][0],
                            lite.query("SELECT SUM(age) FROM users")[0][0])
all_ok &= check("AVG(age) (忽略 NULL)", sql.query("SELECT AVG(age) FROM users")[0][0],
                                         lite.query("SELECT AVG(age) FROM users")[0][0])
all_ok &= check("COUNT(*), SUM(total), AVG(total)",
                sql.query("SELECT COUNT(*), SUM(total), AVG(total) FROM orders"),
                lite.query("SELECT COUNT(*), SUM(total), AVG(total) FROM orders"))

# --- GROUP BY + HAVING ---
print("\n[SELECT] GROUP BY + HAVING")
all_ok &= check("dept_id GROUP BY HAVING SUM > 0",
    sql.query("SELECT dept_id, COUNT(*), SUM(age) FROM users WHERE dept_id IS NOT NULL GROUP BY dept_id HAVING SUM(age) > 50 ORDER BY dept_id"),
    lite.query("SELECT dept_id, COUNT(*), SUM(age) FROM users WHERE dept_id IS NOT NULL GROUP BY dept_id HAVING SUM(age) > 50 ORDER BY dept_id"))

# --- JOIN ---
print("\n[SELECT] JOIN")
# 两边应该都是 5: Alice×2 + Bob×1 + Charlie×1 + Diana×1 (Eve 无订单,被 INNER 排除)
sq_count = sql.query("SELECT COUNT(*) FROM users u INNER JOIN orders o ON o.user_id = u.id")[0][0]
lt_count = lite.query("SELECT COUNT(*) FROM users u INNER JOIN orders o ON o.user_id = u.id")[0][0]
all_ok &= check("INNER JOIN users×orders = 5 行", sq_count, lt_count)
all_ok &= check("INNER JOIN users×orders = 5 行 (预期)", sq_count, 5)
all_ok &= check("INNER JOIN + 列对齐",
    sql.query("SELECT u.name, o.total FROM users u INNER JOIN orders o ON o.user_id = u.id ORDER BY u.name, o.total"),
    lite.query("SELECT u.name, o.total FROM users u INNER JOIN orders o ON o.user_id = u.id ORDER BY u.name, o.total"))
# Eve 没有订单,LEFT JOIN 应该保留 Eve 一行 orders 列为 NULL
# 加 o.id 排序确保两边同序
all_ok &= check("LEFT JOIN 含 Eve 的 NULL 行",
    sql.query("SELECT u.name, o.total FROM users u LEFT JOIN orders o ON o.user_id = u.id ORDER BY u.id, o.id"),
    lite.query("SELECT u.name, o.total FROM users u LEFT JOIN orders o ON o.user_id = u.id ORDER BY u.id, o.id"))

# --- UPDATE ---
print("\n[UPDATE]")
sql.exec("UPDATE users SET age=100 WHERE name='Alice'")
lite.exec("UPDATE users SET age=100 WHERE name='Alice'")
all_ok &= check("UPDATE Alice age → 100",
    sql.query("SELECT age FROM users WHERE name='Alice'")[0][0], 100)
all_ok &= check("UPDATE 两引擎结果一致",
    sql.query("SELECT age FROM users WHERE name='Alice'")[0][0],
    lite.query("SELECT age FROM users WHERE name='Alice'")[0][0])

# --- DELETE ---
print("\n[DELETE]")
sql.exec("DELETE FROM users WHERE age < 30")
lite.exec("DELETE FROM users WHERE age < 30")
all_ok &= check("DELETE age<30 → 剩余 dept=1 Alice age=100 + dept=2 Diana age=40",
    sql.query("SELECT name FROM users ORDER BY name"),
    lite.query("SELECT name FROM users ORDER BY name"))

# --- 清理 ---
print("\n[Teardown]")
sql.teardown(); lite.teardown()
print("  清理 OK")

print("\n" + "=" * 60)
if all_ok:
    print("  🎉 SQLTiny vs SQLite — 全场景行为一致!")
else:
    print("  ⚠️  存在不一致 — 请检查 FAIL 项")
print("=" * 60)
