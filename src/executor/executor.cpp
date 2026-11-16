#include "executor/executor.h"
#include "core/session.h"
#include <fmt/format.h>
#include <algorithm>
#include <functional>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <set>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <iterator>

namespace sqltiny::executor {

/// SHOW TABLES [LIKE 'pat' | WHERE col='pat'] 的表名过滤。
/// Catalog 内部表名是小写,客户端常传建表时的原始大小写,故大小写不敏感比较。
static bool show_table_matches(const std::string& name, std::string pattern) {
    if (pattern.empty()) return true;
    if (pattern.size() >= 2 && pattern.front() == '\'' && pattern.back() == '\'')
        pattern = pattern.substr(1, pattern.size() - 2);
    auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    std::string n = lower(name), p = lower(pattern);
    if (p == "%" || p == "%%") return true;
    if (p.find('%') == std::string::npos) return n == p;
    if (p.size() >= 2 && p.front() == '%' && p.back() == '%')
        return n.find(p.substr(1, p.size() - 2)) != std::string::npos;
    if (p.back() == '%') return n.starts_with(p.substr(0, p.size() - 1));
    if (p.front() == '%') return n.ends_with(p.substr(1));
    return n == p;
}

// ---------- M6 P2 · JOIN 优化 ----------
/// 等值 JOIN 的 hash key。数值统一提升到 double 再格式化,与 Value::compare 的
/// "数值类统一提升" 语义一致 (int 1 与 double 1.0 视为同一 key)。
static std::string join_hash_key(const types::Value& v) {
    if (v.is_null()) return {};              // NULL 不参与等值匹配 (SQL 三值逻辑)
    if (v.is_numeric()) {
        double d = 0.0;
        v.try_to_double(d);
        return "n:" + fmt::format("{:.17g}", d);
    }
    if (v.is_string()) return "s:" + v.v_str;
    if (v.kind == types::Value::Kind::Bool) return std::string("b:") + (v.v_bool ? "1" : "0");
    return "o:" + v.to_string();
}

/// 从 ON 表达式提取一个「左表列 = 右表列」的等值条件。
/// 支持 AND 连接的复合条件:取第一个等值项做 hash probe,其余谓词仍由完整 eval 兜底。
struct EquiJoinKey {
    bool valid = false;
    int  left_col  = -1;   // flat row 中的下标 (左侧已累积的 join 结果)
    int  right_col = -1;   // 右表行内的下标
    std::string left_name, right_name;
    bool right_is_pk = false;
    bool right_pk_first = false;  // join 列是(复合)主键首列 → 可用 PK AVL 前缀探测
};
static bool is_equi_pair(const ast::Expression& e) {
    return e.kind == ast::ExprKind::BinaryOp && e.binop == ast::BinOp::Eq &&
           e.lhs && e.rhs &&
           e.lhs->kind == ast::ExprKind::Column && e.rhs->kind == ast::ExprKind::Column;
}
static EquiJoinKey extract_equi(const ast::Expression& on,
                                const std::map<std::string, int>& col_index,
                                const std::set<std::string>& ambiguous_cols,
                                int right_offset, int right_cols,
                                const storage::TableDef& rdef) {
    EquiJoinKey k;
    const ast::Expression* eq = nullptr;
    std::function<const ast::Expression*(const ast::Expression&)> find_eq =
        [&](const ast::Expression& e) -> const ast::Expression* {
            if (is_equi_pair(e)) return &e;
            if (e.kind == ast::ExprKind::BinaryOp && e.binop == ast::BinOp::And &&
                e.lhs && e.rhs) {
                if (auto* a = find_eq(*e.lhs)) return a;
                return find_eq(*e.rhs);
            }
            return nullptr;
        };
    eq = find_eq(on);
    if (!eq) return k;

    auto lookup = [&](const ast::Expression& c) -> int {
        std::string qualified = c.table_name.empty() ? "" : c.table_name + "." + c.column_name;
        if (!qualified.empty()) {
            auto it = col_index.find(qualified);
            if (it != col_index.end()) return it->second;
            return -1;
        }
        // 裸列名歧义时放弃优化 (退化到嵌套循环,由 eval 决定语义)
        if (ambiguous_cols.count(c.column_name)) return -1;
        auto it = col_index.find(c.column_name);
        return it == col_index.end() ? -1 : it->second;
    };
    int a = lookup(*eq->lhs), b = lookup(*eq->rhs);
    auto in_right = [&](int off) { return off >= right_offset && off < right_offset + right_cols; };
    auto in_left  = [&](int off) { return off >= 0 && off < right_offset; };
    int lo = -1, ro = -1;
    const ast::Expression* lc = nullptr;
    const ast::Expression* rc = nullptr;
    if (in_right(a) && in_left(b))      { ro = a; lo = b; rc = eq->lhs.get(); lc = eq->rhs.get(); }
    else if (in_right(b) && in_left(a)) { ro = b; lo = a; rc = eq->rhs.get(); lc = eq->lhs.get(); }
    else return k;

    k.valid = true;
    k.left_col = lo;
    k.right_col = ro - right_offset;
    k.left_name  = lc->column_name;
    k.right_name = rc->column_name;
    // 右表 join 列是否为单列主键 (供 EXPLAIN 标注 key=PRIMARY)
    if (rdef.primary_key_cols.size() == 1 && rdef.primary_key_cols[0].size() == 1) {
        int pk_idx = rdef.column_index(rdef.primary_key_cols[0][0]);
        k.right_is_pk = (pk_idx == k.right_col);
    }
    // join 列是(复合)主键首列 → 逐左行做 PK AVL 前缀扫描,右表零物化
    if (!rdef.primary_key_cols.empty() && !rdef.primary_key_cols[0].empty()) {
        int pk0_idx = rdef.column_index(rdef.primary_key_cols[0][0]);
        if (pk0_idx == k.right_col) k.right_pk_first = true;
    }
    return k;
}

// WAL 小工具:拆 "db.table" 或 "table" → (db_or_empty, table)
static std::pair<std::string, std::string> split_qname(const std::string& q) {
    auto dot = q.find('.');
    if (dot == std::string::npos) return {"", q};
    return {q.substr(0, dot), q.substr(dot + 1)};
}

static std::string ci_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string row_dedup_key(const storage::Row& row) {
    std::string k;
    for (auto& v : row) { k += v.to_string(); k.push_back('\x01'); }
    return k;
}

// 把结果集物化成临时表(CTE / 派生表用)。显式列名优先,其次结果列名(去掉表限定)
static std::shared_ptr<storage::Table> table_from_result(
        const std::string& name, const ResultSet& rs,
        const std::vector<std::string>& col_names) {
    storage::TableDef def;
    def.name = name;
    size_t n = rs.column_names.size();
    for (auto& row : rs.rows) n = std::max(n, row.size());
    for (size_t i = 0; i < n; ++i) {
        ast::ColumnDef c;
        std::string nm = (i < col_names.size() && !col_names[i].empty()) ? col_names[i]
                       : (i < rs.column_names.size() ? rs.column_names[i]
                                                     : fmt::format("col{}", i + 1));
        auto dot = nm.rfind('.');
        if (dot != std::string::npos) nm = nm.substr(dot + 1);
        c.name = nm;
        c.type.type = i < rs.column_types.size() ? rs.column_types[i] : types::SqlType::Varchar;
        c.nullable = true;
        def.columns.push_back(std::move(c));
    }
    auto tbl = std::make_shared<storage::Table>(std::move(def));
    for (auto& row : rs.rows) {
        storage::Row r2 = row;
        r2.resize(n, types::Value::make_null());
        tbl->insert(r2);
    }
    return tbl;
}

// MVCC 读上下文:READ COMMITTED 取语句级 SCN,RR/SERIALIZABLE 取事务快照 SCN
static uint64_t query_scn(core::Session* s) {
    return s ? s->readSCN() : core::Database::instance().wal().current_scn();
}
static uint64_t reader_txn(core::Session* s) {
    return s ? s->txnId() : 0;
}
static uint64_t writer_txn(core::Session* s) {
    return (s && s->isInTransaction()) ? s->txnId() : 0;
}

std::string Executor::default_db() const {
    return session_ ? session_->current_database : catalog_.current_database();
}

ResultSet Executor::execute(const ast::Statement& stmt) {
    // 顶层语句开始时清空子查询缓存(缓存键是 AST 节点指针,跨语句会失效)
    if (exec_depth_ == 0) { sq_cache_.clear(); eval_error_.clear(); eval_errno_ = 0; }
    struct DepthGuard {
        int& d;
        explicit DepthGuard(int& x) : d(x) { ++d; }
        ~DepthGuard() { --d; }
    } guard(exec_depth_);
    ResultSet r;
    switch (stmt.kind) {
        case ast::StmtKind::Select:      return exec_select(*stmt.select_);
        case ast::StmtKind::Insert:      return exec_insert(*stmt.insert_);
        case ast::StmtKind::Update:      return exec_update(*stmt.update_);
        case ast::StmtKind::Delete:      return exec_delete(*stmt.delete_);
        case ast::StmtKind::CreateTable: return exec_create_table(*stmt.create_table_);
        case ast::StmtKind::DropTable:   return exec_drop_table(*stmt.drop_table_);
        case ast::StmtKind::CreateDatabase: return exec_create_database(*stmt.create_database_);
        case ast::StmtKind::DropDatabase:   return exec_drop_database(*stmt.drop_database_);
        case ast::StmtKind::Use: {
            // Session 层已经同步过 catalog,这里空返回
            r.ok = true;
            return r;
        }
        case ast::StmtKind::Set: {
            // 占位:兼容 PyMySQL 的 SET NAMES/SET AUTOCOMMIT
            r.ok = true;
            return r;
        }
        case ast::StmtKind::Show: {
            auto& sh = *stmt.show_;
            switch (sh.kind) {
                case ast::ShowStmt::Kind::Tables:      return exec_show_tables(sh.full, sh.table_or_pattern, sh.filter_pattern);
                case ast::ShowStmt::Kind::Columns:     return exec_show_columns(sh.db_name, sh.table_or_pattern);
                case ast::ShowStmt::Kind::Databases:   return exec_show_databases();
                case ast::ShowStmt::Kind::CreateTable: return exec_show_create_table(sh.db_name, sh.table_or_pattern);
                case ast::ShowStmt::Kind::CreateDatabase: return exec_show_create_database(sh.table_or_pattern);
                case ast::ShowStmt::Kind::Status:      return exec_show_status();
                case ast::ShowStmt::Kind::Variables:   return exec_show_variables(sh.table_or_pattern);
                case ast::ShowStmt::Kind::Engines:     return exec_show_engines();
                case ast::ShowStmt::Kind::Plugins:     return exec_show_plugins();
                case ast::ShowStmt::Kind::Charset:     return exec_show_charset();
                case ast::ShowStmt::Kind::Collation:   return exec_show_collation();
                case ast::ShowStmt::Kind::Index:       return exec_show_index(sh.db_name, sh.table_or_pattern);
                case ast::ShowStmt::Kind::TableStatus: return exec_show_table_status(sh.db_name, sh.table_or_pattern);
                case ast::ShowStmt::Kind::Triggers:    return exec_show_triggers();
                case ast::ShowStmt::Kind::ProcessList: return exec_show_processlist();
                case ast::ShowStmt::Kind::Warnings:    return exec_show_warnings();
            }
            break;
        }
    }
    return r;
}

// ---------- CREATE TABLE ----------
ResultSet Executor::exec_create_table(const ast::CreateTableStmt& s) {
    ResultSet r;
    bool ok = catalog_.create_table(s, default_db());
    if (!ok) {
        r.ok = false; r.mysql_errno = 1050; r.sql_state = "42S01";
        r.error_message = fmt::format("Table '{}' already exists", s.table_name);
    } else {
        auto [db_part, tbl_part] = split_qname(s.table_name);
        std::string db_name = db_part.empty() ? default_db() : db_part;
        auto* tbl = catalog_.get_table(s.table_name, default_db());
        if (tbl) core::Database::instance().wal().append_create_table(db_name, tbl->def());
    }
    return r;
}

// ---------- DROP TABLE ----------
ResultSet Executor::exec_drop_table(const ast::DropTableStmt& s) {
    ResultSet r;
    bool ok = catalog_.drop_table(s.table_name, s.if_exists, default_db());
    if (!ok) {
        r.ok = false; r.mysql_errno = 1051; r.sql_state = "42S02";
        r.error_message = fmt::format("Unknown table '{}'", s.table_name);
    } else {
        auto [db_part, tbl_part] = split_qname(s.table_name);
        std::string db_name = db_part.empty() ? default_db() : db_part;
        core::Database::instance().wal().append_drop_table(db_name, tbl_part);
    }
    return r;
}

// ---------- INSERT ----------
ResultSet Executor::exec_insert(const ast::InsertStmt& s) {
    ResultSet r;
    auto* tbl = catalog_.get_table(s.table_name, default_db());
    if (!tbl) {
        r.ok = false; r.mysql_errno = 1146; r.sql_state = "42S02";
        r.error_message = fmt::format("Table '{}' doesn't exist", s.table_name);
        return r;
    }
    auto& def = tbl->def();

    // 列名列表校验:未知列(1054)/ 列数与值数不匹配(1136)
    for (auto& cname : s.columns) {
        if (def.column_index(cname) < 0) {
            r.ok = false; r.mysql_errno = 1054; r.sql_state = "42S22";
            r.error_message = fmt::format("Unknown column '{}' in 'field list'", cname);
            return r;
        }
    }
    for (auto& expr_row : s.value_rows) {
        size_t expect = s.columns.empty() ? def.columns.size() : s.columns.size();
        if (!expr_row.empty() && expr_row.size() != expect) {
            r.ok = false; r.mysql_errno = 1136; r.sql_state = "21S01";
            r.error_message = fmt::format(
                "Column count doesn't match value count at row 1 (expected {}, got {})",
                expect, expr_row.size());
            return r;
        }
    }

    // 严格模式取值校验:NOT NULL(1048) / 非法整数(1366) / 超长字符串(1406) / 数值越界(1264)
    auto coerce_row = [&](storage::Row& row) -> bool {
        for (size_t i = 0; i < def.columns.size() && i < row.size(); ++i) {
            auto& cd = def.columns[i];
            auto& v = row[i];
            if (v.is_null()) {
                // AUTO_INCREMENT 列显式给 NULL → 分配下一个值(MySQL 行为)
                if (cd.is_auto_increment) {
                    v = types::Value::make_int(tbl->next_auto_increment());
                    continue;
                }
                if (!cd.nullable) {
                    r.ok = false; r.mysql_errno = 1048; r.sql_state = "23000";
                    r.error_message = fmt::format("Column '{}' cannot be null", cd.name);
                    return false;
                }
                continue;
            }
            auto t = cd.type.type;
            if (types::is_integer(t)) {
                int64_t iv = 0;
                if (v.is_double()) {
                    double d = v.v_double;
                    long double lo = -2147483649.0L, hi = 2147483648.0L;
                    if (t == types::SqlType::BigInt) { lo = -9.2233720368547758e18L; hi = 9.2233720368547758e18L; }
                    if (static_cast<long double>(d) < lo || static_cast<long double>(d) > hi) {
                        r.ok = false; r.mysql_errno = 1264; r.sql_state = "22003";
                        r.error_message = fmt::format("Out of range value for column '{}' at row 1", cd.name);
                        return false;
                    }
                    iv = static_cast<int64_t>(d);
                    v = types::Value::make_int(iv);
                } else if (!v.try_to_int(iv)) {
                    r.ok = false; r.mysql_errno = 1366; r.sql_state = "HY000";
                    r.error_message = fmt::format(
                        "Incorrect integer value: '{}' for column '{}' at row 1",
                        v.is_string() ? v.v_str : v.to_string(), cd.name);
                    return false;
                } else {
                    v = types::Value::make_int(iv);
                }
                if (t == types::SqlType::Int && (iv > 2147483647LL || iv < -2147483648LL)) {
                    r.ok = false; r.mysql_errno = 1264; r.sql_state = "22003";
                    r.error_message = fmt::format("Out of range value for column '{}' at row 1", cd.name);
                    return false;
                }
                if (t == types::SqlType::SmallInt && (iv > 32767 || iv < -32768)) {
                    r.ok = false; r.mysql_errno = 1264; r.sql_state = "22003";
                    r.error_message = fmt::format("Out of range value for column '{}' at row 1", cd.name);
                    return false;
                }
                if (t == types::SqlType::TinyInt && (iv > 127 || iv < -128)) {
                    r.ok = false; r.mysql_errno = 1264; r.sql_state = "22003";
                    r.error_message = fmt::format("Out of range value for column '{}' at row 1", cd.name);
                    return false;
                }
            } else if (t == types::SqlType::Varchar || t == types::SqlType::Char) {
                if (v.is_string() && cd.type.length > 0 && v.v_str.size() > cd.type.length) {
                    r.ok = false; r.mysql_errno = 1406; r.sql_state = "22001";
                    r.error_message = fmt::format("Data too long for column '{}' at row 1", cd.name);
                    return false;
                }
            }
        }
        return true;
    };

    int affected = 0;
    for (auto& expr_row : s.value_rows) {
        storage::Row row;
        if (!s.columns.empty()) {
            // 按列名对齐
            row.resize(def.columns.size());
            std::vector<bool> set_flags(def.columns.size(), false);
            for (size_t i = 0; i < s.columns.size() && i < expr_row.size(); ++i) {
                int idx = def.column_index(s.columns[i]);
                if (idx < 0) continue;
                ExecContext ctx{ catalog_, s.table_name, {}, {} };
            ctx.exec = this;
                ctx.exec = this;
                row[idx] = eval(*expr_row[i], ctx);
                set_flags[idx] = true;  // 显式设置了(哪怕值是 NULL)
            }
            // 未指定列:补默认(用户没写的列才补)
            for (size_t i = 0; i < def.columns.size(); ++i) {
                if (!set_flags[i]) {
                    auto& cdef = def.columns[i];
                    if (cdef.is_auto_increment) row[i] = types::Value::make_int(tbl->next_auto_increment());
                    else if (cdef.default_value) {
                        ExecContext ctx2{ catalog_, s.table_name, {}, {} };
                        ctx2.exec = this;
                        row[i] = eval(*cdef.default_value, ctx2);
                    }
                    else row[i] = types::Value::make_null();
                }
            }
        } else {
            // 按位置对齐
            ExecContext ctx{ catalog_, s.table_name, {}, {} };
            ctx.exec = this;
            for (auto& e : expr_row) row.push_back(eval(*e, ctx));
            for (size_t i = row.size(); i < def.columns.size(); ++i) {
                auto& cdef = def.columns[i];
                if (cdef.is_auto_increment) row.push_back(types::Value::make_int(tbl->next_auto_increment()));
                else if (cdef.default_value) row.push_back(eval(*cdef.default_value, ctx));
                else row.push_back(types::Value::make_null());
            }
        }

        if (!coerce_row(row)) return r;

        int64_t rid = -1;
        uint64_t create_scn = core::Database::instance().wal().next_scn();
        if (!tbl->insert(row, &rid, create_scn, writer_txn(session_))) {
            r.ok = false; r.mysql_errno = 1062; r.sql_state = "23000";
            r.error_message = "Duplicate entry for PRIMARY KEY";
            return r;
        }
        // WAL append: InsertRow
        {
            auto [db_part, tbl_part] = split_qname(s.table_name);
            std::string db_name = db_part.empty() ? default_db() : db_part;
            core::Database::instance().wal().append_insert_row(db_name, tbl_part, rid, row);
            core::Database::instance().wal().append_auto_inc_update(db_name, tbl_part,
                                                                    static_cast<uint64_t>(tbl->auto_inc_cursor()));

            // M6: 记录 RowAction (INSERT 是新行, 主键冲突已防, 不需要行锁)
            if (session_) {
                auto ra = core::RowAction::make_insert(
                    session_, create_scn, rid,
                    db_name, tbl_part, row);
                session_->addRowAction(std::move(ra));
            }
        }
        affected++;
        // 更新 last_insert_id:只反映自增列生成的值(显式主键不算)
        for (size_t ci = 0; ci < row.size() && ci < def.columns.size(); ++ci) {
            if (def.columns[ci].is_auto_increment && row[ci].is_int())
                r.last_insert_id = row[ci].v_int;
        }
    }

    // M3: INSERT … SELECT …
    if (s.select_source) {
        auto sel = execute(*s.select_source);
        if (!sel.ok) { r.ok = false; r.mysql_errno = sel.mysql_errno; r.sql_state = sel.sql_state; r.error_message = sel.error_message; return r; }
        for (auto& src_row : sel.rows) {
            storage::Row row;
            // 简单按位置对齐:SELECT 列数 >= INSERT 目标列数
            size_t n = s.columns.empty() ? def.columns.size() : s.columns.size();
            for (size_t i = 0; i < n && i < src_row.size(); ++i) {
                if (s.columns.empty()) row.push_back(src_row[i]);
                else {
                    int idx = def.column_index(s.columns[i]);
                    if (idx >= 0 && row.size() <= idx) row.resize(idx + 1);
                    if (idx >= 0) row[idx] = src_row[i];
                }
            }
            // 补默认列
            for (size_t i = row.size(); i < def.columns.size(); ++i) {
                auto& cdef = def.columns[i];
                if (cdef.is_auto_increment) row.push_back(types::Value::make_int(tbl->next_auto_increment()));
                else if (cdef.default_value) {
                    ExecContext ctx0{ catalog_, s.table_name, {}, {} }; ctx0.exec = this;
                    row.push_back(eval(*cdef.default_value, ctx0));
                } else row.push_back(types::Value::make_null());
            }
            if (!coerce_row(row)) return r;
            int64_t rid = -1;
            uint64_t create_scn = core::Database::instance().wal().next_scn();
            if (!tbl->insert(row, &rid, create_scn, writer_txn(session_))) {
                r.ok = false; r.mysql_errno = 1062; r.sql_state = "23000";
                r.error_message = "Duplicate entry for PRIMARY KEY";
                return r;
            }
            // WAL append: InsertRow (INSERT...SELECT 分支)
            {
                auto [db_part, tbl_part] = split_qname(s.table_name);
                std::string db_name = db_part.empty() ? default_db() : db_part;
                core::Database::instance().wal().append_insert_row(db_name, tbl_part, rid, row);
                core::Database::instance().wal().append_auto_inc_update(db_name, tbl_part,
                                                                        static_cast<uint64_t>(tbl->auto_inc_cursor()));

                // M6: RowAction
                if (session_) {
                    session_->addRowAction(core::RowAction::make_insert(
                        session_, create_scn, rid, db_name, tbl_part, row));
                }
            }
            affected++;
            // last_insert_id
            for (auto& col_list : def.primary_key_cols)
                for (auto& cname : col_list) {
                    int idx = def.column_index(cname);
                    if (idx >= 0 && idx < static_cast<int>(row.size()) && row[idx].is_int()) {
                        if (def.columns[idx].is_auto_increment) r.last_insert_id = row[idx].v_int;
                    }
                }
        }
    }

    r.affected_rows = affected;
    return r;
}

// ---------- UPDATE ----------
ResultSet Executor::exec_update(const ast::UpdateStmt& s) {
    ResultSet r;
    auto* tbl = catalog_.get_table(s.table_name, default_db());
    if (!tbl) {
        r.ok = false; r.mysql_errno = 1146; r.sql_state = "42S02";
        r.error_message = fmt::format("Table '{}' doesn't exist", s.table_name);
        return r;
    }
    auto& def = tbl->def();

    std::vector<std::pair<int, const ast::Expression*>> set_idx_expr;
    for (auto& [col, expr] : s.set_clause) {
        int idx = def.column_index(col);
        if (idx < 0) {
            r.ok = false; r.mysql_errno = 1054; r.sql_state = "42S22";
            r.error_message = fmt::format("Unknown column '{}'", col);
            return r;
        }
        set_idx_expr.emplace_back(idx, expr.get());
    }

    int affected = 0;
    // 先收集所有要改的行 (不能在 for_each 里 modify AVL)
    struct Pending { int64_t rid; storage::Row new_row; storage::Row pk_old; };
    std::vector<Pending> pending;
    uint64_t qscn = query_scn(session_);
    tbl->for_each_visible(qscn, [&](int64_t rid, const storage::Row& row) {
        ExecContext ctx{ catalog_, s.table_name, {}, row };
        ctx.exec = this;
        ctx.col_index.clear();
        for (size_t i = 0; i < def.columns.size(); ++i) {
            ctx.col_index[def.columns[i].name] = static_cast<int>(i);
            ctx.col_index[s.table_name + "." + def.columns[i].name] = static_cast<int>(i);
        }

        bool match = true;
        if (s.where) {
            auto v = eval(*s.where, ctx);
            match = to_bool(v);
        }
        if (!match) return true;

        storage::Row new_row = row;
        for (auto& [idx, expr] : set_idx_expr) {
            new_row[idx] = eval(*expr, ctx);
        }
        // 收集旧 pk
        storage::Row pk_old;
        for (auto& col_list : def.primary_key_cols)
            for (auto& cname : col_list) {
                int idx = def.column_index(cname);
                if (idx >= 0) pk_old.push_back(row[idx]);
            }
        pending.push_back({ rid, std::move(new_row), std::move(pk_old) });
        return true;
    }, reader_txn(session_));

    // 循环外改
    auto [u_db, u_tbl] = split_qname(s.table_name);
    std::string u_db_name = u_db.empty() ? default_db() : u_db;
    for (auto& p : pending) {
        // M6: 拿 EXCLUSIVE 行锁
        if (session_ && !session_->tryLockRowWrite(*tbl, p.rid)) {
            r.ok = false; r.mysql_errno = 1213; r.sql_state = "40001";
            r.error_message = "Lock wait timeout exceeded; try restarting transaction";
            return r;
        }

        uint64_t del_scn = core::Database::instance().wal().next_scn();
        uint64_t ins_scn = core::Database::instance().wal().next_scn();
        storage::Row old_row = *tbl->get_row(p.rid);   // 拿旧行存 RowAction
        uint64_t wtxn = writer_txn(session_);
        if (!p.pk_old.empty()) tbl->delete_by_pk(p.pk_old, del_scn, wtxn);
        int64_t new_rid = -1;
        if (!tbl->insert(p.new_row, &new_rid, ins_scn, wtxn)) {
            // 目标主键已存在:把刚才的逻辑删除还原,保证语句原子性
            tbl->restore_row(p.rid);
            r.ok = false; r.mysql_errno = 1062; r.sql_state = "23000";
            r.error_message = "Duplicate entry for PRIMARY KEY";
            return r;
        }
        affected++;
        core::Database::instance().wal().append_update_row(u_db_name, u_tbl, p.pk_old, p.new_row);

        // M6: 两个 RowAction (UPDATE = DELETE旧 + INSERT新)
        // 关键:两条 action 必须各自指向自己真正影响的 row_id,
        // 否则 commit/rollback 会把版本信息打在错误的行上。
        if (session_) {
            auto ra_del = core::RowAction::make_delete(
                session_, del_scn, p.rid, u_db_name, u_tbl, old_row);
            ra_del.type = core::RowAction::UPDATE_DELETE;
            session_->addRowAction(std::move(ra_del));

            auto ra_ins = core::RowAction::make_insert(
                session_, ins_scn, new_rid, u_db_name, u_tbl, p.new_row);
            ra_ins.type = core::RowAction::UPDATE_INSERT;
            session_->addRowAction(std::move(ra_ins));
        }
    }

    r.affected_rows = affected;
    return r;
}

// ---------- DELETE ----------
ResultSet Executor::exec_delete(const ast::DeleteStmt& s) {
    ResultSet r;
    auto* tbl = catalog_.get_table(s.table_name, default_db());
    if (!tbl) {
        r.ok = false; r.mysql_errno = 1146; r.sql_state = "42S02";
        r.error_message = fmt::format("Table '{}' doesn't exist", s.table_name);
        return r;
    }
    auto& def = tbl->def();

    // 收集要删的 row_id(不能在 for_each 里删)
    std::vector<int64_t> to_delete;
    uint64_t dqscn = query_scn(session_);
    tbl->for_each_visible(dqscn, [&](int64_t rid, const storage::Row& row) {
        if (!s.where) { to_delete.push_back(rid); return true; }
        ExecContext ctx{ catalog_, s.table_name, {}, row };
        ctx.exec = this;
        ctx.col_index.clear();
        for (size_t i = 0; i < def.columns.size(); ++i) {
            ctx.col_index[def.columns[i].name] = static_cast<int>(i);
            ctx.col_index[s.table_name + "." + def.columns[i].name] = static_cast<int>(i);
        }
        auto v = eval(*s.where, ctx);
        if (to_bool(v)) to_delete.push_back(rid);
        return true;
    }, reader_txn(session_));

    int affected = 0;
    auto [d_db, d_tbl] = split_qname(s.table_name);
    std::string d_db_name = d_db.empty() ? default_db() : d_db;
    for (int64_t rid : to_delete) {
        // M6: 拿 EXCLUSIVE 行锁
        if (session_ && !session_->tryLockRowWrite(*tbl, rid)) {
            r.ok = false; r.mysql_errno = 1213; r.sql_state = "40001";
            r.error_message = "Lock wait timeout exceeded; try restarting transaction";
            return r;
        }

        auto maybe = tbl->get_row(rid);
        if (!maybe) continue;
        storage::Row pk;
        for (auto& col_list : def.primary_key_cols)
            for (auto& cname : col_list) {
                int idx = def.column_index(cname);
                if (idx >= 0 && idx < static_cast<int>(maybe->size())) pk.push_back((*maybe)[idx]);
            }
        if (!pk.empty()) {
            uint64_t del_scn = core::Database::instance().wal().next_scn();
            tbl->delete_by_pk(pk, del_scn, writer_txn(session_));

            // M6: RowAction
            if (session_) {
                session_->addRowAction(core::RowAction::make_delete(
                    session_, del_scn, rid, d_db_name, d_tbl, *maybe));
            }
        }
        else {
            // 无主键 — 不支持按 row_id 删
        }
        affected++;
        core::Database::instance().wal().append_delete_row(d_db_name, d_tbl, pk);
    }
    // 全表清空时用 truncate-like
    if (!s.where && to_delete.empty()) {
        // 上面 for_each 没加任何东西(因为没有 where 我 push 了),但如果原本就有数据...
        // 其实上面 for_each 里已经 push 了,这里 safe
    }
    r.affected_rows = affected;
    return r;
}

// ---------- SELECT ----------
ResultSet Executor::execute_sub(const ast::Statement& stmt, const ExecContext* outer,
                                bool* used, const CteMap* ctes) {
    ResultSet r;
    if (stmt.kind != ast::StmtKind::Select || !stmt.select_) {
        r.ok = false; r.mysql_errno = 1064; r.sql_state = "42000";
        r.error_message = "subquery must be a SELECT statement";
        return r;
    }
    if (exec_depth_ >= 32) {
        r.ok = false; r.mysql_errno = 1064; r.sql_state = "42000";
        r.error_message = "subquery nesting too deep (limit 32)";
        return r;
    }
    ++exec_depth_;
    r = exec_select(*stmt.select_, outer, used, ctes);
    --exec_depth_;
    return r;
}

const std::vector<storage::Row>* Executor::subquery_rows(const ast::Expression& e,
                                                         const ExecContext& ctx) const {
    auto it = sq_cache_.find(&e);
    if (it != sq_cache_.end()) return &it->second.rows;   // 非相关:只执行一次
    bool used = false;
    ResultSet rs = const_cast<Executor*>(this)->execute_sub(*e.subquery_stmt, &ctx, &used, ctx.ctes);
    if (!rs.ok) {
        eval_error_ = rs.error_message;
        return nullptr;
    }
    if (!used) {
        auto& ent = sq_cache_[&e];
        ent.rows = std::move(rs.rows);
        return &ent.rows;
    }
    // 相关子查询:结果依赖外层当前行,不缓存
    sq_scratch_ = std::move(rs.rows);
    return &sq_scratch_;
}

ResultSet Executor::materialize_ctes(const std::vector<ast::SelectStmt::CteDef>& defs,
                                     CteMap& ctes) {
    ResultSet ok_r;
    for (auto& cd : defs) {
        if (!cd.query || cd.query->kind != ast::StmtKind::Select || !cd.query->select_) {
            ResultSet e;
            e.ok = false; e.mysql_errno = 1064; e.sql_state = "42000";
            e.error_message = fmt::format("CTE '{}' must be a SELECT statement", cd.name);
            return e;
        }
        std::string key = ci_lower(cd.name);
        if (!cd.recursive) {
            ResultSet rs = exec_select(*cd.query->select_, nullptr, nullptr, &ctes);
            if (!rs.ok) return rs;
            ctes[key] = table_from_result(cd.name, rs, cd.column_names);
            continue;
        }
        // 递归 CTE:anchor UNION [ALL] 递归项,迭代到不动点
        const auto& q = *cd.query->select_;
        if (q.set_op == ast::SelectSetOp::None || !q.set_right ||
            q.set_right->kind != ast::StmtKind::Select) {
            ResultSet e;
            e.ok = false; e.mysql_errno = 1064; e.sql_state = "42000";
            e.error_message = fmt::format("recursive CTE '{}' requires UNION [ALL]", cd.name);
            return e;
        }
        auto anchor_stmt = ast::clone_stmt(*cd.query);
        anchor_stmt->select_->set_op = ast::SelectSetOp::None;
        anchor_stmt->select_->set_right.reset();
        ResultSet anchor = exec_select(*anchor_stmt->select_, nullptr, nullptr, &ctes);
        if (!anchor.ok) return anchor;
        auto tbl = table_from_result(cd.name, anchor, cd.column_names);
        const ast::SelectStmt* rec = q.set_right->select_.get();
        bool dedup = (q.set_op == ast::SelectSetOp::Union);
        std::unordered_set<std::string> seen;
        if (dedup) {
            tbl->for_each_visible(0, [&](int64_t, const storage::Row& row) {
                seen.insert(row_dedup_key(row)); return true;
            });
        }
        // working set = 上一轮新增行(首轮为 anchor)。不能用「累积全集」:
        // UNION ALL 下递归项会对历史行反复生成,行数指数增长(seq 1..100 直接跑飞)。
        auto work = std::make_shared<storage::Table>(tbl->def());
        tbl->for_each_visible(0, [&](int64_t, const storage::Row& row) {
            work->insert(row); return true;
        });
        for (int iter = 0; iter < 10000; ++iter) {
            sq_cache_.clear();   // CTE 内容逐轮变化,子查询缓存必须失效
            CteMap iter_ctes = ctes;
            iter_ctes[key] = work;   // 递归项只看到上一轮结果
            ResultSet delta = exec_select(*rec, nullptr, nullptr, &iter_ctes);
            if (!delta.ok) return delta;
            if (delta.rows.empty()) break;
            auto next_work = std::make_shared<storage::Table>(tbl->def());
            bool added = false;
            for (auto& row : delta.rows) {
                storage::Row r2 = row;
                r2.resize(tbl->def().columns.size(), types::Value::make_null());
                if (dedup && !seen.insert(row_dedup_key(r2)).second) continue;
                tbl->insert(r2);        // 累积最终结果
                next_work->insert(r2);  // 下一轮输入
                added = true;
            }
            if (!added) break;
            work = std::move(next_work);
        }
        ctes[key] = std::move(tbl);
    }
    return ok_r;
}

ResultSet Executor::exec_select(const ast::SelectStmt& s, const ExecContext* outer,
                                bool* outer_used, const CteMap* outer_ctes) {
    // WITH 子句:按声明顺序物化,后面的 CTE 可引用前面的
    CteMap local_ctes;
    const CteMap* ctes = outer_ctes;
    if (!s.ctes.empty()) {
        if (outer_ctes) local_ctes = *outer_ctes;
        ResultSet mr = materialize_ctes(s.ctes, local_ctes);
        if (!mr.ok) return mr;
        ctes = &local_ctes;
    }
    if (s.set_op == ast::SelectSetOp::None || !s.set_right ||
        s.set_right->kind != ast::StmtKind::Select) {
        return exec_select_body(s, outer, outer_used, ctes);
    }
    ResultSet left = exec_select_body(s, outer, outer_used, ctes);
    if (!left.ok) return left;
    ResultSet right = exec_select(*s.set_right->select_, outer, outer_used, ctes);
    if (!right.ok) return right;

    ResultSet r;
    r.ok = true;
    r.column_names = std::move(left.column_names);
    r.column_types = std::move(left.column_types);
    r.rows = std::move(left.rows);
    // UNION 两侧列数必须一致(MySQL 1222)
    size_t left_cols = r.column_names.size();
    size_t right_cols = right.column_names.size();
    if (left_cols != right_cols) {
        ResultSet er;
        er.ok = false; er.mysql_errno = 1222; er.sql_state = "21000";
        er.error_message = "The used SELECT statements have a different number of columns";
        return er;
    }
    // 类型统一:两侧列类型不同 → 提升为字符串(避免列声明 INT 却下发文本导致客户端崩溃)
    for (size_t i = 0; i < r.column_types.size() && i < right.column_types.size(); ++i) {
        if (r.column_types[i] != right.column_types[i])
            r.column_types[i] = types::SqlType::Varchar;
    }
    size_t ncols = left_cols;
    if (ncols == 0 && !r.rows.empty()) ncols = r.rows[0].size();
    if (ncols == 0 && !right.rows.empty()) ncols = right.rows[0].size();
    while (r.column_names.size() < ncols) {
        r.column_names.push_back(fmt::format("col{}", r.column_names.size() + 1));
        r.column_types.push_back(types::SqlType::Varchar);
    }
    for (auto& row : r.rows) {
        while (row.size() < ncols) row.push_back(types::Value::make_null());
        if (row.size() > ncols) row.resize(ncols);
    }
    for (auto& row : right.rows) {
        while (row.size() < ncols) row.push_back(types::Value::make_null());
        if (row.size() > ncols) row.resize(ncols);
    }

    if (s.set_op == ast::SelectSetOp::UnionAll) {
        r.rows.insert(r.rows.end(), std::make_move_iterator(right.rows.begin()),
                      std::make_move_iterator(right.rows.end()));
    } else {
        std::unordered_set<std::string> seen;
        seen.reserve(r.rows.size() + right.rows.size());
        auto key_of = [](const storage::Row& row) {
            std::string k;
            for (auto& v : row) { k += v.to_string(); k.push_back('\x01'); }
            return k;
        };
        for (auto& row : r.rows) seen.insert(key_of(row));
        for (auto& row : right.rows) {
            if (seen.insert(key_of(row)).second) r.rows.push_back(std::move(row));
        }
    }

    // 无括号语法下,右分支末尾的 ORDER BY / LIMIT 作用于整个 UNION 结果
    const auto& tail = *s.set_right->select_;
    if (!tail.order_by.empty()) {
        std::map<std::string, int> names;
        for (size_t i = 0; i < r.column_names.size(); ++i) names[r.column_names[i]] = static_cast<int>(i);
        std::stable_sort(r.rows.begin(), r.rows.end(), [&](const storage::Row& a, const storage::Row& b) {
            for (auto& [oe, asc] : tail.order_by) {
                int idx = -1;
                if (oe->kind == ast::ExprKind::Literal && oe->literal.is_int())
                    idx = static_cast<int>(oe->literal.v_int) - 1;
                else if (oe->kind == ast::ExprKind::Column) {
                    auto it = names.find(oe->column_name);
                    if (it != names.end()) idx = it->second;
                }
                if (idx < 0 || static_cast<size_t>(idx) >= ncols) continue;
                int c = a[idx].compare(b[idx]);
                if (c != 0) return asc ? c < 0 : c > 0;
            }
            return false;
        });
    }
    int64_t off = tail.offset.value_or(0);
    if (off > 0) {
        if (static_cast<size_t>(off) >= r.rows.size()) r.rows.clear();
        else r.rows.erase(r.rows.begin(), r.rows.begin() + off);
    }
    if (tail.limit && *tail.limit >= 0 && static_cast<size_t>(*tail.limit) < r.rows.size())
        r.rows.resize(static_cast<size_t>(*tail.limit));
    r.affected_rows = static_cast<int64_t>(r.rows.size());
    return r;
}

ResultSet Executor::exec_select_body(const ast::SelectStmt& s, const ExecContext* outer,
                                     bool* outer_used, const CteMap* ctes) {
    ResultSet r;
    auto take_eval_error = [&]() -> ResultSet {
        ResultSet er;
        er.ok = false;
        er.mysql_errno = eval_errno_ ? eval_errno_ : 1064;
        er.sql_state = eval_errno_ ? eval_sql_state_ : std::string("42000");
        er.error_message = eval_error_;
        eval_error_.clear();
        eval_errno_ = 0;
        eval_sql_state_ = "42000";
        return er;
    };

    // 推断表达式静态类型 — 供 wire 协议 field_type 用
    struct _SlotRef {
        const std::string& alias_or_real;
        const storage::TableDef* def;
    };
    // 用 std::function 支持递归
    std::function<types::SqlType(const ast::Expression&, const std::vector<_SlotRef>*)> infer_type =
        [&](const ast::Expression& e, const std::vector<_SlotRef>* slots) -> types::SqlType {
        using namespace types;
        if (e.is_window) {
            std::string wl = e.func_name;
            std::transform(wl.begin(), wl.end(), wl.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (wl == "row_number" || wl == "rank" || wl == "dense_rank" ||
                wl == "count" || wl == "ntile")
                return SqlType::BigInt;
            if (wl == "avg" || wl == "percent_rank" || wl == "cume_dist")
                return SqlType::Double;
            if (slots && !e.func_args.empty()) {
                auto t = infer_type(*e.func_args[0], slots);
                if (t != SqlType::Null) return t;
            }
            return SqlType::Double;
        }
        if (e.kind == ast::ExprKind::Column && slots) {
            for (auto& sl : *slots) {
                for (auto& col : sl.def->columns) {
                    bool match = col.name == e.column_name;
                    if (!e.table_name.empty()) {
                        match = match && (e.table_name == sl.alias_or_real);
                    }
                    if (match) return col.type.type;
                }
            }
        }
        if (e.kind == ast::ExprKind::Case) {
            // CASE 的结果类型取第一个 THEN 分支
            if (!e.case_whens.empty() && e.case_whens[0].second)
                return infer_type(*e.case_whens[0].second, slots);
            return SqlType::Varchar;
        }
        if (e.kind == ast::ExprKind::BinaryOp) {
            switch (e.binop) {
                case ast::BinOp::Eq: case ast::BinOp::Ne:
                case ast::BinOp::Lt: case ast::BinOp::Gt:
                case ast::BinOp::Le: case ast::BinOp::Ge:
                case ast::BinOp::And: case ast::BinOp::Or:
                    return SqlType::Boolean;
                case ast::BinOp::Concat:
                    return SqlType::Varchar;
                default: break;
            }
            auto lt = e.lhs ? infer_type(*e.lhs, slots) : SqlType::Null;
            auto rt = e.rhs ? infer_type(*e.rhs, slots) : SqlType::Null;
            if (e.binop == ast::BinOp::Slash) return SqlType::Double;
            // 只有两侧都是已知数值类型才给出数值列类型;否则保持字符串(安全兜底)
            if (!is_numeric(lt) || !is_numeric(rt)) return SqlType::Varchar;
            if (lt == SqlType::Double || rt == SqlType::Double ||
                lt == SqlType::Decimal || rt == SqlType::Decimal ||
                lt == SqlType::Float || rt == SqlType::Float)
                return SqlType::Double;
            if (is_integer(lt) && is_integer(rt))
                return static_cast<int>(lt) >= static_cast<int>(rt) ? lt : rt;
            return SqlType::Double;
        }
        if (e.kind == ast::ExprKind::UnaryOp) {
            if (e.unary == ast::Expression::UnaryKind::Not ||
                e.unary == ast::Expression::UnaryKind::IsNull ||
                e.unary == ast::Expression::UnaryKind::IsNotNull)
                return SqlType::Boolean;
            return e.operand ? infer_type(*e.operand, slots) : SqlType::BigInt;
        }
        if (e.kind == ast::ExprKind::Literal) {
            switch (e.literal.kind) {
                case Value::Kind::Int64:  return SqlType::BigInt;
                case Value::Kind::Double:  return SqlType::Double;
                case Value::Kind::Bool:   return SqlType::Boolean;
                case Value::Kind::Null:   return SqlType::Null;
                default:                  return SqlType::Varchar;
            }
        }
        if (e.kind == ast::ExprKind::FunctionCall) {
            std::string lower = e.func_name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            if (lower == "count")  return SqlType::BigInt;
            if (lower == "avg")    return SqlType::Decimal;
            if (lower == "sum") {
                // 声明类型必须与实际聚合值一致:整型输入用 int64 累加;浮点/DECIMAL 输入
                // 经 double 路径聚合(恒返回 double)。若 DECIMAL 误报 LONGLONG,pymysql
                // 会对文本 '300.5' 调 int() 崩溃并中断行读取,污染连接包序。
                if (!e.func_args.empty() && slots) {
                    auto t = infer_type(*e.func_args[0], slots);
                    if (is_numeric(t))
                        return (t == SqlType::Double || t == SqlType::Float ||
                                t == SqlType::Decimal) ? SqlType::Double
                                                       : SqlType::BigInt;
                }
                return SqlType::Decimal;
            }
            if (lower == "min" || lower == "max") {
                // MySQL 语义:MIN/MAX 返回与参数相同的类型(含 DATE/DATETIME/字符串)
                if (!e.func_args.empty() && slots) {
                    auto t = infer_type(*e.func_args[0], slots);
                    if (t != SqlType::Null) return t;
                }
                return SqlType::Decimal;
            }
            if (lower == "coalesce" || lower == "ifnull") {
                // 取第一个非 NULL 参数的类型,避免 COALESCE(SUM(int),0) 落到字符串类型
                if (!e.func_args.empty() && slots) {
                    for (auto& a : e.func_args) {
                        auto t = infer_type(*a, slots);
                        if (t != SqlType::Null) return t;
                    }
                }
                return SqlType::Varchar;
            }
            return SqlType::Varchar;
        }
        return SqlType::Varchar;
    };

    if (s.from_tables.empty() && s.joins.empty()) {
        // 无表 SELECT:例如 SELECT 1, NOW()
        if (s.explain) {
            r.column_names = {"id", "select_type", "table", "type", "possible_keys",
                              "key", "rows", "Extra"};
            r.column_types = { types::SqlType::BigInt, types::SqlType::Varchar,
                               types::SqlType::Varchar, types::SqlType::Varchar,
                               types::SqlType::Varchar, types::SqlType::Varchar,
                               types::SqlType::BigInt, types::SqlType::Varchar };
            storage::Row row;
            row.push_back(types::Value::make_int(1));
            row.push_back(types::Value::make_string("SIMPLE"));
            row.push_back(types::Value::make_string(""));
            row.push_back(types::Value::make_string("constant"));
            row.push_back(types::Value::make_string(""));   // possible_keys
            row.push_back(types::Value::make_string(""));   // key
            row.push_back(types::Value::make_int(1));       // rows
            row.push_back(types::Value::make_string("No tables used"));
            r.rows.push_back(std::move(row));
            return r;
        }
        ExecContext ctx{ catalog_, "", {}, {} };
        ctx.exec = this;
        ctx.outer = outer;
        ctx.outer_used = outer_used;
        ctx.ctes = ctes;
        storage::Row row;
        ctx.col_index.clear();
        r.column_names.clear();
        r.column_types.clear();
        for (auto& [expr, alias] : s.columns) {
            std::string name = alias.empty() ? "<expr>" : alias;
            if (expr->kind == ast::ExprKind::Column) name = expr->column_name;
            r.column_names.push_back(name);
            r.column_types.push_back(infer_type(*expr, nullptr));
        }
        // WHERE 在无表查询里同样生效:SELECT 1 WHERE FALSE → 空结果集(列元数据仍要下发)
        if (s.where && !to_bool(eval(*s.where, ctx))) {
            if (!eval_error_.empty()) return take_eval_error();
            return r;
        }
        for (auto& [expr, alias] : s.columns) {
            (void)alias;
            row.push_back(eval(*expr, ctx));
        }
        if (!eval_error_.empty()) return take_eval_error();
        // LIMIT / OFFSET:LIMIT 0 → 空集;OFFSET ≥ 1 → 空集
        int64_t nofrom_off = s.offset.value_or(0);
        int64_t nofrom_lim = s.limit.value_or(1);
        if (nofrom_off > 0) return r;
        if (nofrom_lim <= 0) return r;
        r.rows.push_back(row);
        return r;
    }

    // 派生表物化:FROM (SELECT ...) / JOIN (SELECT ...) → 临时表
    std::vector<std::shared_ptr<storage::Table>> virtual_tables;   // 保活虚拟表
    std::map<std::string, storage::Table*> derived_map;
    auto add_derived = [&](const std::string& name, const ast::Statement& d) -> bool {
        ResultSet drs = exec_select(*d.select_, outer, outer_used, ctes);
        if (!drs.ok) { r = std::move(drs); return false; }
        auto tbl = table_from_result(name, drs, {});
        derived_map[ci_lower(name)] = tbl.get();
        virtual_tables.push_back(std::move(tbl));
        return true;
    };
    for (size_t i = 0; i < s.from_tables.size(); ++i) {
        if (i < s.from_derived.size() && s.from_derived[i] &&
            s.from_derived[i]->kind == ast::StmtKind::Select) {
            // 派生表必须有别名(MySQL 1248)
            if (s.from_tables[i].rfind("__derived", 0) == 0) {
                r.ok = false; r.mysql_errno = 1248; r.sql_state = "42000";
                r.error_message = "Every derived table must have its own alias";
                return r;
            }
            if (!add_derived(s.from_tables[i], *s.from_derived[i])) return r;
        }
    }
    for (auto& jc : s.joins) {
        if (jc.derived && jc.derived->kind == ast::StmtKind::Select) {
            if (jc.table_name.rfind("__join_derived", 0) == 0) {
                r.ok = false; r.mysql_errno = 1248; r.sql_state = "42000";
                r.error_message = "Every derived table must have its own alias";
                return r;
            }
            if (!add_derived(jc.table_name, *jc.derived)) return r;
        }
    }

    // FROM 首表 —— 派生表 / CTE / information_schema 虚拟表 / 普通表
    std::string primary_table = s.from_tables[0];
    auto resolve_table = [&](const std::string& name) -> storage::Table* {
        auto dit = derived_map.find(ci_lower(name));
        if (dit != derived_map.end()) return dit->second;
        if (ctes) {
            auto cit = ctes->find(ci_lower(name));
            if (cit != ctes->end()) return cit->second.get();
        }
        if (is_information_schema_name(name)) {
            auto dot = name.find('.');
            virtual_tables.push_back(build_information_schema_table(
                name.substr(0, dot), name.substr(dot + 1)));
            return virtual_tables.back().get();
        }
        return catalog_.get_table(name, default_db());
    };
    auto* tbl = resolve_table(primary_table);
    if (!tbl) {
        auto [missing_db, missing_tbl] = split_qname(primary_table);
        if (!missing_db.empty() && !catalog_.database_exists(missing_db)) {
            r.ok = false; r.mysql_errno = 1049; r.sql_state = "42000";
            r.error_message = fmt::format("Unknown database '{}'", missing_db);
            return r;
        }
        r.ok = false; r.mysql_errno = 1146; r.sql_state = "42S02";
        r.error_message = fmt::format("Table '{}' doesn't exist", primary_table);
        return r;
    }

    // 构造列索引映射:把所有表列合并成一个 flat row
    struct TableSlot {
        std::string real_name;   // 真实表名
        std::string alias;        // 别名
        const storage::TableDef* def;
        int col_offset;           // 在合并 row 里的起始偏移
    };
    std::vector<TableSlot> slots;
    int total_cols = 0;

    // 主表
    TableSlot main;
    main.real_name = primary_table;
    main.alias = s.from_aliases.empty() ? "" : s.from_aliases[0].second;
    main.def = &tbl->def();
    main.col_offset = 0;
    slots.push_back(main);
    total_cols += static_cast<int>(main.def->columns.size());

    // JOIN 表
    for (auto& jc : s.joins) {
        auto* jtbl = resolve_table(jc.table_name);
        if (!jtbl) {
            r.ok = false; r.mysql_errno = 1146; r.sql_state = "42S02";
            r.error_message = fmt::format("Table '{}' doesn't exist", jc.table_name);
            return r;
        }
        TableSlot js;
        js.real_name = jc.table_name;
        js.alias = jc.alias;
        js.def = &jtbl->def();
        js.col_offset = total_cols;
        slots.push_back(js);
        total_cols += static_cast<int>(js.def->columns.size());
    }

    // 列索引只构建一次;旧实现在 O(N×M) 的每个组合里重建 map + 拼接字符串,是主要常数开销
    std::map<std::string, int> base_col_index;
    std::set<std::string> ambiguous_cols;
    {
        std::map<std::string, int> name_count;
        for (auto& slot : slots) {
            for (size_t i = 0; i < slot.def->columns.size(); ++i) {
                std::string bare = slot.def->columns[i].name;
                int offset = slot.col_offset + static_cast<int>(i);
                base_col_index[bare] = offset;
                // MySQL 语义:表一旦起了别名,原表名不能再作为限定符。
                // (同名表自连接 + 别名时必须严格区分,否则 d.id 会互相覆盖)
                if (slot.alias.empty()) base_col_index[slot.real_name + "." + bare] = offset;
                else base_col_index[slot.alias + "." + bare] = offset;
                ++name_count[bare];
            }
        }
        for (auto& [n, c] : name_count) if (c > 1) ambiguous_cols.insert(n);
    }
    // 热路径:零拷贝上下文 —— 只存「行 + 列索引」的指针,不拷行、不拷 map。
    // 旧实现每行都 ExecContext{...,flat_row}(拷贝整行) + col_index = base_col_index
    // (深拷贝 ~40 节点 map),万行级聚合下是决定性常数开销。生命周期由调用方保证
    // (行来自容器/局部变量,存活期覆盖 ctx 使用范围)。
    auto build_ctx = [&](const storage::Row& flat_row) {
        ExecContext ctx{ catalog_, primary_table, {}, {} };
        ctx.exec = this;   // 子查询递归求值需要
        ctx.row_ref = &flat_row;
        ctx.idx_ref = &base_col_index;
        ctx.outer = outer;
        ctx.outer_used = outer_used;
        ctx.ctes = ctes;
        return ctx;
    };

    // WHERE 谓词下推:把只引用主表列的 AND 连接词提前到主表扫描,
    // 大幅减少进入 JOIN 的行数 (如 WHERE e.emp_no=10001 先把 30 万行滤成 1 行)。
    // 下推只是预过滤:最终 after_where 仍用完整 WHERE 复核,语义不变。
    std::vector<const ast::Expression*> pushdown_conjuncts;
    if (s.where) {
        int primary_cols = static_cast<int>(tbl->def().columns.size());
        std::vector<const ast::Expression*> all_conj;
        std::function<void(const ast::Expression&)> split_and =
            [&](const ast::Expression& e) {
                if (e.kind == ast::ExprKind::BinaryOp && e.binop == ast::BinOp::And &&
                    e.lhs && e.rhs) { split_and(*e.lhs); split_and(*e.rhs); }
                else all_conj.push_back(&e);
            };
        split_and(*s.where);
        std::function<bool(const ast::Expression&)> primary_only =
            [&](const ast::Expression& e) -> bool {
                switch (e.kind) {
                    case ast::ExprKind::Literal: return true;
                    case ast::ExprKind::Column: {
                        int idx = -1;
                        if (!e.table_name.empty()) {
                            // 限定名必须精确命中(真实表名或别名),否则保守不下推
                            auto it = base_col_index.find(e.table_name + "." + e.column_name);
                            if (it == base_col_index.end()) return false;
                            idx = it->second;
                        } else {
                            if (ambiguous_cols.count(e.column_name)) return false;  // 裸名歧义
                            auto it = base_col_index.find(e.column_name);
                            if (it == base_col_index.end()) return false;           // 未知名
                            idx = it->second;
                        }
                        return idx >= 0 && idx < primary_cols;   // 只允许主表列
                    }
                    case ast::ExprKind::BinaryOp:
                        return e.lhs && e.rhs && primary_only(*e.lhs) && primary_only(*e.rhs);
                    case ast::ExprKind::UnaryOp:
                        return e.operand && primary_only(*e.operand);
                    case ast::ExprKind::FunctionCall: {
                        for (auto& a : e.func_args) if (a && !primary_only(*a)) return false;
                        return true;
                    }
                    default: return false;   // Subquery/Case/Star/Placeholder:保守不下推
                }
            };
        for (auto* c : all_conj)
            if (primary_only(*c)) pushdown_conjuncts.push_back(c);
    }

    // M6 P2 · JOIN 执行计划: 每个 join 选 HASH / NESTED_LOOP / CROSS。
    // Hash Join 把等值 JOIN 从 O(N×M) 降到 O(N+M);非等值条件仍退化到嵌套循环,
    // 但列索引只构建一次(见上方 base_col_index),常数开销大幅下降。
    struct JoinPlan {
        ast::JoinKind kind;
        EquiJoinKey equi;
        bool pure_equi = false;         // ON 就是单个等值条件
        bool hash_join = false;
        bool idx_probe = false;         // PK 首列前缀索引探测 (右表零物化)
        size_t right_rows = 0, left_rows = 0;
        size_t est_cost = 0;
        std::string table;
    };
    std::vector<JoinPlan> join_plans;
    size_t est_left = tbl->row_count();
    for (size_t ji = 0; ji < s.joins.size(); ++ji) {
        auto& jc = s.joins[ji];
        auto* jtbl = resolve_table(jc.table_name);
        JoinPlan p;
        p.kind = jc.kind;
        p.table = jc.table_name;
        p.left_rows = est_left;
        p.right_rows = jtbl ? jtbl->row_count() : 0;
        int right_offset = (1 + ji < slots.size()) ? slots[1 + ji].col_offset : total_cols;
        int right_cols = jtbl ? static_cast<int>(jtbl->def().columns.size()) : 0;
        if (jc.on && jtbl && (jc.kind == ast::JoinKind::Inner || jc.kind == ast::JoinKind::Left)) {
            p.equi = extract_equi(*jc.on, base_col_index, ambiguous_cols,
                                  right_offset, right_cols, jtbl->def());
            p.pure_equi = is_equi_pair(*jc.on);
            p.hash_join = p.equi.valid;
            // PK 首列等值:逐左行前缀探测,避免右表全量物化 + 建哈希桶
            // (左侧行数不大于右表才划算: 探测成本 O(left·log right + 输出行数))
            p.idx_probe = p.equi.valid && p.equi.right_pk_first && p.left_rows <= p.right_rows;
        }
        // 成本估算: nested loop = left×right, hash = left+right。
        // 下一轮的输入行数按「上一轮输出」粗估: hash 取 max(left,right),嵌套循环取乘积上界。
        p.est_cost = p.hash_join ? (p.left_rows + p.right_rows) : (p.left_rows * p.right_rows);
        est_left = p.hash_join ? std::max(p.left_rows, p.right_rows)
                               : p.left_rows * p.right_rows;
        join_plans.push_back(std::move(p));
    }

    // EXPLAIN: 只输出执行计划,不扫描数据、不执行 JOIN (MySQL 风格最小版)
    if (s.explain) {
        r.column_names = {"id", "select_type", "table", "type", "possible_keys",
                          "key", "rows", "Extra"};
        r.column_types = { types::SqlType::BigInt, types::SqlType::Varchar,
                           types::SqlType::Varchar, types::SqlType::Varchar,
                           types::SqlType::Varchar, types::SqlType::Varchar,
                           types::SqlType::BigInt, types::SqlType::Varchar };
        auto put = [&](int id, const char* sel, const std::string& table,
                       const char* type, const char* possible, const char* key,
                       int64_t rows, const std::string& extra) {
            storage::Row row;
            row.push_back(types::Value::make_int(id));
            row.push_back(types::Value::make_string(sel));
            row.push_back(types::Value::make_string(table));
            row.push_back(types::Value::make_string(type));
            row.push_back(types::Value::make_string(possible));
            row.push_back(types::Value::make_string(key));
            row.push_back(types::Value::make_int(rows));
            row.push_back(types::Value::make_string(extra));
            r.rows.push_back(std::move(row));
        };
        put(1, "SIMPLE", primary_table, "ALL", "", "",
            static_cast<int64_t>(tbl->row_count()), s.where ? "Using where" : "");
        for (size_t ji = 0; ji < join_plans.size(); ++ji) {
            const auto& p = join_plans[ji];
            std::string extra;
            if (p.kind == ast::JoinKind::Cross) {
                extra = "Using cross join (cartesian)";
            } else if (p.hash_join) {
                extra = p.idx_probe
                    ? fmt::format("Using index probe; cost {} vs hash join {}",
                                  p.left_rows, p.left_rows + p.right_rows)
                    : fmt::format("Using hash join (build: {}, cost {} vs nested {})",
                                  p.table, p.est_cost, p.left_rows * p.right_rows);
                if (!p.pure_equi) extra += "; Using where (residual predicate)";
            } else {
                extra = fmt::format("Using nested loop (cost {})", p.est_cost);
            }
            const char* type = p.kind == ast::JoinKind::Cross ? "ALL"
                             : p.hash_join ? (p.equi.right_is_pk ? "eq_ref" : "ref")
                             : "ALL";
            std::string key = p.hash_join ? (p.equi.right_is_pk ? "PRIMARY" : p.equi.right_name) : "";
            put(static_cast<int>(ji + 2), "SIMPLE", p.table, type,
                key.empty() ? "" : key.c_str(), key.c_str(),
                static_cast<int64_t>(p.right_rows), extra);
        }
        r.affected_rows = static_cast<int64_t>(r.rows.size());
        r.found_rows = s.from_tables.empty() ? -1
                     : (tbl ? static_cast<int64_t>(tbl->row_count()) : -1);
        return r;
    }

    // 聚合检测: SELECT 列(含嵌套在标量函数内的聚合)、GROUP BY、HAVING 任一出现即走聚合路径。
    // 旧实现只扫 SELECT 顶层,COALESCE(SUM(x),0) 与「SELECT 无聚合但有 GROUP BY/HAVING」会被漏掉。
    auto is_agg_func = [](const std::string& n) {
        std::string low = n;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return low == "count" || low == "sum" || low == "avg" || low == "min" ||
               low == "max" || low == "group_concat";
    };
    std::function<bool(const ast::Expression&)> contains_agg =
        [&](const ast::Expression& e) -> bool {
            if (e.kind == ast::ExprKind::FunctionCall) {
                // 窗口函数(SUM(...) OVER (...))不是分组聚合,由投影阶段的窗口求值器处理
                if (e.is_window) return false;
                if (is_agg_func(e.func_name)) return true;
                for (auto& a : e.func_args) if (a && contains_agg(*a)) return true;
            }
            if (e.kind == ast::ExprKind::Case) {
                if (e.case_base && contains_agg(*e.case_base)) return true;
                for (auto& [w, r] : e.case_whens) {
                    if (w && contains_agg(*w)) return true;
                    if (r && contains_agg(*r)) return true;
                }
                if (e.case_else && contains_agg(*e.case_else)) return true;
            }
            if (e.lhs && contains_agg(*e.lhs)) return true;
            if (e.rhs && contains_agg(*e.rhs)) return true;
            if (e.operand && contains_agg(*e.operand)) return true;
            return false;
        };
    bool has_agg = !s.group_by.empty() || static_cast<bool>(s.having);
    for (auto& [e, _] : s.columns) if (contains_agg(*e)) has_agg = true;

    // ---- 静态语义校验(在扫描数据前失败,避免先做无用功) ----
    auto make_error = [&](uint16_t err, const char* state, std::string msg) -> ResultSet {
        ResultSet er;
        er.ok = false; er.mysql_errno = err; er.sql_state = state;
        er.error_message = std::move(msg);
        return er;
    };
    {
        // 窗口函数不允许出现在 WHERE(MySQL 3593)
        std::function<bool(const ast::Expression&)> has_window =
            [&](const ast::Expression& e) -> bool {
                if (e.kind == ast::ExprKind::FunctionCall && e.is_window) return true;
                if (e.lhs && has_window(*e.lhs)) return true;
                if (e.rhs && has_window(*e.rhs)) return true;
                if (e.operand && has_window(*e.operand)) return true;
                if (e.case_base && has_window(*e.case_base)) return true;
                for (auto& [w, r] : e.case_whens) {
                    if (w && has_window(*w)) return true;
                    if (r && has_window(*r)) return true;
                }
                if (e.case_else && has_window(*e.case_else)) return true;
                for (auto& a : e.func_args) if (a && has_window(*a)) return true;
                return false;
            };
        if (s.where && has_window(*s.where))
            return make_error(3593, "HY000",
                "You can't use window functions in WHERE clause");

        // 嵌套聚合不允许:SUM(COUNT(*)) → 1111
        std::function<bool(const ast::Expression&)> has_nested_agg =
            [&](const ast::Expression& e) -> bool {
                if (e.kind == ast::ExprKind::FunctionCall) {
                    bool self_agg = !e.is_window && is_agg_func(e.func_name);
                    if (self_agg) {
                        for (auto& a : e.func_args)
                            if (a && contains_agg(*a)) return true;
                    }
                    for (auto& a : e.func_args) if (a && has_nested_agg(*a)) return true;
                }
                if (e.lhs && has_nested_agg(*e.lhs)) return true;
                if (e.rhs && has_nested_agg(*e.rhs)) return true;
                if (e.operand && has_nested_agg(*e.operand)) return true;
                if (e.case_base && has_nested_agg(*e.case_base)) return true;
                for (auto& [w, r] : e.case_whens) {
                    if (w && has_nested_agg(*w)) return true;
                    if (r && has_nested_agg(*r)) return true;
                }
                if (e.case_else && has_nested_agg(*e.case_else)) return true;
                return false;
            };
        bool nested = false;
        for (auto& [e, _] : s.columns) if (has_nested_agg(*e)) { nested = true; break; }
        if (!nested && s.having) nested = has_nested_agg(*s.having);
        if (nested)
            return make_error(1111, "HY000", "Invalid use of group function");
    }

    // 输出列别名/类型(提前: 聚合下推路径会在 JOIN 阶段直接产出结果)
    r.column_names.clear();
    r.column_types.clear();
    std::vector<_SlotRef> slot_refs;
    for (auto& slot : slots) {
        slot_refs.push_back({ slot.alias.empty() ? slot.real_name : slot.alias, slot.def });
    }
    for (auto& [e, alias] : s.columns) {
        if (e->kind == ast::ExprKind::Star) {
            for (auto& slot : slots) {
                for (auto& col : slot.def->columns) {
                    std::string name = slot.alias.empty() ? col.name : (slot.alias + "." + col.name);
                    r.column_names.push_back(name);
                    r.column_types.push_back(col.type.type);
                }
            }
        } else {
            // 有别名时输出列名就是别名(ORDER BY 别名依赖这一点)
            std::string name = alias;
            if (name.empty()) {
                name = (e->kind == ast::ExprKind::Column) ? e->column_name : std::string("<expr>");
            }
            r.column_names.push_back(name);
            r.column_types.push_back(infer_type(*e, &slot_refs));
        }
    }

    // GROUP BY 别名/位置解析 (D1):GROUP BY select_alias / GROUP BY 1 → 对应 SELECT 表达式。
    // 裸列名能命中 FROM 列时保持原义(MySQL 优先 FROM 列,其次才是别名)。
    std::vector<const ast::Expression*> group_exprs;
    for (auto& [ge, asc] : s.group_by) {
        const ast::Expression* re = ge.get();
        if (ge->kind == ast::ExprKind::Literal && ge->literal.is_int()) {
            int idx = static_cast<int>(ge->literal.v_int) - 1;
            if (idx >= 0 && idx < static_cast<int>(s.columns.size()))
                re = s.columns[idx].first.get();
        } else if (ge->kind == ast::ExprKind::Column && ge->table_name.empty() &&
                   base_col_index.find(ge->column_name) == base_col_index.end()) {
            for (auto& [ce, alias] : s.columns) {
                if (!alias.empty() && _stricmp(alias.c_str(), ge->column_name.c_str()) == 0) {
                    re = ce.get();
                    break;
                }
            }
        }
        group_exprs.push_back(re);
    }

    // sql_mode 含 ONLY_FULL_GROUP_BY:未聚合的裸列必须在 GROUP BY 中(否则 1055)
    if (!s.group_by.empty()) {
        std::function<bool(const ast::Expression&, const ast::Expression&)> same_expr =
            [&](const ast::Expression& a, const ast::Expression& b) -> bool {
                if (a.kind != b.kind) return false;
                switch (a.kind) {
                    case ast::ExprKind::Column:
                        return _stricmp(a.column_name.c_str(), b.column_name.c_str()) == 0 &&
                               _stricmp(a.table_name.c_str(), b.table_name.c_str()) == 0;
                    case ast::ExprKind::Literal:
                        return a.literal.compare(b.literal) == 0 &&
                               a.literal.kind == b.literal.kind;
                    case ast::ExprKind::FunctionCall:
                        return _stricmp(a.func_name.c_str(), b.func_name.c_str()) == 0;
                    default: return false;
                }
            };
        for (auto& [ce, alias] : s.columns) {
            if (!ce || ce->kind != ast::ExprKind::Column) continue;   // 只查裸列
            if (contains_agg(*ce)) continue;
            bool in_group = false;
            for (auto* ge : group_exprs) if (ge && same_expr(*ce, *ge)) { in_group = true; break; }
            if (!in_group)
                return make_error(1055, "42000",
                    fmt::format("Expression #1 of SELECT list is not in GROUP BY clause and "
                                "contains nonaggregated column '{}'", ce->column_name));
        }
    }

    // ORDER BY 输出列位置/别名 → 输出列下标;未命中返回 -1
    auto order_output_index = [&](const ast::Expression& oe) -> int {
        if (oe.kind == ast::ExprKind::Literal && oe.literal.is_int()) {
            int idx = static_cast<int>(oe.literal.v_int) - 1;
            if (idx >= 0 && idx < static_cast<int>(r.column_names.size())) return idx;
            return -1;
        }
        if (oe.kind == ast::ExprKind::Column && oe.table_name.empty()) {
            for (size_t i = 0; i < r.column_names.size(); ++i) {
                const std::string& nm = r.column_names[i];
                auto dot = nm.rfind('.');
                std::string bare = (dot == std::string::npos) ? nm : nm.substr(dot + 1);
                if (_stricmp(nm.c_str(), oe.column_name.c_str()) == 0 ||
                    _stricmp(bare.c_str(), oe.column_name.c_str()) == 0)
                    return static_cast<int>(i);
            }
        }
        return -1;
    };

    // ---- 聚合下推到 PK 前缀探测 JOIN ----
    // 判据(保守): 最后一个 JOIN 为纯等值 PK 前缀探测;有 GROUP BY,无 HAVING/DISTINCT;
    // 分组键与 WHERE 只引用左表列;聚合参数只引用右表列;非聚合 SELECT 列只引用左表列。
    // 满足时边探测边累加,不再物化中间行 —— 中间行构造(每行 10 列、含多次字符串分配)
    // 是 3 表 JOIN 的决定性开销。
    struct FusedCol { int kind = 0; const ast::Expression* arg = nullptr; };
    std::vector<FusedCol> fused_cols;   // 0=非聚合 1=COUNT(*) 2=COUNT 3=SUM 4=AVG 5=MIN 6=MAX
    std::map<std::string, int> right_col_index;   // 右表列名 -> 行内局部下标
    int  fused_right_offset = 0, fused_right_cols = 0;
    bool fuse_agg = false;
    // 表达式中所有列引用是否都落在 [lo, hi) 的行偏移区间
    auto cols_in_range = [&](const ast::Expression& e, int lo, int hi) -> bool {
        std::function<bool(const ast::Expression&)> rec =
            [&](const ast::Expression& x) -> bool {
                // 子查询/CASE/占位符/星号: 引用面无法静态判定,一律不下推
                if (x.kind == ast::ExprKind::Subquery || x.kind == ast::ExprKind::Case ||
                    x.kind == ast::ExprKind::Placeholder || x.kind == ast::ExprKind::Star)
                    return false;
                if (x.kind == ast::ExprKind::Column) {
                    if (!x.column_name.empty() && x.column_name[0] == '@') return false;
                    std::string key = x.table_name.empty()
                        ? x.column_name : (x.table_name + "." + x.column_name);
                    auto it = base_col_index.find(key);
                    if (it == base_col_index.end()) return false;
                    return it->second >= lo && it->second < hi;
                }
                if (x.lhs && !rec(*x.lhs)) return false;
                if (x.rhs && !rec(*x.rhs)) return false;
                if (x.operand && !rec(*x.operand)) return false;
                for (auto& a : x.func_args) if (a && !rec(*a)) return false;
                return true;
            };
        return rec(e);
    };
    std::vector<const ast::Expression*> fused_left_preds, fused_right_preds;  // WHERE 按表侧拆分
    if (has_agg && !s.having && !s.distinct && !s.joins.empty()) {
        const auto& lastp = join_plans.back();
        // 仅 INNER: LEFT JOIN 无匹配需保留左行(聚合下推路径不做 NULL 补齐)
        // 探测方式: PK 前缀探测(不物化右表)或 哈希等值探测(需物化右表建桶)
        if (lastp.pure_equi && lastp.kind == ast::JoinKind::Inner &&
            (lastp.idx_probe || lastp.hash_join) &&
            slots.size() == s.joins.size() + 1) {
            auto& rslot = slots.back();
            fused_right_offset = rslot.col_offset;
            fused_right_cols   = static_cast<int>(rslot.def->columns.size());
            for (int i = 0; i < fused_right_cols; ++i) {
                const std::string& bare = rslot.def->columns[i].name;
                right_col_index[bare] = i;
                right_col_index[rslot.real_name + "." + bare] = i;
                if (!rslot.alias.empty()) right_col_index[rslot.alias + "." + bare] = i;
            }
            bool ok = true;
            for (auto* ge : group_exprs)
                if (!cols_in_range(*ge, 0, fused_right_offset)) { ok = false; break; }
            if (ok && s.where) {
                // WHERE 按 AND 拆分: 只引用左表列的在扫描阶段预过滤,只引用右表列的在探测时过滤;
                // 跨表引用/无法解析的连接词则整体回退到通用(物化)路径
                std::vector<const ast::Expression*> conj;
                std::function<void(const ast::Expression&)> split_and_where =
                    [&](const ast::Expression& e) {
                        if (e.kind == ast::ExprKind::BinaryOp && e.binop == ast::BinOp::And &&
                            e.lhs && e.rhs) { split_and_where(*e.lhs); split_and_where(*e.rhs); }
                        else conj.push_back(&e);
                    };
                split_and_where(*s.where);
                for (auto* c : conj) {
                    if (cols_in_range(*c, 0, fused_right_offset)) fused_left_preds.push_back(c);
                    else if (cols_in_range(*c, fused_right_offset, total_cols)) fused_right_preds.push_back(c);
                    else { ok = false; break; }
                }
            }
            if (ok) {
                for (auto& [e, _] : s.columns) {
                    FusedCol fc;
                    if (e->kind == ast::ExprKind::Star) { ok = false; break; }  // SELECT * 不走下推
                    if (e->kind == ast::ExprKind::FunctionCall && is_agg_func(e->func_name)) {
                        if (e->func_distinct) { ok = false; break; }   // DISTINCT 聚合不下推
                        std::string low = e->func_name;
                        std::transform(low.begin(), low.end(), low.begin(),
                                       [](unsigned char c) { return std::tolower(c); });
                        if (e->func_star_arg && low == "count") { fc.kind = 1; }
                        else if (e->func_args.empty() || !e->func_args[0]) { ok = false; break; }
                        else {
                            if (contains_agg(*e->func_args[0])) { ok = false; break; }  // 嵌套聚合不下推
                            if (!cols_in_range(*e->func_args[0], fused_right_offset, total_cols)) {
                                ok = false; break;
                            }
                            fc.arg = e->func_args[0].get();
                            fc.kind = (low == "count") ? 2 : (low == "sum") ? 3
                                    : (low == "avg")   ? 4 : (low == "min") ? 5 : 6;
                        }
                    } else {
                        if (contains_agg(*e) || !cols_in_range(*e, 0, fused_right_offset)) {
                            ok = false; break;
                        }
                    }
                    fused_cols.push_back(fc);
                }
            }
            // ORDER BY:输出列别名/位置可直接排序；其余表达式必须只引用左表列且不含聚合
            if (ok) {
                for (auto& [oe, asc] : s.order_by) {
                    if (order_output_index(*oe) >= 0) continue;
                    if (contains_agg(*oe) || !cols_in_range(*oe, 0, fused_right_offset)) {
                        ok = false; break;
                    }
                }
            }
            fuse_agg = ok;
        }
    }
    // 右表局部上下文: 聚合参数在右表行上直接求值,无需拼接整行
    auto build_right_ctx = [&](const storage::Row& r) {
        ExecContext ctx{ catalog_, primary_table, {}, {} };
        ctx.exec = this;
        ctx.row_ref = &r;
        ctx.idx_ref = &right_col_index;
        ctx.outer = outer;
        ctx.outer_used = outer_used;
        ctx.ctes = ctes;
        return ctx;
    };

    // 生成候选行:先主表遍历,CROSS/INNER/LEFT/RIGHT JOIN 追加
    std::vector<storage::Row> candidate_rows;

    // 先拿主表所有行 (MVCC:querySCN)
    uint64_t sqscn = query_scn(session_);
    uint64_t rtxn = reader_txn(session_);

    std::vector<storage::Row> left_rows;

    // ---- 主表 PK 首列等值 → 索引前缀探测,避免全表扫描 ----
    // 实测:employees WHERE emp_no=10001 走全表扫描 74ms(30 万行逐行 eval),
    // salaries(280 万行)同条件 678ms;PK 前缀探测可降到亚毫秒级。
    // 只认「PK 首列 = 同类型家族字面量」:索引按键存储类型严格比较,而 WHERE 求值允许
    // 隐式转换,跨类型下推会漏行,故此处置信域保守。
    const types::Value* pk_eq_lit = nullptr;
    types::Value pk_lit_storage;   // 外层列求值结果的存放处(相关子查询去相关用)
    if (s.where && !tbl->def().primary_key_cols.empty() &&
        !tbl->def().primary_key_cols.front().empty()) {
        int pk_off = tbl->def().column_index(tbl->def().primary_key_cols.front().front());
        if (pk_off >= 0 && pk_off < static_cast<int>(tbl->def().columns.size())) {
            types::SqlType pk_type = tbl->def().columns[pk_off].type.type;
            std::vector<const ast::Expression*> eq_conj;
            std::function<void(const ast::Expression&)> split_eq =
                [&](const ast::Expression& e) {
                    if (e.kind == ast::ExprKind::BinaryOp && e.binop == ast::BinOp::And &&
                        e.lhs && e.rhs) { split_eq(*e.lhs); split_eq(*e.rhs); }
                    else eq_conj.push_back(&e);
                };
            split_eq(*s.where);
            for (auto* c : eq_conj) {
                if (c->kind != ast::ExprKind::BinaryOp || c->binop != ast::BinOp::Eq) continue;
                const ast::Expression* col = nullptr;
                const ast::Expression* other = nullptr;
                if (c->lhs && c->lhs->kind == ast::ExprKind::Column && c->rhs) {
                    col = c->lhs.get(); other = c->rhs.get();
                } else if (c->rhs && c->rhs->kind == ast::ExprKind::Column && c->lhs) {
                    col = c->rhs.get(); other = c->lhs.get();
                }
                if (!col || !other) continue;
                std::string ck = col->table_name.empty()
                    ? col->column_name : (col->table_name + "." + col->column_name);
                auto it = base_col_index.find(ck);
                if (it == base_col_index.end() || it->second != pk_off) continue;
                // 等值右侧:字面量,或**外层列引用**(相关子查询去相关 ——
                // 外层列在一次子查询执行内是常量,可当字面量做 PK 前缀探测)。
                types::Value lit_val;
                if (other->kind == ast::ExprKind::Literal) {
                    lit_val = other->literal;
                } else if (other->kind == ast::ExprKind::Column && outer) {
                    // 带限定名且不在本查询列索引里 → 外层列;裸名可能撞本地列,保守跳过
                    std::string ok = other->table_name.empty()
                        ? other->column_name
                        : (other->table_name + "." + other->column_name);
                    if (base_col_index.find(ok) != base_col_index.end()) continue;
                    // 相关子查询:必须标记,否则结果会被当"非相关"缓存,只算第一行
                    if (outer_used) *outer_used = true;
                    lit_val = eval(*other, *outer);
                } else {
                    continue;
                }
                if (lit_val.is_null()) continue;
                bool same_family = (types::is_integer(pk_type) && lit_val.is_int()) ||
                                   (types::is_numeric(pk_type) && lit_val.is_numeric()) ||
                                   (types::is_string(pk_type) && lit_val.is_string()) ||
                                   (types::is_datetime(pk_type) && lit_val.is_string());
                if (!same_family) continue;
                pk_lit_storage = std::move(lit_val);
                pk_eq_lit = &pk_lit_storage;
                break;
            }
        }
    }

    // 存在逻辑删除历史时索引不再是全扫的超集(见 Table::has_dead_rows 说明),
    // 此时回退全表扫描以保证四个隔离级别下的可见性语义。
    const bool pk_probe_ok = pk_eq_lit && !tbl->has_dead_rows();

    // ---- 单表聚合快路径:一次扫描直接过滤 + 累加,不物化中间行 ----
    // 旧路径对 280 万行要做 left_rows / candidate_rows / buckets 三次全表行拷贝,
    // 扫描阶段逐行 build_ctx + eval(列/谓词)(每行拼字符串查 map),聚合阶段再来一遍 ——
    // SUM 约 2.9s、WHERE salary>100000 约 0.77s。
    // 触发条件保守:单表、无 GROUP BY/HAVING/DISTINCT/ORDER BY/LIMIT,投影列全是简单聚合,
    // WHERE 可拆成 AND 连接的「列 op 字面量」(op 仅比较符),且没有可走索引探测的 PK 等值。
    // 列偏移与谓词操作数在扫描前解析一次,回调里直接读 row[off] 并复用 eval 的比较语义。
    bool fast_agg = has_agg && s.joins.empty() && s.group_by.empty() &&
                    !s.having && !s.distinct && s.order_by.empty() &&
                    !s.offset && !s.limit && !pk_probe_ok;
    struct FastAcc {
        int kind = 0;   // 0=COUNT(*) 1=COUNT(col) 2=SUM/AVG 3=MIN 4=MAX 5=常量
        bool avg = false;
        int off = -1;
        int64_t cnt = 0;
        int64_t iacc = 0;
        double acc = 0;
        bool all_int = true;
        bool first = true;
        types::Value best;
    };
    struct FastPred {
        int off = -1;
        types::Value lit;
        ast::BinOp op = ast::BinOp::Eq;
        bool lit_left = false;   // 保留原式操作数顺序,literal.compare(col) 与 col.compare(literal) 可能不同
    };
    std::vector<FastAcc> faccs;
    std::vector<FastPred> fpreds;
    if (fast_agg) {
        faccs.reserve(s.columns.size());
        for (auto& [e, _] : s.columns) {
            if (!e) { fast_agg = false; break; }
            // 常量投影列(SELECT 'x', COUNT(*) ...):UNION ALL 分支常见,直接输出
            if (e->kind == ast::ExprKind::Literal) {
                FastAcc a;
                a.kind = 5;
                a.best = e->literal;
                faccs.push_back(std::move(a));
                continue;
            }
            if (e->kind != ast::ExprKind::FunctionCall) { fast_agg = false; break; }
            if (e->func_distinct) { fast_agg = false; break; }   // DISTINCT 聚合走通用路径
            std::string low = e->func_name;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            FastAcc a;
            if (low == "count" && e->func_star_arg) {
                a.kind = 0;
            } else if (low == "count" || low == "sum" || low == "avg" ||
                       low == "min" || low == "max") {
                if (e->func_args.size() != 1 || !e->func_args[0] ||
                    e->func_args[0]->kind != ast::ExprKind::Column) { fast_agg = false; break; }
                auto& c = *e->func_args[0];
                std::string key = c.table_name.empty()
                    ? c.column_name : (c.table_name + "." + c.column_name);
                auto it = base_col_index.find(key);
                if (it == base_col_index.end() ||
                    (!c.table_name.empty() && ambiguous_cols.count(c.column_name)) ||
                    it->second < 0 || it->second >= total_cols) { fast_agg = false; break; }
                a.off = it->second;
                a.kind = (low == "count") ? 1 : (low == "sum" || low == "avg") ? 2
                       : (low == "min") ? 3 : 4;
                a.avg = (low == "avg");
            } else {
                fast_agg = false; break;
            }
            faccs.push_back(std::move(a));
        }
    }
    if (fast_agg && s.where) {
        std::function<bool(const ast::Expression&)> add_pred =
            [&](const ast::Expression& e) -> bool {
                if (e.kind == ast::ExprKind::BinaryOp && e.binop == ast::BinOp::And &&
                    e.lhs && e.rhs)
                    return add_pred(*e.lhs) && add_pred(*e.rhs);
                if (e.kind != ast::ExprKind::BinaryOp || !e.lhs || !e.rhs) return false;
                switch (e.binop) {
                    case ast::BinOp::Eq: case ast::BinOp::Ne: case ast::BinOp::Lt:
                    case ast::BinOp::Gt: case ast::BinOp::Le: case ast::BinOp::Ge: break;
                    default: return false;
                }
                const ast::Expression* col = nullptr;
                const ast::Expression* lit = nullptr;
                bool lit_left = false;
                if (e.lhs->kind == ast::ExprKind::Column &&
                    e.rhs->kind == ast::ExprKind::Literal) {
                    col = e.lhs.get(); lit = e.rhs.get();
                } else if (e.rhs->kind == ast::ExprKind::Column &&
                           e.lhs->kind == ast::ExprKind::Literal) {
                    col = e.rhs.get(); lit = e.lhs.get(); lit_left = true;
                } else {
                    return false;
                }
                // @@系统变量在 eval(Column) 里被特殊拦截,不能当普通列处理
                if (col->column_name.size() >= 2 && col->column_name[0] == '@') return false;
                std::string key = col->table_name.empty()
                    ? col->column_name : (col->table_name + "." + col->column_name);
                auto it = base_col_index.find(key);
                if (it == base_col_index.end() || it->second < 0 ||
                    it->second >= total_cols) return false;
                fpreds.push_back({it->second, lit->literal, e.binop, lit_left});
                return true;
            };
        if (!add_pred(*s.where)) fast_agg = false;
    }
    if (fast_agg) {
        tbl->for_each_visible(sqscn, [&](int64_t, const storage::Row& row) {
            for (auto& p : fpreds) {
                if (p.off >= static_cast<int>(row.size())) return true;   // 缺列 → NULL → 不匹配
                const types::Value& l = p.lit_left ? p.lit : row[p.off];
                const types::Value& r = p.lit_left ? row[p.off] : p.lit;
                if (l.is_null() || r.is_null()) return true;             // UNKNOWN → WHERE 不匹配
                bool pass = false;
                // Value::compare 对两个数值统一转 double;整数在 2^53 内转换无损,
                // 这里直接比 int64(常见 salary>100000 热路径)可省两次 try_to_double。
                constexpr int64_t kExactDoubleMax = int64_t{1} << 53;
                if (l.is_int() && r.is_int() &&
                    l.v_int >= -kExactDoubleMax && l.v_int <= kExactDoubleMax &&
                    r.v_int >= -kExactDoubleMax && r.v_int <= kExactDoubleMax) {
                    switch (p.op) {
                        case ast::BinOp::Eq: pass = l.v_int == r.v_int; break;
                        case ast::BinOp::Ne: pass = l.v_int != r.v_int; break;
                        case ast::BinOp::Lt: pass = l.v_int <  r.v_int; break;
                        case ast::BinOp::Gt: pass = l.v_int >  r.v_int; break;
                        case ast::BinOp::Le: pass = l.v_int <= r.v_int; break;
                        case ast::BinOp::Ge: pass = l.v_int >= r.v_int; break;
                        default: break;
                    }
                } else {
                    switch (p.op) {
                        case ast::BinOp::Eq: pass = compare_mysql(l, r) == 0; break;
                        case ast::BinOp::Ne: pass = compare_mysql(l, r) != 0; break;
                        case ast::BinOp::Lt: pass = compare_mysql(l, r) <  0; break;
                        case ast::BinOp::Gt: pass = compare_mysql(l, r) >  0; break;
                        case ast::BinOp::Le: pass = compare_mysql(l, r) <= 0; break;
                        case ast::BinOp::Ge: pass = compare_mysql(l, r) >= 0; break;
                        default: break;
                    }
                }
                if (!pass) return true;
            }
            for (auto& a : faccs) {
                const types::Value* v = nullptr;
                if (a.kind != 0 && a.kind != 5) {
                    if (a.off >= static_cast<int>(row.size())) continue;
                    v = &row[a.off];
                    if (v->is_null()) continue;
                }
                switch (a.kind) {
                    case 5:
                        break;   // 常量列无需累加
                    case 0: case 1:
                        ++a.cnt;
                        break;
                    case 2:
                        if (v->is_int()) {
                            a.iacc += v->v_int;
                            a.acc += static_cast<double>(v->v_int);
                        } else {
                            a.all_int = false;
                            double d;
                            if (v->try_to_double(d)) a.acc += d;
                        }
                        ++a.cnt;
                        break;
                    default:
                        if (a.first) { a.best = *v; a.first = false; }
                        else {
                            int cv = v->compare(a.best);
                            if ((a.kind == 3 && cv < 0) || (a.kind == 4 && cv > 0)) a.best = *v;
                        }
                        ++a.cnt;
                        break;
                }
            }
            return true;
        }, rtxn);
        storage::Row out;
        out.reserve(faccs.size());
        for (auto& a : faccs) {
            if (a.kind == 5) { out.push_back(a.best); continue; }
            if (a.kind == 0 || a.kind == 1) out.push_back(types::Value::make_int(a.cnt));
            else if (a.kind == 2) {
                if (a.cnt == 0) out.push_back(types::Value::make_null());
                else if (a.avg) out.push_back(types::Value::make_double(a.acc / a.cnt));
                else if (a.all_int) out.push_back(types::Value::make_int(a.iacc));
                else out.push_back(types::Value::make_double(a.acc));
            } else {
                out.push_back(a.first ? types::Value::make_null() : a.best);
            }
        }
        r.rows.push_back(std::move(out));
        r.affected_rows = static_cast<int64_t>(r.rows.size());
        return r;
    }

    if (pk_probe_ok) {
        // 索引探测:只访问首列匹配的键(谓词仍逐行复核,保证与全扫语义一致)
        tbl->for_each_by_pk_prefix(*pk_eq_lit, sqscn, [&](int64_t, const storage::Row& row) {
            for (auto* c : pushdown_conjuncts) {
                auto ctx = build_ctx(row);
                if (!to_bool(eval(*c, ctx))) return true;
            }
            left_rows.push_back(row);
            return true;
        }, rtxn);
    } else if (pushdown_conjuncts.empty()) {
        tbl->for_each_visible(sqscn, [&](int64_t, const storage::Row& row) { left_rows.push_back(row); return true; }, rtxn);
    } else {
        // 下推谓词预过滤:只把通过主表局部条件的行送入 JOIN
        tbl->for_each_visible(sqscn, [&](int64_t, const storage::Row& row) {
            auto ctx = build_ctx(row);
            for (auto* c : pushdown_conjuncts)
                if (!to_bool(eval(*c, ctx))) return true;   // 预过滤掉,继续扫描
            left_rows.push_back(row);
            return true;
        }, rtxn);
    }

    bool fused_done = false;                      // 聚合下推已直接产出结果
    std::vector<storage::Row> fused_agg_rows;     // 每个分组一行(SELECT 投影)
    std::vector<storage::Row> fused_reps;         // 每个分组的代表性整行(供 ORDER BY / 非聚合列)

    for (size_t ji = 0; ji < s.joins.size(); ++ji) {
        auto& jc = s.joins[ji];
        auto* jtbl = resolve_table(jc.table_name);
        const JoinPlan& plan = join_plans[ji];

        // ---- 聚合下推:最后一个 PK 前缀探测 JOIN 直接边探测边聚合,不物化中间行 ----
        if (fuse_agg && ji + 1 == s.joins.size()) {
            struct FAcc {
                int64_t c = 0, iacc = 0;
                double  acc = 0;
                bool    all_int = true, first = true;
                types::Value best;
            };
            struct FGrp {
                storage::Row rep;
                std::vector<FAcc> accs;
            };
            std::vector<FGrp> grps;
            std::unordered_map<std::string, size_t> gmap;
            const int rcol = plan.equi.right_col;
            const int lcol = plan.equi.left_col;
            // 哈希探测所需: 右表物化 + 等值桶(仅非 idx_probe 路径)
            std::vector<storage::Row> fr_right;
            std::unordered_map<std::string, std::vector<size_t>> fr_buckets;
            if (!plan.idx_probe) {
                jtbl->for_each_visible(sqscn, [&](int64_t, const storage::Row& row) {
                    fr_right.push_back(row); return true;
                }, rtxn);
                for (size_t ri = 0; ri < fr_right.size(); ++ri) {
                    if (rcol < 0 || rcol >= static_cast<int>(fr_right[ri].size())) continue;
                    std::string hk = join_hash_key(fr_right[ri][rcol]);
                    if (!hk.empty()) fr_buckets[std::move(hk)].push_back(ri);
                }
            }
            for (auto& l : left_rows) {
                if (lcol < 0 || lcol >= static_cast<int>(l.size())) continue;
                if (static_cast<int>(l.size()) < fused_right_offset) continue;  // 左行必须是完整前缀
                bool pre_skip = false;
                for (auto* c : fused_left_preds) {                              // 左表侧 WHERE 预过滤
                    auto wctx = build_ctx(l);
                    if (!to_bool(eval(*c, wctx))) { pre_skip = true; break; }
                }
                if (pre_skip) continue;
                const auto& keyv = l[lcol];
                if (keyv.is_null()) continue;
                std::string key;
                {
                    auto kctx = build_ctx(l);
                    for (auto* ge : group_exprs) key += eval(*ge, kctx).to_string() + "\x01";
                }
                FGrp* gp = nullptr;
                auto git = gmap.find(key);
                if (git != gmap.end()) gp = &grps[git->second];
                bool any = false;
                // 单条匹配行: 首次命中建组(含代表性整行) + 累加聚合
                auto process_match = [&](const storage::Row& r) {
                    if (!any) {
                        any = true;
                        if (!gp) {
                            grps.push_back(FGrp{});
                            gp = &grps.back();
                            gp->accs.resize(fused_cols.size());
                            gp->rep = l;                       // 仅首次命中构造一次
                            gp->rep.resize(total_cols);
                            gmap.emplace(key, grps.size() - 1);
                        }
                        for (int i = 0; i < fused_right_cols && i < static_cast<int>(r.size()); ++i)
                            gp->rep[fused_right_offset + i] = r[i];
                    }
                    for (size_t ci = 0; ci < fused_cols.size(); ++ci) {
                        const auto& fc = fused_cols[ci];
                        if (fc.kind == 0) continue;
                        auto& a = gp->accs[ci];
                        if (fc.kind == 1) { ++a.c; continue; }          // COUNT(*)
                        if (!fc.arg) continue;
                        auto v = eval(*fc.arg, build_right_ctx(r));
                        if (v.is_null()) continue;
                        if (fc.kind == 2) ++a.c;                       // COUNT(col)
                        else if (fc.kind == 3 || fc.kind == 4) {       // SUM / AVG
                            if (v.is_int()) { a.iacc += v.v_int; a.acc += static_cast<double>(v.v_int); }
                            else {
                                a.all_int = false;
                                double d; if (v.try_to_double(d)) a.acc += d;
                            }
                            ++a.c;
                        } else {                                       // MIN / MAX
                            if (a.first) { a.best = v; a.first = false; }
                            else {
                                int cv = v.compare(a.best);
                                if ((fc.kind == 5 && cv < 0) || (fc.kind == 6 && cv > 0)) a.best = v;
                            }
                            ++a.c;
                        }
                    }
                };
                // 右表侧 WHERE: 不满足则跳过该匹配行
                auto pass_right_preds = [&](const storage::Row& r) {
                    for (auto* c : fused_right_preds) {
                        auto rc = build_right_ctx(r);
                        if (!to_bool(eval(*c, rc))) return false;
                    }
                    return true;
                };
                if (plan.idx_probe) {
                    jtbl->for_each_by_pk_prefix(keyv, sqscn,
                        [&](int64_t, const storage::Row& r) {
                            if (rcol < 0 || rcol >= static_cast<int>(r.size())) return true;
                            if (!keyv.sql_equals(r[rcol])) return true;
                            if (!pass_right_preds(r)) return true;     // 索引已过滤的键不受影响
                            process_match(r);
                            return true;
                        }, rtxn);
                } else {
                    std::string hk = join_hash_key(keyv);
                    auto bit = hk.empty() ? fr_buckets.end() : fr_buckets.find(hk);
                    if (bit != fr_buckets.end()) {
                        for (size_t ri : bit->second) {
                            const auto& r = fr_right[ri];
                            if (rcol < 0 || rcol >= static_cast<int>(r.size())) continue;
                            if (!keyv.sql_equals(r[rcol])) continue;
                            if (!pass_right_preds(r)) continue;
                            process_match(r);
                        }
                    }
                }
            }
            // 无 GROUP BY 时,空输入也要产出一行(COUNT=0 / SUM=NULL,与 MySQL 一致)
            if (s.group_by.empty() && grps.empty()) {
                grps.push_back(FGrp{});
                grps.back().accs.resize(fused_cols.size());
            }
            for (auto& g : grps) {
                storage::Row row;
                auto rctx = build_ctx(g.rep);
                for (size_t ci = 0; ci < fused_cols.size(); ++ci) {
                    const auto& fc = fused_cols[ci];
                    if (fc.kind == 0) { row.push_back(eval(*s.columns[ci].first, rctx)); continue; }
                    auto& a = g.accs[ci];
                    if (fc.kind == 1 || fc.kind == 2) row.push_back(types::Value::make_int(a.c));
                    else if (fc.kind == 3) row.push_back(a.c == 0 ? types::Value::make_null()
                                             : (a.all_int ? types::Value::make_int(a.iacc)
                                                          : types::Value::make_double(a.acc)));
                    else if (fc.kind == 4) row.push_back(a.c == 0 ? types::Value::make_null()
                                             : types::Value::make_double(a.acc / static_cast<double>(a.c)));
                    else row.push_back(a.first ? types::Value::make_null() : a.best);
                }
                fused_agg_rows.push_back(std::move(row));
                fused_reps.push_back(std::move(g.rep));
            }
            fused_done = true;
            break;
        }

        // 右表物化 + 哈希桶:仅哈希/嵌套循环路径需要。
        // PK 前缀探测(idx_probe)通过索引直接取右表行,若仍物化整表(如 salaries 280 万行)
        // 并逐行 to_string 建桶,等于白白付出全表拷贝与哈希成本。
        std::vector<storage::Row> right_rows;
        std::unordered_map<std::string, std::vector<size_t>> buckets;
        if (!plan.idx_probe) {
            jtbl->for_each_visible(sqscn, [&](int64_t, const storage::Row& row) { right_rows.push_back(row); return true; }, rtxn);
            if (plan.hash_join) {
                for (size_t ri = 0; ri < right_rows.size(); ++ri) {
                    if (plan.equi.right_col < 0 ||
                        plan.equi.right_col >= static_cast<int>(right_rows[ri].size())) continue;
                    std::string key = join_hash_key(right_rows[ri][plan.equi.right_col]);
                    if (!key.empty()) buckets[std::move(key)].push_back(ri);
                }
            }
        }

        std::vector<storage::Row> new_left;
        const bool is_right_join = (jc.kind == ast::JoinKind::Right);
        std::vector<bool> right_matched(is_right_join ? right_rows.size() : 0, false);
        const size_t left_width = (ji + 1 < slots.size())
            ? static_cast<size_t>(slots[ji + 1].col_offset) : static_cast<size_t>(total_cols);
        for (auto& l : left_rows) {
            if (jc.kind == ast::JoinKind::Cross) {
                for (auto& r : right_rows) {
                    storage::Row flat = l; flat.insert(flat.end(), r.begin(), r.end());
                    new_left.push_back(std::move(flat));
                }
                continue;
            }
            if (!is_right_join) {
            if (jc.kind != ast::JoinKind::Inner && jc.kind != ast::JoinKind::Left &&
                jc.kind != ast::JoinKind::Right) continue;

            std::vector<storage::Row> matches;
            if (plan.idx_probe && plan.equi.left_col >= 0 &&
                plan.equi.left_col < static_cast<int>(l.size())) {
                // ---- PK 前缀索引探测:逐左行查右表 PK AVL,右表零物化 ----
                const auto& keyv = l[plan.equi.left_col];
                if (!keyv.is_null()) {
                    jtbl->for_each_by_pk_prefix(keyv, sqscn,
                        [&](int64_t, const storage::Row& r) {
                            if (plan.equi.right_col < 0 ||
                                plan.equi.right_col >= static_cast<int>(r.size()) ||
                                !keyv.sql_equals(r[plan.equi.right_col])) return true;
                            if (plan.pure_equi) {
                                // 纯等值:前缀扫描已保证首列相等
                                storage::Row flat = l; flat.insert(flat.end(), r.begin(), r.end());
                                matches.push_back(std::move(flat));
                            } else {
                                // 复合 ON:等值项已由索引保证,其余谓词完整 eval 兜底
                                storage::Row flat = l; flat.insert(flat.end(), r.begin(), r.end());
                                auto ctx = build_ctx(flat);
                                if (to_bool(eval(*jc.on, ctx))) matches.push_back(std::move(flat));
                            }
                            return true;
                        }, rtxn);
                }
            } else if (plan.hash_join && plan.equi.left_col >= 0 &&
                plan.equi.left_col < static_cast<int>(l.size())) {
                std::string key = join_hash_key(l[plan.equi.left_col]);
                auto it = key.empty() ? buckets.end() : buckets.find(key);
                if (it != buckets.end()) {
                    for (size_t ri : it->second) {
                        const auto& r = right_rows[ri];
                        // 纯等值: hash key 已保证相等,只需 O(1) 复核(避免 to_string 撞 key)
                        // 复合 ON (等值 AND 其它谓词): 仍完整 eval 兜底,语义与旧实现一致
                        if (plan.pure_equi) {
                            if (plan.equi.right_col >= static_cast<int>(r.size()) ||
                                !l[plan.equi.left_col].sql_equals(r[plan.equi.right_col])) continue;
                        } else {
                            storage::Row flat = l; flat.insert(flat.end(), r.begin(), r.end());
                            auto ctx = build_ctx(flat);
                            if (!to_bool(eval(*jc.on, ctx))) continue;
                        }
                        storage::Row flat = l; flat.insert(flat.end(), r.begin(), r.end());
                        matches.push_back(std::move(flat));
                    }
                }
            } else {
                // ---- 退化路径: 非等值 / CROSS / RIGHT / 无 ON ----
                for (size_t ri = 0; ri < right_rows.size(); ++ri) {
                    auto& r = right_rows[ri];
                    storage::Row flat = l; flat.insert(flat.end(), r.begin(), r.end());
                    auto ctx = build_ctx(flat);
                    bool ok = true;
                    if (jc.on) ok = to_bool(eval(*jc.on, ctx));
                    if (ok) matches.push_back(std::move(flat));
                }
            }
            if (!matches.empty()) {
                for (auto& m : matches) new_left.push_back(std::move(m));
            } else if (jc.kind == ast::JoinKind::Left) {
                // LEFT JOIN:保留左表,右表列 NULL
                storage::Row flat = l;
                for (size_t i = 0; i < jtbl->def().columns.size(); ++i)
                    flat.push_back(types::Value::make_null());
                new_left.push_back(std::move(flat));
            }
            continue;
            }

            // ---- RIGHT JOIN:保留右表,左表列 NULL(对未匹配的右行补空行) ----
            for (size_t ri = 0; ri < right_rows.size(); ++ri) {
                auto& r = right_rows[ri];
                storage::Row flat = l; flat.insert(flat.end(), r.begin(), r.end());
                auto ctx = build_ctx(flat);
                bool ok = true;
                if (jc.on) ok = to_bool(eval(*jc.on, ctx));
                if (ok) {
                    right_matched[ri] = true;
                    new_left.push_back(std::move(flat));
                }
            }
        }
        if (is_right_join) {
            for (size_t ri = 0; ri < right_rows.size(); ++ri) {
                if (right_matched[ri]) continue;
                storage::Row flat(left_width, types::Value::make_null());
                flat.insert(flat.end(), right_rows[ri].begin(), right_rows[ri].end());
                new_left.push_back(std::move(flat));
            }
        }
        left_rows = std::move(new_left);
    }

    // 聚合下推路径:结果已产出,补齐 ORDER BY / LIMIT 后直接返回
    if (fused_done) {
        if (!s.order_by.empty() && fused_agg_rows.size() > 1) {
            struct FTerm { int out_idx = -1; const ast::Expression* expr = nullptr; bool asc = true; };
            std::vector<FTerm> fterms;
            for (auto& [oe, asc] : s.order_by) {
                FTerm t;
                t.asc = asc;
                t.out_idx = order_output_index(*oe);
                if (t.out_idx < 0) t.expr = oe.get();
                fterms.push_back(t);
            }
            std::vector<size_t> ord(fused_agg_rows.size());
            for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
            std::stable_sort(ord.begin(), ord.end(), [&](size_t ia, size_t ib) {
                for (auto& t : fterms) {
                    types::Value va, vb;
                    if (t.out_idx >= 0) {
                        va = t.out_idx < static_cast<int>(fused_agg_rows[ia].size())
                           ? fused_agg_rows[ia][t.out_idx] : types::Value::make_null();
                        vb = t.out_idx < static_cast<int>(fused_agg_rows[ib].size())
                           ? fused_agg_rows[ib][t.out_idx] : types::Value::make_null();
                    } else {
                        auto ca = build_ctx(fused_reps[ia]);
                        auto cb = build_ctx(fused_reps[ib]);
                        va = eval(*t.expr, ca);
                        vb = eval(*t.expr, cb);
                    }
                    int c = va.compare(vb);
                    if (c != 0) return t.asc ? c < 0 : c > 0;
                }
                return false;
            });
            std::vector<storage::Row> sorted;
            sorted.reserve(fused_agg_rows.size());
            for (size_t i : ord) sorted.push_back(std::move(fused_agg_rows[i]));
            fused_agg_rows = std::move(sorted);
        }
        int64_t off = s.offset.value_or(0);
        int64_t lim = s.limit.value_or(static_cast<int64_t>(fused_agg_rows.size()));
        if (off > 0) {
            if (static_cast<size_t>(off) >= fused_agg_rows.size()) fused_agg_rows.clear();
            else fused_agg_rows.erase(fused_agg_rows.begin(), fused_agg_rows.begin() + off);
        }
        if (lim >= 0 && static_cast<size_t>(lim) < fused_agg_rows.size())
            fused_agg_rows.resize(static_cast<size_t>(lim));
        r.rows = std::move(fused_agg_rows);
        r.affected_rows = static_cast<int64_t>(r.rows.size());
        return r;
    }

    // 没有 JOIN —— 主表行直接放
    if (s.joins.empty()) {
        for (auto& l : left_rows) {
            storage::Row flat = l;
            while (static_cast<int>(flat.size()) < total_cols) flat.push_back(types::Value::make_null());
            candidate_rows.push_back(std::move(flat));
        }
    } else {
        candidate_rows = std::move(left_rows);
    }

    // WHERE
    std::vector<storage::Row> after_where;
    for (auto& row : candidate_rows) {
        auto ctx = build_ctx(row);
        bool match = true;
        if (s.where) {
            auto v = eval(*s.where, ctx);
            match = to_bool(v);
        }
        if (match) after_where.push_back(std::move(row));
    }
    if (!eval_error_.empty()) return take_eval_error();

    auto compute_row = [&](storage::Row& flat_row) -> storage::Row {
        storage::Row out;
        auto ctx = build_ctx(flat_row);
        for (auto& [e, _] : s.columns) {
            if (e->kind == ast::ExprKind::Star) {
                for (auto& slot : slots) {
                    for (size_t i = 0; i < slot.def->columns.size(); ++i) {
                        out.push_back(flat_row[slot.col_offset + static_cast<int>(i)]);
                    }
                }
            } else if (e->is_window) {
                out.push_back(types::Value::make_null());   // 占位,投影后统一计算
            } else {
                out.push_back(eval(*e, ctx));
            }
        }
        return out;
    };

    if (has_agg) {
        // M3:GROUP BY — 按 group_by 表达式分桶,每桶独立聚合
        struct Bucket {
            std::vector<storage::Row> rows;   // flat rows
        };
        std::vector<Bucket> buckets;
        // key -> bucket 下标。旧实现线性扫描 bucket_keys,GROUP BY 高基数时是 O(组数²):
        // salaries GROUP BY emp_no(30 万组 / 280 万行)因此跑到 100s 以上。
        std::unordered_map<std::string, size_t> bucket_index;

        // 生成每个 flat row 的分组 key (使用别名/位置解析后的分组表达式)
        auto group_key_str = [&](const storage::Row& flat) -> std::string {
            if (group_exprs.empty()) return "";   // 空 group_by = 单组 (MySQL 允许,等效全表一组)
            auto ctx = build_ctx(flat);
            std::string k;
            for (auto* ge : group_exprs) {
                auto v = eval(*ge, ctx);
                k += v.to_string() + "\x01";
            }
            return k;
        };

        // 按 key 分桶 (保持输入顺序,稳定)
        for (auto& flat : after_where) {
            std::string key = group_key_str(flat);
            auto it = bucket_index.find(key);
            size_t bi;
            if (it == bucket_index.end()) {
                bi = buckets.size();
                buckets.push_back({});
                bucket_index.emplace(std::move(key), bi);
            } else {
                bi = it->second;
            }
            buckets[bi].rows.push_back(std::move(flat));
        }
        // 无 GROUP BY 时,空输入也要产出一行(COUNT=0 / SUM=NULL,与 MySQL 一致);
        // 有 GROUP BY 时,空输入必须返回空集(不能凭空造一个 NULL 分组)。
        if (buckets.empty() && group_exprs.empty()) buckets.push_back({});

        // 聚合计算(流式累加,支持 DISTINCT):对整桶求值
        auto compute_agg = [&](const ast::Expression& fe,
                               const std::vector<storage::Row>& brows) -> types::Value {
            std::string low = ci_lower(fe.func_name);
            if (low == "count" && fe.func_star_arg)
                return types::Value::make_int(static_cast<int64_t>(brows.size()));
            if (low == "count" && fe.func_distinct && fe.func_args.size() > 1) {
                // COUNT(DISTINCT a, b):按元组去重;任一列为 NULL 的行不计入。
                std::unordered_set<std::string> seen;
                for (auto& row : brows) {
                    std::string key;
                    bool has_null = false;
                    for (auto& arg : fe.func_args) {
                        auto v = eval(*arg, build_ctx(row));
                        if (v.is_null()) { has_null = true; break; }
                        key += v.to_string();
                        key.push_back('\x01');
                    }
                    if (!has_null) seen.insert(std::move(key));
                }
                return types::Value::make_int(static_cast<int64_t>(seen.size()));
            }
            if (fe.func_args.empty() || !fe.func_args[0]) return types::Value::make_null();
            double acc = 0; int64_t iacc = 0; int64_t c = 0;
            bool all_int = true, first = true;
            types::Value best;
            std::string gcat;
            bool gcat_any = false;
            std::unordered_set<std::string> dseen;
            for (auto& row : brows) {
                auto v = eval(*fe.func_args[0], build_ctx(row));
                if (v.is_null()) continue;
                if (fe.func_distinct && !dseen.insert(v.to_string()).second) continue;
                if (low == "count") { ++c; continue; }
                if (low == "group_concat") {
                    if (gcat_any) gcat.push_back(',');
                    gcat += v.is_string() ? v.v_str : v.to_string();
                    gcat_any = true;
                    ++c;
                    continue;
                }
                if (low == "min" || low == "max") {
                    if (first) { best = v; first = false; }
                    else {
                        int cv = v.compare(best);
                        if ((low == "min" && cv < 0) || (low == "max" && cv > 0)) best = v;
                    }
                } else if (v.is_int()) {
                    iacc += v.v_int; acc += static_cast<double>(v.v_int);
                } else {
                    all_int = false;
                    double d; if (v.try_to_double(d)) acc += d;
                }
                ++c;
            }
            if (low == "count") return types::Value::make_int(c);
            if (low == "group_concat")
                return gcat_any ? types::Value::make_string(gcat)
                                : types::Value::make_null();
            // SQL 标准:空集 SUM / AVG 返回 NULL (只有 COUNT 返回 0)
            if (c == 0) return types::Value::make_null();
            if (low == "avg") return types::Value::make_double(acc / static_cast<double>(c));
            if (low == "sum") return all_int ? types::Value::make_int(iacc)
                                             : types::Value::make_double(acc);
            return first ? types::Value::make_null() : best;
        };

        // 聚合上下文统一求值器 (D2):
        // 聚合对整桶计算；标量函数/算术先递归求值参数再套用函数本身(ROUND(AVG(x),2) 等);
        // 别名解析到 SELECT 表达式;普通列取桶内第一行(ANY_VALUE 语义)。
        std::function<types::Value(const ast::Expression&, const std::vector<storage::Row>&)> eval_agg;
        eval_agg = [&](const ast::Expression& expr,
                       const std::vector<storage::Row>& brows) -> types::Value {
            auto scalar_ctx = [&]() {
                ExecContext ctx0{ catalog_, "", {}, {} };
                ctx0.exec = this;
                ctx0.outer = outer;
                ctx0.outer_used = outer_used;
                ctx0.ctes = ctes;
                if (!brows.empty()) {
                    ctx0.row_ref = &brows.front();
                    ctx0.idx_ref = &base_col_index;
                }
                return ctx0;
            };
            switch (expr.kind) {
                case ast::ExprKind::Literal:
                    return expr.literal;
                case ast::ExprKind::Column: {
                    if (expr.table_name.empty()) {
                        for (auto& [ce, alias] : s.columns)
                            if (!alias.empty() &&
                                _stricmp(alias.c_str(), expr.column_name.c_str()) == 0)
                                return eval_agg(*ce, brows);
                    }
                    if (brows.empty()) return types::Value::make_null();
                    return eval(expr, build_ctx(brows.front()));
                }
                case ast::ExprKind::BinaryOp: {
                    auto l = eval_agg(*expr.lhs, brows);
                    if (expr.binop == ast::BinOp::And || expr.binop == ast::BinOp::Or) {
                        bool is_and = (expr.binop == ast::BinOp::And);
                        if (!l.is_null() && to_bool(l) != is_and)
                            return types::Value::make_bool(!is_and);
                        auto rr = eval_agg(*expr.rhs, brows);
                        auto decisive = [&](const types::Value& v) {
                            return !v.is_null() && (to_bool(v) != is_and);
                        };
                        if (decisive(l) || decisive(rr)) return types::Value::make_bool(!is_and);
                        if (l.is_null() || rr.is_null()) return types::Value::make_null();
                        return types::Value::make_bool(is_and);
                    }
                    auto rr = eval_agg(*expr.rhs, brows);
                    // 用字面量临时节点复用 eval 的运算/NULL 语义
                    ast::Expression tmp;
                    tmp.kind = ast::ExprKind::BinaryOp;
                    tmp.binop = expr.binop;
                    tmp.lhs = ast::Expression::make_literal(std::move(l));
                    tmp.rhs = ast::Expression::make_literal(std::move(rr));
                    auto ctx0 = scalar_ctx();
                    return eval(tmp, ctx0);
                }
                case ast::ExprKind::UnaryOp: {
                    auto v = eval_agg(*expr.operand, brows);
                    ast::Expression tmp;
                    tmp.kind = ast::ExprKind::UnaryOp;
                    tmp.unary = expr.unary;
                    tmp.operand = ast::Expression::make_literal(std::move(v));
                    auto ctx0 = scalar_ctx();
                    return eval(tmp, ctx0);
                }
                case ast::ExprKind::FunctionCall: {
                    std::string low = ci_lower(expr.func_name);
                    if (is_agg_func(low)) return compute_agg(expr, brows);
                    // 标量函数:参数先按聚合上下文求值,再套函数本身
                    std::vector<ast::ExprPtr> tmp_args;
                    tmp_args.reserve(expr.func_args.size());
                    for (auto& a : expr.func_args)
                        tmp_args.push_back(ast::Expression::make_literal(eval_agg(*a, brows)));
                    auto ctx0 = scalar_ctx();
                    return eval_function(expr.func_name, tmp_args, expr.func_star_arg, ctx0);
                }
                case ast::ExprKind::Case: {
                    if (expr.case_base) {
                        auto bv = eval_agg(*expr.case_base, brows);
                        for (auto& [w, rr] : expr.case_whens) {
                            auto wv = eval_agg(*w, brows);
                            if (!bv.is_null() && !wv.is_null() && bv.compare(wv) == 0)
                                return eval_agg(*rr, brows);
                        }
                    } else {
                        for (auto& [cnd, rr] : expr.case_whens)
                            if (to_bool(eval_agg(*cnd, brows))) return eval_agg(*rr, brows);
                    }
                    return expr.case_else ? eval_agg(*expr.case_else, brows)
                                          : types::Value::make_null();
                }
                case ast::ExprKind::Subquery: {
                    auto ctx0 = scalar_ctx();
                    return eval(expr, ctx0);
                }
                default:
                    return types::Value::make_null();
            }
        };

        // 对每个桶算聚合行
        auto agg_one_bucket = [&](const std::vector<storage::Row>& flat_rows) -> storage::Row {
            storage::Row agg_row;
            for (auto& [e, _] : s.columns) agg_row.push_back(eval_agg(*e, flat_rows));
            return agg_row;
        };

        // 算每个桶(保留桶下标,供 HAVING / ORDER BY 回到原始行)
        struct AggOut { storage::Row row; size_t bi; };
        std::vector<AggOut> outs;
        outs.reserve(buckets.size());
        for (size_t bi = 0; bi < buckets.size(); ++bi)
            outs.push_back({ agg_one_bucket(buckets[bi].rows), bi });
        if (!eval_error_.empty()) return take_eval_error();

        // HAVING 过滤:聚合/别名/普通列统一走 eval_agg
        if (s.having) {
            std::vector<AggOut> filtered;
            for (auto& o : outs)
                if (to_bool(eval_agg(*s.having, buckets[o.bi].rows)))
                    filtered.push_back(std::move(o));
            outs = std::move(filtered);
            if (!eval_error_.empty()) return take_eval_error();
        }

        // ORDER BY:输出列别名/位置直接取投影值;其余表达式按聚合上下文对桶求值
        if (!s.order_by.empty() && outs.size() > 1) {
            struct OTerm { int out_idx = -1; const ast::Expression* expr = nullptr; bool asc = true; };
            std::vector<OTerm> terms;
            for (auto& [oe, asc] : s.order_by) {
                OTerm t;
                t.asc = asc;
                t.out_idx = order_output_index(*oe);
                if (t.out_idx < 0) t.expr = oe.get();
                terms.push_back(t);
            }
            // 排序键预计算一次,避免比较器里重复算聚合
            std::vector<std::vector<types::Value>> keys(outs.size());
            for (size_t i = 0; i < outs.size(); ++i) {
                keys[i].reserve(terms.size());
                for (auto& t : terms) {
                    if (t.out_idx >= 0) {
                        keys[i].push_back(t.out_idx < static_cast<int>(outs[i].row.size())
                                          ? outs[i].row[t.out_idx] : types::Value::make_null());
                    } else {
                        keys[i].push_back(eval_agg(*t.expr, buckets[outs[i].bi].rows));
                    }
                }
            }
            if (!eval_error_.empty()) return take_eval_error();
            std::vector<size_t> ord(outs.size());
            for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
            std::stable_sort(ord.begin(), ord.end(), [&](size_t ia, size_t ib) {
                for (size_t ti = 0; ti < terms.size(); ++ti) {
                    int c = keys[ia][ti].compare(keys[ib][ti]);
                    if (c != 0) return terms[ti].asc ? c < 0 : c > 0;
                }
                return false;
            });
            std::vector<AggOut> sorted;
            sorted.reserve(outs.size());
            for (size_t i : ord) sorted.push_back(std::move(outs[i]));
            outs = std::move(sorted);
        }

        for (auto& o : outs) r.rows.push_back(std::move(o.row));
        // GROUP BY 结果同样受 LIMIT/OFFSET 约束(之前直接 return 导致 LIMIT 失效)
        int64_t goff = s.offset.value_or(0);
        int64_t glim = s.limit.value_or(static_cast<int64_t>(r.rows.size()));
        if (goff > 0) {
            if (static_cast<size_t>(goff) >= r.rows.size()) r.rows.clear();
            else r.rows.erase(r.rows.begin(), r.rows.begin() + goff);
        }
        if (glim >= 0 && static_cast<size_t>(glim) < r.rows.size())
            r.rows.resize(static_cast<size_t>(glim));
        r.affected_rows = static_cast<int64_t>(r.rows.size());
        return r;
    }
    // 先投影(ORDER BY 别名/位置需要投影值)
    std::vector<storage::Row> projected;
    projected.reserve(after_where.size());
    for (auto& row : after_where) projected.push_back(compute_row(row));
    if (!eval_error_.empty()) return take_eval_error();

    // 窗口函数:PARTITION BY 分组 → 组内 ORDER BY 排序 → 填值
    // 支持 ROW_NUMBER / RANK / DENSE_RANK / COUNT / SUM / AVG / MIN / MAX
    {
        std::vector<std::pair<size_t, const ast::Expression*>> win_cols;
        size_t oi = 0;
        for (auto& [e, _] : s.columns) {
            if (e->kind == ast::ExprKind::Star) {
                for (auto& slot : slots) oi += slot.def->columns.size();
                continue;
            }
            if (e->is_window) win_cols.push_back({oi, e.get()});
            ++oi;
        }
        for (auto& [ocol, we] : win_cols) {
            std::string low = we->func_name;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            const bool is_rank = (low == "row_number" || low == "rank" || low == "dense_rank");
            const bool is_agg  = (low == "count" || low == "sum" || low == "avg" ||
                                  low == "min" || low == "max");
            if (!is_rank && !is_agg) continue;
            std::vector<std::vector<types::Value>> okeys(projected.size());
            std::unordered_map<std::string, std::vector<size_t>> groups;
            std::vector<std::string> gorder;
            for (size_t i = 0; i < projected.size(); ++i) {
                auto ctx = build_ctx(after_where[i]);
                std::string k;
                for (auto& pe : we->win_partition) k += eval(*pe, ctx).to_string() + "\x01";
                for (auto& [oe, asc] : we->win_order) okeys[i].push_back(eval(*oe, ctx));
                auto it = groups.find(k);
                if (it == groups.end()) { gorder.push_back(k); groups[k].push_back(i); }
                else it->second.push_back(i);
            }
            if (!eval_error_.empty()) return take_eval_error();
            for (auto& k : gorder) {
                auto& idxs = groups[k];
                std::stable_sort(idxs.begin(), idxs.end(), [&](size_t a, size_t b) {
                    for (size_t t = 0; t < we->win_order.size(); ++t) {
                        int c = okeys[a][t].compare(okeys[b][t]);
                        if (c != 0) return we->win_order[t].second ? c < 0 : c > 0;
                    }
                    return false;
                });
                if (is_rank) {
                    for (size_t r = 0; r < idxs.size(); ++r) {
                        int64_t val = static_cast<int64_t>(r) + 1;
                        if (low != "dense_rank" && low != "row_number") {
                            // RANK:与上一行排序键相同则取同序号
                            if (r > 0) {
                                bool same = true;
                                for (size_t t = 0; t < we->win_order.size(); ++t)
                                    if (okeys[idxs[r]][t].compare(okeys[idxs[r-1]][t]) != 0) { same = false; break; }
                                if (same && ocol < projected[idxs[r]].size())
                                    val = projected[idxs[r-1]][ocol].is_int()
                                        ? projected[idxs[r-1]][ocol].v_int : val;
                            }
                        }
                        if (ocol < projected[idxs[r]].size())
                            projected[idxs[r]][ocol] = types::Value::make_int(val);
                    }
                    continue;
                }
                // ---- 聚合窗口 ----
                // 无 ORDER BY:整组聚合;有 ORDER BY:累积到当前行(MySQL 默认 RANGE 帧)
                const bool cumulative = !we->win_order.empty();
                const bool star_arg = (low == "count" && we->func_star_arg);
                std::vector<types::Value> argsrc(idxs.size());
                for (size_t r = 0; r < idxs.size(); ++r) {
                    argsrc[r] = (star_arg || we->func_args.empty())
                        ? types::Value::make_int(1)
                        : eval(*we->func_args[0], build_ctx(after_where[idxs[r]]));
                }
                double acc = 0;
                int64_t iacc = 0, cnt = 0;
                bool all_int = true, first = true;
                types::Value best;
                for (size_t r = 0; r < idxs.size(); ++r) {
                    const auto& v = argsrc[r];
                    if (!v.is_null()) {
                        if (low == "count") ++cnt;
                        else if (low == "min" || low == "max") {
                            if (first) { best = v; first = false; }
                            else {
                                int c = v.compare(best);
                                if ((low == "min" && c < 0) || (low == "max" && c > 0)) best = v;
                            }
                            ++cnt;
                        } else if (v.is_int()) { iacc += v.v_int; acc += static_cast<double>(v.v_int); ++cnt; }
                        else {
                            all_int = false;
                            double d; if (v.try_to_double(d)) acc += d;
                            ++cnt;
                        }
                    }
                    if (!cumulative && r + 1 < idxs.size()) continue;   // 整组:先累完再一次性写
                    types::Value outv;
                    if (low == "count") outv = types::Value::make_int(cnt);
                    else if (cnt == 0) outv = types::Value::make_null();
                    else if (low == "sum") outv = all_int ? types::Value::make_int(iacc)
                                                          : types::Value::make_double(acc);
                    else if (low == "avg") outv = types::Value::make_double(acc / static_cast<double>(cnt));
                    else outv = best;
                    if (cumulative) {
                        if (ocol < projected[idxs[r]].size()) projected[idxs[r]][ocol] = outv;
                    } else {
                        for (size_t q = 0; q < idxs.size(); ++q)
                            if (ocol < projected[idxs[q]].size()) projected[idxs[q]][ocol] = outv;
                    }
                }
            }
        }
    }

    // ORDER BY:输出列位置/别名取投影值,其余表达式在原始行上求值
    if (!s.order_by.empty() && projected.size() > 1) {
        struct OTerm { int out_idx = -1; const ast::Expression* expr = nullptr; bool asc = true; };
        std::vector<OTerm> terms;
        for (auto& [oe, asc] : s.order_by) {
            OTerm t;
            t.asc = asc;
            t.out_idx = order_output_index(*oe);
            if (t.out_idx < 0) t.expr = oe.get();
            terms.push_back(t);
        }
        std::vector<size_t> ord(projected.size());
        for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
        std::stable_sort(ord.begin(), ord.end(), [&](size_t ia, size_t ib) {
            for (auto& t : terms) {
                types::Value va, vb;
                if (t.out_idx >= 0) {
                    va = t.out_idx < static_cast<int>(projected[ia].size())
                       ? projected[ia][t.out_idx] : types::Value::make_null();
                    vb = t.out_idx < static_cast<int>(projected[ib].size())
                       ? projected[ib][t.out_idx] : types::Value::make_null();
                } else {
                    auto ca = build_ctx(after_where[ia]);
                    auto cb = build_ctx(after_where[ib]);
                    va = eval(*t.expr, ca);
                    vb = eval(*t.expr, cb);
                }
                int c = va.compare(vb);
                if (c != 0) return t.asc ? c < 0 : c > 0;
            }
            return false;
        });
        if (!eval_error_.empty()) return take_eval_error();
        std::vector<storage::Row> sorted;
        sorted.reserve(projected.size());
        for (size_t i : ord) sorted.push_back(std::move(projected[i]));
        projected = std::move(sorted);
    }

    // DISTINCT
    if (s.distinct) {
        std::vector<storage::Row> dedup;
        std::unordered_set<std::string> seen;
        seen.reserve(projected.size());
        for (auto& row : projected) {
            if (seen.insert(row_dedup_key(row)).second) dedup.push_back(std::move(row));
        }
        projected = std::move(dedup);
    }
    after_where = std::move(projected);

    // LIMIT / OFFSET
    int64_t off = s.offset.value_or(0);
    int64_t lim = s.limit.value_or(static_cast<int64_t>(after_where.size()));
    if (off > 0) {
        if (static_cast<size_t>(off) >= after_where.size()) after_where.clear();
        else after_where.erase(after_where.begin(), after_where.begin() + off);
    }
    if (lim >= 0 && static_cast<size_t>(lim) < after_where.size()) {
        after_where.resize(static_cast<size_t>(lim));
    }

    r.rows = std::move(after_where);
    r.affected_rows = static_cast<int64_t>(r.rows.size());
    r.found_rows = s.from_tables.empty() ? -1
                 : static_cast<int64_t>(r.rows.size());
    return r;
}

// ---------- SHOW TABLES / SHOW FULL TABLES ----------
ResultSet Executor::exec_show_tables(bool full, const std::string& db_name,
                                     const std::string& filter_pattern) {
    ResultSet r;
    std::string db = db_name.empty() ? default_db() : db_name;
    auto col_name = fmt::format("Tables_in_{}", db);
    r.column_names = { col_name };
    r.column_types = { types::SqlType::Varchar };
    if (full) {
        r.column_names.push_back("Table_type");
        r.column_types.push_back(types::SqlType::Varchar);
    }
    auto names = catalog_.table_names(db);
    for (auto& n : names) {
        if (!show_table_matches(n, filter_pattern)) continue;
        if (full)
            r.rows.push_back({ types::Value::make_string(n),
                               types::Value::make_string("BASE TABLE") });
        else
            r.rows.push_back({ types::Value::make_string(n) });
    }
    return r;
}

ResultSet Executor::exec_show_columns(const std::string& db_name, const std::string& table_name) {
    ResultSet r;
    r.column_names = { "Field", "Type", "Null", "Key", "Default", "Extra" };
    r.column_types = { types::SqlType::Varchar, types::SqlType::Varchar, types::SqlType::Varchar,
                       types::SqlType::Varchar, types::SqlType::Varchar, types::SqlType::Varchar };
    // 支持 SHOW COLUMNS FROM t FROM db / DESCRIBE db.t
    std::string qualified = db_name.empty() ? table_name : db_name + "." + table_name;
    auto* tbl = catalog_.get_table(qualified, default_db());
    if (!tbl) {
        r.ok = false; r.mysql_errno = 1146; r.sql_state = "42S02";
        r.error_message = fmt::format("Table '{}' doesn't exist", qualified);
        return r;
    }
    auto& def = tbl->def();
    for (auto& col : def.columns) {
        // Type 字符串
        std::string type_str;
        switch (col.type.type) {
            case types::SqlType::BigInt:   type_str = "bigint"; break;
            case types::SqlType::Int:      type_str = "int"; break;
            case types::SqlType::SmallInt: type_str = "smallint"; break;
            case types::SqlType::TinyInt:  type_str = "tinyint"; break;
            case types::SqlType::Double:   type_str = "double"; break;
            case types::SqlType::Float:    type_str = "float"; break;
            case types::SqlType::Decimal:  type_str = fmt::format("decimal({},{})", col.type.length, col.type.scale); break;
            case types::SqlType::Varchar:  type_str = fmt::format("varchar({})", col.type.length ? col.type.length : 255); break;
            case types::SqlType::Char:     type_str = fmt::format("char({})", col.type.length ? col.type.length : 1); break;
            case types::SqlType::Text:     type_str = "text"; break;
            case types::SqlType::Blob:     type_str = "blob"; break;
            case types::SqlType::Date:     type_str = "date"; break;
            case types::SqlType::Time:     type_str = "time"; break;
            case types::SqlType::DateTime: type_str = "datetime"; break;
            case types::SqlType::Timestamp:type_str = "timestamp"; break;
            case types::SqlType::Boolean:  type_str = "tinyint(1)"; break;
            case types::SqlType::Json:     type_str = "json"; break;
            default: type_str = "unknown";
        }
        // Nullable
        std::string nul = col.nullable ? "YES" : "NO";
        // Key
        std::string key = col.is_primary_key ? "PRI" : "";
        // Extra
        std::string extra = col.is_auto_increment ? "auto_increment" : "";
        // Default:有 DEFAULT 子句时输出其字面量(MySQL:字符串不带引号,数字原样)
        types::Value default_cell = types::Value::make_null();
        if (col.default_value && col.default_value->kind == ast::ExprKind::Literal) {
            auto& lit = col.default_value->literal;
            if (!lit.is_null()) {
                if (lit.is_string()) default_cell = types::Value::make_string(lit.v_str);
                else default_cell = types::Value::make_string(lit.to_string());
            }
        }
        r.rows.push_back({
            types::Value::make_string(col.name),
            types::Value::make_string(type_str),
            types::Value::make_string(nul),
            types::Value::make_string(key),
            default_cell,
            types::Value::make_string(extra),
        });
    }
    return r;
}

// ---------- SHOW DATABASES ----------
ResultSet Executor::exec_show_databases() {
    ResultSet r;
    r.column_names = { "Database" };
    r.column_types = { types::SqlType::Varchar };
    auto dbs = catalog_.list_databases();
    for (auto& n : dbs) r.rows.push_back({ types::Value::make_string(n) });
    return r;
}

// ---------- SHOW CREATE TABLE ----------
ResultSet Executor::exec_show_create_table(const std::string& db_name, const std::string& table_name) {
    ResultSet r;
    r.column_names = { "Table", "Create Table" };
    r.column_types = { types::SqlType::Varchar, types::SqlType::Varchar };

    // 构造带 db 限定的表名
    std::string qualified = db_name.empty() ? table_name : (db_name + "." + table_name);
    auto* tbl = catalog_.get_table(qualified, default_db());
    if (!tbl) {
        r.ok = false; r.mysql_errno = 1146; r.sql_state = "42S02";
        r.error_message = fmt::format("Table '{}' doesn't exist", qualified);
        return r;
    }
    auto& def = tbl->def();

    std::string sql = fmt::format("CREATE TABLE `{}` (\n", def.name);
    for (size_t i = 0; i < def.columns.size(); ++i) {
        auto& col = def.columns[i];
        // 类型字符串 — MySQL 的 SHOW CREATE TABLE 输出小写类型名,
        // SQLAlchemy 反射(ischema_names 小写键)依赖这一点
        std::string type_str;
        switch (col.type.type) {
            case types::SqlType::BigInt:   type_str = "bigint"; break;
            case types::SqlType::Int:      type_str = "int"; break;
            case types::SqlType::SmallInt: type_str = "smallint"; break;
            case types::SqlType::TinyInt:  type_str = "tinyint"; break;
            case types::SqlType::Double:   type_str = "double"; break;
            case types::SqlType::Float:    type_str = "float"; break;
            case types::SqlType::Decimal:  type_str = fmt::format("decimal({},{})", col.type.length, col.type.scale); break;
            case types::SqlType::Varchar:  type_str = fmt::format("varchar({})", col.type.length ? col.type.length : 255); break;
            case types::SqlType::Char:     type_str = fmt::format("char({})", col.type.length ? col.type.length : 1); break;
            case types::SqlType::Text:     type_str = "text"; break;
            case types::SqlType::Blob:     type_str = "blob"; break;
            case types::SqlType::Date:     type_str = "date"; break;
            case types::SqlType::Time:     type_str = "time"; break;
            case types::SqlType::DateTime: type_str = "datetime"; break;
            case types::SqlType::Timestamp:type_str = "timestamp"; break;
            case types::SqlType::Boolean:  type_str = "tinyint(1)"; break;
            case types::SqlType::Json:     type_str = "json"; break;
            default: type_str = "unknown";
        }
        if (col.type.unsigned_) type_str += " UNSIGNED";
        sql += fmt::format("  `{}` {}", col.name, type_str);
        if (!col.nullable) sql += " NOT NULL";
        if (col.is_auto_increment) sql += " AUTO_INCREMENT";
        // 注意:PRIMARY KEY 不内联到列上 —— MySQL 输出为表级 PRIMARY KEY 行,
        // SQLAlchemy 方言只从表级行识别主键
        if (col.default_value) sql += " DEFAULT <expr>";
        sql += ",\n";
    }
    // 表级 PRIMARY KEY 行(MySQL 风格,单列/多列都输出)
    if (!def.primary_key_cols.empty() && !def.primary_key_cols[0].empty()) {
        std::string pk_cols;
        for (size_t i = 0; i < def.primary_key_cols[0].size(); ++i) {
            if (i) pk_cols += ", ";
            pk_cols += "`" + def.primary_key_cols[0][i] + "`";
        }
        sql += fmt::format("  PRIMARY KEY ({})\n", pk_cols);
    }
    sql += ")";
    r.rows.push_back({
        types::Value::make_string(def.name),
        types::Value::make_string(std::move(sql)),
    });
    return r;
}

// ---------- SHOW CREATE DATABASE ----------
ResultSet Executor::exec_show_create_database(const std::string& db_name) {
    ResultSet r;
    r.column_names = { "Database", "Create Database" };
    r.column_types = { types::SqlType::Varchar, types::SqlType::Varchar };
    if (!catalog_.database_exists(db_name)) {
        r.ok = false; r.mysql_errno = 1049; r.sql_state = "42000";
        r.error_message = fmt::format("Unknown database '{}'", db_name);
        return r;
    }
    r.rows.push_back({
        types::Value::make_string(db_name),
        types::Value::make_string(fmt::format("CREATE DATABASE `{}`", db_name)),
    });
    return r;
}

// ---------- SHOW STATUS ----------
ResultSet Executor::exec_show_status() {
    ResultSet r;
    r.column_names = { "Variable_name", "Value" };
    r.column_types = { types::SqlType::Varchar, types::SqlType::Varchar };
    static const std::pair<const char*, const char*> kvs[] = {
        {"Aborted_clients", "0"}, {"Aborted_connects", "0"},
        {"Connections", "1"}, {"Delayed_errors", "0"},
        {"Delayed_insert_threads", "0"}, {"Delayed_writes", "0"},
        {"Not_flushed_delayed_rows", "0"}, {"Open_files", "16"},
        {"Open_streams", "0"}, {"Open_tables", "1"},
        {"Opened_tables", "1"}, {"Threads_connected", "1"},
        {"Threads_created", "1"}, {"Threads_running", "1"},
        {"Uptime", "0"}, {"Version", "5.7.0-SQLTiny"},
    };
    for (auto& [k, v] : kvs)
        r.rows.push_back({ types::Value::make_string(k), types::Value::make_string(v) });
    return r;
}

// ---------- SHOW VARIABLES ----------
ResultSet Executor::exec_show_variables(const std::string& pattern) {
    ResultSet r;
    r.column_names = { "Variable_name", "Value" };
    r.column_types = { types::SqlType::Varchar, types::SqlType::Varchar };
    static const std::pair<const char*, const char*> kvs[] = {
        {"character_set_client", "utf8mb4"},
        {"character_set_connection", "utf8mb4"}, {"character_set_database", "utf8mb4"},
        {"character_set_server", "utf8mb4"}, {"character_set_results", "utf8mb4"},
        {"collation_connection", "utf8mb4_general_ci"},
        {"collation_database", "utf8mb4_general_ci"},
        {"collation_server", "utf8mb4_general_ci"},
        {"default_storage_engine", "InnoDB"}, {"engine_condition_pushdown", "ON"},
        {"innodb_version", "5.7.0"}, {"interactive_timeout", "28800"},
        {"join_buffer_size", "262144"}, {"lower_case_table_names", "1"},
        {"max_allowed_packet", "67108864"}, {"net_read_timeout", "30"},
        {"net_write_timeout", "60"}, {"max_connections", "151"},
        {"net_buffer_length", "16384"}, {"sql_mode", "ONLY_FULL_GROUP_BY"},
        {"thread_cache_size", "9"}, {"wait_timeout", "28800"},
        {"version", "5.7.0-SQLTiny"},
    };
    // LIKE 简易过滤:pattern 去掉单引号, % = 通配任意
    auto match = [&](const char* name) -> bool {
        if (pattern.empty()) return true;
        std::string p = pattern;
        if (p.size() >= 2 && p.front() == '\'' && p.back() == '\'') p = p.substr(1, p.size()-2);
        // 简单实现:全 % 或精确 name 相等
        if (p == "%" || p == "%%") return true;
        // 支持 %xxx 或 xxx% 或 xxx (子串匹配)
        if (p.size() >= 2 && p.front() == '%' && p.back() == '%') {
            std::string inner = p.substr(1, p.size()-2);
            return std::string(name).find(inner) != std::string::npos;
        }
        if (p.size() >= 1 && p.back() == '%') return std::string(name).starts_with(p.substr(0, p.size()-1));
        if (p.size() >= 1 && p.front() == '%') return std::string(name).ends_with(p.substr(1));
        return std::string(name) == p;
    };
    std::vector<std::pair<std::string, std::string>> vars;
    vars.push_back({"autocommit", (session_ && !session_->autocommit) ? "OFF" : "ON"});
    std::string iso = session_ ? session_->isolationName() : std::string("READ-COMMITTED");
    vars.push_back({"tx_isolation", iso});
    vars.push_back({"transaction_isolation", iso});
    for (auto& [k, v] : kvs) vars.push_back({k, v});
    for (auto& [k, v] : vars)
        if (match(k.c_str()))
            r.rows.push_back({ types::Value::make_string(k), types::Value::make_string(v) });
    return r;
}

// ---------- SHOW ENGINES / PLUGINS / CHARSET / COLLATION ----------
ResultSet Executor::exec_show_engines() {
    ResultSet r;
    r.column_names = { "Engine", "Support", "Comment", "Transactions", "XA", "Savepoints" };
    r.column_types = { types::SqlType::Varchar, types::SqlType::Varchar, types::SqlType::Varchar,
                       types::SqlType::Varchar, types::SqlType::Varchar, types::SqlType::Varchar };
    r.rows.push_back({ types::Value::make_string("InnoDB"), types::Value::make_string("DEFAULT"),
        types::Value::make_string("Supports transactions, row-level locking, and foreign keys"),
        types::Value::make_string("YES"), types::Value::make_string("YES"), types::Value::make_string("YES") });
    r.rows.push_back({ types::Value::make_string("MEMORY"), types::Value::make_string("YES"),
        types::Value::make_string("Hash based, stored in memory, useful for temporary tables"),
        types::Value::make_string("NO"), types::Value::make_string("NO"), types::Value::make_string("NO") });
    r.rows.push_back({ types::Value::make_string("CSV"), types::Value::make_string("YES"),
        types::Value::make_string("CSV storage engine"),
        types::Value::make_string("NO"), types::Value::make_string("NO"), types::Value::make_string("NO") });
    return r;
}

ResultSet Executor::exec_show_plugins() {
    ResultSet r;
    r.column_names = { "Name", "Status", "Type", "Library", "License" };
    r.column_types = { types::SqlType::Varchar, types::SqlType::Varchar, types::SqlType::Varchar,
                       types::SqlType::Varchar, types::SqlType::Varchar };
    struct Row3 { const char* name; const char* status; const char* type; };
    static const Row3 rows[] = {
        {"binlog", "ACTIVE", "SERVER"}, {"InnoDB", "ACTIVE", "STORAGE ENGINE"},
        {"CSV", "ACTIVE", "STORAGE ENGINE"}, {"MEMORY", "ACTIVE", "STORAGE ENGINE"},
        {"mysql_native_password", "ACTIVE", "AUTHENTICATION"},
        {"sha256_password", "ACTIVE", "AUTHENTICATION"},
    };
    for (auto& row : rows)
        r.rows.push_back({ types::Value::make_string(row.name), types::Value::make_string(row.status),
                           types::Value::make_string(row.type), types::Value::make_null(),
                           types::Value::make_string("GPL") });
    return r;
}

ResultSet Executor::exec_show_charset() {
    ResultSet r;
    r.column_names = { "Charset", "Description", "Default collation", "Maxlen" };
    r.column_types = { types::SqlType::Varchar, types::SqlType::Varchar,
                       types::SqlType::Varchar, types::SqlType::Int };
    struct Cs { const char* name; const char* desc; const char* coll; int maxlen; };
    static const Cs rows[] = {
        {"utf8mb4", "UTF-8 Unicode", "utf8mb4_general_ci", 4},
        {"utf8",    "UTF-8 Unicode", "utf8_general_ci", 3},
        {"latin1",  "cp1252 West European", "latin1_swedish_ci", 1},
    };
    for (auto& row : rows)
        r.rows.push_back({ types::Value::make_string(row.name), types::Value::make_string(row.desc),
                           types::Value::make_string(row.coll), types::Value::make_int(row.maxlen) });
    return r;
}

ResultSet Executor::exec_show_collation() {
    ResultSet r;
    r.column_names = { "Collation", "Charset", "Id", "Default", "Compiled", "Sortlen" };
    r.column_types = { types::SqlType::Varchar, types::SqlType::Varchar, types::SqlType::Int,
                       types::SqlType::Varchar, types::SqlType::Varchar, types::SqlType::Int };
    struct Cl { const char* coll; const char* cs; int id; bool dflt; };
    static const Cl rows[] = {
        {"utf8mb4_general_ci", "utf8mb4", 45, true},
        {"utf8mb4_bin",        "utf8mb4", 46, false},
        {"utf8_general_ci",    "utf8",    33, true},
        {"utf8_bin",           "utf8",    83, false},
        {"latin1_swedish_ci",  "latin1",   8, true},
    };
    for (auto& row : rows)
        r.rows.push_back({ types::Value::make_string(row.coll), types::Value::make_string(row.cs),
                           types::Value::make_int(row.id),
                           types::Value::make_string(row.dflt ? "Yes" : ""),
                           types::Value::make_string("Yes"), types::Value::make_int(1) });
    return r;
}

// ---------- SHOW INDEX / KEYS FROM tbl ----------
ResultSet Executor::exec_show_index(const std::string& db_name, const std::string& table_name) {
    ResultSet r;
    r.column_names = { "Table", "Non_unique", "Key_name", "Seq_in_index", "Column_name",
                       "Collation", "Cardinality", "Sub_part", "Packed", "Null",
                       "Index_type", "Comment", "Index_comment" };
    r.column_types = { types::SqlType::Varchar, types::SqlType::Int, types::SqlType::Varchar,
                       types::SqlType::Int, types::SqlType::Varchar, types::SqlType::Varchar,
                       types::SqlType::Int, types::SqlType::Int, types::SqlType::Varchar,
                       types::SqlType::Varchar, types::SqlType::Varchar, types::SqlType::Varchar,
                       types::SqlType::Varchar };
    std::string qualified = db_name.empty() ? table_name : db_name + "." + table_name;
    auto* tbl = catalog_.get_table(qualified, default_db());
    if (!tbl) {
        r.ok = false; r.mysql_errno = 1146; r.sql_state = "42S02";
        r.error_message = fmt::format("Table '{}' doesn't exist", qualified);
        return r;
    }
    auto& def = tbl->def();
    // 主键 → PRIMARY 索引行
    if (!def.primary_key_cols.empty()) {
        int seq = 1;
        for (auto& col : def.primary_key_cols[0]) {
            r.rows.push_back({
                types::Value::make_string(def.name),
                types::Value::make_int(0),                    // Non_unique: PK 是 0
                types::Value::make_string("PRIMARY"),
                types::Value::make_int(seq++),
                types::Value::make_string(col),
                types::Value::make_string("A"),               // Collation: 升序
                types::Value::make_int(static_cast<int64_t>(tbl->row_count())),
                types::Value::make_null(), types::Value::make_null(),
                types::Value::make_string(""),                // Null: PK 列非空
                types::Value::make_string("BTREE"),
                types::Value::make_string(""), types::Value::make_string(""),
            });
        }
    }
    // 次级索引
    for (auto& [idx_name, idx] : def.indexes) {
        int seq = 1;
        for (auto& col : idx.columns) {
            int ci = def.column_index(col);
            bool nullable = (ci >= 0) && def.columns[static_cast<size_t>(ci)].nullable;
            r.rows.push_back({
                types::Value::make_string(def.name),
                types::Value::make_int(idx.unique ? 0 : 1),
                types::Value::make_string(idx_name),
                types::Value::make_int(seq++),
                types::Value::make_string(col),
                types::Value::make_string("A"),
                types::Value::make_int(static_cast<int64_t>(tbl->row_count())),
                types::Value::make_null(), types::Value::make_null(),
                types::Value::make_string(nullable ? "YES" : ""),
                types::Value::make_string("BTREE"),
                types::Value::make_string(""), types::Value::make_string(""),
            });
        }
    }
    return r;
}

// ---------- SHOW TABLE STATUS [FROM db] [LIKE 'pat'] ----------
ResultSet Executor::exec_show_table_status(const std::string& db_name, const std::string& pattern) {
    ResultSet r;
    r.column_names = { "Name", "Engine", "Version", "Row_format", "Rows", "Avg_row_length",
                       "Data_length", "Max_data_length", "Index_length", "Data_free",
                       "Auto_increment", "Create_time", "Update_time", "Check_time",
                       "Collation", "Checksum", "Create_options", "Comment" };
    r.column_types.assign(18, types::SqlType::Varchar);
    r.column_types[2] = types::SqlType::Int;
    r.column_types[4] = types::SqlType::Int;
    r.column_types[5] = types::SqlType::Int;
    r.column_types[6] = types::SqlType::Int;
    r.column_types[7] = types::SqlType::Int;
    r.column_types[8] = types::SqlType::Int;
    r.column_types[9] = types::SqlType::Int;
    r.column_types[10] = types::SqlType::Int;

    std::string db = db_name.empty() ? default_db() : db_name;
    std::string p = pattern;
    if (p.size() >= 2 && p.front() == '\'' && p.back() == '\'') p = p.substr(1, p.size() - 2);
    auto match_pattern = [&](const std::string& name) -> bool {
        if (p.empty() || p == "%" || p == "%%") return true;
        if (p.size() >= 2 && p.front() == '%' && p.back() == '%')
            return name.find(p.substr(1, p.size() - 2)) != std::string::npos;
        if (p.back() == '%') return name.starts_with(p.substr(0, p.size() - 1));
        if (p.front() == '%') return name.ends_with(p.substr(1));
        return name == p;
    };
    for (auto& n : catalog_.table_names(db)) {
        if (!match_pattern(n)) continue;
        auto* tbl = catalog_.get_table(n, db);
        int64_t rows = tbl ? static_cast<int64_t>(tbl->row_count()) : 0;
        int64_t auto_inc = 0;
        if (tbl) {
            int64_t cur = tbl->auto_inc_cursor();
            auto_inc = cur > 0 ? cur + 1 : 1;
        }
        r.rows.push_back({
            types::Value::make_string(n),
            types::Value::make_string("InnoDB"),
            types::Value::make_int(10),
            types::Value::make_string("Dynamic"),
            types::Value::make_int(rows),
            types::Value::make_int(0),
            types::Value::make_int(16384),
            types::Value::make_null(),
            types::Value::make_int(0),
            types::Value::make_int(0),
            auto_inc > 0 ? types::Value::make_int(auto_inc) : types::Value::make_null(),
            types::Value::make_null(), types::Value::make_null(), types::Value::make_null(),
            types::Value::make_string("utf8mb4_general_ci"),
            types::Value::make_null(),
            types::Value::make_string(""),
            types::Value::make_string(""),
        });
    }
    return r;
}

// ---------- SHOW PROCESSLIST / WARNINGS ----------
ResultSet Executor::exec_show_triggers() {
    ResultSet r;
    r.column_names = { "Trigger", "Event", "Table", "Statement", "Timing", "Created",
                       "sql_mode", "Definer", "character_set_client",
                       "collation_connection", "Database Collation" };
    r.column_types.assign(r.column_names.size(), types::SqlType::Varchar);
    // SQLTiny 不支持触发器 —— 返回结构正确的空结果集
    return r;
}

ResultSet Executor::exec_show_processlist() {
    ResultSet r;
    r.column_names = { "Id", "User", "Host", "db", "Command", "Time", "State", "Info" };
    r.column_types = { types::SqlType::BigInt, types::SqlType::Varchar, types::SqlType::Varchar,
                       types::SqlType::Varchar, types::SqlType::Varchar, types::SqlType::Int,
                       types::SqlType::Varchar, types::SqlType::Varchar };
    r.rows.push_back({
        types::Value::make_int(1), types::Value::make_string("root"),
        types::Value::make_string("localhost:0"),
        session_ ? types::Value::make_string(session_->current_database) : types::Value::make_null(),
        types::Value::make_string("Query"), types::Value::make_int(0),
        types::Value::make_string("starting"), types::Value::make_null(),
    });
    return r;
}

ResultSet Executor::exec_show_warnings() {
    ResultSet r;
    r.column_names = { "Level", "Code", "Message" };
    r.column_types = { types::SqlType::Varchar, types::SqlType::Int, types::SqlType::Varchar };
    // 无 warning 队列实现 —— 返回空结果集(0 行)
    return r;
}

// ---------- information_schema 虚拟表 (TABLES / COLUMNS) ----------
bool Executor::is_information_schema_name(const std::string& qualified) {
    auto dot = qualified.find('.');
    if (dot == std::string::npos) return false;
    std::string db = qualified.substr(0, dot);
    std::string tbl = qualified.substr(dot + 1);
    auto ci_eq = [](const std::string& a, const char* b) {
        if (a.size() != std::strlen(b)) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    };
    if (!ci_eq(db, "information_schema")) return false;
    return ci_eq(tbl, "TABLES") || ci_eq(tbl, "COLUMNS") || ci_eq(tbl, "SCHEMATA") ||
           ci_eq(tbl, "ENGINES") ||
           ci_eq(tbl, "TRIGGERS") || ci_eq(tbl, "ROUTINES") || ci_eq(tbl, "EVENTS") ||
           ci_eq(tbl, "VIEWS") || ci_eq(tbl, "TABLE_CONSTRAINTS") ||
           ci_eq(tbl, "KEY_COLUMN_USAGE") || ci_eq(tbl, "STATISTICS") ||
           ci_eq(tbl, "REFERENTIAL_CONSTRAINTS");
}

namespace {
// 列类型 → MySQL 风格类型字符串 (DATA_TYPE / COLUMN_TYPE 共用)
std::string is_data_type(const ast::ColumnDef& col) {
    switch (col.type.type) {
        case types::SqlType::BigInt:   return "bigint";
        case types::SqlType::Int:      return "int";
        case types::SqlType::SmallInt: return "smallint";
        case types::SqlType::TinyInt:  return "tinyint";
        case types::SqlType::Double:   return "double";
        case types::SqlType::Float:    return "float";
        case types::SqlType::Decimal:  return "decimal";
        case types::SqlType::Varchar:  return "varchar";
        case types::SqlType::Char:     return "char";
        case types::SqlType::Text:     return "text";
        case types::SqlType::Blob:     return "blob";
        case types::SqlType::Date:     return "date";
        case types::SqlType::Time:     return "time";
        case types::SqlType::DateTime: return "datetime";
        case types::SqlType::Timestamp:return "timestamp";
        case types::SqlType::Boolean:  return "tinyint";
        case types::SqlType::Json:     return "json";
        default: return "unknown";
    }
}
std::string is_column_type(const ast::ColumnDef& col) {
    std::string base = is_data_type(col);
    switch (col.type.type) {
        case types::SqlType::Int:      return "int(11)";
        case types::SqlType::BigInt:   return col.type.unsigned_ ? "bigint(20) unsigned" : "bigint(20)";
        case types::SqlType::TinyInt:  return col.type.type == types::SqlType::Boolean ? "tinyint(1)" : "tinyint(4)";
        case types::SqlType::SmallInt: return "smallint(6)";
        case types::SqlType::Decimal:  return fmt::format("decimal({},{})", col.type.length ? col.type.length : 10, col.type.scale);
        case types::SqlType::Varchar:  return fmt::format("varchar({})", col.type.length ? col.type.length : 255);
        case types::SqlType::Char:     return fmt::format("char({})", col.type.length ? col.type.length : 1);
        default: return base;
    }
}
ast::ColumnDef is_col(const char* name, types::SqlType t) {
    ast::ColumnDef c;
    c.name = name;
    types::TypeInfo ti;
    ti.type = t;
    ti.nullable = true;
    c.type = ti;
    return c;
}
} // anonymous

std::unique_ptr<storage::Table> Executor::build_information_schema_table(
        const std::string& db_name, const std::string& table_name) const {
    auto ci_eq = [](const std::string& a, const char* b) {
        if (a.size() != std::strlen(b)) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    };

    if (ci_eq(table_name, "TABLES")) {
        storage::TableDef def;
        def.name = "TABLES";
        def.columns = {
            is_col("TABLE_CATALOG",   types::SqlType::Varchar),
            is_col("TABLE_SCHEMA",    types::SqlType::Varchar),
            is_col("TABLE_NAME",      types::SqlType::Varchar),
            is_col("TABLE_TYPE",      types::SqlType::Varchar),
            is_col("ENGINE",          types::SqlType::Varchar),
            is_col("VERSION",         types::SqlType::Int),
            is_col("ROW_FORMAT",      types::SqlType::Varchar),
            is_col("TABLE_ROWS",      types::SqlType::BigInt),
            is_col("AVG_ROW_LENGTH",  types::SqlType::BigInt),
            is_col("DATA_LENGTH",     types::SqlType::BigInt),
            is_col("MAX_DATA_LENGTH", types::SqlType::BigInt),
            is_col("INDEX_LENGTH",    types::SqlType::BigInt),
            is_col("DATA_FREE",       types::SqlType::BigInt),
            is_col("AUTO_INCREMENT",  types::SqlType::BigInt),
            is_col("CREATE_TIME",     types::SqlType::DateTime),
            is_col("UPDATE_TIME",     types::SqlType::DateTime),
            is_col("CHECK_TIME",      types::SqlType::DateTime),
            is_col("TABLE_COLLATION", types::SqlType::Varchar),
            is_col("CHECKSUM",        types::SqlType::BigInt),
            is_col("CREATE_OPTIONS",  types::SqlType::Varchar),
            is_col("TABLE_COMMENT",   types::SqlType::Varchar),
        };
        auto tbl = std::make_unique<storage::Table>(std::move(def));
        for (auto& db : catalog_.list_databases()) {
            for (auto& n : catalog_.table_names(db)) {
                auto* t = catalog_.get_table(n, db);
                int64_t rows = t ? static_cast<int64_t>(t->row_count()) : 0;
                int64_t auto_inc = 0;
                if (t) {
                    int64_t cur = t->auto_inc_cursor();
                    auto_inc = cur > 0 ? cur + 1 : 1;
                }
                tbl->insert({
                    types::Value::make_string("def"),
                    types::Value::make_string(db),
                    types::Value::make_string(n),
                    types::Value::make_string("BASE TABLE"),
                    types::Value::make_string("InnoDB"),
                    types::Value::make_int(10),
                    types::Value::make_string("Dynamic"),
                    types::Value::make_int(rows),
                    types::Value::make_int(0),
                    types::Value::make_int(16384),
                    types::Value::make_null(),
                    types::Value::make_int(0),
                    types::Value::make_int(0),
                    auto_inc > 0 ? types::Value::make_int(auto_inc) : types::Value::make_null(),
                    types::Value::make_null(), types::Value::make_null(), types::Value::make_null(),
                    types::Value::make_string("utf8mb4_general_ci"),
                    types::Value::make_null(),
                    types::Value::make_string(""),
                    types::Value::make_string(""),
                });
            }
        }
        return tbl;
    }

    if (ci_eq(table_name, "SCHEMATA")) {
        storage::TableDef def;
        def.name = "SCHEMATA";
        def.columns = {
            is_col("CATALOG_NAME",              types::SqlType::Varchar),
            is_col("SCHEMA_NAME",               types::SqlType::Varchar),
            is_col("DEFAULT_CHARACTER_SET_NAME",types::SqlType::Varchar),
            is_col("DEFAULT_COLLATION_NAME",    types::SqlType::Varchar),
            is_col("SQL_PATH",                  types::SqlType::Varchar),
        };
        auto tbl = std::make_unique<storage::Table>(std::move(def));
        for (auto& db : catalog_.list_databases()) {
            tbl->insert({
                types::Value::make_string("def"),
                types::Value::make_string(db),
                types::Value::make_string("utf8mb4"),
                types::Value::make_string("utf8mb4_general_ci"),
                types::Value::make_null(),
            });
        }
        return tbl;
    }

    // ---- ENGINES:Navicat 连接探测会查 information_schema.ENGINES 判断 NDB 支持 ----
    if (ci_eq(table_name, "ENGINES")) {
        storage::TableDef def;
        def.name = "ENGINES";
        def.columns = {
            is_col("ENGINE",      types::SqlType::Varchar),
            is_col("SUPPORT",     types::SqlType::Varchar),
            is_col("COMMENT",     types::SqlType::Varchar),
            is_col("TRANSACTIONS",types::SqlType::Varchar),
            is_col("XA",          types::SqlType::Varchar),
            is_col("SAVEPOINTS",  types::SqlType::Varchar),
        };
        auto tbl = std::make_unique<storage::Table>(std::move(def));
        // SQLTiny 事务引擎,以 InnoDB 兼容身份上报
        tbl->insert({
            types::Value::make_string("InnoDB"),
            types::Value::make_string("DEFAULT"),
            types::Value::make_string("Supports transactions, row-level locking, and foreign keys"),
            types::Value::make_string("YES"),
            types::Value::make_string("YES"),
            types::Value::make_string("YES"),
        });
        return tbl;
    }

    // ---- 无对象的元数据表:SQLTiny 不支持触发器/存储过程/事件/视图/外键约束,
    //      返回结构正确但 0 行的结果,客户端(Navicat 类工具)即可正常加载 ----
    auto make_empty = [](const char* name, std::vector<ast::ColumnDef> cols) {
        storage::TableDef def;
        def.name = name;
        def.columns = std::move(cols);
        return std::make_unique<storage::Table>(std::move(def));
    };

    if (ci_eq(table_name, "TRIGGERS")) {
        std::vector<ast::ColumnDef> cols = {
            is_col("TRIGGER_CATALOG", types::SqlType::Varchar),
            is_col("TRIGGER_SCHEMA", types::SqlType::Varchar),
            is_col("TRIGGER_NAME", types::SqlType::Varchar),
            is_col("EVENT_MANIPULATION", types::SqlType::Varchar),
            is_col("EVENT_OBJECT_CATALOG", types::SqlType::Varchar),
            is_col("EVENT_OBJECT_SCHEMA", types::SqlType::Varchar),
            is_col("EVENT_OBJECT_TABLE", types::SqlType::Varchar),
            is_col("ACTION_ORDER", types::SqlType::BigInt),
            is_col("ACTION_CONDITION", types::SqlType::Text),
            is_col("ACTION_STATEMENT", types::SqlType::Text),
            is_col("ACTION_ORIENTATION", types::SqlType::Varchar),
            is_col("ACTION_TIMING", types::SqlType::Varchar),
            is_col("ACTION_REFERENCE_OLD_TABLE", types::SqlType::Varchar),
            is_col("ACTION_REFERENCE_NEW_TABLE", types::SqlType::Varchar),
            is_col("ACTION_REFERENCE_OLD_ROW", types::SqlType::Varchar),
            is_col("ACTION_REFERENCE_NEW_ROW", types::SqlType::Varchar),
            is_col("CREATED", types::SqlType::DateTime),
            is_col("SQL_MODE", types::SqlType::Varchar),
            is_col("DEFINER", types::SqlType::Varchar),
            is_col("CHARACTER_SET_CLIENT", types::SqlType::Varchar),
            is_col("COLLATION_CONNECTION", types::SqlType::Varchar),
            is_col("DATABASE_COLLATION", types::SqlType::Varchar),
        };
        return make_empty("TRIGGERS", std::move(cols));
    }

    if (ci_eq(table_name, "ROUTINES")) {
        std::vector<ast::ColumnDef> cols = {
            is_col("SPECIFIC_CATALOG", types::SqlType::Varchar),
            is_col("SPECIFIC_SCHEMA", types::SqlType::Varchar),
            is_col("SPECIFIC_NAME", types::SqlType::Varchar),
            is_col("ROUTINE_CATALOG", types::SqlType::Varchar),
            is_col("ROUTINE_SCHEMA", types::SqlType::Varchar),
            is_col("ROUTINE_NAME", types::SqlType::Varchar),
            is_col("ROUTINE_TYPE", types::SqlType::Varchar),
            is_col("DATA_TYPE", types::SqlType::Varchar),
            is_col("CHARACTER_MAXIMUM_LENGTH", types::SqlType::BigInt),
            is_col("CHARACTER_OCTET_LENGTH", types::SqlType::BigInt),
            is_col("NUMERIC_PRECISION", types::SqlType::BigInt),
            is_col("NUMERIC_SCALE", types::SqlType::BigInt),
            is_col("DATETIME_PRECISION", types::SqlType::BigInt),
            is_col("CHARACTER_SET_NAME", types::SqlType::Varchar),
            is_col("COLLATION_NAME", types::SqlType::Varchar),
            is_col("DTD_IDENTIFIER", types::SqlType::Text),
            is_col("ROUTINE_BODY", types::SqlType::Varchar),
            is_col("ROUTINE_DEFINITION", types::SqlType::Text),
            is_col("EXTERNAL_NAME", types::SqlType::Text),
            is_col("EXTERNAL_LANGUAGE", types::SqlType::Varchar),
            is_col("PARAMETER_STYLE", types::SqlType::Varchar),
            is_col("IS_DETERMINISTIC", types::SqlType::Varchar),
            is_col("SQL_DATA_ACCESS", types::SqlType::Varchar),
            is_col("SQL_PATH", types::SqlType::Varchar),
            is_col("SECURITY_TYPE", types::SqlType::Varchar),
            is_col("CREATED", types::SqlType::DateTime),
            is_col("LAST_ALTERED", types::SqlType::DateTime),
            is_col("SQL_MODE", types::SqlType::Varchar),
            is_col("ROUTINE_COMMENT", types::SqlType::Text),
            is_col("DEFINER", types::SqlType::Varchar),
            is_col("CHARACTER_SET_CLIENT", types::SqlType::Varchar),
            is_col("COLLATION_CONNECTION", types::SqlType::Varchar),
            is_col("DATABASE_COLLATION", types::SqlType::Varchar),
        };
        return make_empty("ROUTINES", std::move(cols));
    }

    if (ci_eq(table_name, "EVENTS")) {
        std::vector<ast::ColumnDef> cols = {
            is_col("EVENT_CATALOG", types::SqlType::Varchar),
            is_col("EVENT_SCHEMA", types::SqlType::Varchar),
            is_col("EVENT_NAME", types::SqlType::Varchar),
            is_col("DEFINER", types::SqlType::Varchar),
            is_col("TIME_ZONE", types::SqlType::Varchar),
            is_col("EVENT_BODY", types::SqlType::Varchar),
            is_col("EVENT_DEFINITION", types::SqlType::Text),
            is_col("EVENT_TYPE", types::SqlType::Varchar),
            is_col("EXECUTE_AT", types::SqlType::DateTime),
            is_col("INTERVAL_VALUE", types::SqlType::Varchar),
            is_col("INTERVAL_FIELD", types::SqlType::Varchar),
            is_col("SQL_MODE", types::SqlType::Varchar),
            is_col("STARTS", types::SqlType::DateTime),
            is_col("ENDS", types::SqlType::DateTime),
            is_col("STATUS", types::SqlType::Varchar),
            is_col("ON_COMPLETION", types::SqlType::Varchar),
            is_col("CREATED", types::SqlType::DateTime),
            is_col("LAST_ALTERED", types::SqlType::DateTime),
            is_col("LAST_EXECUTED", types::SqlType::DateTime),
            is_col("EVENT_COMMENT", types::SqlType::Varchar),
            is_col("ORIGINATOR", types::SqlType::BigInt),
            is_col("CHARACTER_SET_CLIENT", types::SqlType::Varchar),
            is_col("COLLATION_CONNECTION", types::SqlType::Varchar),
            is_col("DATABASE_COLLATION", types::SqlType::Varchar),
        };
        return make_empty("EVENTS", std::move(cols));
    }

    if (ci_eq(table_name, "VIEWS")) {
        std::vector<ast::ColumnDef> cols = {
            is_col("TABLE_CATALOG", types::SqlType::Varchar),
            is_col("TABLE_SCHEMA", types::SqlType::Varchar),
            is_col("TABLE_NAME", types::SqlType::Varchar),
            is_col("VIEW_DEFINITION", types::SqlType::Text),
            is_col("CHECK_OPTION", types::SqlType::Varchar),
            is_col("IS_UPDATABLE", types::SqlType::Varchar),
            is_col("DEFINER", types::SqlType::Varchar),
            is_col("SECURITY_TYPE", types::SqlType::Varchar),
            is_col("CHARACTER_SET_CLIENT", types::SqlType::Varchar),
            is_col("COLLATION_CONNECTION", types::SqlType::Varchar),
        };
        return make_empty("VIEWS", std::move(cols));
    }

    if (ci_eq(table_name, "TABLE_CONSTRAINTS")) {
        std::vector<ast::ColumnDef> cols = {
            is_col("CONSTRAINT_CATALOG", types::SqlType::Varchar),
            is_col("CONSTRAINT_SCHEMA", types::SqlType::Varchar),
            is_col("CONSTRAINT_NAME", types::SqlType::Varchar),
            is_col("TABLE_SCHEMA", types::SqlType::Varchar),
            is_col("TABLE_NAME", types::SqlType::Varchar),
            is_col("CONSTRAINT_TYPE", types::SqlType::Varchar),
            is_col("ENFORCED", types::SqlType::Varchar),
        };
        return make_empty("TABLE_CONSTRAINTS", std::move(cols));
    }

    if (ci_eq(table_name, "KEY_COLUMN_USAGE")) {
        std::vector<ast::ColumnDef> cols = {
            is_col("CONSTRAINT_CATALOG", types::SqlType::Varchar),
            is_col("CONSTRAINT_SCHEMA", types::SqlType::Varchar),
            is_col("CONSTRAINT_NAME", types::SqlType::Varchar),
            is_col("TABLE_CATALOG", types::SqlType::Varchar),
            is_col("TABLE_SCHEMA", types::SqlType::Varchar),
            is_col("TABLE_NAME", types::SqlType::Varchar),
            is_col("COLUMN_NAME", types::SqlType::Varchar),
            is_col("ORDINAL_POSITION", types::SqlType::BigInt),
            is_col("POSITION_IN_UNIQUE_CONSTRAINT", types::SqlType::BigInt),
            is_col("REFERENCED_TABLE_SCHEMA", types::SqlType::Varchar),
            is_col("REFERENCED_TABLE_NAME", types::SqlType::Varchar),
            is_col("REFERENCED_COLUMN_NAME", types::SqlType::Varchar),
        };
        // 主键列约束必须可见:依赖该视图判断主键的 ORM / DBeaver 会误判
        storage::TableDef def;
        def.name = "KEY_COLUMN_USAGE";
        def.columns = cols;
        auto tbl = std::make_unique<storage::Table>(std::move(def));
        for (auto& db : catalog_.list_databases()) {
            for (auto& n : catalog_.table_names(db)) {
                auto* t = catalog_.get_table(n, db);
                if (!t) continue;
                auto& tdef = t->def();
                for (auto& pk_cols : tdef.primary_key_cols) {
                    int64_t ord = 1;
                    for (auto& cname : pk_cols) {
                        tbl->insert({
                            types::Value::make_string("def"),
                            types::Value::make_string(db),
                            types::Value::make_string("PRIMARY"),
                            types::Value::make_string("def"),
                            types::Value::make_string(db),
                            types::Value::make_string(n),
                            types::Value::make_string(cname),
                            types::Value::make_int(ord++),
                            types::Value::make_null(),
                            types::Value::make_null(),
                            types::Value::make_null(),
                            types::Value::make_null(),
                        });
                    }
                }
            }
        }
        return tbl;
    }

    if (ci_eq(table_name, "REFERENTIAL_CONSTRAINTS")) {
        // SQLTiny 不支持外键,返回结构正确但为空的表,
        // 否则 DBeaver 打开任意表时查外键信息都会报 1146。
        std::vector<ast::ColumnDef> cols = {
            is_col("CONSTRAINT_CATALOG", types::SqlType::Varchar),
            is_col("CONSTRAINT_SCHEMA", types::SqlType::Varchar),
            is_col("CONSTRAINT_NAME", types::SqlType::Varchar),
            is_col("UNIQUE_CONSTRAINT_CATALOG", types::SqlType::Varchar),
            is_col("UNIQUE_CONSTRAINT_SCHEMA", types::SqlType::Varchar),
            is_col("UNIQUE_CONSTRAINT_NAME", types::SqlType::Varchar),
            is_col("MATCH_OPTION", types::SqlType::Varchar),
            is_col("UPDATE_RULE", types::SqlType::Varchar),
            is_col("DELETE_RULE", types::SqlType::Varchar),
            is_col("TABLE_NAME", types::SqlType::Varchar),
            is_col("REFERENCED_TABLE_NAME", types::SqlType::Varchar),
        };
        return make_empty("REFERENTIAL_CONSTRAINTS", std::move(cols));
    }

    if (ci_eq(table_name, "STATISTICS")) {
        std::vector<ast::ColumnDef> cols = {
            is_col("TABLE_CATALOG", types::SqlType::Varchar),
            is_col("TABLE_SCHEMA", types::SqlType::Varchar),
            is_col("TABLE_NAME", types::SqlType::Varchar),
            is_col("NON_UNIQUE", types::SqlType::BigInt),
            is_col("INDEX_SCHEMA", types::SqlType::Varchar),
            is_col("INDEX_NAME", types::SqlType::Varchar),
            is_col("SEQ_IN_INDEX", types::SqlType::BigInt),
            is_col("COLUMN_NAME", types::SqlType::Varchar),
            is_col("COLLATION", types::SqlType::Varchar),
            is_col("CARDINALITY", types::SqlType::BigInt),
            is_col("SUB_PART", types::SqlType::BigInt),
            is_col("PACKED", types::SqlType::Varchar),
            is_col("NULLABLE", types::SqlType::Varchar),
            is_col("INDEX_TYPE", types::SqlType::Varchar),
            is_col("COMMENT", types::SqlType::Varchar),
            is_col("INDEX_COMMENT", types::SqlType::Varchar),
        };
        // 主键索引真实存在(SHOW INDEX 已有 PRIMARY),统计视图也必须可见
        storage::TableDef def;
        def.name = "STATISTICS";
        def.columns = cols;
        auto tbl = std::make_unique<storage::Table>(std::move(def));
        for (auto& db : catalog_.list_databases()) {
            for (auto& n : catalog_.table_names(db)) {
                auto* t = catalog_.get_table(n, db);
                if (!t) continue;
                auto& tdef = t->def();
                int64_t card = static_cast<int64_t>(t->row_count());
                for (auto& pk_cols : tdef.primary_key_cols) {
                    int64_t seq = 1;
                    for (auto& cname : pk_cols) {
                        int ci = tdef.column_index(cname);
                        bool nullable = (ci >= 0) ? tdef.columns[ci].nullable : false;
                        tbl->insert({
                            types::Value::make_string("def"),
                            types::Value::make_string(db),
                            types::Value::make_string(n),
                            types::Value::make_int(0),              // NON_UNIQUE
                            types::Value::make_string(db),
                            types::Value::make_string("PRIMARY"),
                            types::Value::make_int(seq++),
                            types::Value::make_string(cname),
                            types::Value::make_string("A"),
                            types::Value::make_int(card),
                            types::Value::make_null(),
                            types::Value::make_null(),
                            types::Value::make_string(nullable ? "YES" : ""),
                            types::Value::make_string("BTREE"),
                            types::Value::make_string(""),
                            types::Value::make_string(""),
                        });
                    }
                }
            }
        }
        return tbl;
    }

    if (ci_eq(table_name, "COLUMNS")) {
        storage::TableDef def;
        def.name = "COLUMNS";
        def.columns = {
            is_col("TABLE_CATALOG",          types::SqlType::Varchar),
            is_col("TABLE_SCHEMA",           types::SqlType::Varchar),
            is_col("TABLE_NAME",             types::SqlType::Varchar),
            is_col("COLUMN_NAME",            types::SqlType::Varchar),
            is_col("ORDINAL_POSITION",       types::SqlType::BigInt),
            is_col("COLUMN_DEFAULT",         types::SqlType::Varchar),
            is_col("IS_NULLABLE",            types::SqlType::Varchar),
            is_col("DATA_TYPE",              types::SqlType::Varchar),
            is_col("CHARACTER_MAXIMUM_LENGTH", types::SqlType::BigInt),
            is_col("CHARACTER_OCTET_LENGTH", types::SqlType::BigInt),
            is_col("NUMERIC_PRECISION",      types::SqlType::BigInt),
            is_col("NUMERIC_SCALE",          types::SqlType::BigInt),
            is_col("DATETIME_PRECISION",     types::SqlType::BigInt),
            is_col("CHARACTER_SET_NAME",     types::SqlType::Varchar),
            is_col("COLLATION_NAME",         types::SqlType::Varchar),
            is_col("COLUMN_TYPE",            types::SqlType::Varchar),
            is_col("COLUMN_KEY",             types::SqlType::Varchar),
            is_col("EXTRA",                  types::SqlType::Varchar),
            is_col("PRIVILEGES",             types::SqlType::Varchar),
            is_col("COLUMN_COMMENT",         types::SqlType::Varchar),
            is_col("GENERATION_EXPRESSION",  types::SqlType::Varchar),
        };
        auto tbl = std::make_unique<storage::Table>(std::move(def));
        for (auto& db : catalog_.list_databases()) {
            for (auto& n : catalog_.table_names(db)) {
                auto* t = catalog_.get_table(n, db);
                if (!t) continue;
                auto& tdef = t->def();
                int pos = 1;
                for (auto& col : tdef.columns) {
                    bool is_str = col.type.type == types::SqlType::Varchar ||
                                  col.type.type == types::SqlType::Char ||
                                  col.type.type == types::SqlType::Text;
                    auto num_prec = [&]() -> types::Value {
                        switch (col.type.type) {
                            case types::SqlType::TinyInt:  return types::Value::make_int(3);
                            case types::SqlType::SmallInt: return types::Value::make_int(5);
                            case types::SqlType::Int:      return types::Value::make_int(10);
                            case types::SqlType::BigInt:   return types::Value::make_int(19);
                            case types::SqlType::Float:    return types::Value::make_int(12);
                            case types::SqlType::Double:   return types::Value::make_int(22);
                            case types::SqlType::Decimal:  return types::Value::make_int(col.type.length ? col.type.length : 10);
                            default: return types::Value::make_null();
                        }
                    };
                    auto num_scale = [&]() -> types::Value {
                        switch (col.type.type) {
                            case types::SqlType::Decimal: return types::Value::make_int(col.type.scale);
                            case types::SqlType::Float:
                            case types::SqlType::Double:  return types::Value::make_null();
                            default: return types::Value::make_int(0);
                        }
                    };
                    std::string key;
                    if (col.is_primary_key) key = "PRI";
                    tbl->insert({
                        types::Value::make_string("def"),
                        types::Value::make_string(db),
                        types::Value::make_string(n),
                        types::Value::make_string(col.name),
                        types::Value::make_int(pos++),
                        col.default_value ? types::Value::make_string(col.default_value->literal.to_string())
                                          : types::Value::make_null(),
                        types::Value::make_string(col.nullable ? "YES" : "NO"),
                        types::Value::make_string(is_data_type(col)),
                        is_str ? types::Value::make_int(col.type.length ? col.type.length : 255)
                               : types::Value::make_null(),
                        is_str ? types::Value::make_int((col.type.length ? col.type.length : 255) * 4)
                               : types::Value::make_null(),
                        num_prec(),
                        num_scale(),
                        (col.type.type == types::SqlType::DateTime || col.type.type == types::SqlType::Timestamp)
                            ? types::Value::make_int(0) : types::Value::make_null(),
                        is_str ? types::Value::make_string("utf8mb4") : types::Value::make_null(),
                        is_str ? types::Value::make_string("utf8mb4_general_ci") : types::Value::make_null(),
                        types::Value::make_string(is_column_type(col)),
                        types::Value::make_string(key),
                        types::Value::make_string(col.is_auto_increment ? "auto_increment" : ""),
                        types::Value::make_string("select,insert,update,references"),
                        types::Value::make_string(col.comment.empty() ? "" : col.comment),
                        types::Value::make_string(""),
                    });
                }
            }
        }
        return tbl;
    }
    return nullptr;
}

// ---------- CREATE DATABASE ----------
ResultSet Executor::exec_create_database(const ast::CreateDatabaseStmt& s) {
    ResultSet r;
    if (!catalog_.create_database(s.database_name, s.if_not_exists)) {
        r.ok = false; r.mysql_errno = 1007; r.sql_state = "HY000";
        r.error_message = fmt::format("Can't create database '{}'; database exists", s.database_name);
    } else {
        core::Database::instance().wal().append_create_database(s.database_name);
    }
    return r;
}

// ---------- DROP DATABASE ----------
ResultSet Executor::exec_drop_database(const ast::DropDatabaseStmt& s) {
    ResultSet r;
    // MySQL 允许删除当前连接所在的库 (GAP #8)
    if (!catalog_.drop_database(s.database_name, s.if_exists)) {
        r.ok = false; r.mysql_errno = 1008; r.sql_state = "HY000";
        r.error_message = fmt::format("Can't drop database '{}'; database doesn't exist", s.database_name);
    } else {
        core::Database::instance().wal().append_drop_database(s.database_name);
    }
    return r;
}

} // namespace sqltiny::executor
