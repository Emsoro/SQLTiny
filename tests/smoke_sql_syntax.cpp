// 常用 SQL 语法基本用例(端到端:Lexer -> Parser -> Executor)—— 无 GTest 依赖
//
// 分组:
//   A DDL / 元数据 / 列约束   B DML              C SELECT 基础与别名
//   D WHERE 谓词              E 表达式与函数     F GROUP BY / ORDER BY / LIMIT
//   G UNION / JOIN            H 事务与系统语句   I 负向用例(错误码)
//   J 尚未支持但常用的语法 -> 按项目约定记 [GAP],不计 FAIL
//
// 与 smoke_m2 / smoke_m6_* 的关系:那些是里程碑专项回归,本文件是「常用 SQL 子集」
// 的广度覆盖,每条语句都走 Session::execute,验证的是用户视角能否跑通。

#include <cstdio>
#include <cstdint>
#include <string>
#include <limits>

#include "core/session.h"
#include "executor/executor.h"
#include "types/value.h"

using namespace sqltiny;
using executor::ResultSet;

static int g_pass = 0, g_fail = 0, g_gap = 0;
static const int64_t kBad = std::numeric_limits<int64_t>::min();

static void check(bool cond, const std::string& label, const std::string& detail = "") {
    if (cond) { ++g_pass; }
    else {
        ++g_fail;
        printf("  [FAIL] %s %s\n", label.c_str(), detail.c_str());
    }
}

/// 与标准 SQL / MySQL 存在差距的点:只记录,不让整套用例变红
static void gap(const std::string& label, const std::string& actual) {
    ++g_gap;
    printf("  [GAP ] %s — 实际 %s\n", label.c_str(), actual.c_str());
}
static void gap_unless(bool cond, const std::string& label, const std::string& actual) {
    if (cond) { ++g_pass; printf("  [PASS] %s\n", label.c_str()); }
    else gap(label, actual);
}

static ResultSet q(core::Session& s, const std::string& sql) { return s.execute(sql); }

static std::string brief(const ResultSet& r) {
    if (!r.ok) return "errno=" + std::to_string(r.mysql_errno) + " " + r.error_message;
    if (r.rows.empty() || r.rows[0].empty()) return "<empty>";
    return "首值=" + r.rows[0][0].to_string() + " / " + std::to_string(r.rows.size()) + " 行";
}

static int64_t vi(const ResultSet& r) {
    if (!r.ok || r.rows.empty() || r.rows[0].empty()) return kBad;
    int64_t v = 0;
    return r.rows[0][0].try_to_int(v) ? v : kBad;
}
static std::string vs(const ResultSet& r) {
    if (!r.ok) return "ERR:" + std::to_string(r.mysql_errno);
    if (r.rows.empty() || r.rows[0].empty()) return "<empty>";
    const auto& v = r.rows[0][0];
    if (v.is_null()) return "NULL";
    return v.kind == types::Value::Kind::String ? v.v_str : v.to_string();
}

static void ok(core::Session& s, const std::string& sql, const std::string& label) {
    auto r = q(s, sql);
    check(r.ok, label, brief(r));
}
static void eq_i(core::Session& s, const std::string& sql, int64_t want,
                 const std::string& label) {
    auto r = q(s, sql);
    int64_t got = vi(r);
    check(got == want, label, brief(r) + " want=" + std::to_string(want));
}
static void eq_s(core::Session& s, const std::string& sql, const std::string& want,
                 const std::string& label) {
    auto r = q(s, sql);
    std::string got = vs(r);
    check(got == want, label, "got '" + got + "' want '" + want + "'");
}
static void eq_rows(core::Session& s, const std::string& sql, size_t want,
                    const std::string& label) {
    auto r = q(s, sql);
    check(r.ok && r.rows.size() == want, label,
          brief(r) + " want=" + std::to_string(want) + " 行");
}
static void err(core::Session& s, const std::string& sql, uint16_t code,
                const std::string& label) {
    auto r = q(s, sql);
    check(!r.ok && r.mysql_errno == code, label,
          brief(r) + " want errno=" + std::to_string(code));
}

