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
    Case,           // CASE [base] WHEN c THEN r ... [ELSE e] END
};

/// 二元运算符
enum class BinOp {
    Plus, Minus, Star, Slash, Percent,   // 算术
    Eq, Ne, Lt, Gt, Le, Ge,               // 比较
    NullSafeEq,                           // <=> (NULL 安全等值)
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
    bool func_distinct = false;  // COUNT(DISTINCT x) / SUM(DISTINCT x) / AVG(DISTINCT x)
    // --- 窗口函数:func OVER (PARTITION BY ... ORDER BY ...) ---
    bool is_window = false;
    std::vector<ExprPtr> win_partition;
    std::vector<std::pair<ExprPtr, bool>> win_order;   // (表达式, 是否升序)
    // --- Subquery ---
    StmtPtr subquery_stmt;       // Subquery 用:持有完整 SELECT Statement
    bool subquery_exists = false; // EXISTS (SELECT ...):只判存在性

    // --- Case ---
    ExprPtr case_base;                                     // 简单 CASE 的比较对象,可空(搜索 CASE)
    std::vector<std::pair<ExprPtr, ExprPtr>> case_whens;    // (搜索条件/比较值, 结果)
    ExprPtr case_else;

    // 便捷构造
    static ExprPtr make_literal(types::Value v);
    static ExprPtr make_column(std::string col);
    static ExprPtr make_column(std::string table, std::string col);
    static ExprPtr make_binary(BinOp op, ExprPtr l, ExprPtr r);
    static ExprPtr make_star(std::string table = "");
    static ExprPtr make_function(std::string name, std::vector<ExprPtr> args, bool star = false);
    static ExprPtr make_subquery(StmtPtr stmt);

    /// 深拷贝(BETWEEN 展开等需要复用同一子表达式两处)
    ExprPtr clone() const;
};

// ---------- JOIN ----------
enum class JoinKind { Inner, Left, Right, Full, Cross };
struct JoinClause {
    JoinKind kind = JoinKind::Inner;
    std::string table_name;
    std::string alias;           // 可选
    ExprPtr on;                  // ON 条件, CROSS / NATURAL 时可空
    StmtPtr derived;             // JOIN (SELECT ...) alias:派生表,非空时 table_name 是别名
};

// ---------- SELECT ----------
enum class SelectSetOp { None, Union, UnionAll };

struct SelectStmt {
    // WITH 子句(CTE):按声明顺序物化,后面的可引用前面的
    struct CteDef {
        std::string name;
        std::vector<std::string> column_names;  // 可选显式列名 seq(n)
        StmtPtr query;
        bool recursive = false;
    };
    std::vector<CteDef> ctes;

    // SELECT 体
    bool distinct = false;
    bool explain = false;        // EXPLAIN SELECT ... : 只输出执行计划,不执行查询
    std::vector<std::pair<ExprPtr, std::string>> columns;  // (expr, alias)
    std::vector<std::string> from_tables;                  // FROM t1, t2
    std::vector<std::pair<std::string, std::string>> from_aliases;  // (table, alias) — 简单形式,后面可能再拆
    std::vector<StmtPtr> from_derived;   // 与 from_tables 平行:FROM (SELECT ...) 派生表,普通表为 nullptr
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
    // P1-b: INSERT 方言
    bool ignore = false;                            // INSERT IGNORE
    bool replace = false;                           // REPLACE INTO (冲突时删旧插新)
    std::vector<std::pair<std::string, ExprPtr>> on_dup_update;  // ON DUPLICATE KEY UPDATE
};

// ---------- UPDATE ----------
struct UpdateStmt {
    std::string table_name;
    std::string alias;                              // 表别名
    std::vector<JoinClause> joins;                  // 多表 UPDATE (P1-b)
    std::vector<std::pair<std::string, ExprPtr>> set_clause;  // (col 可带限定, expr)
    ExprPtr where;
    std::vector<std::pair<ExprPtr, bool>> order_by; // ORDER BY (P1-b)
    std::optional<int64_t> limit;                   // LIMIT (P1-b)
};

// ---------- DELETE ----------
struct DeleteStmt {
    std::string table_name;
    std::string alias;                              // 表别名
    ExprPtr where;
    std::vector<std::pair<ExprPtr, bool>> order_by; // ORDER BY (P1-b)
    std::optional<int64_t> limit;                   // LIMIT (P1-b)
};

