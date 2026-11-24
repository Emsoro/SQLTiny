#include "executor/executor.h"
#include "core/session.h"
#include <fmt/format.h>
#include <algorithm>
#include <cmath>
#include <regex>
#include <sstream>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <set>
#include <unordered_set>
#include <algorithm>

namespace sqltiny::executor {

// ========== 表达式求值 ==========

bool to_bool(const types::Value& v) {
    if (v.is_null()) return false;
    if (v.is_int()) return v.v_int != 0;
    if (v.is_double()) return v.v_double != 0.0;
    if (v.kind == types::Value::Kind::Bool) return v.v_bool;
    return !v.to_string().empty();
}

int compare_mysql(const types::Value& a0, const types::Value& b0) {
    types::Value a = a0, b = b0;
    auto coerce = [](types::Value& x, const types::Value& other) {
        if (!x.is_string() || !other.is_numeric()) return;
        int64_t iv = 0; double dv = 0;
        if (x.try_to_int(iv))      x = types::Value::make_int(iv);
        else if (x.try_to_double(dv)) x = types::Value::make_double(dv);
        else                       x = types::Value::make_int(0);   // MySQL:非法字符串按 0
    };
    coerce(a, b);
    coerce(b, a);
    return a.compare(b);
}

static types::Value make_err() { return types::Value::make_null(); }

void Executor::raise_error(uint16_t errno_, const char* sql_state, std::string msg) const {
    if (!eval_error_.empty()) return;   // 保留第一个错误
    eval_errno_ = errno_;
    eval_sql_state_ = sql_state;
    eval_error_ = std::move(msg);
}

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
                    // SELECT @@autocommit 是整数 1/0;SHOW VARIABLES 才是 ON/OFF
                    return types::Value::make_string(
                        (session_ && !session_->autocommit) ? "0" : "1");
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
            // 列解析:先本层作用域,未命中沿 outer 链向上(相关子查询)。
            // 限定名 t.col 只做精确匹配 —— 不再剥掉限定符退化成本层同名列,
            // 否则子查询里 e.emp_no 会错误命中内层 de.emp_no,相关谓词退化为恒真。
            const ExecContext* c = &ctx;
            for (;;) {
                // 热路径(row_ref/idx_ref 已设置)零拷贝取行与列索引
                const std::map<std::string, int>& col_index =
                    c->idx_ref ? *c->idx_ref : c->col_index;
                const storage::Row& row = c->row_ref ? *c->row_ref : c->current_row;
                std::string key = e.table_name.empty() ? e.column_name : (e.table_name + "." + e.column_name);
                auto it = col_index.find(key);
                if (it == col_index.end()) {
                    // 大小写不敏感的全键匹配
                    for (auto& [k, idx] : col_index) {
                        if (_stricmp(k.c_str(), key.c_str()) == 0) { it = col_index.find(k); break; }
                    }
                }
                if (it == col_index.end() && e.table_name.empty()) {
                    // 裸列:尝试所有表的裸列名(取最后一个匹配,与旧行为一致)
                    for (auto& [k, idx] : col_index) {
                        auto dot = k.find('.');
                        std::string bare = (dot == std::string::npos) ? k : k.substr(dot + 1);
                        if (_stricmp(bare.c_str(), e.column_name.c_str()) == 0)
                            it = col_index.find(k);
                    }
                }
                if (it != col_index.end() && it->second >= 0 &&
                    it->second < static_cast<int>(row.size())) {
                    return row[it->second];
                }
                if (!c->outer) break;
                // 离开本层作用域 → 标记本层子查询引用了外部(相关性探测)
                if (c->outer_used) *c->outer_used = true;
                c = c->outer;
            }
            // 完全无法解析:MySQL 报 1054(之前静默 NULL,导致拼写错误被吞掉)
            raise_error(1054, "42S22",
                fmt::format("Unknown column '{}{}' in 'field list'",
                            e.table_name.empty() ? "" : (e.table_name + "."),
                            e.column_name));
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
                    // 三值逻辑: NOT UNKNOWN 仍是 UNKNOWN
                    if (v.is_null()) return types::Value::make_null();
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
            // AND / OR 三值逻辑 (TRUE / FALSE / UNKNOWN)
            if (e.binop == ast::BinOp::And || e.binop == ast::BinOp::Or) {
                bool is_and = (e.binop == ast::BinOp::And);
                // 短路: FALSE AND x = FALSE, TRUE OR x = TRUE
                if (!l.is_null() && to_bool(l) != is_and) {
                    return types::Value::make_bool(!is_and);
                }
                auto r = eval(*e.rhs, ctx);
                auto decisive = [&](const types::Value& v) {
                    return !v.is_null() && (to_bool(v) != is_and);
                };
                if (decisive(l) || decisive(r)) return types::Value::make_bool(!is_and);
                if (l.is_null() || r.is_null()) return types::Value::make_null();
                return types::Value::make_bool(is_and);
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

            // NULL 安全等值 <=>：NULL 与 NULL 相等，NULL 与非 NULL 不等。
            if (e.binop == ast::BinOp::NullSafeEq) {
                if (l.is_null() || r.is_null())
                    return types::Value::make_bool(l.is_null() && r.is_null());
                return types::Value::make_bool(l.compare(r) == 0);
            }

            // DATETIME/DATE 相减:MySQL 给出秒差(NOW()-NOW() = 0)
            if (e.binop == ast::BinOp::Minus &&
                (l.kind == types::Value::Kind::DateTime || l.kind == types::Value::Kind::Date) &&
                (r.kind == types::Value::Kind::DateTime || r.kind == types::Value::Kind::Date)) {
                auto to_epoch = [](const types::Value& v) -> int64_t {
                    std::tm tm{};
                    if (v.kind == types::Value::Kind::DateTime) {
                        tm.tm_year = v.v_dt.year - 1900; tm.tm_mon = v.v_dt.month - 1;
                        tm.tm_mday = v.v_dt.day; tm.tm_hour = v.v_dt.hour;
                        tm.tm_min = v.v_dt.minute; tm.tm_sec = v.v_dt.second;
                    } else {
                        tm.tm_year = v.v_date.year - 1900; tm.tm_mon = v.v_date.month - 1;
                        tm.tm_mday = v.v_date.day;
                    }
                    return static_cast<int64_t>(_mkgmtime(&tm));
                };
                return types::Value::make_int(to_epoch(l) - to_epoch(r));
            }

            // MySQL 隐式类型转换:数字字符串在算术/比较中按数值处理('1' = 1 → 真)。
            // 预编译语句把字符串参数绑定到整数列时依赖这一行为。
            // 算术里非数字字符串按 0 处理('abc' + 1 = 1)；比较里则视为 UNKNOWN。
            const bool is_arith = e.binop == ast::BinOp::Plus ||
                                  e.binop == ast::BinOp::Minus ||
                                  e.binop == ast::BinOp::Star ||
                                  e.binop == ast::BinOp::Slash ||
                                  e.binop == ast::BinOp::Percent;
            auto as_num = [](const types::Value& v, types::Value& out,
                             bool invalid_as_zero) -> bool {
                if (v.is_numeric()) { out = v; return true; }
                if (v.kind == types::Value::Kind::Bool) {
                    out = types::Value::make_int(v.v_bool ? 1 : 0);
                    return true;
                }
                if (v.is_string()) {
                    int64_t iv = 0; double dv = 0;
                    if (v.try_to_int(iv)) { out = types::Value::make_int(iv); return true; }
                    if (v.try_to_double(dv)) { out = types::Value::make_double(dv); return true; }
                    if (invalid_as_zero) { out = types::Value::make_int(0); return true; }
                }
                return false;
            };
            bool num_str_mismatch = false;
            if (is_arith) {
                types::Value t;
                if (l.is_string() && as_num(l, t, true)) l = t;
                if (r.is_string() && as_num(r, t, true)) r = t;
            } else if (l.is_string() && r.is_numeric()) {
                types::Value t;
                if (as_num(l, t, false)) l = t; else num_str_mismatch = true;
            } else if (r.is_string() && l.is_numeric()) {
                types::Value t;
                if (as_num(r, t, false)) r = t; else num_str_mismatch = true;
            }

            // 算术
            if (e.binop == ast::BinOp::Plus) {
                if (l.is_int() && r.is_int()) {
                    // 溢出时 MySQL 提升为 DECIMAL;用无符号幅值精确输出。
                    int64_t a = l.v_int, b = r.v_int;
                    if ((b > 0 && a > INT64_MAX - b) ||
                        (b < 0 && a < INT64_MIN - b)) {
                        uint64_t ua = static_cast<uint64_t>(a);
                        uint64_t ub = static_cast<uint64_t>(b);
                        if (a >= 0 && b >= 0)
                            return types::Value::make_string(
                                std::to_string(ua + ub));
                        uint64_t mag = (uint64_t{0} - ua) + (uint64_t{0} - ub);
                        return types::Value::make_string(
                            "-" + std::to_string(mag));
                    }
                    return types::Value::make_int(a + b);
                }
                if (l.is_numeric() && r.is_numeric()) {
                    double a, b; l.try_to_double(a); r.try_to_double(b);
                    return types::Value::make_double(a + b);
                }
            }
            if (e.binop == ast::BinOp::Minus) {
                if (l.is_int() && r.is_int()) {
                    int64_t a = l.v_int, b = r.v_int;
                    if ((b < 0 && a > INT64_MAX + b) ||
                        (b > 0 && a < INT64_MIN + b)) {
                        uint64_t ua = static_cast<uint64_t>(a);
                        uint64_t ub = static_cast<uint64_t>(b);
                        if (b < 0) {
                            uint64_t mag = ua + (uint64_t{0} - ub);
                            return types::Value::make_string(std::to_string(mag));
                        }
                        uint64_t mag = (uint64_t{0} - ua) + ub;
                        return types::Value::make_string("-" + std::to_string(mag));
                    }
                    return types::Value::make_int(a - b);
                }
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

            // 比较: 任一操作数为 NULL → UNKNOWN (三值逻辑), WHERE 层 to_bool 仍视为不匹配
            // 非数字字符串与数值比较同样视为 UNKNOWN(MySQL 语义)
            if (l.is_null() || r.is_null() || num_str_mismatch) return types::Value::make_null();
            if (e.binop == ast::BinOp::Eq)   return types::Value::make_bool(l.sql_equals(r));
            if (e.binop == ast::BinOp::Ne)   return types::Value::make_bool(l.compare(r) != 0);
            if (e.binop == ast::BinOp::Lt)   return types::Value::make_bool(l.compare(r) <  0);
            if (e.binop == ast::BinOp::Gt)   return types::Value::make_bool(l.compare(r) >  0);
            if (e.binop == ast::BinOp::Le)   return types::Value::make_bool(l.compare(r) <= 0);
            if (e.binop == ast::BinOp::Ge)   return types::Value::make_bool(l.compare(r) >= 0);

            return types::Value::make_null();
        }

        case ast::ExprKind::FunctionCall:
            return eval_function(e.func_name, e.func_args, e.func_star_arg, ctx);

        case ast::ExprKind::Subquery: {
            if (!ctx.exec || !e.subquery_stmt) return types::Value::make_null();
            if (e.subquery_exists)
                return types::Value::make_bool(ctx.exec->exists_subquery(e, ctx));
            const auto* rows = ctx.exec->subquery_rows(e, ctx);
            if (!rows) return types::Value::make_null();
            if (rows->empty() || (*rows)[0].empty()) return types::Value::make_null();
            // 标量子查询只能返回一行(MySQL 1242)
            if (rows->size() > 1) {
                raise_error(1242, "21000", "Subquery returns more than 1 row");
                return types::Value::make_null();
            }
            if ((*rows)[0].size() > 1) {
                raise_error(1241, "21000", "Operand should contain 1 column(s)");
                return types::Value::make_null();
            }
            return (*rows)[0][0];   // 标量子查询:取第一行第一列
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
        // 默认 collation(utf8mb4_general_ci)大小写不敏感:'Alice' LIKE 'a%' 应为真
        std::regex rx(re, std::regex::ECMAScript | std::regex::icase);
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
        // SQL 三值逻辑:NULL IN (...) 恒为 UNKNOWN
        if (left.is_null()) return types::Value::make_null();
        // 右值是 SELECT 子查询:非相关只物化一次(缓存)+hash 索引,相关则逐行重算
        if (args[1] && args[1]->kind == ast::ExprKind::Subquery && args[1]->subquery_stmt) {
            if (!ctx.exec) return types::Value::make_bool(false);
            bool saw_null = false, multi_col = false;
            bool found = ctx.exec->in_subquery_contains(*args[1], ctx, left, saw_null, multi_col);
            if (multi_col) {
                raise_error(1241, "21000", "Operand should contain 1 column(s)");
                return types::Value::make_null();
            }
            if (found) return types::Value::make_bool(true);
            // 未命中但候选集含 NULL → UNKNOWN
            return saw_null ? types::Value::make_null() : types::Value::make_bool(false);
        }
        bool saw_null = false;
        for (size_t i = 1; i < args.size(); ++i) {
            auto v = eval(*args[i], ctx);
            if (v.is_null()) { saw_null = true; continue; }
            if (left.compare(v) == 0) return types::Value::make_bool(true);
        }
        return saw_null ? types::Value::make_null() : types::Value::make_bool(false);
    }
    if (lower == "__like") {
        if (args.size() < 2) return types::Value::make_null();
        auto v = eval(*args[0], ctx);
        auto pat = eval(*args[1], ctx);
        return eval_like(v, pat);
    }

    // DIV 整除
    if (lower == "__intdiv") {
        if (args.size() < 2) return types::Value::make_null();
        auto a = eval(*args[0], ctx);
        auto b = eval(*args[1], ctx);
        if (a.is_null() || b.is_null()) return types::Value::make_null();
        int64_t ia = 0, ib = 0;
        if (a.try_to_int(ia) && b.try_to_int(ib)) {
            if (ib == 0) return types::Value::make_null();
            return types::Value::make_int(ia / ib);
        }
        double da = 0, db = 0;
        if (a.try_to_double(da) && b.try_to_double(db)) {
            if (db == 0) return types::Value::make_null();
            return types::Value::make_int(static_cast<int64_t>(std::trunc(da / db)));
        }
        return types::Value::make_null();
    }

    // CAST(expr AS type) / CONVERT(expr, type) —— parser 折叠为 __cast(expr, 'type')
    if (lower == "__cast") {
        if (args.size() < 2) return types::Value::make_null();
        auto v = eval(*args[0], ctx);
        if (v.is_null()) return types::Value::make_null();
        auto tv = eval(*args[1], ctx);
        std::string type = tv.is_string() ? tv.v_str : tv.to_string();
        std::transform(type.begin(), type.end(), type.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        auto raw = [](const types::Value& x) -> std::string {
            if (x.is_string()) return x.v_str;
            if (x.is_int())    return std::to_string(x.v_int);
            if (x.is_double()) return fmt::to_string(x.v_double);
            if (x.kind == types::Value::Kind::Bool) return x.v_bool ? "1" : "0";
            return x.to_string();
        };
        if (type.find("SIGNED") != std::string::npos ||
            type.find("UNSIGNED") != std::string::npos ||
            type.find("INT") != std::string::npos) {
            int64_t iv = 0;
            if (v.try_to_int(iv)) return types::Value::make_int(iv);
            if (v.is_string()) {
                // MySQL 前缀解析:'12abc' AS SIGNED = 12;完全非法 = 0
                const char* s = v.v_str.c_str();
                char* end = nullptr;
                long long x = std::strtoll(s, &end, 10);
                return types::Value::make_int(end == s ? 0 : x);
            }
            double d = 0;
            if (v.try_to_double(d)) return types::Value::make_int(static_cast<int64_t>(d));
            return types::Value::make_int(0);
        }
        if (type.find("DECIMAL") != std::string::npos ||
            type.find("NUMERIC") != std::string::npos ||
            type.find("DOUBLE") != std::string::npos ||
            type.find("FLOAT") != std::string::npos ||
            type.find("REAL") != std::string::npos) {
            double d = 0;
            if (v.try_to_double(d)) return types::Value::make_double(d);
            return types::Value::make_double(0);
        }
        if (type.find("CHAR") != std::string::npos ||
            type.find("TEXT") != std::string::npos ||
            type.find("BINARY") != std::string::npos) {
            return types::Value::make_string(raw(v));
        }
        return v;
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
    if (lower == "length" || lower == "char_length" || lower == "character_length") {
        if (args.empty()) return types::Value::make_int(0);
        auto v = eval(*args[0], ctx);
        if (v.is_null()) return types::Value::make_null();   // NULL 传播
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
        if (v.is_null()) return types::Value::make_null();
        if (v.is_int()) return types::Value::make_int(v.v_int < 0 ? -v.v_int : v.v_int);
        if (v.is_double()) return types::Value::make_double(std::fabs(v.v_double));
        // MySQL 会把数字字符串按数值处理:ABS('-3') = 3
        int64_t iv = 0;
        if (v.try_to_int(iv)) return types::Value::make_int(iv < 0 ? -iv : iv);
        double d = 0;
        if (v.try_to_double(d)) return types::Value::make_double(std::fabs(d));
        return types::Value::make_null();
    }
    if (lower == "round") {
        if (args.empty()) return types::Value::make_null();
        auto v = eval(*args[0], ctx);
        if (v.is_null()) return types::Value::make_null();   // NULL 传播
        int prec = 0;
        if (args.size() >= 2) {
            auto p = eval(*args[1], ctx);
            if (p.is_int()) prec = static_cast<int>(p.v_int);
        }
        if (prec < 0) prec = 0;
        // 定点语义:先按十进制展开,再按「四舍五入(远离零)」截断。
        // 直接 d*10^n 再 std::round 会把 1.005 的二进制误差放大成 1.00。
        double d = 0;
        v.try_to_double(d);
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%.*f", prec + 8, d);
        std::string s(buf);
        auto dot = s.find('.');
        if (dot == std::string::npos) {
            return types::Value::make_double(d);
        }
        std::string ip = s.substr(0, dot);
        std::string fp = s.substr(dot + 1);
        bool neg = false;
        if (!ip.empty() && ip[0] == '-') { neg = true; ip.erase(ip.begin()); }
        if (static_cast<int>(fp.size()) <= prec) {
            return types::Value::make_double(d);
        }
        char next = fp[prec];
        fp = fp.substr(0, prec);
        if (next >= '5') {
            // 十进制 +1 进位
            int carry = 1;
            for (int i = static_cast<int>(fp.size()) - 1; i >= 0 && carry; --i) {
                int dig = (fp[i] - '0') + carry;
                fp[i] = static_cast<char>('0' + dig % 10);
                carry = dig / 10;
            }
            for (int i = static_cast<int>(ip.size()) - 1; i >= 0 && carry; --i) {
                int dig = (ip[i] - '0') + carry;
                ip[i] = static_cast<char>('0' + dig % 10);
                carry = dig / 10;
            }
            if (carry) ip.insert(ip.begin(), '1');
        }
        std::string out = (neg ? "-" : "") + ip + (fp.empty() ? "" : "." + fp);
        try { return types::Value::make_double(std::stod(out)); }
        catch (...) { return types::Value::make_double(d); }
    }
    if (lower == "nullif") {
        // NULLIF(a,b): a=b → NULL,否则 a。任一为 NULL 时返回 a(=NULL)
        if (args.size() < 2) return types::Value::make_null();
        auto a = eval(*args[0], ctx);
        auto b = eval(*args[1], ctx);
        if (a.is_null() || b.is_null()) return a;
        return a.compare(b) == 0 ? types::Value::make_null() : a;
    }
    if (lower == "if") {
        if (args.size() < 3) return types::Value::make_null();
        return to_bool(eval(*args[0], ctx)) ? eval(*args[1], ctx) : eval(*args[2], ctx);
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
    if (lower == "instr" || lower == "locate" || lower == "position") {
        // INSTR(str, sub) / LOCATE(sub, str [, pos]) —— 子串位置, 1 起, 未找到为 0。
        if (args.size() < 2) return types::Value::make_int(0);
        auto str_of = [&](const ast::Expression& a) -> std::string {
            auto v = eval(a, ctx);
            if (v.is_null()) return {};
            std::string s = v.is_string() ? v.v_str : v.to_string();
            if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') s = s.substr(1, s.size()-2);
            return s;
        };
        auto lc = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        std::string s   = (lower == "instr") ? str_of(*args[0]) : str_of(*args[1]);
        std::string sub = (lower == "instr") ? str_of(*args[1]) : str_of(*args[0]);
        int64_t start = 1;
        if (lower == "locate" && args.size() >= 3) {
            auto sp = eval(*args[2], ctx);
            if (sp.is_int() && sp.v_int > 0) start = sp.v_int;
        }
        std::string ls = lc(s), lsub = lc(sub);
        if (start > 1 && static_cast<size_t>(start - 1) > ls.size()) return types::Value::make_int(0);
        auto pos = ls.find(lsub, static_cast<size_t>(start - 1));
        return types::Value::make_int(pos == std::string::npos ? 0 : static_cast<int64_t>(pos) + 1);
    }
    if (lower == "database" || lower == "schema") {
        return types::Value::make_string("default");
    }
    if (lower == "user") {
        return types::Value::make_string("root@localhost");
    }
    // ---- P1-a:日期时间函数模块 ----
    auto to_dt = [](const types::Value& v, types::DateTimeValue& dt) -> bool {
        if (v.is_null()) return false;
        if (v.kind == types::Value::Kind::DateTime) { dt = v.v_dt; return true; }
        if (v.kind == types::Value::Kind::Date) {
            dt.year = v.v_date.year; dt.month = v.v_date.month; dt.day = v.v_date.day;
            return true;
        }
        if (v.kind == types::Value::Kind::Time) {
            dt.hour = v.v_time.hour; dt.minute = v.v_time.minute; dt.second = v.v_time.second;
            return true;
        }
        std::string s = v.is_string() ? v.v_str : v.to_string();
        if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'')
            s = s.substr(1, s.size() - 2);
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
        int n = std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se);
        if (n >= 3) {
            dt.year = y; dt.month = mo; dt.day = d;
            dt.hour = h; dt.minute = mi; dt.second = se;
            return true;
        }
        return false;
    };
    auto epoch_of = [](const types::DateTimeValue& dt) -> int64_t {
        std::tm tm{};
        tm.tm_year = dt.year - 1900; tm.tm_mon = dt.month - 1; tm.tm_mday = dt.day;
        tm.tm_hour = dt.hour; tm.tm_min = dt.minute; tm.tm_sec = dt.second;
        return static_cast<int64_t>(_mkgmtime(&tm));
    };
    auto days_from_civil = [](int y, int m, int d) -> int64_t {
        y -= (m <= 2);
        int era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y - era * 400);
        unsigned doy = static_cast<unsigned>((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        // 719468 = 0000-03-01 到 1970-01-01 的天数;与 civil_from_days 的 +719468 互逆
        return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
    };
    auto civil_from_days = [](int64_t z, int& y, int& m, int& d) {
        z += 719468;
        int era = static_cast<int>((z >= 0 ? z : z - 146096) / 146097);
        unsigned doe = static_cast<unsigned>(z - static_cast<int64_t>(era) * 146097);
        unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        y = static_cast<int>(yoe) + era * 400;
        unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        unsigned mp = (5 * doy + 2) / 153;
        d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
        m = static_cast<int>(mp + (mp < 10 ? 3 : -9));
        y += (m <= 2);
    };
    auto is_leap = [](int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; };
    auto days_in_month = [&](int y, int m) {
        static const int md[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
        if (m < 1 || m > 12) return 30;
        return (m == 2 && is_leap(y)) ? 29 : md[m];
    };
    auto dt_arg = [&](size_t i, types::DateTimeValue& dt) -> bool {
        return i < args.size() && to_dt(eval(*args[i], ctx), dt);
    };

    if (lower == "__interval") return types::Value::make_null();  // 只作 DATE_ADD/SUB 的参数载体
    if (lower == "datediff" && args.size() >= 2) {
        types::DateTimeValue a{}, b{};
        if (!dt_arg(0, a) || !dt_arg(1, b)) return types::Value::make_null();
        return types::Value::make_int(days_from_civil(a.year, a.month, a.day) -
                                      days_from_civil(b.year, b.month, b.day));
    }
    if ((lower == "date_add" || lower == "date_sub") && args.size() >= 2) {
        types::DateTimeValue dt{};
        if (!dt_arg(0, dt)) return types::Value::make_null();
        bool is_date = args[0]->kind != ast::ExprKind::FunctionCall ||
                       (args[0]->func_name != "now" && args[0]->func_name != "current_timestamp");
        int64_t n = 0;
        std::string unit;
        if (args[1]->kind == ast::ExprKind::FunctionCall &&
            args[1]->func_name == "__interval" && args[1]->func_args.size() == 2) {
            auto nv = eval(*args[1]->func_args[0], ctx);
            nv.try_to_int(n);
            auto uv = eval(*args[1]->func_args[1], ctx);
            unit = uv.is_string() ? uv.v_str : uv.to_string();
        } else {
            auto nv = eval(*args[1], ctx);
            nv.try_to_int(n);
            if (args.size() >= 3) {
                auto uv = eval(*args[2], ctx);
                unit = uv.is_string() ? uv.v_str : uv.to_string();
            } else unit = "DAY";
        }
        std::transform(unit.begin(), unit.end(), unit.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (lower == "date_sub") n = -n;
        if (unit == "YEAR" || unit == "YEAR_MONTH") {
            dt.year = static_cast<int>(dt.year + n);
            dt.day = static_cast<decltype(dt.day)>(
                std::min<int>(dt.day, days_in_month(dt.year, dt.month)));
        } else if (unit == "MONTH" || unit == "QUARTER") {
            if (unit == "QUARTER") n *= 3;
            int64_t t = static_cast<int64_t>(dt.year) * 12 + (dt.month - 1) + n;
            dt.year = static_cast<int>(t / 12);
            dt.month = static_cast<int>(t % 12) + 1;
            if (dt.month < 1) { dt.month += 12; --dt.year; }
            dt.day = static_cast<decltype(dt.day)>(
                std::min<int>(dt.day, days_in_month(dt.year, dt.month)));
        } else if (unit == "DAY" || unit == "WEEK") {
            if (unit == "WEEK") n *= 7;
            int y, m, d;
            civil_from_days(days_from_civil(dt.year, dt.month, dt.day) + n, y, m, d);
            dt.year = y; dt.month = m; dt.day = d;
        } else {
            int64_t sec = epoch_of(dt);
            if (unit == "HOUR") sec += n * 3600;
            else if (unit == "MINUTE") sec += n * 60;
            else sec += n;
            std::time_t tt = static_cast<std::time_t>(sec);
            std::tm tm{}; gmtime_s(&tm, &tt);
            dt.year = tm.tm_year + 1900; dt.month = tm.tm_mon + 1; dt.day = tm.tm_mday;
            dt.hour = tm.tm_hour; dt.minute = tm.tm_min; dt.second = tm.tm_sec;
            is_date = false;
        }
        if (is_date) {
            types::DateValue d;
            d.year = dt.year; d.month = dt.month; d.day = dt.day;
            return types::Value::make_date(d);
        }
        return types::Value::make_datetime(dt);
    }
    if (lower == "last_day") {
        types::DateTimeValue dt{};
        if (!dt_arg(0, dt)) return types::Value::make_null();
        types::DateValue d;
        d.year = dt.year; d.month = dt.month; d.day = days_in_month(dt.year, dt.month);
        return types::Value::make_date(d);
    }
    if (lower == "dayofweek" || lower == "week") {
        types::DateTimeValue dt{};
        if (!dt_arg(0, dt)) return types::Value::make_null();
        int64_t z = days_from_civil(dt.year, dt.month, dt.day);
        if (lower == "dayofweek") return types::Value::make_int(((z + 4) % 7 + 7) % 7 + 1);
        int64_t jan1 = days_from_civil(dt.year, 1, 1);
        int jan1_dow = static_cast<int>(((jan1 + 4) % 7 + 7) % 7 + 1);
        int first_sunday = 1 + ((1 - jan1_dow + 7) % 7);   // 当年第一个周日的 day-of-year
        int doy = static_cast<int>(z - jan1) + 1;
        if (doy < first_sunday) return types::Value::make_int(0);
        return types::Value::make_int((doy - first_sunday) / 7 + 1);
    }
    if (lower == "unix_timestamp") {
        types::DateTimeValue dt{};
        if (args.empty()) {
            std::time_t t = std::time(nullptr);
            return types::Value::make_int(static_cast<int64_t>(t));
        }
        if (!dt_arg(0, dt)) return types::Value::make_null();
        return types::Value::make_int(epoch_of(dt));
    }
    if (lower == "from_unixtime" && !args.empty()) {
        auto v = eval(*args[0], ctx);
        int64_t sec = 0;
        if (!v.try_to_int(sec)) return types::Value::make_null();
        std::time_t tt = static_cast<std::time_t>(sec);
        std::tm tm{}; gmtime_s(&tm, &tt);
        return types::Value::make_string(fmt::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec));
    }
    if (lower == "date_format" && args.size() >= 2) {
        types::DateTimeValue dt{};
        if (!dt_arg(0, dt)) return types::Value::make_null();
        std::string f = [&] {
            auto v = eval(*args[1], ctx);
            return v.is_string() ? v.v_str : v.to_string();
        }();
        std::string out;
        for (size_t i = 0; i < f.size(); ++i) {
            if (f[i] != '%' || i + 1 >= f.size()) { out.push_back(f[i]); continue; }
            char c = f[++i];
            switch (c) {
                case 'Y': case 'y': out += fmt::format("{:04d}", dt.year); break;
                case 'm': out += fmt::format("{:02d}", dt.month); break;
                case 'd': out += fmt::format("{:02d}", dt.day); break;
                case 'H': out += fmt::format("{:02d}", dt.hour); break;
                case 'i': out += fmt::format("{:02d}", dt.minute); break;
                case 's': out += fmt::format("{:02d}", dt.second); break;
                case '%': out.push_back('%'); break;
                default: out.push_back('%'); out.push_back(c); break;
            }
        }
        return types::Value::make_string(out);
    }
    if (lower == "str_to_date" && args.size() >= 2) {
        auto sv = eval(*args[0], ctx), fv = eval(*args[1], ctx);
        std::string s = sv.is_string() ? sv.v_str : sv.to_string();
        std::string f = fv.is_string() ? fv.v_str : fv.to_string();
        std::string scanf_fmt;
        bool with_time = false;
        for (size_t i = 0; i < f.size(); ++i) {
            if (f[i] == '%' && i + 1 < f.size()) {
                char c = f[++i];
                if (c == 'H' || c == 'i' || c == 's') with_time = true;
                scanf_fmt += "%d";
            } else scanf_fmt.push_back(f[i]);
        }
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
        int n = with_time ? std::sscanf(s.c_str(), scanf_fmt.c_str(), &y, &mo, &d, &h, &mi, &se)
                          : std::sscanf(s.c_str(), scanf_fmt.c_str(), &y, &mo, &d);
        if (n < 3) return types::Value::make_null();
        types::DateTimeValue dt{};
        dt.year = y; dt.month = mo; dt.day = d;
        dt.hour = h; dt.minute = mi; dt.second = se;
        if (with_time) return types::Value::make_datetime(dt);
        types::DateValue dv;
        dv.year = y; dv.month = mo; dv.day = d;
        return types::Value::make_date(dv);
    }
    if (lower == "curtime" || lower == "current_time") {
        std::time_t t = std::time(nullptr);
        std::tm tm{}; localtime_s(&tm, &t);
        return types::Value::make_string(fmt::format("{:02d}:{:02d}:{:02d}",
            tm.tm_hour, tm.tm_min, tm.tm_sec));
    }

    // 日期/时间提取:YEAR / MONTH / DAY / HOUR / MINUTE / SECOND / CURDATE / DATE
    if (lower == "year" || lower == "month" || lower == "day" || lower == "dayofmonth" ||
        lower == "hour" || lower == "minute" || lower == "second" ||
        lower == "curdate" || lower == "current_date" || lower == "date") {
        types::DateTimeValue dt{};
        bool have = false;
        if (lower == "curdate" || lower == "current_date") {
            std::time_t t = std::time(nullptr);
            std::tm tm{}; localtime_s(&tm, &t);
            dt.year = tm.tm_year + 1900; dt.month = tm.tm_mon + 1; dt.day = tm.tm_mday;
            have = true;
        } else if (!args.empty()) {
            auto v = eval(*args[0], ctx);
            if (v.kind == types::Value::Kind::DateTime) { dt = v.v_dt; have = true; }
            else if (v.kind == types::Value::Kind::Date) {
                dt.year = v.v_date.year; dt.month = v.v_date.month; dt.day = v.v_date.day;
                have = true;
            } else if (v.kind == types::Value::Kind::Time) {
                dt.hour = v.v_time.hour; dt.minute = v.v_time.minute; dt.second = v.v_time.second;
                have = true;
            } else if (!v.is_null()) {
                // 字符串/数字形式 'YYYY-MM-DD [HH:MM:SS]'
                std::string s = v.is_string() ? v.v_str : v.to_string();
                if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'')
                    s = s.substr(1, s.size() - 2);
                int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
                int n = std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se);
                if (n >= 3) {
                    dt.year = y; dt.month = mo; dt.day = d;
                    dt.hour = h; dt.minute = mi; dt.second = se;
                    have = true;
                }
            }
        }
        if (!have) return types::Value::make_null();
        if (lower == "year")  return types::Value::make_int(dt.year);
        if (lower == "month") return types::Value::make_int(dt.month);
        if (lower == "day" || lower == "dayofmonth") return types::Value::make_int(dt.day);
        if (lower == "hour")   return types::Value::make_int(dt.hour);
        if (lower == "minute") return types::Value::make_int(dt.minute);
        if (lower == "second") return types::Value::make_int(dt.second);
        if (lower == "date") {
            types::DateValue d; d.year = dt.year; d.month = dt.month; d.day = dt.day;
            return types::Value::make_date(d);
        }
        // curdate / current_date
        types::DateValue d; d.year = dt.year; d.month = dt.month; d.day = dt.day;
        return types::Value::make_date(d);
    }
    // ---- 连接/会话信息函数 ----
    if (lower == "connection_id") return types::Value::make_int(1);
    if (lower == "last_insert_id") {
        // LAST_INSERT_ID(expr) 形式:设置并返回 expr
        if (!args.empty()) {
            auto v = eval(*args[0], ctx);
            int64_t iv = 0;
            if (v.try_to_int(iv) && session_) session_->setLastInsertId(iv);
            return v;
        }
        return types::Value::make_int(session_ ? session_->lastInsertId() : 0);
    }
    if (lower == "row_count")
        return types::Value::make_int(session_ ? session_->rowCount() : -1);
    if (lower == "found_rows")
        return types::Value::make_int(session_ ? session_->foundRows() : 0);
    if (lower == "session_user" || lower == "system_user" || lower == "current_role")
        return types::Value::make_string("root@localhost");

    // ---- 字符串函数 ----
    auto arg_str = [&](size_t i) -> std::string {
        if (i >= args.size()) return {};
        auto v = eval(*args[i], ctx);
        if (v.is_null()) return {};
        std::string s = v.is_string() ? v.v_str : v.to_string();
        if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') s = s.substr(1, s.size()-2);
        return s;
    };
    auto arg_is_null = [&](size_t i) -> bool {
        if (i >= args.size()) return true;
        return eval(*args[i], ctx).is_null();
    };
    if (lower == "upper" || lower == "ucase" || lower == "lower" || lower == "lcase") {
        if (args.empty()) return types::Value::make_null();
        if (arg_is_null(0)) return types::Value::make_null();
        std::string s = arg_str(0);
        std::transform(s.begin(), s.end(), s.begin(), [&](unsigned char c) {
            return static_cast<char>((lower == "upper" || lower == "ucase")
                                     ? std::toupper(c) : std::tolower(c));
        });
        return types::Value::make_string(s);
    }
    if (lower == "trim" || lower == "ltrim" || lower == "rtrim") {
        if (args.empty() || arg_is_null(0)) return types::Value::make_null();
        std::string s = arg_str(0);
        auto sp = [](unsigned char c) { return std::isspace(c) != 0; };
        if (lower != "rtrim") s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), sp));
        if (lower != "ltrim") s.erase(std::find_if_not(s.rbegin(), s.rend(), sp).base(), s.end());
        return types::Value::make_string(s);
    }
    if (lower == "substr" || lower == "substring") {
        if (args.size() < 2 || arg_is_null(0) || arg_is_null(1)) return types::Value::make_null();
        std::string s = arg_str(0);
        int64_t pos = 1, len = INT64_MAX;
        auto pv = eval(*args[1], ctx);
        if (pv.is_int()) pos = pv.v_int;
        else if (pv.is_double()) pos = static_cast<int64_t>(pv.v_double);
        if (args.size() >= 3) {
            auto lv = eval(*args[2], ctx);
            if (lv.is_int()) len = lv.v_int;
        }
        int64_t n = static_cast<int64_t>(s.size());
        if (pos < 0) pos = n + pos + 1;
        if (pos < 1) pos = 1;
        if (pos > n) return types::Value::make_string("");
        int64_t start = pos - 1;
        int64_t take = (len == INT64_MAX) ? (n - start) : std::max<int64_t>(0, len);
        return types::Value::make_string(s.substr(static_cast<size_t>(start),
                                                  static_cast<size_t>(std::min(take, n - start))));
    }
    if (lower == "left" || lower == "right") {
        if (args.size() < 2 || arg_is_null(0) || arg_is_null(1)) return types::Value::make_null();
        std::string s = arg_str(0);
        int64_t k = 0;
        auto kv = eval(*args[1], ctx);
        if (kv.is_int()) k = kv.v_int;
        if (k < 0) k = 0;
        size_t take = std::min<size_t>(static_cast<size_t>(k), s.size());
        return types::Value::make_string(lower == "left" ? s.substr(0, take)
                                                         : s.substr(s.size() - take));
    }
    if (lower == "replace") {
        if (args.size() < 3 || arg_is_null(0) || arg_is_null(1) || arg_is_null(2))
            return types::Value::make_null();
        std::string s = arg_str(0), from = arg_str(1), to = arg_str(2);
        if (from.empty()) return types::Value::make_string(s);
        std::string out;
        size_t p = 0;
        for (;;) {
            auto f = s.find(from, p);
            if (f == std::string::npos) { out += s.substr(p); break; }
            out += s.substr(p, f - p);
            out += to;
            p = f + from.size();
        }
        return types::Value::make_string(out);
    }
    if (lower == "reverse") {
        if (args.empty() || arg_is_null(0)) return types::Value::make_null();
        std::string s = arg_str(0);
        std::reverse(s.begin(), s.end());
        return types::Value::make_string(s);
    }
    if (lower == "lpad" || lower == "rpad") {
        if (args.size() < 3) return types::Value::make_null();
        std::string s = arg_str(0), pad = arg_str(2);
        int64_t k = 0;
        auto kv = eval(*args[1], ctx);
        if (kv.is_int()) k = kv.v_int;
        if (k < 0) k = 0;
        if (pad.empty()) return types::Value::make_string(s);
        std::string fill;
        while (static_cast<int64_t>(fill.size()) < k) fill += pad;
        fill.resize(static_cast<size_t>(k));
        if (static_cast<int64_t>(s.size()) >= k) s.resize(static_cast<size_t>(k));
        else if (lower == "lpad") s = fill.substr(0, static_cast<size_t>(k) - s.size()) + s;
        else s += fill.substr(0, static_cast<size_t>(k) - s.size());
        return types::Value::make_string(s);
    }
    if (lower == "repeat") {
        if (args.size() < 2 || arg_is_null(0) || arg_is_null(1)) return types::Value::make_null();
        std::string s = arg_str(0);
        int64_t k = 0;
        auto kv = eval(*args[1], ctx);
        if (kv.is_int()) k = kv.v_int;
        std::string out;
        for (int64_t i = 0; i < k; ++i) out += s;
        return types::Value::make_string(out);
    }
    if (lower == "concat_ws") {
        if (args.size() < 2) return types::Value::make_null();
        std::string sep = arg_str(0), out;
        for (size_t i = 1; i < args.size(); ++i) {
            if (arg_is_null(i)) continue;
            if (!out.empty() || i > 1) out += sep;
            out += arg_str(i);
        }
        return types::Value::make_string(out);
    }
    if (lower == "ascii" || lower == "ord") {
        if (args.empty() || arg_is_null(0)) return types::Value::make_null();
        std::string s = arg_str(0);
        return types::Value::make_int(s.empty() ? 0 : static_cast<int64_t>(static_cast<unsigned char>(s[0])));
    }
    if (lower == "char") {
        std::string out;
        for (size_t i = 0; i < args.size(); ++i) {
            auto v = eval(*args[i], ctx);
            int64_t c = 0;
            if (!v.try_to_int(c)) continue;
            out.push_back(static_cast<char>(c & 0xff));
        }
        return types::Value::make_string(out);
    }

    // ---- 数值函数 ----
    auto arg_num = [&](size_t i, bool& ok) -> double {
        ok = false;
        if (i >= args.size()) return 0;
        auto v = eval(*args[i], ctx);
        double d = 0;
        if (v.try_to_double(d)) { ok = true; return d; }
        return 0;
    };
    if (lower == "ceil" || lower == "ceiling" || lower == "floor") {
        if (args.empty() || arg_is_null(0)) return types::Value::make_null();
        bool ok = false; double d = arg_num(0, ok);
        if (!ok) return types::Value::make_null();
        return types::Value::make_int(static_cast<int64_t>(
            (lower == "floor") ? std::floor(d) : std::ceil(d)));
    }
    if (lower == "mod") {
        if (args.size() < 2 || arg_is_null(0) || arg_is_null(1)) return types::Value::make_null();
        auto a = eval(*args[0], ctx), b = eval(*args[1], ctx);
        if (a.is_int() && b.is_int()) {
            if (b.v_int == 0) return types::Value::make_null();
            return types::Value::make_int(a.v_int % b.v_int);
        }
        bool oka = false, okb = false;
        double da = arg_num(0, oka), db = arg_num(1, okb);
        if (!oka || !okb || db == 0) return types::Value::make_null();
        return types::Value::make_double(std::fmod(da, db));
    }
    if (lower == "power" || lower == "pow") {
        if (args.size() < 2 || arg_is_null(0) || arg_is_null(1)) return types::Value::make_null();
        bool oka = false, okb = false;
        double da = arg_num(0, oka), db = arg_num(1, okb);
        if (!oka || !okb) return types::Value::make_null();
        return types::Value::make_double(std::pow(da, db));
    }
    if (lower == "sqrt") {
        if (args.empty() || arg_is_null(0)) return types::Value::make_null();
        bool ok = false; double d = arg_num(0, ok);
        if (!ok || d < 0) return types::Value::make_null();
        return types::Value::make_double(std::sqrt(d));
    }
    if (lower == "sign") {
        if (args.empty() || arg_is_null(0)) return types::Value::make_null();
        bool ok = false; double d = arg_num(0, ok);
        if (!ok) return types::Value::make_null();
        return types::Value::make_int(d > 0 ? 1 : (d < 0 ? -1 : 0));
    }
    if (lower == "truncate") {
        if (args.size() < 2 || arg_is_null(0) || arg_is_null(1)) return types::Value::make_null();
        bool ok = false; double d = arg_num(0, ok);
        int64_t prec = 0;
        auto pv = eval(*args[1], ctx);
        if (pv.is_int()) prec = pv.v_int;
        if (!ok) return types::Value::make_null();
        double factor = std::pow(10.0, static_cast<double>(prec));
        return types::Value::make_double(std::trunc(d * factor) / factor);
    }
    if (lower == "greatest" || lower == "least") {
        if (args.empty()) return types::Value::make_null();
        types::Value best;
        bool first = true;
        for (size_t i = 0; i < args.size(); ++i) {
            auto v = eval(*args[i], ctx);
            if (v.is_null()) return types::Value::make_null();
            if (first) { best = v; first = false; continue; }
            int c = v.compare(best);
            if ((lower == "greatest" && c > 0) || (lower == "least" && c < 0)) best = v;
        }
        return best;
    }

    // 已知但尚未实现的 MySQL 函数:保持历史行为(返回 NULL),避免误报 1305
    static const std::set<std::string> kKnownUnimplemented = {
        "row_number", "rank", "dense_rank", "percent_rank", "cume_dist", "ntile",
        "lag", "lead", "first_value", "last_value", "nth_value",
        "sum", "avg", "min", "max", "count", "group_concat",
        "date_add", "date_sub", "adddate", "subdate", "datediff", "timediff",
        "last_day", "dayofweek", "dayofyear", "week", "weekday", "quarter",
        "unix_timestamp", "from_unixtime", "date_format", "str_to_date",
        "curtime", "current_time", "localtime", "utc_date", "utc_time",
        "timestampdiff", "timestampadd", "makedate", "period_add",
        "cast", "convert", "binary", "json_extract", "json_unquote",
    };
    if (kKnownUnimplemented.count(lower)) return types::Value::make_null();

    // 真正未知的函数:MySQL 报 1305,而不是静默 NULL
    raise_error(1305, "42000", fmt::format("FUNCTION {} does not exist", name));
    return types::Value::make_null();
}

} // namespace sqltiny::executor
