#include "net/mysql_server.h"
#include "core/session.h"
#include <fmt/format.h>
#include <cstdlib>

void print_usage(const char* prog) {
    fmt::print("Usage: {} [--port N] [--user U] [--password P] [--data-dir DIR] [--query-log FILE]\n", prog);
    fmt::print("  --data-dir DIR     数据目录(WAL 文件存这里). 缺省 = 内存模式(无持久化)\n");
    fmt::print("  --query-log FILE   每条收到的 SQL 及结果追加写入该文件(调试客户端兼容性用)\n");
}

int main(int argc, char** argv) {
    sqltiny::net::ServerConfig cfg;
    std::string data_dir;

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
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else { fmt::print("unknown arg: {}\n", a); print_usage(argv[0]); return 1; }
    }

    fmt::print("SQLTiny v1.2.0 — MySQL wire protocol server\n");
    fmt::print("  port:     {}\n", cfg.port);
    fmt::print("  user:     {}\n", cfg.user);
    fmt::print("  password: {}\n", cfg.password.empty() ? "<any>" : "<set>");
    fmt::print("  data-dir: {}\n", data_dir.empty() ? "<memory-only>" : data_dir);

    // 初始化持久化层 (如果设置了 data-dir)
    sqltiny::core::Database::instance().open(data_dir);

    fmt::print("  (开发期密码任意,user=root 即可登录)\n\n");

    int rc = sqltiny::net::run_mysql_server(cfg);

    // 优雅关闭
    sqltiny::core::Database::instance().shutdown();

    return rc;
}
