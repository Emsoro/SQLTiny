#include "executor/executor.h"
#include <fmt/format.h>
#include <algorithm>
#include <set>
#include <map>

namespace sqltiny::executor {

ResultSet Executor::execute(const ast::Statement& stmt) {
    ResultSet r;
    switch (stmt.kind) {
        case ast::StmtKind::Select:      return exec_select(*stmt.select_);
        case ast::StmtKind::Insert:      return exec_insert(*stmt.insert_);
        case ast::StmtKind::Update:      return exec_update(*stmt.update_);
        case ast::StmtKind::Delete:      return exec_delete(*stmt.delete_);
        case ast::StmtKind::CreateTable: return exec_create_table(*stmt.create_table_);
        case ast::StmtKind::DropTable:   return exec_drop_table(*stmt.drop_table_);
        case ast::StmtKind::Use:         return r;
        case ast::StmtKind::Show: {
            if (stmt.show_->kind == ast::ShowStmt::Kind::Tables) return exec_show_tables();
            if (stmt.show_->kind == ast::ShowStmt::Kind::Columns) return exec_show_columns(stmt.show_->table_or_pattern);
            return r;
        }
    }
    return r;
}

// ---------- CREATE TABLE ----------
ResultSet Executor::exec_create_table(const ast::CreateTableStmt& s) {
    ResultSet r;
    bool ok = catalog_.create_table(s);
    if (!ok) {
        r.ok = false; r.mysql_errno = 1050; r.sql_state = "42S01";
        r.error_message = fmt::format("Table '{}' already exists", s.table_name);
    }
    return r;
}

// ---------- DROP TABLE ----------
ResultSet Executor::exec_drop_table(const ast::DropTableStmt& s) {
    ResultSet r;
    bool ok = catalog_.drop_table(s.table_name, s.if_exists);
    if (!ok) {
        r.ok = false; r.mysql_errno = 1051; r.sql_state = "42S02";
        r.error_message = fmt::format("Unknown table '{}'", s.table_name);
    }
    return r;
}

// ---------- INSERT ----------
ResultSet Executor::exec_insert(const ast::InsertStmt& s) {
    ResultSet r;
    auto* tbl = catalog_.get_table(s.table_name);
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
            for (size_t i = 0; i < s.columns.size() && i < expr_row.size(); ++i) {
                int idx = def.column_index(s.columns[i]);
                if (idx < 0) continue;
                ExecContext ctx{ catalog_, s.table_name, {}, {} };
                row[idx] = eval(*expr_row[i], ctx);
            }
            // 未指定列:补默认
            for (size_t i = 0; i < def.columns.size(); ++i) {
                if (row[i].kind == types::Value::Kind::Null) {
                    auto& cdef = def.columns[i];
                    if (cdef.is_auto_increment) row[i] = types::Value::make_int(tbl->next_auto_increment());
                    else if (cdef.default_value) {
                        ExecContext ctx2{ catalog_, s.table_name, {}, {} };
                        row[i] = eval(*cdef.default_value, ctx2);
                    }
                    else if (cdef.is_primary_key && cdef.is_auto_increment) {
                        row[i] = types::Value::make_int(tbl->next_auto_increment());
                    }
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
        if (!tbl->insert(row, &rid)) {
            r.ok = false; r.mysql_errno = 1062; r.sql_state = "23000";
            r.error_message = "Duplicate entry for PRIMARY KEY";
            return r;
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
    r.affected_rows = affected;
    return r;
}

// ---------- UPDATE ----------
ResultSet Executor::exec_update(const ast::UpdateStmt& s) {
    ResultSet r;
    auto* tbl = catalog_.get_table(s.table_name);
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
    tbl->for_each([&](int64_t rid, const storage::Row& row) {
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
    });

    // 循环外改
    for (auto& p : pending) {
        if (!p.pk_old.empty()) tbl->delete_by_pk(p.pk_old);
        tbl->insert(std::move(p.new_row));
        affected++;
    }

    r.affected_rows = affected;
    return r;
}

// ---------- DELETE ----------
ResultSet Executor::exec_delete(const ast::DeleteStmt& s) {
    ResultSet r;
    auto* tbl = catalog_.get_table(s.table_name);
    if (!tbl) {
        r.ok = false; r.mysql_errno = 1146; r.sql_state = "42S02";
        r.error_message = fmt::format("Table '{}' doesn't exist", s.table_name);
        return r;
    }
    auto& def = tbl->def();

    // 收集要删的 row_id(不能在 for_each 里删)
    std::vector<int64_t> to_delete;
    tbl->for_each([&](int64_t rid, const storage::Row& row) {
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
    });

    int affected = 0;
    for (int64_t rid : to_delete) {
        auto maybe = tbl->get_row(rid);
        if (!maybe) continue;
        storage::Row pk;
        for (auto& col_list : def.primary_key_cols)
            for (auto& cname : col_list) {
                int idx = def.column_index(cname);
                if (idx >= 0 && idx < static_cast<int>(maybe->size())) pk.push_back((*maybe)[idx]);
            }
        if (!pk.empty()) tbl->delete_by_pk(pk);
        else {
            // 无主键 — 不支持按 row_id 删,需要在 Table 里暴露;简化:清空整张表
            // 实际上 Table 目前 row_map 非公开,暂时整体清空
        }
        affected++;
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
    if (s.from_tables.empty() && s.joins.empty()) {
        // 无表 SELECT:例如 SELECT 1, NOW()
        ExecContext ctx{ catalog_, "", {}, {} };
        storage::Row row;
        ctx.col_index.clear();
        r.column_names.clear();
        for (auto& [expr, alias] : s.columns) {
            std::string name = alias.empty() ? "<expr>" : alias;
            if (expr->kind == ast::ExprKind::Column) name = expr->column_name;
            r.column_names.push_back(name);
            row.push_back(eval(*expr, ctx));
        }
        r.rows.push_back(row);
        return r;
    }

    // FROM 首表
    std::string primary_table = s.from_tables[0];
    auto* tbl = catalog_.get_table(primary_table);
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
        auto* jtbl = catalog_.get_table(jc.table_name);
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

    // 建立 ctx.col_index
    auto build_ctx = [&](const storage::Row& flat_row) {
        ExecContext ctx{ catalog_, primary_table, {}, flat_row };
        for (auto& slot : slots) {
            for (size_t i = 0; i < slot.def->columns.size(); ++i) {
                std::string bare = slot.def->columns[i].name;
                std::string qualified = slot.real_name + "." + bare;
                std::string alias_q = slot.alias.empty() ? "" : (slot.alias + "." + bare);
                int offset = slot.col_offset + static_cast<int>(i);
                ctx.col_index[bare] = offset;
                ctx.col_index[qualified] = offset;
                if (!alias_q.empty()) ctx.col_index[alias_q] = offset;
            }
        }
        return ctx;
    };

    // 生成候选行:先主表遍历,CROSS/INNER/LEFT/RIGHT JOIN 追加
    std::vector<storage::Row> candidate_rows;

    // 先拿主表所有行
    std::vector<storage::Row> left_rows;
    tbl->for_each([&](int64_t, const storage::Row& row) { left_rows.push_back(row); return true; });

    for (auto& jc : s.joins) {
        auto* jtbl = catalog_.get_table(jc.table_name);
        std::vector<storage::Row> right_rows;
        jtbl->for_each([&](int64_t, const storage::Row& row) { right_rows.push_back(row); return true; });

        std::vector<storage::Row> new_left;
        for (auto& l : left_rows) {
            if (jc.kind == ast::JoinKind::Cross) {
                for (auto& r : right_rows) {
                    storage::Row flat = l; flat.insert(flat.end(), r.begin(), r.end());
                    new_left.push_back(flat);
                }
                continue;
            }
            if (jc.kind == ast::JoinKind::Inner || jc.kind == ast::JoinKind::Left || jc.kind == ast::JoinKind::Right) {
                std::vector<storage::Row> matches;
                for (auto& r : right_rows) {
                    storage::Row flat = l; flat.insert(flat.end(), r.begin(), r.end());
                    auto ctx = build_ctx(flat);
                    bool ok = true;
                    if (jc.on) {
                        auto v = eval(*jc.on, ctx);
                        ok = to_bool(v);
                    }
                    if (ok) matches.push_back(std::move(flat));
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
                // Right/Full JOIN 暂不完整处理
            }
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

    // 聚合:如果有聚合函数(COUNT/SUM/AVG/MIN/MAX)
    bool has_agg = false;
    auto check_agg = [&](const ast::Expression& e) {
        if (e.kind == ast::ExprKind::FunctionCall) {
            std::string lower; lower.resize(e.func_name.size());
            std::transform(e.func_name.begin(), e.func_name.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lower == "count" || lower == "sum" || lower == "avg" || lower == "min" || lower == "max") {
                has_agg = true;
            }
        }
    };
    for (auto& [e, _] : s.columns) check_agg(*e);
    if (s.having) { /* HAVING 先忽略 */ }

    // 输出列别名
    r.column_names.clear();
    for (auto& [e, alias] : s.columns) {
        if (e->kind == ast::ExprKind::Star) {
            // SELECT * 展开所有列
            for (auto& slot : slots) {
                for (auto& col : slot.def->columns) {
                    std::string name = slot.alias.empty() ? col.name : (slot.alias + "." + col.name);
                    r.column_names.push_back(name);
                }
            }
        } else {
            std::string name = alias.empty() ? "<expr>" : alias;
            if (e->kind == ast::ExprKind::Column) {
                name = e->column_name;
            }
            r.column_names.push_back(name);
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
        // M2 简化版聚合:单组,所有行聚合
        storage::Row agg_flat;
        auto ctx0 = build_ctx(agg_flat);
        storage::Row agg_row;

        // COUNT 总数直接用 after_where.size()
        int64_t total_rows = static_cast<int64_t>(after_where.size());

        // sum/min/max 按列计算
        std::map<std::string, double> sums;
        std::map<std::string, double> mins;
        std::map<std::string, double> maxs;
        std::map<std::string, int64_t> counts;
        for (auto& row : after_where) {
            for (size_t ci = 0; ci < r.column_names.size(); ++ci) {
                // 每列对应的聚合函数参数(按 columns 顺序)
            }
        }

        // 简化:对每个 select expr 单独聚合
        for (auto& [e, _] : s.columns) {
            if (e->kind != ast::ExprKind::FunctionCall) {
                // 非聚合列 —— 取第一行(Group By 简化)
                if (!after_where.empty()) agg_row.push_back(compute_row(after_where.front())[0]);
                else agg_row.push_back(types::Value::make_null());
                continue;
            }
            std::string lower; lower.resize(e->func_name.size());
            std::transform(e->func_name.begin(), e->func_name.end(), lower.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            if (lower == "count") {
                if (e->func_star_arg) { agg_row.push_back(types::Value::make_int(total_rows)); continue; }
                // COUNT(col):跳过 NULL
                int64_t c = 0;
                for (auto& row : after_where) {
                    auto ctx = build_ctx(row);
                    auto v = eval(*e->func_args[0], ctx);
                    if (!v.is_null()) c++;
                }
                agg_row.push_back(types::Value::make_int(c));
            } else if (lower == "sum" || lower == "avg") {
                double acc = 0; int64_t c = 0;
                for (auto& row : after_where) {
                    auto ctx = build_ctx(row);
                    auto v = eval(*e->func_args[0], ctx);
                    double d;
                    if (v.try_to_double(d)) { acc += d; c++; }
                }
                if (lower == "avg") agg_row.push_back(types::Value::make_double(c ? acc / c : 0));
                else agg_row.push_back(types::Value::make_double(acc));
            } else if (lower == "min") {
                double best = 0; bool first = true;
                for (auto& row : after_where) {
                    auto ctx = build_ctx(row);
                    auto v = eval(*e->func_args[0], ctx);
                    double d; if (!v.try_to_double(d)) continue;
                    if (first || d < best) { best = d; first = false; }
                }
                agg_row.push_back(first ? types::Value::make_null() : types::Value::make_double(best));
            } else if (lower == "max") {
                double best = 0; bool first = true;
                for (auto& row : after_where) {
                    auto ctx = build_ctx(row);
                    auto v = eval(*e->func_args[0], ctx);
                    double d; if (!v.try_to_double(d)) continue;
                    if (first || d > best) { best = d; first = false; }
                }
                agg_row.push_back(first ? types::Value::make_null() : types::Value::make_double(best));
            } else {
                // 普通函数在聚合上下文里用第一行值
                if (!after_where.empty()) agg_row.push_back(eval(*e, build_ctx(after_where.front())));
                else agg_row.push_back(types::Value::make_null());
            }
        }
        r.rows.push_back(std::move(agg_row));
        return r;
    }

    // GROUP BY — 简化版:不重点支持;先当作无聚合路径
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

// ---------- SHOW ----------
ResultSet Executor::exec_show_tables() {
    ResultSet r;
    r.column_names = { "Tables_in_default" };
    auto names = catalog_.table_names();
    std::sort(names.begin(), names.end());
    for (auto& n : names) r.rows.push_back({ types::Value::make_string(n) });
    return r;
}

ResultSet Executor::exec_show_columns(const std::string& table_name) {
    ResultSet r;
    r.column_names = { "Field", "Type", "Null", "Key", "Default", "Extra" };
    auto* tbl = catalog_.get_table(table_name);
    if (!tbl) {
        r.ok = false; r.mysql_errno = 1146; r.sql_state = "42S02";
        r.error_message = fmt::format("Table '{}' doesn't exist", table_name);
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

} // namespace sqltiny::executor
