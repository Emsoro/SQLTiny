#include "net/mysql_server.h"
#include "core/session.h"
#include "cli/sql_io.h"
#include <fmt/format.h>
#include <cstdlib>
#include <iostream>
#include <string>

void print_usage(const char* prog) {
    fmt::print("Usage: {} [options]\n", prog);
    fmt::print("\nServer options:\n");
    fmt::print("  --port N           监听端口 (默认 3306)\n");
    fmt::print("  --user U           用户名 (默认 root)\n");
    fmt::print("  --password P       密码\n");
    fmt::print("  --data-dir DIR     数据目录(WAL 文件存这里). 缺省 = 内存模式(无持久化)\n");
    fmt::print("  --query-log FILE   每条收到的 SQL 及结果追加写入该文件(调试客户端兼容性用)\n");
    fmt::print("\nImport / dump (需要 --data-dir,执行后退出):\n");
    fmt::print("  --import FILE              从 SQL 脚本导入数据\n");
    fmt::print("  --import-continue-on-error 单条语句失败后继续执行\n");
    fmt::print("  --import-quiet             不打印导入进度\n");
    fmt::print("  --import-then-serve        导入完成后继续启动 MySQL 服务\n");
    fmt::print("  --dump FILE                导出为 SQL 脚本 (FILE 为 - 时写 stdout)\n");
    fmt::print("  --dump-database NAME       仅导出指定库,可重复\n");
    fmt::print("  --dump-table [db.]NAME     仅导出指定表,可重复\n");
    fmt::print("  --dump-no-create           不输出 CREATE TABLE\n");
    fmt::print("  --dump-no-data             不输出 INSERT\n");
    fmt::print("  --dump-no-drop             不输出 DROP TABLE IF EXISTS\n");
    fmt::print("  --dump-batch-rows N        每条 INSERT 最多 N 行 (默认 1000)\n");
}

int main(int argc, char** argv) {
    // 重定向到文件时也立即刷新,方便调试观察 server 输出
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    sqltiny::net::ServerConfig cfg;
    std::string data_dir;

    std::string import_file;
    std::string dump_file;
    sqltiny::cli::ImportOptions import_opts;
    sqltiny::cli::DumpOptions dump_opts;
    bool import_then_serve = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() {
            if (i + 1 >= argc) { fmt::print("{} requires a value\n", a); exit(1); }
            return argv[++i];
        };
        if (a == "--port")        cfg.port = static_cast<uint16_t>(std::atoi(next()));
        else if (a == "--user")   cfg.user = next();
        else if (a == "--password") cfg.password = next();
        else if (a == "--data-dir") data_dir = next();
        else if (a == "--query-log") cfg.query_log = next();
        else if (a == "--import") import_file = next();
        else if (a == "--import-continue-on-error") import_opts.continue_on_error = true;
        else if (a == "--import-quiet") import_opts.quiet = true;
        else if (a == "--import-then-serve") import_then_serve = true;
        else if (a == "--dump") dump_file = next();
        else if (a == "--dump-database") dump_opts.databases.push_back(next());
        else if (a == "--dump-table") dump_opts.tables.push_back(next());
        else if (a == "--dump-no-create") dump_opts.include_schema = false;
        else if (a == "--dump-no-data") dump_opts.include_data = false;
        else if (a == "--dump-no-drop") dump_opts.drop_tables = false;
        else if (a == "--dump-batch-rows") {
            const char* value = next();
            char* end = nullptr;
            unsigned long long n = std::strtoull(value, &end, 10);
            if (*value == '-' || !end || *end != '\0' || n == 0) {
                fmt::print("--dump-batch-rows requires a positive integer\n");
                return 1;
            }
            dump_opts.rows_per_insert = static_cast<size_t>(n);
        }
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else { fmt::print("unknown arg: {}\n", a); print_usage(argv[0]); return 1; }
    }

    const bool import_requested = !import_file.empty();
    const bool dump_requested = !dump_file.empty();
    const bool batch_mode = import_requested || dump_requested;

    if (import_then_serve && !import_requested) {
        fmt::print("--import-then-serve requires --import\n");
        return 1;
    }
    if (import_then_serve && dump_requested) {
        fmt::print("--import-then-serve cannot be used with --dump\n");
        return 1;
    }
    if (batch_mode && data_dir.empty()) {
        fmt::print("--import / --dump require --data-dir\n");
        return 1;
    }

    const bool dump_to_stdout = dump_requested && dump_file == "-";
    FILE* info = dump_to_stdout ? stderr : stdout;

    fmt::print(info, "SQLTiny v2.2.1 — MySQL wire protocol server\n");
    fmt::print(info, "  port:     {}\n", cfg.port);
    fmt::print(info, "  user:     {}\n", cfg.user);
    fmt::print(info, "  password: {}\n", cfg.password.empty() ? "<any>" : "<set>");
    fmt::print(info, "  data-dir: {}\n", data_dir.empty() ? "<memory-only>" : data_dir);

    // 初始化持久化层 (如果设置了 data-dir)
    if (!sqltiny::core::Database::instance().open(data_dir)) {
        fmt::print(stderr, "failed to open data-dir: {}\n", data_dir);
        return 1;
    }

    bool batch_ok = true;

    if (import_requested) {
        fmt::print(info, "\n[import] {}\n", import_file);
        sqltiny::core::Session session;
        sqltiny::cli::ImportStats stats;
        batch_ok = sqltiny::cli::import_sql_file(import_file, session, import_opts, stats, std::cerr);
        fmt::print(info, "[import] done: {} statements, {} ok, {} failed, {} skipped, {} rows affected\n",
                   stats.statements, stats.succeeded, stats.failed, stats.skipped,
                   stats.affected_rows);
        if (!batch_ok) {
            if (!stats.first_error.empty())
                fmt::print(stderr, "[import] first error: {}\n", stats.first_error);
        }
    }

    if (dump_requested) {
        if (!batch_ok) {
            fmt::print(info, "[dump] skipped because import failed\n");
        } else {
            fmt::print(info, "\n[dump] {}\n", dump_file == "-" ? "<stdout>" : dump_file);
            sqltiny::core::Session session;
            sqltiny::cli::DumpStats stats;
            batch_ok = sqltiny::cli::dump_sql_file(dump_file, session, dump_opts, stats, std::cerr);
            fmt::print(info, "[dump] done: {} databases, {} tables, {} rows\n",
                       stats.databases, stats.tables, stats.rows);
        }
    }

    if (batch_mode && !import_then_serve) {
        sqltiny::core::Database::instance().shutdown();
        return batch_ok ? 0 : 1;
    }
    if (import_then_serve && !batch_ok) {
        fmt::print("import failed; server not started\n");
        sqltiny::core::Database::instance().shutdown();
        return 1;
    }

    fmt::print("\n  (开发期密码任意,user=root 即可登录)\n\n");

    int rc = sqltiny::net::run_mysql_server(cfg);

    // 优雅关闭
    sqltiny::core::Database::instance().shutdown();

    return rc;
}
