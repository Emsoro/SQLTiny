// M3 Smoke: GROUP BY + HAVING + 子查询 + INSERT … SELECT …
#include <cstdio>
#include <cassert>
#include <string>
#include <fmt/format.h>
#include "core/session.h"

using namespace sqltiny;

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); return 1; } } while(0)
#define CHECK_OK(rs) do { if (!(rs).ok) { fprintf(stderr, "FAIL: errno=%d msg=%s\n", (rs).mysql_errno, (rs).error_message.c_str()); return 1; } } while(0)

static int exec_int(core::Session& s, const char* sql) {
    auto r = s.execute(sql); CHECK_OK(r);
    if (r.rows.empty() || r.rows[0].empty()) return 0;
    int64_t v = 0; r.rows[0][0].try_to_int(v); return static_cast<int>(v);
}

// ---------- GROUP BY ----------
int test_group_by() {
    core::Session s;
    s.execute("CREATE TABLE t(dept VARCHAR(20), salary INT)");
    s.execute("INSERT INTO t VALUES ('A', 100), ('A', 200), ('B', 300), ('B', 400), ('C', 500)");

    // 按部门分组,每组 SUM
    auto r = s.execute("SELECT dept, SUM(salary) FROM t GROUP BY dept ORDER BY dept");
    CHECK_OK(r);
    CHECK(r.rows.size() == 3);
    CHECK(r.rows[0][0].v_str == "A" && r.rows[0][1].v_double == 300.0);
    CHECK(r.rows[1][0].v_str == "B" && r.rows[1][1].v_double == 700.0);
    CHECK(r.rows[2][0].v_str == "C" && r.rows[2][1].v_double == 500.0);

    // 空 group_by = 全表一组 (等效旧 flat 聚合)
    CHECK(exec_int(s, "SELECT COUNT(*) FROM t") == 5);

    s.execute("DROP TABLE t");
    printf("  GROUP BY ....... PASS\n");
    return 0;
}

// ---------- HAVING ----------
int test_having() {
    core::Session s;
    s.execute("CREATE TABLE t(dept VARCHAR(20), salary INT)");
    s.execute("INSERT INTO t VALUES ('A', 100), ('A', 200), ('B', 300), ('B', 400), ('C', 500)");

    // 只保留总薪水 > 500 的部门 (B=700 满足,C=500 不满足严格 >,被过滤)
    auto r = s.execute("SELECT dept, SUM(salary) AS total FROM t GROUP BY dept HAVING SUM(salary) > 500 ORDER BY dept");
    CHECK_OK(r);
    CHECK(r.rows.size() == 1);   // 只有 B
    CHECK(r.rows[0][0].v_str == "B");
    CHECK(r.rows[0][1].v_double == 700.0);

    // >= 应该保留 B 和 C
    r = s.execute("SELECT dept, SUM(salary) AS total FROM t GROUP BY dept HAVING SUM(salary) >= 500 ORDER BY dept");
    CHECK_OK(r);
    CHECK(r.rows.size() == 2);
    CHECK(r.rows[0][0].v_str == "B");
    CHECK(r.rows[1][0].v_str == "C");

    s.execute("DROP TABLE t");
    printf("  HAVING ......... PASS\n");
    return 0;
}

// ---------- 子查询 WHERE IN (SELECT) ----------
int test_subquery_in() {
    core::Session s;
    s.execute("CREATE TABLE users(id INT PRIMARY KEY, dept_id INT, name VARCHAR(20))");
    s.execute("CREATE TABLE depts(id INT PRIMARY KEY, name VARCHAR(20))");
    s.execute("INSERT INTO depts VALUES (1, 'A'), (2, 'B'), (3, 'C')");
    s.execute("INSERT INTO users VALUES (1, 1, 'Alice'), (2, 2, 'Bob'), (3, 99, 'Zack'), (4, 1, 'Ava')");

    // 找属于已存在部门的用户
    auto r = s.execute("SELECT name FROM users WHERE dept_id IN (SELECT id FROM depts) ORDER BY id");
    CHECK_OK(r);
    CHECK(r.rows.size() == 3);
    CHECK(r.rows[0][0].v_str == "Alice");
    CHECK(r.rows[1][0].v_str == "Bob");
    CHECK(r.rows[2][0].v_str == "Ava");

    // 非子查询 IN (常量列表)还要正常
    r = s.execute("SELECT COUNT(*) FROM users WHERE id IN (1, 2)");
    CHECK_OK(r); CHECK(r.rows[0][0].v_int == 2);

    s.execute("DROP TABLE users"); s.execute("DROP TABLE depts");
    printf("  subquery IN .... PASS\n");
    return 0;
}

// ---------- INSERT … SELECT … ----------
int test_insert_select() {
    core::Session s;
    s.execute("CREATE TABLE src(a INT, b INT)");
    s.execute("INSERT INTO src VALUES (1, 10), (2, 20), (3, 30)");
    s.execute("CREATE TABLE dst(x INT, y INT)");

    auto r = s.execute("INSERT INTO dst SELECT a, b FROM src");
    CHECK_OK(r); CHECK(r.affected_rows == 3);
    CHECK(exec_int(s, "SELECT COUNT(*) FROM dst") == 3);
    CHECK(exec_int(s, "SELECT SUM(y) FROM dst") == 60);

    s.execute("DROP TABLE src"); s.execute("DROP TABLE dst");
    printf("  INSERT SELECT .. PASS\n");
    return 0;
}

int main() {
    printf("=== SQLTiny M3 Smoke Tests ===\n");
    int rc = 0;
    rc |= test_group_by();
    rc |= test_having();
    rc |= test_subquery_in();
    rc |= test_insert_select();
    if (rc == 0) printf("All M3 smoke tests PASSED.\n");
    return rc;
}
