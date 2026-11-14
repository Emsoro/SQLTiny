# SQLTiny — 轻量级嵌入式数据库 · MySQL 协议兼容

> **当前版本 `v1.2.5`(2026-11-14)** · 版本号统一维护在 `CMakeLists.txt`(`project(SQLTiny VERSION 1.2.5)`)、`vcpkg.json`(`version-string`)与 `src/main.cpp` 启动横幅。协议层对外仍伪装为 `5.7.0-SQLTiny` 以兼容 MySQL 客户端,与产品版本无关。

**SQLTiny** 是用现代 C++20 从零重写的轻量级关系型数据库,核心设计灵感来自 [HSQLDB](http://hsqldb.org/),额外暴露 **MySQL Wire Protocol**——标准 MySQL 客户端(`mysql` CLI、Navicat、DBeaver、Connector/J、PyMySQL、DBX/dbExpress)直接连接。

> 完整项目规划见 [`CODEPLAN.md`](./CODEPLAN.md),架构映射、技术选型、里程碑验收都在里面。

---

## ✨ 特点

| 特性 | 说明 |
|---|---|
| 🧠 **纯 C++20** | 无 Java / SQLite / RocksDB 依赖 |
| 🗄️ **嵌入式优先** | 进程内 C++ 调用 或 TCP 客户端连接,两种模式 |
| 🎯 **MySQL 协议兼容** | PyMySQL / mysql CLI / Navicat / SQLAlchemy / **dbExpress(DBX) 驱动** 直连,握手 + 类型推断 + NULL 编解码完整 |
| 🔧 **服务端预编译语句** | `COM_STMT_PREPARE / EXECUTE / CLOSE / RESET / SEND_LONG_DATA` 完整 binary protocol |
| 🗂️ **information_schema** | SCHEMATA / TABLES / COLUMNS 等虚拟表,第三方工具元数据浏览直接可用 |
| 🗃️ **多库 Catalog** | CREATE/DROP/USE DATABASE,库级独立表注册 |
| ⚡ **AVL 内存索引** | 自带平衡 BST,主键查询 O(log n) |
| 🪶 **轻量** | 单库文件,启动毫秒级,依赖仅 fmtlib;**静态编译**(`x64-windows-static`),产物不依赖 `fmt.dll` / VC 运行时 DLL |

---

## 🏛️ 系统架构

![SQLTiny Architecture](./architecture.svg)

> 🎨 **配色语义**: 靛蓝 = 执行/事务写路径 · 青绿 = Session 与隔离级别(读快照策略) · 琥珀 = WAL 与持久化(提交边界) · 灰蓝 = Catalog/Table 存储与 MVCC 版本
>
> 🔒 **锁分层**: Catalog.mu_(DDL) → Table.mu_ shared_lock(SELECT 多读者并行) / unique_lock(DML 独占) → RowLockMap(行级冲突检测) → TransactionManager.writeLock_(提交边界) → WalManager.mu_(WAL 文件读写)
>
> 🧊 **MVCC**: SELECT 完全不加锁,`for_each_visible(querySCN, readerTxn)` 按 `createSCN/deleteSCN` 与 `pendingTxn` 过滤;未提交行只对所属事务可见,commit 才 `finalize_commit` 对外发布

---

## 🆕 v1.2.5 更新内容

本版本聚焦 **大表 JOIN 主键索引利用**(CODEPLAN §M6 P2 续):employees 标准数据集(392 万行)实测发现 3 表 JOIN 77s(MySQL 6.5s),右表 join 列为复合主键首列时仍走 Hash Join 全量物化。本版本改为 **PK 前缀索引探测**,右表零物化。

### 性能报告(2026-11-14,employees 数据集 300024×331603×2844047 行)

| 查询 | 优化前 | 优化后 | 提升 |
|---|---|---|---|
| 3 表 JOIN 求各部门均薪 | 77 s | **23.8 s** | 约 **3.2x** |
| `employees⋈salaries WHERE emp_no=10001` | 28 s | **2.3 s** | 约 **12x** |
| 混合谓词(下推+索引探测) | — | 9.8 s | 新路径 |
| 2 表 JOIN 计行(哈希路径回归) | — | 1.15 s | 无退化 |

`EXPLAIN` 正确显示 `salaries | ref | emp_no | Using index probe`(Extra 兼容保留 "hash join" 字样供既有断言)。

### 做了什么

- **AVL PK 前缀扫描**:`index_avl` 新增 `prefix_scan(key, cb)` —— 节点 key 截断到前缀长度逐列比较决定剪枝方向,命中键长 ≥ 前缀长即回调,复杂度 **O(log n + 命中数)**
- **表层前缀遍历**:`Table::for_each_by_pk_prefix(first_pk_value, querySCN, cb, readerTxn)` —— 前缀扫描 + MVCC `row_visible` 过滤(NULL 首列不命中)
- **执行器索引探测路径**:`extract_equi` 识别「join 列 = 右表复合 PK 首列」且左行数 ≤ 右行数时,逐左行构造 PK 首列 key 前缀探测右表,纯等值直接拼 flat row、复合 ON 再 `eval` 兜底,语义与 Hash Join 完全一致
- **WHERE 谓词下推**:`AND` 连接词拆分,只引用主表列且无歧义的 conjunct 在主表扫描阶段预过滤(限定名精确命中 / 裸名查 `ambiguous_cols`,Subquery/Case/Star 保守不下推)
- **SUM(DECIMAL) 列类型一致性修复**:`infer_type` 此前对 `SUM(DECIMAL)` 声明 LONGLONG,但聚合值经 double 路径返回 `'300.5'`,PyMySQL 按 LONGLONG 调 `int()` 崩溃并中断行读取,造成连接包序错位(`Packet sequence number wrong`)。现声明 DOUBLE 与实际返回值一致
- **协议排查工具**:`tests/_repro_pkt.py`(裸 socket 逐包 dump,验证 server 帧连续性)、`tests/_trace_seq.py`(PyMySQL 包序插桩)、`tests/_bench_join.py`(employees JOIN 基准)

### 回归

`ctest` **10/10**(smoke_m25_e2e 连 server 全过);`test_full_flow.py` **48/48**;`test_vs_sqlite.py` 全场景一致;`test_stmt_prepare.py` 7/7;`compare_employees.py` **14/14 MATCH**;smoke_m3 聚合断言改为类型无关 `num_eq`。

---

## 🆕 v1.2.2 更新内容

本版本聚焦 **Navicat 实测兼容修复** —— Navicat 连接时报 `2013 - Lost connection to server during query`,根因是握手包错位解析。

### 修了什么

- **握手包 reserved 字段少写 1 字节**(致命修复):MySQL 协议 Handshake v10 在 `auth-plugin-data-length` 之后要求**恰好 10 字节**保留区,旧实现只写了 9 字节,导致后面所有字段整体前移 1 字节 —— Navicat 按规范解析时 auth-plugin 名变成错乱字节,识别不出插件后回退发送 `dummy_fallback_auth`,并在认证 OK 后直接断开(表现即 2013)。现改为循环写满 10 字节
- **`auth-plugin-data-length` 按规范修正为 21**(part1 8 字节 + part2 12 字节 salt + 1 字节 NUL),此前写的是 `min(21, len-8)=12`,与 MySQL 官方服务器广播值不一致
- **新增 `information_schema.ENGINES` 虚拟表**:Navicat 建连后固定探测 `SELECT COUNT(*) FROM information_schema.ENGINES WHERE Engine='ndbcluster'`,旧版回 ERR 1146 导致连接打开失败。现以 InnoDB 兼容身份上报单行(事务/XA/保存点 = YES)
- **`--query-log` 调试增强**:每次连接记录握手 + 认证 + 每条命令的**原始字节 hex dump**(含 seq),收发包失败点(EOF/reset)也会标记,客户端兼容性问题可一跑定位;`stdout/stderr` 改为无缓冲,重定向到文件时不再丢启动日志

### 回归

Navicat 建连实测通过(连接测试 + 打开连接 + 元数据加载);PyMySQL / SQLAlchemy 端到端全绿;握手包经脚本按协议逐字段解析校验(version / caps / charset / status / auth-data / plugin 全部对齐)。

---

## 🆕 v1.2.0 更新内容

本版本聚焦 **JOIN 性能优化**(CODEPLAN §M6 P2):从纯嵌套循环 O(N×M) 升级为 Hash Join,并补齐 `EXPLAIN` 与查询成本估算。

### 性能报告(2026-11-12,Release 版,PyMySQL 单进程,200 users × 800 orders)

| 查询 | 优化前 | 优化后 | 提升 |
|---|---|---|---|
| `INNER JOIN` | 298.40 ms | **3.91 ms** | 约 **77x** |
| `LEFT JOIN` | 297.20 ms | **4.77 ms** | 约 **62x** |
| 单进程整体 wall | 1822 ms | **73 ms** | 约 **25x** |

**线性扩展**:500 users × 2000 orders 大表 `INNER JOIN` 仅 **9.05 ms**(不再 100 万行 explode)。

### 做了什么

- **Hash Join**:从 `ON` 表达式递归提取「左表列 = 右表列」等值项(支持 `AND` 复合条件),右表按 key 分桶、左表 O(1) probe,复杂度 **O(N×M) → O(N+M)**;数值 key 统一提升 double、NULL 不参与等值;纯等值只做 `sql_equals` 复核,复合 `ON` 仍完整 `eval` 兜底保证语义完全一致
- **预建列索引**:列名→偏移映射只构建一次,消除旧实现里每个行组合都重建 map 的高额常数开销
- **`EXPLAIN` 最小版**:`EXPLAIN SELECT …` 只生成执行计划、不扫描数据,输出 MySQL 风格 8 列(`id / select_type / table / type / possible_keys / key / rows / Extra`);右表 join 列是单列主键时 `type=eq_ref`、`key=PRIMARY`,非等值 `ON` 显示 `Using nested loop (cost …)`
- **成本估算**:按表 `row_count()` 估算 `nested loop = left×right` / `hash = left+right`,多表 JOIN 逐轮传递基数,写入 `EXPLAIN` 的 `rows` 与 `Extra`

完整实现与回归说明见 `CODEPLAIN.md` §M6 P2。基准数据由 `tests/test_perf_vs_sqlite.py` 生成并保存在 `benchmark/`(`latest.json` 含各查询平均耗时与提速倍数)。

### 回归

`smoke_m6_hsqldb_matrix` C 组新增 11 项断言(等值 / 反向等值 / 复合 ON / 非等值退化 / CROSS / LEFT 补 NULL / NULL key / 多表连续 Hash Join / EXPLAIN type 与 cost),现为 **73 PASS / 0 GAP**;`ctest` 10/10,Python 端到端全绿。

---

## 🆕 v1.1.2 更新内容

本版本聚焦 **GUI 客户端实测兼容修复**(以 DBeaver 26.2.4 / Connector/J 8.2.0 与 VS Code Database Client 实测驱动):

| 类别 | 内容 |
|---|---|
| **DDL 选项** | `CREATE DATABASE/SCHEMA/TABLE` 尾部的 `DEFAULT CHARACTER SET / COLLATE / ENGINE / AUTO_INCREMENT / COMMENT` 等选项被正确跳过,不再被当成下一条语句(修复 DBeaver 建库建表 1064 `unexpected start of statement: 'DEFAULT'`) |
| **SHOW TABLES 过滤** | `SHOW [FULL] TABLES [FROM db] [LIKE 'pat' \| WHERE col='pat']` 完整解析,表名过滤大小写不敏感(修复 DBeaver 建表校验 1064 `'WHERE'`) |
| **information_schema** | 新增 `REFERENTIAL_CONSTRAINTS` 虚拟表(空结果集),DBeaver 打开表时的外键查询不再报 1146 |
| **系统变量** | `SHOW VARIABLES` 补齐 `lower_case_table_names` / `max_allowed_packet` / `net_read_timeout` / `net_write_timeout` |
| **别名语法** | 双引号串(`AS "alias"` / 无 AS 裸 `"alias"`)与裸非保留关键字(`COLUMN_COMMENT comment`)可作列/表别名——Database Client 元数据查询的 1064 |
| **新函数** | `INSTR(str,sub)` / `LOCATE(sub,str)` / `POSITION`(大小写不敏感,1 起,未找到 0) |
| **回归** | `tests/smoke_m4.cpp` 扩展上述场景断言;`ctest` 10/10 |

---

## 🆕 v1.1.0 更新内容

本版本聚焦**第三方客户端兼容性**(以 Delphi dbExpress/DBX 驱动和类 Navicat 工具实测驱动):

| 类别 | 内容 |
|---|---|
| **服务端预编译语句** | 完整实现 `COM_STMT_PREPARE`(0x16) / `COM_STMT_EXECUTE`(0x17) / `COM_STMT_CLOSE` / `COM_STMT_RESET` / `COM_STMT_SEND_LONG_DATA`。参数按 binary protocol 解码(全类型 + NULL 位图)拼回 SQL 重放执行;SELECT/SHOW 在 prepare 阶段用 NULL 替换占位符探测列元数据(只读、安全);SELECT 结果以 binary resultset 返回 |
| **EOF 帧兼容** | 服务端不再广播 `CLIENT_DEPRECATE_EOF`,永远使用经典 EOF 帧帧定结果集——修复 DBX 等老客户端回显能力位后挂起 5 秒的问题 |
| **information_schema 虚拟表** | `SCHEMATA` / `TABLES` / `COLUMNS`(含 LEFT JOIN KEY_COLUMN_USAGE 这类工具元数据查询) / `TABLE_CONSTRAINTS` / `STATISTICS` / `TRIGGERS` / `ROUTINES` / `VIEWS` / `EVENTS`——每次查询物化,工具浏览元数据直接可用 |
| **SHOW 扩展** | `SHOW ENGINES` / `PLUGINS` / `CHARSET` / `CHARACTER SET` / `COLLATION` / `TABLE STATUS [LIKE]` / `INDEX|KEYS FROM db.tbl` / `FULL COLUMNS FROM db.tbl` / `PROCESSLIST` / `WARNINGS` / `TRIGGERS [WHERE]`;反引号 `db`.`tbl` 限定名解析 |
| **表达式** | `CASE [base] WHEN .. THEN .. [ELSE ..] END`(简单/搜索两种形式);`AUTO_INCREMENT` 等关键字可用作列名 |
| **协议命令** | `COM_FIELD_LIST`(db 限定 + `\0` 截断) / `COM_STATISTICS` / `COM_PROCESS_INFO` / `COM_CHANGE_USER`;SSL 请求返回明确 ERR |
| **系统变量** | `@@max_allowed_packet` / `@@net_write_timeout` / `@@net_read_timeout` / `@@sql_mode` / `@@lower_case_table_names` / `@@sql_auto_is_null` |
| **调试工具** | 新增 `--query-log FILE` 参数(逐条记录命令 + 握手 + 执行结果);`tests/dbx_probe.cpp`(58 断言,进程内逐条跑 DBX 典型 SQL) 与 `tests/test_stmt_prepare.py`(7 断言,裸 socket 预编译协议) 回归 |
| **回归适配** | `tests/smoke_m25_e2e.cpp` 改为兼容经典 EOF 帧(列定义后的 `0xfe` 包),CTest 恢复 10/10 |

---

## 🔒 四个隔离级别(参考 HSQLDB 2.7.4)

SQL 语法:`SET SESSION TRANSACTION ISOLATION LEVEL <级别>`(会话默认)、`SET TRANSACTION ISOLATION LEVEL <级别>`(仅下一事务)、`START TRANSACTION ISOLATION LEVEL <级别>`、`SET SESSION tx_isolation='...'`。`SELECT @@tx_isolation` / `SHOW VARIABLES LIKE 'tx_isolation'` 返回真实值。

| 请求级别 | 实际生效 | 读快照 | 说明 |
|---|---|---|---|
| READ UNCOMMITTED | **READ COMMITTED** | 语句级 | HSQLDB 提升;下一事务级附加只读(写返回 25006) |
| READ COMMITTED(默认) | READ COMMITTED | 语句级 | 允许不可重复读/幻读 |
| REPEATABLE READ | **SERIALIZABLE** | 事务级 | HSQLDB 提升,MVCC 下即 SNAPSHOT ISOLATION |
| SERIALIZABLE | SERIALIZABLE | 事务级 | 陈旧写/写冲突返回 40001(1213) |

四个级别**都不会读到未提交数据**;自己的未提交写只对自己可见。行版本在 commit 时才 `finalize`,未提交的行 DML 缓存在事务里,commit 整体落 WAL、rollback 整体丢弃,因此重启不会复活回滚掉的数据。事务还支持 `SET AUTOCOMMIT=0/1`、`BEGIN/START TRANSACTION`、`COMMIT`、`ROLLBACK`、`SAVEPOINT` / `ROLLBACK TO SAVEPOINT` / `RELEASE SAVEPOINT`。

---

## 🏗️ 当前进度

| 里程碑 | 状态 | 说明 |
|---|---|---|
| M1 基础骨架 | ✅ | 类型系统 + AVL + Lexer |
| M2 Catalog + DDL/DML | ✅ | Parser + AST + 执行器 |
| M2.5 MySQL 协议 | ✅ | Wire Protocol 握手 + 编解码 + TCP server |
| M3 子查询 + JOIN 补全 | ✅ | GROUP BY · HAVING · 子查询 IN · INSERT SELECT · INNER/LEFT JOIN |
| M4 协议层补全 | ✅ | Catalog 多库化 · SHOW 系列(含 LIKE) · SQLAlchemy 系统变量兼容 |
| **M5 持久化 + MVCC + SQLAlchemy** | ✅ | WAL+Replay+Checkpoint · MVCC SCN 可见性 · AUTO_INCREMENT · 并发安全(M5 为全局 mutex,M6 起改为行锁 + MVCC 快照) · **SQLAlchemy 全场景 33/0/0 PASS · 4+ 集成测试** |
| **M6 事务 + 四隔离级别 (HSQLDB 2.7.4)** | ✅ | 行级锁 · 四隔离级别与提升规则 · SAVEPOINT · 事务级 WAL 缓冲 · 提交期冲突检测 40001 |
| **M6 客户端兼容性 (v1.1.0)** | ✅ | 服务端预编译语句 · information_schema 虚拟表 · SHOW 扩展 · CASE 表达式 · EOF 帧兼容 · **DBX/dbExpress + 类 Navicat 工具实测通过 · dbx_probe 58/58 · test_stmt_prepare 7/7** |
| **M6 客户端兼容性 (v1.1.2)** | ✅ | DBeaver / Database Client 实测修复:DDL 选项跳过 · SHOW TABLES WHERE/LIKE · REFERENTIAL_CONSTRAINTS · 双引号与裸关键字别名 · INSTR 系列函数 |
| **M6 P2 JOIN 优化 (v1.2.0)** | ✅ | Hash Join + 预建列索引 + 成本估算 + `EXPLAIN`;INNER/LEFT JOIN 约 77x / 62x 提速 · 500×2000 大表 INNER JOIN 仅 9ms |
| **M6 P2 JOIN 优化·续 (v1.2.5)** | ✅ | PK 前缀索引探测(右表零物化)+ WHERE 谓词下推;employees 3 表 JOIN 77s→23.8s · 单员工⋈薪酬 28s→2.3s · SUM(DECIMAL) 列类型一致性修复 |
| **后续** | 📋 | 等待队列/死锁检测 → 存储过程 · caching_sha2_password / SSL · 复制 |

---

## 🛠️ 构建

### 环境要求
- **Windows**: Visual Studio 2022 MSVC 14.40+ (MSBuild)
- **CMake**: 3.20+
- **vcpkg**: 设 `VCPKG_ROOT` 环境变量;依赖仅 `fmt` (vcpkg manifest 自动拉取)。默认 triplet `x64-windows-static`,fmt 与 MSVC CRT 都静态链接,产物自包含、无需随附 `fmt.dll`;需要动态链接时必须**成对**覆盖 `-DVCPKG_TARGET_TRIPLET=x64-windows` 与 `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL...`(`/MD`)并重新配置,只改其中一个会造成 CRT 混用
- **Python 3.10+** (跑 PyMySQL 全场景测试可选)

```powershell
cmake --preset windows-vcpkg
cmake --build build --config Debug
cmake --build build --config Release
```

> **排障**:若 Debug 版运行时报 `Debug Assertion Failed: __acrt_first_block == header`(`debug_heap.cpp:996`),说明 `build/` 缓存的 triplet/CRT 与配置不一致(两套 CRT 堆并存,跨堆 `free`)。删除 `build/` 后用上面的 preset 重新配置并全量重建即可,不要直接 `cmake -S . -B build`。

```powershell
python tests\test_perf_vs_sqlite.py --mode both   # Release server 单/多进程对比,结果存 benchmark/
```

### 稳定性压测

专为「本地数据库服务本地程序」场景设计的压力与稳定性套件(密码不测,专攻稳定性):

```powershell
python tests\stress_stability.py --self-server --phase all --duration 30 --workers 16
```

`--self-server` 自动拉起/管理 server(内存或临时 `--data-dir`);`--phase` 可选 `complex`(复杂查询计时)/ `soak`(并发长时)/ `crash`(崩溃恢复)/ `all`。覆盖:10 条复杂查询(多表 JOIN+聚合 / 子查询 / GROUP BY+HAVING / CASE / 自连接 / NULL 聚合)、并发读写与事务冲突(40001 后无丢写/幻行不变量)、`kill -9` 后 WAL replay 原子性、soak 期间 RSS 内存稳定。详见脚本头部与 `CODEPLAN.md` §8.2 C09。

### JOIN 优化与 EXPLAIN

等值 JOIN 走 **Hash Join**(右表按 key 分桶、左表 O(1) probe),复杂度 O(N×M) → O(N+M);
非等值 / CROSS / RIGHT JOIN 仍走嵌套循环,但列索引只构建一次。实测 200 users × 800 orders:
`INNER JOIN` 298ms → **3.9ms**(约 77x),`LEFT JOIN` 297ms → **4.8ms**(约 62x)。

```sql
EXPLAIN SELECT u.name, o.total
FROM users u INNER JOIN orders o ON o.user_id = u.id;
-- id | select_type | table | type | possible_keys | key | rows | Extra
-- 1  | SIMPLE      | users | ALL  |               |     | 200  |
-- 2  | SIMPLE      | orders| ref  | user_id       | user_id | 800 | Using hash join (build: orders, cost 1000 vs nested 160000)
```

`EXPLAIN` 只生成计划、不扫描数据;右表 join 列是单列主键时 `type=eq_ref`、`key=PRIMARY`。
非等值 ON 显示 `Using nested loop (cost ...)`。完整实现说明见 `CODEPLAN.md` §M6 P2。

### 构建产物

| 产物 | 类型 | 说明 |
|---|---|---|
| `build/Debug/sqltiny.lib` | 静态库 | 引擎核心,嵌入式进程内调用 |
| `build/Debug/sqltiny_server.exe` | 可执行 | MySQL 协议 TCP 服务器(v1.2.5) |
| `build/Release/sqltiny_server.exe` | 可执行(发布版) | 同上,Release 版约 **0.9 MB** 单文件(静态 CRT + fmt 静态链接,无 dll 依赖,可直接拷贝部署);Debug 版约 4.8 MB |

**`sqltiny_server.exe` 文件信息(v1.2.5)**:

| 属性 | 值 |
|---|---|
| 用途 | MySQL Wire Protocol TCP 服务器,Navicat / DBeaver / mysql CLI / PyMySQL / SQLAlchemy 直接连接 |
| 默认监听 | `0.0.0.0:3306`(可用 `--port` 改) |
| 认证 | `mysql_native_password`,开发期 user=root、密码任意 |
| 持久化 | `--data-dir` 指定目录后启用 WAL(`sql.wal`)+ Snapshot(`sql.data`) |
| 调试 | `--query-log FILE` 记录握手/每条命令原始报文 hex(客户端兼容性定位用) |
| 协议伪装 | 服务器版本字符串 `5.7.0-SQLTiny`(与产品版本号无关) |
| `build/Debug/smoke_m1.exe` | 冒烟测试 | Value / AVL / Lexer — **3/3 PASS** |
| `build/Debug/smoke_m2.exe` | 冒烟测试 | CRUD / 聚合 / 表达式 / DDL / NULL — **5/5 PASS** |
| `build/Debug/smoke_m3.exe` | 冒烟测试 | GROUP BY / HAVING / 子查询 IN / INSERT SELECT — **4/4 PASS** |
| `build/Debug/smoke_m4.exe` | 冒烟测试 | CREATE/DROP DB / USE / SHOW / SET 占位 — **6/6 PASS** |
| `build/Debug/smoke_m5_wal.exe` | 冒烟测试 | WAL Replay + CRC 校验 + Checkpoint 恢复 — **28/28 PASS** |
| `build/Debug/smoke_m25_e2e.exe` | E2E 测试 | Winsock 直连 server — **13/13 PASS** |
| `build/Debug/smoke_m6_isolation.exe` | 隔离级别测试 | 四级别 + 事务/保存点/冲突 — **451/451 PASS** |
| `build/Debug/smoke_m6_hsqldb_matrix.exe` | HSQLDB 语义矩阵 + JOIN 优化回归 | NULL 三值 / 空集聚合 / JOIN 基数(Hash Join、EXPLAIN) / 语句原子性 / 多库隔离 — **73 PASS / 0 GAP** |
| `build/Debug/smoke_m6_session_db.exe` | 多库会话隔离 | 每连接 `USE` / 握手 database 互不干扰 — **PASS** |
| `build/Debug/dbx_probe.exe` | 客户端兼容性探针 | 进程内逐条跑 DBX/dbExpress 连接/元数据/事务阶段的典型 SQL — **58/58 PASS** |
| `tests/test_txn_vs_sqlite.py` | 事务行为对比 | SQLTiny vs SQLite 四级别 + WAL 重启 — **70/70 一致** |
| `tests/test_full_flow.py` | PyMySQL 全链路(双连接) | DDL/INSERT→SELECT*含NULL→WHERE/ORDER/聚合/HAVING→JOIN→UPDATE/DELETE→约束与错误码→事务/SAVEPOINT/autocommit→四隔离级别/40001→多库→清理 — **48 PASS / 0 GAP** |
| `tests/test_sqlalchemy.py` | SQLAlchemy 2.x | Engine/Core/ORM/2.0 风格表达式/`text():param`/rollback/relationship/`engine.begin()`/类型映射/Inspector — **33 PASS / 0 FAIL / 0 GAP**(`DESCRIBE`/`SHOW FULL TABLES` 已支持) |
| `tests/test_vs_sqlite.py` | 行为对比 | 同一逻辑 SQLTiny vs SQLite 全场景 assert — **全绿** |
| `tests/test_perf_vs_sqlite.py` | 性能对比 | Release 版 server,单进程 / 多进程两种模式对比 SQLite,结果保存 `benchmark/`(JSON + CSV + `latest.json`) — **跑通无 crash** |
| `tests/test_concurrency.py` | 并发安全 | 5 线程(2W+2R+1D)15s 并发读写 — **通过** |
| `tests/stress_stability.py` | 稳定性压测 | 自带 server 的并发/soak/崩溃恢复/内存稳定套件(`--self-server --phase all`) — **全 PASS** |
| `tests/test_stmt_prepare.py` | 预编译语句协议 | 裸 socket 走 COM_STMT_PREPARE/EXECUTE/CLOSE 全流程(含 NULL/转义/语法错误) — **7/7 PASS** |

### 一键跑全量测试
```powershell
cmake --build build --config Debug

# 另开一个终端启动 server(smoke_m25_e2e 与部分 Python 脚本需要)
.\build\Debug\sqltiny_server.exe --port 3306 --user root
# 然后一键跑完 10 个 CTest 目标(未启动 server 时为 9/10)
ctest --test-dir build -C Debug --output-on-failure

# 也可逐个运行(以下无需 server,纯进程内)
.\build\Debug\smoke_m1.exe
.\build\Debug\smoke_m2.exe
.\build\Debug\smoke_m3.exe
.\build\Debug\smoke_m4.exe
.\build\Debug\smoke_m5_wal.exe
.\build\Debug\smoke_m6_isolation.exe
.\build\Debug\smoke_m6_hsqldb_matrix.exe
.\build\Debug\smoke_m6_session_db.exe
.\build\Debug\dbx_probe.exe

# MySQL 协议端到端(复用上面已启动的 server)
.\build\Debug\smoke_m25_e2e.exe

# Python 全场景(PyMySQL client,单一 connection)
pip install pymysql sqlalchemy
python tests\test_full_flow.py
python tests\test_sqlalchemy.py
python tests\test_vs_sqlite.py
python tests\test_perf_vs_sqlite.py
python tests\test_concurrency.py
python tests\test_wal_recovery.py    # server 重启恢复(需 server)
python tests\test_txn_vs_sqlite.py   # 自动起停 server,不占用 3306
python tests\test_stmt_prepare.py    # COM_STMT_* 预编译协议(需 server)
python tests\stress_stability.py --self-server --phase all   # 稳定性压测(自带 server)
```

### 测试总览 (CTest 10/10 · Python 8 scripts)

> `smoke_m25_e2e` 通过 TCP 连接 3306,需先启动 server;未启动 server 时 CTest 为 9/10。
```
smoke_m1              3/3   PASS  Value / AVL / Lexer
smoke_m2              5/5   PASS  CRUD / 聚合 / 表达式 / DDL edge / IS NULL
smoke_m3              4/4   PASS  GROUP BY / HAVING / 子查询 IN / INSERT SELECT
smoke_m4              6/6   PASS  CREATE/DROP DB / USE / SHOW
smoke_m5_wal         28/28  PASS  WAL Replay + CRC + Checkpoint + 启动恢复
smoke_m25_e2e        13/13  PASS  MySQL wire C++ client E2E
smoke_m6_isolation  451/451 PASS  四隔离级别 + 事务 / 保存点 / 冲突
smoke_m6_hsqldb_matrix 73 PASS / 0 GAP   HSQLDB 语义矩阵 + JOIN 优化/EXPLAIN 回归
smoke_m6_session_db  PASS        多库会话级隔离(每连接 USE/握手 db 互不干扰)
test_full_flow       48 PASS / 0 GAP    PyMySQL 全链路(双连接):DDL→DML→聚合→JOIN→事务→四隔离级别→多库→清理
test_txn_vs_sqlite   70/70 对比        四隔离级别 + WAL 重启与 SQLite 逐项比对
test_sqlalchemy      33 PASS / 0 FAIL / 0 GAP  15 组:Engine/Core/ORM/2.0 表达式/绑定/事务/relationship/类型映射/Inspector
test_vs_sqlite       全绿             同逻辑 SQLTiny vs SQLite 行为一致
test_wal_recovery    全通过            server 重启 / UPDATE / DELETE 状态恢复
test_perf_vs_sqlite  跑通              Release server 单/多进程 vs SQLite,结果存 benchmark/
test_concurrency     通过              5 线程 15s 并发读写
stress_stability    全 PASS           自带 server:复杂查询计时/并发soak不变量/kill-9崩溃恢复/RSS稳定
dbx_probe           58/58 PASS         DBX/dbExpress SQL 探针(进程内:连接/元数据/事务)
test_stmt_prepare    7/7   PASS        预编译语句 binary protocol 全流程
```

---

## 🚀 使用方式

### 方式 A:C++ 嵌入式调用(进程内,无协议开销)

```cpp
#include "core/session.h"

sqltiny::core::Session s;
auto r = s.execute("CREATE DATABASE shop");
r = s.execute("USE shop");
r = s.execute("CREATE TABLE users (id BIGINT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(64) NOT NULL, age INT DEFAULT 18)");
r = s.execute("INSERT INTO users (name, age) VALUES ('Alice', 28), ('Bob', 32), ('Eve', NULL)");
r = s.execute("SELECT name, age FROM users WHERE age > 28 ORDER BY age");
// r.column_names = {"name", "age"}
// r.rows[0] = {Value("Bob"), Value(32)}
```

### 方式 B:TCP server + mysql / PyMySQL 客户端

```powershell
# 启动 server (开发期不校验密码,user=root 即可登录)
.\build\Debug\sqltiny_server.exe --port 3306 --user root

# mysql CLI 连接
mysql -h 127.0.0.1 -P 3306 -u root

# 或 Python PyMySQL
python -c "import pymysql; c=pymysql.connect(host='127.0.0.1',port=3306,user='root'); cur=c.cursor(); cur.execute('SELECT 1'); print(cur.fetchone())"
```

### CLI 参数

```
sqltiny_server [--port <n>] [--user <name>] [--password <pwd>] [--data-dir <DIR>] [--query-log <FILE>]
               默认 --port 3306 --user root
               --data-dir   缺省 = 内存模式(无持久化,重启丢数据)
               --query-log  记录握手/认证/每条命令的原始报文 hex 及执行结果(调试客户端兼容性用)
```

> **开发期简化**: `mysql_native_password` 不校验密码,只要 user=root 就放行。

### 数据存储位置

SQLTiny 通过 `--data-dir` 指定持久化目录,目录下生成两个文件:

| 文件 | 说明 |
|---|---|
| `sql.wal` | 写前日志(WAL),所有 INSERT / UPDATE / DELETE / DDL 实时追加。Server 启动时先读 `sql.data` 再 replay 这个 WAL |
| `sql.data` | Snapshot 快照,由 checkpoint 时从 Catalog 全量 dump 生成。Server 正常 shutdown 自动做 checkpoint |

**三种运行模式**:

```powershell
# 1. 内存模式 (默认,不传 --data-dir)
#    不写磁盘,重启丢数据,适合开发调试
.\build\Debug\sqltiny_server.exe --port 3306 --user root

# 2. 指定持久化目录 (最常用)
#    第一次启动自动创建目录,生成 sql.wal + sql.data
.\build\Debug\sqltiny_server.exe --port 3306 --user root --data-dir D:\SQLTiny\data

# 3. 自定义位置 (相对或绝对路径都可以)
.\build\Debug\sqltiny_server.exe --port 3306 --user root --data-dir .\sqltiny_data
```

**修改存储位置**:停 server → 换 `--data-dir` 参数重启即可。新目录自动创建并初始化空 WAL。如果想迁移旧数据,直接把旧目录下的 `sql.wal` / `sql.data` 复制到新目录,Server 启动时会自动 replay。

> 注意:Server 正在运行时不要手动删除或修改 `sql.wal` / `sql.data`,会导致下次启动数据丢失或损坏。

### 当前支持的 SQL 子集

| 类别 | 语法 | 备注 |
|---|---|---|
| **多库 DDL** | `CREATE DATABASE [IF NOT EXISTS]` / `DROP DATABASE [IF EXISTS]` / `USE db` | Catalog 多库化,库级独立表 |
| **表 DDL** | `CREATE TABLE t(...) [IF NOT EXISTS]` / `DROP TABLE [IF EXISTS]` | PRIMARY KEY / AUTO_INCREMENT / DEFAULT / NOT NULL |
| **DML** | `INSERT INTO t VALUES(…, …)` / 多行 / 指定列 / `UPDATE t SET x=… WHERE …` / `DELETE FROM t WHERE …` | 显式 NULL 不被补 DEFAULT |
| **查询** | `SELECT cols FROM t WHERE … ORDER BY … LIMIT n` | IS NULL / IS NOT NULL / LIKE / IN / 比较运算 / AND / OR / NOT |
| **表达式** | `CASE [base] WHEN .. THEN .. [ELSE ..] END` / 标量子查询 / `?` 占位符 | 简单 CASE 与搜索 CASE |
| **聚合** | `COUNT(*)` / `COUNT(col)` / `SUM` / `AVG` / `MIN` / `MAX` | GROUP BY + HAVING 完整 |
| **表连接** | INNER JOIN / LEFT JOIN / CROSS JOIN / 多 JOIN 链 | 别名限定列 + LEFT JOIN 保留 NULL 行 |
| **子查询** | `WHERE col IN (SELECT …)` | 标量子查询 |
| **SHOW** | `SHOW DATABASES` / `SHOW TABLES` / `SHOW [FULL] COLUMNS FROM [db.]t` / `SHOW CREATE TABLE t` / `SHOW STATUS` / `SHOW VARIABLES [LIKE pattern]` / `SHOW INDEX|KEYS FROM [db.]t` / `SHOW TABLE STATUS [LIKE]` / `SHOW ENGINES` / `SHOW PLUGINS` / `SHOW CHARSET` / `SHOW COLLATION` / `SHOW PROCESSLIST` / `SHOW WARNINGS` / `SHOW TRIGGERS [WHERE]` | LIKE 支持 `%xxx%` / `xxx%` / `%xxx` / 精确匹配;支持 `` `db`.`tbl` `` 反引号限定名 |
| **元数据** | `information_schema.SCHEMATA` / `TABLES` / `COLUMNS` / `TABLE_CONSTRAINTS` / `KEY_COLUMN_USAGE` / `STATISTICS` / `TRIGGERS` / `ROUTINES` / `VIEWS` / `EVENTS` | 每次查询物化的虚拟表,第三方工具元数据浏览可用 |
| **系统** | `SET NAMES utf8mb4` / `SET AUTOCOMMIT = 0/1` / `SET [SESSION] TRANSACTION ISOLATION LEVEL …` | 真实生效;`@@tx_isolation` / `@@autocommit` 返回会话真实值 |
| **事务** | `BEGIN` / `START TRANSACTION` / `COMMIT` / `ROLLBACK` / `SAVEPOINT` / `ROLLBACK TO` / `RELEASE` | 四隔离级别 + 行锁 + 事务级 WAL 缓冲,commit 才可见 |
| **并发安全** | `TransactionManager.writeLock_` + 行锁 + `Table.shared_mutex` | SELECT 不加锁(MVCC 快照),写-写冲突立即 40001 |

### 已验证 SQL 示例

```sql
-- ============ 多库生命周期 ============
CREATE DATABASE shop;
USE shop;

CREATE TABLE users (id BIGINT PRIMARY KEY AUTO_INCREMENT,
                    name VARCHAR(64) NOT NULL,
                    age INT DEFAULT 18,
                    dept_id INT);
CREATE TABLE orders (id INT PRIMARY KEY AUTO_INCREMENT,
                     user_id INT, total DECIMAL(10,2) NOT NULL,
                     status VARCHAR(16) DEFAULT 'pending');

-- 插入含显式 NULL 的行
INSERT INTO users (name, age, dept_id) VALUES
  ('Alice', 28, 1), ('Bob', 32, 1), ('Charlie', 25, 2),
  ('Diana', 40, 2), ('Eve', NULL, NULL);

-- ============ 查询 ============
SELECT * FROM users ORDER BY id;                      -- Eve age=NULL 能正确返回
SELECT COUNT(*), SUM(age), AVG(age) FROM users;       -- 5, 125, 31.25 (PyMySQL 正确类型)
SELECT dept_id, COUNT(*), AVG(age) FROM users
  GROUP BY dept_id HAVING SUM(age) > 30;

-- ============ JOIN ============
SELECT u.name, o.total FROM users u
  INNER JOIN orders o ON o.user_id = u.id;

SELECT u.name, o.id, o.total FROM users u
  LEFT JOIN orders o ON o.user_id = u.id;             -- Eve 无订单, o.* 返回 NULL 行

-- ============ 更新 / 删除 ============
UPDATE users SET age = 30 WHERE name = 'Eve';
DELETE FROM users WHERE age < 30;

-- ============ 清理 ============
USE default;
DROP DATABASE shop;
```

---

## 📁 目录结构

```
SQLTiny/
├── CMakeLists.txt             顶层构建
├── CODEPLAN.md                完整项目规划(架构映射 + 里程碑)
├── architecture.svg           系统架构图(四隔离级别 + 事务数据流)
├── README.md                  本文件
├── vcpkg.json                 vcpkg manifest(仅依赖 fmt)
├── src/
│   ├── types/                 SqlType + Value 运行时值 + MySQL 协议类型映射
│   ├── parser/                Lexer + AST + 递归下降 Parser(支持多库/多 JOIN)
│   ├── storage/               AVL 内存索引 + 表存储
│   ├── catalog/               Catalog 多库化 (databases_[db].tables_)
│   ├── core/                  Database / Session 入口
│   ├── executor/              Executor + infer_type() 静态类型推断 + 内置函数
│   └── net/                   MySQL Wire Protocol 编解码(field_type 动态推断) + TCP server + 预编译语句
└── tests/
    ├── smoke_m1.cpp           Value / AVL / Lexer 冒烟
    ├── smoke_m2.cpp           CRUD / 聚合 / 表达式 / DDL / NULL
    ├── smoke_m3.cpp           GROUP BY / HAVING / 子查询 IN / INSERT SELECT
    ├── smoke_m4.cpp           CREATE/DROP DB / USE / SHOW / SET 占位
    ├── smoke_m5_wal.cpp       WAL Replay + CRC + Checkpoint 恢复
    ├── smoke_m25_e2e.cpp      Winsock 直连 server E2E
    ├── smoke_m6_isolation.cpp 四隔离级别 + 事务/保存点/冲突
    ├── smoke_m6_hsqldb_matrix.cpp  HSQLDB 可移植 SQL 语义矩阵
    ├── smoke_m6_session_db.cpp 多库会话级隔离(每连接 USE / 握手 db)
    ├── dbx_probe.cpp          DBX/dbExpress 客户端兼容性探针(58 断言)
    ├── test_stmt_prepare.py   预编译语句 binary protocol 测试(7 断言)
    ├── test_full_flow.py      PyMySQL 全链路(双连接,含事务/隔离级别)
    ├── test_sqlalchemy.py     SQLAlchemy 2.x 15 组:Engine/Core/ORM/2.0 表达式/事务/类型映射/Inspector
    ├── test_vs_sqlite.py      SQLTiny vs SQLite 行为对比
    ├── test_perf_vs_sqlite.py 大数据量 INSERT + JOIN 性能对比
    ├── test_concurrency.py    多线程并发读写
    ├── test_wal_recovery.py   server 重启 / UPDATE / DELETE 状态恢复
    └── test_txn_vs_sqlite.py  四隔离级别 + 事务结果与 SQLite 对比、WAL 重启验证
```

---

## ❓ 常见问题

**Q: 启动 mysql 客户端时报错 `Can't connect`?**
A: 确认 `sqltiny_server.exe` 正在运行,监听 3306 端口:
```powershell
netstat -ano | findstr :3306
```

**Q: PyMySQL 连时报 `invalid literal for int()`?**
A: M4 前 version 字符串以 `SQLTiny-` 开头,PyMySQL 解析 version 失败。M4 已修成 `5.7.0-SQLTiny`。如果还有这个错,确认是最新 build。

**Q: PyMySQL 报 `Packet sequence number wrong` / `InterfaceError (0, '')`?**
A: 这通常是前一个查询没正确处理错误包导致连接状态不同步。重新建立连接即可;也可以先 `conn.ping(reconnect=True)`。

**Q: 密码登录失败?**
A: 开发期不校验密码,user 必须是 `root`,密码任意填(`mysql -u root -p` 直接回车也行)。

**Q: 支持哪些 MySQL 版本?**
A: 协议层按 MySQL 8.0 Client 41 实现,`mysql` CLI / Navicat / DBeaver / Connector/J / PyMySQL 均已验证。

---

## 📜 许可证

Apache License 2.0 — 见 [`LICENSE`](./LICENSE) 文件。

## 🤝 致谢

- [HSQLDB](http://hsqldb.org/) — 架构和数据结构的重要参考(AVL 索引、MVCC、Session 分层)
- [MySQL Wire Protocol](https://dev.mysql.com/doc/dev/mysql-server/latest/) — 协议层参考