static void setup(core::Session& s) {
    ok(s, "USE default", "setup: USE default");
    ok(s, "DROP DATABASE IF EXISTS sql_basics", "setup: 清理旧库");
    ok(s, "CREATE DATABASE sql_basics", "setup: CREATE DATABASE");
    ok(s, "USE sql_basics", "setup: USE sql_basics");
    ok(s, "CREATE TABLE emp ("
          "id INT PRIMARY KEY AUTO_INCREMENT, "
          "name VARCHAR(32) NOT NULL, "
          "dept VARCHAR(16), "
          "salary INT DEFAULT 0, "
          "age INT, "
          "city VARCHAR(16)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4",
       "setup: CREATE TABLE(含表选项)");
    ok(s, "INSERT INTO emp (id,name,dept,salary,age,city) VALUES "
          "(1,'Alice','dev',12000,30,'BJ'),"
          "(2,'Bob','dev',9000,25,'SH'),"
          "(3,'Carol','qa',8000,28,'BJ'),"
          "(4,'Dave','qa',11000,35,NULL),"
          "(5,'Eve',NULL,NULL,22,'SH')",
       "setup: 多行 VALUES(含 NULL)");
    ok(s, "CREATE TABLE dept (id INT PRIMARY KEY, name VARCHAR(16), budget INT)",
       "setup: CREATE TABLE dept");
    ok(s, "INSERT INTO dept VALUES (1,'dev',100000),(2,'qa',80000),(3,'ops',50000)",
       "setup: dept 数据");
    ok(s, "CREATE TABLE empty_t (id INT, v INT)", "setup: 空表");
}

// ---------------------------------------------------------------- A · DDL
static void group_ddl(core::Session& s) {
    printf("\n[A] DDL / 元数据 / 列约束\n");
    ok(s, "CREATE DATABASE IF NOT EXISTS sql_basics", "CREATE DATABASE IF NOT EXISTS");
    ok(s, "CREATE TABLE IF NOT EXISTS emp (id INT)", "CREATE TABLE IF NOT EXISTS");
    err(s, "CREATE TABLE emp (id INT)", 1050, "重复建表 -> 1050");
    ok(s, "DROP TABLE IF EXISTS no_such_table", "DROP TABLE IF EXISTS 不存在的表");

    auto tables = q(s, "SHOW TABLES");
    check(tables.ok && tables.rows.size() >= 3, "SHOW TABLES", brief(tables));
    eq_rows(s, "SHOW COLUMNS FROM emp", 6, "SHOW COLUMNS FROM emp(6 列)");
    eq_rows(s, "DESCRIBE emp", 6, "DESCRIBE emp");
    ok(s, "SHOW CREATE TABLE emp", "SHOW CREATE TABLE");
    ok(s, "SHOW DATABASES", "SHOW DATABASES");
    ok(s, "SHOW TABLES LIKE 'emp'", "SHOW TABLES LIKE");

    ok(s, "CREATE TABLE cons (id INT PRIMARY KEY, v INT NOT NULL DEFAULT 7, s VARCHAR(8))",
       "约束表: PRIMARY KEY / NOT NULL / DEFAULT");
    ok(s, "INSERT INTO cons (id) VALUES (1)", "省略列插入");
    eq_i(s, "SELECT v FROM cons WHERE id=1", 7, "DEFAULT 7 生效");
    eq_s(s, "SELECT s FROM cons WHERE id=1", "NULL", "省略列默认 NULL");
    err(s, "INSERT INTO cons VALUES (1,9,'dup')", 1062, "重复主键 -> 1062");

    ok(s, "CREATE TABLE ai (id INT PRIMARY KEY AUTO_INCREMENT, v INT)", "AUTO_INCREMENT 表");
    auto ins = q(s, "INSERT INTO ai (v) VALUES (10)");
    check(ins.ok && ins.last_insert_id == 1, "AUTO_INCREMENT 首次 last_insert_id=1",
          brief(ins));
    ins = q(s, "INSERT INTO ai (v) VALUES (20)");
    check(ins.ok && ins.last_insert_id == 2, "AUTO_INCREMENT 递增 =2", brief(ins));
    eq_i(s, "SELECT COUNT(*) FROM ai", 2, "AUTO_INCREMENT 共 2 行");
    ok(s, "DROP TABLE cons", "cleanup cons");
    ok(s, "DROP TABLE ai", "cleanup ai");
}

