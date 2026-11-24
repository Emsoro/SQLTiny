# SQLTiny CodePlan: HSQLDB → C++ 移植 + MySQL 协议兼容

> 目标:用现代 C++ (C++20) 重写 HSQLDB 核心引擎,构建一个轻量级嵌入式数据库 **SQLTiny**,同时暴露 **MySQL 客户端协议** 兼容端口——让标准 MySQL 客户端(mysql CLI、Navicat、JDBC Connector/J、Go sql.Open("mysql",...) 等)直接连接 SQLTiny。
>
> 源码输入:HSQLDB 2.7.4 `org.hsqldb`(Session / TransactionManagerMVCC / RowAction / persist)
> 当前工作区:本仓库根目录

---

## 0. 当前状态(2026-11-25,v2.2.3)

> 下文 M1–M5 为原始规划、M6 各节含设计稿伪代码,均保留供追溯。**落地状态以本节与各节 ✅ 标记为准;实际目录与运行方式见 `README.md`,架构图见 `architecture.svg`。**

### 交付进度

| 里程碑 | 状态 | 说明 |
|---|---|---|
| M1 / M2 / M2.5 / M3 | ✅ | 类型/AVL/Lexer、DDL/DML、MySQL 协议接入、子查询与 JOIN |
| M4 | ✅ | SHOW 系列、多库 Catalog、协议字段类型推断(`DESCRIBE` / `SHOW FULL TABLES` 已补) |
| M5 | ✅ | WAL + Snapshot + Replay、AUTO_INCREMENT、并发安全 |
| **M6 P0** | ✅ | 行锁 + `TransactionManager` + `Table.shared_mutex`,SELECT 走 MVCC 快照不加行锁 |
| **M6 P1 事务部分** | ✅ | 四隔离级别、autocommit、SAVEPOINT、WAL 事务缓冲、协议状态位 |
| M6 P1 边缘项 | ⬜ | `FOR UPDATE` / `LOCK IN SHARE MODE`、死锁检测 / 等待队列 |
| **M6 P3 客户端兼容(v1.1.0)** | ✅ | **服务端预编译语句(`COM_STMT_*` binary protocol)、information_schema 虚拟表、SHOW 扩展、CASE 表达式、EOF 帧兼容、`--query-log`** |
| **M6 P2 JOIN 优化** | ✅ | Hash Join + 预建列索引 + 成本估算 + `EXPLAIN`;INNER/LEFT JOIN 约 77x / 62x 提速 |
| **M6 P2 JOIN 优化·续(v1.2.5)** | ✅ | **PK 前缀索引探测** + WHERE 谓词下推 + SUM(DECIMAL) 列类型一致性修复;employees 3 表 JOIN 77s→23.8s |
| **存储引擎升级(v2.0.0)** | ✅ | 主键索引 **AVL → B+ 树**(`IndexBPlusTree`);索引级点查 +19% / 前缀扫描 +24% / 全量遍历 7.6x / 树高 22→4;端到端 employees JOIN 18.99s→18.42s(Release 基线) |
| **3 表 JOIN 执行优化(v2.0.1)** | ✅ | ExecContext 零拷贝 + idx_probe 跳过右表物化 + **聚合下推到 PK 前缀探测**;employees 3 表 JOIN **18.25s→1.97s**(9.3x),与 MySQL 差距 11.7x→1.25x |
| **下推覆盖面扩展(v2.0.2)** | ✅ | WHERE 按表侧拆分 + 哈希等值 JOIN 也聚合下推 + 子查询/CASE/占位符/星号不下推加固;高薪员工数 2.76s→0.94s · 2 表哈希计行 0.37s→0.13s |
| **主键点查索引探测(v2.0.3)** | ✅ | `WHERE PK首列=同族字面量` 改走 `for_each_by_pk_prefix`(谓词仍逐行复核)+ `dead_rows_` / `has_dead_rows()` 在逻辑删除历史存在时回退全扫;employees 点查 74ms→0.13ms · salaries 678ms→0.17ms |
| **融合聚合 + UNION ALL(v2.0.4)** | ✅ | 单表聚合单遍扫描快路径(`SUM` 2.97s→**0.35s**、`WHERE salary>100000` 计行 0.774s→**0.248s**)+ UNION/UNION ALL(去重、链式、尾部 ORDER BY/LIMIT)+ `ORDER BY 1` 位置序号 |
| **复杂查询正确性 + 语法补全(v2.0.5)** | ✅ | compare_employees 扩至 39 条;D1–D3 静默正确性缺陷、S1–S8 语法缺口(WITH/递归 CTE/标量子查询/EXISTS/派生表/COUNT(DISTINCT)/BETWEEN/窗口函数)、P1 子查询物化与去相关全部落地;**39/39 MATCH / 0 MISMATCH / 0 ST-ERR**,基础类 SQLTiny 5.45s vs MySQL 14.00s |
| **MySQL 语义兼容修复(v2.0.6,2026-11-16)** | ✅ | 由独立 sqllogictest 套件(与本仓库同级的 `SQLTinyTest/testcase`)驱动:**FAIL 67→0**;SLT 1184 项 PASS 971→**1092** / GAP 146→**92** / FAIL **0**,协议套件 **42/42**,持久化 **15/15**。修复覆盖值语义、协议、SQL 校验、JOIN/窗口、元数据、内置函数六类,P0 语义批(`<=>`/CAST/CONVERT/会话函数/聚合扩展)已完成,详见下节 |
| **P1-a 日期时间函数模块(v2.0.7)** | ✅ | `DATEDIFF`/`DATE_ADD`/`DATE_SUB`/`LAST_DAY`/`DAYOFWEEK`/`WEEK`/`DATE_FORMAT`/`STR_TO_DATE`/`UNIX_TIMESTAMP`/`FROM_UNIXTIME`/`CURTIME` + 无括号 `CURRENT_*` + INTERVAL 语法;SLT **1184 / PASS 1106 / GAP 78 / FAIL 0**,协议 42/42,持久化 15/15 |
| **P1-b DML 方言 + DDL 核心(v2.0.8)** | ✅ | INSERT 方言(VALUE/SET/IGNORE/REPLACE/ON DUPLICATE)、UPDATE/DELETE ORDER BY LIMIT、多表 UPDATE、TRUNCATE、AUTO_INCREMENT 起始值/非主键自增、CTAS/LIKE/TEMPORARY/VIEW/INDEX/ALTER/RENAME、DDL 隐式提交;SLT **1184 / PASS 1132 / GAP 52 / FAIL 0** |
| **聚合下推增强(v2.0.9)** | ✅ | 标量函数包裹聚合(ROUND(AVG))与 HAVING 下推;3 表 JOIN+聚合+HAVING 18.7s→~5s;SLT **1184 / PASS 1132 / GAP 52 / FAIL 0** |
| **benchmark TOP 查询优化(v2.1.0)** | ✅ | 单表 GROUP BY 流式聚合、非相关标量子查询常量折叠、IN 候选集 hash 索引;rank1 10.3s→1.15s / rank2 8.17s→0.77s / rank20 0.67s→0.43s;SLT **1184 / PASS 1132 / GAP 52 / FAIL 0** |
| **命令行导入 / dump 导出(v2.2.0)** | ✅ | `sqltiny_server --import FILE` 导入 SQL/mysqldump 脚本(注释、`DELIMITER`、`LOCK TABLES` 跳过、错误继续策略);`--dump FILE` 导出 MySQL 风格建库/建表/多行 INSERT(dump 库/表过滤、只结构/只数据、INSERT 批大小);`smoke_cli_io` 40/40 |
| **ASan 构建 + 模糊测试支撑(v2.2.1)** | ✅ | 可选 `windows-asan` preset(`/fsanitize=address`,无需 Clang/LLVM)、ASan 运行时 DLL 自动部署、fmt STL 注解冲突规避;ASan 下 SLT 1184/PASS 1133/GAP 51/FAIL 0、协议 42/42、持久化 15/15;同级 `SQLTinySLT` 接入 Hypothesis 状态化 fuzz(100+ examples,0 崩溃/协议失步/oracle 失败) |
| **benchmark TOP5 查询优化(v2.2.2,2026-11-24)** | ✅ | Release 同条件对比 v2.2.1:CASE 流式分组 7.02s→2.59s、EXISTS 首行即停 4.11s→4.12s(持平)、融合 JOIN 裸列直取 4.26s→3.63s、DISTINCT 提前去重 2.00s→0.81s、PK 上界范围扫描 1.62s→0.02s;复杂类总耗时 18.90s→10.92s(-42%);compare_employees **39/39 MATCH**、SLT **1184 / PASS 1132 / GAP 52 / FAIL 0** |
| **benchmark 两轮定向优化(v2.2.3,2026-11-25)** | ✅ | 第一轮 priority 前三项:EXISTS 半连接物化 2.086s→0.702s、小维度表过滤键反传 1.079s→0.435s、日期函数流式分组 0.824s→0.442s;第二轮 delta 第 3/4/6 项:EXISTS 整数键 1.05–1.13x、DISTINCT 投影期去重 1.05–1.08x、COUNT(DISTINCT) 裸列偏移 1.36–2.28x;39 条总差距 10.55s→5.56s(-47%);compare_employees **39/39 MATCH**、CTest **13/13**、SLT **1184 / PASS 1133 / GAP 51 / FAIL 0** |
| M6 P4 | ⬜ | 复制与更大测试覆盖;认证(caching_sha2_password / SSL)仍未做 |

### 本轮交付(v1.1.0 客户端兼容性)

以 Delphi dbExpress(DBX)驱动与类 Navicat 图形工具实测为驱动,逐条用 `--query-log` 抓取失败语句修复:

- **服务端预编译语句**:`COM_STMT_PREPARE`(0x16)/ `COM_STMT_EXECUTE`(0x17)/ `COM_STMT_CLOSE` / `COM_STMT_RESET` / `COM_STMT_SEND_LONG_DATA`。参数按 binary protocol 全类型解码(int/float/字符串/日期时间/NULL 位图)拼回 SQL 重放执行;SELECT/SHOW 在 prepare 阶段用 NULL 替换占位符探测列元数据(只读、安全);SELECT 结果以 binary resultset 返回。
- **EOF 帧兼容**:服务端不再广播 `CLIENT_DEPRECATE_EOF`,永远用经典 EOF 帧——修复 DBX 等老客户端回显能力位后挂起 5 秒的问题。
- **information_schema 虚拟表**:`SCHEMATA` / `TABLES` / `COLUMNS` / `TABLE_CONSTRAINTS` / `KEY_COLUMN_USAGE` / `STATISTICS` / `TRIGGERS` / `ROUTINES` / `VIEWS` / `EVENTS`。每次查询物化,createSCN=0 恒可见;支持 `information_schema.COLUMNS c LEFT JOIN information_schema.KEY_COLUMN_USAGE pk ON ...` 这类工具元数据查询(含 `CASE WHEN ... END` 投影)。
- **SHOW 扩展**:`ENGINES` / `PLUGINS` / `CHARSET` / `CHARACTER SET` / `COLLATION` / `TABLE STATUS [LIKE]` / `INDEX|KEYS FROM [db.]t` / `[FULL] COLUMNS FROM [db.]t` / `PROCESSLIST` / `WARNINGS` / `TRIGGERS [WHERE]`;`` `db`.`tbl` `` 反引号限定名解析。
- **表达式与词法**:`CASE [base] WHEN .. THEN .. [ELSE ..] END`(简单/搜索,解析 + 求值 + 类型推断);`AUTO_INCREMENT` 等关键字放开可用作列名。
- **协议命令**:`COM_FIELD_LIST`(db 限定 + `\0` 截断)、`COM_STATISTICS`、`COM_PROCESS_INFO`、`COM_CHANGE_USER`;SSL 请求返回明确 ERR;`@@max_allowed_packet` / `@@net_write_timeout` / `@@net_read_timeout` / `@@sql_mode` / `@@lower_case_table_names` / `@@sql_auto_is_null`。
- **调试设施**:`--query-log FILE`(逐条记录命令 0x?? + SQL + 执行结果);`tests/dbx_probe.cpp`(58 断言,进程内用 `Session::execute` 逐条跑 DBX 连接/元数据/事务阶段的典型 SQL)、`tests/functional/test_stmt_prepare.py`(7 断言,裸 socket 走 `COM_STMT_*` 全流程)。
- **回归适配**:`tests/smoke_m25_e2e.cpp` 改为兼容经典 EOF 帧(列定义后的 `0xfe` 包,包长 < 9),否则老用例会把 EOF 误判为结果集结束;`ctest` 在启动 server 后恢复 10/10。

