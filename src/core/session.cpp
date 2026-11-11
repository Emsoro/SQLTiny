#include "core/session.h"
#include <fmt/format.h>

namespace sqltiny::core {

Database& Database::instance() {
    static Database db;
    return db;
}

Session::Session() = default;

executor::ResultSet Session::execute(const std::string& sql) {
    try {
        parser::Parser p(sql);
        auto stmts = p.parse_all();
        executor::ResultSet last;
        for (auto& s : stmts) {
            last = exec_.execute(*s);
            if (!last.ok) return last;
            // 处理 USE
            if (s->kind == ast::StmtKind::Use) {
                current_database = s->use_->database_name;
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
