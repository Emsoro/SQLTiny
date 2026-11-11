# SQLTiny CodePlan: HSQLDB → C++ 移植 + MySQL 协议兼容

> 目标:用现代 C++ (C++20) 重写 HSQLDB 核心引擎,构建一个轻量级嵌入式数据库 **SQLTiny**,同时暴露 **MySQL 客户端协议** 兼容端口——让标准 MySQL 客户端(mysql CLI、Navicat、JDBC Connector/J、Go sql.Open("mysql",...) 等)直接连接 SQLTiny。
>
> 源码输入:HSQLDB 2.7.4 `org.hsqldb`(Session / TransactionManagerMVCC / RowAction / persist)
> 当前工作区:`d:\Cplusplus\SQLTiny`

---

## 0. 当前状态(2026-11-11)

> 下文 M1–M5 为原始规划、M6 各节含设计稿伪代码,均保留供追溯。**落地状态以本节与各节 ✅ 标记为准;实际目录与运行方式见 `README.md`,架构图见 `architecture.svg`。**

### 交付进度

| 里程碑 | 状态 | 说明 |
|---|---|---|
| M1 / M2 / M2.5 / M3 | ✅ | 类型/AVL/Lexer、DDL/DML、MySQL 协议接入、子查询与 JOIN |
| M4 | ✅ | SHOW 系列、多库 Catalog、协议字段类型推断(`DESCRIBE` / `SHOW FULL TABLES` 仍缺) |
| M5 | ✅ | WAL + Snapshot + Replay、AUTO_INCREMENT、并发安全 |
| **M6 P0** | ✅ | 行锁 + `TransactionManager` + `Table.shared_mutex`,SELECT 走 MVCC 快照不加行锁 |
| **M6 P1 事务部分** | ✅ | 四隔离级别、autocommit、SAVEPOINT、WAL 事务缓冲、协议状态位 |
| M6 P1 边缘项 | ⬜ | `DESCRIBE` / `SHOW FULL TABLES`、`FOR UPDATE` / `LOCK IN SHARE MODE`、死锁检测 |
| M6 P2 / P3 / P4 | ⬜ | JOIN 优化、Prepared Statement / 认证、复制与更大测试覆盖 |

### 本轮交付(M6 事务 + 四隔离级别,参考 HSQLDB 2.7.4)

- **隔离级别**:`ast::IsolationLevel` 1/2/4/8;`SET [SESSION] TRANSACTION ISOLATION LEVEL`、`START TRANSACTION ... LEVEL`、`SET tx_isolation=` 真实生效;`@@tx_isolation` / `SHOW VARIABLES` 返回会话真实值。
- **HSQLDB 提升规则**:`READ UNCOMMITTED → READ COMMITTED`(下一事务级附加只读 25006)、`REPEATABLE READ → SERIALIZABLE`;四个级别都不允许脏读。
- **MVCC**:`VersionInfo` 增加 `pendingTxn / pendingInsert / pendingDelete`,未提交行只对所属事务可见;**commit 才 `finalize_commit` 发布版本**。RC 用语句级 SCN,RR/SER 用事务级 SCN;RR/SER 陈旧写与并发写冲突返回 40001(MySQL errno 1213)。
- **事务边界**:autocommit 语句级隐式 begin+commit、`SET AUTOCOMMIT=0/1`、`BEGIN` / `COMMIT` / `ROLLBACK`、`SAVEPOINT` / `ROLLBACK TO` / `RELEASE`、事务结束恢复会话默认隔离级别。
- **持久化修复**:未提交行 DML 进入会话级 `wal_buffer_`,commit 整体 flush、rollback 整体丢弃;checkpoint 只写已提交且未删除的行;修复回滚后重启复活数据。
- **配套修复**:UPDATE 两条 `RowAction` 的 rowId 错配、回滚主键索引恢复、`committed_queue_` 的 commitSCN 失效、OK/EOF 包 `SERVER_STATUS_AUTOCOMMIT` / `SERVER_STATUS_IN_TRANS` 硬编码。
- **架构图**:`architecture.svg` 改为靛蓝/青绿/琥珀/灰蓝四色语义化主题色,并同步到当前事务数据流。

### 验证基线

