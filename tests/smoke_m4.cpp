// M4 Smoke: Catalog 多库化 + CREATE/DROP DATABASE + SHOW 系列 + USE
#include <cstdio>
#include <cassert>
#include <string>
#include <fmt/format.h>
#include "core/session.h"

using namespace sqltiny;

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); return 1; } } while(0)
#define CHECK_OK(rs) do { if (!(rs).ok) { fprintf(stderr, "FAIL: errno=%d msg=%s\n", (rs).mysql_errno, (rs).error_message.c_str()); return 1; } } while(0)
#define CHECK_FAIL(rs, _errno) do { if ((rs).ok || (rs).mysql_errno != (_errno)) { fprintf(stderr, "FAIL: expected errno=%d, got ok=%d errno=%d msg=%s\n", (_errno), (rs).ok, (rs).mysql_errno, (rs).error_message.c_str()); return 1; } } while(0)

static int exec_int(core::Session& s, const char* sql) {
    auto r = s.execute(sql); CHECK_OK(r);
    if (r.rows.empty() || r.rows[0].empty()) return 0;
    int64_t v = 0; r.rows[0][0].try_to_int(v); return static_cast<int>(v);
}

// ---------- CREATE / DROP DATABASE ----------
int test_create_drop_database() {
    core::Session s;
    // 已有 default 库
    auto r = s.execute("SHOW DATABASES");
    CHECK_OK(r);
    printf("  SHOW DATABASES 有 %zu 个 db:\n", r.rows.size());
    for (auto& row : r.rows) printf("    %s\n", row[0].v_str.c_str());
    // default 必须在
    bool has_default = false;
    for (auto& row : r.rows) if (row[0].v_str == "default") has_default = true;
    CHECK(has_default);

    // CREATE DATABASE
    r = s.execute("CREATE DATABASE mydb");
    CHECK_OK(r);
    // IF NOT EXISTS 正常
    r = s.execute("CREATE DATABASE IF NOT EXISTS mydb");
    CHECK_OK(r);
    // 重复建库无 IF NOT EXISTS 应该失败
    r = s.execute("CREATE DATABASE mydb");
    CHECK_FAIL(r, 1007);

    // SHOW DATABASES 现在有两个
    r = s.execute("SHOW DATABASES");
    CHECK_OK(r);
    CHECK(r.rows.size() >= 2);

    // DROP DATABASE
    r = s.execute("DROP DATABASE mydb");
    CHECK_OK(r);
    // IF EXISTS 正常
    r = s.execute("DROP DATABASE IF EXISTS mydb");
    CHECK_OK(r);
    // 不存在的库无 IF EXISTS 失败
    r = s.execute("DROP DATABASE mydb");
    CHECK_FAIL(r, 1008);

    // DBeaver 26.2.4 建 schema 会附加 DEFAULT CHARACTER SET / DEFAULT COLLATE
    // (曾报 1064: unexpected start of statement: 'DEFAULT' at line 2 col 1)
    r = s.execute("CREATE SCHEMA `dbeaver_db`\n"
                  "DEFAULT CHARACTER SET utf8\n"
                  "DEFAULT COLLATE utf8_general_ci");
    CHECK_OK(r);
    r = s.execute("SHOW DATABASES");
    CHECK_OK(r);
    bool has_dbeaver_db = false;
    for (auto& row : r.rows) if (row[0].v_str == "dbeaver_db") has_dbeaver_db = true;
    CHECK(has_dbeaver_db);
    r = s.execute("DROP DATABASE dbeaver_db");
    CHECK_OK(r);

    // 建表选项(ENGINE / DEFAULT CHARSET / COLLATE)也不能被当成下一条语句
    r = s.execute("CREATE TABLE dbeaver_t(id INT PRIMARY KEY) "
                  "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    CHECK_OK(r);
    r = s.execute("INSERT INTO dbeaver_t VALUES (1)");
    CHECK_OK(r);
    r = s.execute("SELECT id FROM dbeaver_t");
    CHECK_OK(r);
    CHECK(r.rows.size() == 1);

    // DBeaver 建表后用它校验表是否存在: WHERE 子句 + 原始大小写表名
    r = s.execute("SHOW FULL TABLES FROM `default` WHERE Tables_in_default = 'DBeaver_T'");
    CHECK_OK(r);
    CHECK(r.rows.size() == 1);
    r = s.execute("SHOW FULL TABLES FROM `default` WHERE Tables_in_default = 'NoSuchTable'");
    CHECK_OK(r);
    CHECK(r.rows.empty());
    // DBeaver 握手后查询该系统变量, 之前返回 0 行
    r = s.execute("SHOW VARIABLES LIKE 'lower_case_table_names'");
    CHECK_OK(r);
    CHECK(r.rows.size() == 1);

    r = s.execute("DROP TABLE dbeaver_t");
    CHECK_OK(r);

    printf("  CREATE/DROP DATABASE .. PASS\n");
    return 0;
}

// ---------- USE + 多库隔离 ----------
int test_use_database() {
    core::Session s;
    // 在 default 里建 users 表
    s.execute("CREATE TABLE default.users(id INT PRIMARY KEY, name VARCHAR(20))");
    s.execute("INSERT INTO default.users VALUES (1, 'Alice')");

    // 新建 testdb
    CHECK_OK(s.execute("CREATE DATABASE testdb"));
    CHECK_OK(s.execute("USE testdb"));

    // testdb 里不应该有 users 表
    auto r = s.execute("SHOW TABLES");
    CHECK_OK(r);
    CHECK(r.rows.empty());

    // 在 testdb 建 orders
    CHECK_OK(s.execute("CREATE TABLE orders(id INT PRIMARY KEY, total INT)"));
    CHECK_OK(s.execute("INSERT INTO orders VALUES (1, 99)"));
    CHECK(exec_int(s, "SELECT COUNT(*) FROM orders") == 1);

    // 切回 default,应该只有 users
    CHECK_OK(s.execute("USE default"));
    r = s.execute("SHOW TABLES");
    CHECK_OK(r);
    CHECK(r.rows.size() == 1);
    CHECK(r.rows[0][0].v_str == "users");

    // 跨库引用:从 default 里 SELECT testdb.orders 应该失败?—— Catalog 多库 get_table 应该支持 db.table
    r = s.execute("SELECT COUNT(*) FROM testdb.orders");
    CHECK_OK(r);
    CHECK(r.rows[0][0].v_int == 1);

    // USE 不存在的库
    r = s.execute("USE nonexist");
    CHECK_FAIL(r, 1049);

    // 清理
    s.execute("DROP TABLE default.users");
    s.execute("DROP TABLE testdb.orders");
    s.execute("DROP DATABASE testdb");
    printf("  USE + 多库隔离 ....... PASS\n");
    return 0;
}

// ---------- SHOW TABLES / COLUMNS ----------
int test_show_tables_columns() {
    core::Session s;
    s.execute("CREATE TABLE t1(id INT PRIMARY KEY, name VARCHAR(20), score DOUBLE)");
    s.execute("CREATE TABLE t2(a INT, b INT, c VARCHAR(10))");

    auto r = s.execute("SHOW TABLES");
    CHECK_OK(r);
    CHECK(r.rows.size() == 2);

    r = s.execute("SHOW COLUMNS FROM t1");
    CHECK_OK(r);
    CHECK(r.rows.size() == 3);
    CHECK(r.rows[0][0].v_str == "id");
    CHECK(r.rows[0][3].v_str == "PRI");   // Key 列

    // 不存在的表
    r = s.execute("SHOW COLUMNS FROM nope");
    CHECK_FAIL(r, 1146);

    // Database Client / DBeaver 风格: 双引号字符串作列别名 (MySQL 默认语义),
    // 之前报 1064 unexpected start of statement: 'comment'
    r = s.execute("SELECT name \"n\", id AS \"id\" FROM t1 WHERE id <> 0 ORDER BY name");
    CHECK_OK(r);
    // 元数据查询全风格: 反引号列名 + 双引号别名 + <> 比较 + information_schema
    r = s.execute("SELECT TABLE_COMMENT \"comment\", TABLE_NAME \"name\", TABLE_ROWS \"rows\", "
                  "`ENGINE` engine FROM information_schema.TABLES "
                  "WHERE TABLE_SCHEMA = 'default' AND TABLE_TYPE <> 'VIEW' ORDER BY TABLE_NAME");
    CHECK_OK(r);
    // 列元数据风格: 裸关键字别名(comment) + 表别名限定 + INSTR 函数
    r = s.execute("SELECT c.COLUMN_NAME name, COLUMN_COMMENT comment, `COLUMN_KEY` `key`, "
                  "INSTR(COLUMN_TYPE,'zerofill')>0 \"zerofill\" "
                  "FROM information_schema.columns c "
                  "WHERE c.table_schema = 'default' ORDER BY c.ORDINAL_POSITION");
    CHECK_OK(r);
    r = s.execute("SELECT INSTR('abcabc','ABC') AS i, INSTR('abc','zz') AS j");
    CHECK_OK(r);
    CHECK(r.rows.size() == 1 && r.rows[0][0].v_int == 1 && r.rows[0][1].v_int == 0);

    s.execute("DROP TABLE t1");
    s.execute("DROP TABLE t2");

    // DBeaver 打开表时查外键信息: 该虚拟表须存在且为 0 行(SQLTiny 不支持外键),
    // 且能被 JOIN / WHERE 正常处理, 否则会报 1146 Table '...REFERENTIAL_CONSTRAINTS' doesn't exist
    r = s.execute("SELECT * FROM information_schema.REFERENTIAL_CONSTRAINTS");
    CHECK_OK(r);
    CHECK(r.rows.empty());
    r = s.execute("SELECT rc.CONSTRAINT_NAME, kcu.COLUMN_NAME "
                  "FROM information_schema.REFERENTIAL_CONSTRAINTS rc "
                  "JOIN information_schema.KEY_COLUMN_USAGE kcu "
                  "ON kcu.CONSTRAINT_NAME = rc.CONSTRAINT_NAME "
                  "WHERE rc.REFERENCED_TABLE_NAME = 't1'");
    CHECK_OK(r);
    CHECK(r.rows.empty());

    printf("  SHOW TABLES/COLUMNS .. PASS\n");
    return 0;
}

// ---------- SHOW CREATE TABLE / CREATE DATABASE ----------
int test_show_create() {
    core::Session s;
    s.execute("CREATE TABLE users(id BIGINT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(64) NOT NULL, age INT DEFAULT 18)");

    auto r = s.execute("SHOW CREATE TABLE users");
    CHECK_OK(r);
    CHECK(r.rows.size() == 1);
    CHECK(r.rows[0][0].v_str == "users");
    auto& create_sql = r.rows[0][1].v_str;
    CHECK(create_sql.find("CREATE TABLE") != std::string::npos);
    CHECK(create_sql.find("users") != std::string::npos);

    // 带 db 限定
    r = s.execute("SHOW CREATE TABLE default.users");
    CHECK_OK(r);

    // SHOW CREATE DATABASE
    r = s.execute("SHOW CREATE DATABASE default");
    CHECK_OK(r);
    CHECK(r.rows[0][0].v_str == "default");
    CHECK(r.rows[0][1].v_str.find("CREATE DATABASE") != std::string::npos);

    // 不存在的库
    r = s.execute("SHOW CREATE DATABASE nope");
    CHECK_FAIL(r, 1049);

    // 不存在的表
    r = s.execute("SHOW CREATE TABLE nope");
    CHECK_FAIL(r, 1146);

    s.execute("DROP TABLE users");
    printf("  SHOW CREATE .......... PASS\n");
    return 0;
}

// ---------- SHOW STATUS / VARIABLES ----------
int test_show_status_variables() {
    core::Session s;
    auto r = s.execute("SHOW STATUS");
    CHECK_OK(r);
    CHECK(!r.rows.empty());
    CHECK(r.column_names.size() == 2);
    // 应该有 Version
    bool has_version = false;
    for (auto& row : r.rows) if (row[0].v_str == "Version") has_version = true;
    CHECK(has_version);

    // SHOW VARIABLES
    r = s.execute("SHOW VARIABLES");
    CHECK_OK(r);
    CHECK(!r.rows.empty());
    bool has_encoding = false;
    for (auto& row : r.rows) if (row[0].v_str.find("character_set") != std::string::npos) has_encoding = true;
    CHECK(has_encoding);

    // SHOW GLOBAL STATUS / SESSION VARIABLES 也能解析
    r = s.execute("SHOW GLOBAL STATUS");
    CHECK_OK(r);
    r = s.execute("SHOW SESSION VARIABLES");
    CHECK_OK(r);

    printf("  SHOW STATUS/VARIABLES  PASS\n");
    return 0;
}

int main() {
    printf("=== SQLTiny M4 Smoke Tests ===\n");
    int rc = 0;
    rc |= test_create_drop_database();
    rc |= test_use_database();
    rc |= test_show_tables_columns();
    rc |= test_show_create();
    rc |= test_show_status_variables();
    if (rc == 0) printf("All M4 smoke tests PASSED.\n");
    return rc;
}
