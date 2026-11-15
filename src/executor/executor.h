#pragma once

#include "parser/ast.h"
#include "types/value.h"
#include "storage/table.h"
#include "catalog/catalog.h"
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <unordered_map>

// M6 前向声明: Session 在 sqltiny::core 里, 我们只需要指针
namespace sqltiny { namespace core { class Session; } }

namespace sqltiny::executor {

// 前向声明 — ExecContext 里要持有 Executor* 用于子查询递归求值
class Executor;

// 表达式层 free function(跨 cpp 可见,定义在 expr.cpp)
bool to_bool(const types::Value& v);

// CTE / 派生表物化结果:name → 临时表(shared_ptr 便于跨作用域共享视图)
using CteMap = std::map<std::string, std::shared_ptr<storage::Table>>;

/// 执行上下文:提供行上下文 + 列查找 + 表查找
struct ExecContext {
    catalog::Catalog& catalog;
    std::string primary_table;
    std::vector<std::pair<std::string, std::string>> tables;
    storage::Row current_row;
    std::map<std::string, int> col_index;

    // 热路径零拷贝:指向「调用方持有的行」与「共享列索引」。设置后优先于上面两个值成员,
    // 避免每行构造 ctx 时拷贝整行 + 深拷贝 col_index(万行级聚合是决定性常数开销)。
    // 生命周期契约:两者指向的对象必须比 ctx 活得更久(调用方保证行来自容器/外层变量)。
    const storage::Row* row_ref = nullptr;
    const std::map<std::string, int>* idx_ref = nullptr;

    // 子查询求值时递归调 exec_select 需要 executor 指针
    Executor* exec = nullptr;

    // 相关子查询:外层作用域链。列在本层解析不到时沿 outer 向上找;
    // 离开某层作用域时置位该层的 outer_used,供子查询缓存判定是否相关。
    const ExecContext* outer = nullptr;
    bool* outer_used = nullptr;
    // 当前可见的 CTE 物化表
    const CteMap* ctes = nullptr;

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

    /// M6 P1: 未限定库名的表/DDL 归属哪个库 —— 有 session 用 session 的 current_database,
    /// 否则退回 catalog 的 server 级 current_database(WAL replay 等)
    std::string default_db() const;

    // M6: Session 可选关联 — 用于事务内拿锁 + 记录 RowAction
    void bind_session(core::Session* s) { session_ = s; }
    core::Session* bound_session() const { return session_; }

    /// information_schema 虚拟表解析:TABLES / COLUMNS。
    /// 不是虚拟表返回 nullptr。每次调用都重新物化(数据很小)。
    std::unique_ptr<storage::Table> build_information_schema_table(
        const std::string& db_name, const std::string& table_name) const;
    /// 是否 information_schema 限定名 (大小写不敏感)
    static bool is_information_schema_name(const std::string& qualified);

private:
    catalog::Catalog& catalog_;
    core::Session* session_ = nullptr;

    ResultSet exec_create_table(const ast::CreateTableStmt& s);
    ResultSet exec_drop_table(const ast::DropTableStmt& s);
    ResultSet exec_insert(const ast::InsertStmt& s);
    ResultSet exec_update(const ast::UpdateStmt& s);
    ResultSet exec_delete(const ast::DeleteStmt& s);
    ResultSet exec_select(const ast::SelectStmt& s, const ExecContext* outer = nullptr,
                          bool* outer_used = nullptr, const CteMap* outer_ctes = nullptr);
    ResultSet exec_select_body(const ast::SelectStmt& s, const ExecContext* outer = nullptr,
                               bool* outer_used = nullptr, const CteMap* outer_ctes = nullptr);
    // 子查询入口:带外部作用域与相关性探测
    ResultSet execute_sub(const ast::Statement& stmt, const ExecContext* outer,
                          bool* used, const CteMap* ctes);
    // 子查询结果行(非相关则按 AST 节点缓存;相关则每次重算)
    const std::vector<storage::Row>* subquery_rows(const ast::Expression& e,
                                                   const ExecContext& ctx) const;
    // WITH 子句物化(含递归 CTE 不动点迭代),失败返回 ok=false
    ResultSet materialize_ctes(const std::vector<ast::SelectStmt::CteDef>& defs, CteMap& ctes);
    ResultSet exec_show_tables(bool full = false, const std::string& db_name = "",
                               const std::string& filter_pattern = "");
    ResultSet exec_show_columns(const std::string& db_name, const std::string& table_name);
    ResultSet exec_show_databases();
    ResultSet exec_show_create_table(const std::string& db_name, const std::string& table_name);
    ResultSet exec_show_create_database(const std::string& db_name);
    ResultSet exec_show_status();
    ResultSet exec_show_variables(const std::string& pattern = "");
    ResultSet exec_show_engines();
    ResultSet exec_show_plugins();
    ResultSet exec_show_charset();
    ResultSet exec_show_collation();
    ResultSet exec_show_index(const std::string& db_name, const std::string& table_name);
    ResultSet exec_show_table_status(const std::string& db_name, const std::string& pattern);
    ResultSet exec_show_triggers();
    ResultSet exec_show_processlist();
    ResultSet exec_show_warnings();

    ResultSet exec_create_database(const ast::CreateDatabaseStmt& s);
    ResultSet exec_drop_database(const ast::DropDatabaseStmt& s);

    // 表达式求值
    types::Value eval(const ast::Expression& e, const ExecContext& ctx) const;
    types::Value eval_like(const types::Value& v, const types::Value& pattern) const;
    types::Value eval_function(const std::string& name, const std::vector<ast::ExprPtr>& args,
                               bool star, const ExecContext& ctx) const;

    // 子查询缓存:按 Subquery 表达式节点指针键控,每条顶层语句开始时清空。
    // 非相关子查询只执行一次(P1:IN 子查询逐行重算 → 物化)。
    struct SqEntry {
        bool correlated = false;
        std::vector<storage::Row> rows;
    };
    mutable std::unordered_map<const void*, SqEntry> sq_cache_;
    mutable std::vector<storage::Row> sq_scratch_;   // 相关子查询的临时结果
    mutable std::string eval_error_;                 // 子查询执行错误向上传播
    mutable int exec_depth_ = 0;
};

} // namespace sqltiny::executor
