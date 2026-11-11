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

} // namespace sqltiny::ast