// ---------------------------------------------------------------- B · DML
static void group_dml(core::Session& s) {
    printf("\n[B] DML / INSERT / UPDATE / DELETE\n");
    ok(s, "CREATE TABLE dml_t (id INT PRIMARY KEY, v INT, s VARCHAR(16))", "DML 测试表");
    auto r = q(s, "INSERT INTO dml_t VALUES (1,10,'a'),(2,20,'b'),(3,30,'c')");
    check(r.ok && r.affected_rows == 3, "多行 INSERT affected_rows=3", brief(r));
    ok(s, "INSERT INTO dml_t (id,v) VALUES (4,40)", "指定列 INSERT");
    eq_i(s, "SELECT COUNT(*) FROM dml_t", 4, "INSERT 后 4 行");
    ok(s, "INSERT INTO dml_t (id,v,s) SELECT id+10, v*2, s FROM dml_t WHERE id<=2",
       "INSERT ... SELECT");
    eq_i(s, "SELECT COUNT(*) FROM dml_t", 6, "INSERT SELECT 后 6 行");

    r = q(s, "UPDATE dml_t SET v = v + 100, s = CONCAT(s,'!') WHERE id = 1");
    check(r.ok && r.affected_rows == 1, "UPDATE 多列 + 表达式", brief(r));
    eq_i(s, "SELECT v FROM dml_t WHERE id=1", 110, "UPDATE 生效(v=110)");
    r = q(s, "DELETE FROM dml_t WHERE id = 3");
    check(r.ok && r.affected_rows == 1, "DELETE WHERE affected_rows=1", brief(r));
    eq_i(s, "SELECT COUNT(*) FROM dml_t", 5, "DELETE 后 5 行");
    ok(s, "DELETE FROM dml_t", "无 WHERE 全表 DELETE");
    eq_i(s, "SELECT COUNT(*) FROM dml_t", 0, "全表 DELETE 后 0 行");
    ok(s, "DROP TABLE dml_t", "cleanup dml_t");
}

// ---------------------------------------------------------------- C · SELECT
static void group_select(core::Session& s) {
    printf("\n[C] SELECT 基础 / 别名 / DISTINCT / DUAL\n");
    eq_i(s, "SELECT COUNT(*) FROM emp", 5, "COUNT(*) = 5");
    auto all = q(s, "SELECT * FROM emp");
    check(all.ok && all.rows.size() == 5 && all.column_names.size() == 6,
          "SELECT * 返回 5 行 6 列", brief(all));
    eq_s(s, "SELECT name FROM emp WHERE id=1", "Alice", "单列投影");
    eq_i(s, "SELECT salary FROM emp WHERE id=1", 12000, "数值列投影");

    auto a = q(s, "SELECT name AS n FROM emp WHERE id=1");
    check(a.ok && vs(a) == "Alice", "AS 列别名(值)", brief(a));
    gap_unless(!a.column_names.empty() && a.column_names[0] == "n",
               "AS 别名写入 column_names",
               a.column_names.empty() ? "<empty>" : a.column_names[0]);
    auto b = q(s, "SELECT name n FROM emp WHERE id=1");
    check(b.ok && vs(b) == "Alice", "无 AS 裸别名(值)", brief(b));
    gap_unless(!b.column_names.empty() && b.column_names[0] == "n",
               "裸别名写入 column_names",
               b.column_names.empty() ? "<empty>" : b.column_names[0]);
    eq_s(s, "SELECT e.name FROM emp e WHERE e.id = 1", "Alice", "表别名 e.col");
    eq_s(s, "SELECT emp.name FROM emp WHERE emp.id = 1", "Alice", "表名限定列");
    eq_s(s, "SELECT `name` FROM emp WHERE `id` = 1", "Alice", "反引号标识符");

    eq_i(s, "SELECT 1 + 2", 3, "无表 SELECT 表达式");
    eq_i(s, "SELECT 1 + 2 FROM DUAL", 3, "FROM DUAL");
    eq_i(s, "SELECT TRUE", 1, "TRUE 字面量");
    eq_i(s, "SELECT NULL IS NULL", 1, "NULL 字面量 + IS NULL");
    eq_rows(s, "SELECT ALL name FROM emp", 5, "SELECT ALL = 5 行");
    eq_rows(s, "SELECT DISTINCT name FROM emp", 5, "SELECT DISTINCT name = 5 行");
    gap_unless(q(s, "SELECT DISTINCT dept FROM emp").rows.size() == 3,
               "SELECT DISTINCT 含 NULL 组(dev/qa/NULL)",
               brief(q(s, "SELECT DISTINCT dept FROM emp")));
}

