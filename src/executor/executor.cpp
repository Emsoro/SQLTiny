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
    return k;
}

// WAL 小工具:拆 "db.table" 或 "table" → (db_or_empty, table)
static std::pair<std::string, std::string> split_qname(const std::string& q) {
    auto dot = q.find('.');
    if (dot == std::string::npos) return {"", q};
    return {q.substr(0, dot), q.substr(dot + 1)};
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
                        row[i] = eval(*cdef.default_value, ctx2);
                    }
                    else row[i] = types::Value::make_null();
                }
            }
        } else {
            // 按位置对齐
            ExecContext ctx{ catalog_, s.table_name, {}, {} };
            for (auto& e : expr_row) row.push_back(eval(*e, ctx));
            for (size_t i = row.size(); i < def.columns.size(); ++i) {
                auto& cdef = def.columns[i];
                if (cdef.is_auto_increment) row.push_back(types::Value::make_int(tbl->next_auto_increment()));
                else if (cdef.default_value) row.push_back(eval(*cdef.default_value, ctx));
                else row.push_back(types::Value::make_null());
            }
        }

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
        // 更新 last_insert_id(最后一个插入的 auto_increment 值)
        for (auto& col_list : def.primary_key_cols) {
            for (auto& cname : col_list) {
                int idx = def.column_index(cname);
                if (idx >= 0 && idx < static_cast<int>(row.size()) && row[idx].is_int()) {
                    auto& cd = def.columns[idx];
                    if (cd.is_auto_increment || (cd.is_primary_key && def.primary_key_cols.size() == 1)) {
                        r.last_insert_id = row[idx].v_int;
                    }
                }
            }
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
ResultSet Executor::exec_select(const ast::SelectStmt& s) {
    ResultSet r;

    // 推断表达式静态类型 — 供 wire 协议 field_type 用
    struct _SlotRef {
        const std::string& alias_or_real;
        const storage::TableDef* def;
    };
    // 用 std::function 支持递归
    std::function<types::SqlType(const ast::Expression&, const std::vector<_SlotRef>*)> infer_type =
        [&](const ast::Expression& e, const std::vector<_SlotRef>* slots) -> types::SqlType {
        using namespace types;
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
                // MySQL 语义:SUM(整型)->整型, SUM(浮点)->DOUBLE,避免聚合值以字符串回包
                if (!e.func_args.empty() && slots) {
                    auto t = infer_type(*e.func_args[0], slots);
                    if (is_numeric(t))
                        return (t == SqlType::Double || t == SqlType::Float) ? SqlType::Double
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
        storage::Row row;
        ctx.col_index.clear();
        r.column_names.clear();
        r.column_types.clear();
        for (auto& [expr, alias] : s.columns) {
            std::string name = alias.empty() ? "<expr>" : alias;
            if (expr->kind == ast::ExprKind::Column) name = expr->column_name;
            r.column_names.push_back(name);
            r.column_types.push_back(infer_type(*expr, nullptr));
            row.push_back(eval(*expr, ctx));
        }
        r.rows.push_back(row);
        return r;
    }

    // FROM 首表 —— information_schema.TABLES / COLUMNS 走虚拟表 (元数据客户端常用)
    std::string primary_table = s.from_tables[0];
    std::vector<std::unique_ptr<storage::Table>> virtual_tables;   // 保活虚拟表
    auto resolve_table = [&](const std::string& name) -> storage::Table* {
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
                std::string qualified = slot.real_name + "." + bare;
                std::string alias_q = slot.alias.empty() ? "" : (slot.alias + "." + bare);
                int offset = slot.col_offset + static_cast<int>(i);
                base_col_index[bare] = offset;
                base_col_index[qualified] = offset;
                if (!alias_q.empty()) base_col_index[alias_q] = offset;
                ++name_count[bare];
            }
        }
        for (auto& [n, c] : name_count) if (c > 1) ambiguous_cols.insert(n);
    }
    auto build_ctx = [&](const storage::Row& flat_row) {
        ExecContext ctx{ catalog_, primary_table, {}, flat_row };
        ctx.exec = this;   // 子查询递归求值需要
        ctx.col_index = base_col_index;
        return ctx;
    };

    // M6 P2 · JOIN 执行计划: 每个 join 选 HASH / NESTED_LOOP / CROSS。
    // Hash Join 把等值 JOIN 从 O(N×M) 降到 O(N+M);非等值条件仍退化到嵌套循环,
    // 但列索引只构建一次(见上方 base_col_index),常数开销大幅下降。
    struct JoinPlan {
        ast::JoinKind kind;
        EquiJoinKey equi;
        bool pure_equi = false;         // ON 就是单个等值条件
        bool hash_join = false;
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
                extra = fmt::format("Using hash join (build: {}, cost {} vs nested {})",
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
        return r;
    }

    // 生成候选行:先主表遍历,CROSS/INNER/LEFT/RIGHT JOIN 追加
    std::vector<storage::Row> candidate_rows;

    // 先拿主表所有行 (MVCC:querySCN)
    uint64_t sqscn = query_scn(session_);
    uint64_t rtxn = reader_txn(session_);
    std::vector<storage::Row> left_rows;
    tbl->for_each_visible(sqscn, [&](int64_t, const storage::Row& row) { left_rows.push_back(row); return true; }, rtxn);

    for (size_t ji = 0; ji < s.joins.size(); ++ji) {
        auto& jc = s.joins[ji];
        auto* jtbl = resolve_table(jc.table_name);
        std::vector<storage::Row> right_rows;
        jtbl->for_each_visible(sqscn, [&](int64_t, const storage::Row& row) { right_rows.push_back(row); return true; }, rtxn);
        const JoinPlan& plan = join_plans[ji];

        // ---- Hash Join: 右表按等值 key 分桶,左表 O(1) probe ----
        std::unordered_map<std::string, std::vector<size_t>> buckets;
        if (plan.hash_join) {
            for (size_t ri = 0; ri < right_rows.size(); ++ri) {
                if (plan.equi.right_col < 0 ||
                    plan.equi.right_col >= static_cast<int>(right_rows[ri].size())) continue;
                std::string key = join_hash_key(right_rows[ri][plan.equi.right_col]);
                if (!key.empty()) buckets[std::move(key)].push_back(ri);
            }
        }

        std::vector<storage::Row> new_left;
        for (auto& l : left_rows) {
            if (jc.kind == ast::JoinKind::Cross) {
                for (auto& r : right_rows) {
                    storage::Row flat = l; flat.insert(flat.end(), r.begin(), r.end());
                    new_left.push_back(std::move(flat));
                }
                continue;
            }
            if (jc.kind != ast::JoinKind::Inner && jc.kind != ast::JoinKind::Left &&
                jc.kind != ast::JoinKind::Right) continue;

            std::vector<storage::Row> matches;
            if (plan.hash_join && plan.equi.left_col >= 0 &&
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
                for (auto& r : right_rows) {
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
            // Right/Full JOIN 暂不完整处理(保持既有语义)
        }
        left_rows = std::move(new_left);
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

    // 聚合检测: SELECT 列(含嵌套在标量函数内的聚合)、GROUP BY、HAVING 任一出现即走聚合路径。
    // 旧实现只扫 SELECT 顶层,COALESCE(SUM(x),0) 与「SELECT 无聚合但有 GROUP BY/HAVING」会被漏掉。
    auto is_agg_func = [](const std::string& n) {
        std::string low = n;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return low == "count" || low == "sum" || low == "avg" || low == "min" || low == "max";
    };
    std::function<bool(const ast::Expression&)> contains_agg =
        [&](const ast::Expression& e) -> bool {
            if (e.kind == ast::ExprKind::FunctionCall) {
                if (is_agg_func(e.func_name)) return true;
                for (auto& a : e.func_args) if (a && contains_agg(*a)) return true;
            }
            if (e.lhs && contains_agg(*e.lhs)) return true;
            if (e.rhs && contains_agg(*e.rhs)) return true;
            if (e.operand && contains_agg(*e.operand)) return true;
            return false;
        };
    bool has_agg = !s.group_by.empty() || static_cast<bool>(s.having);
    for (auto& [e, _] : s.columns) if (contains_agg(*e)) has_agg = true;

    // 输出列别名
    r.column_names.clear();
    r.column_types.clear();
    // 构造 slot refs 供 infer_type 用
    std::vector<_SlotRef> slot_refs;
    for (auto& slot : slots) {
        slot_refs.push_back({ slot.alias.empty() ? slot.real_name : slot.alias, slot.def });
    }
    for (auto& [e, alias] : s.columns) {
        if (e->kind == ast::ExprKind::Star) {
            // SELECT * 展开所有列
            for (auto& slot : slots) {
                for (auto& col : slot.def->columns) {
                    std::string name = slot.alias.empty() ? col.name : (slot.alias + "." + col.name);
                    r.column_names.push_back(name);
                    r.column_types.push_back(col.type.type);
                }
            }
        } else {
            std::string name = alias.empty() ? "<expr>" : alias;
            if (e->kind == ast::ExprKind::Column) {
                name = e->column_name;
            }
            r.column_names.push_back(name);
            r.column_types.push_back(infer_type(*e, &slot_refs));
        }
    }

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
        std::vector<std::string> bucket_keys;   // 并行存分桶 key,保持输入顺序

        // 生成每个 flat row 的分组 key (vector<Value>)
        auto group_key_str = [&](const storage::Row& flat) -> std::string {
            if (s.group_by.empty()) return "";   // 空 group_by = 单组 (MySQL 允许,等效全表一组)
            auto ctx = build_ctx(flat);
            std::string k;
            for (auto& [e, _] : s.group_by) {
                auto v = eval(*e, ctx);
                k += v.to_string() + "\x01";
            }
            return k;
        };

        // 按 key 分桶 (保持输入顺序,稳定)
        for (auto& flat : after_where) {
            std::string key = group_key_str(flat);
            size_t bi = 0;
            bool found = false;
            for (size_t i = 0; i < bucket_keys.size(); ++i) {
                if (bucket_keys[i] == key) { bi = i; found = true; break; }
            }
            if (!found) { bucket_keys.push_back(key); buckets.push_back({}); bi = buckets.size() - 1; }
            buckets[bi].rows.push_back(std::move(flat));
        }
        // 空表 + 无 group_by — 生成一个空桶(对应 SELECT COUNT(*) FROM empty_t)
        if (buckets.empty()) buckets.push_back({});

        // 对每个桶算聚合行 (抽 lambda,可复用)
        auto agg_one_bucket = [&](const std::vector<storage::Row>& flat_rows) -> storage::Row {
            storage::Row agg_row;
            int64_t total = static_cast<int64_t>(flat_rows.size());

            for (auto& [e, _] : s.columns) {
                if (e->kind != ast::ExprKind::FunctionCall) {
                    // 非聚合列
                    if (flat_rows.empty()) { agg_row.push_back(types::Value::make_null()); continue; }
                    // GROUP BY 列:取桶内第一行的值 (MySQL 默认 ANY_VALUE 语义)
                    auto ctx0 = build_ctx(flat_rows.front());
                    agg_row.push_back(eval(*e, ctx0));
                    continue;
                }
                std::string lower; lower.resize(e->func_name.size());
                std::transform(e->func_name.begin(), e->func_name.end(), lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });

                if (lower == "count") {
                    if (e->func_star_arg) { agg_row.push_back(types::Value::make_int(total)); continue; }
                    int64_t c = 0;
                    for (auto& row : flat_rows) {
                        auto ctx = build_ctx(row);
                        auto v = eval(*e->func_args[0], ctx);
                        if (!v.is_null()) c++;
                    }
                    agg_row.push_back(types::Value::make_int(c));
                } else if (lower == "sum" || lower == "avg") {
                    // 整型列用 int64 累加,避免大整数 SUM 经 double 以字符串回包
                    double acc = 0; int64_t iacc = 0; int64_t c = 0; bool all_int = true;
                    for (auto& row : flat_rows) {
                        auto ctx = build_ctx(row);
                        auto v = eval(*e->func_args[0], ctx);
                        if (v.is_null()) continue;
                        if (v.is_int()) { iacc += v.v_int; acc += static_cast<double>(v.v_int); }
                        else {
                            all_int = false;
                            double d; if (v.try_to_double(d)) acc += d;
                        }
                        c++;
                    }
                    // SQL 标准: 空集 SUM / AVG 返回 NULL (只有 COUNT 返回 0)
                    if (c == 0) agg_row.push_back(types::Value::make_null());
                    else if (lower == "avg") agg_row.push_back(types::Value::make_double(acc / c));
                    else if (all_int) agg_row.push_back(types::Value::make_int(iacc));
                    else agg_row.push_back(types::Value::make_double(acc));
                } else if (lower == "min" || lower == "max") {
                    // 保留原值类型(DATE/DATETIME/字符串),用 Value 原生比较 ——
                    // 旧实现经 try_to_double 会把日期 '2019-12-31' 截成 2019
                    types::Value best; bool first = true;
                    for (auto& row : flat_rows) {
                        auto ctx = build_ctx(row);
                        auto v = eval(*e->func_args[0], ctx);
                        if (v.is_null()) continue;
                        if (first) { best = v; first = false; continue; }
                        int cv = v.compare(best);
                        if ((lower == "min" && cv < 0) || (lower == "max" && cv > 0)) best = v;
                    }
                    agg_row.push_back(first ? types::Value::make_null() : best);
                } else if (contains_agg(*e)) {
                    // 标量函数嵌套聚合 (COALESCE(SUM(x),0)): 纯值递归,不构造临时 AST
                    std::function<types::Value(const ast::Expression&)> eval_nested =
                        [&](const ast::Expression& src) -> types::Value {
                            if (src.kind == ast::ExprKind::Literal) return src.literal;
                            if (src.kind == ast::ExprKind::FunctionCall) {
                                std::string low = src.func_name;
                                std::transform(low.begin(), low.end(), low.begin(),
                                               [](unsigned char c) { return std::tolower(c); });
                                if (is_agg_func(low)) {
                                    // 按当前桶重算该聚合 (与上面 sum/avg/min/max 分支同语义)
                                    std::vector<types::Value> vals;
                                    for (auto& row : flat_rows)
                                        vals.push_back(eval(*src.func_args[0], build_ctx(row)));
                                    if (low == "count") {
                                        if (src.func_star_arg)
                                            return types::Value::make_int(static_cast<int64_t>(flat_rows.size()));
                                        int64_t n = 0;
                                        for (auto& v : vals) if (!v.is_null()) ++n;
                                        return types::Value::make_int(n);
                                    }
                                    double acc = 0; int64_t iacc = 0; int64_t n = 0;
                                    bool all_int = true, first = true;
                                    types::Value best;
                                    for (auto& v : vals) {
                                        if (v.is_null()) continue;
                                        if (low == "min" || low == "max") {
                                            if (first) { best = v; first = false; }
                                            else {
                                                int cv = v.compare(best);
                                                if ((low == "min" && cv < 0) || (low == "max" && cv > 0))
                                                    best = v;
                                            }
                                        } else if (v.is_int()) {
                                            iacc += v.v_int; acc += static_cast<double>(v.v_int);
                                        } else {
                                            all_int = false;
                                            double d; if (v.try_to_double(d)) acc += d;
                                        }
                                        ++n;
                                    }
                                    if (n == 0) return types::Value::make_null();
                                    if (low == "avg") return types::Value::make_double(acc / n);
                                    if (low == "sum") return all_int ? types::Value::make_int(iacc)
                                                                     : types::Value::make_double(acc);
                                    return first ? types::Value::make_null() : best;
                                }
                                if (low == "coalesce" || low == "ifnull") {
                                    for (auto& a : src.func_args) {
                                        auto v = eval_nested(*a);
                                        if (!v.is_null()) return v;
                                    }
                                    return types::Value::make_null();
                                }
                            }
                            if (flat_rows.empty()) return types::Value::make_null();
                            return eval(src, build_ctx(flat_rows.front()));
                        };
                    agg_row.push_back(eval_nested(*e));
                } else {
                    // 普通函数在聚合上下文用桶内第一行
                    if (!flat_rows.empty()) agg_row.push_back(eval(*e, build_ctx(flat_rows.front())));
                    else agg_row.push_back(types::Value::make_null());
                }
            }
            return agg_row;
        };

        // 算每个桶
        std::vector<storage::Row> agg_rows;
        for (auto& b : buckets) agg_rows.push_back(agg_one_bucket(b.rows));

        // HAVING 过滤 (M3)
        if (s.having) {
            std::vector<storage::Row> filtered;
            // HAVING 表达式里的聚合函数要对整个桶算(不是单行),需要专门的求值器
            // std::function 解决递归 lambda 初始化问题
            std::function<types::Value(const ast::Expression&, const std::vector<storage::Row>&)> eval_having;
            eval_having = [&](const ast::Expression& expr, const std::vector<storage::Row>& bucket_rows) -> types::Value {
                // 先判断 expr 类型
                if (expr.kind == ast::ExprKind::Literal) return expr.literal;
                if (expr.kind == ast::ExprKind::Column) {
                    if (bucket_rows.empty()) return types::Value::make_null();
                    // 列引用:取桶内第一行的值(MySQL 允许非聚合列出现在 HAVING,ANY_VALUE 语义)
                    auto ctx0 = build_ctx(bucket_rows.front());
                    return eval(expr, ctx0);
                }
                if (expr.kind == ast::ExprKind::BinaryOp) {
                    auto l = eval_having(*expr.lhs, bucket_rows);
                    auto r = eval_having(*expr.rhs, bucket_rows);
                    switch (expr.binop) {
                        case ast::BinOp::Eq:  return types::Value::make_bool(l.sql_equals(r));
                        case ast::BinOp::Ne:  return types::Value::make_bool(!l.is_null() && !r.is_null() && l.compare(r) != 0);
                        case ast::BinOp::Gt:  return types::Value::make_bool(!l.is_null() && !r.is_null() && l.compare(r) >  0);
                        case ast::BinOp::Lt:  return types::Value::make_bool(!l.is_null() && !r.is_null() && l.compare(r) <  0);
                        case ast::BinOp::Ge:  return types::Value::make_bool(!l.is_null() && !r.is_null() && l.compare(r) >= 0);
                        case ast::BinOp::Le:  return types::Value::make_bool(!l.is_null() && !r.is_null() && l.compare(r) <= 0);
                        case ast::BinOp::And: return types::Value::make_bool(to_bool(l) && to_bool(r));
                        case ast::BinOp::Or:  return types::Value::make_bool(to_bool(l) || to_bool(r));
                        default: { auto ctx0 = build_ctx(bucket_rows.front()); return eval(expr, ctx0); }
                    }
                }
                if (expr.kind == ast::ExprKind::UnaryOp) {
                    auto v = eval_having(*expr.operand, bucket_rows);
                    if (expr.unary == ast::Expression::UnaryKind::Not) return types::Value::make_bool(!to_bool(v));
                    return v;
                }
                if (expr.kind == ast::ExprKind::FunctionCall) {
                    std::string lower; lower.resize(expr.func_name.size());
                    std::transform(expr.func_name.begin(), expr.func_name.end(), lower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (lower == "count" || lower == "sum" || lower == "avg" || lower == "min" || lower == "max") {
                        storage::Row fake_agg;   // 复用上面 agg_one_bucket 的逻辑?不行,会有递归
                        // 直接内联实现
                        if (lower == "count") {
                            if (expr.func_star_arg) return types::Value::make_int(static_cast<int64_t>(bucket_rows.size()));
                            int64_t c = 0;
                            for (auto& row : bucket_rows) {
                                auto ctx = build_ctx(row);
                                auto v = eval(*expr.func_args[0], ctx);
                                if (!v.is_null()) c++;
                            }
                            return types::Value::make_int(c);
                        } else {
                            double acc = 0; int64_t iacc = 0; int64_t c = 0;
                            bool all_int = true, first = true;
                            types::Value best;
                            for (auto& row : bucket_rows) {
                                auto ctx = build_ctx(row);
                                auto v = eval(*expr.func_args[0], ctx);
                                if (v.is_null()) continue;
                                if (lower == "min" || lower == "max") {
                                    if (first) { best = v; first = false; }
                                    else {
                                        int cv = v.compare(best);
                                        if ((lower == "min" && cv < 0) || (lower == "max" && cv > 0))
                                            best = v;
                                    }
                                } else if (v.is_int()) {
                                    iacc += v.v_int; acc += static_cast<double>(v.v_int);
                                } else {
                                    all_int = false;
                                    double d; if (v.try_to_double(d)) acc += d;
                                }
                                c++;
                            }
                            if (c == 0) return types::Value::make_null();
                            if (lower == "avg") return types::Value::make_double(acc / c);
                            if (lower == "sum") return all_int ? types::Value::make_int(iacc)
                                                               : types::Value::make_double(acc);
                            return first ? types::Value::make_null() : best;
                        }
                    }
                    // 非聚合函数:用桶内第一行算
                    if (!bucket_rows.empty()) {
                        auto ctx0 = build_ctx(bucket_rows.front());
                        return eval(expr, ctx0);
                    }
                    return types::Value::make_null();
                }
                // 其他:用第一行算
                if (!bucket_rows.empty()) {
                    auto ctx0 = build_ctx(bucket_rows.front());
                    return eval(expr, ctx0);
                }
                return types::Value::make_null();
            };
            for (size_t bi = 0; bi < agg_rows.size(); ++bi) {
                auto v = eval_having(*s.having, buckets[bi].rows);
                if (to_bool(v)) filtered.push_back(std::move(agg_rows[bi]));
            }
            agg_rows = std::move(filtered);
        }

        // ORDER BY 可作用于聚合结果行
        if (!s.order_by.empty()) {
            std::sort(agg_rows.begin(), agg_rows.end(), [&](const storage::Row& a, const storage::Row& b) {
                for (auto& [expr, asc] : s.order_by) {
                    // ORDER BY 表达式在聚合上下文 — 用桶内 flat row 做 ctx
                    storage::Row flat_a = a.empty() ? storage::Row() : buckets[0].rows.front(); // 简化
                    storage::Row flat_b = b.empty() ? storage::Row() : buckets[0].rows.front();
                    auto ctx_a = build_ctx(flat_a);
                    auto ctx_b = build_ctx(flat_b);
                    int c = eval(*expr, ctx_a).compare(eval(*expr, ctx_b));
                    if (c != 0) return asc ? c < 0 : c > 0;
                }
                return false;
            });
        }

        r.rows = std::move(agg_rows);
        r.affected_rows = static_cast<int64_t>(r.rows.size());
        return r;
    }
    // ORDER BY
    if (!s.order_by.empty()) {
        std::sort(after_where.begin(), after_where.end(), [&](const storage::Row& a, const storage::Row& b) {
            for (auto& [expr, asc] : s.order_by) {
                auto ctx_a = build_ctx(a);
                auto ctx_b = build_ctx(b);
                auto va = eval(*expr, ctx_a);
                auto vb = eval(*expr, ctx_b);
                int c = va.compare(vb);
                if (c != 0) return asc ? c < 0 : c > 0;
            }
            return false;
        });
    }

    // DISTINCT — 简化
    if (s.distinct) {
        std::vector<storage::Row> dedup;
        std::set<std::string> seen;
        for (auto& row : after_where) {
            storage::Row out = compute_row(row);
            std::string key;
            for (auto& v : out) key += v.to_string() + "\x01";
            if (seen.insert(key).second) dedup.push_back(std::move(out));
        }
        after_where = std::move(dedup);
    } else {
        std::vector<storage::Row> projected;
        for (auto& row : after_where) projected.push_back(compute_row(row));
        after_where = std::move(projected);
    }

    // LIMIT / OFFSET
    int64_t off = s.offset.value_or(0);
    int64_t lim = s.limit.value_or(static_cast<int64_t>(after_where.size()));
    if (off > 0 && static_cast<size_t>(off) < after_where.size()) {
        after_where.erase(after_where.begin(), after_where.begin() + off);
    }
    if (lim >= 0 && static_cast<size_t>(lim) < after_where.size()) {
        after_where.resize(static_cast<size_t>(lim));
    }

    r.rows = std::move(after_where);
    r.affected_rows = static_cast<int64_t>(r.rows.size());
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
        r.rows.push_back({
            types::Value::make_string(col.name),
            types::Value::make_string(type_str),
            types::Value::make_string(nul),
            types::Value::make_string(key),
            types::Value::make_null(),
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
        {"join_buffer_size", "262144"}, {"lower_case_table_names", "0"},
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
        return make_empty("KEY_COLUMN_USAGE", std::move(cols));
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
        return make_empty("STATISTICS", std::move(cols));
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
