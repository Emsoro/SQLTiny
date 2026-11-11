#include "storage/persist.h"
#include "storage/table.h"
#include "types/value.h"
#include "parser/ast.h"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

using namespace sqltiny;

static int g_pass = 0, g_fail = 0;
#define CHECK(name, cond) do { \
    if ((cond)) { printf("  %s ... PASS\n", name); ++g_pass; } \
    else { printf("  %s ... FAIL\n", name); ++g_fail; } \
} while (0)

// ---------- Value codec ----------
static void test_value_codec() {
    printf("=== Value codec round-trip ===\n");
    struct Case { types::Value v; };
    std::vector<Case> cases = {
        { types::Value::make_null() },
        { types::Value::make_bool(true) },
        { types::Value::make_bool(false) },
        { types::Value::make_int(0) },
        { types::Value::make_int(1234567890LL) },
        { types::Value::make_int(-9999LL) },
        { types::Value::make_double(3.14159) },
        { types::Value::make_double(-1e-300) },
        { types::Value::make_string("hello world") },
        { types::Value::make_string("") },
        { types::Value::make_string("中文测试") },
    };
    for (auto& c : cases) {
        auto enc = persist::codec::encode_value(c.v);
        auto buf = std::span<const uint8_t>(enc);
        auto dec = persist::codec::decode_value(buf);
        bool ok = dec.has_value() && dec->compare(c.v) == 0;
        CHECK(("Value kind=" + std::to_string((int)c.v.kind)).c_str(), ok);
    }
}

// ---------- Row codec ----------
static void test_row_codec() {
    printf("=== Row codec round-trip ===\n");
    storage::Row row = {
        types::Value::make_int(1),
        types::Value::make_string("Alice"),
        types::Value::make_null(),
        types::Value::make_double(99.5),
    };
    auto enc = persist::codec::encode_row(row);
    auto buf = std::span<const uint8_t>(enc);
    auto dec = persist::codec::decode_row(buf);
    bool ok = dec.has_value() && dec->size() == row.size();
    if (ok) {
        for (size_t i = 0; i < row.size(); ++i)
            if (dec->at(i).compare(row[i]) != 0) { ok = false; break; }
    }
    CHECK("Row 4-values round-trip", ok);
}

// ---------- TableDef codec ----------
static void test_tabledef_codec() {
    printf("=== TableDef codec round-trip ===\n");
    storage::TableDef def;
    def.name = "users";
    def.columns = {
        // id BIGINT PRIMARY KEY AUTO_INCREMENT
        [&]{ ast::ColumnDef c; c.name="id"; c.type.type=types::SqlType::BigInt; c.is_primary_key=true; c.is_auto_increment=true; return c; }(),
        // name VARCHAR(64) NOT NULL
        [&]{ ast::ColumnDef c; c.name="name"; c.type.type=types::SqlType::Varchar; c.type.length=64; c.nullable=false; return c; }(),
        // age INT DEFAULT 18
        [&]{ ast::ColumnDef c; c.name="age"; c.type.type=types::SqlType::Int;
             c.default_value = std::shared_ptr<ast::Expression>(ast::Expression::make_literal(types::Value::make_int(18)));
             return c; }(),
        // dept_id INT (nullable)
        [&]{ ast::ColumnDef c; c.name="dept_id"; c.type.type=types::SqlType::Int; return c; }(),
    };
    def.primary_key_cols = { {"id"} };

    auto enc = persist::codec::encode_table_def(def);
    auto buf = std::span<const uint8_t>(enc);
    auto dec = persist::codec::decode_table_def(buf);
    bool ok = dec.has_value()
        && dec->name == "users"
        && dec->columns.size() == 4
        && dec->columns[0].is_primary_key == true
        && dec->columns[0].is_auto_increment == true
        && dec->columns[1].nullable == false
        && dec->columns[1].type.length == 64
        && dec->columns[2].default_value != nullptr
        && dec->primary_key_cols.size() == 1
        && dec->primary_key_cols[0][0] == "id";
    CHECK("TableDef 4 columns PK DEFAULT round-trip", ok);
}

