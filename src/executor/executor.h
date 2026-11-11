#pragma once

#include "parser/ast.h"
#include "types/value.h"
#include "storage/table.h"
#include "catalog/catalog.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

// M6 前向声明: Session 在 sqltiny::core 里, 我们只需要指针
namespace sqltiny { namespace core { class Session; } }

namespace sqltiny::executor {

// 前向声明 — ExecContext 里要持有 Executor* 用于子查询递归求值
class Executor;

// 表达式层 free function(跨 cpp 可见,定义在 expr.cpp)
bool to_bool(const types::Value& v);

/// 执行上下文:提供行上下文 + 列查找 + 表查找
struct ExecContext {
    catalog::Catalog& catalog;
    std::string primary_table;
    std::vector<std::pair<std::string, std::string>> tables;
    storage::Row current_row;
    std::map<std::string, int> col_index;

    // 子查询求值时递归调 exec_select 需要 executor 指针
    Executor* exec = nullptr;

    // 聚合状态(SELECT 执行时临时填充)
    struct AggState {
        std::string func;       // COUNT, SUM, AVG, MIN, MAX
        std::string arg_col;    // 或 "*"
        double sum = 0; int64_t count = 0; int64_t int_sum = 0;
        double min_d = 0, max_d = 0; bool min_set = false, max_set = false;
        bool has_star = false;
    };
    std::vector<AggState> aggs;

    // MySQL 协议用:last_insert_id
    int64_t last_insert_id = 0;
    // 影响行数
    int64_t affected_rows = 0;
    // 警告数
    uint16_t warnings = 0;
};

/// 执行结果集
struct ResultSet {
    std::vector<std::string> column_names;     // 显示名(别名或列名)
    std::vector<types::SqlType> column_types;   // 对应每列的 SQL 类型(供 wire 协议 field_type)
    std::vector<storage::Row> rows;            // 每行按 column_names 对齐
    int64_t affected_rows = 0;
    int64_t last_insert_id = 0;
    bool    ok = true;
    std::string error_message;
    std::string sql_state = "00000";
    uint16_t    mysql_errno = 0;
    uint16_t    warnings = 0;
};

/// 执行器
class Executor {
public:
    Executor(catalog::Catalog& cat) : catalog_(cat) {}

    /// 执行单条语句
    ResultSet execute(const ast::Statement& stmt);

    /// 暴露 catalog 引用供外部(Session USE 处理)同步 current_database
    catalog::Catalog& catalog_ref() { return catalog_; }
    const catalog::Catalog& catalog_ref() const { return catalog_; }

    // M6: Session 可选关联 — 用于事务内拿锁 + 记录 RowAction
    void bind_session(core::Session* s) { session_ = s; }
    core::Session* bound_session() const { return session_; }

private:
    catalog::Catalog& catalog_;
    core::Session* session_ = nullptr;

    ResultSet exec_create_table(const ast::CreateTableStmt& s);
    ResultSet exec_drop_table(const ast::DropTableStmt& s);
    ResultSet exec_insert(const ast::InsertStmt& s);
    ResultSet exec_update(const ast::UpdateStmt& s);
    ResultSet exec_delete(const ast::DeleteStmt& s);
    ResultSet exec_select(const ast::SelectStmt& s);
    ResultSet exec_show_tables();
    ResultSet exec_show_columns(const std::string& table_name);
    ResultSet exec_show_databases();
    ResultSet exec_show_create_table(const std::string& db_name, const std::string& table_name);
    ResultSet exec_show_create_database(const std::string& db_name);
    ResultSet exec_show_status();
    ResultSet exec_show_variables(const std::string& pattern = "");

    ResultSet exec_create_database(const ast::CreateDatabaseStmt& s);
    ResultSet exec_drop_database(const ast::DropDatabaseStmt& s);

    // 表达式求值
    types::Value eval(const ast::Expression& e, const ExecContext& ctx) const;
    types::Value eval_like(const types::Value& v, const types::Value& pattern) const;
    types::Value eval_function(const std::string& name, const std::vector<ast::ExprPtr>& args,
                               bool star, const ExecContext& ctx) const;
};

} // namespace sqltiny::executor
