#include "net/mysql_server.h"
#include "core/session.h"
#include "parser/parser.h"
#include "executor/executor.h"
#include <fmt/format.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#endif

namespace sqltiny::net {

// 这些函数在 mysql_proto.cpp 里定义,extern 过来
std::vector<uint8_t> encode_handshake_v10(uint32_t tid, const std::vector<uint8_t>& auth);
bool send_all(int fd, const uint8_t* p, size_t n);
bool recv_all(int fd, uint8_t* p, size_t n);
std::vector<uint8_t> encode_mysql_packet(const std::vector<uint8_t>& payload, uint8_t seq);
std::vector<uint8_t> read_mysql_packet(int socket_fd, uint8_t& seq_out);
bool send_mysql_response(int fd, const executor::ResultSet& rs, uint8_t& seq, bool deprecate_eof);

namespace {

int make_listen_socket(uint16_t port) {
#ifdef _WIN32
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
#endif
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fmt::print(stderr, "socket failed\n"); return -1; }
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        fmt::print(stderr, "bind :{} failed\n", port);
        return -1;
    }
    if (::listen(fd, 64) < 0) {
        fmt::print(stderr, "listen failed\n");
        return -1;
    }
    fmt::print("SQLTiny MySQL server listening on 0.0.0.0:{}\n", port);
    return fd;
}

void close_fd(int fd) {
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

// 处理一个新连接
void handle_connection(int client_fd, const ServerConfig& cfg, uint32_t thread_id) {
    core::Session session;
    uint8_t seq = 0;

    // 1. 发 handshake
    std::vector<uint8_t> auth_salt(20);
    // 随机 salt 直接用占位(开发期不校验密码)
    for (int i = 0; i < 20; ++i) auth_salt[i] = static_cast<uint8_t>(i * 13 + 7);

    auto hb = encode_handshake_v10(thread_id, auth_salt);
    auto pkt = encode_mysql_packet(hb, seq++);
    if (!send_all(client_fd, pkt.data(), pkt.size())) return;

    // 2. 收 HandshakeResponse
    auto hs_payload = read_mysql_packet(client_fd, seq);
    if (hs_payload.empty()) return;
    seq++;

    auto hr_opt = parse_handshake_response(hs_payload.data(), hs_payload.size());
    if (!hr_opt) {
        fmt::print(stderr, "[conn] failed to parse HandshakeResponse\n");
        return;
    }
    auto& hr = *hr_opt;

    // 3. 校验 user + auth(开发期简化:user=root 就行,不校验密码)
    if (_stricmp(hr.user.c_str(), cfg.user.c_str()) != 0) {
        auto err = encode_err_packet(1045, "28000", "Access denied for user");
        auto epkt = encode_mysql_packet(err, seq++);
        send_all(client_fd, epkt.data(), epkt.size());
        return;
    }

    // 4. 发 OK
    auto ok = encode_ok_packet(0, 0, SERVER_STATUS_AUTOCOMMIT, 0, "");
    auto okpkt = encode_mysql_packet(ok, seq++);
    if (!send_all(client_fd, okpkt.data(), okpkt.size())) return;

    session.user = hr.user;
    if (!hr.database.empty()) {
        session.current_database = hr.database;
    }

    // 5. 命令循环
    bool deprecate_eof = (hr.client_cap & CLIENT_DEPRECATE_EOF) != 0;
    while (true) {
        seq = 0;
        auto cmd_payload = read_mysql_packet(client_fd, seq);
        if (cmd_payload.empty()) break;
        seq++;
        if (cmd_payload.empty()) continue;

        uint8_t cmd = cmd_payload[0];
        // MySQL 协议不要求处理的命令就回 OK
        // 简单分发:只处理 QUERY,其他回 OK

        switch (cmd) {
            case COM_QUIT: return;

            case COM_PING: {
                auto r = encode_ok_packet(0, 0, SERVER_STATUS_AUTOCOMMIT, 0, "");
                auto p = encode_mysql_packet(r, seq++);
                if (!send_all(client_fd, p.data(), p.size())) return;
                break;
            }
            case COM_INIT_DB: {
                std::string db_name(reinterpret_cast<const char*>(cmd_payload.data() + 1), cmd_payload.size() - 1);
                session.current_database = db_name;
                auto r = encode_ok_packet(0, 0, SERVER_STATUS_AUTOCOMMIT, 0, "");
                auto p = encode_mysql_packet(r, seq++);
                if (!send_all(client_fd, p.data(), p.size())) return;
                break;
            }
            case COM_QUERY: {
                std::string sql(reinterpret_cast<const char*>(cmd_payload.data() + 1), cmd_payload.size() - 1);
                auto result = session.execute(sql);
                if (!send_mysql_response(client_fd, result, seq, deprecate_eof)) return;
                break;
            }
            case COM_FIELD_LIST: {
                // 简化:返回 SHOW COLUMNS 结果
                std::string table_name(reinterpret_cast<const char*>(cmd_payload.data() + 1), cmd_payload.size() - 1);
                auto result = session.execute("SHOW COLUMNS FROM " + table_name);
                if (!send_mysql_response(client_fd, result, seq, deprecate_eof)) return;
                break;
            }
            default: {
                fmt::print("[conn] unsupported COM=0x{:02x}\n", cmd);
                auto err = encode_err_packet(1047, "08S01", fmt::format("Unsupported command 0x{:02x}", cmd));
                auto p = encode_mysql_packet(err, seq++);
                if (!send_all(client_fd, p.data(), p.size())) return;
                break;
            }
        }
    }
}

} // anonymous

int run_mysql_server(const ServerConfig& cfg) {
    int listen_fd = make_listen_socket(cfg.port);
    if (listen_fd < 0) return 1;

    std::atomic<uint32_t> next_tid{1};

    while (true) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int client = ::accept(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), &addr_len);
        if (client < 0) {
            fmt::print(stderr, "accept failed\n");
            continue;
        }
        uint32_t tid = next_tid.fetch_add(1);
        fmt::print("[conn] new client tid={}\n", tid);
        // detach 后台线程
        std::thread([client, cfg, tid]() {
            handle_connection(client, cfg, tid);
            close_fd(client);
            fmt::print("[conn] closed tid={}\n", tid);
        }).detach();
    }
    return 0;
}

} // namespace sqltiny::net
