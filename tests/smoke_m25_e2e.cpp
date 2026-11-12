// M2.5 E2E: MySQL wire protocol 客户端测试 ( Winsock )
// 先启动: sqltiny_server.exe --port 3306 --user root
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <optional>

#pragma comment(lib, "ws2_32.lib")

static std::vector<uint8_t> recv_pkt(SOCKET s, uint8_t& seq) {
    uint8_t hdr[4];
    int n = recv(s, (char*)hdr, 4, 0);
    if (n < 4) return {};
    uint32_t plen = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16);
    seq = hdr[3];
    std::vector<uint8_t> buf(plen);
    uint32_t got = 0;
    while (got < plen) {
        int r = recv(s, (char*)buf.data() + got, plen - got, 0);
        if (r <= 0) return {};
        got += r;
    }
    return buf;
}

static bool send_pkt(SOCKET s, const uint8_t* data, size_t len, uint8_t seq) {
    uint8_t hdr[4] = { (uint8_t)(len & 0xff), (uint8_t)((len >> 8) & 0xff), (uint8_t)((len >> 16) & 0xff), seq };
    if (send(s, (char*)hdr, 4, 0) != 4) return false;
    return send(s, (char*)data, (int)len, 0) == (int)len;
}

static uint64_t parse_lenenc(const uint8_t*& p, const uint8_t* end) {
    if (p >= end) return 0;
    uint8_t f = *p++;
    if (f < 0xfb) return f;
    if (f == 0xfb) return 0;
    if (f == 0xfc && p + 2 <= end) { uint16_t v = p[0] | (p[1] << 8); p += 2; return v; }
    if (f == 0xfd && p + 3 <= end) { uint32_t v = p[0] | (p[1] << 8) | (p[2] << 16); p += 3; return v; }
    return 0;
}