### 本轮交付(v2.0.5 复杂查询正确性修复 + 语法补全 + 子查询物化)

**问题来源**:`tests/benchmark/compare_employees.py` 由 14 条扩展至 **39 条**(基础 14 / 嵌套子查询 10 / WITH·CTE 4 / 常见复杂查询 11),与 MySQL 3306 同库双端执行、规范化比对。首轮结果:**25 MATCH / 3 MISMATCH / 11 ST-ERR**;分类耗时 MySQL 25.27s vs SQLTiny 265.07s(嵌套类 2.70s vs 202.48s,差距集中在子查询逐行重执行)。

**最终结果(v2.0.5)**:`39/39 MATCH、0 MISMATCH、0 ST-ERR`;分类耗时 `基础 14.00s vs 5.45s / 嵌套 3.26s vs 5.84s / WITH 1.42s vs 4.13s / 复杂 9.82s vs 12.73s`(MySQL vs SQLTiny)。修完的 3 条超时项:高基数 GROUP BY(salaries 30 万组)100s→**3.80s**、相关 `IN` 子查询 100s→**0.096s**、递归 CTE 100s→**0.001s**。

**附带修复**:`compare_employees.py` 改为每条查询独立连接 + `read_timeout=100`,避免某条超时后 PyMySQL 连接失效、后续查询连锁误报 `(0,'')`(首轮 11 条 ST-ERR 中大部分是这种假象);GROUP BY 分桶由线性查找改 `unordered_map`(O(组数²)→O(1));递归 CTE 由「累积全集」改为「上一轮增量」工作集;子查询去相关时必须置 `outer_used`,否则相关子查询结果被误缓存,只返回第一行。

**A. 静默正确性缺陷(不报错、结果错,优先级最高)**

| # | 缺陷 | 最小复现 | 根因定位 | 状态 |
|---|---|---|---|---|
| D1 | GROUP BY 别名完全不生效 | `SELECT gender AS g, COUNT(*) FROM employees WHERE emp_no<=10100 GROUP BY g` → 单组 `('M',100)` | 分组键未把别名解析回 SELECT 表达式,按未知列求值 NULL → 全表塌成一组;CASE 分桶 284 万行全落 `'2_mid'`(MISMATCH)同因。`GROUP BY` 重复写原表达式则正确 | ✅ 别名解析回 SELECT 表达式 |
| D2 | 标量函数内嵌聚合返回 0 | `SELECT ROUND(AVG(salary),2) FROM salaries WHERE emp_no<=10010` → `'0'`(AVG 单用正确) | 聚合输出路径 `eval_nested` 只特判 `coalesce`/`ifnull`(executor.cpp:1862),其余函数参数内的聚合按普通列求值 → NULL → ROUND→0;3 表 JOIN+HAVING+ROUND(AVG) 各部门均薪全 0(MISMATCH)同因 | ✅ 聚合上下文递归求值 |
| D3 | 日期函数缺失静默返回 NULL | `SELECT YEAR(hire_date), COUNT(*) FROM employees GROUP BY 1` → 单组 `(NULL,300024)` | 未实现函数一律返回 NULL(expr.cpp:413),无 YEAR/MONTH/DAY 等;按年分组塌成一组(MISMATCH) | ✅ YEAR/MONTH/DAY 等已实现 |

**B. 语法缺口(解析期 1064,ST-ERR)**

| # | 语法 | 用例 | 状态 |
|---|---|---|---|
| S1 | WITH / CTE(单 / 多 CTE) | `WITH dc AS (…) SELECT … FROM dc JOIN …` | ✅ |
| S2 | WITH RECURSIVE 递归 CTE | `WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n<100) …` | ✅ 增量工作集迭代(修掉累积全集导致的指数膨胀) |
| S3 | 标量子查询(WHERE 比较 / SELECT 列,含相关) | `salary > (SELECT AVG(salary) …)`、`SELECT …, (SELECT MAX(s.salary) … WHERE s.emp_no=e.emp_no)` | ✅ |
| S4 | EXISTS 相关子查询 | `WHERE EXISTS (SELECT 1 FROM titles t WHERE t.emp_no=e.emp_no …)` | ✅ 外层列等值走 PK 前缀探测 + `outer_used` 禁止误缓存 |
| S5 | FROM 派生表 | `FROM (SELECT dept_no, COUNT(*) c FROM dept_emp GROUP BY dept_no) x` | ✅ |
| S6 | COUNT(DISTINCT) | `COUNT(DISTINCT de.emp_no)`(解析器把 DISTINCT 当列名) | ✅ |
| S7 | BETWEEN 谓词 | `salary BETWEEN 60000 AND 70000`(BETWEEN 是保留字但无解析分支) | ✅ |
| S8 | 窗口函数 OVER | `ROW_NUMBER() OVER (PARTITION BY emp_no ORDER BY from_date DESC)` | ✅ PARTITION BY + ORDER BY + 投影后置编号,类型 BIGINT |

**C. 性能问题(与正确性缺陷同等对待)**

| # | 问题 | 实测 SQLTiny vs MySQL | 根因 | 状态 |
|---|---|---|---|---|
| P1 | IN 子查询按外层逐行重执行 | 16.28s vs 0.01s(**1600x**);NOT IN 15.77s vs 0.08s;两层嵌套 IN **107.44s**;相关 IN 34.22s | `__in` 每次求值都 `ctx.exec->execute` 重跑子查询(expr.cpp:294),无物化缓存;外层 30 万行 × 内表全扫 | ✅ 非相关子查询物化缓存 + 相关子查询外层列等值走 PK 前缀探测(100s 超时→0.10s) |
| P2 | UNION ALL 多分支 COUNT 未走融合快路径 | 三分支 `COUNT(*)` 8.37s vs 0.23s(单分支快路径应 ~0.4s/条) | 各分支已走单表聚合快路径,降至 0.71s(仍慢于 MySQL 0.09s:包装层逐分支执行) | ✅ |
| P3 | HAVING 使聚合下推失效 | 3 表 JOIN+HAVING+ROUND 19.77s vs 4.10s | 融合下推判据排除 HAVING(v2.0.1 既有保守限制),回退全量物化 | ⬜ |
| P4 | DISTINCT 全量物化去重 | `DISTINCT title` 2.79s vs 0.29s | 443k 行全物化后按字符串键去重,无索引 loose scan | ⬜ |

**本轮修复范围**:D1–D3、S1–S8 全部落地;P1 已修复(非相关物化缓存 + 相关去相关),P2 已改善;P3(HAVING 使聚合下推失效)、P4(DISTINCT 全量物化)记录基线后**转待办**。

**回归口径与结果**:`compare_employees.py` 39 条 → **39 MATCH / 0 MISMATCH / 0 ST-ERR**;`ctest` **12/12** 全通过(`smoke_m25_e2e` 端口统一为 3307,不再受 3306 MySQL 干扰);新增 `smoke_sql_keywords`(649 PASS)、`smoke_sql_syntax`(160 PASS / 15 GAP)随 CTest 一起跑。

### 本轮交付(v2.0.7 P1-a 日期时间函数模块,2026-11-16)

由 `SQLTinySLT/slt/05_function/03_datetime.slt` 的 14 条 GAP 驱动,一次补齐日期时间模块,GAP **92→78**、FAIL 保持 0。

- **解析层**:lexer 新增 `INTERVAL`;parser 把 `INTERVAL n UNIT` 折为 `__interval(n,'UNIT')` 表达式供 `DATE_ADD`/`DATE_SUB` 消费;无括号 `CURRENT_DATE` / `CURRENT_TIMESTAMP` / `CURRENT_TIME` token 直接折成函数调用(此前落为列名 → NULL)
- **求值层**(`src/executor/expr.cpp`):统一 `to_dt` 入口,civil-days 算法(含 719468 epoch 偏移)实现 `DATEDIFF` / `DATE_ADD` / `DATE_SUB` / `LAST_DAY` / `DAYOFWEEK`(1=周日)/ `WEEK`;`UNIX_TIMESTAMP` / `FROM_UNIXTIME` 用 `_mkgmtime`/`gmtime_s`;`DATE_FORMAT` / `STR_TO_DATE` 支持 `%Y %m %d %H %i %s`;月末按当月天数收敛;支持 YEAR/MONTH/QUARTER/DAY/WEEK/HOUR/MINUTE/SECOND 单位
- **算术**:`DATETIME - DATETIME` 返回秒差(`NOW()-NOW()` = 0)
- **顺带修复**:P0 引入的 1055 回归 —— `SELECT dept_name … GROUP BY d.dept_name` 的裸名/限定名现在视为同一列

**验证**:SLT **1184 / PASS 1106 / GAP 78 / FAIL 0**(`03_datetime.slt` 45/45,14 条 `gap` 标记移除);协议 **42/42**;持久化 **15/15**;`compare_employees.py` **39/39 MATCH**。

### 本轮交付(v2.2.0 命令行导入 / dump 导出,2026-11-17)

`sqltiny_server.exe` 增加批处理入口：`--import` / `--dump` 不启动网络服务，直接对 `--data-dir` 下的库执行脚本导入或导出，完成后退出。实现集中在 `src/cli/sql_io.{h,cpp}`，`src/main.cpp` 只做参数解析与流程编排。

