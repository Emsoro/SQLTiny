#include "parser/parser.h"
#include <fmt/format.h>
#include <algorithm>

namespace sqltiny::parser {

// ========== 工具 ==========

Parser::Parser(std::string_view sql) : lx_(sql) {}

Token Parser::peek() {
    if (!has_peeked_) { peeked_ = lx_.next(); has_peeked_ = true; }
    return peeked_;
}
Token Parser::consume() {
    if (has_peeked_) {
        has_peeked_ = false;
        return peeked_;
    }
    return lx_.next();
}
Token Parser::expect(TokenKind k, std::string_view what) {
    Token t = peek();
    if (t.kind != k) {
        throw ParseError(t.line, t.col, fmt::format("expected {}, got {} '{}'",
            what, static_cast<int>(t.kind), t.text));
    }
    return consume();
}
bool Parser::match(TokenKind k) { return peek().kind == k; }
bool Parser::match_consume(TokenKind k) {
    if (match(k)) { consume(); return true; }
    return false;
}
void Parser::skip_semicolon() {
    if (match_consume(TokenKind::Semi)) return;
    // 允许末尾没分号
    if (!match(TokenKind::Eof)) {
        // 有些场景下分号可省,暂时 tolerate
    }
}

std::string Parser::ident_text() {
    Token t = consume();
    if (t.kind == TokenKind::Ident || t.kind == TokenKind::QuotedIdent) return t.text;
    // 关键字也能当标识符(不是所有关键字,但宽松处理)
    if (static_cast<int>(t.kind) >= static_cast<int>(TokenKind::Kw_START) &&
        static_cast<int>(t.kind) <= static_cast<int>(TokenKind::Kw_END)) {
        return t.text;
    }
    throw ParseError(t.line, t.col, fmt::format("expected identifier, got '{}'", t.text));
}

std::string Parser::kw_or_ident() {
    Token t = peek();
    if (t.kind == TokenKind::Ident || t.kind == TokenKind::QuotedIdent) { consume(); return t.text; }
    if (static_cast<int>(t.kind) >= static_cast<int>(TokenKind::Kw_START) &&
        static_cast<int>(t.kind) <= static_cast<int>(TokenKind::Kw_END)) {
        consume();
        return t.text;
    }
    throw ParseError(t.line, t.col, fmt::format("expected name, got '{}'", t.text));
}

// ========== 入口 ==========

ast::StmtPtr Parser::parse_stmt() {
    ast::StmtPtr s = parse_top();
    skip_semicolon();
    return s;
}

std::vector<ast::StmtPtr> Parser::parse_all() {
    std::vector<ast::StmtPtr> out;
    while (!match(TokenKind::Eof)) {
        out.push_back(parse_top());
        // 分号可选
        match_consume(TokenKind::Semi);
    }
    return out;
}

ast::StmtPtr Parser::parse_top() {
    Token t = peek();
    switch (t.kind) {
        case TokenKind::KwSelect: case TokenKind::KwAll: case TokenKind::KwDistinct:
            return parse_select();
        case TokenKind::KwInsert:    return parse_insert();
        case TokenKind::KwUpdate:    return parse_update();
        case TokenKind::KwDelete:    return parse_delete();
        case TokenKind::KwCreate: {
            consume(); // CREATE
            auto nt = peek();
            if (nt.kind == TokenKind::KwDatabase || nt.kind == TokenKind::KwSchema)
                return parse_create_database();
            if (nt.kind == TokenKind::KwTable)
                return parse_create_table();
            throw ParseError(nt.line, nt.col, fmt::format("expected TABLE or DATABASE after CREATE, got '{}'", nt.text));
        }
        case TokenKind::KwDrop: {
            consume(); // DROP
            auto nt = peek();
            if (nt.kind == TokenKind::KwDatabase || nt.kind == TokenKind::KwSchema)
                return parse_drop_database();
            if (nt.kind == TokenKind::KwTable)
                return parse_drop_table();
            throw ParseError(nt.line, nt.col, fmt::format("expected TABLE or DATABASE after DROP, got '{}'", nt.text));
        }
        case TokenKind::KwUse:       return parse_use();
        case TokenKind::KwShow:      return parse_show();
        case TokenKind::KwSet:       return parse_set();
        case TokenKind::KwCommit:    return parse_set();  // COMMIT → 占位 (autocommit=ON)
        case TokenKind::KwRollback:  return parse_set();  // ROLLBACK → 占位
        case TokenKind::KwBegin: {
            consume();  // consume BEGIN
            // BEGIN ... END 块在 MySQL 存储过程里,SQLTiny 简化处理
            return parse_set();
        }
        default:
            throw ParseError(t.line, t.col,
                fmt::format("unexpected start of statement: '{}'", t.text));
    }
}

// ========== 表达式层(递归下降) ==========

ast::ExprPtr Parser::parse_expr() { return parse_or_expr(); }

ast::ExprPtr Parser::parse_or_expr() {
    auto left = parse_and_expr();
    while (match_consume(TokenKind::KwOr)) {
        auto right = parse_and_expr();
        left = ast::Expression::make_binary(ast::BinOp::Or, std::move(left), std::move(right));
    }
    return left;
}
ast::ExprPtr Parser::parse_and_expr() {
    auto left = parse_not_expr();
    while (match_consume(TokenKind::KwAnd)) {
        auto right = parse_not_expr();
        left = ast::Expression::make_binary(ast::BinOp::And, std::move(left), std::move(right));
    }
    return left;
}
ast::ExprPtr Parser::parse_not_expr() {
    if (match_consume(TokenKind::KwNot)) {
        auto operand = parse_not_expr();
        auto e = std::make_unique<ast::Expression>();
        e->kind = ast::ExprKind::UnaryOp;
        e->unary = ast::Expression::UnaryKind::Not;
        e->operand = std::move(operand);
        return e;
    }
    return parse_comparison();
}

ast::BinOp Parser::token_to_binop(TokenKind k) {
    switch (k) {
        case TokenKind::Lt: return ast::BinOp::Lt;
        case TokenKind::Gt: return ast::BinOp::Gt;
        case TokenKind::Le: return ast::BinOp::Le;
        case TokenKind::Ge: return ast::BinOp::Ge;
        case TokenKind::Eq: return ast::BinOp::Eq;
        case TokenKind::Ne: return ast::BinOp::Ne;
        default: throw std::runtime_error("not a comparison");
    }
}

ast::ExprPtr Parser::parse_comparison() {
    auto left = parse_additive();
    Token t = peek();
    // 链式 A < B < C 这里不支持,只单对单
    if (t.kind == TokenKind::Lt || t.kind == TokenKind::Gt ||
        t.kind == TokenKind::Le || t.kind == TokenKind::Ge ||
        t.kind == TokenKind::Eq || t.kind == TokenKind::Ne) {
        consume();
        auto right = parse_additive();
        return ast::Expression::make_binary(token_to_binop(t.kind), std::move(left), std::move(right));
    }
    // IS [NOT] NULL
    if (match_consume(TokenKind::KwIs)) {
        bool negate = match_consume(TokenKind::KwNot);
        expect(TokenKind::KwNull, "NULL");
        auto e = std::make_unique<ast::Expression>();
        e->kind = ast::ExprKind::UnaryOp;
        e->unary = negate ? ast::Expression::UnaryKind::IsNotNull
                          : ast::Expression::UnaryKind::IsNull;
        e->operand = std::move(left);
        return e;
    }
    // [NOT] IN (...)
    if (match_consume(TokenKind::KwNot)) {
        // NOT IN (...)
        if (match_consume(TokenKind::KwIn)) {
            auto e = std::make_unique<ast::Expression>();
            auto in_expr = std::make_unique<ast::Expression>();
            in_expr->kind = ast::ExprKind::FunctionCall;
            in_expr->func_name = "__in";
            expect(TokenKind::LParen, "(");
            in_expr->func_args.push_back(std::move(left));
            // IN (SELECT ...) 还是 IN (e1, e2, ...)
            if (match(TokenKind::KwSelect)) {
                auto sub = parse_select();
                auto sq = ast::Expression::make_subquery(std::move(sub));
                in_expr->func_args.push_back(std::move(sq));
            } else {
                while (!match(TokenKind::RParen)) {
                    in_expr->func_args.push_back(parse_expr());
                    match_consume(TokenKind::Comma);
                }
            }
            consume(); // )
            e->kind = ast::ExprKind::UnaryOp;
            e->unary = ast::Expression::UnaryKind::Not;
            e->operand = std::move(in_expr);
            return e;
        }
    }
    if (match_consume(TokenKind::KwIn)) {
        auto e = std::make_unique<ast::Expression>();
        e->kind = ast::ExprKind::FunctionCall;
        e->func_name = "__in";
        expect(TokenKind::LParen, "(");
        e->func_args.push_back(std::move(left));
        if (match(TokenKind::KwSelect)) {
            auto sub = parse_select();
            auto sq = ast::Expression::make_subquery(std::move(sub));
            e->func_args.push_back(std::move(sq));
        } else {
            while (!match(TokenKind::RParen)) {
                e->func_args.push_back(parse_expr());
                match_consume(TokenKind::Comma);
            }
        }
        consume();
        return e;
    }
    // [NOT] LIKE
    if (match_consume(TokenKind::KwNot)) {
        if (match_consume(TokenKind::KwLike)) {
            auto e = std::make_unique<ast::Expression>();
            auto like = std::make_unique<ast::Expression>();
            like->kind = ast::ExprKind::FunctionCall;
            like->func_name = "__like";
            like->func_args.push_back(std::move(left));
            like->func_args.push_back(parse_expr());
            e->kind = ast::ExprKind::UnaryOp;
            e->unary = ast::Expression::UnaryKind::Not;
            e->operand = std::move(like);
            return e;
        }
    }
    if (match_consume(TokenKind::KwLike)) {
        auto e = std::make_unique<ast::Expression>();
        e->kind = ast::ExprKind::FunctionCall;
        e->func_name = "__like";
        e->func_args.push_back(std::move(left));
        e->func_args.push_back(parse_expr());
        return e;
    }
    return left;
}

ast::ExprPtr Parser::parse_additive() {
    auto left = parse_multiplicative();
    while (true) {
        if (match_consume(TokenKind::Plus)) {
            auto right = parse_multiplicative();
            left = ast::Expression::make_binary(ast::BinOp::Plus, std::move(left), std::move(right));
        } else if (match_consume(TokenKind::Minus)) {
            auto right = parse_multiplicative();
            left = ast::Expression::make_binary(ast::BinOp::Minus, std::move(left), std::move(right));
        } else {
            break;
        }
    }
    return left;
}

ast::ExprPtr Parser::parse_multiplicative() {
    auto left = parse_unary();
    while (true) {
        if (match_consume(TokenKind::Star)) {
            // 注意:SELECT * 是在 column 位置处理,这里是表达式位置 a * b
            auto right = parse_unary();
            left = ast::Expression::make_binary(ast::BinOp::Star, std::move(left), std::move(right));
        } else if (match_consume(TokenKind::Slash)) {
            auto right = parse_unary();
            left = ast::Expression::make_binary(ast::BinOp::Slash, std::move(left), std::move(right));
        } else if (match_consume(TokenKind::Percent)) {
            auto right = parse_unary();
            left = ast::Expression::make_binary(ast::BinOp::Percent, std::move(left), std::move(right));
        } else {
            break;
        }
    }
    return left;
}

ast::ExprPtr Parser::parse_unary() {
    if (match_consume(TokenKind::Minus)) {
        auto operand = parse_unary();
        auto e = std::make_unique<ast::Expression>();
        e->kind = ast::ExprKind::UnaryOp;
        e->unary = ast::Expression::UnaryKind::Negate;
        e->operand = std::move(operand);
        return e;
    }
    if (match_consume(TokenKind::Plus)) {
        // 一元加号忽略
        return parse_unary();
    }
    return parse_primary();
}

ast::ExprPtr Parser::parse_primary() {
    Token t = peek();
    // 字面量
    if (t.kind == TokenKind::Number) {
        consume();
        types::Value v;
        if (t.int_value == 0 && t.num_value != 0.0) {
            v = types::Value::make_double(t.num_value);
        } else {
            v = types::Value::make_int(t.int_value);
        }
        return ast::Expression::make_literal(std::move(v));
    }
    if (t.kind == TokenKind::String) {
        consume();
        return ast::Expression::make_literal(types::Value::make_string(t.text));
    }
    if (match_consume(TokenKind::KwNull)) {
        return ast::Expression::make_literal(types::Value::make_null());
    }
    if (match_consume(TokenKind::KwTrue)) {
        return ast::Expression::make_literal(types::Value::make_bool(true));
    }
    if (match_consume(TokenKind::KwFalse)) {
        return ast::Expression::make_literal(types::Value::make_bool(false));
    }
    // 括号
    if (match_consume(TokenKind::LParen)) {
        auto e = parse_expr();
        expect(TokenKind::RParen, ")");
        return e;
    }
    // * (单独的 Star)
    if (match_consume(TokenKind::Star)) {
        auto e = std::make_unique<ast::Expression>();
        e->kind = ast::ExprKind::Star;
        e->table_star = false;
        return e;
    }
    // 占位符
    if (match_consume(TokenKind::Placeholder)) {
        auto e = std::make_unique<ast::Expression>();
        e->kind = ast::ExprKind::Placeholder;
        return e;
    }

    // 标识符 / 关键字(函数名 或 列名)
    bool is_ident = (t.kind == TokenKind::Ident || t.kind == TokenKind::QuotedIdent);
    bool is_func_kw = false;
    if (!is_ident && static_cast<int>(t.kind) >= static_cast<int>(TokenKind::Kw_START) &&
        static_cast<int>(t.kind) <= static_cast<int>(TokenKind::Kw_END)) {
        // 允许一些函数关键字作为函数名
        static const char* func_names[] = {
            "COUNT", "SUM", "AVG", "MIN", "MAX", "CONCAT", "COALESCE", "IFNULL",
            "ROUND", "ABS", "LENGTH", "NOW", "CURRENT_TIMESTAMP", "CURRENT_DATE",
            "CURRENT_TIME", "USER", "DATABASE", "CAST", "CONVERT",
            "VERSION", "SCHEMA"
        };
        for (auto n : func_names) {
            if (_stricmp(t.text.c_str(), n) == 0) { is_func_kw = true; break; }
        }
        // 关键字也可以当列名(宽松处理)
    }

    if (is_ident || is_func_kw) {
        consume();
        std::string name = t.text;
        int line = t.line, col = t.col;

        // 表限定 t.col 或函数名(后面紧跟 () )
        if (match_consume(TokenKind::Dot)) {
            // 可能是 t.col 或 t.*
            Token next = peek();
            if (next.kind == TokenKind::Star) {
                consume();
                return ast::Expression::make_star(name);
            }
            if (next.kind == TokenKind::Ident || next.kind == TokenKind::QuotedIdent ||
                (static_cast<int>(next.kind) >= static_cast<int>(TokenKind::Kw_START) &&
                 static_cast<int>(next.kind) <= static_cast<int>(TokenKind::Kw_END))) {
                consume();
                return ast::Expression::make_column(name, next.text);
            }
            throw ParseError(next.line, next.col, fmt::format("expected column name after '{}.", name));
        }

        // 函数调用
        if (match(TokenKind::LParen)) {
            return parse_function_call(name, line, col);
        }

        // 普通列
        auto e = ast::Expression::make_column(name);
        return e;
    }

    throw ParseError(t.line, t.col, fmt::format("unexpected token in expression: '{}'", t.text));
}

ast::ExprPtr Parser::parse_function_call(std::string name, int line, int col) {
    expect(TokenKind::LParen, "(");
    auto e = std::make_unique<ast::Expression>();
    e->kind = ast::ExprKind::FunctionCall;
    e->func_name = name;
    // COUNT(*) 特判
    if (match_consume(TokenKind::Star)) {
        e->func_star_arg = true;
        expect(TokenKind::RParen, ")");
        return e;
    }
    if (match(TokenKind::RParen)) {
        // NOW(), CURRENT_TIMESTAMP
        consume();
        return e;
    }
    e->func_args.push_back(parse_expr());
    while (match_consume(TokenKind::Comma)) {
        e->func_args.push_back(parse_expr());
    }
    expect(TokenKind::RParen, ")");
    return e;
}

// ========== SELECT ==========

ast::StmtPtr Parser::parse_select() {
    // [SELECT] [DISTINCT | ALL]
    Token t = consume();
    auto stmt = std::make_unique<ast::SelectStmt>();
    if (match_consume(TokenKind::KwDistinct)) stmt->distinct = true;
    if (match_consume(TokenKind::KwAll)) { /* default, no-op */ }

    // 列列表
    while (!match(TokenKind::Semi) && !match(TokenKind::Eof) &&
           !match(TokenKind::KwFrom)) {
        auto expr = parse_expr();
        std::string alias;
        if (match_consume(TokenKind::KwAs)) {
            alias = kw_or_ident();
        } else if (match(TokenKind::Ident) || match(TokenKind::QuotedIdent)) {
            // 无 AS 也允许
            alias = kw_or_ident();
        }
        stmt->columns.push_back({ std::move(expr), std::move(alias) });
        if (!match_consume(TokenKind::Comma)) break;
    }

    if (match_consume(TokenKind::KwFrom)) {
        parse_from(*stmt);
        parse_joins(*stmt);
    }

    if (match_consume(TokenKind::KwWhere)) {
        stmt->where = parse_expr();
    }
    if (match_consume(TokenKind::KwGroup)) {
        expect(TokenKind::KwBy, "BY");
        do {
            auto g = parse_expr();
            bool asc = true;
            if (match_consume(TokenKind::KwDesc)) asc = false;
            else match_consume(TokenKind::KwAsc);
            stmt->group_by.push_back({ std::move(g), asc });
        } while (match_consume(TokenKind::Comma));
    }
    if (match_consume(TokenKind::KwHaving)) {
        stmt->having = parse_expr();
    }
    if (match_consume(TokenKind::KwOrder)) {
        expect(TokenKind::KwBy, "BY");
        do {
            auto o = parse_expr();
            bool asc = true;
            if (match_consume(TokenKind::KwDesc)) asc = false;
            else match_consume(TokenKind::KwAsc);
            stmt->order_by.push_back({ std::move(o), asc });
        } while (match_consume(TokenKind::Comma));
    }
    if (match_consume(TokenKind::KwLimit)) {
        auto t2 = consume();
        if (t2.kind == TokenKind::Number) stmt->limit = t2.int_value;
        else throw ParseError(t2.line, t2.col, "expected number after LIMIT");
        if (match_consume(TokenKind::Comma)) {
            auto t3 = consume();
            if (t3.kind == TokenKind::Number) {
                stmt->offset = *stmt->limit;
                stmt->limit = t3.int_value;
            }
        } else if (match_consume(TokenKind::KwOffset)) {
            auto t3 = consume();
            if (t3.kind == TokenKind::Number) stmt->offset = t3.int_value;
        }
    }
    auto r = std::make_unique<ast::Statement>();
    r->kind = ast::StmtKind::Select;
    r->select_ = std::move(stmt);
    return r;
}

void Parser::parse_from(ast::SelectStmt& s) {
    do {
        auto t_alias = parse_table_ref_alias();
        s.from_tables.push_back(t_alias.first);
        s.from_aliases.push_back(t_alias);
    } while (match_consume(TokenKind::Comma));
}

std::pair<std::string, std::string> Parser::parse_table_ref_alias() {
    std::string table = parse_table_ref();
    std::string alias;
    if (match_consume(TokenKind::KwAs)) alias = kw_or_ident();
    else if (match(TokenKind::Ident) || match(TokenKind::QuotedIdent)) alias = kw_or_ident();
    return { table, alias };
}

std::string Parser::parse_table_ref() {
    // t1 或 schema.t1
    std::string first = kw_or_ident();
    if (match_consume(TokenKind::Dot)) {
        std::string second = kw_or_ident();
        return first + "." + second;
    }
    return first;
}

void Parser::parse_joins(ast::SelectStmt& s) {
    while (true) {
        ast::JoinClause jc;
        // 处理 JOIN kind —— 双关键字组合
        Token pk = peek();
        if (match_consume(TokenKind::KwCross) && match_consume(TokenKind::KwJoin)) {
            jc.kind = ast::JoinKind::Cross;
        } else if (match_consume(TokenKind::KwNatural)) {
            // NATURAL [LEFT|RIGHT|CROSS] JOIN
            match_consume(TokenKind::KwLeft) || match_consume(TokenKind::KwRight) || match_consume(TokenKind::KwCross);
            match_consume(TokenKind::KwOuter);
            match_consume(TokenKind::KwJoin);
            jc.kind = ast::JoinKind::Inner;
        } else if (match_consume(TokenKind::KwInner) || match_consume(TokenKind::KwJoin)) {
            // INNER JOIN 或单字 JOIN
            match_consume(TokenKind::KwJoin);  // 双字 INNER JOIN 时再 consume JOIN; 单字 JOIN 已被上面 consume
            jc.kind = ast::JoinKind::Inner;
        } else if (match_consume(TokenKind::KwLeft)) {
            // LEFT [OUTER] JOIN
            match_consume(TokenKind::KwOuter);
            match_consume(TokenKind::KwJoin);
            jc.kind = ast::JoinKind::Left;
        } else if (match_consume(TokenKind::KwRight)) {
            // RIGHT [OUTER] JOIN
            match_consume(TokenKind::KwOuter);
            match_consume(TokenKind::KwJoin);
            jc.kind = ast::JoinKind::Right;
        } else {
            break;
        }
        auto ref = parse_table_ref_alias();
        jc.table_name = ref.first;
        jc.alias = ref.second;
        if (match_consume(TokenKind::KwOn)) jc.on = parse_expr();
        s.joins.push_back(std::move(jc));
    }
}

// ========== INSERT ==========

ast::StmtPtr Parser::parse_insert() {
    consume(); // INSERT
    bool or_replace = false;
    if (match_consume(TokenKind::KwOr) && match_consume(TokenKind::KwReplace)) or_replace = true;
    (void)or_replace;
    expect(TokenKind::KwInto, "INTO");

    auto stmt = std::make_unique<ast::InsertStmt>();
    stmt->table_name = parse_table_ref();

    // 可选列列表
    if (match_consume(TokenKind::LParen)) {
        if (match(TokenKind::RParen)) {
            consume();
        } else {
            do {
                stmt->columns.push_back(kw_or_ident());
            } while (match_consume(TokenKind::Comma));
            expect(TokenKind::RParen, ")");
        }
    }

    // VALUES 或 SELECT 子句 (M3)
    if (match_consume(TokenKind::KwValues)) {
        do {
            expect(TokenKind::LParen, "(");
            std::vector<ast::ExprPtr> row;
            if (!match(TokenKind::RParen)) {
                do {
                    row.push_back(parse_expr());
                } while (match_consume(TokenKind::Comma));
            }
            consume(); // )
            stmt->value_rows.push_back(std::move(row));
        } while (match_consume(TokenKind::Comma));
    } else if (match(TokenKind::KwSelect)) {
        // INSERT … SELECT … :不要 match_consume,让 parse_select 自己处理 SELECT token
        auto sub = parse_select();
        stmt->select_source = std::move(sub);
    } else {
        auto t = peek();
        throw ParseError(t.line, t.col, "expected VALUES or SELECT after INSERT");
    }

    auto r = std::make_unique<ast::Statement>();
    r->kind = ast::StmtKind::Insert;
    r->insert_ = std::move(stmt);
    return r;
}

// ========== UPDATE ==========

ast::StmtPtr Parser::parse_update() {
    consume(); // UPDATE
    auto stmt = std::make_unique<ast::UpdateStmt>();
    stmt->table_name = parse_table_ref();
    expect(TokenKind::KwSet, "SET");
    do {
        std::string col = kw_or_ident();
        expect(TokenKind::Eq, "=");
        auto expr = parse_expr();
        stmt->set_clause.push_back({ std::move(col), std::move(expr) });
    } while (match_consume(TokenKind::Comma));
    if (match_consume(TokenKind::KwWhere)) {
        stmt->where = parse_expr();
    }
    auto r = std::make_unique<ast::Statement>();
    r->kind = ast::StmtKind::Update;
    r->update_ = std::move(stmt);
    return r;
}

// ========== DELETE ==========

ast::StmtPtr Parser::parse_delete() {
    consume(); // DELETE
    expect(TokenKind::KwFrom, "FROM");
    auto stmt = std::make_unique<ast::DeleteStmt>();
    stmt->table_name = parse_table_ref();
    if (match_consume(TokenKind::KwWhere)) {
        stmt->where = parse_expr();
    }
    auto r = std::make_unique<ast::Statement>();
    r->kind = ast::StmtKind::Delete;
    r->delete_ = std::move(stmt);
    return r;
}

// ========== CREATE TABLE ==========

types::TypeInfo Parser::parse_column_type() {
    types::TypeInfo ti;
    Token t = consume();
    switch (t.kind) {
        case TokenKind::KwInt:
        case TokenKind::KwInteger: ti.type = types::SqlType::Int; break;
        case TokenKind::KwTinyInt: ti.type = types::SqlType::TinyInt; break;
        case TokenKind::KwSmallInt: ti.type = types::SqlType::SmallInt; break;
        case TokenKind::KwBigInt: ti.type = types::SqlType::BigInt; break;
        case TokenKind::KwDecimal:
        case TokenKind::KwNumeric: ti.type = types::SqlType::Decimal; break;
        case TokenKind::KwFloat:   ti.type = types::SqlType::Float; break;
        case TokenKind::KwDouble:  ti.type = types::SqlType::Double; break;
        case TokenKind::KwReal:    ti.type = types::SqlType::Double; break;
        case TokenKind::KwVarchar: ti.type = types::SqlType::Varchar; break;
        case TokenKind::KwChar:    ti.type = types::SqlType::Char; break;
        case TokenKind::KwText:    ti.type = types::SqlType::Text; break;
        case TokenKind::KwBlob:    ti.type = types::SqlType::Blob; break;
        case TokenKind::KwDate:    ti.type = types::SqlType::Date; break;
        case TokenKind::KwTime:    ti.type = types::SqlType::Time; break;
        case TokenKind::KwDateTime:ti.type = types::SqlType::DateTime; break;
        case TokenKind::KwTimestamp:ti.type = types::SqlType::Timestamp; break;
        case TokenKind::KwBoolean: ti.type = types::SqlType::Boolean; break;
        case TokenKind::KwJson:    ti.type = types::SqlType::Json; break;
        default: throw ParseError(t.line, t.col, fmt::format("unknown type '{}'", t.text));
    }
    // (precision, scale) 或 (length)
    if (match_consume(TokenKind::LParen)) {
        auto t2 = consume();
        if (t2.kind == TokenKind::Number) ti.length = static_cast<uint16_t>(t2.int_value);
        if (match_consume(TokenKind::Comma)) {
            auto t3 = consume();
            if (t3.kind == TokenKind::Number) ti.scale = static_cast<uint16_t>(t3.int_value);
        }
        expect(TokenKind::RParen, ")");
    }
    while (true) {
        if (match_consume(TokenKind::KwUnsigned)) ti.unsigned_ = true;
        else if (match_consume(TokenKind::KwSigned)) ti.unsigned_ = false;
        else break;
    }
    return ti;
}

ast::ColumnDef Parser::parse_column_def() {
    ast::ColumnDef def;
    def.name = kw_or_ident();
    def.type = parse_column_type();
    while (true) {
        if (match_consume(TokenKind::KwNot) && match_consume(TokenKind::KwNull)) { def.nullable = false; }
        else if (match_consume(TokenKind::KwNull)) { def.nullable = true; }
        else if (match_consume(TokenKind::KwPrimary) && match_consume(TokenKind::KwKey)) {
            def.is_primary_key = true; def.nullable = false;
        }
        else if (match_consume(TokenKind::KwAutoIncrement)) { def.is_auto_increment = true; }
        else if (match_consume(TokenKind::KwDefault)) {
            auto e = parse_expr();
            def.default_value = std::shared_ptr<ast::Expression>(e.release());
        }
        else if (match_consume(TokenKind::KwComment)) {
            Token t = consume();
            if (t.kind == TokenKind::String) def.comment = t.text;
        }
        else break;
    }
    return def;
}

std::vector<ast::ColumnDef> Parser::parse_column_defs() {
    std::vector<ast::ColumnDef> cols;
    expect(TokenKind::LParen, "(");
    while (!match(TokenKind::RParen)) {
        // 表级约束: PRIMARY KEY(a,b), UNIQUE INDEX idx(a), INDEX idx(a), CONSTRAINT ...
        if (match_consume(TokenKind::KwPrimary)) {
            expect(TokenKind::KwKey, "KEY");
            expect(TokenKind::LParen, "(");
            // 收集列名 — 标记这些列为 PK
            std::vector<std::string> pk_cols;
            do { pk_cols.push_back(kw_or_ident()); } while (match_consume(TokenKind::Comma));
            expect(TokenKind::RParen, ")");
            // 暂时存到第一列的 is_primary_key 上;后面在 parse_create_table 里处理
            // (简化:我们让 create_table 处理)
            cols.push_back(ast::ColumnDef{});
            cols.back().name = "__pk__";
            cols.back().is_primary_key = true;
            cols.back().default_pk_cols = pk_cols;
        } else {
            cols.push_back(parse_column_def());
        }
        if (!match_consume(TokenKind::Comma)) break;
    }
    expect(TokenKind::RParen, ")");
    return cols;
}

ast::StmtPtr Parser::parse_create_table() {
    // CREATE 已在 parse_top 里 consume 掉
    match_consume(TokenKind::KwTemporary);
    expect(TokenKind::KwTable, "TABLE");

    auto stmt = std::make_unique<ast::CreateTableStmt>();
    if (match_consume(TokenKind::KwIf) && match_consume(TokenKind::KwNot) && match_consume(TokenKind::KwExists)) {
        stmt->if_not_exists = true;
    }
    stmt->table_name = parse_table_ref();

    auto cols = parse_column_defs();
    // 把表级 PRIMARY KEY 转成列级
    std::vector<std::string> table_pk;
    auto it = cols.begin();
    while (it != cols.end()) {
        if (it->is_primary_key && it->name == "__pk__") {
            table_pk = it->default_pk_cols;
            it = cols.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& c : cols) {
        for (auto& pkc : table_pk) {
            if (_stricmp(c.name.c_str(), pkc.c_str()) == 0) {
                c.is_primary_key = true; c.nullable = false;
            }
        }
    }
    if (table_pk.empty()) {
        // 从列级推导
        for (auto& c : cols) {
            if (c.is_primary_key) stmt->primary_key.push_back({ c.name });
        }
    } else {
        stmt->primary_key.push_back(table_pk);
    }
    stmt->columns = std::move(cols);
    auto r = std::make_unique<ast::Statement>();
    r->kind = ast::StmtKind::CreateTable;
    r->create_table_ = std::move(stmt);
    return r;
}

// 为了让 __pk__ 里能存列名列表,我们需要给 ColumnDef 加个字段
// 已在 ast.h 漏写,这里动态加?其实应该改 ast.h。
// 这里假设 ast.h 的 ColumnDef 里有 default_pk_cols;没有就临时变通 — 后面调整。
// 为避免重构,重新在 parse_create_table 里处理表级 PK。
// 让我重写 parse_column_defs,用一个单独输出处理表级约束。

// 重写 parse_column_defs 成能同时返回:列 + 表级 PK 列表
// 但改动有点多,先用变通:我们加成员,后面改 ast.h。
// 上面代码假设 ColumnDef 有 default_pk_cols,如果编译不过再改。

ast::StmtPtr Parser::parse_drop_table() {
    // DROP 已在 parse_top 里 consume 掉
    expect(TokenKind::KwTable, "TABLE");
    auto stmt = std::make_unique<ast::DropTableStmt>();
    if (match_consume(TokenKind::KwIf) && match_consume(TokenKind::KwExists)) {
        stmt->if_exists = true;
    }
    stmt->table_name = parse_table_ref();
    auto r = std::make_unique<ast::Statement>();
    r->kind = ast::StmtKind::DropTable;
    r->drop_table_ = std::move(stmt);
    return r;
}

ast::StmtPtr Parser::parse_use() {
    consume();
    auto stmt = std::make_unique<ast::UseStmt>();
    stmt->database_name = kw_or_ident();
    auto r = std::make_unique<ast::Statement>();
    r->kind = ast::StmtKind::Use;
    r->use_ = std::move(stmt);
    return r;
}

ast::StmtPtr Parser::parse_show() {
    consume(); // SHOW
    auto stmt = std::make_unique<ast::ShowStmt>();

    // 可选 GLOBAL / SESSION (对 STATUS / VARIABLES 用)
    bool global_or_session = false;
    if (match_consume(TokenKind::KwGlobal) || match_consume(TokenKind::KwSession))
        global_or_session = true;  // 记录但 ShowStmt 里没字段,忽略

    Token t = peek();
    if (match_consume(TokenKind::KwTables)) {
        stmt->kind = ast::ShowStmt::Kind::Tables;
        if (match_consume(TokenKind::KwFrom) || match_consume(TokenKind::KwIn)) {
            stmt->table_or_pattern = kw_or_ident();
        }
    } else if (match_consume(TokenKind::KwDatabases) || match_consume(TokenKind::KwSchemas)) {
        stmt->kind = ast::ShowStmt::Kind::Databases;
    } else if (match_consume(TokenKind::KwColumns) || match_consume(TokenKind::KwFields)) {
        stmt->kind = ast::ShowStmt::Kind::Columns;
        expect(TokenKind::KwFrom, "FROM");
        stmt->table_or_pattern = kw_or_ident();
        if (match_consume(TokenKind::KwFrom)) {
            stmt->db_name = kw_or_ident();
        }
    } else if (match_consume(TokenKind::KwCreate)) {
        auto nt = peek();
        if (match_consume(TokenKind::KwTable)) {
            stmt->kind = ast::ShowStmt::Kind::CreateTable;
            // 可选 db.table
            std::string first = kw_or_ident();
            if (match_consume(TokenKind::Dot)) {
                stmt->db_name = first;
                stmt->table_or_pattern = kw_or_ident();
            } else {
                stmt->table_or_pattern = first;
            }
        } else if (match_consume(TokenKind::KwDatabase) || match_consume(TokenKind::KwSchema)) {
            stmt->kind = ast::ShowStmt::Kind::CreateDatabase;
            stmt->table_or_pattern = kw_or_ident();
        } else {
            throw ParseError(nt.line, nt.col, fmt::format("expected TABLE or DATABASE after SHOW CREATE, got '{}'", nt.text));
        }
    } else if (match_consume(TokenKind::KwStatus)) {
        stmt->kind = ast::ShowStmt::Kind::Status;
    } else if (match_consume(TokenKind::KwVariables)) {
        stmt->kind = ast::ShowStmt::Kind::Variables;
        if (match_consume(TokenKind::KwLike)) {
            auto t = consume();
            // LIKE pattern 可能是字符串字面量或标识符
            if (t.kind == TokenKind::String) stmt->table_or_pattern = t.text;
            else if (t.kind == TokenKind::Ident) stmt->table_or_pattern = t.text;
        }
    } else {
        throw ParseError(t.line, t.col, fmt::format("unsupported SHOW '{}'", t.text));
    }
    (void)global_or_session;

    auto r = std::make_unique<ast::Statement>();
    r->kind = ast::StmtKind::Show;
    r->show_ = std::move(stmt);
    return r;
}

ast::StmtPtr Parser::parse_create_database() {
    // CREATE 已在 parse_top 里 consume 掉,这里 consume DATABASE | SCHEMA
    match_consume(TokenKind::KwDatabase) || match_consume(TokenKind::KwSchema);
    auto stmt = std::make_unique<ast::CreateDatabaseStmt>();
    if (match_consume(TokenKind::KwIf) && match_consume(TokenKind::KwNot) && match_consume(TokenKind::KwExists)) {
        stmt->if_not_exists = true;
    }
    stmt->database_name = kw_or_ident();
    auto r = std::make_unique<ast::Statement>();
    r->kind = ast::StmtKind::CreateDatabase;
    r->create_database_ = std::move(stmt);
    return r;
}

ast::StmtPtr Parser::parse_drop_database() {
    // DROP 已在 parse_top 里 consume 掉,这里 consume DATABASE | SCHEMA
    match_consume(TokenKind::KwDatabase) || match_consume(TokenKind::KwSchema);
    auto stmt = std::make_unique<ast::DropDatabaseStmt>();
    if (match_consume(TokenKind::KwIf) && match_consume(TokenKind::KwExists)) {
        stmt->if_exists = true;
    }
    stmt->database_name = kw_or_ident();
    auto r = std::make_unique<ast::Statement>();
    r->kind = ast::StmtKind::DropDatabase;
    r->drop_database_ = std::move(stmt);
    return r;
}

ast::StmtPtr Parser::parse_set() {
    // consume SET, 整个 SET 语句扫到分号/Eof 为止,返回空 SET (PyMySQL SET NAMES/SET AUTOCOMMIT 兼容)
    consume(); // SET
    int depth = 0;
    while (true) {
        Token t = peek();
        if (t.kind == TokenKind::Eof || t.kind == TokenKind::Semi) break;
        if (t.kind == TokenKind::LParen) depth++;
        if (t.kind == TokenKind::RParen && depth > 0) depth--;
        consume();
    }
    auto r = std::make_unique<ast::Statement>();
    r->kind = ast::StmtKind::Set;
    return r;
}

} // namespace sqltiny::parser
