#include "core/session.h"
#include <fmt/format.h>
#include <cstdio>
#include <string>
#include <mutex>

namespace sqltiny::core {

// 全局执行锁:server 多线程时串行化 SQL 执行
// (Catalog/Table/Wal 目前不是线程安全的,M6 才细粒度锁)
static std::mutex g_execute_mutex;

Database& Database::instance() {
    static Database db;
    return db;
}

bool Database::open(const std::string& data_dir) {
    if (data_dir.empty()) {
        fmt::print("[SQLTiny] data-dir not set — running in memory-only mode (no persistence)\n");
        wal_path_.clear();
        return true;
    }
    // 确保目录存在
#ifdef _WIN32
    std::string sys_cmd = "mkdir \"" + data_dir + "\" 2>NUL";
    (void)std::system(sys_cmd.c_str());
#else
    std::string sys_cmd = "mkdir -p \"" + data_dir + "\"";
    (void)std::system(sys_cmd.c_str());
#endif
    wal_path_ = data_dir + "/sql.wal";
    return wal_.open(wal_path_, catalog_);
}

void Database::shutdown() {
    // 优雅关闭前做 checkpoint:写 snapshot + truncate WAL
    if (wal_.enabled()) {
        fmt::print("[SQLTiny] shutting down — checkpointing...\n");
        wal_.do_checkpoint(catalog_);
    }
    wal_.close();
}

Session::Session() = default;

executor::ResultSet Session::execute(const std::string& sql) {
    std::lock_guard<std::mutex> lock(g_execute_mutex);
    try {
        parser::Parser p(sql);
        auto stmts = p.parse_all();
        executor::ResultSet last;
        for (auto& s : stmts) {
            last = exec_.execute(*s);
            if (!last.ok) return last;
            // 处理 USE:必须通过 catalog 切换,同时更新 Session 字段
            if (s->kind == ast::StmtKind::Use) {
                auto& name = s->use_->database_name;
                if (!exec_.catalog_ref().switch_database(name)) {
                    executor::ResultSet u;
                    u.ok = false; u.mysql_errno = 1049; u.sql_state = "42000";
                    u.error_message = fmt::format("Unknown database '{}'", name);
                    return u;
                }
                current_database = name;
                // WAL 记录 USE
                Database::instance().wal().append_use_database(name);
                executor::ResultSet u;
                u.ok = true; u.affected_rows = 0;
                return u;
            }
        }
        return last;
    } catch (const parser::ParseError& e) {
        executor::ResultSet r;
        r.ok = false;
        r.mysql_errno = 1064;
        r.sql_state = "42000";
        r.error_message = fmt::format("You have an error in your SQL syntax: {}", e.what());
        return r;
    } catch (const std::exception& e) {
        executor::ResultSet r;
        r.ok = false;
        r.mysql_errno = 1105;
        r.sql_state = "HY000";
        r.error_message = e.what();
        return r;
    }
}

} // namespace sqltiny::core