// ---------------------------------------------------------------- D · WHERE
static void group_where(core::Session& s) {
    printf("\n[D] WHERE 谓词 / 三值逻辑 / 子查询\n");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE salary = 9000", 1, "= 比较");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE salary <> 9000", 3, "<> 比较(NULL 不计)");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE salary != 9000", 3, "!= 比较");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE salary > 9000", 2, "> 比较");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE salary >= 9000", 3, ">= 比较");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE salary < 9000", 1, "< 比较");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE salary <= 9000", 2, "<= 比较");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE dept='dev' AND salary>10000", 1, "AND");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE dept='qa' OR dept='dev'", 4, "OR");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE (dept='dev' OR dept='qa') AND salary>=9000",
         3, "括号优先级");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE id IN (1,3,5)", 3, "IN 列表");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE id NOT IN (1,3,5)", 2, "NOT IN 列表");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE id IN (SELECT id FROM emp WHERE dept='qa')",
         2, "IN (子查询)");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE name LIKE 'B%'", 1, "LIKE 前缀");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE name LIKE '%o%'", 2, "LIKE 包含(Bob/Carol)");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE name NOT LIKE 'B%'", 4, "NOT LIKE");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE city IS NULL", 1, "IS NULL");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE city IS NOT NULL", 4, "IS NOT NULL");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE salary = NULL", 0, "= NULL 永不匹配");
    eq_i(s, "SELECT COUNT(*) FROM emp WHERE NOT (dept = 'dev')", 2,
         "NOT 与 NULL 三值逻辑");
}

// ---------------------------------------------------------------- E · 表达式
static void group_expr(core::Session& s) {
    printf("\n[E] 表达式 / 内置函数 / 聚合\n");
    eq_i(s, "SELECT 10 + 3", 13, "加法");
    eq_i(s, "SELECT 10 - 3", 7, "减法");
    eq_i(s, "SELECT 10 * 3", 30, "乘法");
    eq_i(s, "SELECT 10 / 2", 5, "除法");
    eq_i(s, "SELECT 10 % 3", 1, "取模");
    eq_i(s, "SELECT -5 + 2", -3, "一元负号");
    eq_i(s, "SELECT (1+2)*3", 9, "括号优先级");
    eq_s(s, "SELECT CASE WHEN 1=1 THEN 'yes' ELSE 'no' END", "yes", "搜索 CASE");
    eq_s(s, "SELECT CASE 2 WHEN 1 THEN 'a' WHEN 2 THEN 'b' ELSE 'c' END", "b", "简单 CASE");
    eq_i(s, "SELECT ABS(-42)", 42, "ABS");
    eq_i(s, "SELECT LENGTH('abc')", 3, "LENGTH");
    eq_i(s, "SELECT INSTR('abc','b')", 2, "INSTR");
    eq_s(s, "SELECT CONCAT('a','-','b')", "a-b", "CONCAT");
    eq_s(s, "SELECT COALESCE(NULL,'fallback')", "fallback", "COALESCE");
    eq_i(s, "SELECT IFNULL(NULL,7)", 7, "IFNULL");
    check(vs(q(s, "SELECT VERSION()")).find("SQLTiny") != std::string::npos,
          "VERSION()", brief(q(s, "SELECT VERSION()")));
    eq_s(s, "SELECT DATABASE()", "sql_basics", "DATABASE() 返回当前库");

    eq_i(s, "SELECT COUNT(*) FROM emp", 5, "COUNT(*)");
    eq_i(s, "SELECT COUNT(salary) FROM emp", 4, "COUNT(col) 跳过 NULL");
    eq_i(s, "SELECT SUM(salary) FROM emp", 40000, "SUM");
    eq_i(s, "SELECT AVG(salary) FROM emp", 10000, "AVG");
    eq_i(s, "SELECT MIN(salary) FROM emp", 8000, "MIN");
    eq_i(s, "SELECT MAX(salary) FROM emp", 12000, "MAX");
    eq_i(s, "SELECT COUNT(*) FROM empty_t", 0, "空表 COUNT(*)=0");
    eq_i(s, "SELECT COALESCE(SUM(v),0) FROM empty_t", 0, "空集 SUM 用 COALESCE 兜底");
}

