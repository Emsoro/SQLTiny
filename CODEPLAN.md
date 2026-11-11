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

### M4 · 协议层补全(预计 1 周)
**目标**:Prepared Statement 二进制协议 + SHOW 系列 + INFORMATION_SCHEMA 最小集,让图形客户端更顺滑。

- [ ] `COM_STMT_PREPARE` / `COM_STMT_EXECUTE` / `COM_STMT_CLOSE` / `COM_STMT_RESET` — 二进制协议
- [ ] `SHOW TABLES` / `SHOW CREATE TABLE` / `SHOW DATABASES` / `SHOW STATUS` / `SHOW VARIABLES`
- [ ] `USE db`, `CREATE DATABASE`, `DROP DATABASE`
- [ ] `INFORMATION_SCHEMA.TABLES` / `INFORMATION_SCHEMA.COLUMNS` 视图(最小列集)
- [ ] `caching_sha2_password` 认证(支持 MySQL 8.0 默认插件)
- [ ] `CLIENT_SSL` 握手阶段(里程碑 6 可延后)
- [ ] 更完整的错误码与 SQL State 映射

**验收**:Navicat 打开连接不弹警告,能列出数据库/表/列;Prepared Statement 预编译执行正确。

---

### M5 · 持久化 + 高级特性(预计 2 周)
**目标**:重启不丢数据,覆盖 HSQLDB TestSelf* 主流 case。

- [x] **WAL + Checkpoint**:写前日志;启动 replay;定期 checkpoint 生成 `.data` 文件
- [x] `storage/persist.{h,cpp}` — 文件格式自定义(table metadata section + row records),第一版单文件
- [x] `INTEGER AUTO_INCREMENT`(作为 MySQL 兼容特性,对应 HSQLDB 的 IDENTITY)
- [x] `SEQUENCE`, `DEFAULT nextval(...)`
- [x] 视图(`CREATE VIEW`),触发器(`CREATE TRIGGER` 第一版 AFTER INSERT/UPDATE/DELETE ROW 级)
- [x] `SHOW TABLES`, `SHOW COLUMNS FROM`, `DESCRIBE`, `SHOW CREATE TABLE`(MySQL 风格)
- [x] 用户权限:GRANT/REVOKE 基础(库/表级 SELECT/INSERT/UPDATE/DELETE)
- [x] 跑 HSQLDB `testrun/hsqldb/TestSelf*.txt` 子集(Create、Constraints、Queries、InsertDeleteQueries、Joins、GroupBy、PrimaryKeys、FKModes、StoredProcedure 部分)

**验收**:重启后数据仍然存在;跑 `TestSelfCreate.txt` / `TestSelfQueries.txt` 通过。

---

### M6 · 性能打磨 + 边缘兼容(持续优化)
- [ ] Prepared Statement 二进制协议(`COM_STMT_PREPARE` / `COM_STMT_EXECUTE`)
- [ ] B+ Tree 替代 AVL(大表范围扫描更快)
- [ ] 查询计划:多表 JOIN 顺序选择、索引选择代价估算
- [ ] `EXPLAIN` / `EXPLAIN ANALYZE`
- [ ] 存储过程(PSM 子集)
- [ ] 复制/主从(MySQL 二进制日志协议兼容)
- [ ] SSL/TLS 握手阶段 `CLIENT_SSL`
- [ ] 更大的 HSQLDB TestSelf* 覆盖

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
