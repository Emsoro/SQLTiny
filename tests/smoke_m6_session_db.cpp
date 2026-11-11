// M6 P1 Smoke: per-session current db (CODEPLAN §8.4 #6/#7/#8) + DESCRIBE / SHOW FULL TABLES (P1 1.2)
#include <cstdio>
#include <string>
#include "core/session.h"

using namespace sqltiny;

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); return 1; } } while(0)
#define CHECK_OK(rs) do { if (!(rs).ok) { fprintf(stderr, "FAIL: errno=%d msg=%s\n", (rs).mysql_errno, (rs).error_message.c_str()); return 1; } } while(0)
#define CHECK_FAIL(rs, _errno) do { if ((rs).ok || (rs).mysql_errno != (_errno)) { fprintf(stderr, "FAIL: expected errno=%d, got ok=%d errno=%d msg=%s\n", (_errno), (rs).ok, (rs).mysql_errno, (rs).error_message.c_str()); return 1; } } while(0)

// ---------- #7: current db 是 per-session 状态 ----------
int test_per_session_db() {
    core::Session a, b;

    CHECK_OK(a.execute("CREATE DATABASE pdb"));
    CHECK_OK(a.execute("USE pdb"));
    CHECK_OK(a.execute("CREATE TABLE t(id INT PRIMARY KEY, v INT)"));
    CHECK_OK(a.execute("INSERT INTO t VALUES (1, 10)"));

    // B 没有切库,当前库仍是 default;DATABASE() 各自独立
    {
        auto ra = a.execute("SELECT DATABASE()");
        auto rb = b.execute("SELECT DATABASE()");
        CHECK_OK(ra); CHECK_OK(rb);
        CHECK(ra.rows[0][0].v_str == "pdb");
        CHECK(rb.rows[0][0].v_str == "default");
    }

    // B 看不到 A 库里的表 (1146),也不受 A 的 USE 影响
    {
        auto r = b.execute("SELECT * FROM t");
        CHECK_FAIL(r, 1146);
        CHECK_OK(b.execute("CREATE TABLE t(id INT PRIMARY KEY, w INT)"));
        CHECK_OK(b.execute("INSERT INTO t VALUES (100, 7)"));
        auto rb = b.execute("SELECT COUNT(*) FROM t");
        CHECK_OK(rb);
        CHECK(rb.rows[0][0].v_int == 1);   // B 的 t 只有自己那行
        auto ra = a.execute("SELECT COUNT(*) FROM t");
        CHECK_OK(ra);
        CHECK(ra.rows[0][0].v_int == 1);   // A 的 t 不被 B 干扰
    }

    // B 的 USE 不影响 A
    CHECK_OK(b.execute("USE default"));
    {
        auto ra = a.execute("SELECT DATABASE()");
        CHECK_OK(ra);
        CHECK(ra.rows[0][0].v_str == "pdb");
    }

    // 跨库引用仍可用
    {
        auto r = a.execute("SELECT COUNT(*) FROM default.t");
        CHECK_OK(r);
        CHECK(r.rows[0][0].v_int == 1);
    }

    // 清理
    a.execute("DROP TABLE pdb.t");
    b.execute("DROP TABLE default.t");
    a.execute("DROP DATABASE pdb");
    printf("  per-session current db ... PASS\n");
    return 0;
}

// ---------- #6 等价验证: 连接级 db 设置即生效 (server 端为握手 db, 进程内等价于 USE) ----------
int test_session_start_db() {
    core::Session a;
    CHECK_OK(a.execute("CREATE DATABASE sdb"));
    // 模拟"握手 database 字段":会话一开始 current db 就指向 sdb
    a.current_database = "sdb";
    CHECK_OK(a.execute("CREATE TABLE t(id INT PRIMARY KEY)"));
    CHECK_OK(a.execute("INSERT INTO t VALUES (1)"));
    {
        auto r = a.execute("SELECT COUNT(*) FROM t");
        CHECK_OK(r);
        CHECK(r.rows[0][0].v_int == 1);
        auto d = a.execute("SELECT DATABASE()");
        CHECK_OK(d);
        CHECK(d.rows[0][0].v_str == "sdb");
    }
    a.execute("DROP TABLE sdb.t");
    a.execute("DROP DATABASE sdb");
    printf("  会话初始 db 生效 ..... PASS\n");
    return 0;
}

