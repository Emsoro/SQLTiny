#include "net/mysql_server.h"
#include "core/session.h"
#include "parser/parser.h"
#include "parser/lexer.h"
#include "executor/executor.h"
#include <fmt/format.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <unordered_map>
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
bool send_mysql_response(int fd, const executor::ResultSet& rs, uint8_t& seq,
                         bool deprecate_eof, uint16_t status_flags);

namespace {

// ============================================================
// 预编译语句 (COM_STMT_PREPARE / EXECUTE) 辅助
// ============================================================

/// 数一下 SQL 里的 ? 占位符个数(走 lexer,避免字符串里的 ? 误计)
size_t count_placeholders(const std::string& sql) {
    parser::Lexer lx(sql);
    size_t n = 0;
    while (true) {
        auto t = lx.next();
        if (t.kind == parser::TokenKind::Eof) break;
        if (t.kind == parser::TokenKind::Placeholder) ++n;
    }
    return n;
}

/// SQL 字符串字面量转义 + 加引号
std::string sql_quote_literal(const std::string& raw) {
    std::string out = "'";
    for (char c : raw) {
        if (c == '\'') out += "\\'";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    out += "'";
    return out;
}

/// 把 SQL 里(引号外的)? 依次替换成 literals;耗尽后替换成 NULL
std::string splice_params(const std::string& sql, const std::vector<std::string>& literals) {
    std::string out;
    size_t li = 0;
    char quote = 0;
    for (size_t i = 0; i < sql.size(); ++i) {
        char c = sql[i];
        if (quote) {
            out += c;
            if (c == '\\' && i + 1 < sql.size()) { out += sql[++i]; continue; }  // 转义下一字符
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') { quote = c; out += c; continue; }
        if (c == '?') {
            out += (li < literals.size()) ? literals[li++] : std::string("NULL");
            continue;
        }
        out += c;
    }
    return out;
}

/// 从 COM_STMT_EXECUTE 包读一个二进制参数值并渲染成 SQL 字面量
std::string read_binary_param(PacketReader& r, uint8_t type, uint8_t unflag) {
    using namespace sqltiny::types;
    auto need = [&](size_t n) { return r.len - r.pos >= n; };
    auto read_int = [&](size_t bytes) -> int64_t {
        uint64_t u = 0;
        for (size_t i = 0; i < bytes && !r.eof(); ++i) u |= static_cast<uint64_t>(r.u8()) << (8 * i);
        if (unflag) return static_cast<int64_t>(u);
        // 符号扩展
        if (bytes < 8) {
            uint64_t sign_bit = 1ULL << (bytes * 8 - 1);
            if (u & sign_bit) u -= (sign_bit << 1);
        }
        return static_cast<int64_t>(u);
    };
    switch (type) {
        case MYSQL_TYPE_TINY:   return std::to_string(read_int(1));
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_YEAR:   return std::to_string(read_int(2));
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_INT24:  return std::to_string(read_int(4));
        case MYSQL_TYPE_LONGLONG: return std::to_string(read_int(8));
        case MYSQL_TYPE_FLOAT: {
            if (!need(4)) return "NULL";
            uint32_t u = r.u32(); float f;
            std::memcpy(&f, &u, 4);
            return fmt::format("{}", f);
        }
        case MYSQL_TYPE_DOUBLE: {
            if (!need(8)) return "NULL";
            uint64_t u = r.u64(); double d;
            std::memcpy(&d, &u, 8);
            return fmt::format("{}", d);
        }
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIMESTAMP: {
            if (r.eof()) return "NULL";
            uint8_t n = r.u8();
            if (n == 0) return sql_quote_literal("0000-00-00 00:00:00");
            int32_t year = 0; uint8_t month = 0, day = 0, hour = 0, minute = 0, second = 0; uint32_t us = 0;
            if (n >= 4 && need(4)) { year = static_cast<int32_t>(r.u16()); month = r.u8(); day = r.u8(); }
            if (n >= 7 && need(3)) { hour = r.u8(); minute = r.u8(); second = r.u8(); }
            if (n >= 11 && need(4)) us = r.u32();
            std::string s = fmt::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}",
                                        year, month, day, hour, minute, second);
            if (us) s += fmt::format(".{:06d}", us);
            return sql_quote_literal(s);
        }
        case MYSQL_TYPE_TIME: {
            if (r.eof()) return "NULL";
            uint8_t n = r.u8();
            if (n == 0) return sql_quote_literal("00:00:00");
            if (!need(n >= 12 ? 12 : 8)) return "NULL";
            uint8_t neg = r.u8();
            uint32_t days = r.u32(); uint8_t h = r.u8(), mi = r.u8(), s = r.u8();
            if (n >= 12) r.u32();  // microseconds
            uint32_t total_h = days * 24 + h;
            std::string t = fmt::format("{:02d}:{:02d}:{:02d}", total_h, mi, s);
            return sql_quote_literal(neg ? "-" + t : t);
        }
        case MYSQL_TYPE_BIT: {
            uint64_t n = r.lenenc_int();
            uint64_t v = 0;
            for (uint64_t i = 0; i < n && !r.eof(); ++i) v |= static_cast<uint64_t>(r.u8()) << (8 * i);
            return std::to_string(v);
        }
        default: {
            // VARCHAR/VAR_STRING/STRING/BLOB/DECIMAL/NEWDECIMAL/JSON → lenenc 字符串
            return sql_quote_literal(r.lenenc_string());
        }
    }
}

// 预编译语句记录
struct PreparedStmt {
    std::string sql;
    uint16_t num_params = 0;
    uint16_t num_columns = 0;
    std::vector<std::string> column_names;
    std::vector<sqltiny::types::SqlType> column_types;
    std::vector<uint8_t> param_types;   // 每参数 2 字节 (type, unsigned)
};

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

