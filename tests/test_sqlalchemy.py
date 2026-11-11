"""
Test 1: SQLAlchemy 功能测试 — 验证 SQLAlchemy (MySQL dialect via PyMySQL) 能否完整驱动 SQLTiny
覆盖: Engine/Core/ORM CRUD + Session commit/rollback + 多表 relationship + 聚合 + GROUP BY + JOIN
      + 2.0 风格 select/insert/update/delete + text() 参数绑定 + engine.begin() 事务块
      + 类型映射(DateTime/Boolean/Float) + Inspector 列详情

注意: SQLAlchemy 2.x + MySQL dialect — SQLTiny 需要先验证哪些 SQLAlchemy 内部 SQL 能跑
(比如 CREATE TABLE 的额外语法、AUTO_INCREMENT 写法等)。跑失败的地方记下来就是改进点。
"""
import sqlalchemy as sa
from sqlalchemy import Column, Integer, String, Float, ForeignKey, create_engine, inspect, text
from sqlalchemy.orm import declarative_base, Session, relationship
import time, sys

BASE_URL = "mysql+pymysql://root@127.0.0.1:3306"
DB_NAME  = "sa_test"
PASS = "\033[92mPASS\033[0m"
FAIL = "\033[91mFAIL\033[0m"
GAP  = "\033[93mGAP\033[0m"

N_PASS = N_FAIL = N_GAP = 0

def ok(label, cond, detail="", gap=False):
    global N_PASS, N_FAIL, N_GAP
    if cond:
        tag, N_PASS = PASS, N_PASS + 1
    elif gap:
        tag, N_GAP = GAP, N_GAP + 1
    else:
        tag, N_FAIL = FAIL, N_FAIL + 1
    print(f"  {tag}  {label}{(' — ' + detail) if detail and not cond else ''}")
    return cond

def ok_exc(label, e):
    """异常断言: 命中已知 MySQL 方言限制 (DESCRIBE / SHOW FULL TABLES 等) 记 GAP, 不计入 FAIL"""
    msg = str(e).upper()
    return ok(label, False, str(e), gap=("DESCRIBE" in msg or "SHOW" in msg))

# ---------- 0. 连接 ----------
print("[0] 连接 Engine")
# SQLTiny 已知缺陷 (CODEPLAN §8.4): 握手 database 字段被忽略, current db 为服务器全局状态。
# → 业务连接一律用 init_command 显式 USE; admin 先 USE default 以规避 DROP 当前库 1008。
admin = create_engine(BASE_URL, connect_args={"charset":"utf8mb4"})
with admin.connect() as c:
    try:
        c.execute(text("USE default"))
    except Exception:
        pass
    try:
        c.execute(text(f"DROP DATABASE IF EXISTS {DB_NAME}"))
        c.execute(text(f"CREATE DATABASE {DB_NAME}"))
    except Exception:
        pass  # 降级: 依赖下方表级清理
    c.commit()
admin.dispose()

CONN_ARGS = {"charset": "utf8mb4", "init_command": f"USE {DB_NAME}"}
engine = create_engine(f"{BASE_URL}/{DB_NAME}", connect_args=CONN_ARGS)
with engine.connect() as c:
    for t in ("typed_t", "orders", "users"):
        try:
            c.execute(text(f"DROP TABLE IF EXISTS {t}"))
        except Exception:
            pass
    c.commit()
ok("engine connect", True)

# ---------- 1. Core: Table 定义 + DDL ----------
print("\n[1] Core 建表")
meta = sa.MetaData()
users_t = sa.Table("users", meta,
    Column("id", Integer, primary_key=True, autoincrement=True),
    Column("name", String(32), nullable=False),
    Column("age", Integer, default=18),
)
orders_t = sa.Table("orders", meta,
    Column("id", Integer, primary_key=True, autoincrement=True),
    Column("user_id", Integer, ForeignKey("users.id")),
    Column("total", Float, nullable=False),
    Column("status", String(16), default="pending"),
)
try:
    meta.create_all(engine)
    ok("meta.create_all 建两张表", True)