- **脚本切分**：逐字符状态机（普通 / 单引号 / 双引号 / 反引号 / 行注释 / 块注释）流式切分语句，识别 `--`、`#`、`/* */`、MySQL `/*! */` 条件注释与 `DELIMITER` 自定义分隔符；转义字符（`\'`、`\"`、`\\`、`` \` ``）与 MySQL 反引号标识符正确处理，注释不进入语句文本
- **mysqldump 兼容**：导入时跳过 `LOCK TABLES` / `UNLOCK TABLES`；parser 补列级 `COLLATE`、`CHARACTER SET`、`ON UPDATE` 与任意位置的表级 `AUTO_INCREMENT=n`
- **导入策略**：逐条 `Session::execute` 自动提交；`--import-continue-on-error` 失败后继续并记录首条错误，`--import-quiet` 抑制进度，`--import-then-serve` 成功后不退出而是继续 `run_mysql_server`
- **dump 生成**：遍历 catalog 输出 `CREATE DATABASE IF NOT EXISTS` / `USE` / `DROP TABLE IF EXISTS` / `CREATE TABLE`（主键、默认值、AUTO_INCREMENT 游标）/ 多行 `INSERT`；`--dump-database` / `--dump-table` 过滤，`--dump-no-create` / `--dump-no-data` / `--dump-no-drop` 裁剪，`--dump-batch-rows` 控制单条 INSERT 行数；文本值按 MySQL 规则转义
- **stdout 纯净**：引擎与导入/dump 日志统一改到 stderr（`Session`、`WalManager`、`Snapshot`），`--dump -` 可安全重定向为纯 SQL 文件

**验证**：`smoke_cli_io` **40 checks / 0 FAIL**（注释/条件注释、引号转义、`DELIMITER`、`LOCK TABLES` 跳过、dump→import 往返、默认值与自增游标）；手工验证 mysqldump 风格脚本导入、`--dump -` 重定向、`--import-then-serve` 服务启动。`smoke_m6_isolation` 的系统变量断言改为 MySQL 口径（`@@autocommit`=1/0、隔离级别原样返回），用真实 MySQL 8.4（3306）逐项对比后 **493/493 PASS**；全量构建通过，`ctest` 仅 `smoke_m25_e2e`（需先启动 3307 server）在未启动服务时失败。

### 本轮交付(v2.1.0 benchmark TOP 查询优化,2026-11-16)

由 `benchmark/employees_query_detail.csv` 的一级/二级对比驱动,按 `rank_by_delta` 优化 TOP 项(各查询单独拆解耗时定位根因)。

- **rank2 `COUNT(*) WHERE salary > (SELECT AVG(salary))`(8.17s→0.77s)**:根因不是子查询重复执行(非相关子查询已按 AST 节点缓存),而是 WHERE 里 rhs 是 Subquery 使 `add_pred` 拒绝该谓词、`fast_agg` 失效,284 万行走通用路径。修复:扫描前 `fold` 递归把非相关标量子查询(用 `execute_sub` + `used==false` 判定)求值为 Literal,生成 `folded_where` 供 `add_pred` 使用,恢复 fast_agg
- **rank20 `COUNT(*) WHERE emp_no IN (SELECT ...)`(0.67s→0.43s)**:根因是 `__in` 对缓存的候选集做逐行线性 compare。修复:`SqEntry` 新增 `keys_built`/`saw_null`/`unordered_set<string> keys`,`subquery_rows` 首建时按 `in_hash_key`(数值统一 double,与 `Value::compare` 语义一致)建索引;`in_subquery_contains` 用 hash 查找,保留 NULL→UNKNOWN 三值逻辑,相关子查询仍线性兜底
- **rank1 CTE `MAX(salary) GROUP BY emp_no`(10.3s→1.15s)**:根因是单表高基数 GROUP BY 无流式路径,284 万整行物化进 30 万桶再逐桶 eval_agg。修复:`stream_group` 快路径(单表/简单分组列/简单聚合/无 HAVING-ORDER-LIMIT-WINDOW),`for_each_visible` 边扫描边按 group key 累加 `SAcc`(COUNT*/COUNT(col)/SUM/AVG/MIN/MAX/分组列/常量),不物化中间行;WHERE 用折叠后的 `fast_where` 拆简单谓词

**验证**:SLT **1184 / PASS 1132 / GAP 52 / FAIL 0**(无回归);compare_employees **39/39 MATCH**;GROUP BY 10 类边界(NULL/多分组列/多聚合/COUNT(col)/常量列/WHERE)全 MATCH;IN 含 NULL 三值逻辑 MATCH。

### 本轮交付(v2.0.9 聚合下推增强,2026-11-16)

由 `benchmark/employees_latest.json` 的二级对比定位:TOP1 瓶颈是 3 表 JOIN + `ROUND(AVG(salary),2)` + `HAVING AVG(...)` 查询,18.7s(MySQL 1.05s,17.85x)。根因:聚合下推判据把"标量函数包裹聚合"与"HAVING"两类查询排除,回退到物化 284 万行 salaries 中间结果的慢路径。修复后该查询降到 ~5s(约 4x),TOP 瓶颈排名从第 1 降到第 4。

- **判据扩展**(`executor.cpp` `exec_select_body`):
  - 新增 `count_agg_nodes` / `agg_kind_of` / `agg_sig` helper;SELECT 列识别支持"唯一聚合节点被标量函数包裹"(如 `ROUND(AVG(x),2)`),记录 `wrap`/`agg_node`/`sig`
  - 去掉 `!s.having` 限制;产出结果后用 `clone()` + 原地替换把 HAVING 的聚合节点按签名换成该组已累加的纯聚合值、列别名换成输出列值,再 eval 过滤
  - HAVING 引用的聚合若不在 SELECT 投影中(`having_ok` 校验失败)则禁用下推,回退通用路径保证语义
  - `apply_wrap` 把聚合结果替换进外层函数参数后套用标量函数
- **bug 修复**:`fused_raw_aggs` 与 `fused_cols` 索引对齐(非聚合列也 push 占位,原实现导致 HAVING 取错列而返回空/崩溃);`Expression::clone()` 补拷 `is_window`/窗口字段

**验证**:SLT **1184 / PASS 1132 / GAP 52 / FAIL 0**(无回归);compare_employees **39/39 MATCH**,目标查询 18.7s→~5s;HAVING 6 类边界(别名/多聚合/COUNT/WHERE混合/ROUND包裹/直接AVG)全部返回正确行;CTest 2 项既有 smoke 偏差与本次无关(HEAD 基线已确认)。

### 本轮交付(v2.0.8 P1-b DML 方言 + DDL 核心,2026-11-16)

由 `SQLTinySLT` 的 P1-b 批次驱动,GAP **78→52**、FAIL 保持 0。AST 一次性扩展(Insert/Update/Delete/CreateTable 增字段 + 5 个新 DDL StmtKind),lexer 新增 `RENAME` token,再分 parser / executor / catalog / storage / session 五层落地。

- **INSERT 方言**(`parser.cpp` / `executor.cpp`):`VALUE` 单数、`() VALUES()`(NOT NULL 数值列填 0)、`INSERT ... SET`(折成单行多列)、`INSERT IGNORE`(冲突 warnings++ 跳过)、`REPLACE INTO`(冲突删旧插新)、`ON DUPLICATE KEY UPDATE`(冲突按表达式更新);1062 冲突分支按三种策略分流
- **UPDATE / DELETE**(`executor.cpp`):ORDER BY 排序键 + LIMIT 截断(先收集再排序);多表 `UPDATE ... JOIN ... SET a.c=b.d` 用扩展行 + 合并 col_index 在关联行上下文求值 SET;SET 列名支持 `a.c` 限定
- **AUTO_INCREMENT**(`catalog.cpp` / `table.cpp`):建表 `AUTO_INCREMENT=n` → cursor=n-1;非主键自增列建表 + `KEY(seq)` 语法可解析
- **DDL 核心**(`parser.cpp` / `executor.cpp` / `catalog.cpp` / `table.cpp`):CTAS(查询推断列+填数据)、`LIKE`(复制定义)、`TEMPORARY`、`VIEW`、`CREATE [UNIQUE] INDEX`、`ALTER TABLE ADD COLUMN`(现有行补默认)、`RENAME TABLE`、`TRUNCATE`(清行+重建唯一索引+游标归零);表级/列级 `KEY`/`UNIQUE`/`CHECK` 语法解析
- **DDL 隐式提交**(`session.cpp`):DDL 语句执行前若在事务中先 commit,之后的 ROLLBACK 不再撤销此前的 DML(MySQL 语义)

**验证**:SLT **1184 / PASS 1132 / GAP 52 / FAIL 0**;协议 **42/42**;持久化 **15/15**。CTest 12 项中 2 项 smoke 失败(`smoke_m25_e2e` 端口依赖、`smoke_m6_isolation` 的 `@@autocommit` 显示格式),已用 git stash 切到 HEAD 基线对比确认为既有问题,与本次改动无关。

### 本轮交付(v2.0.6 MySQL 语义兼容修复,2026-11-16)

**驱动与口径**:外部 sqllogictest 套件(与本仓库同级的 `SQLTinyTest/testcase`,36 个 `.slt` + 2 个 Python 套件,期望值按 MySQL 8.0 语义书写)。首轮 **1184 项 / PASS 971 / GAP 146 / FAIL 67**;修复后 **PASS 1092 / GAP 92 / FAIL 0**,协议套件 **42/42**,持久化 **15 PASS / 0 GAP**。

**A. 值语义与表达式(`src/types/value.cpp` / `src/executor/expr.cpp`)**

| 项 | 修复 |
|---|---|
| NULL 排序 | `Value::compare` 把 NULL 视为最小值 → ASC 最前 / DESC 最后 |
| 三值逻辑 | `__in` 对 NULL 左值或列表含 NULL 返回 UNKNOWN;`NOT IN (…,NULL)` 不再误返行 |
| LIKE | `std::regex::icase`,默认 collation 不区分大小写 |
| 隐式数值转换 | 算术/比较表达式里 string↔numeric 按数值提升(`'5' + 3 = 8`);非数值字符串参与数值运算返回 NULL。**已知例外**:`列 = '数字字符串'` 的等值比较仍按字符串语义(`sal = '9000'` 不命中),列入 sqllogictest GAP |
| NULL 传播 | `length` / `char_length` / `round` / `instr` / `locate` 等 NULL-safe |
| `NULLIF` / `IF` | `NULLIF` 修复(此前恒 NULL);`IF` 求值已实现,但解析器在函数名位置按关键字处理 → 1064,列入 GAP |
| `ROUND` | DECIMAL 字面量按定点字符串舍入,`ROUND(1.005,2)=1.01` |
| 未知函数 | `raise_error(1305, "42000", "FUNCTION ... does not exist")`;窗口/聚合/日期/CAST 等已知未实现函数仍按白名单静默 NULL |
| 连接函数 | `connection_id` / `last_insert_id` / `row_count` / `found_rows` / `session_user` / `system_user` / `current_role` |
| 字符串函数 | `upper/lower/trim/ltrim/rtrim/substr/substring/left/right/replace/reverse/lpad/rpad/repeat/concat_ws/ascii/ord/char` |
| 数值函数 | `ceil/ceiling/floor/mod/power/pow/sqrt/sign/truncate/greatest/least` |
| 错误通道 | Executor 新增 `eval_errno_` / `eval_sql_state_`,表达式层错误经 `take_eval_error()` 上抛为 MySQL ERR 包 |

> 其中 `left` / `right` / `truncate` 的求值实现已存在于 `expr.cpp`,但解析器在这些位置按关键字处理(1064),仍需在函数调用上下文放开;`cast` / `convert` 同属此类。

**B. 协议(`src/net/mysql_proto.cpp` / `infer_type`)**

- BIGINT 极值:整型列且值为整型 double 时以 `"{:.0f}"` 文本下发,不再出现 `-9.223372036854776e+18`
- 表达式结果列类型:BinaryOp / Case / 窗口函数由 `infer_type` 推断;UNION 左右类型不一致统一为 `Varchar`
- `SELECT 1` 列类型为 LONGLONG;`COM_STMT_EXECUTE` 字符串参数绑定整数列时按数值命中
- `COM_STMT_PREPARE` 语法错误返回 1064

**C. SQL 校验(`src/executor/executor.cpp` / `src/parser/parser.cpp`)**

| 错误码 | 场景 |
|---|---|
| 1048 | `coerce_row`:NOT NULL 写入 NULL;AUTO_INCREMENT 显式 NULL 分配下一个值 |
| 1054 | 未知列(SELECT 表达式路径 + INSERT 列清单) |
| 1136 | INSERT 列数与值数不一致 |
| 1366 / 1264 | 整数列写入非数字 / 超出 TINYINT..BIGINT 范围 |
| 1406 | VARCHAR / CHAR 超长 |
| 1064 | 缺投影列表(`SELECT FROM t`) |
| 1242 / 1241 | 多行标量子查询 / IN 子查询多列 |
| 1248 | 派生表缺别名 |
| 1049 | 限定名指向不存在的库 |
| 1111 / 1055 / 3593 | 嵌套聚合 / ONLY_FULL_GROUP_BY / 窗口函数出现在 WHERE |
| 1222 | UNION 列数不匹配 |

**D. 查询能力**

- 逗号连接在 `parse_from` 转成隐式 `CROSS JOIN`
- `RIGHT JOIN` 补齐右表无匹配行(左列 NULL)
- 链式 JOIN 列索引只登记 alias-or-real-name,避免 `d.id` 覆盖 `d2.id`
- 空输入 + `GROUP BY` 不再产出伪行;GROUP BY 结果应用 LIMIT/OFFSET
- 窗口函数块重写:`ROW_NUMBER` 与 `COUNT/SUM/AVG/MIN/MAX ... OVER (PARTITION BY … ORDER BY …)` 逐行返回;`RANK()` 结果与 MySQL 一致(用例期望值待校正),`DENSE_RANK` 返回行号而非稠密名次(已知缺陷),`LAG/LEAD/COUNT(*) OVER/命名窗口` 未实现
- LIMIT/OFFSET 越界统一返回空集(UNION / 融合聚合 / 聚合 / 主路径 4 处)

**E. 元数据与系统变量**

- `information_schema.STATISTICS` / `KEY_COLUMN_USAGE` 从 `primary_key_cols` 生成主键索引与约束行
- `SHOW COLUMNS` 的 `Default` 列取列默认值字面量
- `SELECT @@autocommit` 返回 `1`/`0`(`SHOW VARIABLES` 仍 `ON`/`OFF`)
- `lower_case_table_names` 统一为 `1`
- `Session::setIsolation` 不再做级别提升映射,按请求级别原样报告;会话默认级别为 `REPEATABLE READ`

**F. 测试与遗留**

- SLT **1184 / 1092 / 92 / 0**、协议 **42/42**、持久化 **15/0/0**
- 清除 52 个已通过的 `gap` 标记;4 处测试期望值按 MySQL 语义修正(NULL 自连接、`LIKE '%a%'` 大小写、RIGHT JOIN 行数、SHOW COLUMNS 列对齐)
- 持久化 2 项 GAP 已清零:补齐 `SIGINT`/`SIGTERM` + `SHUTDOWN` 优雅关闭 → checkpoint → 截断 WAL;同时修复 checkpoint 写快照时 Catalog 递归加锁、WAL 截断后 0 字节 header、EOF 之后追加记录不回放三处缺陷

### 本轮交付(v2.0.4 融合聚合 + UNION ALL)

**1. 汇聚聚合快路径**

`SUM(salary)`(salaries 280 万行)此前走通用聚合:三次全表行拷贝(`left_rows` / `candidate_rows` / `buckets`),扫描阶段逐行 `build_ctx` + `eval(column)`,聚合阶段再逐行 `eval`,耗时 **2.969s**;`COUNT(*) WHERE salary>100000` 同样逐行 `eval` 谓词并物化 9.4 万行,耗时 **0.774s**。

新路径在扫描前解析列偏移与谓词操作数,`for_each_visible` 回调内直接过滤 + 累加,零中间行。触发条件:单表、无 JOIN/GROUP BY/HAVING/DISTINCT/ORDER BY/LIMIT、投影全为简单聚合(`COUNT(*)` / `COUNT|SUM|AVG|MIN|MAX(列)`)、WHERE 可拆为 AND 连接的「列 op 字面量」;存在可走 PK 前缀探测的等值条件时仍优先索引探测。整数比较在 2^53 无损区间直比 `int64`,省掉每操作数两次 `try_to_double`。

| 查询(Release,employees 392 万行,5 次均值) | v2.0.3 | v2.0.4 | MySQL 3306 |
|---|---|---|---|
| `SUM(salary) FROM salaries` | 2.969s | **0.352s** | 0.33s |
| `COUNT(*) FROM salaries WHERE salary>100000` | 0.774s | **0.248s** | 0.28s |
| `COUNT+SUM+AVG+MIN+MAX` 五合一 | — | 0.422s | — |
| `SUM(salary) WHERE emp_no=10001` | 0.000s | 0.000s(PK 探测未变) | — |

**2. UNION / UNION ALL**

AST 已有 `set_op` / `set_right`,此前 parser/executor 均未接入:

- `src/parser/parser.cpp`:`UNION [ALL|DISTINCT] SELECT ...`,右分支递归,支持链式
- `src/executor/executor.{h,cpp}`:`exec_select` 拆为 UNION 包装 + `exec_select_body`;UNION ALL 拼接、UNION 去重(行 `to_string` 键)、尾部 ORDER BY/LIMIT 作用于整体、列数不齐补 NULL
- 顺带支持 `ORDER BY 1` 位置序号(此前缺失)

三端对比(3 次均值):`count(employees) ∪ALL count(salaries)` MySQL 0.03s / tcdb 0.00s* / ST 0.26s;`salaries>100000 ∪ALL <=100000` 0.38s / 0.00s* / 0.58s;GROUP BY 结果 UNION ALL 0.10s / 0.00s* / 0.64s(``*`` tcdb 疑似 COUNT 缓存)。结果一致性:8 条三端一致,带 ORDER BY+LIMIT 的一条 MySQL 与 SQLTiny 一致、tcdb 报 16384 行上限。

**回归**:`ctest` 9/10(`smoke_m25_e2e` 硬编码 3306、当前 3306 为需密码 MySQL,环境性 auth 失败);指向 3307 的 `test_full_flow` 48/48、`test_vs_sqlite` 全场景一致、`test_stmt_prepare` 7/7。

### 本轮交付(v2.0.3 主键点查索引探测)

v2.0.2 之前 `SELECT ... WHERE emp_no = 10001` 与普通查询同路:主表全表扫描 + 逐行 `eval`,employees(30 万行)**74ms**、salaries(280 万行)**678ms**。本版本把「主表主键首列等值」接到已有的 PK 前缀探测能力上:

- **执行器走索引探测**:`exec_select` 在取基表行之前,把 `s.where` 按 `AND` 拆成连接词,寻找 `主表PK首列 = 字面量` 形式的等值条件(列可左右互换),命中则以 `tbl->for_each_by_pk_prefix(lit, ...)` 只遍历键匹配的版本链;`pushdown_conjuncts` 仍在回调内逐行复核,保证与全扫路径结果一致。**只接受同类型家族字面量**(`is_integer` / `is_numeric` / `is_string` / `is_datetime` 对应),跨类型一律不下推 —— 索引按键存储类型严格比较,而 WHERE 求值允许隐式转换,跨类型下推会漏行;`IS NULL` 与非字面量(占位符、表达式)不参与。
- **逻辑删除回退保护(正确性关键)**:MVCC 删除会**立即** `pk_index_->remove()`,行本体留在 `row_map_` 直到 checkpoint。这种状态下「按索引扫描」不再是「全表扫描」的超集 —— 旧快照可见的行、以及未提交删除的行,都已不在索引里。为此 `Table` 新增 `dead_rows_` 计数(`delete_by_pk` 首次置删自增、`erase_row` 物理清除 / `restore_row` 回滚恢复自减)与 `has_dead_rows()`(shared_lock 下只读一个整数),执行器在 `has_dead_rows()` 为真时**自动回退全表扫描**,四个隔离级别下可见性语义不变。

**实测(Release,employees 392 万行,5 次均值)**

| 查询 | v2.0.2 | v2.0.3 | 说明 |
|---|---|---|---|
| `COUNT(*) FROM employees WHERE emp_no=10001` | 74 ms | **0.13 ms** | PK 等值 → 索引探测 |
| `COUNT(*) FROM salaries WHERE emp_no=10001` | 678 ms | **0.17 ms** | PK 等值 → 索引探测 |
| `COUNT(*) FROM employees WHERE emp_no>499000` | 92 ms | 92 ms | 范围谓词,仍全扫 |
| `COUNT(*) FROM employees WHERE first_name='Georgi'` | 87 ms | 87 ms | 无索引列,仍全扫 |

**边界验证**(裸 PyMySQL):不存在的主键 → 0;`salaries` 单员工 → 17;范围 → 999;`first_name='Georgi'` → 253;提交删除后该行点查 → 0;`DELETE` + `ROLLBACK` 后该行重新可见、总行数不变。

**文档/图形**:`architecture.svg` 同步刷新 —— 移除 HSQLDB 字样、PK 索引标注 AVL → B+ 树、执行器标注 `PK prefix probe · Hash Join · aggregate pushdown`、版本徽标改为 `v2.0.3`、回归数字与 Known gaps 更新为当前基线。

### 本轮交付(v2.0.2 聚合下推覆盖面扩展)

v2.0.1 的聚合下推只覆盖「最后一个 JOIN 为 PK 前缀探测 + WHERE 全左表列」的窄场景,`高薪员工数`(混合谓词)与 `2 表 JOIN 计行`(哈希路径)仍物化中间行。本版本把两者纳入:

- **WHERE 按表侧拆分**:`s.where` 按 `AND` 递归拆成连接词,用 `cols_in_range` 分类——`[0, right_offset)` 为左表侧(主表扫描时预过滤),`[right_offset, total_cols)` 为右表侧(探测回调内过滤);任一词跨表引用则整体回退通用路径。混合谓词 `e.hire_date < '1990-01-01' AND s.salary > 90000` 因此可下推。
- **哈希等值 JOIN 也下推**:判据由 `lastp.idx_probe` 放宽为 `lastp.idx_probe || lastp.hash_join`(仍需 `pure_equi && Inner`);融合分支在 `!idx_probe` 时本地物化右表建桶(与通用路径同构),探测时直接累加,不再拼装 `left × matches` 行。退化为索引探测与哈希探测两条探测分支,共用 `process_match` 累加逻辑。
- **加固**:`cols_in_range` 遇 `Subquery` / `Case` / `Placeholder` / `Star` 直接判否——这类节点的引用面无法静态判定,避免把引用右表的子查询误当左表谓词在左行上下求值。

**实测(Release,employees 392 万行)**

| 查询 | v2.0.1 | v2.0.2 | MySQL |
|---|---|---|---|
| 3 表 JOIN + GROUP BY + AVG | 2.06s | 2.05s | 1.60s |
| emp⋈sal WHERE emp_no=10001 | 0.105s | 0.081s | <0.01s |
| 单员工行数 | 0.094s | 0.086s | <0.01s |
| 高薪员工数(下推+混合谓词) | 2.76s | **0.94s** | 0.37s |
| 2 表 JOIN 计行(哈希路径) | 0.37s | **0.13s** | 0.05s |

**回归**:`ctest` 10/10;`smoke_m6_hsqldb_matrix` 73/73;`test_full_flow` 48/48;`test_vs_sqlite` 全场景一致;`test_stmt_prepare` 7/7;`compare_employees` 14/14 MATCH(逐行数值一致,仅既有 AVG 小数位 / SUM 类型表现差异)。

### 本轮交付(v2.0.1 3 表 JOIN 执行性能优化)

v2.0.0(B+ 树)基线上,employees 数据集 3 表 JOIN 求各部门均薪 **18.25s** vs MySQL **1.57s**。逐阶段分解(2 表 0.30s → 加上 salaries 后 6.44s → GROUP BY 7.51s → AVG 8.28s)定位到三处执行器开销:

1. **`ExecContext` 每行深拷贝**(决定性):`build_ctx` 每行 `current_row = 整行拷贝` + `col_index = 整张 map 深拷贝`(~40 节点);GROUP BY 分桶 + 聚合两遍合计约 1100 万次调用。改为在 `ExecContext` 增加 `const storage::Row* row_ref` / `const std::map<std::string,int>* idx_ref`,`eval` 的 Column 求值优先走引用路径,`build_ctx` 只做指针赋值(O(1))。**18.25s → 7.24s**。
2. **`idx_probe` 未真正零物化**:原实现在进入每个 join 前无条件 `jtbl->for_each_visible(...)` 拷贝右表全部行,且 `plan.hash_join` 为真时逐行 `join_hash_key`(`to_string`)建桶——salaries 的 280 万行被完整拷贝并哈希一遍后弃用。改为仅 `!plan.idx_probe` 时物化/建桶。**7.24s → 6.27s**,`emp⋈sal WHERE emp_no=10001` **2.10s → 0.08s**。
3. **中间行物化 → 聚合下推**(结构性):JOIN 为 314 万匹配行各构造 10 列 `flat` 行(6 次字符串堆分配/行)。新增「聚合下推到 PK 前缀探测 JOIN」:判据为最后一个 JOIN 为 `idx_probe && pure_equi && INNER`、`has_agg && !having && !distinct`、分组键与 WHERE 只引用左表列、聚合参数只引用右表列、非聚合 SELECT 列只引用左表列(均通过 `cols_in_range` 按行偏移区间校验,`@@变量` / `SELECT *` / 嵌套聚合 / 歧义列一律回退)。满足时逐左行算分组键、逐右行更新累加器(COUNT/SUM/AVG/MIN/MAX),每个分组仅保留 1 条代表性整行供 ORDER BY 与非聚合列求值。**6.27s → 1.97s**。
4. **单组聚合也下推**:放宽为 `s.group_by` 可空(空输入仍产出一行,COUNT=0 / SUM=NULL,与 MySQL 一致),`SELECT COUNT(*)` 类查询 **4.49s → 1.60s**。

**附带修正**

- 聚合下推严格限定 `INNER JOIN`:LEFT JOIN 无匹配需补 NULL 行,由 `smoke_m6_hsqldb_matrix`「无匹配时 LEFT JOIN 保留左表」用例暴露并修复(修复前 72/73)。
- 聚合结果 `ORDER BY`:原实现对比较双方都用 `buckets[0].rows.front()` 求值(恒等,等于不排序);下推路径改为按各分组代表性整行求值,`ORDER BY d.dept_name` 正确输出字母序。

**实测(Release,employees 392 万行)**

| 查询 | v2.0.0 | v2.0.1 | MySQL |
|---|---|---|---|
| 3 表 JOIN + GROUP BY + AVG | 18.25s | **1.97s** | 1.57s |
| 3 表 JOIN + COUNT(*) | 6.44s | **1.60s** | — |
| 3 表 + SUM/AVG/MIN/MAX/COUNT | — | 3.28s | — |
| emp⋈sal WHERE emp_no=10001 | 2.10s | **0.08s** | — |

**回归**:`ctest` 10/10;`smoke_m6_hsqldb_matrix` 73/73;`test_full_flow` 48/48;`test_vs_sqlite` 全场景一致;`test_stmt_prepare` 7/7;`compare_employees` 14/14 MATCH。

### 本轮交付(v2.0.0 主键索引 AVL → B+ 树)

主键索引从自研 AVL 树整体替换为 B+ 树。内部节点只存路由分隔键,全部 (Key, row_id) 落在叶子;叶子单向链表相连,范围/前缀/全量扫描沿链顺序推进,缓存局部性显著优于 AVL 递归中序遍历。

- **`IndexBPlusTree`**(`storage/index_bptree.h/.cpp`):内部节点 ≤64 子 / 叶子 ≤128 条;插入自底向上迭代分裂,叶子分裂上推右叶首键、内部分裂继续上推中位分隔键;`range_scan`/`prefix_scan`/`for_each` 沿叶子链推进,`prefix_scan` 依赖键全局有序性"越过前缀即剪枝";`Key` 迁至 `storage/index_key.h` 供两种索引共用;`IndexAvl` 保留用于对照测试。
- **惰性删除**:只从叶子移除条目,不收缩树高、不回收空叶——分隔键允许"偏高"(指向已删键),路由/扫描语义不变;非唯一索引的等键条目跨叶分裂场景由 `find_all`/`remove` 的左右兄弟叶回查覆盖。
- **`tests/bench_index`**(不进 ctest):AVL/B+ 树 A/B 微基准 —— 顺序/乱序插入(模拟 WAL 回放/最坏路径)、随机点查、复合键前缀扫描(JOIN 探测路径)、全量遍历。
- **`smoke_m1` 新增 B+ 树测试块**:乱序插入、点查、unique 重复拒绝、删除、范围扫描、复合键前缀扫描(cb 提前停)、非唯一等键 find_all/按 row_id 删除、全量扫描有序性、不变量校验(children==seps+1、分隔键下界、叶子链全局有序且计数一致)。

**性能实测(Release 构建;v1.2.5 文档的 23.8s 为 Debug 数字,自 v2.0.0 起性能基线统一 Release)**:

| 指标 | AVL | B+ 树 | 变化 |
|---|---|---|---|
| 微基准:点查 ×20 万 | 351.6ms | 285.4ms | +19% |
| 微基准:复合键前缀扫描 ×20 万 | 449.7ms | 340.4ms | +24% |
| 微基准:全量遍历 200 万 | 67.9ms | 8.9ms | 7.6x |
| 微基准:乱序插入 200 万 | 3787.8ms | 3704.3ms | 持平 |
| 端到端:3 表 JOIN(Release) | 18.99s | 18.42s | +3.0% |
| 端到端:emp⋈sal emp_no=10001 | 2.17s | 2.10s | +3.2% |
| 端到端:混合谓词 | 7.20s | 6.90s | +4.2% |
| WAL 回放 548MB | ~10s | ~11s | 持平 |

端到端仅 +3~4%:JOIN 耗时大头在执行器逐行求值/MVCC 过滤(与索引结构无关);索引级收益(前缀扫描 +24%、遍历 7.6x)在扫描密集型负载中放大。WAL 回放持平:回放 783 万条记录的耗时主要在 WAL 解码与行物化,且本数据集插入路径接近顺序、AVL 顺序插入本就不慢。

### 本轮交付(v1.2.5 大表 JOIN 主键索引利用)

employees 标准数据集(392 万行)实测:3 表 JOIN 求均薪 77s vs MySQL 6.5s,`employees⋈salaries WHERE emp_no=10001` 28s——EXPLAIN 显示 salaries 侧 join 列为复合 PK 首列时仍走 Hash Join 全量物化(280 万行)逐行 string 哈希。逐条修复:

- **AVL PK 前缀扫描**(`index_avl.h/.cpp`):新增 `prefix_scan(key, cb)` + `do_prefix_scan` —— 节点 key 截断到前缀长度逐列比较决定剪枝方向;截断相等且键长 ≥ 前缀长则命中回调,左右子树都递归。复杂度 O(log n + 命中数)。
- **表层前缀遍历**(`table.h/.cpp`):`for_each_by_pk_prefix(first_pk_value, querySCN, cb, readerTxn)` —— PK 前缀扫描后做 MVCC `row_visible` 过滤再回调;NULL 首列不命中。
- **执行器索引探测路径**(`executor.cpp`):`extract_equi` 识别 join 列 = 右表复合 PK 首列(`right_pk_first`);规划循环 `idx_probe = equi.valid && right_pk_first && left_rows <= right_rows`;执行时逐左行 `for_each_by_pk_prefix` 探测——纯等值直接拼 flat row,复合 ON 再 `eval` 兜底;`right_col` 越界与 NULL key 保护。右表零物化。EXPLAIN Extra 显示 `Using index probe; cost … vs hash join …`(兼容保留 "hash join" 字样供既有断言)。
- **WHERE 谓词下推**:`split_and` 拆 AND 连接词,`primary_only` 递归判定(限定名精确命中 base_col_index、裸名查 `ambiguous_cols`,歧义/未知名/Subquery/Case/Star 保守不下推),主表扫描阶段逐行预过滤。
- **SUM(DECIMAL) 列类型一致性修复**:`infer_type` 此前对 SUM(DECIMAL) 声明 LONGLONG,但聚合值经 double 路径返回 `'300.5'` → PyMySQL 按 LONGLONG 调 `int()` 抛 ValueError → 行读取中断、socket 残留包 → 下一条命令 `Packet sequence number wrong - got 6 expected 1`。现 DECIMAL/浮点输入声明 DOUBLE,与实际返回值一致;嵌套聚合(COALESCE(SUM(x),0))路径同语义。`tests/smoke_m3.cpp` 聚合断言改为类型无关 `num_eq`(解耦 SUM int/double 内部表示)。
- **排查工具**:`tests/debug/_repro_pkt.py`(裸 socket 逐包 dump,验证 server 帧连续性/seq 重置/无残留)、`tests/debug/_trace_seq.py`(PyMySQL `_read_packet`/`_execute_command` 插桩,文件日志)、`tests/debug/_bench_join.py`(employees JOIN 基准 + EXPLAIN)。

### 本轮交付(v1.2.2 稳定性与兼容性修复)

以 employees 标准数据集(test_db-master,392 万行 / 6 表)与真实 MySQL 同库对比验证驱动,逐条修复:

- **握手残留字节(Navicat 2013)**:`parse_handshake_response` 未消费 `CLIENT_CONNECT_ATTRS` 连接属性段——带完整能力位的 GUI 客户端握手后,残留属性字节被当成下一条命令读包,连接错位报 `2013 Lost connection during query`。现按 lenenc 长度跳过属性段;`mysql_proto.h` 新增 `CLIENT_CONNECT_ATTRS` 常量;`tests/debug/_verify_fixes.py` 用裸 socket 模拟 Navicat 握手(caps=0x00ffa684 + 属性串)回归,握手 OK 且后续 COM_QUERY 正常。
- **DATE 聚合**:MIN/MAX 经 `try_to_double` 把 `'2019-12-31'` 截成 2019 只返回年份。现改用 `Value::compare` 原生比较并保留原值类型;`infer_type` 对 MIN/MAX 返回参数原类型(DATE/DATETIME/字符串)。`MIN/MAX(hire_date)` 返回完整日期。
- **整型 SUM 以字符串回包**:聚合值统一走 double,列类型推断与值类型错位。现整型列用 int64 累加返回 `make_int`,SUM(int) 类型推断为 BigInt;分组桶 / 嵌套聚合(COALESCE(SUM(x),0)) / HAVING 内联三条路径同步修正;COALESCE/IFNULL 取第一个非 NULL 参数的类型。
- **Windows 版本资源**:新增 `src/version.rc.in`(经 `configure_file(@ONLY)` 注入 CMake 项目版本)挂接到 `sqltiny_server` 目标,exe 属性页显示产品名 / 版本 / 版权;版本号只维护 CMakeLists 一处。

### employees 数据集对比验证

- `tests/data/load_employees.py`:把 `test_db-master` 的 employees 数据集灌入 SQLTiny(默认 3307,WAL 模式),多行批插(~3.5 万行/s,392 万行约 50s);避开外键、ENUM→CHAR(1),保留复合主键与 DATE。
- `tests/benchmark/compare_employees.py`:同一组 14 条查询(过滤 / GROUP BY / 2 表&3 表 JOIN / SUM·AVG·COUNT / 定点)在 MySQL 与 SQLTiny 双端执行并规范化比对,修复后 **14/14 MATCH**。
- 行数精确一致:employees 300024 / departments 9 / dept_manager 24 / dept_emp 331603 / titles 443308 / salaries 2844047;548MB WAL 重放 783 万条记录完整恢复 392 万行。
- **已知差距(待办)**:PK 前缀索引探测(v1.2.5)、B+ 树主键索引(v2.0.0)、3 表 JOIN 执行优化 + 聚合下推(v2.0.1)、下推覆盖面扩展(v2.0.2)、主键点查索引探测(v2.0.3)、融合聚合 + UNION ALL(v2.0.4)、复杂查询正确性 + 语法补全(v2.0.5)、MySQL 语义兼容(v2.0.6)、P1-a 日期时间模块(v2.0.7)均已落地,Release 基线下 3 表 JOIN 2.05s vs MySQL 1.60s(约 **1.28x**),高薪员工数 / 2 表哈希计行约 2.5x;仍走物化路径的场景:带 HAVING / 嵌套聚合 / LEFT JOIN / DISTINCT、聚合参数跨表引用、WHERE 跨表连接词;融合聚合快路径未覆盖 WHERE 含 OR / 函数 / 子查询、GROUP BY 分支,UNION 去重按 `to_string` 键(未做表达式级等价);AVG 仍以 Decimal 字符串回包;结果集 >16MB 未分包;**v2.0.5 遗留待优化**:P3(HAVING 使 3 表 JOIN 聚合下推失效,19.77s vs 4.10s)、P4(`DISTINCT` 全量物化去重,2.79s vs 0.29s)、UNION 多分支 COUNT 仍未达 MySQL 水平(0.71s vs 0.09s);超长事务专项压测未做。

### 验证基线

| 测试 | 结果 |
|---|---|
| `ctest -C Debug` | **13/13**(含 `smoke_m6_isolation`、`smoke_m6_hsqldb_matrix`、`smoke_m6_session_db`、`dbx_probe` 与 v2.0.5 新增的 `smoke_sql_keywords` / `smoke_sql_syntax`、命令行导入/导出 `smoke_cli_io`);`smoke_m25_e2e` 端口统一为 3307,启动 server 后全通过 |
| `sqllogictest`(外部套件 `SQLTinySLT`) | **1184 项 · PASS 1132 / GAP 52 / FAIL 0**(首轮 971/146/67;v2.0.6 修复后 1092/92/0;v2.0.7 P1-a 消 14 项→78;v2.0.8 P1-b 再消 26 项→52) |
| `test_protocol.py` | **42 / 42 PASS** — 握手、COM_QUERY/PING/INIT_DB、COM_STMT_*,多连接隔离 |
| `test_persistence.py` | **13 PASS / 2 GAP / 0 FAIL** — WAL 回放、强杀恢复、未提交不恢复、DROP TABLE 持久化;缺优雅关闭 checkpoint |
| `smoke_sql_keywords` | 649 PASS / 0 FAIL — 180 个常用关键词大小写不敏感、运算符、字面量、注释、行列号、词法错误、Parser 分发 |
| `smoke_sql_syntax` | 160 PASS / 0 FAIL / 15 GAP — 常用 SQL 语法端到端矩阵(未实现语法记 GAP) |
| `smoke_m6_isolation` | 493/493 PASS — 四级别、脏读、快照、冲突、autocommit、SAVEPOINT、DML 回滚;系统变量按 MySQL 口径(`@@autocommit` 返回 1/0、隔离级别原样返回不提升),已用真实 MySQL 8.4(3306) 逐项对比(`tests/debug/compare_session_vars.py`) |
| `smoke_m6_hsqldb_matrix` | 73 PASS / 0 FAIL / 0 GAP — HSQLDB 语义矩阵 + JOIN 优化/EXPLAIN 回归(§8.4 #1–#4 已修复) |
| `smoke_m6_session_db` | PASS — 多库会话级隔离(每个连接的 `USE`/握手 db 互不干扰,验证 §8.4 #6/#7 修复) |
| `dbx_probe` | 58/58 PASS — 进程内 `Session::execute` 跑 DBX/dbExpress 连接/元数据/事务阶段的典型 SQL(含 SHOW/information_schema) |
| `test_stmt_prepare.py` | 7/7 PASS — 预编译语句 binary protocol 全流程(PREPARE/EXECUTE/CLOSE、NULL 参数、转义、语法错误) |
| `test_txn_vs_sqlite.py` | 70/70 与 SQLite 一致 + 2 项预期差异(RC 事务内语义、RU 提升 vs SQLite 脏读) |
| `test_full_flow.py` / `test_vs_sqlite.py` | 48 PASS / 0 FAIL / 0 GAP / 全场景一致 |
| `test_wal_recovery.py` / `test_concurrency.py` | 重启恢复全通过 / 5 线程 15s 无 crash |
| `tests/benchmark/test_perf_vs_sqlite.py` | Release 版 server 单进程 / 多进程(`multiprocessing.Pool`)两种模式对比 SQLite,结果保存 `benchmark/perf_sqlite_*`(带时间戳 JSON + CSV、`perf_sqlite_history.csv` 追加、`perf_sqlite_latest.json`) |
| `tests/benchmark/compare_employees.py` | employees 数据集 MySQL(3306) vs SQLTiny(3307) 39 条查询准确性+耗时测评,结果保存 `benchmark/employees_*`(`employees_latest.json` 含一级类别/二级查询对比详情 + TOP10 瓶颈、`employees_query_detail.csv` 表格版、`employees_history.csv` 一级历史),当前 **39/39 MATCH** |
| `tests/data/load_employees.py` | 从 test_db-master dump 灌入 employees 6 表约 392 万行到 SQLTiny |
| `stress_stability.py` | 稳定性压测:复杂查询计时 / 并发 soak(ops·错误率·p50/p95/p99) / 事务冲突不变量 / `kill -9` 崩溃恢复(WAL replay 原子性) / RSS 内存稳定;`--self-server` 自动拉起实例 |
| `load_employees.py` / `compare_employees.py` | employees 标准数据集(392 万行 / 6 表)灌入 + 与真实 MySQL 同库 14 条查询对比 —— 14/14 MATCH;WAL 548MB 重放完整恢复 |
| `_verify_fixes.py` | 裸 socket 模拟 Navicat 握手(CONNECT_ATTRS)+ DATE 聚合 / SUM / COALESCE 类型断言(回归防护) |
| `test_sqlalchemy.py` | 33 PASS / 0 FAIL / 0 GAP(原 #5–#8 与 DESCRIBE/SHOW FULL TABLES 反射限制均已修复) |
| **实测客户端** | DBX/dbExpress(Delphi)连接 + 打开表 + 浏览索引/列正常;类 Navicat 图形工具浏览表/索引/列正常;PyMySQL / mysql CLI 正常 |

### 版本标识(v2.2.3)

| 位置 | 内容 |
|---|---|
| `CMakeLists.txt` | `project(SQLTiny VERSION 2.2.3 LANGUAGES CXX)` |
| `vcpkg.json` | `"version-string": "2.2.3"` |
| `src/main.cpp` | 启动横幅 `SQLTiny v2.2.3 — MySQL wire protocol server` |
| `src/version.rc.in` | exe Windows 版本资源(经 configure_file 注入 CMake 版本),右键属性页显示产品名 / 版本 / 版权 |
| `README.md` / `CODEPLAN.md` | 本版本变更清单与验证基线 |
| 协议层 `@@version` / `VERSION()` | 仍为 `5.7.0-SQLTiny` —— MySQL 客户端兼容用伪装版本,**不是**产品版本,不随产品版本改动 |

> v1.1.0 统一了此前分散的版本号(启动横幅 `v1.0`、manifest `0.1.0`、CMake 未声明),交付 Prepared Statement、information_schema、SHOW 扩展与 EOF 帧兼容。**v1.1.2** 在此基础上交付 DBeaver / Database Client 实测兼容修复(DDL 选项跳过、SHOW TABLES WHERE/LIKE 过滤、`REFERENTIAL_CONSTRAINTS` 虚拟表、系统变量补齐、双引号与裸关键字别名、`INSTR/LOCATE/POSITION` 函数)。**v1.2.0** 交付 JOIN 性能优化(Hash Join + 预建列索引 + 成本估算 + `EXPLAIN`),INNER/LEFT JOIN 约 77x / 62x 提速,详见 README「v1.2.0 更新内容」。**v1.2.5** 交付大表 JOIN 主键索引利用(PK 前缀索引探测 + WHERE 谓词下推)与 SUM(DECIMAL) 列类型一致性修复,employees 3 表 JOIN 77s→23.8s、单员工⋈薪酬 28s→2.3s,详见 README「v1.2.5 更新内容」。**v2.0.0** 主键索引整体升级 B+ 树,索引级点查 +19% / 前缀扫描 +24% / 全量遍历 7.6x,端到端 employees JOIN 18.99s→18.42s(Release 基线),详见 README「v2.0.0 更新内容」。**v2.0.1** 优化 3 表 JOIN 执行性能(ExecContext 零拷贝、idx_probe 跳过右表物化、聚合下推到 PK 前缀探测),employees 3 表 JOIN 18.25s→1.97s(9.3x),与 MySQL 差距 11.7x→1.25x,详见 README「v2.0.1 更新内容」。**v2.0.2** 扩展聚合下推覆盖面(WHERE 按表侧拆分、哈希等值 JOIN 也下推、子查询/CASE 不下推加固),高薪员工数 2.76s→0.94s、2 表哈希计行 0.37s→0.13s,详见 README「v2.0.2 更新内容」。**v2.0.3** 让主键点查走索引前缀探测(`WHERE PK首列=同族字面量`),并以 `dead_rows_` / `has_dead_rows()` 在存在逻辑删除历史时回退全表扫描以保 MVCC 可见性,employees 点查 74ms→0.13ms、salaries 678ms→0.17ms,详见 README「v2.0.3 更新内容」。**v2.0.4** 增加单表聚合单遍扫描快路径(`SUM(salary)` 2.97s→0.35s、`WHERE salary>100000` 计行 0.774s→0.248s)并实现 UNION / UNION ALL(去重、链式、尾部 ORDER BY/LIMIT)与 `ORDER BY 1` 位置序号,详见 README「v2.0.4 更新内容」。**v2.0.5** 补齐复杂查询能力与正确性:WITH/递归 CTE、标量子查询、EXISTS、FROM 派生表、COUNT(DISTINCT)、BETWEEN、`ROW_NUMBER() OVER`、YEAR/MONTH/DAY 等,GROUP BY 别名与内嵌聚合修复;性能上 GROUP BY 高基数分桶 O(n²)→O(1)、相关子查询外层列等值走 PK 前缀探测、递归 CTE 改增量工作集,`compare_employees.py` 扩至 39 条并达 **39/39 MATCH**,详见 README「v2.0.5 更新内容」。**v2.0.6** 由外部 sqllogictest 套件驱动,把 SQL 语义与报错对齐 MySQL:NULL 排序/三值逻辑、LIKE 大小写、BIGINT 与表达式列类型、SQL 校验(1048/1054/1136/1366/1406/1242/1241/1111/1055/1222/3593)、RIGHT JOIN/逗号连接、窗口聚合、information_schema 主键元数据、字符串与数值函数;SLT **1184 项 PASS 1092 / GAP 92 / FAIL 0**,协议 **42/42**,详见 README「v2.0.6 更新内容」。**v2.0.7** 完成 P1-a 日期时间模块:`INTERVAL` 语法、14 个日期/时间函数与无括号 `CURRENT_*`、DATETIME 减法,SLT **PASS 1106 / GAP 78 / FAIL 0**,协议 42/42、持久化 15/15、`compare_employees` 39/39,详见 README「v2.0.7 更新内容」。**v2.2.0** 新增命令行导入 / dump 导出:`sqltiny_server --import FILE` 可直接灌入 SQL/mysqldump 脚本(注释、`DELIMITER`、`LOCK TABLES` 跳过、错误继续策略),`--dump FILE` 导出可回灌的 MySQL 风格建库/建表/多行 `INSERT`(库表过滤、只结构/只数据、批大小、`-` 写 stdout),详见 README「v2.2.0 更新内容」。**v2.2.1** 新增 MSVC AddressSanitizer 构建(`windows-asan` preset,无需 Clang/LLVM),配套 ASan 运行时 DLL 自动部署与 fmt 注解冲突规避;ASan 下 SLT 1184/PASS 1133/GAP 51/FAIL 0、协议 42/42、持久化 15/15,并支撑同级 `SQLTinySLT` 的 Hypothesis 状态化模糊测试,详见 README「v2.2.1 更新内容」。**v2.2.2** 交付 benchmark TOP5 查询优化(CASE 流式分组、EXISTS 首行即停、融合 JOIN 裸列直取、DISTINCT 提前去重、PK 上界范围扫描),复杂类总耗时 18.90s→10.92s(-42%),详见 README「v2.2.2 更新内容」。**v2.2.3** 分两轮按 `employees_query_detail.csv` 定向优化:第一轮 priority 前三项(EXISTS 半连接物化、小维度表过滤键反传、日期函数流式分组),第二轮 delta 第 3/4/6 项(EXISTS 整数键、DISTINCT 投影期去重、COUNT(DISTINCT) 裸列偏移);39 条总差距 10.55s→5.56s(-47%),两轮均 39/39 MATCH,详见 README「v2.2.3 更新内容」。

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

> 立项时的设计稿路径见 §2 落地偏差说明。以下为仓库根目录当前真实结构。

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
- [x] `COM_STMT_PREPARE` / `COM_STMT_EXECUTE` / `COM_STMT_CLOSE` / `COM_STMT_RESET` — 二进制协议(**✅ v1.1.0 已落地,见 §0 本轮交付**)
- [x] `INFORMATION_SCHEMA` 虚拟表(**✅ v1.1.0 已落地**:SCHEMATA/TABLES/COLUMNS 等 10 张,每次查询物化)
- [ ] `caching_sha2_password` 认证(→ M6 P3 剩余)
- [ ] `CLIENT_SSL` 握手(→ M6 P3 剩余)

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
  - `test_concurrency.py` — 5 线程 15s 并发读写(M5 时为全局 mutex,M6 起由行锁 + MVCC 快照保护,无 crash)

**验收**:server 重启后 `SELECT COUNT(*)` 与 shutdown 前一致;并发测试 15s 无 crash;SQLAlchemy ORM `s.get(User, 1)` 可用。

---

### M6 · 行锁 + 事务 + JOIN 优化 + 存储过程(按优先级)

> **进度(2026-11-12,v1.1.0)**:P0 ✅ 已完成 · P1 事务语义 ✅ 已完成,边缘项 ⬜ 待办 · **P3 客户端兼容 ✅ 完成(v1.1.0:Prepared Statement + INFORMATION_SCHEMA + SHOW/CASE 表达式 + EOF 帧兼容 + `--query-log`)**,剩余认证(SSL / caching_sha2_password)· P2 / P4 ⬜ 未启动。详见 §0 当前状态。

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

**验证**:`smoke_m6_isolation` 451/451 PASS、`test_txn_vs_sqlite.py` 70/70 与 SQLite 一致,`ctest` 10/10(v1.1.0 基线)。

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
| ✅ | **P1-09** DESCRIBE / SHOW FULL TABLES | 9e752ba 已补齐 server 端反射(§8.4 #5–#8 一并修复),`test_sqlalchemy.py` 33 PASS / 0 FAIL / 0 GAP | `test_sqlalchemy.py` |
| ✅ | **P1-10** 重启不复活未提交数据 | 回滚/提交后重启,SQLTiny 与 SQLite 状态一致;WAL 事务缓冲 + 快照只写已提交可见行 | `test_txn_vs_sqlite.py` WAL 段、`smoke_m5_wal` |

> 新增 `tests/smoke_m6_isolation.cpp`(451 项进程内断言)与 `tests/benchmark/test_txn_vs_sqlite.py`(70 项 SQLTiny/SQLite 对比 + WAL 重启),覆盖上表绝大多数 ✅ 项;前者已注册 `CTest`。v1.1.0 另新增 `tests/dbx_probe.cpp`(58 断言)、`tests/functional/test_stmt_prepare.py`(7 断言),见 §0 验证基线与 §8.2 C10。

---

#### P2 · JOIN 性能优化 ✅ (2026-11-12)
> **【已完成】** 旧实现是纯嵌套循环 O(N×M) 且每个行组合都重建 `col_index` map,
> 200 users × 800 orders 的 INNER JOIN / LEFT JOIN 各约 298ms / 297ms(单次查询)。
> 落地 Hash Join + 预建列索引后,**inner_join 3.9ms(约 77x)、left_join 4.8ms(约 62x)**,
> 单进程整体 wall 1822ms → 73ms;500×2000 大表 INNER JOIN 仅 9ms(线性扩展,不再 explode)。

- [x] **Hash Join**:从 ON 表达式递归提取「左表列 = 右表列」等值项(支持 `AND` 复合条件),
      右表按 key 分桶、左表 O(1) probe,O(N×M) → O(N+M)。数值 key 统一提升 double,
      NULL 不参与等值;纯等值只做 O(1) `sql_equals` 复核,复合 ON 仍完整 `eval` 兜底保证语义
- [x] **Index Nested Loop / PK 等值**:右表 join 列是单列主键时,`EXPLAIN` 标注
      `eq_ref` + `key=PRIMARY`。执行层统一走 Hash Join:内存表上 O(N+M) 优于 AVL 的 O(N log M),
      故不再单独实现索引探测分支(结果语义一致)
- [x] **预建列索引**:`base_col_index` 只构建一次,消除 O(N×M) 循环里重复拼接字符串 + map 插入;
      同时记录歧义裸列名,遇到歧义自动退化嵌套循环由 `eval` 决定语义
- [x] **JOIN 成本估算**:按表 `row_count()` 估算 nested loop = left×right / hash = left+right,
      多表 JOIN 逐轮传递估算基数,结果写入 `EXPLAIN` 的 `rows` / `Extra`
- [x] **`EXPLAIN`(最小版)**:`EXPLAIN SELECT ...` 只生成计划不扫描数据;8 列 MySQL 风格
      (`id/select_type/table/type/possible_keys/key/rows/Extra`),输出 join type
      (`eq_ref` / `ref` / `ALL`)、使用的 key、`rows` 与成本对比;支持无表 SELECT

**回归**:`smoke_m6_hsqldb_matrix` 的 C 组新增 11 项断言,覆盖等值/反向等值/复合 ON/
非等值退化/CROSS/LEFT 补 NULL/NULL key/多表连续 Hash Join/EXPLAIN type 与 cost,
共 **73 PASS / 0 FAIL / 0 GAP**;CTest Debug + Release 均 10/10,Python 端到端全绿。

#### P3 · Prepared Statement + 边缘兼容
- [x] `COM_STMT_PREPARE` / `COM_STMT_EXECUTE` / `COM_STMT_CLOSE` / `COM_STMT_RESET` / `COM_STMT_SEND_LONG_DATA` — 二进制协议(**✅ v1.1.0**:参数全类型解码拼回 SQL 重放;SELECT/SHOW 用 NULL 占位探测列元数据;binary resultset 返回)
- [x] `INFORMATION_SCHEMA.TABLES` / `COLUMNS` 等虚拟表(**✅ v1.1.0**:SCHEMATA / TABLES / COLUMNS / TABLE_CONSTRAINTS / KEY_COLUMN_USAGE / STATISTICS / TRIGGERS / ROUTINES / VIEWS / EVENTS,每查询物化)
- [ ] `caching_sha2_password` 认证(MySQL 8.0 默认插件)
- [ ] `CLIENT_SSL` 握手阶段
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
| 构建 | CMake 3.20+ + vcpkg(默认 `x64-windows-static`) | 跨平台,Windows/VS2022 友好;fmt 与 MSVC CRT 静态链接(`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded...`),产物不依赖 `fmt.dll`,可 `-DVCPKG_TARGET_TRIPLET=x64-windows` 覆盖 |
| 测试 | 自写 smoke(C++) + CTest + PyMySQL / SQLAlchemy / sqlite3 对比脚本 | 实际未引入 GoogleTest / GMock;10 个 CTest 目标 + 8 个 Python 端到端脚本(v1.1.0 新增 `dbx_probe` / `test_stmt_prepare.py`) |
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
| JOIN 非等值条件仍 O(N×M) | 低 | M6 P2 已用 Hash Join 覆盖等值 JOIN(77x);非等值 / CROSS / RIGHT 仍走嵌套循环,但列索引已预建,常数开销大幅下降。多表 JOIN 顺序仍按书写顺序,不做物理重排 |
| SQLAlchemy inspector 兼容 | 低 | `DESCRIBE` / `SHOW FULL TABLES` / `SHOW COLUMNS` 已在 9e752ba 落地(§8.4 #5–#8 一并修复),`get_table_names` / `get_columns` 正常使用 |
| AVL 大表性能不如 B+Tree | 低 | 当前继续用 AVL,替换计划列在 M6 P3,对外 `Index` 接口不变 |
| 磁盘持久化二进制兼容未来升级 | 中 | 文件头 magic + version 字段,启动时检查;不承诺跨大版本兼容 |

---

## 7. 里程碑交付节奏总览(实际)

```
  M1 ──► M2 ──► M2.5 ──► M3 ──► M4 ──► M5 ──► M6 P0 ──► M6 P1(事务) ──► M6 P3(v1.1.0)
 类型/   DDL/    协议    子查询   SHOW/    WAL     行锁 +    四隔离级别    预编译语句 /
 AVL/    DML/    接入    JOIN     多库     快照    事务管理  autocommit    information_schema
 Lexer   Catalog                                  savepoint  WAL 缓冲      SHOW/CASE/EOF
  ✅      ✅       ✅      ✅       ✅      ✅       ✅         ✅              ✅
 ─────────────────────────────────────────────────────────────────────────────────────►
  已完成:M1–M5、M6 P0 / P1 事务语义,以及 M6 P3 客户端兼容性(v1.1.0),均纳入 CTest / Python 回归
  待办:  M6 P1 边缘项 → P2 JOIN 优化 → 认证(SSL / caching_sha2_password)→ P4 远期
