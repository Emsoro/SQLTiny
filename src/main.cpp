#include "net/mysql_server.h"
#include <fmt/format.h>
#include <cstdlib>

void print_usage(const char* prog) {
    fmt::print("Usage: {} [--port N] [--user U] [--password P]\n", prog);
}

int main(int argc, char** argv) {
    sqltiny::net::ServerConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() {
            if (i + 1 >= argc) { fmt::print("{} requires a value\n", a); exit(1); }
            return argv[++i];
        };
        if (a == "--port")        cfg.port = static_cast<uint16_t>(std::atoi(next()));
        else if (a == "--user")   cfg.user = next();
        else if (a == "--password") cfg.password = next();
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else { fmt::print("unknown arg: {}\n", a); print_usage(argv[0]); return 1; }
    }

    fmt::print("SQLTiny v1.0 — MySQL wire protocol server\n");
    fmt::print("  port:     {}\n", cfg.port);
    fmt::print("  user:     {}\n", cfg.user);
    fmt::print("  password: {}\n", cfg.password.empty() ? "<any>" : "<set>");
    fmt::print("  (开发期密码任意,user=root 即可登录)\n\n");

    return sqltiny::net::run_mysql_server(cfg);
}
