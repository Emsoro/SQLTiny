// Smoke test for M2 — C++ 单元级测试,无 GTest 依赖
#include <cstdio>
#include <cassert>
#include <string>
#include <fmt/format.h>
#include "core/session.h"
#include "executor/executor.h"

using namespace sqltiny;

static int total_pass = 0, total_fail = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
        total_fail++; \
        return 1; \
    } \
} while(0)

#define CHECK_OK(rs) do { \
    if (!(rs).ok) { \
        fprintf(stderr, "FAIL: expected OK, got errno=%d msg=%s\n", \
                (rs).mysql_errno, (rs).error_message.c_str()); \
        total_fail++; \
        return 1; \
    } \
} while(0)

static int exec_int(core::Session& s, const char* sql) {
    auto r = s.execute(sql);
    CHECK_OK(r);
    if (r.rows.empty() || r.rows[0].empty()) return 0;
    int64_t v = 0; r.rows[0][0].try_to_int(v); return static_cast<int>(v);
}

static std::string exec_str(core::Session& s, const char* sql) {
    auto r = s.execute(sql);
    if (!r.ok) {
        fprintf(stderr, "FAIL: expected OK, got errno=%d msg=%s\n",
                r.mysql_errno, r.error_message.c_str());
        total_fail++;
        return "";
    }
    if (r.rows.empty() || r.rows[0].empty()) return "";
    return r.rows[0][0].to_string();
}

int test_basic_crud() {
    core::Session s;
    auto r = s.execute("DROP TABLE IF EXISTS t");
    r = s.execute("CREATE TABLE users ("
                   "id BIGINT PRIMARY KEY AUTO_INCREMENT,"
                   "name VARCHAR(64) NOT NULL,"
                   "age INT DEFAULT 0)");
    CHECK_OK(r);

    r = s.execute("INSERT INTO users VALUES (1, 'Alice', 30)");
    CHECK_OK(r); CHECK(r.affected_rows == 1); CHECK(r.last_insert_id == 1);

    r = s.execute("INSERT INTO users VALUES (2, 'Bob', 25), (3, 'Charlie', 35)");
    CHECK_OK(r); CHECK(r.affected_rows == 2);

    r = s.execute("SELECT COUNT(*) FROM users");
    CHECK_OK(r);
    CHECK(r.rows.size() == 1 && r.rows[0].size() == 1);
    CHECK(r.rows[0][0].v_int == 3);

    // WHERE
    r = s.execute("SELECT name, age FROM users WHERE age > 28 ORDER BY age");
    CHECK_OK(r);
    CHECK(r.rows.size() == 2);
    CHECK(r.rows[0][0].v_str == "Alice");
    CHECK(r.rows[1][0].v_str == "Charlie");

    // LIKE
    r = s.execute("SELECT name FROM users WHERE name LIKE 'B%'");
    CHECK_OK(r); CHECK(r.rows.size() == 1);
    CHECK(r.rows[0][0].v_str == "Bob");

    // UPDATE
    r = s.execute("UPDATE users SET age = 26 WHERE name = 'Bob'");
    CHECK_OK(r); CHECK(r.affected_rows == 1);
    CHECK(exec_int(s, "SELECT age FROM users WHERE name = 'Bob'") == 26);

    // DELETE
    r = s.execute("DELETE FROM users WHERE age = 35");
    CHECK_OK(r); CHECK(r.affected_rows == 1);
    CHECK(exec_int(s, "SELECT COUNT(*) FROM users") == 2);

    // DROP
    r = s.execute("DROP TABLE users");
    CHECK_OK(r);
    r = s.execute("SELECT * FROM users");
    CHECK(!r.ok);

    printf("  basic CRUD ... PASS\n");
    return 0;
}

