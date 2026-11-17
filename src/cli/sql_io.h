#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace sqltiny { namespace core { class Session; } }

namespace sqltiny::cli {

// ============================================================
// SQL 脚本导入
// ============================================================

struct ImportOptions {
    /// 单条语句失败后是否继续执行后续语句(默认遇到第一个错误即停止)
    bool continue_on_error = false;
    /// 不打印进度
    bool quiet = false;
    /// 每执行多少条语句打印一次进度
    size_t progress_every = 1000;
};

struct ImportStats {
    uint64_t statements = 0;      ///< 从脚本中读到的语句数
    uint64_t succeeded  = 0;      ///< 成功执行数
    uint64_t failed     = 0;      ///< 失败数
    uint64_t skipped    = 0;      ///< 主动跳过的语句(LOCK/UNLOCK TABLES 等)
    int64_t  affected_rows = 0;   ///< INSERT/UPDATE/DELETE 影响行数累计
    std::string first_error;      ///< 第一条错误信息(含行号)
};

/// 从流中导入 SQL 脚本。支持:
///   - `--` / `#` 行注释、`/* ... */` 块注释(含 MySQL `/*! ... */` 条件注释)
///   - 单引号/双引号/反引号字符串及转义
///   - `DELIMITER` 指令、多语句脚本
///   - 自动跳过 mysqldump 生成的 `LOCK TABLES` / `UNLOCK TABLES`
/// 返回 false 表示有语句失败或读取失败;continue_on_error 只决定是否继续执行后续语句。
bool import_sql(std::istream& in, core::Session& session, const ImportOptions& opts,
                ImportStats& stats, std::ostream& log);

/// 打开文件并导入(文本模式)。
bool import_sql_file(const std::string& path, core::Session& session,
                     const ImportOptions& opts, ImportStats& stats, std::ostream& log);

// ============================================================
// dump 导出(MySQL 风格 SQL 脚本)
// ============================================================

struct DumpOptions {
    /// 仅导出这些库;为空表示全部库
    std::vector<std::string> databases;
    /// 仅导出这些表(`db.table` 或裸表名);为空表示所选库的全部表
    std::vector<std::string> tables;
    bool include_schema = true;   ///< 输出 CREATE TABLE
    bool include_data   = true;   ///< 输出 INSERT
    bool drop_tables    = true;   ///< CREATE 前输出 DROP TABLE IF EXISTS
    size_t rows_per_insert = 1000; ///< 每条 INSERT 最多包含多少行
};

struct DumpStats {
    uint64_t databases = 0;
    uint64_t tables    = 0;
    uint64_t rows      = 0;
};

/// 把当前 Catalog 中的库/表数据写成可被 import_sql 重新读入的 SQL 脚本。
/// 返回 false 表示参数错误或输出流写入失败。
bool dump_sql(core::Session& session, const DumpOptions& opts,
              std::ostream& out, DumpStats& stats, std::ostream& log);

/// 打开文件并导出;path 为 "-" 时写 stdout。
bool dump_sql_file(const std::string& path, core::Session& session,
                   const DumpOptions& opts, DumpStats& stats, std::ostream& log);

} // namespace sqltiny::cli