```

**下一步优先级(2026-11-12,v1.1.0 之后)**

1. **协议层收尾**:`DESCRIBE` / `SHOW FULL TABLES` 反射(9e752ba)、Prepared Statement(`COM_STMT_*`)、`INFORMATION_SCHEMA`、SHOW / CASE / EOF 帧兼容(v1.1.0)均已落地;**剩余**:`FOR UPDATE` / `LOCK IN SHARE MODE`、等待队列与死锁检测。
2. **M6 P2 已完成**(2026-11-12):Hash Join + 预建列索引 + 成本估算 + `EXPLAIN`,等值 JOIN 提速约 77x;**剩余**非等值/CROSS JOIN 物理优化与多表 JOIN 顺序重排。
3. **版本回收**:`merge_expired_transactions()` 目前只清理冲突检测队列,`versions_` 不做压缩,长事务/长生命周期进程内存会增长,需补表压缩。
4. **HSQLDB 语义扩展**:§8.4 的 8 项 GAP 已全部修复(2026-11-12);后续按 §8.3 待补清单扩展 `TestSelfNot.txt` 完整三值真值表(8 类型 × AND/OR/NOT/BETWEEN/IN/ALL/ANY)、解析器负向用例与 GROUP BY 合法性组合。
5. **M6 P3 剩余 / P4**:`caching_sha2_password` / SSL、B+Tree、复制与更大的 HSQLDB TestSelf* 覆盖。

6. **稳定性压测专项(后续)**:`stress_stability.py` 已覆盖并发 / soak / 崩溃恢复 / 内存稳定;**待补两项专项用例**:
   - **大结果集(>16MB 分包)**:`src/net/mysql_proto.cpp` 当前对 >16MB 结果包未处理(注释明说"先不考虑"),需补分包并加专项压测(超大 SELECT 结果、流式返回)
   - **超长事务**:配合 #3 版本回收(长事务导致 `versions_` 不压缩、内存增长),加"开事务→持续写入→长时 hold→崩溃恢复"专项压测

7. **employees 数据集验证完成(2026-11-14)**:与真实 MySQL 同库对比 14/14 MATCH;同步修复 Navicat 2013(握手 `CLIENT_CONNECT_ATTRS` 残留字节)、DATE MIN/MAX(年份截断)、整型 SUM 字符串回包三项。**剩余差距**:大表 JOIN 疑似未利用主键索引走全表扫描(3 表 JOIN 77s vs MySQL 6.5s,待补 JOIN 索引利用);AVG 仍以字符串回包 —— 详见 §0 v1.2.2 交付。

8. **sqllogictest 套件收尾(2026-11-16)**:FAIL 已清零,剩余 113 项 GAP 按域排序:函数 24(日期 14)、DDL/DML 方言 31、高级查询 19(窗口 Rank/LAG/LEAD、FULL OUTER JOIN、集合运算)、元数据 16、事务 5、表达式 15、兼容/回归 3。优先级:
   - **持久化(已完成)**:`SIGINT`/`SIGTERM` + `SHUTDOWN` → checkpoint → truncate WAL,persistence 2 项 GAP 已清零
   - **函数**:日期函数优先;解析器放开 `IF/LEFT/RIGHT/TRUNCATE/CAST/CONVERT` 的函数名上下文
   - **DDL/DML 方言**:CTAS、VIEW、INDEX、UNIQUE/CHECK、ALTER、TRUNCATE、INSERT 方言族
   - **高级查询**:窗口方向先校正 `RANK()` 用例期望值、修复 `DENSE_RANK()`,再补 `LAG/LEAD/COUNT(*) OVER`;以及 `FULL OUTER JOIN`、`JOIN USING`、`INTERSECT/EXCEPT`

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
| C03 | DML / 表达式 / NULL / 全链路 | `smoke_m2.cpp`、`test_full_flow.py`、`test_vs_sqlite.py` | CRUD、WHERE/ORDER/LIMIT、聚合、内置函数、NULL 列、LIKE、错误码;`test_full_flow.py` 另跨 C02/C06/C08 覆盖 DDL、事务、四隔离级别、多库 | ✅ 48 PASS / 0 GAP、全场景一致 |
| C04 | 聚合 / 子查询 / JOIN | `smoke_m3.cpp` | GROUP BY / HAVING / 子查询 IN / `INSERT…SELECT` / 各类 JOIN | ✅ |
| C05 | HSQLDB 可移植 SQL 语义矩阵 | `smoke_m6_hsqldb_matrix.cpp` | A NULL 三值、B 空集聚合、C JOIN 基数(含 Hash Join / EXPLAIN 回归)、D 主键与语句原子性、E 多库隔离、F 事务内可见性 | ✅ 73 PASS / 0 GAP |
| C06 | 事务 / 四隔离级别 / MVCC | `smoke_m6_isolation.cpp`、`test_txn_vs_sqlite.py` | 四级别提升、无脏读、不可重复读、40001 冲突、autocommit、SAVEPOINT、DML 回滚、WAL 重启 | ✅ 493/493、70/70 对比 |
| C07 | WAL / 快照 / 恢复 | `smoke_m5_wal.cpp`、`test_wal_recovery.py` | Replay、CRC 截断、Checkpoint、重启后数据与 UPDATE/DELETE 状态 | ✅ |
| C08 | MySQL 协议 / 客户端兼容 | `smoke_m25_e2e.cpp`、`test_sqlalchemy.py` | 握手与 packet、COM_QUERY、OK/EOF/ERR;SQLAlchemy 15 组:Engine、Core DDL/CRUD、聚合、ORM、2.0 风格 select/insert/update/delete、`text():param`、Session rollback、relationship 级联、`engine.begin()` 事务块、类型映射(DateTime/Boolean/Float)、Inspector(`DESCRIBE`/`SHOW FULL TABLES` 已支持) | ✅ 33 PASS / 0 FAIL / 0 GAP |
| C09 | 并发 / 性能 / 稳定性 | `test_concurrency.py`、`test_perf_vs_sqlite.py`、`stress_stability.py` | 5 线程 15s 读写一致性、大数据量 INSERT + JOIN 对比、10 条复杂查询计时、并发 soak + 事务冲突不变量(无丢写/幻行)、`kill -9` 崩溃恢复(WAL replay 原子性)、RSS 内存稳定 | ✅ 无 crash / PASS |
| C10 | 第三方客户端兼容(v1.1.0) | `dbx_probe.cpp`、`test_stmt_prepare.py`、`smoke_m6_session_db.cpp` | 进程内跑 DBX/dbExpress 连接/元数据/事务典型 SQL(58 断言)、裸 socket 走 `COM_STMT_PREPARE/EXECUTE/CLOSE` binary protocol 全流程(7 断言)、多库会话级隔离、`smoke_m25_e2e` 适配经典 EOF 帧 | ✅ 58/58、7/7、PASS |
| C11 | 真实数据集对比(v1.2.2) | `load_employees.py`、`compare_employees.py`、`_verify_fixes.py` | test_db-master employees 6 表 392 万行灌入(WAL)、与 MySQL 14 条查询双端比对、Navicat 式握手(CONNECT_ATTRS)回归、DATE 聚合 / SUM 类型断言 | ✅ 14/14 MATCH |

**分层**:C01–C07 为进程内 C++ smoke(无需 server,纳入 `CTest`);C08–C10 需要 server(端口 3306 或由脚本自动起停的随机端口);`test_txn_vs_sqlite.py` 自带 server 生命周期管理。当前 `ctest` **10/10**(10 个 CTest 目标,含 v1.1.0 新增的 `dbx_probe`;需先启动 3306 端口的 server 供 `smoke_m25_e2e` 连接)。

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
| 1 | `WHERE NOT (a = NULL)` | 0 行(UNKNOWN 取反仍是 UNKNOWN) | 3 行(UNKNOWN 被当作 FALSE 取反) | **✅ 已修复**:`expr.cpp` 比较遇 NULL 返回 UNKNOWN,`NOT UNKNOWN` 仍是 UNKNOWN,`AND/OR` 实现完整三值逻辑 |
| 2 | 空集 `SUM` / `AVG` | NULL | 0 | **✅ 已修复**:`agg_one_bucket` 在无输入行时返回 NULL(只有 `COUNT` 返回 0) |
| 3 | `COALESCE(SUM(id),0)` | 0 | 空结果集 | **✅ 已修复**:递归检测标量函数内嵌的聚合,聚合上下文用纯值递归求值 |
| 4 | `SELECT user_id FROM orders GROUP BY user_id HAVING SUM(total)>100` | 2 行 `(1),(3)` | 5 行 `(1),(1),(2),(3),(4)`(逐行输出) | **✅ 已修复**:`has_agg` 改为递归检测 SELECT 列,并让 GROUP BY / HAVING 也触发聚合路径 |
| 5 | `SELECT COUNT(*) FROM orders WHERE status = :st`(SQLAlchemy `text()` 绑定) | 正常执行 | 1064 `unexpected token in expression: 'status'`(`status` 疑似被词法层当保留字,且仅在该位置冲突) | **✅ 已修复 9e752ba**:列名与保留字冲突处按标识符处理 |
| 6 | 连接握手 `database` 字段 | 连接即切到指定库(`SELECT DATABASE()` 返回该库) | 字段被忽略,`SELECT DATABASE()` 仍返回全局 current db | **✅ 已修复 9e752ba**:握手响应后执行等价 `USE <db>`(写入 session 状态) |
| 7 | 多连接 current database 隔离 | per-session(连接 A 的 `USE` 不影响连接 B) | **服务器全局单例**:B 未 `USE` 却看到 A 的库;A 的库被 B 的 `USE` 覆盖,后续查询落到错误的库 | **✅ 已修复 9e752ba**:current db 移入 Session 状态(`smoke_m6_session_db` 覆盖) |
| 8 | `DROP DATABASE` 当前库 | MySQL 允许删除当前连接所在库 | 报 1008 `database doesn't exist or is current database` | **✅ 已修复 9e752ba**:判定改为仅校验库存在,删除成功即不再报 1008 |
| 5–8 | 协议/会话层(见上) | — | — | **✅ 已于 commit `9e752ba` 全部修复**:`status` 关键字冲突、`USE` 握手生效、current db 移入 Session 状态、`DROP DATABASE` 当前库改判;并顺带实现 `DESCRIBE` / `SHOW FULL TABLES` / `SHOW COLUMNS`(见 M6 P1 1.2)。`test_sqlalchemy.py` 现为 33 PASS / 0 FAIL / 0 GAP |