// ---------------------------------------------------------------- F · 分组排序
static void group_group_order_limit(core::Session& s) {
    printf("\n[F] GROUP BY / HAVING / ORDER BY / LIMIT\n");
    eq_rows(s, "SELECT dept, COUNT(*) FROM emp GROUP BY dept", 3, "GROUP BY 3 组");
    eq_rows(s, "SELECT dept, COUNT(*) FROM emp GROUP BY dept HAVING COUNT(*) >= 2", 2,
            "HAVING 过滤分组");
    eq_rows(s, "SELECT dept, SUM(salary) FROM emp GROUP BY dept "
               "HAVING SUM(salary) > 10000 ORDER BY dept", 2,
            "GROUP BY + HAVING 聚合 + ORDER BY");
    eq_s(s, "SELECT name FROM emp WHERE salary IS NOT NULL ORDER BY salary DESC LIMIT 1",
         "Alice", "ORDER BY DESC + LIMIT 1");
    eq_s(s, "SELECT name FROM emp WHERE salary IS NOT NULL ORDER BY salary ASC LIMIT 1",
         "Carol", "ORDER BY ASC + LIMIT 1");
    eq_rows(s, "SELECT name FROM emp ORDER BY dept DESC, name ASC", 5, "多列 ORDER BY");
    gap_unless(vs(q(s, "SELECT name, salary FROM emp WHERE salary IS NOT NULL "
                       "ORDER BY 2 DESC LIMIT 1")) == "Alice",
               "ORDER BY 2 位置序号",
               brief(q(s, "SELECT name, salary FROM emp WHERE salary IS NOT NULL "
                          "ORDER BY 2 DESC LIMIT 1")));
    auto p = q(s, "SELECT id FROM emp ORDER BY id LIMIT 2 OFFSET 1");
    check(p.ok && p.rows.size() == 2 && vi(p) == 2, "LIMIT 2 OFFSET 1 (首行 id=2)",
          brief(p));
    p = q(s, "SELECT id FROM emp ORDER BY id LIMIT 1, 2");
    check(p.ok && p.rows.size() == 2 && vi(p) == 2, "LIMIT 1,2 (首行 id=2)", brief(p));
    eq_rows(s, "SELECT id FROM emp ORDER BY id LIMIT 3", 3, "LIMIT n");
}

