#pragma once

#include "parser/lexer.h"
#include "types/value.h"
#include <memory>
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <utility>

namespace sqltiny::ast {

// ---------- 前向 ----------
struct Expression;
using ExprPtr = std::unique_ptr<Expression>;

struct Statement;
using StmtPtr = std::unique_ptr<Statement>;

// ---------- 列定义 ----------
/// CREATE TABLE t(id BIGINT PRIMARY KEY, name VARCHAR(64))
struct ColumnDef {
    std::string name;
    types::TypeInfo type;
    bool nullable = true;
    bool is_primary_key = false;
    bool is_auto_increment = false;
    std::shared_ptr<Expression> default_value;   // 可选,当前放 LiteralExpr; nullptr 即无默认
    std::string comment;
    // 用于表级 PRIMARY KEY(a,b) 临时传递,后面清空
    std::vector<std::string> default_pk_cols;
};

// ---------- 表达式 ----------
enum class ExprKind {
    Literal,        // 字面量 Value
    Column,         // 列引用,可选带表名 t.col
    BinaryOp,       // 二元运算
    UnaryOp,        // 一元运算(-, NOT, IS NULL)
    FunctionCall,   // COUNT(*), CONCAT(a,b), COALESCE(a,b) 等
    Star,           // * 或 t.*
    Subquery,       // 标量子查询: SELECT expr FROM t WHERE ...  作为一个表达式
    Placeholder,    // ?
};

/// 二元运算符
enum class BinOp {
    Plus, Minus, Star, Slash, Percent,   // 算术
    Eq, Ne, Lt, Gt, Le, Ge,               // 比较
    And, Or,                              // 逻辑
    Concat,                               // MySQL CONCAT / SQL ||
};

struct Expression {
    ExprKind kind;
    // --- Literal ---
    types::Value literal;
    // --- Column ---
    std::string table_name;      // 可选限定
    std::string column_name;
    // --- Star ---
    bool table_star = false;     // true 时 table_name 有效
    // --- BinaryOp ---
    BinOp binop;
    ExprPtr lhs, rhs;
    // --- UnaryOp ---
    enum class UnaryKind { Negate, Not, IsNull, IsNotNull };
    UnaryKind unary;
    ExprPtr operand;
    // --- FunctionCall ---
    std::string func_name;
    std::vector<ExprPtr> func_args;
    bool func_star_arg = false;  // COUNT(*)
    // --- Subquery ---
    StmtPtr subquery_stmt;       // Subquery 用:持有完整 SELECT Statement

    // 便捷构造
    static ExprPtr make_literal(types::Value v);
    static ExprPtr make_column(std::string col);
    static ExprPtr make_column(std::string table, std::string col);
    static ExprPtr make_binary(BinOp op, ExprPtr l, ExprPtr r);
    static ExprPtr make_star(std::string table = "");
    static ExprPtr make_function(std::string name, std::vector<ExprPtr> args, bool star = false);
    static ExprPtr make_subquery(StmtPtr stmt);
};

// ---------- JOIN ----------
enum class JoinKind { Inner, Left, Right, Full, Cross };
struct JoinClause {
    JoinKind kind = JoinKind::Inner;
    std::string table_name;
    std::string alias;           // 可选
    ExprPtr on;                  // ON 条件, CROSS / NATURAL 时可空
};

// ---------- SELECT ----------
enum class SelectSetOp { None, Union, UnionAll };

struct SelectStmt {
    // SELECT 体
    bool distinct = false;
    std::vector<std::pair<ExprPtr, std::string>> columns;  // (expr, alias)
    std::vector<std::string> from_tables;                  // FROM t1, t2
    std::vector<std::pair<std::string, std::string>> from_aliases;  // (table, alias) — 简单形式,后面可能再拆
    std::vector<JoinClause> joins;
    ExprPtr where;
    std::vector<std::pair<ExprPtr, bool>> group_by;  // (expr, asc)
    ExprPtr having;
    std::vector<std::pair<ExprPtr, bool>> order_by;
    std::optional<int64_t> limit;
    std::optional<int64_t> offset;

    // UNION
    SelectSetOp set_op = SelectSetOp::None;
    StmtPtr set_right;   // 右边也可以是 SelectStmt
};

// ---------- INSERT ----------
struct InsertStmt {
    std::string table_name;
    std::vector<std::string> columns;   // 可空,空表示全列
    std::vector<std::vector<ExprPtr>> value_rows;  // VALUES 子句
    StmtPtr select_source;                          // INSERT … SELECT … (M3)
};

// ---------- UPDATE ----------
struct UpdateStmt {
    std::string table_name;
    std::vector<std::pair<std::string, ExprPtr>> set_clause;  // (col, expr)
    ExprPtr where;
};

// ---------- DELETE ----------
struct DeleteStmt {
    std::string table_name;
    ExprPtr where;
};

// ---------- DDL ----------
struct CreateTableStmt {
    bool if_not_exists = false;
    std::string table_name;
    std::vector<ColumnDef> columns;
    std::vector<std::vector<std::string>> primary_key;  // 列名列表
    struct IdxDef { std::string name; std::vector<std::string> columns; bool unique; };
    std::vector<IdxDef> indexes;
};

struct DropTableStmt {
    bool if_exists = false;
    std::string table_name;
};

// ---------- USE ----------
struct UseStmt {
    std::string database_name;
};

// ---------- SHOW ----------
struct ShowStmt {
    enum class Kind {
        Tables,
        Columns,
        Databases,
        CreateTable,       // SHOW CREATE TABLE [db.]tbl
        CreateDatabase,    // SHOW CREATE DATABASE db
        Status,            // SHOW [GLOBAL|SESSION] STATUS
        Variables,         // SHOW [GLOBAL|SESSION] VARIABLES
    };
    Kind kind;
    std::string table_or_pattern;   // Tables:pattern / Columns:table / CreateTable:tbl / CreateDatabase:db
    std::string db_name;            // Columns / CreateTable 里可选限定
};

// ---------- CREATE DATABASE ----------
struct CreateDatabaseStmt {
    std::string database_name;
    bool if_not_exists = false;
};

// ---------- DROP DATABASE ----------
struct DropDatabaseStmt {
    std::string database_name;
    bool if_exists = false;
};

// ---------- 统一 Statement variant ----------
enum class StmtKind {
    Select, Insert, Update, Delete,
    CreateTable, DropTable,
    CreateDatabase, DropDatabase,
    Use, Show, Set,
    Begin, Commit, Rollback,       // M6 事务
};

struct Statement {
    StmtKind kind;

    // 各具体 AST
    std::unique_ptr<SelectStmt>      select_;
    std::unique_ptr<InsertStmt>      insert_;
    std::unique_ptr<UpdateStmt>      update_;
    std::unique_ptr<DeleteStmt>      delete_;
    std::unique_ptr<CreateTableStmt> create_table_;
    std::unique_ptr<DropTableStmt>   drop_table_;
    std::unique_ptr<CreateDatabaseStmt> create_database_;
    std::unique_ptr<DropDatabaseStmt>   drop_database_;
    std::unique_ptr<UseStmt>         use_;
    std::unique_ptr<ShowStmt>        show_;

    std::string raw;

    SelectStmt*      as_select()       { return select_.get(); }
    InsertStmt*      as_insert()       { return insert_.get(); }
    UpdateStmt*      as_update()       { return update_.get(); }
    DeleteStmt*      as_delete()       { return delete_.get(); }
    CreateTableStmt* as_create_table() { return create_table_.get(); }
    DropTableStmt*   as_drop_table()   { return drop_table_.get(); }
};

} // namespace sqltiny::ast