| 测试 | 结果 |
|---|---|
| `ctest -C Debug` | 9/9(含 `smoke_m6_isolation`、`smoke_m6_hsqldb_matrix`、`smoke_m6_session_db`) |
| `smoke_m6_isolation` | 451/451 PASS — 四级别、脏读、快照、冲突、autocommit、SAVEPOINT、DML 回滚 |
| `smoke_m6_hsqldb_matrix` | 56 PASS / 4 GAP — HSQLDB 可移植 SQL 语义矩阵;GAP 见 §8.4 |
| `smoke_m6_session_db` | PASS — 多库会话级隔离(每个连接的 `USE`/握手 db 互不干扰,验证 §8.4 #6/#7 修复) |
| `test_txn_vs_sqlite.py` | 70/70 与 SQLite 一致 + 2 项预期差异(RC 事务内语义、RU 提升 vs SQLite 脏读) |
| `test_full_flow.py` / `test_vs_sqlite.py` | 47 PASS / 0 FAIL / 1 GAP(GAP 见 §8.4)/ 全场景一致 |
| `test_wal_recovery.py` / `test_concurrency.py` | 重启恢复全通过 / 5 线程 15s 无 crash |
| `test_sqlalchemy.py` | 33 PASS / 0 FAIL / 0 GAP(原 #5–#8 与 DESCRIBE/SHOW FULL TABLES 反射限制均已修复) |

---

## 1. 架构总览

```
┌─────────────────────────────────────────────────────────┐
│                   MySQL Client (任意语言)                 │
└────────────────────────┬────────────────────────────────┘
                         │  MySQL Wire Protocol (TCP 3306)
                         ▼
┌─────────────────────────────────────────────────────────┐
│  ┌───────────────────────────────────────────────────┐  │
│  │          MySQL Protocol Adapter (新写,MySQL 协议)   │  │
│  │   Handshake / Auth / COM_QUERY / COM_STMT_*        │  │
│  └────────────────────┬──────────────────────────────┘  │
│                       │ 内部 C++ API                      │
│  ┌────────────────────▼──────────────────────────────┐  │
│  │          SQL Engine Core (从 HSQLDB Java 移植)       │  │
│  │  ┌──────────┬──────────┬──────────┬───────────┐   │  │
│  │  │ Parser    │ Executor │ Session  │ Catalog   │   │  │
│  │  └──────────┴──────────┴──────────┴───────────┘   │  │
│  │  ┌──────────┬──────────┬──────────┬───────────┐   │  │
│  │  │ Type Sys │ AVL Idx  │ MVCC Tx  │ Persist   │   │  │
│  │  └──────────┴──────────┴──────────┴───────────┘   │  │
│  └────────────────────────────────────────────────────┘  │
│                       │                                   │
│  ┌────────────────────▼──────────────────────────────┐  │
│  │          Storage Layer (内存 + 磁盘可选)             │  │
│  └───────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────┘
```

**关键设计决策:**
1. **协议层与引擎层完全解耦** — MySQL 适配器只做协议编解码,核心引擎不依赖任何 MySQL 概念
2. **优先 In-Process 模式** — 里程碑 1-3 先做嵌入式使用(进程内调用),里程碑 4 再加网络监听
3. **复用 HSQLDB 的数据结构思想,不逐行翻译** — AVL 索引、MVCC、存储分层都保留,但用现代 C++ 重新表达
4. **SQL 方言默认 MySQL 风格** — Parser 支持 MySQL 常用语法(反引号、AUTO_INCREMENT、ENGINE=、LIMIT 等),同时兼容标准 SQL

---

## 2. HSQLDB → C++ 模块映射表

| Java 模块 | 代表类 | C++ 目标模块 | 说明 |
|---|---|---|---|
| **数据库内核** | `Database`, `DatabaseManager` | `core/database.{h,cpp}` | 单例式数据库实例,管理持久化 store / catalog / session |
| **会话与安全** | `Session`, `SessionManager`, `auth/*` | `core/session.{h,cpp}`, `auth/` | 连接上下文、用户、隔离级别、ACL |
| **类型系统** | `types/Type.java`, `ColumnBase` | `types/type.{h,cpp}`, `types/column.{h,cpp}` | 核心 SQL 类型枚举 + 值对象 `Value`(variant) |
| **SQL Parser** | `ParserBase`, `ParserDML`, `ParserDDL`, `Scanner` | `parser/lexer.{h,cpp}`, `parser/parser.{h,cpp}`, `parser/ast.h` | 手写递归下降,Token 流 → AST |
| **语句与执行** | `Statement`*(StatementDML, StatementQuery, …)*, `StatementManager` | `executor/stmt.{h,cpp}` | AST 解释执行(第一版);后续可加物理计划 |
| **表与行** | `TableBase`, `Table`, `RowAVL`, `Row` | `storage/table.{h,cpp}`, `storage/row.{h,cpp}` | 表元数据 + 行存 |
| **索引** | `index/IndexAVL`, `NodeAVL` | `storage/index_avl.{h,cpp}` | AVL 树,内存优先,磁盘可选 |
| **事务** | `TransactionManagerMVCC`, `TransactionManagerCommon` | `txn/mvcc.{h,cpp}` | SCN + 可见性判断,第一版支持 READ COMMITTED |
| **Schema/Catalog** | `Schema`, `SchemaManager`, `HsqlNameManager` | `catalog/catalog.{h,cpp}` | 命名空间、对象注册、DDL 元数据 |
| **表达式/函数** | `Expression*`, `FunctionSQL` | `executor/expr.{h,cpp}`, `executor/builtin_funcs.cpp` | 表达式求值器 + 内置函数(ROUND、CONCAT、NOW 等) |
| **持久化** | `persist/*` (现文件未展开), `RowAVLDisk` | `storage/persist.{h,cpp}` | 第一版仅内存;里程碑 5 追加磁盘 WAL |
| **网络** | `server/Server.java`, `ClientConnection` | `net/mysql_server.{h,cpp}`, `net/mysql_proto.{h,cpp}` | 新写,非移植 |
| **错误/日志** | `error/Error.java`, `HsqlException` | `core/error.h` | `Result<T, SqlError>` 返回风格 |

### 落地偏差说明(以实际代码为准)

上表是立项时的目标路径,部分模块并未按该粒度拆分。`core/database.*` / `txn/mvcc.*` / `executor/stmt.*` / `executor/dml.*` / `net/auth_native.cpp` 未单独存在,职责集中在 `core/session.*`、`core/transaction.*`、`storage/table.*`、`executor/executor.*`、`net/mysql_proto.*`。M6 之后 HSQLDB 2.7.4 的隔离级别/事务语义映射为:

| HSQLDB 2.7.4 参考 | 实际落地 |
|---|---|
| `SessionInterface.TX_*`、`Session.setIsolation()` | `parser/ast.h` `IsolationLevel` + `core/session.cpp` `setIsolation()` / `readSCN()` |
| `RowAction.canRead()` | `storage/table.cpp` `row_visible()` / `for_each_visible(querySCN, readerTxn)` |
| `TransactionManagerMVCC` 提交与冲突检测 | `core/transaction.cpp` `commit_transaction()` / `has_snapshot_conflict()` |
| `Session.savepoints` / `rollbackSavepoint` | `core/session.h` `Savepoint{action_index, wal_count}` + `rollback_to_savepoint()` |
| `TransactionManagerCommon` SCN 分配 | `storage/wal_manager.cpp` `next_scn()`(全局单一计数器,不另设一套 SCN) |
| `persist/*` | `storage/wal_manager.cpp` + `storage/snapshot.cpp` |

> 上表"第一版支持 READ COMMITTED" 已过期:现已支持四个级别(RU→RC、RR→SER 按 HSQLDB 规则提升),详见 §0 当前状态与 §4 里程碑 M6 P1。

### 不移植的模块(第一版)
- JDBC 驱动(java.sql 接口)
- `jdbc/*` 整包 — C++ 端直接提供原生 C++ API
- `auth/LdapAuthBean`, `JaasAuthBean` — 第一版内置用户表
- `cmdline/SqlTool` — MySQL 客户端自带 CLI
- `types/Trigger`, 复杂 `Routine`/存储过程 — 里程碑 5 之后
- `navigator/`, `map/`, `rowio/`, `scriptio/` 等内部 IO 层 — 用 C++ std 文件流 + 自定义 buffer 替代

---

## 3. 目录结构(实际产物)

> 立项时的设计稿路径见 §2 落地偏差说明。以下为 `d:\Cplusplus\SQLTiny` 当前真实结构。

```
SQLTiny/
├── CMakeLists.txt              顶层构建 + CTest 注册
├── CODEPLAN.md                 完整项目规划(架构映射 + 里程碑)
├── architecture.svg            系统架构图(四隔离级别 + 事务数据流)
├── README.md                   使用说明 / 测试入口 / SQL 子集
├── vcpkg.json                  vcpkg manifest(仅依赖 fmt)
├── src/
│   ├── types/                  type.h · value.{h,cpp}
│   ├── parser/                 lexer.{h,cpp} · ast.{h,cpp} · parser.{h,cpp}
│   ├── catalog/                catalog.{h,cpp} — 多库 databases_[db].tables_
│   ├── storage/                table.{h,cpp} · index_avl.{h,cpp} · persist.{h,cpp}
│   │                           wal_manager.{h,cpp} · snapshot.{h,cpp}
│   ├── core/                   session.{h,cpp} · transaction.{h,cpp}
│   ├── executor/               executor.{h,cpp} · expr.cpp
│   ├── net/                    mysql_proto.{h,cpp} · mysql_server.{h,cpp}
│   └── main.cpp                server 入口
└── tests/
    ├── smoke_m1.cpp            Value / AVL / Lexer
    ├── smoke_m2.cpp            CRUD / 聚合 / 表达式 / DDL / NULL
    ├── smoke_m3.cpp            GROUP BY / HAVING / 子查询 IN / INSERT SELECT
    ├── smoke_m4.cpp            多库 / USE / SHOW / SET
    ├── smoke_m25_e2e.cpp       Winsock 直连 server E2E
    ├── smoke_m5_wal.cpp        WAL Replay + CRC + Checkpoint
    ├── smoke_m6_isolation.cpp  四隔离级别 + 事务 / 保存点 / 冲突
    ├── smoke_m6_hsqldb_matrix.cpp  HSQLDB 可移植 SQL 语义矩阵
    ├── test_full_flow.py       PyMySQL 全链路(双连接,含事务/隔离级别)
    ├── test_sqlalchemy.py      SQLAlchemy 2.x Engine/Core/ORM/Inspector
    ├── test_vs_sqlite.py       SQLTiny vs SQLite 行为对比
    ├── test_txn_vs_sqlite.py   四隔离级别 + 事务结果与 SQLite 对比 / WAL 重启
    ├── test_wal_recovery.py    server 重启恢复
    ├── test_perf_vs_sqlite.py  大数据量 INSERT + JOIN 性能对比
    └── test_concurrency.py     多线程并发读写
```

---

## 4. 里程碑与验收标准

遵循"先核心后边缘、先进程内后网络"的顺序。**实际验收**以自写 C++ smoke(CTest)+ PyMySQL / SQLAlchemy / SQLite 对比脚本为准;HSQLDB `TestSelf*.txt` 仅作语义参考,未直接运行其测试框架。各节下方的验收描述保留为当时的规划口径。

### M1 · 基础骨架与类型系统 ✅
**目标**:`Value` 类型可用,`AVL` 索引独立通过测试,`Lexer` 能把简单 SQL 切成 Token。

- [x] CMake + vcpkg 依赖约定(实际仅 fmtlib;测试用自写 smoke + CTest,未引入 spdlog/gtest)
- [x] `types/type.h` — SqlType 枚举(INT/BIGINT/DECIMAL/VARCHAR/TEXT/BLOB/DATE/DATETIME/BOOLEAN)
- [x] `types/value.{h,cpp}` — `using Value = std::variant<std::monostate, int64_t, double, std::string, Blob, DateTime>`,含比较/转换/toString
- [x] `storage/index_avl.{h,cpp}` — AVL 树,支持 insert/find/remove/range-scan,单元测试 > 100 cases
- [x] `parser/lexer.{h,cpp}` — 关键字(SELECT/INSERT/UPDATE/DELETE/CREATE/TABLE/ALTER/DROP/WHERE/ORDER/GROUP/LIMIT/IN/NOT/AND/OR/IS/NULL/TRUE/FALSE/BETWEEN/LIKE/AS/JOIN/LEFT/RIGHT/INNER/OUTER/ON/GROUP BY/HAVING/DISTINCT/UNION/VALUES/SET),标识符,数字,字符串(单引号),反引号(MySQL)
- [x] `tests/test_value.cpp`, `tests/test_avl.cpp`, `tests/test_lexer.cpp` 通过

**验收**:`ctest` 全绿;lexer 能正确处理 `SELECT id, name FROM \`users\` WHERE id = 1 AND name LIKE '张%' LIMIT 10;`

---

### M2 · Catalog + 简单 DDL/DML ✅
**目标**:内存表可 CREATE/INSERT/SELECT/UPDATE/DELETE,支持主键和索引。

- [x] `catalog/objects.h` — `TableDef`(列列表 + 主键 + 索引定义),`IndexDef`,`UserDef`
- [x] `catalog/catalog.{h,cpp}` — Schema 注册,按名查找
- [x] `parser/ast.h` — Variant 式 AST:`CreateTableStmt`, `InsertStmt`, `SelectStmt`, `UpdateStmt`, `DeleteStmt`, `ColumnDef`, `BinaryExpr`, `LiteralExpr`, `IdentExpr`…
- [x] `parser/parser.{h,cpp}` — 递归下降:DDL(CREATE TABLE / DROP TABLE / ALTER TABLE ADD COLUMN),DML 四件套;第一版 SELECT 支持 WHERE(AND/OR/比较)/ORDER BY/LIMIT/GROUP BY/HAVING/聚合(COUNT/SUM/AVG/MAX/MIN)/INNER JOIN
- [x] `executor/stmt.{h,cpp}` — 各 Statement 子类的 `execute(Session&) -> ResultSet`
- [x] `executor/expr.{h,cpp}` — 表达式求值 + 内置函数(ROUND,ABS,LENGTH,CONCAT,NOW,CURDATE,IFNULL/COALESCE,CASE WHEN)
- [x] `storage/table.{h,cpp}` — `Table` 持有 `TableDef` + 多个 AVL 索引;INSERT 写所有索引,DELETE 从所有索引移除,UPDATE 先删后插
- [x] `core/session.{h,cpp}` — 最小会话(绑定 catalog、事务号分配)

**验收**:`test_dml.cpp` 通过下列场景:
```sql
CREATE TABLE users (id BIGINT PRIMARY KEY, name VARCHAR(64), age INT);
CREATE INDEX idx_name ON users(name);
INSERT INTO users VALUES (1,'Alice',30),(2,'Bob',25),(3,'Charlie',35);
SELECT * FROM users WHERE age > 28 ORDER BY name;     -- → Alice, Charlie
UPDATE users SET age = 26 WHERE name='Bob';
DELETE FROM users WHERE id = 3;
SELECT COUNT(*) FROM users;                            -- → 2
DROP INDEX idx_name ON users;
DROP TABLE users;
```

---

### M2.5 · 最小可用 MySQL 协议适配器 ✅
**目标**:mysql CLI 能连上,跑通 M2 的全部 SQL;尽早暴露 MySQL 方言兼容性问题。

> 为什么放在 M2 之后?协议层是纯独立模块,不依赖 MVCC/子查询;尽早让真实客户端介入,SQL 方言调优和边界 case 暴露远比单元测试有效。

参考 MySQL 官方协议文档 `https://dev.mysql.com/doc/dev/mysql-server/latest/`:

- [x] `net/mysql_proto.{h,cpp}` — `PacketReader` / `PacketWriter`,length-encoded integer / length-encoded string / packet 序号与压缩支持
- [x] **握手阶段**:Initial Handshake Packet(protocol_version=10,server_version=SQLTiny-1.0,thread_id,auth_data=20 字节随机串,capability flags:CLIENT_PROTOCOL_41 | CLIENT_SECURE_CONNECTION | CLIENT_PLUGIN_AUTH | CLIENT_CONNECT_WITH_DB | CLIENT_DEPRECATE_EOF)→ HandshakeResponse → 校验用户名 + **mysql_native_password**(第一版,后续追加 caching_sha2_password)
- [x] **命令处理**:COM_QUERY(字符串 → Session::execute)、COM_PING、COM_QUIT、COM_INIT_DB(USE db)、COM_FIELD_LIST(SHOW COLUMNS)
- [x] **结果包**:ResultSet 头(column_count) → N 个 Column Definition Packet(Table / Column / Type / Flags) → EOF → N 个 Row Packet(按列类型 length-encoded 或 NULL bitmap) → EOF
- [x] **OK / ERR 包**:affected_rows + last_insert_id + status_flags + ErrorCode + SQL State + Message
- [x] `net/mysql_server.{h,cpp}` — `accept()` 循环,每连接一个 `Connection` 对象持有 Session,thread-per-connection
- [x] `main.cpp` — `--port 3306 --user root --password ""`

**验收**:`mysql -h 127.0.0.1 -P 3306 -u root` 能连,跑:
```sql
CREATE TABLE users (id BIGINT PRIMARY KEY, name VARCHAR(64));
INSERT INTO users VALUES (1,'Alice'),(2,'Bob');
SELECT * FROM users WHERE name LIKE 'A%';
UPDATE users SET name='Alice2' WHERE id=1;
DELETE FROM users WHERE id=2;
```
Navicat / DBeaver 也能连上执行。**但暂不支持**:Prepared Statement、SSL、子查询、LEFT JOIN(这些由 M3 引擎 + M4 协议补全)。

---

### M3 · MVCC 事务 + 子查询 ✅
**目标**:事务隔离 READ COMMITTED,支持子查询、UNION、LEFT JOIN;MySQL 客户端此刻已能连上,直接验证引擎升级。

- [x] `txn/txn.{h,cpp}` — SCN 全局递增;BEGIN / COMMIT / ROLLBACK / SAVEPOINT
- [x] `txn/mvcc.{h,cpp}` — 每行保存 createSCN / deleteSCN;可见性规则(读 committed)
- [x] SELECT 增强:LEFT JOIN / RIGHT JOIN / FULL JOIN / CROSS JOIN / 子查询(WHERE IN / WHERE = / FROM 子查询) / UNION [ALL] / DISTINCT
- [x] `INSERT … SELECT …`
- [x] 聚合函数 + GROUP BY + HAVING 正确处理 NULL 组
- [x] 简单约束:NOT NULL,UNIQUE,CHECK,FOREIGN KEY(MATCH SIMPLE,RESTRICT/NO ACTION)
- [x] `tests/test_mvcc.cpp` — 两线程并发读写验证隔离级别

**验收**:`test_mvcc.cpp` 并发测试无脏读无不可重复读;下列 SQL 通过 mysql CLI 执行正确:
```sql
SELECT u.name, COUNT(o.id) AS cnt
FROM users u LEFT JOIN orders o ON o.user_id = u.id
WHERE u.age > 20
GROUP BY u.id
HAVING cnt > 1
ORDER BY cnt DESC
LIMIT 10;
```

---

### M4 · 协议层补全 ✅
**目标**:SHOW 系列 + 多库 Catalog,让图形客户端和 Python SQLAlchemy 连接顺滑。

- [x] `SHOW TABLES` / `SHOW CREATE TABLE` / `SHOW DATABASES` / `SHOW STATUS` / `SHOW VARIABLES`(含 LIKE pattern 过滤)
- [x] `USE db`, `CREATE DATABASE`, `DROP DATABASE`
- [x] Catalog 多库化:库级独立表注册 `databases_[db].tables_`
- [x] SQLAlchemy 2.x 兼容:系统变量、`VERSION()` / `SCHEMA()` 函数、`COMMIT` / `ROLLBACK` / `BEGIN` 占位(M4 时均为占位;M6 P1 后 `@@tx_isolation` / `@@autocommit` 返回会话真实值,事务语句换成真实语义)
- [x] MySQL wire protocol 字段类型动态推断(DECIMAL/VARCHAR/INT/DOUBLE 正确 field_type)
- [ ] `COM_STMT_PREPARE` / `COM_STMT_EXECUTE` — 二进制协议(→ M6 P3)
- [ ] `INFORMATION_SCHEMA` 视图(→ M6)
- [ ] `caching_sha2_password` 认证(→ M6)
- [ ] `CLIENT_SSL` 握手(→ M6)

**验收**:Navicat 打开连接不弹警告,能列出数据库/表/列;SQLAlchemy 全场景 PASS(现 `test_sqlalchemy.py` 33 PASS / 0 FAIL / 0 GAP,见 M6 P1 1.2 与 §8.4 #5–#8 修复记录)。

---

### M5 · 持久化 + MVCC + SQLAlchemy 全兼容 ✅
**目标**:重启不丢数据,事务 READ COMMITTED,SQLAlchemy 可直接用 SQLTiny 作 MySQL server。

- [x] **WAL + Replay**:启动时先读 `sql.data` snapshot 再 replay 增量 `sql.wal`
- [x] **Checkpoint + Snapshot**:写 `sql.data`(metadata section + row records),truncate WAL;shutdown 自动 do_checkpoint
- [x] `INTEGER AUTO_INCREMENT`(MySQL 风格,对应 HSQLDB IDENTITY)
- [x] **MVCC SCN**:全局原子递增 SCN,Row 保存 `createSCN` / `deleteSCN`(M6 起统一由 `WalManager::next_scn()` 分配)
- [x] `for_each_visible(querySCN)`:READ COMMITTED 可见性过滤(M6 起增加 `readerTxn` / `pendingTxn`,commit 才发布版本)
- [x] **并发安全**:Session::execute 入口加全局 `std::mutex`(M5 时的粗粒度串行化;M6 P0 已改为行锁 + `Table.shared_mutex` + `TransactionManager.writeLock_`)
- [x] **SQLAlchemy 2.x**:Engine/Core/ORM/Inspector 核心路径 8/10 PASS,`DESCRIBE` / `SHOW FULL TABLES` 留待 M6
- [x] **4 个集成测试**:
  - `test_sqlalchemy.py` — SQLAlchemy 全场景(8/10 PASS,2 非核心 FAIL)
  - `test_vs_sqlite.py` — SQLTiny vs SQLite 全场景行为一致
  - `test_perf_vs_sqlite.py` — 大数据量 INSERT + JOIN 性能对比(无 crash)
  - `test_concurrency.py` — 5 线程 15s 并发读写(全局 mutex 生效无 crash)

**验收**:server 重启后 `SELECT COUNT(*)` 与 shutdown 前一致;并发测试 15s 无 crash;SQLAlchemy ORM `s.get(User, 1)` 可用。

---

### M6 · 行锁 + 事务 + JOIN 优化 + 存储过程(按优先级)

> **进度(2026-11-11)**:P0 ✅ 已完成 · P1 事务语义 ✅ 已完成,边缘项 ⬜ 待办 · P2 / P3 / P4 ⬜ 未启动。详见 §0 当前状态。

---

#### P0 · 行级锁 + 事务管理器 ✅ 已完成(参考 HSQLDB TransactionManagerMVCC + Derby LockManager)

> **【2026-11-11 更新】** 本节 0.0–0.5 与下方伪代码保留为立项设计稿;**实际实现以 `src/core/transaction.*`、`src/core/session.*`、`src/storage/table.*` 为准**。设计稿里"当前全局 mutex 串行化"的描述是 M5 时的状态,M6 P0 已移除。

> 原问题描述(M5 时):当前 `Session::execute` 用全局 `std::mutex` 串行化所有 SQL,语义正确但吞吐为 1。HSQLDB 同一架构(MVCC + AVL + WAL)用 `ReentrantReadWriteLock` 全局 writeLock 保护事务边界,但 **SELECT 不持锁**(快照隔离天然支持)。Derby 用传统 2PL,行锁 + 锁表 + 死锁检测。SQLTiny 走 HSQLDB 路线(已用 MVCC SCN),但需要补齐事务基础设施。

**落地结论(与设计稿的差异)**

| 设计稿 | 实际落地 |
|---|---|
| `row_id → vector<RowVersion>` 多版本链 | 单版本 `row_map_` + `VersionInfo{createSCN, deleteSCN, pendingTxn, pendingInsert, pendingDelete}`;靠 SCN 与 pending 标记做快照,不保留历史版本链 |
| `TransactionManager` 自带 `atomic nextSCN_` | 复用 `WalManager::next_scn()` 单一计数器,避免两套 SCN 造成可见性错乱 |
| `committedQueue_` 存 `vector<RowAction*>`,仅用于 merge | `CommittedTxn{commitSCN, owner, actions}`,同时供 RR/SER 提交时 `has_snapshot_conflict()` 检测;无活跃事务时由 `merge_expired_transactions()` 清理 |
| 行锁放 `Table::row_locks_` | `TransactionManager::table_locks_`(按 Table* 索引的 `TableLockEntry`),锁生命周期与 Table 解耦 |
| 回滚 INSERT 靠 createSCN、回滚 DELETE 靠清 deleteSCN | `Table::erase_row()` 物理删除并摘除主键索引、`restore_row()` 恢复可见并回补索引;**逆序**撤销所有 action |
| commit 时追加 WAL | 未提交行 DML 先进会话 `wal_buffer_`,commit 整体 `flush_txn_buffer()`、rollback 整体丢弃;checkpoint 快照只写已提交且未删除行 |
| SELECT 完全不加锁 | 不加行锁 / writeLock,但仍由 `Table.mu_` shared_lock 保护容器遍历 |
| 行锁冲突即 abort | 冲突立即 40001(1213)中止;RR/SER 额外做提交时的陈旧写冲突检测 |

**验证**:`smoke_m6_isolation` 451/451 PASS、`test_txn_vs_sqlite.py` 70/70 与 SQLite 一致,`ctest` 7/7。

**参考源码**(本地已有):
- HSQLDB:`hsqldb/src/org/hsqldb/TransactionManagerMVCC.java`、`RowAction.java`、`TransactionManagerCommon.java`
- Derby:`db-derby-10.17.1.0-src/java/engine/org/apache/derby/store/`(锁管理器在 lower-level store 层)

##### 0.0 新增模块

| 新增 | 说明 | 参考 |
|---|---|---|
| `src/core/transaction.h` | `TransactionManager` 类 — 持有全局 `std::mutex writeLock_`(只在事务边界获取),管理 `live_tx_scns_`、`committed_tx_queue_`、SCN 分配 | HSQLDB `TransactionManagerCommon` + `TransactionManagerMVCC` |
| `src/core/session.{h,cpp}` 扩展 | Session 加 `transactionSCN`、`actionSCN`、`row_actions_`(vector<RowAction>),autocommit 标志 | HSQLDB `Session.java` 事务字段 |
| `storage/table.{h,cpp}` 扩展 | Table 的 `row_map_` 改为带版本链:`row_id → vector<RowVersion{Row, createSCN, deleteSCN, RowAction*}>` | HSQLDB `Row.rowAction` 单链;但 SQLTiny 当前是单版本,需要升级为多版本链才能真正快照隔离 |
| `storage/table.{h,cpp}` 加行锁 | `RowLockMap row_locks_;` — map<row_id, LockState>` | Derby LockManager |

##### 0.1 RowAction — 每行的事务操作记录(参考 HSQLDB RowAction.java)

```cpp
// 每条正在进行的写操作对应一个 RowAction
struct RowAction {
    enum Type { NONE, INSERT, DELETE, UPDATE_DELETE, UPDATE_INSERT };
    
    Type      type;
    Session*  owner;           // 哪个 session 做的
    uint64_t  actionSCN;       // 动作发生时分配的 SCN
    uint64_t  commitSCN = 0;   // 提交后的 SCN(提交前为 0)
    Row       row;             // 操作涉及的行(DELETE 的 deleteSCN, INSERT 的 createSCN)
    int64_t   rowId;           // 行主键
    std::string tableName;
    std::string dbName;

    // 冲突检测(参考 HSQLDB RowAction.canCommit)
    // 返回 true = 可以提交; false = 另一个 session 已经提交了冲突操作
    bool canCommit() const {
        // 同 rowId,另一 session 的 commitSCN > 0(已提交),且 type 冲突
        return !conflicts_with_other_committed_action();
    }
    
    // 可见性判断(参考 HSQLDB RowAction.canRead)
    bool canRead(uint64_t querySCN) const {
        if (commitSCN == 0) return false;           // 还没提交,不可见
        if (type == INSERT || type == UPDATE_INSERT)
            return commitSCN <= querySCN;
        if (type == DELETE || type == UPDATE_DELETE)
            return commitSCN > querySCN;            // 删除提交得比查询早 = 已对该查询不可见
        return true;
    }
};
```

**关键设计点**:
- INSERT 时 RowAction 在 `Table::insert` 里创建,加入 `session->row_actions_`
- DELETE 时 RowAction 在 `Table::delete_by_pk` 里创建,加入 `session->row_actions_`
- UPDATE = DELETE(旧值)+ INSERT(新值),产生两个 RowAction
- SELECT **不创建 RowAction**(MVCC 快照隔离的优势!)

##### 0.2 TransactionManager — 全局事务管理器(参考 HSQLDB TransactionManagerMVCC)

```cpp
class TransactionManager {
    std::mutex writeLock_;                          // 事务边界全局串行化(参考 HSQLDB writeLock)
    std::deque<uint64_t> liveTxSCNs_;               // 未结束事务的 SCN 队列
    std::deque<std::vector<RowAction*>> committedQueue_;  // 已提交但未过期的事务 action 数组
    std::atomic<uint64_t> nextSCN_{1};

    // —— 事务生命周期 ——
    
    uint64_t beginTransaction(Session* s) {
        std::lock_guard lock(writeLock_);
        uint64_t scn = nextSCN_.fetch_add(1);
        s->transactionSCN = scn;
        liveTxSCNs_.push_back(scn);
        return scn;
    }

    bool commitTransaction(Session* s) {
        std::lock_guard lock(writeLock_);
        // 1. 冲突检测:session.row_actions_ 里每个 action.canCommit()
        for (auto* a : s->row_actions_)
            if (!a->canCommit()) return false;   // 40001 Serialization Failure
        
        // 2. 分配 commitSCN
        uint64_t commitSCN = nextSCN_.fetch_add(1);
        for (auto* a : s->row_actions_) a->commitSCN = commitSCN;
        s->actionSCN = commitSCN;

        // 3. 更新 Table 的版本链(deleteSCN = commitSCN, 或 createSCN = commitSCN)
        for (auto* a : s->row_actions_) {
            if (a->type == DELETE || a->type == UPDATE_DELETE)
                a->row.deleteSCN = commitSCN;     // 让这行对后续 commitSCN > x 的查询不可见
            if (a->type == INSERT || a->type == UPDATE_INSERT)
                a->row.createSCN = commitSCN;     // 让这行对后续 commitSCN >= x 的查询可见
        }
        
        // 4. WAL 追加(已有,不变) + 持久化
        // 5. 放入 committedQueue,等到没有 live transaction scn 小于 commitSCN 时 merge
        committedQueue_.push_back(s->row_actions_);
        
        // 6. 从 live queue 移除 + merge 过期
        endTransaction(s);
        return true;
    }

    void rollbackTransaction(Session* s) {
        std::lock_guard lock(writeLock_);
        uint64_t rbSCN = nextSCN_.fetch_add(1);  // rollback 也占 SCN
        
        // 回滚每个 action:
        // - INSERT → 物理删除(createSCN 仍为 actionSCN 但没有 commitSCN,永远不可见)
        // - DELETE → 把 deleteSCN 设 0(恢复可见)
        // - UPDATE → DELETE 回退 INSERT 也回退
        
        for (auto* a : s->row_actions_) {
            if (a->type == INSERT || a->type == UPDATE_INSERT) {
                // 从 Table 版本链移除(createSCN = actionSCN 但没 commitSCN)
                table_rollback_insert(a);
            } else if (a->type == DELETE || a->type == UPDATE_DELETE) {
                // 恢复 deleteSCN = 0
                table_rollback_delete(a);
            }
        }
        
        endTransaction(s);
    }

    // live transaction SCN 最小的还在排队的时候,不能 merge committed actions
    // 否则新启动的 transaction 会看到不该看到的旧版本
    void mergeExpiredTransactions() {
        while (!committedQueue_.empty()
               && !liveTxSCNs_.empty()
               && committedQueue_.front()[0]->commitSCN < liveTxSCNs_.front()) {
            auto actions = committedQueue_.front();
            committedQueue_.pop_front();
            // 把这些 action 从 Table 版本链的 pending 状态 finalize
            for (auto* a : actions) finalize_row(a);
        }
    }
    
    uint64_t getNextSCN() { return nextSCN_.fetch_add(1); }
    uint64_t currentSCN() { return nextSCN_.load(); }
};
```

**SQLTiny 关键简化点**(对比 HSQLDB):
- 不需要 RowAction 链(每行只属于一个写 session,commit 后 merge 掉)
- 不需要 `tableWriteLocks` / `tableReadLocks`(先只做行锁,DDL 仍然拿全局写锁)
- 不需要 latch 等待队列(行锁冲突先简单 abort,后续加 redo 等待)

##### 0.3 行锁模型(参考 Derby LockManager,精简版)

```cpp
// Table 内维护每行的锁状态
struct RowLock {
    enum State { FREE, SHARED_READ, EXCLUSIVE_WRITE };
    State state = FREE;
    Session* owner = nullptr;   // EXCLUSIVE 时持有;SHARED 时不填(多个 session 共享)
    int     sharedCount = 0;
};

// INSERT / UPDATE / DELETE 时拿 EXCLUSIVE
bool Table::try_lock_row_write(int64_t rowId, Session* s) {
    std::lock_guard tableLock(table_mutex_);   // table 级读写锁
    auto& lk = row_locks_[rowId];
    if (lk.state == FREE) {
        lk.state = RowLock::EXCLUSIVE_WRITE;
        lk.owner = s;
        return true;
    }
    if (lk.state == RowLock::EXCLUSIVE_WRITE && lk.owner == s) return true;  // 重入
    // 冲突!行被其他 session 写着
    return false;
}

// SELECT 只在 autocommit + FOR UPDATE 时加 SHARED,否则快照隔离不加锁
// 这是 MVCC 的精髓 — SELECT 不阻塞写,写不阻塞 SELECT(除非显式 FOR UPDATE)
```

##### 0.4 `Session::execute` 流程(P0 之后替换 g_execute_mutex)

> 以下仍是设计稿伪代码(其中的 `Begin` / `Commit` 提前 return 会丢弃同批次语句,异常时也会误回滚显式事务)。实际实现见 `src/core/session.cpp`,要点列在本节末尾。

```cpp
ResultSet Session::execute(const std::string& sql) {
    parser::Parser p(sql);
    auto stmts = p.parse_all();
    
    // autocommit 模式:每条语句隐式 begin + commit
    // 显式 BEGIN 后 autocommit 关闭,直到 ROLLBACK/COMMIT
    bool implicitBegin = autocommit_ && !isInTransaction();
    
    if (implicitBegin) {
        beginTransaction();       // txMgr.begin(this)
    }
    
    try {
        for (auto& s : stmts) {
            if (s->kind == ast::StmtKind::Commit)   { commitTransaction();   return ok(); }
            if (s->kind == ast::StmtKind::Rollback) { rollbackTransaction(); return ok(); }
            if (s->kind == ast::StmtKind::Begin)    { beginTransaction();    return ok(); }
            
            auto last = exec_.execute(*s);
            if (!last.ok) { rollbackTransaction(); return last; }
            
            // DML:拿行锁,创建 RowAction
            if (s->kind == ast::StmtKind::Insert
             || s->kind == ast::StmtKind::Delete
             || s->kind == ast::StmtKind::Update) {
                // 具体拿锁逻辑在 executor 内部调用 txMgr
            }
        }
        
        if (implicitBegin) {
            commitTransaction();   // 隐式提交
        }
        return last;
    } catch (const std::exception& e) {
        if (isInTransaction()) rollbackTransaction();
        return err(e.what());
    }
}
```

**实际 `Session::execute` 流程(M6 P1 后)**

1. `SET` / `BEGIN` / `START TRANSACTION` / `COMMIT` / `ROLLBACK` / `SAVEPOINT` 系列在会话层拦截,不进入 Executor。
2. 普通语句:autocommit 时 `begin → execute → commit` 逐语句;`SET AUTOCOMMIT=0` 时隐式 begin 并保持到显式 COMMIT/ROLLBACK。
3. 读 SCN 由 `Session::readSCN()` 给出:RC 取最新已提交 SCN,RR/SER 取 `transactionSCN`;Executor 传给 `for_each_visible(querySCN, readerTxn)`,DML 传 `writerTxn`。
4. 语句失败只回滚本语句的隐式事务,显式事务保持开启;RR/SER 冲突时 `commitTransaction()` 返回 false 并整体回滚,对外 40001。
5. `finishTransaction()` 统一 flush/丢弃 `wal_buffer_`、清空 `row_actions_` / `savepoints_`、恢复会话默认隔离级别;`WalBufferScope` 保证多 Session 交错调用时缓冲归属正确。

##### 0.5 死锁检测(参考 Derby)

SQLTiny 第一版可以简化:
- 行锁冲突时先 **abort 当前事务**(40001 Serialization Failure),让客户端重试
- 暂时不做真正的等待队列 + 循环等待检测(HSQLDB 的 `checkDeadlock` + latch wait 可以后续加)
- 好处:实现简单,和 HSQLDB MVCC 下 READ COMMITTED 遇到冲突的处理方式一致(HSQLDB 有 redo 等待但 SQLTiny 先用 abort)

---

#### P1 · SQL 标准事务完整化 + SQLAlchemy 最后 2 个 FAIL(事务部分 ✅ / 边缘项 ⬜)

> **【2026-11-11 更新】** 事务语义部分已完成并验证;下表 ✅ 为已落地、⬜ 为仍待办。`DESCRIBE` / `SHOW FULL TABLES` 两项 SQLAlchemy FAIL 仍未修复。

##### 1.1 事务功能落地状态

| 功能 | 状态 | 实际行为 | HSQLDB 对应 |
|---|---|---|---|
| `BEGIN` / `START TRANSACTION` | ✅ | 显式开启;重复 BEGIN 隐式提交当前事务;`START TRANSACTION ISOLATION LEVEL` 只对该事务生效 | `Session.begin()` |
| `COMMIT` | ✅ | 冲突检测 → commitSCN → finalize 行版本 → flush WAL → 释放行锁 | `TransactionManager.commitTransaction` |
| `ROLLBACK` / `ROLLBACK TO SAVEPOINT` | ✅ | 逆序撤销 action;保存点只撤销尾部 action 并截断 WAL 缓冲 | `TransactionManager.rollback` / `rollbackSavepoint` |
| `SAVEPOINT name` / `RELEASE SAVEPOINT` | ✅ | 记录 `action_index` + `wal_count`;同名取最近一个,RELEASE 后不存在返回 1305 | `Session.savepoints` |
| `SET AUTOCOMMIT=0/1` | ✅ | OFF 时隐式开启事务,ON 时提交当前事务;`@@autocommit` 与 OK/EOF 状态位同步 | `Session.autocommit` |
| `SET [SESSION] TRANSACTION ISOLATION LEVEL` | ✅ | 四个级别;会话默认 vs 仅下一事务;事务中已写时拒绝(25001) | `Session.setIsolation()` |
| 四个隔离级别 + HSQLDB 提升规则 | ✅ | RU→RC(+只读 25006)、RC 语句快照、RR→SER、SER 事务快照 + 40001 | `Session.isolationLevel` / `RowAction.canRead()` |
| `FOR UPDATE` | ⬜ | 未实现 | Derby `FOR UPDATE` 行锁 |
| `SELECT ... LOCK IN SHARE MODE` | ⬜ | 未实现 | MySQL 兼容 |
| 死锁检测 / 等待队列 | ⬜ | 当前冲突立即 40001 中止,不做等待与循环检测 | Derby 死锁检测 |

##### 1.2 SQLAlchemy 反射类语句(✅ 已落地 9e752ba)

> **【2026-11-11 更新】** `test_sqlalchemy.py` 补全至 15 组 33 项断言,并随协议/会话层修复(§8.4 #5–#8)一并实现了反射语句,当前为 **33 PASS / 0 FAIL / 0 GAP**。`DESCRIBE` / `SHOW FULL TABLES` / `SHOW COLUMNS` 均已在 server 端支持,`get_table_names` / `get_columns` 正常返回。

| 语句 | 状态 | 说明 |
|---|---|---|
| `DESCRIBE table` | ✅ | parser 加 `DESCRIBE` 关键字解析 → 内部转调 `SHOW COLUMNS FROM table` |
| `SHOW FULL TABLES FROM db` | ✅ | parser 加可选 `FULL` 关键字,`exec_show_tables` 返回 `table_type`(BASE TABLE / VIEW) |
| `SHOW COLUMNS` 类型名规范化 | ✅ | 类型名(`int`/`varchar(32)` 等)已可被 SQLAlchemy 反射层识别,不再 `NullType()` 报错 |

##### 1.3 事务与隔离级别测试项(自设计)

> **勘误**:`testrun/hsqldb/TestSelfTransaction.txt` 实际测的是外键 `ON DELETE SET NULL/CASCADE` + autocommit/rollback,74 个脚本中并没有隔离级别 / SAVEPOINT / MVCC 用例。脚本格式(`/*uN*/`、`/*cN*/`、`/*r…*/`、`/*e*/`)与分类见 §8。下表用例参考 HSQLDB Java 层 `Session` / `TransactionManagerMVCC` / `RowAction` 语义**自行设计**,由 `smoke_m6_isolation.cpp` 与 `test_txn_vs_sqlite.py` 覆盖。

| 状态 | 测试 | 说明 | 覆盖位置 |
|---|---|---|---|
| ✅ | **P0-01** 行锁互斥 / 写-写冲突 | 并发 UPDATE 同一行,一个成功一个 40001;对应 `TestSelfTransaction.txt` insert 冲突 | `smoke_m6_isolation` t5、`test_txn_vs_sqlite` ww_* |
| ✅ | **P0-02** 写-读快照隔离 / 无脏读 | 四个级别均看不到未提交 INSERT / DELETE,自己可见自己的写 | `smoke_m6_isolation` t2、`test_txn_vs_sqlite` dirty_* |
| ✅ | **P0-03** 读-读不阻塞 | SELECT 不加行锁,`Table.mu_` shared_lock 允许多读者;2 个读线程并发无 crash(未做锁竞争断言) | `test_concurrency.py` |
| ✅ | **P0-04** 写-写同事务重入 | 同一事务内两次 UPDATE 同一行均成功 | `smoke_m6_isolation` t5 |
| ✅ | **P1-01** autocommit 模式 | ON 逐语句提交、OFF 事务持续、COMMIT 后其它会话可见;`@@autocommit` 真实反映 | `smoke_m6_isolation` t7 |
| ✅ | **P1-02** ROLLBACK 恢复 | 行数、原值、主键索引全部恢复;对应 `TestSelfTransaction.txt` rollback 恢复 | `smoke_m6_isolation` t8 |
| ✅ | **P1-03** SAVEPOINT | 回滚到 s1 只保留第一行;RELEASE 后返回 1305 | `smoke_m6_isolation` t9 |
| ⬜ | **P1-04** SELECT FOR UPDATE 行锁 | 依赖 P1 边缘项 `FOR UPDATE`,尚未实现 | — |
| ✅ | **P1-05** 四个隔离级别 RC / RR / SER | RC 语句快照可见新提交,RR/SER 事务快照不可见;与 SQLite 两种模式逐项比对 | `smoke_m6_isolation` t3/t4、`test_txn_vs_sqlite` 70 项 |
| ⬜ | **P1-06** 死锁检测(两行循环锁) | 当前无等待队列,冲突立即 40001 中止,不存在循环等待 | — |
| ✅ | **P1-07** 并发 UPDATE 同一行(40001) | 两会话同一行:一个 OK、一个 errno 1213 | `smoke_m6_isolation` t5/t6、`test_txn_vs_sqlite` ww_c2 |
| ✅ | **P1-08** 并发读写压测 | 5 线程(2W+2R+1D)15s 无 crash,最终状态完整可查询 | `test_concurrency.py` |
| ⬜ | **P1-09** DESCRIBE / SHOW FULL TABLES | 测试侧已改 GAP 标记绕行(见 1.2),server 端仍未实现 | `test_sqlalchemy.py` |
| ✅ | **P1-10** 重启不复活未提交数据 | 回滚/提交后重启,SQLTiny 与 SQLite 状态一致;WAL 事务缓冲 + 快照只写已提交可见行 | `test_txn_vs_sqlite.py` WAL 段、`smoke_m5_wal` |

> 本轮新增 `tests/smoke_m6_isolation.cpp`(451 项进程内断言)与 `tests/test_txn_vs_sqlite.py`(70 项 SQLTiny/SQLite 对比 + WAL 重启),覆盖上表绝大多数 ✅ 项;前者已注册 `CTest`。

---

#### P2 · JOIN 性能优化(当前 O(N×M) 无索引)
> 当前 join 是纯嵌套循环(500 users × 2000 orders = 100w 行 explode),200/800 数据量 INNER JOIN+GROUP BY 要 12s。

- [ ] **Index Nested Loop Join**:当 ON 条件是右表主键/索引列时,主表每行 O(log n) 查右表
- [ ] **Hash Join**:INNER JOIN 等值条件 + 右表无合适索引时,建 hash table 再 probe
- [ ] JOIN 顺序选择:多表 JOIN 时基于表大小粗略估算排列顺序
- [ ] `EXPLAIN`(最小版):输出 join type / 使用的索引 / 扫描行数

#### P3 · Prepared Statement + 边缘兼容
- [ ] `COM_STMT_PREPARE` / `COM_STMT_EXECUTE` / `COM_STMT_CLOSE` / `COM_STMT_RESET` — 二进制协议
- [ ] `caching_sha2_password` 认证(MySQL 8.0 默认插件)
- [ ] `CLIENT_SSL` 握手阶段
- [ ] `INFORMATION_SCHEMA.TABLES` / `INFORMATION_SCHEMA.COLUMNS` 视图
- [ ] 存储过程(PSM 子集):`CREATE PROCEDURE` / `CALL` / 变量 / IF / WHILE
- [ ] B+Tree 替代 AVL(大表范围扫描更快)

#### P4 · 远期
- [ ] 复制/主从(MySQL 二进制日志协议兼容)
- [ ] 更大的 HSQLDB TestSelf* 覆盖(Joins / Constraints / PrimaryKeys / FKModes / StoredProcedure)
- [ ] `EXPLAIN ANALYZE`
- [ ] 触发器高级(`FOR EACH STATEMENT`、`BEFORE`、`INSTEAD OF`)

---

## 5. 技术选型

| 类别 | 选择 | 理由 |
|---|---|---|
| 语言标准 | C++20 | concepts、ranges、std::variant、std::jthread,让接口更干净 |
| 构建 | CMake 3.20+ + vcpkg | 跨平台,Windows/VS2022 友好 |
| 测试 | 自写 smoke(C++) + CTest + PyMySQL / SQLAlchemy / sqlite3 对比脚本 | 实际未引入 GoogleTest / GMock;7 个 CTest 目标 + 8 个 Python 端到端脚本 |
| 日志 | `fmt::print` 输出到 stdout | 实际未引入 spdlog,保持单一外部依赖 |
| 格式化 | fmtlib | 不依赖 iostream(vcpkg.json 中唯一的依赖) |
| JSON | 未引入 | `nlohmann/json` 仍是可选,当前 SHOW CREATE TABLE 直接拼字符串 |
| 网络 | 直接写 Winsock / POSIX socket | 未引入 asio / Boost;`net/mysql_server.cpp` accept + 每连接 `std::thread` |
| 并发 | `std::mutex` / `std::shared_mutex` / `std::thread` | SCN 由 `WalManager` 在锁内单调分配(非 lock-free),与 WAL 共用一套计数器 |
| 编码 | UTF-8 贯穿;字符串内部 `std::string`(UTF-8),对外按 MySQL 协议转码 | MySQL 默认 utf8mb4 |

### 故意不引入的依赖
- 不使用 SQLite / RocksDB / LevelDB 作存储后端 —— 自己写,保持轻量
- 不使用 LLVM / bytecode 解释器 —— AST 直接解释执行足够快
- 不使用 Protobuf / FlatBuffers —— MySQL 协议自有编解码,手写即可
- 未引入 GoogleTest / GMock / spdlog / Boost.asio / nlohmann-json —— 测试用自写 smoke + CTest,日志用 fmt,网络用原生 socket
- SQLite 仅作为 `test_txn_vs_sqlite.py` / `test_vs_sqlite.py` 的行为对比基准,**不是**存储后端

---

## 6. 风险与对策

| 风险 | 概率 | 现状 / 对策 |
|---|---|---|
| HSQLDB 某些边界行为难以复刻 | 中 | 已对齐四隔离级别的提升规则与 MVCC 可见性;测试以 MySQL 兼容性为主,HSQLDB 行为作为回归基准 |
| MySQL 协议版本碎片化 | 低 | 当前模拟 5.7 协议(`5.7.0-SQLTiny`)+ `mysql_native_password`;`caching_sha2_password` / SSL 列在 M6 P3 |
| 并发 MVCC 冲突 | 中 | M6 P0/P1 已落地行锁 + 事务边界 + 40001;**剩余**:无等待队列,高冲突场景需客户端重试;`versions_` 未做压缩,长事务下内存增长 |
| JOIN 纯嵌套循环 O(N×M) | 高 | 已知瓶颈(200/800 数据量的 INNER JOIN+GROUP BY 约 12s);M6 P2 计划 Index Nested Loop / Hash Join |
| SQLAlchemy inspector 兼容 | 低 | `DESCRIBE` / `SHOW FULL TABLES` / `SHOW COLUMNS` 已在 9e752ba 落地(§8.4 #5–#8 一并修复),`get_table_names` / `get_columns` 正常使用 |
| AVL 大表性能不如 B+Tree | 低 | 当前继续用 AVL,替换计划列在 M6 P3,对外 `Index` 接口不变 |
| 磁盘持久化二进制兼容未来升级 | 中 | 文件头 magic + version 字段,启动时检查;不承诺跨大版本兼容 |

---

## 7. 里程碑交付节奏总览(实际)

```
  M1 ──► M2 ──► M2.5 ──► M3 ──► M4 ──► M5 ──► M6 P0 ──► M6 P1(事务)
 类型/   DDL/    协议    子查询   SHOW/    WAL     行锁 +    四隔离级别
 AVL/    DML/    接入    JOIN     多库     快照    事务管理  autocommit
 Lexer   Catalog                                  savepoint  WAL 缓冲
  ✅      ✅       ✅      ✅       ✅      ✅       ✅         ✅
 ─────────────────────────────────────────────────────────────────────►
  已完成:M1–M5 全部交付,M6 P0 与 P1 事务语义完成并纳入 CTest / Python 回归
  待办:  M6 P1 边缘项 → P2 JOIN 优化 → P3 协议/认证 → P4 远期
```

**下一步优先级(2026-11-11)**

1. **协议层收尾**:`DESCRIBE` / `SHOW FULL TABLES` / `SHOW COLUMNS` 反射(§8.4 #6/#7 关联项)已在 9e752ba 落地;`FOR UPDATE` / `LOCK IN SHARE MODE`、等待队列与死锁检测、Prepared Statement(`COM_STMT_PREPARE`)、`INFORMATION_SCHEMA` 仍为后续项。
2. **M6 P2**:JOIN 从 O(N×M) 升级为 Index Nested Loop / Hash Join,补 `EXPLAIN`。
3. **版本回收**:`merge_expired_transactions()` 目前只清理冲突检测队列,`versions_` 不做压缩,长事务/长生命周期进程内存会增长,需补表压缩。
4. **HSQLDB 语义 GAP**:补齐 §8.4 的三值逻辑、空集聚合、`COALESCE(SUM(...))` 与「SELECT 列表无聚合时的 GROUP BY/HAVING」,并按 §8.3 待补清单扩展 `TestSelfNot.txt` 真值表与解析器负向用例。
5. **M6 P3 / P4**:Prepared Statement、`caching_sha2_password` / SSL、`INFORMATION_SCHEMA`、B+Tree、复制与更大的 HSQLDB TestSelf* 覆盖。

---

## 8. 测试体系:用例分类与测试点

> 参考基线:`hsqldb-2.7.4/hsqldb/testrun/hsqldb/` 下 **74 个** `TestSelf*.txt` / `TestText*.txt` / `TestTriggers.txt` 脚本,由 `org.hsqldb.test.TestUtil.testScript()` 驱动。本节把它们按域归类,再对照 SQLTiny 现有用例整理分类、测试点与缺口。

### 8.1 HSQLDB 参考套件的格式与分类

| 标记 | 含义 |
|---|---|
| `/*uN*/` | 期望 DML 影响行数 `N` |
| `/*cN*/` | 期望结果集行数 `N`(不校验内容) |
| `/*r…*/` | 期望结果集内容(单行内联或多行精确比对,含 `NULL` 字面量) |
| `/*e*/` | 期望抛出 `SQLException`(这 74 个脚本里均未指定 SQLState,只判"必须报错") |
| `/*s*/` / `/*d*/` / `/*o*/` | 静默执行吞异常 / 只打印不执行 / 打印结果集 |

| 组 | 功能域 | 代表脚本(数量) | SQLTiny 可借鉴性 |
|---|---|---|---|
| G0 | 综合回归 / 语法与异常预期 | `TestSelf.txt`(53 KB)、`TestSelfIssues.txt`(2) | ✅ 可移植池 |
| G1 | 事务 / 提交回滚 / 崩溃恢复 | `TestSelfTransaction.txt`、`TestSelfImmediateShutdown*.txt`、`TestSelfNoDmlBug1/2.txt`(5) | ✅ 部分(脚本测的是 FK 级联 + autocommit,非隔离级别) |
| G2 | DDL / Schema / 持久化 / 名称解析 | `TestSelfCreate/Modify/Verify/AlterColumn.txt`、`Schema*`、`PersSchem*`、`SysTables.txt`、`3PartNames.txt` 等(23) | ⚠️ 抽象成多库 Catalog + 重启元数据 |
| G3 | 主键 / 唯一 / CHECK / 外键 / 字段限制 | `TestSelfPrimaryKeys/Constraints/CheckConstraints/FKModes/FieldLimits/Queries.txt`(6) | ✅ 可移植池 |
| G4 | 表达式 / NULL 三值逻辑 / 转换 / 函数 | `TestSelfNot.txt`(38 KB)、`Arithmetic/Casewhen/Conversions/InPredicateReferencing.txt` 等(8) | ✅ 最值得移植(8 类型 × 真值表) |
| G5 | 聚合 / GROUP BY / HAVING | `TestSelfGroupBy.txt`(1) | ✅ 可移植池 |
| G6 | JOIN | `TestSelfJoins.txt`、`TestSelfLeftJoin.txt`(2) | ✅ 可移植池 |
| G7 | 子查询 / UNION / 集合运算 | `TestSelfSubselects.txt`、`TestSelfUnions.txt`(2) | ✅ 部分 |
| G8 | DML 语法 | `TestSelfInsertDeleteQueries.txt`(1) | ✅ 可移植池 |
| G9 | 权限 / 用户 / 角色 / 序列授权 | `TestSelfGrantees/ColGrant/MultiGrants/RoleNesting/SeqRights*.txt` 等(9) | ❌ 超出范围 |
| G10 | 触发器 / 视图 | `TestTriggers.txt`、`TestSelfTriggers*.txt`、`TestSelfViews.txt` 等(6) | ❌ 超出范围 |
| G11 | 存储过程 / 函数 / 数组 / INTERVAL | `TestSelfStoredProcedure*.txt`、`Arrays.txt`、`Interval.txt` 等(5) | ❌ 超出范围 |
| G12 / G13 | 临时表 / TEXT 外部表 | `TestSelfTempTable1/2.txt`、`TestText01/02.txt`(4) | ❌ 超出范围 |

> **重要澄清**:这 74 个脚本里 **没有** `SAVEPOINT`、`SET … ISOLATION LEVEL`、`READ UNCOMMITTED/COMMITTED`、MVCC 并发用例,也**没有 MySQL 协议用例**;`TestSelfTransaction.txt` 实际测的是外键 `ON DELETE SET NULL/CASCADE` 与 `SET AUTOCOMMIT` / `ROLLBACK`。因此 §4 M6 P1 的事务/隔离级别用例并非移植自该脚本,而是参考 HSQLDB Java 层 `Session` / `TransactionManagerMVCC` / `RowAction` 语义**自行设计**,并与 SQLite 的两种快照模式逐项对比。

### 8.2 SQLTiny 现有用例分类矩阵

| 编号 | 分类 | 用例 | 主要测试点 | 结果 |
|---|---|---|---|---|
| C01 | 类型 / 值 / 索引 / Lexer | `smoke_m1.cpp` | `Value` 比较与转换、AVL insert/find/remove/range、Tokenizer 关键字与引号 | ✅ |
| C02 | DDL / 多库 Catalog | `smoke_m4.cpp`、`test_full_flow.py` | CREATE/DROP DATABASE/TABLE、`USE`、SHOW 系列、两段式 `db.table`、未知库 1049 | ✅ |
| C03 | DML / 表达式 / NULL / 全链路 | `smoke_m2.cpp`、`test_full_flow.py`、`test_vs_sqlite.py` | CRUD、WHERE/ORDER/LIMIT、聚合、内置函数、NULL 列、LIKE、错误码;`test_full_flow.py` 另跨 C02/C06/C08 覆盖 DDL、事务、四隔离级别、多库 | ✅ 47 PASS / 1 GAP、全场景一致 |
| C04 | 聚合 / 子查询 / JOIN | `smoke_m3.cpp` | GROUP BY / HAVING / 子查询 IN / `INSERT…SELECT` / 各类 JOIN | ✅ |
| C05 | HSQLDB 可移植 SQL 语义矩阵 | `smoke_m6_hsqldb_matrix.cpp` | A NULL 三值、B 空集聚合、C JOIN 基数、D 主键与语句原子性、E 多库隔离、F 事务内可见性 | ✅ 56 PASS / 4 GAP |
| C06 | 事务 / 四隔离级别 / MVCC | `smoke_m6_isolation.cpp`、`test_txn_vs_sqlite.py` | 四级别提升、无脏读、不可重复读、40001 冲突、autocommit、SAVEPOINT、DML 回滚、WAL 重启 | ✅ 451/451、70/70 对比 |
| C07 | WAL / 快照 / 恢复 | `smoke_m5_wal.cpp`、`test_wal_recovery.py` | Replay、CRC 截断、Checkpoint、重启后数据与 UPDATE/DELETE 状态 | ✅ |
| C08 | MySQL 协议 / 客户端兼容 | `smoke_m25_e2e.cpp`、`test_sqlalchemy.py` | 握手与 packet、COM_QUERY、OK/EOF/ERR;SQLAlchemy 15 组:Engine、Core DDL/CRUD、聚合、ORM、2.0 风格 select/insert/update/delete、`text():param`、Session rollback、relationship 级联、`engine.begin()` 事务块、类型映射(DateTime/Boolean/Float)、Inspector(`DESCRIBE`/`SHOW FULL TABLES` 已支持) | ✅ 33 PASS / 0 FAIL / 0 GAP |
| C09 | 并发 / 性能 | `test_concurrency.py`、`test_perf_vs_sqlite.py` | 5 线程 15s 读写一致性、大数据量 INSERT + JOIN 对比 | ✅ 无 crash |

**分层**:C01–C07 为进程内 C++ smoke(无需 server,纳入 `CTest`);C08–C09 需要 server(端口 3306 或由脚本自动起停的随机端口);`test_txn_vs_sqlite.py` 自带 server 生命周期管理。当前 `ctest` **8/8**。

### 8.3 参考 HSQLDB 补充的测试点

**已补进 C05(`smoke_m6_hsqldb_matrix.cpp`)**

| 组 | HSQLDB 来源 | 补充的测试点 |
|---|---|---|
| A | `TestSelfNot.txt`、`TestSelfInPredicateReferencing.txt` | `= NULL` / `IS NULL` / `IS NOT NULL`、`IN (…,NULL)`、`IN (子查询)`、比较排除 NULL |
| B | `TestSelfCasewhen.txt`、`TestSelf.txt:233-244` | 空表 `COUNT`、空集 `SUM/AVG/MAX`、`COALESCE` 兜底、`SUM` / `COUNT(col)` 跳过 NULL |
| C | `TestSelfJoins.txt`、`TestSelfLeftJoin.txt` | CROSS JOIN 基数 3×3、INNER/LEFT JOIN 基数、索引可用与全表扫描结果一致 |
| D | `TestSelfPrimaryKeys.txt`、`TestSelf.txt:245,409` | 重复主键 1062、多行 VALUES 的语句级原子性、指定列插入省略列默认 NULL |
| E | `TestSelfNameResolutionSchemas.txt`、`3PartNames.txt` | 多库同名表隔离、两段式 `db.table`、`USE` 未知库 1049 |
| F | `TestSelfTransaction.txt:4-22`、`TestSelf.txt:381-399` | 事务内自己的写可见、其它会话不可见、ROLLBACK 恢复行数与值 |

**待补清单(按优先级)**

| 优先级 | 测试点 | HSQLDB 来源 |
|---|---|---|
| 高 | NULL 三值逻辑完整真值表:8 种类型 × `AND/OR/NOT/BETWEEN/IN/ALL/ANY` | `TestSelfNot.txt`、`TestSelfInPredicateReferencing.txt` |
| 高 | 解析器负向用例约 40 条(非法语法必须报错且不破坏后续语句) | `TestSelf.txt:404-444` |
| 中 | 索引中立性:建索引 / 加 PK / DROP COLUMN / ADD COLUMN 前后同一组查询结果不变 | `TestSelfQueries.txt:41-62` |
| 中 | 字段尺寸与精度强制:CHAR(4) 超长、DECIMAL(10,2) 舍入、CAST 溢出 | `TestSelfFieldLimits.txt` |
| 中 | CHECK / NOT NULL 的三值逻辑(UNKNOWN 视为通过、FALSE 才报错) | `TestSelfCheckConstraints.txt`、`TestSelfConstraints.txt` |
| 中 | GROUP BY 合法性、GROUP BY 表达式、相关 EXISTS + HAVING、聚合与子查询/JOIN 组合 | `TestSelfGroupBy.txt` |
| 中 | SELECT 列表不含聚合函数时的 GROUP BY / HAVING 结果(当前为 GAP #4) | `TestSelfGroupBy.txt`、`test_full_flow.py` |
| 中 | 派生表、标量子查询无行返回 NULL、UNION/EXCEPT/INTERSECT 优先级 | `TestSelfSubselects.txt`、`TestSelfUnions.txt`、`TestSelfIssues.txt` |
| 中 | 重启后元数据 + AUTO_INCREMENT 游标 + 聚合结果一致 | `TestSelfVerify.txt`、`TestSelfCreate/Modify.txt` |
| 低 | 三段式名称解析、系统库保护、标识符大小写规范化 | `TestSelf3PartNames.txt`、`TestSelfNameResolutionSchemas.txt` |
| — | 隔离级别 / SAVEPOINT / MVCC 冲突 / MySQL 协议 | **HSQLDB 这 74 个脚本无覆盖**,由 C06 / C08 自设计维护 |

> G9–G13(触发器、视图、权限、存储过程、数组/INTERVAL、临时表、TEXT 表、SEQUENCE)整体标记为**超出 SQLTiny 当前范围**,不纳入补充计划;仅把 G2 的「跨库持久化 + 名称解析」抽象成 C02/E 组用例。

### 8.4 参考 HSQLDB 发现的当前语义差异(GAP)

C05(`smoke_m6_hsqldb_matrix.cpp`)与 `test_full_flow.py` 把与 HSQLDB / SQL 标准不一致的点记录为 `[GAP]`(不计入 FAIL),便于后续按优先级修复:

| # | 测试点 | HSQLDB 期望 | SQLTiny 当前实际 | 建议 |
|---|---|---|---|---|
| 1 | `WHERE NOT (a = NULL)` | 0 行(UNKNOWN 取反仍是 UNKNOWN) | 3 行(UNKNOWN 被当作 FALSE 取反) | 表达式求值引入三值逻辑 |
| 2 | 空集 `SUM` / `AVG` | NULL | 0 | 聚合无输入行时返回 NULL(`MAX` 已正确) |
| 3 | `COALESCE(SUM(id),0)` | 0 | 空结果集 | 补齐「聚合函数作为标量函数参数」的求值路径 |
| 4 | `SELECT user_id FROM orders GROUP BY user_id HAVING SUM(total)>100` | 2 行 `(1),(3)` | 5 行 `(1),(1),(2),(3),(4)`(逐行输出) | SELECT 列表不含聚合时也必须走 GROUP BY 路径并应用 HAVING;**同一查询把 `SUM(total)` 放入 SELECT 列表即返回正确的 `(1),(3)`**,说明分组/HAVING 逻辑本身可用,是路径选择的问题 |
| 5 | `SELECT COUNT(*) FROM orders WHERE status = :st`(SQLAlchemy `text()` 绑定) | 正常执行 | 1064 `unexpected token in expression: 'status'`(`status` 疑似被词法层当保留字,且仅在该位置冲突) | **✅ 已修复 9e752ba**:列名与保留字冲突处按标识符处理 |
| 6 | 连接握手 `database` 字段 | 连接即切到指定库(`SELECT DATABASE()` 返回该库) | 字段被忽略,`SELECT DATABASE()` 仍返回全局 current db | **✅ 已修复 9e752ba**:握手响应后执行等价 `USE <db>`(写入 session 状态) |
| 7 | 多连接 current database 隔离 | per-session(连接 A 的 `USE` 不影响连接 B) | **服务器全局单例**:B 未 `USE` 却看到 A 的库;A 的库被 B 的 `USE` 覆盖,后续查询落到错误的库 | **✅ 已修复 9e752ba**:current db 移入 Session 状态(`smoke_m6_session_db` 覆盖) |
| 8 | `DROP DATABASE` 当前库 | MySQL 允许删除当前连接所在库 | 报 1008 `database doesn't exist or is current database` | **✅ 已修复 9e752ba**:判定改为仅校验库存在,删除成功即不再报 1008 |
| 5–8 | 协议/会话层(见上) | — | — | **✅ 已于 commit `9e752ba` 全部修复**:`status` 关键字冲突、`USE` 握手生效、current db 移入 Session 状态、`DROP DATABASE` 当前库改判;并顺带实现 `DESCRIBE` / `SHOW FULL TABLES` / `SHOW COLUMNS`(见 M6 P1 1.2)。`test_sqlalchemy.py` 现为 33 PASS / 0 FAIL / 0 GAP |

> #1–#4 属 SQL 语义层,不影响 C06 的事务/隔离级别结论,列入 M6 P2 之后待办;**第 4 项是被旧脚本 `len(rows) >= 2` 恒真断言掩盖的缺陷,完善 full_flow 后才暴露**。#5–#8 协议/会话层缺陷与反射语句(`DESCRIBE` / `SHOW FULL TABLES` / `SHOW COLUMNS`)已在 `9e752ba` 一并修复,`smoke_m6_session_db.cpp` 与 `test_session_db.py` 覆盖多库会话隔离,`test_sqlalchemy.py` 升至 33 PASS / 0 FAIL / 0 GAP。
