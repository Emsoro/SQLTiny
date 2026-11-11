#pragma once

#include "parser/ast.h"
#include "types/value.h"
#include "storage/table.h"
#include "catalog/catalog.h"
#include <string>
#include <vector>
#include <memory>
#include <map>

namespace sqltiny::executor {

// 表达式层 free function(跨 cpp 可见,定义在 expr.cpp)
bool to_bool(const types::Value& v);

/// 执行上下文:提供行上下文 + 列查找 + 表查找
struct ExecContext {
    catalog::Catalog& catalog;
    // 单表 SELECT: table_name → Row
    std::string primary_table;
    std::vector<std::pair<std::string, std::string>> tables;  // (real_name, alias)
    storage::Row current_row;  // 按列名查找
    std::map<std::string, int> col_index;  // alias.col / col → index in current_row

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

private:
    catalog::Catalog& catalog_;

    ResultSet exec_create_table(const ast::CreateTableStmt& s);
    ResultSet exec_drop_table(const ast::DropTableStmt& s);
    ResultSet exec_insert(const ast::InsertStmt& s);
    ResultSet exec_update(const ast::UpdateStmt& s);
    ResultSet exec_delete(const ast::DeleteStmt& s);
    ResultSet exec_select(const ast::SelectStmt& s);
    ResultSet exec_show_tables();
    ResultSet exec_show_columns(const std::string& table_name);

    // 表达式求值
    types::Value eval(const ast::Expression& e, const ExecContext& ctx) const;
    types::Value eval_like(const types::Value& v, const types::Value& pattern) const;
    types::Value eval_function(const std::string& name, const std::vector<ast::ExprPtr>& args,
                               bool star, const ExecContext& ctx) const;
};

} // namespace sqltiny::executor