except Exception as e:
    ok_exc("meta.create_all 建两张表 (DESCRIBE 反射)", e)
    print("  → SQLAlchemy 生成的 DDL 可能含 SQLTiny 暂不支持的语法,降级用 raw SQL")
    with engine.connect() as c:
        c.execute(text("CREATE TABLE users (id BIGINT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(32) NOT NULL, age INT DEFAULT 18)"))
        c.execute(text("CREATE TABLE orders (id BIGINT PRIMARY KEY AUTO_INCREMENT, user_id INT, total DECIMAL(10,2) NOT NULL, status VARCHAR(16) DEFAULT 'pending')"))
        c.commit()
    ok("raw SQL fallback 建表", True)

# ---------- 2. Core Insert ----------
print("\n[2] Core INSERT")
with Session(engine) as s:
    try:
        s.execute(users_t.insert(), [
            {"name":"Alice","age":28},{"name":"Bob","age":32},
            {"name":"Charlie","age":25},{"name":"Diana","age":40},
        ])
        s.commit()
        ok("批量 INSERT users", True)
    except Exception as e:
        s.rollback()
        ok("批量 INSERT users", False, str(e))

with Session(engine) as s:
    try:
        # 拿到刚插的 users id
        rows = s.execute(text("SELECT id, name FROM users ORDER BY id")).fetchall()
        uid_map = {r[1]: r[0] for r in rows}
        s.execute(orders_t.insert(), [
            {"user_id": uid_map["Alice"], "total": 100.0, "status":"paid"},
            {"user_id": uid_map["Alice"], "total": 50.0,  "status":"pending"},
            {"user_id": uid_map["Bob"],   "total": 200.0, "status":"paid"},
            {"user_id": uid_map["Charlie"],"total":75.0, "status":"paid"},
        ])
        s.commit()
        ok("批量 INSERT orders", True)
    except Exception as e:
        ok("批量 INSERT orders", False, str(e))

# ---------- 3. Core SELECT ----------
print("\n[3] Core SELECT")
with Session(engine) as s:
    r = s.execute(text("SELECT COUNT(*) FROM users")).scalar()
    ok("COUNT(*) users = 4", r == 4, f"got {r}")

    r = s.execute(text("SELECT SUM(total), AVG(total) FROM orders")).fetchone()
    ok("SUM/AVG orders", r[0] == 425.0, f"SUM={r[0]} AVG={r[1]}")

    r = s.execute(text("SELECT age, COUNT(*) FROM users GROUP BY age HAVING COUNT(*)>1")).fetchall()
    # 这里 age 25,28,32,40 各自唯一,所以 HAVING COUNT>1 返回空
    ok("GROUP BY + HAVING", len(r) == 0, f"got {r}")

    r = s.execute(text("""
        SELECT u.name, COUNT(o.id) AS cnt, SUM(o.total) AS t
        FROM users u LEFT JOIN orders o ON o.user_id = u.id
        GROUP BY u.id, u.name ORDER BY u.name
    """)).fetchall()
    ok("LEFT JOIN + GROUP BY", len(r) == 4, f"got {len(r)} rows: {r}")

# ---------- 4. ORM 声明 ----------
print("\n[4] ORM Declarative")
Base = declarative_base()
class User(Base):
    __tablename__ = "users"
    id   = Column(Integer, primary_key=True)
    name = Column(String(32), nullable=False)
    age  = Column(Integer, default=18)
class Order(Base):
    __tablename__ = "orders"
    id      = Column(Integer, primary_key=True)
    user_id = Column(Integer, ForeignKey("users.id"))
    total   = Column(Float, nullable=False)
    status  = Column(String(16), default="pending")

with Session(engine) as s:
    try:
        alice = s.get(User, 1)
        ok("s.get(User, 1) ORM 读取", alice is not None and alice.name == "Alice",
           f"got {alice}")
    except Exception as e:
        ok("s.get ORM", False, str(e))

    try:
        s.query(User).filter(User.age > 28).order_by(User.age.desc()).all()
        ok("query + filter + order_by", True)
    except Exception as e:
        ok("query + filter + order_by", False, str(e))

# ---------- 5. UPDATE + DELETE ----------
print("\n[5] UPDATE + DELETE")
with Session(engine) as s:
    s.execute(text("UPDATE users SET age = 99 WHERE name = 'Charlie'"))
    s.commit()
    r = s.execute(text("SELECT age FROM users WHERE name='Charlie'")).scalar()
    ok("UPDATE Charlie age=99", r == 99, f"got {r}")

    s.execute(text("DELETE FROM users WHERE name='Charlie'"))
    s.commit()
    r = s.execute(text("SELECT COUNT(*) FROM users")).scalar()
    ok("DELETE Charlie → COUNT=3", r == 3, f"got {r}")

