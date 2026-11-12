#pragma once

#include "net/mysql_proto.h"
#include <string>

namespace sqltiny::net {

struct ServerConfig {
    uint16_t port = 3306;
    std::string user = "root";
    std::string password;          // 开发期留空 = 任意密码都过
    uint32_t thread_count = 4;
    std::string query_log;         // 非空时:每条收到的 COM_QUERY 及结果写入该文件(调试用)
};

/// 线程入口:accept 循环
int run_mysql_server(const ServerConfig& cfg);

} // namespace sqltiny::net
