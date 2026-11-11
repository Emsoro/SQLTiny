#pragma once

#include "parser/lexer.h"
#include "parser/ast.h"
#include <string>
#include <vector>
#include <stdexcept>
#include <fmt/format.h>

namespace sqltiny::parser {

/// 解析异常
class ParseError : public std::runtime_error {
public:
    ParseError(int line, int col, std::string msg)
        : std::runtime_error(fmt::format("[parse] {} at line {} col {}", msg, line, col)),
          line_(line), col_(col) {}
    int line() const { return line_; }
    int col()  const { return col_; }
private:
    int line_, col_;
};

/// SQL Parser — 手写递归下降
/// 负责:SELECT / INSERT / UPDATE / DELETE / CREATE TABLE / DROP TABLE / USE / SHOW
class Parser {
public:
    explicit Parser(std::string_view sql);

    /// 解析单条语句(跳到 Eof 或第一个分号)
    ast::StmtPtr parse_stmt();

    /// 解析所有语句(支持用分号分隔多条)
    std::vector<ast::StmtPtr> parse_all();

    /// 入口:根据第一个关键字分发
    ast::StmtPtr parse_top();

private:
    Lexer lx_;
    Token peeked_;
    bool has_peeked_ = false;

    Token peek();
    Token consume();
    Token expect(TokenKind k, std::string_view what);
    bool match(TokenKind k);
    bool match_consume(TokenKind k);
    void skip_semicolon();
    std::string ident_text();
    std::string kw_or_ident();  // 关键字但在标识符位置接受(如 t.name 中 t 是 Ident 还是 KwTable)

    // 顶层分发
    ast::StmtPtr parse_select();
    ast::StmtPtr parse_insert();
    ast::StmtPtr parse_update();
    ast::StmtPtr parse_delete();
    ast::StmtPtr parse_create_table();
    ast::StmtPtr parse_drop_table();
    ast::StmtPtr parse_create_database();
    ast::StmtPtr parse_drop_database();
    ast::StmtPtr parse_use();
    ast::StmtPtr parse_show();
    ast::StmtPtr parse_set();   // 占位:兼容 PyMySQL SET NAMES/SET AUTOCOMMIT

    // 辅助
    std::vector<ast::ColumnDef> parse_column_defs();
    ast::ColumnDef parse_column_def();
    types::TypeInfo parse_column_type();
    std::string parse_table_ref();   // table [alias]
    std::pair<std::string, std::string> parse_table_ref_alias();  // t a / t AS a

    // SELECT 子句
    void parse_select_core(ast::SelectStmt& s);
    void parse_from(ast::SelectStmt& s);
    void parse_joins(ast::SelectStmt& s);

    // 表达式层(递归下降)
    ast::ExprPtr parse_expr();        // entry
    ast::ExprPtr parse_or_expr();
    ast::ExprPtr parse_and_expr();
    ast::ExprPtr parse_not_expr();
    ast::ExprPtr parse_comparison();
    ast::ExprPtr parse_additive();
    ast::ExprPtr parse_multiplicative();
    ast::ExprPtr parse_unary();
    ast::ExprPtr parse_primary();

    ast::ExprPtr parse_function_call(std::string name, int line, int col);

    ast::BinOp token_to_binop(TokenKind k);

    int line_ = 1, col_ = 1;
};

} // namespace sqltiny::parser