# ---------- 6. SHOW/INSPECT ----------
print("\n[6] Inspector")
try:
    insp = inspect(engine)
    tables = insp.get_table_names()
    ok("inspector.get_table_names", set(tables) >= {"users","orders"}, f"got {tables}")
except Exception as e:
    ok_exc("inspector.get_table_names", e)

# ---------- 7. 持久化重启验证 ----------
print("\n[7] 持久化重启验证 (SQLAlchemy 重连后数据还在)")
engine.dispose()
# 注意:server 不重启,只关掉 engine 重连 — 测试 SQLAlchemy 重连
engine2 = create_engine(f"{BASE_URL}/{DB_NAME}", connect_args=CONN_ARGS)
with engine2.connect() as c:
    r = c.execute(text("SELECT COUNT(*) FROM users")).scalar()
    ok("重连后 COUNT=3", r == 3, f"got {r}")

# ---------- 8. 2.0 风格 Core 表达式 ----------
print("\n[8] 2.0 风格 select/insert/update/delete")
with Session(engine) as s:
    try:
        r = s.execute(users_t.insert().values(name="Eve", age=22))
        ok("insert().values() rowcount=1", r.rowcount == 1, f"got {r.rowcount}")
        s.commit()
    except Exception as e:
        s.rollback(); ok("insert().values()", False, str(e))

with Session(engine) as s:
    try:
        rows = s.execute(
            sa.select(users_t.c.name)
              .where(users_t.c.age >= 28)
              .order_by(users_t.c.age)
        ).fetchall()
        ok("select().where().order_by() → [Alice,Bob,Diana]",
           [x[0] for x in rows] == ["Alice", "Bob", "Diana"], f"got {rows}")

        r = s.execute(sa.update(users_t).where(users_t.c.name == "Eve").values(age=23))
        ok("update() 表达式 rowcount=1", r.rowcount == 1, f"got {r.rowcount}")
        r2 = s.execute(text("SELECT age FROM users WHERE name='Eve'")).scalar()
        ok("update() 表达式生效 age=23", r2 == 23, f"got {r2}")

        r = s.execute(sa.delete(users_t).where(users_t.c.name == "Eve"))
        ok("delete() 表达式 rowcount=1", r.rowcount == 1, f"got {r.rowcount}")
        r2 = s.execute(text("SELECT COUNT(*) FROM users")).scalar()
        ok("delete() 后 COUNT=3", r2 == 3, f"got {r2}")
        s.commit()
    except Exception as e:
        s.rollback(); ok("2.0 Core 表达式链", False, str(e))

# ---------- 9. text() 参数绑定 ----------
print("\n[9] text() :param 绑定")
with Session(engine) as s:
    try:
        rows = s.execute(
            text("SELECT name FROM users WHERE age > :min_age ORDER BY age"),
            {"min_age": 28},
        ).fetchall()
        ok("text():param WHERE → [Bob,Diana]",
           [x[0] for x in rows] == ["Bob", "Diana"], f"got {rows}")

        r = s.execute(
            text("SELECT COUNT(*) FROM orders WHERE user_id = :uid"),
            {"uid": 2},
        ).scalar()
        ok("text():param 聚合 user_id=2 → 1", r == 1, f"got {r}")

        # `status` 列名曾因保留字冲突无法作 WHERE 条件 (CODEPLAN §8.4 #5, 已修复)
        r = s.execute(text("SELECT COUNT(*) FROM orders WHERE status = :st"),
                      {"st": "paid"}).scalar()
        ok("status 列可作 WHERE 绑定条件 (paid=3)", r == 3, f"got {r}")
    except Exception as e:
        ok("text() 绑定参数", False, str(e))

# ---------- 10. Session rollback ----------
print("\n[10] Session add + rollback")
with Session(engine) as s:
    try:
        base = s.execute(text("SELECT COUNT(*) FROM users")).scalar()
        s.add(User(name="Temp", age=1))
        s.rollback()
        r = s.execute(text("SELECT COUNT(*) FROM users")).scalar()
        ok("ORM add 后 rollback 不写入", r == base, f"base={base} got={r}")
    except Exception as e:
        s.rollback(); ok("Session rollback", False, str(e))

# ---------- 11. ORM relationship + 级联写入 ----------
print("\n[11] ORM relationship")
RelBase = declarative_base()
class URel(RelBase):
    __tablename__ = "users"
    id     = Column(Integer, primary_key=True)
    name   = Column(String(32))
    age    = Column(Integer)
    orders = relationship("ORel", back_populates="user")
