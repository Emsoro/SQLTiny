#include "parser/ast.h"

namespace sqltiny::ast {

ExprPtr Expression::make_literal(types::Value v) {
    auto e = std::make_unique<Expression>();
    e->kind = ExprKind::Literal;
    e->literal = std::move(v);
    return e;
}
ExprPtr Expression::make_column(std::string col) {
    auto e = std::make_unique<Expression>();
    e->kind = ExprKind::Column;
    e->column_name = std::move(col);
    return e;
}
ExprPtr Expression::make_column(std::string table, std::string col) {
    auto e = std::make_unique<Expression>();
    e->kind = ExprKind::Column;
    e->table_name = std::move(table);
    e->column_name = std::move(col);
    return e;
}
ExprPtr Expression::make_binary(BinOp op, ExprPtr l, ExprPtr r) {
    auto e = std::make_unique<Expression>();
    e->kind = ExprKind::BinaryOp;
    e->binop = op;
    e->lhs = std::move(l);
    e->rhs = std::move(r);
    return e;
}
ExprPtr Expression::make_star(std::string table) {
    auto e = std::make_unique<Expression>();
    e->kind = ExprKind::Star;
    e->table_star = !table.empty();
    e->table_name = std::move(table);
    return e;
}
ExprPtr Expression::make_function(std::string name, std::vector<ExprPtr> args, bool star) {
    auto e = std::make_unique<Expression>();
    e->kind = ExprKind::FunctionCall;
    e->func_name = std::move(name);
    e->func_args = std::move(args);
    e->func_star_arg = star;
    return e;
}
ExprPtr Expression::make_subquery(StmtPtr stmt) {
    auto e = std::make_unique<Expression>();
    e->kind = ExprKind::Subquery;
    e->subquery_stmt = std::move(stmt);
    return e;
}

namespace {

std::unique_ptr<SelectStmt> clone_select(const SelectStmt& s) {
    auto out = std::make_unique<SelectStmt>();
    for (const auto& cte : s.ctes) {
        SelectStmt::CteDef c;
        c.name = cte.name;
        c.column_names = cte.column_names;
        c.recursive = cte.recursive;
        c.query = cte.query ? clone_stmt(*cte.query) : nullptr;
        out->ctes.push_back(std::move(c));
    }
    out->distinct = s.distinct;
    out->explain = s.explain;
    for (const auto& [expr, alias] : s.columns)
        out->columns.emplace_back(expr ? expr->clone() : nullptr, alias);
    out->from_tables = s.from_tables;
    out->from_aliases = s.from_aliases;
    for (const auto& d : s.from_derived)
        out->from_derived.push_back(d ? clone_stmt(*d) : nullptr);
    for (const auto& j : s.joins) {
        JoinClause jc;
        jc.kind = j.kind;
        jc.table_name = j.table_name;
        jc.alias = j.alias;
        jc.on = j.on ? j.on->clone() : nullptr;
        jc.derived = j.derived ? clone_stmt(*j.derived) : nullptr;
        out->joins.push_back(std::move(jc));
    }
    out->where = s.where ? s.where->clone() : nullptr;
    for (const auto& [expr, asc] : s.group_by)
        out->group_by.emplace_back(expr ? expr->clone() : nullptr, asc);
    out->having = s.having ? s.having->clone() : nullptr;
    for (const auto& [expr, asc] : s.order_by)
        out->order_by.emplace_back(expr ? expr->clone() : nullptr, asc);
    out->limit = s.limit;
    out->offset = s.offset;
    out->set_op = s.set_op;
    out->set_right = s.set_right ? clone_stmt(*s.set_right) : nullptr;
    return out;
}

} // namespace

StmtPtr clone_stmt(const Statement& st) {
    // 子查询/CTE/派生表只会是 SELECT
    if (st.kind != StmtKind::Select || !st.select_) return nullptr;
    auto out = std::make_unique<Statement>();
    out->kind = StmtKind::Select;
    out->select_ = clone_select(*st.select_);
    out->raw = st.raw;
    return out;
}

ExprPtr Expression::clone() const {
    auto e = std::make_unique<Expression>();
    e->kind = kind;
    e->literal = literal;
    e->table_name = table_name;
    e->column_name = column_name;
    e->table_star = table_star;
    e->binop = binop;
    e->lhs = lhs ? lhs->clone() : nullptr;
    e->rhs = rhs ? rhs->clone() : nullptr;
    e->unary = unary;
    e->operand = operand ? operand->clone() : nullptr;
    e->func_name = func_name;
    for (const auto& a : func_args) e->func_args.push_back(a ? a->clone() : nullptr);
    e->func_star_arg = func_star_arg;
    e->func_distinct = func_distinct;
    e->is_window = is_window;
    for (auto& p : win_partition) e->win_partition.push_back(p ? p->clone() : nullptr);
    for (auto& [o, asc] : win_order) e->win_order.push_back({o ? o->clone() : nullptr, asc});
    e->subquery_stmt = subquery_stmt ? clone_stmt(*subquery_stmt) : nullptr;
    e->subquery_exists = subquery_exists;
    e->case_base = case_base ? case_base->clone() : nullptr;
    for (const auto& [w, r] : case_whens)
        e->case_whens.emplace_back(w ? w->clone() : nullptr, r ? r->clone() : nullptr);
    e->case_else = case_else ? case_else->clone() : nullptr;
    return e;
}

} // namespace sqltiny::ast