    // 优雅关闭:直接 close 会发 RST,客户端可能来不及读到 ERR 包
    void graceful_close(int fd) {
#ifdef _WIN32
        ::shutdown(fd, SD_SEND);
#else
        ::shutdown(fd, SHUT_WR);
#endif
        // 排空对端剩余数据,直到对端也关闭
        char tmp[256];
        while (::recv(fd, tmp, sizeof(tmp), 0) > 0) {}
    }

// 处理一个新连接
namespace {
// 原始字节 hex(调试 Navicat 兼容性用,最多 160 字节)
std::string hex_dump(const uint8_t* p, size_t n) {
    std::string out = fmt::format("[{} bytes] ", n);
    size_t k = std::min<size_t>(n, 160);
    char tmp[8];
    for (size_t i = 0; i < k; ++i) {
        std::snprintf(tmp, sizeof(tmp), "%02x", p[i]);
        out += tmp;
        if (i % 32 == 31) out += ' ';
    }
    if (n > k) out += fmt::format("...(+{} more)", n - k);
    return out;
}
}

void handle_connection(int client_fd, const ServerConfig& cfg, uint32_t thread_id) {
    core::Session session;
    uint8_t seq = 0;

    // 调试日志:把握手 + 每条命令都写进 query_log(打开 --query-log 时)
    auto dbg = [&](const std::string& line) {
        if (cfg.query_log.empty()) return;
        static std::mutex log_mu;
        std::lock_guard<std::mutex> lk(log_mu);
        if (FILE* f = std::fopen(cfg.query_log.c_str(), "a")) {
            std::fputs(line.c_str(), f);
            std::fclose(f);
        }
    };

    // 1. 发 handshake
    std::vector<uint8_t> auth_salt(20);
    // 随机 salt 直接用占位(开发期不校验密码)
    for (int i = 0; i < 20; ++i) auth_salt[i] = static_cast<uint8_t>(i * 13 + 7);

    auto hb = encode_handshake_v10(thread_id, auth_salt);
    auto pkt = encode_mysql_packet(hb, seq++);
    dbg(fmt::format("[CONN] tid={} fd={}\n", thread_id, client_fd));
    dbg(fmt::format("[SEND] handshake seq=0 {}\n", hex_dump(pkt.data(), pkt.size())));
    if (!send_all(client_fd, pkt.data(), pkt.size())) { dbg("[FAIL] send handshake\n"); return; }

    // 2. 收 HandshakeResponse
    auto hs_payload = read_mysql_packet(client_fd, seq);
    if (hs_payload.empty()) { dbg("[FAIL] read HandshakeResponse (EOF/reset)\n"); return; }
    dbg(fmt::format("[RECV] HandshakeResponse seq={}\n", seq) + hex_dump(hs_payload.data(), hs_payload.size()) + "\n");
    seq++;

    auto hr_opt = parse_handshake_response(hs_payload.data(), hs_payload.size());
    if (!hr_opt) {
        fmt::print(stderr, "[conn] failed to parse HandshakeResponse\n");
        return;
    }
    auto& hr = *hr_opt;

    dbg(fmt::format("[HANDSHAKE] tid={} user='{}' db='{}' caps=0x{:08x} plugin='{}' auth_len={}\n",
                    thread_id, hr.user, hr.database, hr.client_cap,
                    hr.auth_plugin.empty() ? "<none>" : hr.auth_plugin, hr.auth_response.size()));

    // SSL 请求:客户端发完 SSL Request 包后直接等 TLS ServerHello —— 我们不支持 TLS,
    // 必须立刻回 ERR,否则客户端会一直等到超时
    if (hr.client_cap & CLIENT_SSL) {
        dbg("[HANDSHAKE] client requested SSL -> ERR 1043 (TLS unsupported)\n");
        auto err = encode_err_packet(1043, "08S01", "SSL connections not supported by SQLTiny");
        auto epkt = encode_mysql_packet(err, seq++);
        send_all(client_fd, epkt.data(), epkt.size());
        graceful_close(client_fd);
        return;
    }

    // M6 P1 #6: 握手的 database 字段生效 —— 不存在的库返回 1049 并拒绝连接
    // (必须在发 OK 之前校验,认证成功后立刻生效)
    if (!hr.database.empty()) {
        auto& cat = core::Database::instance().catalog();
        if (!cat.database_exists(hr.database)) {
            auto err = encode_err_packet(1049, "42000",
                                         fmt::format("Unknown database '{}'", hr.database));
            auto epkt = encode_mysql_packet(err, seq++);
            send_all(client_fd, epkt.data(), epkt.size());
            graceful_close(client_fd);
            return;
        }
    }

    // 3. 校验 user + auth(开发期简化:user=root 就行,不校验密码)
    if (_stricmp(hr.user.c_str(), cfg.user.c_str()) != 0) {
        auto err = encode_err_packet(1045, "28000", "Access denied for user");
        auto epkt = encode_mysql_packet(err, seq++);
        send_all(client_fd, epkt.data(), epkt.size());
        graceful_close(client_fd);
        return;
    }

    // 4. 发 OK
    auto ok = encode_ok_packet(0, 0, SERVER_STATUS_AUTOCOMMIT, 0, "");
    auto okpkt = encode_mysql_packet(ok, seq++);
    dbg(fmt::format("[SEND] auth-ok seq={}\n", seq - 1) + hex_dump(okpkt.data(), okpkt.size()) + "\n");
    if (!send_all(client_fd, okpkt.data(), okpkt.size())) { dbg("[FAIL] send auth-ok\n"); return; }

    session.user = hr.user;
    session.current_database = hr.database.empty() ? session.current_database : hr.database;

    // 事务状态位:让客户端能正确读到 autocommit / in-transaction (MySQL status flags)
    auto status_flags = [&]() -> uint16_t {
        uint16_t f = session.autocommit ? SERVER_STATUS_AUTOCOMMIT : 0;
        if (session.isInTransaction()) f |= SERVER_STATUS_IN_TRANS;
        return f;
    };

    // 5. 命令循环
    // 刻意忽略客户端的 CLIENT_DEPRECATE_EOF:老客户端(dbExpress/DBX)只是回显服务器
    // 广播的能力位,并不真的支持 OK 包终止结果集 —— 永远用经典 EOF 帧。
    bool deprecate_eof = false;

    // 预编译语句表(DBX/dbExpress 查询走 COM_STMT_PREPARE/EXECUTE)
    std::unordered_map<uint32_t, PreparedStmt> prepared;
    uint32_t next_stmt_id = 1;
    while (true) {
        seq = 0;
        auto cmd_payload = read_mysql_packet(client_fd, seq);
        if (cmd_payload.empty()) {
            dbg("[FAIL] command read (EOF/reset) -> close\n");
            break;
        }
        seq++;
        if (cmd_payload.empty()) continue;

        uint8_t cmd = cmd_payload[0];
        dbg(fmt::format("[CMD] 0x{:02x} payload_len={}\n", cmd,
                        cmd_payload.size() > 0 ? cmd_payload.size() - 1 : 0));
        dbg(fmt::format("[RECV] seq={} {}\n", seq, hex_dump(cmd_payload.data(), cmd_payload.size())));
        // MySQL 协议不要求处理的命令就回 OK
        // 简单分发:只处理 QUERY,其他回 OK

        switch (cmd) {
            case COM_QUIT: return;

            case COM_PING: {
                auto r = encode_ok_packet(0, 0, status_flags(), 0, "");
                auto p = encode_mysql_packet(r, seq++);
                dbg("  -> OK (ping)\n");
                if (!send_all(client_fd, p.data(), p.size())) return;
                break;
            }
            case COM_INIT_DB: {
                std::string db_name(reinterpret_cast<const char*>(cmd_payload.data() + 1), cmd_payload.size() - 1);
                auto& cat = core::Database::instance().catalog();
                if (!cat.database_exists(db_name)) {
                    auto err = encode_err_packet(1049, "42000",
                                                 fmt::format("Unknown database '{}'", db_name));
                    auto p = encode_mysql_packet(err, seq++);
                    dbg(fmt::format("  -> ERR unknown db '{}'\n", db_name));
                    if (!send_all(client_fd, p.data(), p.size())) return;
                    break;
                }
                session.current_database = db_name;
                auto r = encode_ok_packet(0, 0, status_flags(), 0, "");
                auto p = encode_mysql_packet(r, seq++);
                dbg(fmt::format("  -> OK db='{}'\n", db_name));
                if (!send_all(client_fd, p.data(), p.size())) return;
                break;
            }
            case COM_CHANGE_USER: {
                // DBX 连接流程会用 CHANGE_USER 重置会话。解析 user / db 并回 OK。
                // payload: user\0 auth_len(1) auth_data db\0 charset(2) [plugin]
                if (cmd_payload.size() < 2) return;
                size_t off = 1;
                auto read_cstr = [&](std::string& out) {
                    size_t start = off;
                    while (off < cmd_payload.size() && cmd_payload[off] != 0) ++off;
                    out.assign(reinterpret_cast<const char*>(cmd_payload.data()) + start,
                               off > start ? off - start : 0);
                    if (off < cmd_payload.size()) ++off;   // skip NUL
                };
                std::string user, db, auth;
                read_cstr(user);
                if (off < cmd_payload.size()) {
                    uint8_t alen = cmd_payload[off++];
                    off += alen;
                }
                read_cstr(db);
                session.user = user;
                if (!db.empty()) session.current_database = db;
                auto r = encode_ok_packet(0, 0, status_flags(), 0, "");
                auto p = encode_mysql_packet(r, seq++);
                dbg(fmt::format("  -> OK change_user='{}' db='{}'\n", user, db));
                if (!send_all(client_fd, p.data(), p.size())) return;
                break;
            }
            case COM_STMT_PREPARE: {
                std::string sql(reinterpret_cast<const char*>(cmd_payload.data() + 1), cmd_payload.size() - 1);
                size_t num_params = count_placeholders(sql);
                PreparedStmt ps;
                ps.sql = sql;
                ps.num_params = static_cast<uint16_t>(num_params);
                uint16_t num_cols = 0;

                // 语法检查;SELECT/SHOW 额外用 NULL 替换占位符试执行拿列元数据(只读,安全)
                bool syntax_ok = true;
                bool is_read = false;
                std::string syntax_err;
                try {
                    parser::Parser p(sql);
                    auto stmts = p.parse_all();
                    is_read = !stmts.empty() &&
                              (stmts[0]->kind == ast::StmtKind::Select ||
                               stmts[0]->kind == ast::StmtKind::Show);
                } catch (const std::exception& e) {
                    syntax_ok = false;
                    syntax_err = e.what();
                }
                if (!syntax_ok) {
                    auto err = encode_err_packet(1064, "42000",
                        fmt::format("You have an error in your SQL syntax: {}", syntax_err));
                    auto ep = encode_mysql_packet(err, seq++);
                    dbg(fmt::format("[STMT_PREPARE] {}\n  -> ERR syntax: {}\n", sql, syntax_err));
                    if (!send_all(client_fd, ep.data(), ep.size())) return;
                    break;
                }
                if (is_read) {
                    // SELECT/SHOW:用 NULL 替换占位符试执行,拿到列元数据(只读,安全)
                    auto probe = session.execute(splice_params(sql, {}));
                    if (probe.ok && !probe.column_names.empty()) {
                        num_cols = static_cast<uint16_t>(probe.column_names.size());
                        ps.column_names = probe.column_names;
                        ps.column_types = probe.column_types;
                    }
                }
                ps.num_columns = num_cols;
                uint32_t sid = next_stmt_id++;
                prepared[sid] = std::move(ps);
                dbg(fmt::format("[STMT_PREPARE] {}\n  -> id={} params={} cols={}\n",
                                sql, sid, num_params, num_cols));

                // 响应:OK + (参数定义 + EOF) + (列定义 + EOF)
                {
                    auto ok = encode_stmt_prepare_ok(sid, num_cols,
                                                     static_cast<uint16_t>(num_params), 0);
                    auto p1 = encode_mysql_packet(ok, seq++);
                    if (!send_all(client_fd, p1.data(), p1.size())) return;
                }
                if (num_params > 0) {
                    for (size_t i = 0; i < num_params; ++i) {
                        auto d = encode_stmt_column_def("?", sqltiny::types::SqlType::Varchar);
                        auto p2 = encode_mysql_packet(d, seq++);
                        if (!send_all(client_fd, p2.data(), p2.size())) return;
                    }
                    auto eof = encode_mysql_packet(encode_eof_packet(0, status_flags()), seq++);
                    if (!send_all(client_fd, eof.data(), eof.size())) return;
                }
                if (num_cols > 0) {
                    auto& pst = prepared[sid];
                    for (size_t i = 0; i < pst.column_names.size(); ++i) {
                        auto type = i < pst.column_types.size()
                                        ? pst.column_types[i] : sqltiny::types::SqlType::Varchar;
                        auto d = encode_stmt_column_def(pst.column_names[i], type);
                        auto p2 = encode_mysql_packet(d, seq++);
                        if (!send_all(client_fd, p2.data(), p2.size())) return;
                    }
                    auto eof = encode_mysql_packet(encode_eof_packet(0, status_flags()), seq++);
                    if (!send_all(client_fd, eof.data(), eof.size())) return;
                }
                break;
            }
            case COM_STMT_EXECUTE: {
                PacketReader r(cmd_payload.data() + 1, cmd_payload.size() - 1);
                if (r.len < 9) return;
                uint32_t sid = r.u32();
                auto it = prepared.find(sid);
                if (it == prepared.end()) {
                    auto err = encode_err_packet(1243, "HY000", "Unknown statement id");
                    auto ep = encode_mysql_packet(err, seq++);
                    if (!send_all(client_fd, ep.data(), ep.size())) return;
                    break;
                }
                auto& ps = it->second;
                r.u8();      // flags
                r.u32();     // iteration count
                std::vector<std::string> literals;
                if (ps.num_params > 0) {
                    size_t null_bytes = (ps.num_params + 7) / 8;
                    std::vector<uint8_t> nullbm;
                    for (size_t i = 0; i < null_bytes && !r.eof(); ++i)
                        nullbm.push_back(r.u8());
                    uint8_t bound = r.eof() ? 0 : r.u8();
                    if (bound) {
                        ps.param_types.clear();
                        for (size_t i = 0; i < ps.num_params && !r.eof(); ++i) {
                            ps.param_types.push_back(r.u8());
                            ps.param_types.push_back(r.u8());
                        }
                    }
                    for (size_t i = 0; i < ps.num_params; ++i) {
                        bool isnull = (i / 8 < nullbm.size()) &&
                                      ((nullbm[i / 8] >> (i % 8)) & 1);
                        if (isnull) { literals.push_back("NULL"); continue; }
                        uint8_t type = (i * 2 < ps.param_types.size()) ? ps.param_types[i * 2]
                                                                       : MYSQL_TYPE_VAR_STRING;
                        uint8_t unflag = (i * 2 + 1 < ps.param_types.size())
                                             ? ps.param_types[i * 2 + 1] : 0;
                        literals.push_back(read_binary_param(r, type, unflag));
                    }
                }
                std::string final_sql = splice_params(ps.sql, literals);
                dbg(fmt::format("[STMT_EXEC id={}] {}\n", sid, final_sql));
                auto result = session.execute(final_sql);
                if (!result.ok) {
                    dbg(fmt::format("  -> ERR errno={} msg={}\n", result.mysql_errno, result.error_message));
                    auto err = encode_err_packet(result.mysql_errno, result.sql_state, result.error_message);
                    auto ep = encode_mysql_packet(err, seq++);
                    if (!send_all(client_fd, ep.data(), ep.size())) return;
                    break;
                }
                if (result.column_names.empty()) {
                    // DML:OK 包
                    auto ok = encode_ok_packet(result.affected_rows, result.last_insert_id,
                                               status_flags(), result.warnings);
                    auto op = encode_mysql_packet(ok, seq++);
                    dbg(fmt::format("  -> ok affected={} last_id={}\n",
                                    result.affected_rows, result.last_insert_id));
                    if (!send_all(client_fd, op.data(), op.size())) return;
                    break;
                }
                // binary resultset:列数 + 列定义 + EOF + 二进制行 + EOF
                {
                    PacketWriter cnt;
                    cnt.write_lenenc_int(result.column_names.size());
                    auto cp = encode_mysql_packet(cnt.buf, seq++);
                    if (!send_all(client_fd, cp.data(), cp.size())) return;
                    for (size_t i = 0; i < result.column_names.size(); ++i) {
                        auto type = i < result.column_types.size()
                                        ? result.column_types[i] : sqltiny::types::SqlType::Varchar;
                        auto d = encode_stmt_column_def(result.column_names[i], type);
                        auto p2 = encode_mysql_packet(d, seq++);
                        if (!send_all(client_fd, p2.data(), p2.size())) return;
                    }
                    auto eof1 = encode_mysql_packet(encode_eof_packet(0, status_flags()), seq++);
                    if (!send_all(client_fd, eof1.data(), eof1.size())) return;
                    for (auto& row : result.rows) {
                        auto rp = encode_mysql_packet(
                            encode_binary_row(row, result.column_types, status_flags()), seq++);
                        if (!send_all(client_fd, rp.data(), rp.size())) return;
                    }
                    auto eof2 = encode_mysql_packet(encode_eof_packet(0, status_flags()), seq++);
                    if (!send_all(client_fd, eof2.data(), eof2.size())) return;
                    dbg(fmt::format("  -> ok rows={} cols={}\n",
                                    result.rows.size(), result.column_names.size()));
                }
                break;
            }
            case COM_STMT_SEND_LONG_DATA:
                // 无响应(协议规定),长数据暂不支持
                break;
            case COM_STMT_CLOSE: {
                if (cmd_payload.size() >= 5) {
                    uint32_t sid = cmd_payload[1] | (cmd_payload[2] << 8) |
                                   (cmd_payload[3] << 16) | (cmd_payload[4] << 24);
                    prepared.erase(sid);
                }
                break;   // 无响应
            }
            case COM_STMT_RESET: {
                if (cmd_payload.size() >= 5) {
                    uint32_t sid = cmd_payload[1] | (cmd_payload[2] << 8) |
                                   (cmd_payload[3] << 16) | (cmd_payload[4] << 24);
                    auto itp = prepared.find(sid);
                    if (itp != prepared.end()) itp->second.param_types.clear();
                }
                auto r = encode_ok_packet(0, 0, status_flags(), 0);
                auto p = encode_mysql_packet(r, seq++);
                if (!send_all(client_fd, p.data(), p.size())) return;
                break;
            }
            case COM_STATISTICS: {
                // mysql_stat():响应是单个字符串包(不是 OK/ERR)
                std::string stats = "Uptime: 1  Threads: 1  Questions: 1  Slow queries: 0  Opens: 0  Flush tables: 1  Open tables: 0  Queries per second avg: 0.000";
                std::vector<uint8_t> payload;
                // lenenc string
                size_t n = stats.size();
                if (n < 251) payload.push_back(static_cast<uint8_t>(n));
                else { payload.push_back(0xfc); payload.push_back(n & 0xff); payload.push_back((n >> 8) & 0xff); }
                payload.insert(payload.end(), stats.begin(), stats.end());
                auto p = encode_mysql_packet(payload, seq++);
                dbg(fmt::format("  -> STATISTICS string ({} bytes)\n", stats.size()));
                if (!send_all(client_fd, p.data(), p.size())) return;
                break;
            }
            case COM_PROCESS_INFO: {
                // SHOW PROCESSLIST
                auto result = session.execute("SHOW PROCESSLIST");
                dbg("  -> SHOW PROCESSLIST result\n");
                if (!send_mysql_response(client_fd, result, seq, deprecate_eof, status_flags())) return;
                break;
            }
            case COM_QUERY: {
                std::string sql(reinterpret_cast<const char*>(cmd_payload.data() + 1), cmd_payload.size() - 1);
                auto result = session.execute(sql);
                if (result.ok)
                    dbg(fmt::format("[QUERY] {}\n  -> ok rows={} cols={}\n", sql,
                                    result.rows.size(), result.column_names.size()));
                else
                    dbg(fmt::format("[QUERY] {}\n  -> ERR errno={} state={} msg={}\n", sql,
                                    result.mysql_errno, result.sql_state, result.error_message));
                if (!send_mysql_response(client_fd, result, seq, deprecate_eof, status_flags())) return;
                break;
            }
            case COM_FIELD_LIST: {
                // DBX/dbExpress 等客户端取字段元数据:payload = "tbl\0wildcard"
                // (也可能带库名限定 "db.tbl")。截断到第一个 \0 再处理。
                std::string raw_name(reinterpret_cast<const char*>(cmd_payload.data() + 1), cmd_payload.size() - 1);
                auto nul = raw_name.find('\0');
                if (nul != std::string::npos) raw_name.resize(nul);
                std::string sql;
                auto dot = raw_name.find('.');
                if (dot != std::string::npos) {
                    // SHOW COLUMNS FROM tbl FROM db
                    sql = "SHOW COLUMNS FROM " + raw_name.substr(dot + 1) + " FROM " + raw_name.substr(0, dot);
                } else {
                    sql = "SHOW COLUMNS FROM " + raw_name;
                }
                auto result = session.execute(sql);
                dbg(fmt::format("[FIELD_LIST] {}\n  -> {}\n", sql,
                                result.ok ? fmt::format("ok cols={}", result.column_names.size())
                                          : fmt::format("ERR {}", result.error_message)));
                if (!send_mysql_response(client_fd, result, seq, deprecate_eof, status_flags())) return;
                break;
            }
            default: {
                fmt::print("[conn] unsupported COM=0x{:02x}\n", cmd);
                dbg(fmt::format("  -> ERR unsupported COM 0x{:02x}\n", cmd));
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
