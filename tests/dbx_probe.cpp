// dbx_probe: 模拟 DBX/dbExpress 类客户端在"连接测试"和"进入界面(元数据浏览)"
// 阶段发出的典型 SQL,逐条跑过 Session::execute,找出不支持的语句。
#include <cstdio>
#include <string>
#include <vector>
#include "core/session.h"

using namespace sqltiny;

struct Case {
    const char* phase;   // connect / metadata / txn
    const char* sql;
};

static const std::vector<Case> CASES = {
    // ---------- 连接测试阶段 ----------
    {"connect", "SELECT 1"},
    {"connect", "SELECT VERSION()"},
    {"connect", "SELECT @@version"},
    {"connect", "SELECT @@version_comment LIMIT 1"},
    {"connect", "SELECT @@session.auto_increment_increment"},
    {"connect", "SELECT @@session.character_set_client"},
    {"connect", "SELECT @@tx_isolation"},
    {"connect", "SELECT @@autocommit"},
    {"connect", "SET NAMES utf8mb4"},
    {"connect", "SET NAMES 'utf8'"},
    {"connect", "SET NAMES utf8 COLLATE utf8_general_ci"},
    {"connect", "SET character_set_results = NULL"},
    {"connect", "SET SQL_AUTO_IS_NULL = 0"},
    {"connect", "SET autocommit=1"},
    {"connect", "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ"},
    {"connect", "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED"},
    {"connect", "SHOW VARIABLES LIKE 'character%'"},
    {"connect", "SHOW VARIABLES LIKE 'character_set%'"},
    {"connect", "SHOW VARIABLES"},
    {"connect", "SHOW STATUS"},
    {"connect", "SHOW ENGINES"},
    {"connect", "SHOW PLUGINS"},
    {"connect", "SHOW CHARSET"},
    {"connect", "SHOW CHARACTER SET"},
    {"connect", "SHOW COLLATION"},
    {"connect", "SELECT DATABASE()"},
    {"connect", "SELECT USER()"},
    {"connect", "SELECT CURRENT_USER()"},
    {"connect", "SELECT CONNECTION_ID()"},
    {"connect", "SELECT 1 FROM DUAL"},
    // ---------- 进入界面 / 元数据浏览阶段 ----------
    {"metadata", "SHOW DATABASES"},
    {"metadata", "USE test"},
    {"metadata", "CREATE TABLE t(id INT PRIMARY KEY, name VARCHAR(20))"},
    {"metadata", "INSERT INTO t VALUES (1, 'a')"},
    {"metadata", "SHOW TABLES"},
    {"metadata", "SHOW FULL TABLES"},
    {"metadata", "SHOW TABLE STATUS"},
    {"metadata", "SHOW TABLE STATUS LIKE 't%'"},
    {"metadata", "SHOW COLUMNS FROM t"},
    {"metadata", "SHOW FULL COLUMNS FROM t"},
    {"metadata", "SHOW INDEX FROM t"},
    {"metadata", "SHOW KEYS FROM t"},
    {"metadata", "SHOW CREATE TABLE t"},
    {"metadata", "DESCRIBE t"},
    {"metadata", "DESC t"},
    {"metadata", "SELECT * FROM t LIMIT 1"},
    {"metadata", "SELECT * FROM t WHERE 1=0"},
    {"metadata", "SELECT COUNT(*) FROM t"},
    {"metadata", "SELECT * FROM information_schema.TABLES"},
    {"metadata", "SELECT * FROM information_schema.COLUMNS WHERE TABLE_NAME = 't'"},
    {"metadata", "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE()"},
    {"metadata", "SELECT TABLE_NAME, TABLE_TYPE FROM information_schema.TABLES WHERE TABLE_SCHEMA = 'test'"},
    // ---------- 事务相关 ----------
    {"txn", "BEGIN"},
    {"txn", "COMMIT"},
    {"txn", "START TRANSACTION"},
    {"txn", "ROLLBACK"},
    {"txn", "SET autocommit=0"},
    {"txn", "SET autocommit=1"},
};

int main() {
    printf("=== DBX client SQL probe ===\n");
    core::Session s;
    // 准备环境:建 test 库
    s.execute("CREATE DATABASE IF NOT EXISTS test");

    int pass = 0, fail = 0;
    std::vector<std::string> failed;
    for (auto& c : CASES) {
        auto r = s.execute(c.sql);
        if (r.ok) {
            printf("  [PASS] %-8s %s\n", c.phase, c.sql);
            ++pass;
        } else {
            printf("  [FAIL] %-8s %s\n         -> errno=%u state=%s msg=%s\n",
                   c.phase, c.sql, r.mysql_errno, r.sql_state.c_str(),
                   r.error_message.c_str());
            ++fail;
            failed.push_back(c.sql);
        }
    }
    printf("\n%d/%d PASS\n", pass, pass + fail);
    if (!failed.empty()) {
        printf("Unsupported / failed statements:\n");
        for (auto& f : failed) printf("  - %s\n", f.c_str());
    }
    // 清理
    s.execute("USE default");
    s.execute("DROP TABLE IF EXISTS test.t");
    return fail == 0 ? 0 : 1;
}