int main() {
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET; addr.sin_port = htons(3306);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    printf("=== SQLTiny M2.5 E2E (mysql wire protocol) ===\n");
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "FAIL: connect (server not running?)\n");
        closesocket(s); WSACleanup(); return 1;
    }
    printf("  connect ........ OK\n");

    uint8_t seq = 0;
    auto greeting = recv_pkt(s, seq);
    if (greeting.empty()) { fprintf(stderr, "FAIL: no greeting\n"); closesocket(s); WSACleanup(); return 1; }
    printf("  handshake ....... OK\n");

    std::vector<uint8_t> hs;
    uint32_t caps = 0x00000200 | 0x00008000 | 0x00080000 | 0x01000000;
    auto wr_u32 = [&](uint32_t v){ hs.push_back(v&0xff); hs.push_back((v>>8)&0xff); hs.push_back((v>>16)&0xff); hs.push_back((v>>24)&0xff); };
    wr_u32(caps); wr_u32(0xffffff); hs.push_back(33);
    hs.insert(hs.end(), 23, 0);
    const char* user = "root";
    hs.insert(hs.end(), user, user + strlen(user)); hs.push_back(0);
    hs.push_back(0xfb);
    const char* plugin = "mysql_native_password";
    hs.insert(hs.end(), plugin, plugin + strlen(plugin)); hs.push_back(0);
    send_pkt(s, hs.data(), hs.size(), ++seq);
    auto auth_resp = recv_pkt(s, seq);
    if (auth_resp.empty() || auth_resp[0] == 0xff) { fprintf(stderr, "FAIL: auth\n"); closesocket(s); WSACleanup(); return 1; }
    printf("  auth user=root .. OK\n");

    auto query = [&](const char* sql) -> std::optional<std::vector<std::vector<std::string>>> {
        std::vector<uint8_t> pkt; pkt.push_back(0x03);
        pkt.insert(pkt.end(), sql, sql + strlen(sql));
        send_pkt(s, pkt.data(), pkt.size(), ++seq);

        auto first = recv_pkt(s, seq);
        if (first.empty()) return std::nullopt;
        if (first[0] == 0xff) return std::nullopt;
        if (first[0] == 0x00) return std::vector<std::vector<std::string>>{};

        const uint8_t* p = first.data(); const uint8_t* end = first.data() + first.size();
        uint64_t col_count = parse_lenenc(p, end);
        for (uint64_t i = 0; i < col_count; ++i) recv_pkt(s, seq);

        std::vector<std::vector<std::string>> rows;
        auto decode_row = [&](const std::vector<uint8_t>& rpkt) {
            const uint8_t* rp = rpkt.data(); const uint8_t* rq = rpkt.data() + rpkt.size();
            std::vector<std::string> row;
            for (uint64_t i = 0; i < col_count; ++i) {
                if (rp >= rq) break;
                if (*rp == 0xfb) { row.push_back("NULL"); rp++; }
                else { uint64_t n = parse_lenenc(rp, rq); row.push_back(std::string(rp, rp + n)); rp += n; }
            }
            rows.push_back(std::move(row));
        };

        // v1.1.0: 服务端不再广播 CLIENT_DEPRECATE_EOF, 列定义之后会多一个经典 EOF 帧
        // (0xfe + warnings + status, 包长 < 9); 必须跳过它再读行数据。
        auto after_cols = recv_pkt(s, seq);
        if (after_cols.empty() || after_cols[0] == 0xff || after_cols[0] == 0x00) return rows;
        bool classic_eof = after_cols[0] == 0xfe && after_cols.size() < 9;
        if (!classic_eof) decode_row(after_cols);   // 无 EOF 帧时该包就是第一行

        while (true) {
            auto rpkt = recv_pkt(s, seq);
            if (rpkt.empty() || rpkt[0] == 0xff || rpkt[0] == 0x00 || rpkt[0] == 0xfe) break;
            decode_row(rpkt);
        }
        return rows;
    };

    #define RUN_OK(sql) do { if (!query(sql)) { fprintf(stderr, "FAIL at: %s\n", sql); closesocket(s); WSACleanup(); return 1; } } while(0)

    RUN_OK("DROP TABLE IF EXISTS e2e_t");
    printf("  DROP TABLE ...... OK\n");
    RUN_OK("CREATE TABLE e2e_t(id INT PRIMARY KEY, val INT)");
    printf("  CREATE TABLE .... OK\n");
    RUN_OK("INSERT INTO e2e_t VALUES (1, 100), (2, 200), (3, 300)");
    printf("  INSERT .......... OK\n");

    auto sel = query("SELECT id, val FROM e2e_t ORDER BY id");
    if (!sel || sel->size() != 3 || (*sel)[0][0] != "1" || (*sel)[0][1] != "100") { fprintf(stderr, "FAIL: SELECT\n"); closesocket(s); WSACleanup(); return 1; }
    printf("  SELECT ORDER BY . OK\n");

    auto cnt = query("SELECT COUNT(*) FROM e2e_t");
    if (!cnt || cnt->at(0).at(0) != "3") { fprintf(stderr, "FAIL: COUNT\n"); closesocket(s); WSACleanup(); return 1; }
    printf("  COUNT(*) ........ OK (=3)\n");

    auto sum = query("SELECT SUM(val) FROM e2e_t");
    if (!sum || sum->at(0).at(0) != "600") { fprintf(stderr, "FAIL: SUM\n"); closesocket(s); WSACleanup(); return 1; }
    printf("  SUM(val) ........ OK (=600)\n");

    auto wh = query("SELECT val FROM e2e_t WHERE id > 1");
    if (!wh || wh->size() != 2) { fprintf(stderr, "FAIL: WHERE\n"); closesocket(s); WSACleanup(); return 1; }
    printf("  WHERE id>1 ...... OK\n");

    RUN_OK("UPDATE e2e_t SET val = 999 WHERE id = 1");
    printf("  UPDATE .......... OK\n");

    RUN_OK("DELETE FROM e2e_t WHERE id = 3");
    printf("  DELETE .......... OK\n");

    auto chk = query("SELECT val FROM e2e_t ORDER BY id");
    if (!chk || chk->size() != 2 || (*chk)[0][0] != "999" || (*chk)[1][0] != "200") {
        fprintf(stderr, "FAIL: UPDATE+DELETE check\n"); closesocket(s); WSACleanup(); return 1;
    }
    printf("  UPDATE+DELETE ... OK\n");

    auto show = query("SHOW TABLES");
    if (!show || show->empty()) { fprintf(stderr, "FAIL: SHOW TABLES\n"); closesocket(s); WSACleanup(); return 1; }
    printf("  SHOW TABLES ..... OK\n");

    RUN_OK("DROP TABLE e2e_t");
    printf("  DROP TABLE ...... OK\n");

    uint8_t quit = 0x01; send_pkt(s, &quit, 1, ++seq);
    closesocket(s); WSACleanup();
    printf("=== All M2.5 E2E tests PASSED ===\n");
    return 0;
}