> **【2026-11-12 更新】§8.4 的 8 项 GAP 已全部消除**。#1–#4 是 SQL 语义层,本次在 `src/executor/expr.cpp` / `src/executor/executor.cpp` 落地三值逻辑、空集聚合、嵌套聚合与聚合路径判定后,`smoke_m6_hsqldb_matrix` 由 56 PASS / 4 GAP 升至 **62 PASS / 0 FAIL / 0 GAP**,`test_full_flow.py` 由 47 PASS / 1 GAP 升至 **48 PASS / 0 FAIL / 0 GAP**(第 4 项是被旧脚本 `len(rows) >= 2` 恒真断言掩盖的缺陷)。#5–#8 协议/会话层缺陷与反射语句已在 `9e752ba` 修复。当前所有 GAP 计数为 0,后续 HSQLDB 语义补充方向见 §8.3 待补清单。
>
> **构建注意**:曾出现 `Debug Assertion Failed: __acrt_first_block == header`(`debug_heap.cpp:996`),根因是 `build/` 缓存的 `VCPKG_TARGET_TRIPLET=x64-windows`(动态 fmt + 动态 CRT)与 `CMakeLists.txt` 要求的 `x64-windows-static` + 静态 CRT 不一致,两套 CRT 堆并存导致跨堆 `free`。用 `cmake --preset windows-vcpkg` 重新配置并全量重建 Debug/Release 后消除;**新环境务必用 preset 配置**,不要直接 `cmake -S . -B build`。