// ---------------------------------------------------------------- G · 集合/JOIN
static void group_union_join(core::Session& s) {
    printf("\n[G] UNION / JOIN\n");
    eq_rows(s, "SELECT id FROM emp WHERE id=1 UNION SELECT id FROM emp WHERE id=2", 2,
            "UNION 两分支");
    eq_rows(s, "SELECT 1 UNION SELECT 1", 1, "UNION 去重");
    eq_rows(s, "SELECT 1 UNION ALL SELECT 1", 2, "UNION ALL 不去重");
    eq_rows(s, "SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3", 3, "链式 UNION ALL");
    eq_rows(s, "SELECT id FROM emp WHERE id<=2 UNION DISTINCT SELECT id FROM emp WHERE id=2", 2,
            "UNION DISTINCT");
    gap_unless(vi(q(s, "SELECT 1 AS n UNION ALL SELECT 2 ORDER BY 1 DESC LIMIT 1")) == 2,
               "UNION 尾部 ORDER BY 1 DESC",
               brief(q(s, "SELECT 1 AS n UNION ALL SELECT 2 ORDER BY 1 DESC LIMIT 1")));

    eq_i(s, "SELECT COUNT(*) FROM emp e INNER JOIN dept d ON e.dept = d.name", 4,
         "INNER JOIN 等值(dev/qa 各 2)");
    eq_i(s, "SELECT COUNT(*) FROM emp e LEFT JOIN dept d ON e.dept = d.name", 5,
         "LEFT JOIN 保留左表全部 5 行");
    eq_i(s, "SELECT COUNT(*) FROM emp CROSS JOIN dept", 15, "CROSS JOIN 5×3");
    auto comma = q(s, "SELECT COUNT(*) FROM emp, dept");
    gap_unless(comma.ok && vi(comma) == 15,
               "逗号连接 FROM a, b = CROSS JOIN(期望 15)", brief(comma));
    eq_i(s, "SELECT d.budget FROM emp e INNER JOIN dept d ON e.dept = d.name WHERE e.id=1",
         100000, "JOIN 后取右表列");
    eq_i(s, "SELECT COUNT(*) FROM emp a INNER JOIN emp b ON a.id = b.id", 5, "自连接");
}

// ---------------------------------------------------------------- H · 事务/系统
static void group_txn_system(core::Session& s) {
    printf("\n[H] 事务 / 系统语句 / EXPLAIN\n");
    ok(s, "CREATE TABLE txn_t (id INT PRIMARY KEY)", "事务测试表");
    ok(s, "BEGIN", "BEGIN");
    ok(s, "INSERT INTO txn_t VALUES (1)", "事务内 INSERT");
    eq_i(s, "SELECT COUNT(*) FROM txn_t", 1, "事务内可见自己的写");
    ok(s, "ROLLBACK", "ROLLBACK");
    eq_i(s, "SELECT COUNT(*) FROM txn_t", 0, "ROLLBACK 后 0 行");
    ok(s, "START TRANSACTION", "START TRANSACTION");
    ok(s, "INSERT INTO txn_t VALUES (1)", "事务内 INSERT(2)");
    ok(s, "COMMIT", "COMMIT");
    eq_i(s, "SELECT COUNT(*) FROM txn_t", 1, "COMMIT 后 1 行");
    ok(s, "BEGIN", "BEGIN(保存点)");
    ok(s, "INSERT INTO txn_t VALUES (2)", "保存点前 INSERT");
    ok(s, "SAVEPOINT sp1", "SAVEPOINT sp1");
    ok(s, "INSERT INTO txn_t VALUES (3)", "保存点后 INSERT");
    ok(s, "ROLLBACK TO SAVEPOINT sp1", "ROLLBACK TO SAVEPOINT");
    eq_i(s, "SELECT COUNT(*) FROM txn_t", 2, "回滚到保存点后 2 行");
    ok(s, "RELEASE SAVEPOINT sp1", "RELEASE SAVEPOINT");
    ok(s, "COMMIT", "COMMIT(保存点事务)");

    ok(s, "SET NAMES utf8mb4", "SET NAMES");
    ok(s, "SET AUTOCOMMIT = 0", "SET AUTOCOMMIT = 0");
    ok(s, "SET AUTOCOMMIT = 1", "SET AUTOCOMMIT = 1(恢复)");
    ok(s, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ",
       "SET SESSION ... ISOLATION LEVEL");
    ok(s, "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED", "恢复隔离级别");
    ok(s, "SHOW VARIABLES LIKE 'tx_isolation'", "SHOW VARIABLES LIKE");
    ok(s, "SHOW STATUS", "SHOW STATUS");
    ok(s, "SHOW ENGINES", "SHOW ENGINES");
    auto plan = q(s, "EXPLAIN SELECT * FROM emp WHERE id = 1");
    check(plan.ok && !plan.rows.empty(), "EXPLAIN 返回计划行", brief(plan));
    ok(s, "DROP TABLE txn_t", "cleanup txn_t");
}

