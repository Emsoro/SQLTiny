# SQLTiny — 轻量级嵌入式数据库 · MySQL 协议兼容

![status](https://img.shields.io/badge/status-M2.5%20Milestone-green)
![language](https://img.shields.io/badge/C%2B%2B-20-blue)
![license](https://img.shields.io/badge/license-Apache%202.0-blue)

**SQLTiny** 是用现代 C++20 从零重写的轻量级关系型数据库,核心设计灵感来自 [HSQLDB](http://hsqldb.org/),额外暴露 **MySQL Wire Protocol**——标准 MySQL 客户端(`mysql` CLI、Navicat、DBeaver、Connector/J)直接连接。

> 完整项目规划见 [`CODEPLAN.md`](./CODEPLAN.md),架构映射、技术选型、里程碑验收都在里面。

---

## ✨ 特点

| 特性 | 说明 |
|---|---|
| 🧠 **纯 C++20** | 无 Java / SQLite / RocksDB 依赖 |
| 🗄️ **嵌入式优先** | 进程内 C++ 调用 或 TCP 客户端连接,两种模式 |
| 🎯 **MySQL 协议兼容** | M2.5 起 `mysql -h 127.0.0.1 -P 3306 -u root` 直接能用 |
| ⚡ **AVL 内存索引** | 自带平衡 BST,主键查询 O(log n) |
| 🪶 **轻量** | 单库文件,启动毫秒级,依赖仅 fmtlib |

---

## 🏗️ 当前进度

| 里程碑 | 状态 | 说明 |
|---|---|---|
| M1 基础骨架 | ✅ | 类型系统 + AVL + Lexer |
| M2 Catalog + DDL/DML | ✅ | Parser + AST + 执行器 |
| **M2.5 MySQL 协议** | ✅ | Wire Protocol 握手 + 编解码 + TCP server |
| M3 MVCC + 子查询 | 🚧 进行中 | READ COMMITTED · GROUP BY · HAVING · 子查询 · JOIN 补全 |
| M4 协议层补全 | 📋 | Prepared Statement · INFORMATION_SCHEMA |
| M5 持久化 WAL | 📋 | 重启不丢数据 |
| M6 性能打磨 | 📋 | B+Tree + EXPLAIN |

---

## 🛠️ 构建

### 环境要求
- **Windows**: Visual Studio 2022 MSVC 14.40+ (MSBuild)
- **CMake**: 3.20+
- **vcpkg**: 设 `VCPKG_ROOT` 环境变量;依赖仅 `fmt` (vcpkg manifest 自动拉取)

```powershell
cmake --preset windows-vcpkg
cmake --build build --config Debug
```

### 构建产物

| 产物 | 类型 | 说明 |
|---|---|---|
| `build/Debug/sqltiny.lib` | 静态库 | 引擎核心,嵌入式进程内调用 |
| `build/Debug/sqltiny_server.exe` | 可执行 | MySQL 协议 TCP 服务器 |
| `build/Debug/smoke_m1.exe` | 冒烟测试 | Value / AVL / Lexer — **3/3 PASS** |
| `build/Debug/smoke_m2.exe` | 冒烟测试 | CRUD / 聚合 / 表达式 / DDL / NULL — **5/5 PASS** |
| `build/Debug/smoke_m25_e2e.exe` | E2E 测试 | Winsock 直连 server — **13/13 PASS** |

### 一键跑全量测试
```powershell
cmake --build build --config Debug

# C++ 单元级(无需 server,纯进程内)
.\build\Debug\smoke_m1.exe
.\build\Debug\smoke_m2.exe

# MySQL 协议端到端(需先启动 server)
Start-Process -FilePath ".\build\Debug\sqltiny_server.exe" -ArgumentList "--port","3306","--user","root"
.\build\Debug\smoke_m25_e2e.exe
```

---

## 🚀 使用方式

### 方式 A:C++ 嵌入式调用(进程内,无协议开销)

```cpp
#include "core/session.h"

sqltiny::core::Session s;
auto r = s.execute("CREATE TABLE users (id INT PRIMARY KEY, name VARCHAR(64), age INT DEFAULT 0)");
r = s.execute("INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', 25)");
r = s.execute("SELECT name, age FROM users WHERE age > 28 ORDER BY age");
// r.column_names = {"name", "age"}
// r.rows[0] = {Value("Alice"), Value(30)}
```

### 方式 B:TCP server + mysql 客户端

```powershell
# 启动 server (开发期不校验密码,user=root 即可登录)
.\build\Debug\sqltiny_server.exe --port 3306 --user root

# 另开终端,连上去
mysql -h 127.0.0.1 -P 3306 -u root

# 或 Navicat / DBeaver,新建 MySQL 连接,填 Host=127.0.0.1 Port=3306 User=root
```

### CLI 参数

```
sqltiny_server [--port <n>] [--user <name>] [--password <pwd>]
               默认 --port 3306 --user root
```

> **开发期简化**: `mysql_native_password` 不校验密码,只要 user=root 就放行。caching_sha2_password 由 M4 实现。

### 当前支持的 SQL 子集

| 类别 | 语法 | 备注 |
|---|---|---|
| **DDL** | `CREATE TABLE` / `DROP TABLE` / `CREATE TABLE IF NOT EXISTS` / `DROP TABLE IF EXISTS` | PRIMARY KEY / AUTO_INCREMENT / DEFAULT / NOT NULL |
| **DML** | `INSERT INTO t VALUES(…, …)` / 多行 VALUES / `UPDATE t SET x=… WHERE …` / `DELETE FROM t WHERE …` | |
| **查询** | `SELECT cols FROM t WHERE … ORDER BY … LIMIT n` | IS NULL / IS NOT NULL / LIKE / IN / = / != / > / < / >= / <= / AND / OR / NOT |
| **聚合** | `COUNT(*)` / `COUNT(col)` / `SUM` / `AVG` / `MIN` / `MAX` | NULL 自动跳过 COUNT(*) 除外 |
| **表连接** | INNER JOIN / LEFT JOIN / CROSS JOIN | RIGHT/FULL JOIN 由 M3 补 |
| **函数** | NOW() / ABS(x) / LENGTH(s) / CONCAT(a,b,…) / COALESCE / IFNULL / ROUND / IN(…,…) / LIKE | |
| **SHOW** | `SHOW TABLES` / `SHOW COLUMNS FROM t` | |
| **事务** | (M3) `BEGIN` / `COMMIT` / `ROLLBACK` | |

### 已验证 SQL 示例

```sql
CREATE TABLE users (id BIGINT PRIMARY KEY AUTO_INCREMENT,
                    name VARCHAR(64) NOT NULL,
                    age INT DEFAULT 0);
INSERT INTO users VALUES (1, 'Alice', 30), (2, 'Bob', 25), (3, 'Charlie', 35);

SELECT COUNT(*) FROM users;                       -- 3
SELECT name, age FROM users WHERE age > 28 ORDER BY age;
SELECT name FROM users WHERE name LIKE 'B%';
UPDATE users SET age = 26 WHERE name = 'Bob';
DELETE FROM users WHERE age = 35;

CREATE TABLE t(a INT, b INT, name VARCHAR(20));
INSERT INTO t VALUES (10, 3, 'hello'), (7, 2, 'world');
SELECT a + b FROM t LIMIT 1;                      -- 13
SELECT ABS(-42);                                   -- 42  (无表 SELECT)
SELECT CONCAT(name, '-', a) FROM t WHERE a = 10;  -- "hello-10"
```

---

## 📁 目录结构

```
SQLTiny/
├── CMakeLists.txt             顶层构建
├── CODEPLAN.md                完整项目规划(架构映射 + 里程碑)
├── README.md                  本文件
├── vcpkg.json                 vcpkg manifest(仅依赖 fmt)
├── src/
│   ├── types/                 SqlType + Value 运行时值
│   ├── parser/                Lexer + AST + 递归下降 Parser
│   ├── storage/               AVL 内存索引 + 表存储
│   ├── catalog/               Catalog 单例(表注册 / 名字归一)
│   ├── core/                  Database / Session 入口
│   ├── executor/              Executor + Expr 求值 + 内置函数
│   ├── txn/                   MVCC (M3)
│   └── net/                   MySQL Wire Protocol 编解码 + TCP server
└── tests/                     smoke_m1 / smoke_m2 / smoke_m25_e2e (无 gtest 依赖)
```

---

## ❓ 常见问题

**Q: 启动 mysql 客户端时报错 `Can't connect`?**
A: 确认 `sqltiny_server.exe` 正在运行,监听 3306 端口:
```powershell
netstat -ano | findstr :3306
```

**Q: 密码登录失败?**
A: 开发期不校验密码,user 必须是 `root`,密码任意填(`mysql -u root -p` 直接回车也行)。

**Q: 支持哪些 MySQL 版本?**
A: 协议层按 MySQL 8.0 Client 41 实现,`mysql` CLI / Navicat / DBeaver / Connector/J 均已验证。

---

## 📜 许可证

Apache License 2.0 — 见 [`LICENSE`](./LICENSE) 文件。

## 🤝 致谢

- [HSQLDB](http://hsqldb.org/) — 架构和数据结构的重要参考(AVL 索引、MVCC、Session 分层)
- [MySQL Wire Protocol](https://dev.mysql.com/doc/dev/mysql-server/latest/) — 协议层参考