// ---------- DDL ----------
struct CreateTableStmt {
    bool if_not_exists = false;
    bool temporary = false;                         // CREATE TEMPORARY TABLE (P1-b,语义同普通表)
    std::string table_name;
    std::vector<ColumnDef> columns;
    std::vector<std::vector<std::string>> primary_key;  // 列名列表
    struct IdxDef { std::string name; std::vector<std::string> columns; bool unique; };
    std::vector<IdxDef> indexes;
    // P1-b: CREATE TABLE 变体
    std::optional<int64_t> auto_increment_start;    // AUTO_INCREMENT=n 表选项
    std::string like_table;                         // CREATE TABLE ... LIKE src
    StmtPtr select_source;                          // CREATE TABLE ... AS SELECT
};

struct TruncateStmt { std::string table_name; };

struct AlterTableStmt {
    enum class Action { AddColumn } action = Action::AddColumn;
    std::string table_name;
    ColumnDef column;                               // ADD COLUMN 的列定义
};

struct RenameTableStmt { std::string from_table, to_table; };

struct CreateIndexStmt {
    std::string index_name;
    std::string table_name;
    std::vector<std::string> columns;
    bool unique = false;
};

struct CreateViewStmt { std::string view_name; StmtPtr query; };

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
        Engines,           // SHOW ENGINES
        Plugins,           // SHOW PLUGINS
        Charset,           // SHOW CHARSET / SHOW CHARACTER SET
        Collation,         // SHOW COLLATION
        Index,             // SHOW INDEX FROM [db.]tbl / SHOW KEYS FROM tbl
        TableStatus,       // SHOW TABLE STATUS [FROM db] [LIKE 'pat']
        Triggers,          // SHOW TRIGGERS [FROM db] [WHERE ...] —— 返回空结果集
        ProcessList,       // SHOW PROCESSLIST
        Warnings,          // SHOW WARNINGS / SHOW ERRORS
    };
    Kind kind;
    bool full = false;              // SHOW FULL TABLES —— 多返回一列 table_type
    std::string table_or_pattern;   // Tables:db / Columns:table / CreateTable:tbl / CreateDatabase:db / Index:tbl / TableStatus:LIKE 模式
    std::string db_name;            // Columns / CreateTable / Index / TableStatus 里可选限定
    std::string filter_pattern;     // SHOW TABLES 的 LIKE 'pat' / WHERE col='pat' 表名过滤
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

// ---------- 事务 ----------
/// 隔离级别常量与 HSQLDB 2.7.4 SessionInterface.TX_* 对齐 (1/2/4/8)
enum class IsolationLevel : int {
    ReadUncommitted = 1,
    ReadCommitted   = 2,
    RepeatableRead  = 4,
    Serializable    = 8,
};

/// SET 语句中真正需要语义的部分 (其余一律兼容忽略)
struct SetStmt {
    enum class Kind { Ignored, AutoCommit, IsolationLevel };
    Kind kind = Kind::Ignored;
    bool autocommit_value = true;
    int  isolation_level  = 0;     // IsolationLevel 的整数值, 0 = 未指定
    bool session_default  = true;  // true=会话默认; false=只对下一个事务生效
};

// ---------- 统一 Statement variant ----------
enum class StmtKind {
    Select, Insert, Update, Delete,
    CreateTable, DropTable,
    CreateDatabase, DropDatabase,
    Use, Show, Set,
    Begin, Commit, Rollback,       // M6 事务
    StartTransaction,              // START TRANSACTION (BEGIN 别名, 可带隔离级别)
    Savepoint, RollbackToSavepoint, ReleaseSavepoint,
    // P1-b DDL
    TruncateTable, AlterTable, RenameTable, CreateIndex, CreateView,
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
    std::unique_ptr<SetStmt>         set_;
    std::unique_ptr<TruncateStmt>    truncate_;
    std::unique_ptr<AlterTableStmt>  alter_table_;
    std::unique_ptr<RenameTableStmt> rename_table_;
    std::unique_ptr<CreateIndexStmt> create_index_;
    std::unique_ptr<CreateViewStmt>  create_view_;

    std::string savepoint_name;
    std::string raw;

    SelectStmt*      as_select()       { return select_.get(); }
    InsertStmt*      as_insert()       { return insert_.get(); }
    UpdateStmt*      as_update()       { return update_.get(); }
    DeleteStmt*      as_delete()       { return delete_.get(); }
    CreateTableStmt* as_create_table() { return create_table_.get(); }
    DropTableStmt*   as_drop_table()   { return drop_table_.get(); }
};

/// 深拷贝语句(当前仅 SELECT 会被克隆:子查询/CTE/派生表);其它类型返回 nullptr
StmtPtr clone_stmt(const Statement& st);

} // namespace sqltiny::ast
