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

    s.execute("DROP TABLE t1");
    s.execute("DROP TABLE t2");
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