// ---------- #8: 可以删除当前连接所在的库 ----------
int test_drop_current_database() {
    core::Session a;
    CHECK_OK(a.execute("CREATE DATABASE dcur"));
    CHECK_OK(a.execute("USE dcur"));
    CHECK_OK(a.execute("CREATE TABLE t(id INT PRIMARY KEY)"));

    // MySQL 语义:允许删除当前连接所在的库
    CHECK_OK(a.execute("DROP DATABASE dcur"));
    // 删除后引用该库的会话查询 → table doesn't exist
    {
        auto r = a.execute("SELECT * FROM t");
        CHECK_FAIL(r, 1146);
    }
    printf("  DROP 当前库 ..... PASS\n");
    return 0;
}

// ---------- DESCRIBE / SHOW FULL TABLES (P1 1.2) ----------
int test_describe_and_full_tables() {
    core::Session s;
    CHECK_OK(s.execute("CREATE TABLE dt(id BIGINT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(30), score DECIMAL(6,2))"));
    CHECK_OK(s.execute("INSERT INTO dt(name, score) VALUES ('a', 1.5)"));

    // DESCRIBE
    {
        auto r = s.execute("DESCRIBE dt");
        CHECK_OK(r);
        CHECK(r.column_names.size() == 6);
        CHECK(r.column_names[0] == "Field" && r.column_names[1] == "Type");
        CHECK(r.rows.size() == 3);
        CHECK(r.rows[0][0].v_str == "id");
        CHECK(r.rows[0][1].v_str == "bigint");
        CHECK(r.rows[0][2].v_str == "NO");       // NOT NULL(主键)
        CHECK(r.rows[0][3].v_str == "PRI");
        CHECK(r.rows[0][5].v_str == "auto_increment");
        CHECK(r.rows[1][1].v_str == "varchar(30)");
        CHECK(r.rows[2][1].v_str == "decimal(6,2)");
    }
    // DESC 简写
    {
        auto r = s.execute("DESC dt");
        CHECK_OK(r);
        CHECK(r.rows.size() == 3);
    }
    // DESCRIBE db.tbl
    {
        auto r = s.execute("DESCRIBE default.dt");
        CHECK_OK(r);
        CHECK(r.rows.size() == 3);
    }

    // SHOW TABLES(旧格式,单列)
    {
        auto r = s.execute("SHOW TABLES");
        CHECK_OK(r);
        CHECK(r.column_names.size() == 1);
        bool found = false;
        for (auto& row : r.rows) if (row[0].v_str == "dt") found = true;
        CHECK(found);
    }
    // SHOW FULL TABLES(两列,含 table_type)
    {
        auto r = s.execute("SHOW FULL TABLES");
        CHECK_OK(r);
        CHECK(r.column_names.size() == 2);
        CHECK(r.column_names[1] == "Table_type");
        CHECK(r.rows.size() >= 1);
        for (auto& row : r.rows) CHECK(row[1].v_str == "BASE TABLE");
    }
    // SHOW FULL TABLES FROM default
    {
        auto r = s.execute("SHOW FULL TABLES FROM default");
        CHECK_OK(r);
        CHECK(r.column_names[0] == "Tables_in_default");
    }
    // SHOW TABLES FROM <不存在的库> → 空结果不 crash
    {
        auto r = s.execute("SHOW TABLES FROM no_such_db");
        CHECK_OK(r);
        CHECK(r.rows.empty());
    }

    s.execute("DROP TABLE dt");
    printf("  DESCRIBE / SHOW FULL TABLES ... PASS\n");
    return 0;
}

int main() {
    printf("=== smoke_m6_session_db ===\n");
    int rc = 0;
    rc |= test_per_session_db();
    rc |= test_session_start_db();
    rc |= test_drop_current_database();
    rc |= test_describe_and_full_tables();
    if (rc == 0) printf("ALL PASS\n");
    return rc;
}