int test_agg() {
    core::Session s;
    s.execute("CREATE TABLE t(id INT PRIMARY KEY, val INT)");
    s.execute("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30), (4, NULL)");

    CHECK(exec_int(s, "SELECT COUNT(*) FROM t") == 4);
    CHECK(exec_int(s, "SELECT COUNT(val) FROM t") == 3);
    CHECK(exec_int(s, "SELECT SUM(val) FROM t") == 60);
    CHECK(exec_int(s, "SELECT AVG(val) FROM t") == 20);
    CHECK(exec_int(s, "SELECT MIN(val) FROM t") == 10);
    CHECK(exec_int(s, "SELECT MAX(val) FROM t") == 30);

    s.execute("DROP TABLE t");
    printf("  aggregates ... PASS\n");
    return 0;
}

int test_expr() {
    core::Session s;
    s.execute("CREATE TABLE t(a INT, b INT, name VARCHAR(20))");
    s.execute("INSERT INTO t VALUES (10, 3, 'hello'), (7, 2, 'world')");

    CHECK(exec_int(s, "SELECT a + b FROM t LIMIT 1") == 13);
    CHECK(exec_int(s, "SELECT a * b FROM t LIMIT 1") == 30);
    CHECK(exec_int(s, "SELECT a > 5 FROM t LIMIT 1") == 1);
    CHECK(exec_int(s, "SELECT a - b FROM t WHERE a = 7") == 5);
    CHECK(exec_int(s, "SELECT a IN (10, 20, 30) FROM t LIMIT 1") == 1);

    // 无表 SELECT
    CHECK(exec_int(s, "SELECT 1 + 2") == 3);
    CHECK(exec_int(s, "SELECT ABS(-42)") == 42);
    CHECK(exec_int(s, "SELECT LENGTH('abc')") == 3);

    // CONCAT
    auto r = s.execute("SELECT CONCAT(name, '-', a) FROM t WHERE a = 10");
    CHECK_OK(r);
    CHECK(r.rows[0][0].v_str == "hello-10");

    s.execute("DROP TABLE t");
    printf("  expressions ... PASS\n");
    return 0;
}

int test_ddl_edge() {
    core::Session s;
    auto r = s.execute("DROP TABLE IF EXISTS t");
    CHECK_OK(r);
    r = s.execute("CREATE TABLE t(id INT PRIMARY KEY)");
    CHECK_OK(r);
    r = s.execute("CREATE TABLE IF NOT EXISTS t(id INT PRIMARY KEY)");
    CHECK_OK(r);
    r = s.execute("CREATE TABLE t(id INT PRIMARY KEY)");
    CHECK(!r.ok);

    r = s.execute("SHOW TABLES");
    CHECK_OK(r); CHECK(r.rows.size() >= 1);

    r = s.execute("SHOW COLUMNS FROM t");
    CHECK_OK(r);
    CHECK(r.rows.size() == 1);
    CHECK(r.rows[0][0].v_str == "id");
    CHECK(r.rows[0][1].v_str.find("int") != std::string::npos);

    s.execute("DROP TABLE IF EXISTS t");
    r = s.execute("DROP TABLE IF EXISTS nonexistent");
    CHECK_OK(r);
    printf("  DDL edge cases ... PASS\n");
    return 0;
}

int test_where_null() {
    core::Session s;
    s.execute("CREATE TABLE t(id INT PRIMARY KEY, name VARCHAR(20))");
    s.execute("INSERT INTO t VALUES (1, NULL), (2, 'Alice')");

    auto r = s.execute("SELECT COUNT(*) FROM t WHERE name IS NULL");
    CHECK_OK(r); CHECK(r.rows[0][0].v_int == 1);

    r = s.execute("SELECT COUNT(*) FROM t WHERE name IS NOT NULL");
    CHECK_OK(r); CHECK(r.rows[0][0].v_int == 1);

    s.execute("DROP TABLE t");
    printf("  IS NULL ...... PASS\n");
    return 0;
}

int main() {
    printf("=== SQLTiny M2 Smoke Tests ===\n");
    int rc = 0;
    rc |= test_basic_crud();
    rc |= test_agg();
    rc |= test_expr();
    rc |= test_ddl_edge();
    rc |= test_where_null();
    if (rc == 0) printf("All M2 smoke tests PASSED.\n");
    return rc;
}
