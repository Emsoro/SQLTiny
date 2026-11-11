"""
Test 1: SQLAlchemy 功能测试 — 验证 SQLAlchemy (MySQL dialect via PyMySQL) 能否完整驱动 SQLTiny
覆盖: Engine/Core/ORM CRUD + Session commit + 多表关系 + 聚合 + GROUP BY + JOIN

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

def ok(label, cond, detail=""):
    tag = PASS if cond else FAIL
    print(f"  {tag}  {label}{(' — ' + detail) if detail and not cond else ''}")
    return cond

# ---------- 0. 连接 ----------
print("[0] 连接 Engine")
engine = create_engine(f"{BASE_URL}/{DB_NAME}", connect_args={"charset":"utf8mb4"})
with engine.connect() as c:
    c.execute(text(f"DROP DATABASE IF EXISTS {DB_NAME}"))
    c.execute(text(f"CREATE DATABASE {DB_NAME}"))
    c.commit()
engine = create_engine(f"{BASE_URL}/{DB_NAME}", connect_args={"charset":"utf8mb4"})
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
    ok("meta.create_all 建两张表", False, str(e))
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
    ok("inspector", False, str(e))

# ---------- 7. 持久化重启验证 ----------
print("\n[7] 持久化重启验证 (SQLAlchemy 重连后数据还在)")
engine.dispose()
# 注意:server 不重启,只关掉 engine 重连 — 测试 SQLAlchemy 重连
engine2 = create_engine(f"{BASE_URL}/{DB_NAME}", connect_args={"charset":"utf8mb4"})
with engine2.connect() as c:
    r = c.execute(text("SELECT COUNT(*) FROM users")).scalar()
    ok("重连后 COUNT=3", r == 3, f"got {r}")

print("\n=== SQLAlchemy 测试完成 ===")