// ---------- WAL file round-trip ----------
static void test_wal_file() {
    printf("=== WalWriter + WalReader round-trip ===\n");
    const char* path = "test.wal";
    std::remove(path);

    // Writer 追加 4 条 record
    persist::WalWriter w(path);
    CHECK("WalWriter open", w.open());

    persist::WalRecord r1{ persist::RecordType::CreateDatabase, 0, {} };
    auto s1 = persist::codec::encode_string("shop");
    r1.payload.assign(s1.begin(), s1.end());
    CHECK("append CreateDatabase", w.append(r1));

    persist::WalRecord r2{ persist::RecordType::InsertRow, 0, {} };
    auto dbn = persist::codec::encode_string("shop");
    auto tbn = persist::codec::encode_string("users");
    auto rid = persist::codec::encode_u64(1);
    auto row = persist::codec::encode_row({types::Value::make_int(1), types::Value::make_string("Alice")});
    r2.payload.insert(r2.payload.end(), dbn.begin(), dbn.end());
    r2.payload.insert(r2.payload.end(), tbn.begin(), tbn.end());
    r2.payload.insert(r2.payload.end(), rid.begin(), rid.end());
    r2.payload.insert(r2.payload.end(), row.begin(), row.end());
    CHECK("append InsertRow", w.append(r2));

    persist::WalRecord r3{ persist::RecordType::AutoIncUpdate, 0, {} };
    auto dbn3 = persist::codec::encode_string("shop");
    auto tbn3 = persist::codec::encode_string("users");
    auto cur3 = persist::codec::encode_u64(100);
    r3.payload.insert(r3.payload.end(), dbn3.begin(), dbn3.end());
    r3.payload.insert(r3.payload.end(), tbn3.begin(), tbn3.end());
    r3.payload.insert(r3.payload.end(), cur3.begin(), cur3.end());
    CHECK("append AutoIncUpdate", w.append(r3));

    persist::WalRecord r4{ persist::RecordType::EndOfFile, 0, {} };
    CHECK("append EndOfFile", w.append(r4));

    w.close();

    // Reader 读回
    persist::WalReader rd(path);
    CHECK("WalReader open", rd.open());
    bool truncated = false;
    auto rec = rd.read_next(&truncated);
    CHECK("read #1 CreateDatabase",
          rec.has_value() && rec->type == persist::RecordType::CreateDatabase);

    rec = rd.read_next(&truncated);
    CHECK("read #2 InsertRow",
          rec.has_value() && rec->type == persist::RecordType::InsertRow && !truncated);

    rec = rd.read_next(&truncated);
    CHECK("read #3 AutoIncUpdate",
          rec.has_value() && rec->type == persist::RecordType::AutoIncUpdate);

    rec = rd.read_next(&truncated);
    CHECK("read #4 EndOfFile",
          rec.has_value() && rec->type == persist::RecordType::EndOfFile);

    rec = rd.read_next(&truncated);
    CHECK("read #5 EOF (nullopt)", !rec.has_value() && !truncated);

    rd.close();
    std::remove(path);
}

// ---------- CRC 损坏检测 ----------
static void test_crc_corruption() {
    printf("=== CRC corruption detection ===\n");
    const char* path = "corrupt.wal";
    std::remove(path);

    persist::WalWriter w(path); w.open();
    persist::WalRecord r1{ persist::RecordType::CreateDatabase, 0, {} };
    auto s1 = persist::codec::encode_string("shop");
    r1.payload.assign(s1.begin(), s1.end());
    w.append(r1);
    w.close();  // 必须 close 让数据落盘

    // 现在 reopen 文件,手工追加一条 CRC 故意错的 record
    FILE* fh = std::fopen(path, "ab");
    CHECK("fopen for append", fh != nullptr);
    uint8_t bad[10] = {
        (uint8_t)persist::RecordType::EndOfFile, 0,
        0x00, 0x00, 0x00, 0x00,  // len=0
        0xFF, 0xFF, 0xFF, 0xFF,  // CRC 故意错
    };
    std::fwrite(bad, 1, sizeof(bad), fh);
    std::fclose(fh);

    persist::WalReader rd(path);
    CHECK("WalReader open (CRC corrupt)", rd.open());
    bool truncated = false;
    auto r = rd.read_next(&truncated);
    bool got_first = r.has_value() && !truncated;
    auto r2 = rd.read_next(&truncated);
    bool hit_bad = !r2.has_value() && truncated;
    CHECK("first record clean", got_first);
    CHECK("second record CRC bad → truncated=true", hit_bad);

    rd.close();
    std::remove(path);
}

int main() {
    printf("=== SQLTiny M5 WAL Smoke Tests ===\n\n");
    test_value_codec();
    printf("\n");
    test_row_codec();
    printf("\n");
    test_tabledef_codec();
    printf("\n");
    test_wal_file();
    printf("\n");
    test_crc_corruption();
    printf("\nAll WAL smoke tests %s (%d/%d)\n", g_fail == 0 ? "PASSED" : "FAILED", g_pass, g_pass+g_fail);
    return g_fail == 0 ? 0 : 1;
}