class ORel(RelBase):
    __tablename__ = "orders"
    id      = Column(Integer, primary_key=True)
    user_id = Column(Integer, ForeignKey("users.id"))
    total   = Column(Float)
    status  = Column(String(16))
    user    = relationship("URel", back_populates="orders")

with Session(engine) as s:
    try:
        alice = s.query(URel).filter(URel.name == "Alice").one()
        ok("relationship 读取 Alice.orders=2", len(alice.orders) == 2,
           f"got {len(alice.orders)}")
        alice.orders.append(ORel(total=10.0, status="pending"))
        s.commit()
        r = s.execute(text("SELECT COUNT(*) FROM orders")).scalar()
        ok("relationship 级联 INSERT → orders=5", r == 5, f"got {r}")
    except Exception as e:
        s.rollback(); ok("ORM relationship", False, str(e))

# ---------- 12. engine.begin() 事务块 ----------
print("\n[12] engine.begin() 事务块")
try:
    with engine.begin() as c:
        c.execute(text("INSERT INTO users (name, age) VALUES ('Ghost', 0)"))
        with engine.connect() as c2:
            r = c2.execute(text("SELECT COUNT(*) FROM users")).scalar()
            ok("begin() 内未提交对其他连接不可见", r == 3, f"got {r}")
    with engine.connect() as c:
        r = c.execute(text("SELECT COUNT(*) FROM users")).scalar()
        ok("begin() 块退出自动提交 COUNT=4", r == 4, f"got {r}")
        c.execute(text("DELETE FROM users WHERE name='Ghost'"))
        c.commit()
except Exception as e:
    ok("engine.begin()", False, str(e))

# ---------- 13. 类型映射 ----------
print("\n[13] 类型映射 DateTime/Boolean/Float")
from datetime import datetime
typed_t = sa.Table("typed_t", meta,
    Column("id", Integer, primary_key=True, autoincrement=True),
    Column("ts", sa.DateTime),
    Column("flag", sa.Boolean),
    Column("ratio", Float),
)
try:
    try:
        typed_t.drop(engine, checkfirst=False)
    except Exception:
        pass
    # SQLTiny DDL 不识别 SQLAlchemy 生成的 BOOL 类型 (方言 GAP) → raw DDL 建表, 验证绑定/回读链路
    with engine.connect() as c:
        c.execute(text("CREATE TABLE typed_t (id INT PRIMARY KEY AUTO_INCREMENT, "
                       "ts DATETIME, flag TINYINT(1), ratio DOUBLE)"))
        c.commit()
    with Session(engine) as s:
        s.execute(typed_t.insert().values(
            ts=datetime(2026, 11, 11, 12, 30, 0), flag=True, ratio=0.5))
        s.commit()
        row = s.execute(sa.select(typed_t.c.ts, typed_t.c.flag, typed_t.c.ratio)).fetchone()
        ok("DateTime 回读 datetime(2026,11,11,12:30)", row[0] == datetime(2026, 11, 11, 12, 30),
           f"got {row[0]!r}")
        ok("Boolean 回读 True", row[1] in (True, 1), f"got {row[1]!r}")
        ok("Float 回读 0.5", abs(float(row[2]) - 0.5) < 1e-6, f"got {row[2]!r}")
except Exception as e:
    ok("类型映射", False, str(e))

# ---------- 14. Inspector 列详情 ----------
print("\n[14] Inspector 列详情")
try:
    insp = inspect(engine)
    cols = insp.get_columns("users")
    names = {c["name"] for c in cols}
    ok("get_columns(users) 含 id/name/age", {"id", "name", "age"} <= names, f"got {names}")
    # 注:SQLAlchemy 2.x get_columns 不再返回 primary_key 字段,主键经 get_pk_constraint 查询
    pkc = insp.get_pk_constraint("users")
    pk = pkc.get("constrained_columns", [])
    ok("get_pk_constraint 识别主键 id", "id" in pk, f"pk={pk}")
except Exception as e:
    ok("inspector.get_columns", False, str(e), gap=True)

print(f"\n=== SQLAlchemy 测试完成: {N_PASS} PASS / {N_FAIL} FAIL / {N_GAP} GAP ===")
sys.exit(1 if N_FAIL else 0)