// ---------------------------------------------------------------- I · 负向用例
static void group_negative(core::Session& s) {
    printf("\n[I] 负向用例 / MySQL 错误码\n");
    err(s, "SELECT FROM", 1064, "缺列列表 -> 1064 语法错误");
    err(s, "FOO BAR", 1064, "未知语句 -> 1064");
    err(s, "SELECT * FROM no_such_table", 1146, "不存在的表 -> 1146");
    auto unknown = q(s, "SELECT no_such_col FROM emp");
    gap_unless(!unknown.ok && unknown.mysql_errno == 1054,
               "不存在的列 -> 1054", brief(unknown));
    err(s, "INSERT INTO emp (id,name) VALUES (1,'dup')", 1062, "重复主键 -> 1062");
    err(s, "USE no_such_db", 1049, "不存在的库 -> 1049");
}

// ---------------------------------------------------------------- J · 已知 GAP
static void group_gaps(core::Session& s) {
    printf("\n[J] 常用但当前未实现的语法(记录为 GAP,不计 FAIL)\n");
    auto between = q(s, "SELECT COUNT(*) FROM emp WHERE age BETWEEN 25 AND 30");
    gap_unless(between.ok && vi(between) == 3, "BETWEEN ... AND ... (期望 3 行)",
               brief(between));
    auto not_between = q(s, "SELECT COUNT(*) FROM emp WHERE age NOT BETWEEN 25 AND 30");
    gap_unless(not_between.ok && vi(not_between) == 2, "NOT BETWEEN (期望 2 行)",
               brief(not_between));
    auto intersect = q(s, "SELECT id FROM emp WHERE id<=3 INTERSECT SELECT id FROM emp WHERE id>=3");
    gap_unless(intersect.ok && intersect.rows.size() == 1, "INTERSECT 集合运算",
               brief(intersect));
    auto except = q(s, "SELECT id FROM emp EXCEPT SELECT id FROM emp WHERE id=1");
    gap_unless(except.ok && except.rows.size() == 4, "EXCEPT 集合运算", brief(except));
    auto truncate = q(s, "CREATE TABLE gap_t (id INT)");
    (void)truncate;
    gap_unless(q(s, "TRUNCATE TABLE gap_t").ok, "TRUNCATE TABLE", "parse_top 不支持");
    gap_unless(q(s, "ALTER TABLE gap_t ADD COLUMN v INT").ok, "ALTER TABLE ADD COLUMN",
               "parse_top 不支持");
    gap_unless(q(s, "REPLACE INTO gap_t VALUES (1)").ok, "REPLACE INTO",
               "parse_top 不支持");
    gap_unless(q(s, "INSERT INTO gap_t VALUE (1)").ok, "INSERT ... VALUE(单数)",
               "parse_insert 只认 VALUES");
    gap_unless(q(s, "WITH c AS (SELECT 1) SELECT * FROM c").ok, "WITH CTE",
               "parse_top 不支持");
    gap_unless(q(s, "SELECT 'abc' REGEXP 'a'").ok, "REGEXP 谓词", "表达式层未实现");
    ok(s, "DROP TABLE gap_t", "cleanup gap_t");
}

static void cleanup(core::Session& s) {
    ok(s, "USE default", "cleanup: USE default");
    ok(s, "DROP DATABASE IF EXISTS sql_basics", "cleanup: DROP DATABASE");
}

int main() {
    printf("=== SQLTiny 常用 SQL 语法基本用例(端到端) ===\n");
    core::Session s;   // 所有 Session 共享同一进程内 Catalog
    setup(s);
    group_ddl(s);
    group_dml(s);
    group_select(s);
    group_where(s);
    group_expr(s);
    group_group_order_limit(s);
    group_union_join(s);
    group_txn_system(s);
    group_negative(s);
    group_gaps(s);
    cleanup(s);
    printf("\n=== %d PASS / %d FAIL / %d GAP ===\n", g_pass, g_fail, g_gap);
    return g_fail == 0 ? 0 : 1;
}
