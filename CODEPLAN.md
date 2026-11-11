# SQLTiny CodePlan: HSQLDB → C++ 移植 + MySQL 协议兼容

> 目标:用现代 C++ (C++20) 重写 HSQLDB 核心引擎,构建一个轻量级嵌入式数据库 **SQLTiny**,同时暴露 **MySQL 客户端协议** 兼容端口——让标准 MySQL 客户端(mysql CLI、Navicat、JDBC Connector/J、Go sql.Open("mysql",...) 等)直接连接 SQLTiny。
>
> 源码输入:`d:\Work\CPlusPlus\SQLTiny\hsqldb\src\org\hsqldb`(约 200+ Java 源文件)
> 输出目录:`d:\Work\CPlusPlus\SQLTiny\src\`

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

### 不移植的模块(第一版)
- JDBC 驱动(java.sql 接口)
- `jdbc/*` 整包 — C++ 端直接提供原生 C++ API
- `auth/LdapAuthBean`, `JaasAuthBean` — 第一版内置用户表
- `cmdline/SqlTool` — MySQL 客户端自带 CLI
- `types/Trigger`, 复杂 `Routine`/存储过程 — 里程碑 5 之后
- `navigator/`, `map/`, `rowio/`, `scriptio/` 等内部 IO 层 — 用 C++ std 文件流 + 自定义 buffer 替代

---

## 3. 目录结构(产物)

```
d:\Work\CPlusPlus\SQLTiny\
├── CMakeLists.txt                  # 顶层构建
├── src/
│   ├── core/
│   │   ├── database.{h,cpp}        # Database 实例
│   │   ├── session.{h,cpp}         # 会话上下文
│   │   ├── error.h                 # SqlError / Result<T,E>
│   │   └── logger.h                # 简单日志
│   ├── types/
│   │   ├── type.h                  # SqlType 枚举
│   │   ├── value.{h,cpp}           # 运行时值(monostate / i64 / double / string / blob / time / null)
│   │   └── column.h                # 列元数据
│   ├── parser/
│   │   ├── lexer.{h,cpp}           # Tokenizer
│   │   ├── token.h
│   │   ├── ast.h                   # AST 节点定义(Variant/Visitor)
│   │   └── parser.{h,cpp}          # 递归下降入口
│   ├── catalog/
│   │   ├── catalog.{h,cpp}         # Schema 注册中心
│   │   └── objects.h               # TableDef / IndexDef / SequenceDef / UserDef
│   ├── storage/
│   │   ├── row.h
│   │   ├── table.{h,cpp}           # 表 + 行集合
│   │   ├── index_avl.{h,cpp}       # AVL 内存索引
│   │   └── persist.{h,cpp}         # WAL / 数据文件(里程碑 5)
│   ├── txn/
│   │   ├── txn.{h,cpp}             # Transaction / SCN 分配器
│   │   └── mvcc.{h,cpp}            # 可见性判断
│   ├── executor/
│   │   ├── expr.{h,cpp}            # 表达式求值
│   │   ├── stmt.{h,cpp}            # AST → 执行
│   │   ├── dml.{h,cpp}             # SELECT/INSERT/UPDATE/DELETE
│   │   └── builtin_funcs.cpp
│   ├── net/                        # 里程碑 4 启动
│   │   ├── mysql_proto.{h,cpp}     # 编解码
│   │   ├── mysql_server.{h,cpp}    # TCP 监听 + 连接管理
│   │   └── auth_native.cpp         # mysql_native_password 验证
│   └── main.cpp                    # 服务器入口(里程碑 4)
├── tests/
│   ├── test_lexer.cpp
│   ├── test_parser.cpp
│   ├── test_value.cpp
│   ├── test_avl.cpp
│   ├── test_mvcc.cpp
│   ├── test_dml.cpp                # SQL 级集成测试
│   └── test_mysql_proto.cpp
└── docs/
    └── mysql_protocol_notes.md     # 协议抓包记录(里程碑 4)
```

---

## 4. 里程碑与验收标准

遵循"先核心后边缘、先进程内后网络"的顺序。每个里程碑结束都要跑 HSQLDB 自带 TestSelf*.txt 子集 + MySQL 客户端手动验证。

### M1 · 基础骨架与类型系统(预计 1 周)
**目标**:`Value` 类型可用,`AVL` 索引独立通过测试,`Lexer` 能把简单 SQL 切成 Token。

- [x] CMake + vcpkg 依赖约定(fmtlib、spdlog、gtest)
- [x] `types/type.h` — SqlType 枚举(INT/BIGINT/DECIMAL/VARCHAR/TEXT/BLOB/DATE/DATETIME/BOOLEAN)
- [x] `types/value.{h,cpp}` — `using Value = std::variant<std::monostate, int64_t, double, std::string, Blob, DateTime>`,含比较/转换/toString
- [x] `storage/index_avl.{h,cpp}` — AVL 树,支持 insert/find/remove/range-scan,单元测试 > 100 cases
- [x] `parser/lexer.{h,cpp}` — 关键字(SELECT/INSERT/UPDATE/DELETE/CREATE/TABLE/ALTER/DROP/WHERE/ORDER/GROUP/LIMIT/IN/NOT/AND/OR/IS/NULL/TRUE/FALSE/BETWEEN/LIKE/AS/JOIN/LEFT/RIGHT/INNER/OUTER/ON/GROUP BY/HAVING/DISTINCT/UNION/VALUES/SET),标识符,数字,字符串(单引号),反引号(MySQL)
- [x] `tests/test_value.cpp`, `tests/test_avl.cpp`, `tests/test_lexer.cpp` 通过

**验收**:`ctest` 全绿;lexer 能正确处理 `SELECT id, name FROM \`users\` WHERE id = 1 AND name LIKE '张%' LIMIT 10;`

---

### M2 · Catalog + 简单 DDL/DML(预计 1.5 周)
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

### M3 · MVCC 事务 + 子查询(预计 1.5 周)
**目标**:事务隔离 READ COMMITTED,支持子查询、UNION、LEFT JOIN。

- [x] `txn/txn.{h,cpp}` — SCN 全局递增;BEGIN / COMMIT / ROLLBACK / SAVEPOINT
- [x] `txn/mvcc.{h,cpp}` — 每行保存 createSCN / deleteSCN;可见性规则(读 committed)
- [x] SELECT 增强:LEFT JOIN / RIGHT JOIN / FULL JOIN / CROSS JOIN / 子查询(WHERE IN / WHERE = / FROM 子查询) / UNION [ALL] / DISTINCT
- [x] `INSERT … SELECT …`
- [x] 聚合函数 + GROUP BY + HAVING 正确处理 NULL 组
- [x] 简单约束:NOT NULL,UNIQUE,CHECK,FOREIGN KEY(MATCH SIMPLE,RESTRICT/NO ACTION)
- [x] `tests/test_mvcc.cpp` — 两线程并发读写验证隔离级别

**验收**:`test_mvcc.cpp` 并发测试无脏读无不可重复读;下列 SQL 正确:
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

### M2.5 · 最小可用 MySQL 协议适配器(预计 0.5-1 周)
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

### M3 · MVCC 事务 + 子查询(预计 1.5 周)
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
- [x] SQLAlchemy 2.x 兼容:13 个硬编码系统变量(`@@tx_isolation`、`@@autocommit`、`@@character_set_*` 等)、`VERSION()` / `SCHEMA()` 函数、`COMMIT` / `ROLLBACK` / `BEGIN` 占位
- [x] MySQL wire protocol 字段类型动态推断(DECIMAL/VARCHAR/INT/DOUBLE 正确 field_type)
- [ ] `COM_STMT_PREPARE` / `COM_STMT_EXECUTE` — 二进制协议(→ M6 P3)
- [ ] `INFORMATION_SCHEMA` 视图(→ M6)
- [ ] `caching_sha2_password` 认证(→ M6)
- [ ] `CLIENT_SSL` 握手(→ M6)

**验收**:Navicat 打开连接不弹警告,能列出数据库/表/列;SQLAlchemy 8/10 核心路径 PASS。

---

### M5 · 持久化 + MVCC + SQLAlchemy 全兼容 ✅
**目标**:重启不丢数据,事务 READ COMMITTED,SQLAlchemy 可直接用 SQLTiny 作 MySQL server。

- [x] **WAL + Replay**:启动时先读 `sql.data` snapshot 再 replay 增量 `sql.wal`
- [x] **Checkpoint + Snapshot**:写 `sql.data`(metadata section + row records),truncate WAL;shutdown 自动 do_checkpoint
- [x] `INTEGER AUTO_INCREMENT`(MySQL 风格,对应 HSQLDB IDENTITY)
- [x] **MVCC SCN**:全局原子递增 SCN,Row 保存 `createSCN` / `deleteSCN`
- [x] `for_each_visible(querySCN)`:READ COMMITTED 可见性过滤
- [x] **并发安全**:Session::execute 入口加全局 `std::mutex`(当前是粗粒度串行化;行级锁 → M6 P0)
- [x] **SQLAlchemy 2.x**:Engine/Core/ORM/Inspector 核心路径 8/10 PASS,`DESCRIBE` / `SHOW FULL TABLES` 留待 M6
- [x] **4 个集成测试**:
  - `test_sqlalchemy.py` — SQLAlchemy 全场景(8/10 PASS,2 非核心 FAIL)
  - `test_vs_sqlite.py` — SQLTiny vs SQLite 全场景行为一致
  - `test_perf_vs_sqlite.py` — 大数据量 INSERT + JOIN 性能对比(无 crash)
  - `test_concurrency.py` — 5 线程 15s 并发读写(全局 mutex 生效无 crash)

**验收**:server 重启后 `SELECT COUNT(*)` 与 shutdown 前一致;并发测试 15s 无 crash;SQLAlchemy ORM `s.get(User, 1)` 可用。

---

### M6 · 行锁 + 事务 + JOIN 优化 + 存储过程(按优先级)

---

#### P0 · 行级锁 + 事务管理器(参考 HSQLDB TransactionManagerMVCC + Derby LockManager)
> 当前 `Session::execute` 用全局 `std::mutex` 串行化所有 SQL,语义正确但吞吐为 1。HSQLDB 同一架构(MVCC + AVL + WAL)用 `ReentrantReadWriteLock` 全局 writeLock 保护事务边界,但 **SELECT 不持锁**(快照隔离天然支持)。Derby 用传统 2PL,行锁 + 锁表 + 死锁检测。SQLTiny 走 HSQLDB 路线(已用 MVCC SCN),但需要补齐事务基础设施。

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

##### 0.5 死锁检测(参考 Derby)

SQLTiny 第一版可以简化:
- 行锁冲突时先 **abort 当前事务**(40001 Serialization Failure),让客户端重试
- 暂时不做真正的等待队列 + 循环等待检测(HSQLDB 的 `checkDeadlock` + latch wait 可以后续加)
- 好处:实现简单,和 HSQLDB MVCC 下 READ COMMITTED 遇到冲突的处理方式一致(HSQLDB 有 redo 等待但 SQLTiny 先用 abort)

---

#### P1 · SQL 标准事务完整化 + SQLAlchemy 最后 2 个 FAIL
> P0 做完后事务基础设施有了,P1 补 SQL 层的边界语义。

##### 1.1 需实现的事务功能

| 功能 | 说明 | HSQLDB 对应 |
|---|---|---|
| `BEGIN` / `START TRANSACTION` | 显式开始事务,关闭 autocommit | `Session.begin()` |
| `COMMIT` | 提交当前事务,释放所有行锁,更新可见性边界 | `TransactionManager.commitTransaction` |
| `ROLLBACK` [TO SAVEPOINT] | 回滚当前事务(或到 savepoint) | `TransactionManager.rollback` / `rollbackSavepoint` |
| `SAVEPOINT name` / `RELEASE SAVEPOINT` | 中间保存点 | `Session.savepoints` 队列 |
| `SET autocommit = ON/OFF` | autocommit 模式切换 | `Session.autocommit` 字段 |
| `FOR UPDATE` 子句 | SELECT 时对匹配行加 EXCLUSIVE 锁 | Derby 的 `FOR UPDATE` 拿行锁 |
| `SELECT ... LOCK IN SHARE MODE` | SELECT 时对匹配行加 SHARED 锁 | MySQL 兼容 |
| `SET TRANSACTION ISOLATION LEVEL ...` | 隔离级别设置 | HSQLDB `SET DATABASE TRANSACTION CONTROL` |
| 隔离级别:READ COMMITTED(默认) / REPEATABLE READ | 当前 SCN snapshot vs 事务开始 SCN snapshot | HSQLDB `Session.isolationLevel` |

##### 1.2 SQLAlchemy 最后 2 个 FAIL

| FAIL | 修复 |
|---|---|
| `DESCRIBE table` | parser 加 `DESCRIBE` 关键字解析 → 内部转调 `SHOW COLUMNS FROM table`(一行 map) |
| `SHOW FULL TABLES FROM db` | parser 加可选 `FULL` 关键字,executor `exec_show_tables` 返回额外一列 `table_type`(BASE TABLE / VIEW) |

##### 1.3 基于 HSQLDB TestSelfTransaction 的测试项

> HSQLDB 的测试框架 `.txt` 格式:`/*u1*/insert /*c2*/select ... /*r预期*/`,SQLTiny 用 smoke C++ 测试 + Python 并发测试组合覆盖。

| 测试 | 类型 | 说明 | HSQLDB 参考 |
|---|---|---|---|
| **P0-01**:行锁互斥(写-写冲突 abort) | C++ smoke | Session A INSERT 一行,Session B 同主键 INSERT → B 应该被 abort 或阻塞后重试(第一版直接 abort) | `TestSelfTransaction.txt` insert 冲突 |
| **P0-02**:写-读 快照隔离 | C++ smoke | Session A BEGIN 后 INSERT,Session B 同时 SELECT → B 用自己的 querySCN 看不到 A 未提交的行 | `TestSelfTransaction.txt` read committed |
| **P0-03**:读-读 不阻塞 | C++ smoke | 多 session 并发 SELECT 同一表(无锁!)— 这是 MVCC 的关键验证(对比之前全局 mutex 会串行化) | HSQLDB MVCC 多 session 并发读 |
| **P0-04**:写-写 同事务内重入 | C++ smoke | 一个事务内 UPDATE 同一行两次(应该 OK,因为 RowLock 重入) | — |
| **P1-01**:autocommit 模式 | Python | 默认 autocommit=true,每条 SQL 隐式 begin+commit;SET autocommit=OFF 后显式 BEGIN/COMMIT | `TestSelfTransaction.txt` autocommit on/off |
| **P1-02**:ROLLBACK 恢复 | Python | BEGIN → INSERT 3 行 → ROLLBACK → SELECT COUNT(*) = 0 | `TestSelfTransaction.txt` rollback 恢复 |
| **P1-03**:SAVEPOINT | Python | BEGIN → INSERT → SAVEPOINT s1 → INSERT → ROLLBACK TO s1 → SELECT 只看到第一行 | `TestSelfTransaction.txt` savepoint 回滚 |
| **P1-04**:SELECT FOR UPDATE 行锁 | Python | BEGIN → SELECT ... FOR UPDATE → 另一 session INSERT 同主键 → 应该冲突 | Derby 锁场景 |
| **P1-05**:隔离级别 READ COMMITTED vs REPEATABLE READ | C++ | RC:两次 SELECT 之间其他 session 提交了 INSERT,第二次能看到;RR:两次 SELECT 结果一致(同一 SCN 快照) | HSQLDB `Session.isolationLevel` |
| **P1-06**:死锁检测(两行循环锁) | C++ | Session A:lock row 1 → wait row 2;Session B:lock row 2 → wait row 1 → 其中一个 abort | Derby 死锁检测 |
| **P1-07**:并发 UPDATE 同一行(40001) | C++ smoke | 多线程并发 UPDATE 同一行(不同值)— 至少一个被 abort,其他重试成功 | HSQLDB row write conflict |
| **P1-08**:大表并发读写压测 | Python 多线程 | 10 threads × 1000 ops:SELECT / INSERT / UPDATE / DELETE 随机组合,最终 COUNT(*) 正确 | HSQLDB multithread tests |
| **P1-09**:DESCRIBE / SHOW FULL TABLES(SQLAlchemy 最后 2 FAIL) | Python | SQLAlchemy Engine `.connect()` → `meta.create_all()` → 不再 FAIL | 当前已知 FAIL |
| **P1-10**:WAL shutdown + restart 恢复事务状态 | C++ smoke | BEGIN → INSERT → shutdown(异常无 commit) → restart → SELECT COUNT(*) 不应该有那行(insert 没 commit,rollback 了) | WAL recovery + undo-log |

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
| 测试 | GoogleTest + GMock | 单测 + 集成测试 |
| 日志 | spdlog | 低开销 |
| 格式化 | fmtlib | 不依赖 iostream |
| JSON | nlohmann/json (可选) | SHOW CREATE TABLE 将来可能需要 |
| 网络 | `std::socket` (C++23) 或 Boost.Beast / asio | 里程碑 4 再决定;先用 Windows `WSA`/Linux `sys/socket.h` 直接写也可 |
| 并发 | `std::mutex`, `std::shared_mutex`, `std::jthread`, lock-free SCN 原子递增 | 避免重型依赖 |
| 编码 | UTF-8 贯穿;字符串内部 `std::string`(UTF-8),对外按 MySQL 协议转码 | MySQL 默认 utf8mb4 |

### 故意不引入的依赖
- 不使用 SQLite / RocksDB / LevelDB 作存储后端 —— 自己写,保持轻量
- 不使用 LLVM / bytecode 解释器 —— AST 直接解释执行足够快
- 不使用 Protobuf / FlatBuffers —— MySQL 协议自有编解码,手写即可

---

## 6. 风险与对策

| 风险 | 概率 | 对策 |
|---|---|---|
| HSQLDB 某些边界行为难以复刻 | 中 | 先对齐主流 SQL,保留 `HsqlException`→`SqlError` 的错误码映射;测试以 MySQL 兼容性为主,HSQLDB 行为作为回归基准 |
| MySQL 协议版本碎片化 | 低 | 仅支持 8.0 主流版(Client 41 + caching_sha2_password),老版按需追加 |
| 并发 MVCC 在 C++ 下死锁 | 中 | 先单线程正确,再 M3 引入多线程;`TransactionManager` 集中加锁点,避免分散 |
| AVL 大表性能不如 B+Tree | 低 | 里程碑 1-3 用 AVL 先跑通,里程碑 6 再替换索引结构,对外 `Index` 接口不变 |
| 磁盘持久化二进制兼容未来升级 | 中 | 文件头 magic + version 字段,启动时检查;不承诺跨大版本兼容 |

---

## 7. 里程碑交付节奏总览

```
        本 CodePlan 生成
             │
    ┌────────┼────────┬────────┬────────┬────────┬────────┐
    ▼        ▼        ▼        ▼        ▼        ▼        ▼
   M1       M2       M3       M4       M5       M6
  类型+     DDL/DML  MVCC/    MySQL   持久化   性能/
  AVL+      +Catalog 子查询   协议    WAL     边缘兼容
  Lexer
 1周        1.5周    1.5周    2周      2周      持续
 ──────────────────────────────────────────────────────────────▶
```

**立即可启动:** M1。从 `src/types/value.h` 和 `src/storage/index_avl.h` 两个模块并行起步,因为它们没有外部依赖,单元测试先行。
