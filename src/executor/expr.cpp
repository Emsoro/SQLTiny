#include "executor/executor.h"
#include "core/session.h"
#include <fmt/format.h>
#include <algorithm>
#include <cmath>
#include <regex>
#include <sstream>
#include <ctime>

namespace sqltiny::executor {

// ========== 表达式求值 ==========

bool to_bool(const types::Value& v) {
    if (v.is_null()) return false;
    if (v.is_int()) return v.v_int != 0;
    if (v.is_double()) return v.v_double != 0.0;
    if (v.kind == types::Value::Kind::Bool) return v.v_bool;
    return !v.to_string().empty();
}

static types::Value make_err() { return types::Value::make_null(); }

types::Value Executor::eval(const ast::Expression& e, const ExecContext& ctx) const {
    switch (e.kind) {
        case ast::ExprKind::Literal:
            return e.literal;

        case ast::ExprKind::Column: {
            // @@系统变量 拦截
            if (e.column_name.size() >= 2 && e.column_name[0] == '@' && e.column_name[1] == '@') {
                // 事务相关变量必须反映真实会话状态(之前是硬编码 REPEATABLE-READ / ON)
                if (e.column_name == "@@tx_isolation" ||
                    e.column_name == "@@session.tx_isolation" ||
                    e.column_name == "@@session_tx_isolation" ||
                    e.column_name == "@@transaction_isolation" ||
                    e.column_name == "@@session.transaction_isolation") {
                    return types::Value::make_string(
                        session_ ? session_->isolationName() : std::string("READ-COMMITTED"));
                }
                if (e.column_name == "@@autocommit" ||
                    e.column_name == "@@session.autocommit" ||
                    e.column_name == "@@session_autocommit") {
                    return types::Value::make_string(
                        (session_ && !session_->autocommit) ? "OFF" : "ON");
                }
                static const std::unordered_map<std::string, std::string> vars = {
                    {"@@tx_isolation","REPEATABLE-READ"},{"@@session_tx_isolation","REPEATABLE-READ"},
                    {"@@autocommit","ON"},{"@@session_autocommit","ON"},
                    {"@@character_set_client","utf8mb4"},{"@@character_set_connection","utf8mb4"},
                    {"@@character_set_server","utf8mb4"},{"@@character_set_database","utf8mb4"},
                    {"@@character_set_results","utf8mb4"},{"@@collation_server","utf8mb4_general_ci"},
                    {"@@collation_database","utf8mb4_general_ci"},
                    {"@@version","5.7.0-SQLTiny"},{"@@version_comment","SQLTiny"},
                    {"@@max_connections","151"},{"@@wait_timeout","28800"},
                    {"@@interactive_timeout","28800"},
                    {"@@max_allowed_packet","4194304"},{"@@net_write_timeout","60"},
                    {"@@net_read_timeout","30"},{"@@sql_mode",""},
                    {"@@lower_case_table_names","1"},{"@@sql_auto_is_null","0"},
                };
                auto it = vars.find(e.column_name);
                if (it != vars.end()) return types::Value::make_string(it->second);
                return types::Value::make_null();
            }
            // 优先 alias.col, 其次裸 col
            std::string key = e.table_name.empty() ? e.column_name : (e.table_name + "." + e.column_name);
            auto it = ctx.col_index.find(key);
            if (it == ctx.col_index.end()) {
                // 裸列尝试所有表
                for (auto& [k, idx] : ctx.col_index) {
                    // k 可能 "users.id" 或 "id";取点号后的部分
                    auto dot = k.find('.');
                    std::string bare = (dot == std::string::npos) ? k : k.substr(dot + 1);
                    if (_stricmp(bare.c_str(), e.column_name.c_str()) == 0) {
                        if (it == ctx.col_index.end() ||
                            ctx.col_index.find(e.column_name) == ctx.col_index.end()) {
                            it = ctx.col_index.find(k);
                        }
                    }
                }
            }
            if (it != ctx.col_index.end() && it->second >= 0 &&
                it->second < static_cast<int>(ctx.current_row.size())) {
                return ctx.current_row[it->second];
            }
            return types::Value::make_null();
        }

        case ast::ExprKind::Star:
            // SELECT * 处理在 executor 外层,这里不应直接调用
            return types::Value::make_null();

        case ast::ExprKind::UnaryOp: {
            auto v = eval(*e.operand, ctx);
            switch (e.unary) {
                case ast::Expression::UnaryKind::Negate:
                    if (v.is_int()) return types::Value::make_int(-v.v_int);
                    if (v.is_double()) return types::Value::make_double(-v.v_double);
                    return types::Value::make_null();
                case ast::Expression::UnaryKind::Not:
                    return types::Value::make_bool(!to_bool(v));
                case ast::Expression::UnaryKind::IsNull:
                    return types::Value::make_bool(v.is_null());
                case ast::Expression::UnaryKind::IsNotNull:
                    return types::Value::make_bool(!v.is_null());
            }
            return v;
        }

        case ast::ExprKind::BinaryOp: {
            auto l = eval(*e.lhs, ctx);
            // AND / OR 短路
            if (e.binop == ast::BinOp::And) {
                if (!to_bool(l)) return types::Value::make_bool(false);
                auto r = eval(*e.rhs, ctx);
                return types::Value::make_bool(to_bool(r));
            }
            if (e.binop == ast::BinOp::Or) {
                if (to_bool(l)) return types::Value::make_bool(true);
                auto r = eval(*e.rhs, ctx);
                return types::Value::make_bool(to_bool(r));
            }
            // IN 表达式会被 parser 转成 __in function call
            if (e.binop == ast::BinOp::Concat) {
                auto r = eval(*e.rhs, ctx);
                std::string out = l.to_string();
                auto rstr = r.to_string();
                // strip quotes for 字符串拼接
                if (out.size() >= 2 && out.front() == '\'' && out.back() == '\'') out = out.substr(1, out.size()-2);
                if (rstr.size() >= 2 && rstr.front() == '\'' && rstr.back() == '\'') rstr = rstr.substr(1, rstr.size()-2);
                return types::Value::make_string(out + rstr);
            }

            auto r = eval(*e.rhs, ctx);

            // 算术
            if (e.binop == ast::BinOp::Plus) {
                if (l.is_int() && r.is_int()) return types::Value::make_int(l.v_int + r.v_int);
                if (l.is_numeric() && r.is_numeric()) {
                    double a, b; l.try_to_double(a); r.try_to_double(b);
                    return types::Value::make_double(a + b);
                }
            }
            if (e.binop == ast::BinOp::Minus) {
                if (l.is_int() && r.is_int()) return types::Value::make_int(l.v_int - r.v_int);
                if (l.is_numeric() && r.is_numeric()) {
                    double a, b; l.try_to_double(a); r.try_to_double(b);
                    return types::Value::make_double(a - b);
                }
            }
            if (e.binop == ast::BinOp::Star) {
                if (l.is_int() && r.is_int()) return types::Value::make_int(l.v_int * r.v_int);
                if (l.is_numeric() && r.is_numeric()) {
                    double a, b; l.try_to_double(a); r.try_to_double(b);
                    return types::Value::make_double(a * b);
                }
            }
            if (e.binop == ast::BinOp::Slash) {
                double a, b; l.try_to_double(a); r.try_to_double(b);
                if (b == 0) return types::Value::make_null();
                return types::Value::make_double(a / b);
            }
            if (e.binop == ast::BinOp::Percent) {
                if (l.is_int() && r.is_int() && r.v_int != 0)
                    return types::Value::make_int(l.v_int % r.v_int);
                double a, b; l.try_to_double(a); r.try_to_double(b);
                if (b == 0) return types::Value::make_null();
                return types::Value::make_double(std::fmod(a, b));
            }

            // 比较
            if (e.binop == ast::BinOp::Eq)   return types::Value::make_bool(l.sql_equals(r));
            if (e.binop == ast::BinOp::Ne)   return types::Value::make_bool(!l.is_null() && !r.is_null() && l.compare(r) != 0);
            if (e.binop == ast::BinOp::Lt)   return types::Value::make_bool(!l.is_null() && !r.is_null() && l.compare(r) <  0);
            if (e.binop == ast::BinOp::Gt)   return types::Value::make_bool(!l.is_null() && !r.is_null() && l.compare(r) >  0);
            if (e.binop == ast::BinOp::Le)   return types::Value::make_bool(!l.is_null() && !r.is_null() && l.compare(r) <= 0);
            if (e.binop == ast::BinOp::Ge)   return types::Value::make_bool(!l.is_null() && !r.is_null() && l.compare(r) >= 0);

            return types::Value::make_null();
        }

        case ast::ExprKind::FunctionCall:
            return eval_function(e.func_name, e.func_args, e.func_star_arg, ctx);

        case ast::ExprKind::Subquery: {
            if (!ctx.exec || !e.subquery_stmt) return types::Value::make_null();
            auto result = ctx.exec->execute(*e.subquery_stmt);
            if (result.rows.empty() || result.rows[0].empty()) return types::Value::make_null();
            return result.rows[0][0];   // 标量子查询:取第一行第一列
        }

        case ast::ExprKind::Case: {
            if (e.case_base) {
                // 简单 CASE:CASE <base> WHEN <v> THEN <r>
                auto bv = eval(*e.case_base, ctx);
                for (auto& [w, r] : e.case_whens) {
                    auto wv = eval(*w, ctx);
                    if (!bv.is_null() && !wv.is_null() && bv.compare(wv) == 0)
                        return eval(*r, ctx);
                }
            } else {
                // 搜索 CASE:CASE WHEN <cond> THEN <r>
                for (auto& [c, r] : e.case_whens) {
                    if (to_bool(eval(*c, ctx))) return eval(*r, ctx);
                }
            }
            return e.case_else ? eval(*e.case_else, ctx) : types::Value::make_null();
        }

        case ast::ExprKind::Placeholder:
            return types::Value::make_null();
    }
    return types::Value::make_null();
}

types::Value Executor::eval_like(const types::Value& v, const types::Value& pattern) const {
    if (v.is_null() || pattern.is_null()) return types::Value::make_null();
    std::string s = v.is_string() ? v.v_str : v.to_string();
    // 去掉引号
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') s = s.substr(1, s.size()-2);
    std::string p = pattern.is_string() ? pattern.v_str : pattern.to_string();
    if (p.size() >= 2 && p.front() == '\'' && p.back() == '\'') p = p.substr(1, p.size()-2);

    // 转成简易通配:% 匹配任意,_ 匹配单字符,暂不支持转义
    std::string re; re.reserve(p.size() + 2);
    re += '^';
    for (char c : p) {
        if (c == '%') re += ".*";
        else if (c == '_') re += '.';
        else if (c == '.' || c == '\\' || c == '*' || c == '+' || c == '?' || c == '(' || c == ')')
            re += std::string("\\") + c;
        else re += c;
    }
    re += '$';
    try {
        std::regex rx(re);
        return types::Value::make_bool(std::regex_match(s, rx));
    } catch (...) {
        return types::Value::make_bool(false);
    }
}

types::Value Executor::eval_function(
        const std::string& name, const std::vector<ast::ExprPtr>& args,
        bool star, const ExecContext& ctx) const {
    std::string lower; lower.resize(name.size());
    std::transform(name.begin(), name.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // 聚合:交给外层 exec_select 处理,这里先返回一个占位
    if (lower == "count") {
        if (star) {
            // COUNT(*):当前表行数
            if (ctx.primary_table.empty()) return types::Value::make_int(0);
            storage::Table* tbl = nullptr;
            std::unique_ptr<storage::Table> vtbl;
            if (ctx.exec && Executor::is_information_schema_name(ctx.primary_table)) {
                // information_schema 虚拟表
                auto dot = ctx.primary_table.find('.');
                vtbl = ctx.exec->build_information_schema_table(
                    ctx.primary_table.substr(0, dot), ctx.primary_table.substr(dot + 1));
                tbl = vtbl.get();
            } else {
                tbl = ctx.catalog.get_table(ctx.primary_table, default_db());
            }
            if (!tbl) return types::Value::make_int(0);
            return types::Value::make_int(static_cast<int64_t>(tbl->row_count()));
        }
        if (!args.empty()) {
            auto v = eval(*args[0], ctx);
            if (v.is_null()) return types::Value::make_int(0);
            return types::Value::make_int(1);  // 简单:有值就算 1
        }
    }

    // __in / __like 由 parser 转过来
    if (lower == "__in") {
        if (args.size() < 2) return types::Value::make_bool(false);
        auto left = eval(*args[0], ctx);
        // 右值是 SELECT 子查询:取所有行当候选值
        if (args[1] && args[1]->kind == ast::ExprKind::Subquery && args[1]->subquery_stmt) {
            if (!ctx.exec) return types::Value::make_bool(false);
            auto result = ctx.exec->execute(*args[1]->subquery_stmt);
            for (auto& row : result.rows) {
                if (row.empty()) continue;
                if (!left.is_null() && !row[0].is_null() && left.compare(row[0]) == 0)
                    return types::Value::make_bool(true);
            }
            return types::Value::make_bool(false);
        }
        for (size_t i = 1; i < args.size(); ++i) {
            auto v = eval(*args[i], ctx);
            if (!left.is_null() && !v.is_null() && left.compare(v) == 0)
                return types::Value::make_bool(true);
        }
        return types::Value::make_bool(false);
    }
    if (lower == "__like") {
        if (args.size() < 2) return types::Value::make_null();
        auto v = eval(*args[0], ctx);
        auto pat = eval(*args[1], ctx);
        return eval_like(v, pat);
    }

    // 普通函数
    if (lower == "version") {
        return types::Value::make_string("5.7.0-SQLTiny");
    }
    if (lower == "user" || lower == "current_user") {
        return types::Value::make_string("root@localhost");
    }
    if (lower == "database" || lower == "schema") {
        // M6 P1: current db 是 per-session 状态 (#7)
        return types::Value::make_string(session_ ? session_->current_database
                                                  : ctx.catalog.current_database());
    }
    if (lower == "now" || lower == "current_timestamp") {
        std::time_t t = std::time(nullptr);
        std::tm tm{}; localtime_s(&tm, &t);
        types::DateTimeValue dt;
        dt.year = tm.tm_year + 1900;
        dt.month = tm.tm_mon + 1;
        dt.day = tm.tm_mday;
        dt.hour = tm.tm_hour;
        dt.minute = tm.tm_min;
        dt.second = tm.tm_sec;
        return types::Value::make_datetime(dt);
    }
    if (lower == "length" || lower == "char_length") {
        if (args.empty()) return types::Value::make_int(0);
        auto v = eval(*args[0], ctx);
        if (v.is_null()) return types::Value::make_int(0);
        auto s = v.is_string() ? v.v_str : v.to_string();
        if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') s = s.substr(1, s.size()-2);
        return types::Value::make_int(static_cast<int64_t>(s.size()));
    }
    if (lower == "concat") {
        std::string out;
        for (auto& a : args) {
            auto v = eval(*a, ctx);
            if (v.is_null()) return types::Value::make_null();
            auto s = v.is_string() ? v.v_str : v.to_string();
            if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') s = s.substr(1, s.size()-2);
            out += s;
        }
        return types::Value::make_string(out);
    }
    if (lower == "abs") {
        if (args.empty()) return types::Value::make_null();
        auto v = eval(*args[0], ctx);
        if (v.is_int()) return types::Value::make_int(std::abs(v.v_int));
        if (v.is_double()) return types::Value::make_double(std::fabs(v.v_double));
        return types::Value::make_null();
    }
    if (lower == "round") {
        if (args.empty()) return types::Value::make_null();
        auto v = eval(*args[0], ctx);
        int prec = 0;
        if (args.size() >= 2) {
            auto p = eval(*args[1], ctx);
            if (p.is_int()) prec = static_cast<int>(p.v_int);
        }
        double d; v.try_to_double(d);
        double factor = std::pow(10.0, prec);
        return types::Value::make_double(std::round(d * factor) / factor);
    }
    if (lower == "coalesce") {
        for (auto& a : args) {
            auto v = eval(*a, ctx);
            if (!v.is_null()) return v;
        }
        return types::Value::make_null();
    }
    if (lower == "ifnull") {
        if (args.empty()) return types::Value::make_null();
        auto v = eval(*args[0], ctx);
        if (!v.is_null()) return v;
        if (args.size() >= 2) return eval(*args[1], ctx);
        return types::Value::make_null();
    }
    if (lower == "database" || lower == "schema") {
        return types::Value::make_string("default");
    }
    if (lower == "user") {
        return types::Value::make_string("root@localhost");
    }
    // 未知函数 —— NULL
    return types::Value::make_null();
}

} // namespace sqltiny::executor
