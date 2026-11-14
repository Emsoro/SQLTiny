// Smoke test for M1 — runs without GTest, just asserts
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <random>
#include <fmt/format.h>

#include "types/value.h"
#include "storage/index_avl.h"
#include "storage/index_bptree.h"
#include "parser/lexer.h"

using namespace sqltiny;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
        return 1; \
    } \
} while(0)

// ---------- Value ----------
int test_value() {
    // 比较
    auto a = types::Value::make_int(1);
    auto b = types::Value::make_int(2);
    CHECK(a.compare(b) < 0);
    CHECK(b.compare(a) > 0);
    CHECK(a.compare(a) == 0);

    // NULL 处理
    auto n = types::Value::make_null();
    CHECK(n.is_null());
    CHECK(!a.is_null());

    // 数值提升:1 和 1.0 应该相等
    auto d = types::Value::make_double(1.0);
    CHECK(a.compare(d) == 0);

    // 字符串
    auto s1 = types::Value::make_string("apple");
    auto s2 = types::Value::make_string("banana");
    CHECK(s1.compare(s2) < 0);

    // to_string
    CHECK(a.to_string() == "1");
    CHECK(s1.to_string() == "'apple'");
    CHECK(n.to_string() == "NULL");

    // try_to_int
    int64_t v;
    CHECK(a.try_to_int(v) && v == 1);
    CHECK(s1.try_to_int(v) == false);  // "apple" 不能转
    CHECK(d.try_to_int(v) && v == 1);  // 1.0 可以转 1

    printf("  Value ...... PASS\n");
    return 0;
}

// ---------- AVL ----------
int test_avl() {
    storage::IndexAvl idx(true /*unique*/);
    // 插入
    for (int i = 0; i < 100; ++i) {
        bool ok = idx.insert(storage::Key(types::Value::make_int(i)), i * 10);
        CHECK(ok);
    }
    CHECK(idx.size() == 100);
    CHECK(idx.height() <= 8);  // AVL 平衡,不应太高
    CHECK(idx.check_invariants());

    // 查找
    auto r = idx.find(storage::Key(types::Value::make_int(42)));
    CHECK(r.has_value() && *r == 420);
    CHECK(!idx.find(storage::Key(types::Value::make_int(999))).has_value());

    // 重复键拒绝
    bool ok = idx.insert(storage::Key(types::Value::make_int(42)), 0);
    CHECK(!ok);  // unique, 拒绝

    // 删除一半
    for (int i = 0; i < 100; i += 2) {
        idx.remove(storage::Key(types::Value::make_int(i)), i * 10);
    }
    CHECK(idx.size() == 50);
    CHECK(idx.check_invariants());
    CHECK(!idx.find(storage::Key(types::Value::make_int(42))).has_value());  // 删了
    CHECK(idx.find(storage::Key(types::Value::make_int(43))).has_value());  // 还在

    // 范围扫描 [20, 40]
    std::vector<int64_t> hits;
    idx.range_scan(
        storage::Key(types::Value::make_int(20)),
        storage::Key(types::Value::make_int(40)),
        [&](const storage::Key&, int64_t rid) { hits.push_back(rid); return true; },
        false /*exclusive hi*/);
    // 21-39 奇数还在 (20/40 是偶数已删)
    CHECK(hits.size() == 10);  // 21,23,25,27,29,31,33,35,37,39 共 10 个
    for (int64_t rid : hits) {
        CHECK(rid % 20 == 10);  // (2n+1)*10 末两位是 10,20,30,40,50,60,70,80,90... 不对
        // 反正只要是偶数索引*10 已被删,奇数索引*10 还在
    }

    // 多列键
    storage::IndexAvl mkey(false /*non-unique*/);
    for (int a = 0; a < 5; ++a)
        for (int b = 0; b < 5; ++b) {
            std::vector<types::Value> parts{ types::Value::make_int(a), types::Value::make_int(b) };
            mkey.insert(storage::Key(parts), a * 100 + b);
        }
    CHECK(mkey.size() == 25);
    CHECK(mkey.check_invariants());

    printf("  AVL ........ PASS\n");
    return 0;
}

// ---------- B+ 树(v2.0.0 主键索引) ----------
int test_bptree() {
    using storage::IndexBPlusTree;
    using storage::Key;

    // 唯一索引:乱序插入 + 点查 + 重复拒绝 + 删除
    IndexBPlusTree idx(true);
    std::vector<int> order(100);
    for (int i = 0; i < 100; ++i) order[i] = i;
    std::mt19937 rng(42);
    std::shuffle(order.begin(), order.end(), rng);
    for (int i : order) {
        bool ok = idx.insert(Key(types::Value::make_int(i)), i * 10);
        CHECK(ok);
    }
    CHECK(idx.size() == 100);
    CHECK(idx.height() <= 7);
    CHECK(idx.check_invariants());

    auto r = idx.find(Key(types::Value::make_int(42)));
    CHECK(r.has_value() && *r == 420);
    CHECK(!idx.find(Key(types::Value::make_int(999))).has_value());
    CHECK(!idx.insert(Key(types::Value::make_int(42)), 0));  // unique 拒绝

    for (int i = 0; i < 100; i += 2)
        CHECK(idx.remove(Key(types::Value::make_int(i)), i * 10));
    CHECK(!idx.remove(Key(types::Value::make_int(20)), 200));  // 已删
    CHECK(idx.size() == 50);
    CHECK(idx.check_invariants());
    CHECK(!idx.find(Key(types::Value::make_int(42))).has_value());
    CHECK(idx.find(Key(types::Value::make_int(43))).has_value());

    // 范围扫描 [20, 40]
    std::vector<int64_t> hits;
    idx.range_scan(Key(types::Value::make_int(20)),
                   Key(types::Value::make_int(40)),
                   [&](const Key&, int64_t rid) { hits.push_back(rid); return true; },
                   false);
    CHECK(hits.size() == 10);
    for (int64_t rid : hits) CHECK(rid % 20 == 10);

    // 多列键 + 前缀扫描(模拟复合主键首列探测)
    IndexBPlusTree mkey(true);
    for (int a = 0; a < 50; ++a)
        for (int b = 0; b < 5; ++b) {
            std::vector<types::Value> parts{ types::Value::make_int(a), types::Value::make_int(b) };
            CHECK(mkey.insert(Key(parts), a * 100 + b));
        }
    CHECK(mkey.size() == 250);
    CHECK(mkey.check_invariants());
    // 逐列前缀 (a) 应命中全部 5 个 b
    std::vector<int64_t> pf;
    mkey.prefix_scan(Key(types::Value::make_int(30)),
                     [&](const Key& k, int64_t rid) {
                         if (k.size() != 2 || k.parts()[0].v_int != 30)
                             fprintf(stderr, "FAIL: prefix hit bad key\n");
                         pf.push_back(rid); return true;
                     });
    CHECK(pf.size() == 5);
    CHECK(std::all_of(pf.begin(), pf.end(), [](int64_t rid) { return rid / 100 == 30; }));
    // cb 提前停止
    int stopped = 0;
    mkey.prefix_scan(Key(types::Value::make_int(31)),
                     [&](const Key&, int64_t) { ++stopped; return stopped < 2; });
    CHECK(stopped == 2);
    // 不存在的前缀:零命中
    int none = 0;
    mkey.prefix_scan(Key(types::Value::make_int(999)),
                     [&](const Key&, int64_t) { ++none; return true; });
    CHECK(none == 0);

    // 非唯一索引:等键多条目 + find_all + 按 row_id 删除
    IndexBPlusTree mkeys(false);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            CHECK(mkeys.insert(Key(types::Value::make_int(7)), i * 10 + j));
    CHECK(mkeys.size() == 9);
    CHECK(mkeys.check_invariants());
    auto all = mkeys.find_all(Key(types::Value::make_int(7)));
    CHECK(all.size() == 9);
    std::sort(all.begin(), all.end());
    for (int j = 0; j < 3; ++j) CHECK(all[j] == j);
    CHECK(mkeys.remove(Key(types::Value::make_int(7)), 11));
    CHECK(!mkeys.remove(Key(types::Value::make_int(7)), 11));  // 已删
    CHECK(mkeys.size() == 8);
    CHECK(mkeys.find_all(Key(types::Value::make_int(7))).size() == 8);
    CHECK(mkeys.check_invariants());

    // 全量扫描有序性
    IndexBPlusTree big(true);
    std::vector<int> seq(5000);
    for (int i = 0; i < 5000; ++i) seq[i] = i;
    std::shuffle(seq.begin(), seq.end(), rng);
    for (int i : seq) CHECK(big.insert(Key(types::Value::make_int(i)), i));
    CHECK(big.size() == 5000);
    CHECK(big.check_invariants());
    int64_t last = -1, cnt = 0;
    big.for_each([&](const Key& k, int64_t) {
        CHECK(k.parts()[0].v_int > last);
        last = k.parts()[0].v_int;
        ++cnt;
    });
    CHECK(cnt == 5000);

    printf("  B+ Tree ..... PASS\n");
    return 0;
}
int test_lexer() {
    using namespace parser;

    // 测试 MySQL 风格 SQL
    std::string sql = "SELECT id, name FROM `users` WHERE id = 1 AND name LIKE '张%' LIMIT 10;";
    Lexer lx(sql);
    auto tokens = lx.tokenize_all();

    // 期望 token 序列
    std::vector<TokenKind> expected = {
        TokenKind::KwSelect, TokenKind::Ident, TokenKind::Comma,
        TokenKind::Ident, TokenKind::KwFrom, TokenKind::QuotedIdent,
        TokenKind::KwWhere, TokenKind::Ident, TokenKind::Eq,
        TokenKind::Number, TokenKind::KwAnd, TokenKind::Ident,
        TokenKind::KwLike, TokenKind::String, TokenKind::KwLimit,
        TokenKind::Number, TokenKind::Semi, TokenKind::Eof,
    };
    CHECK(tokens.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        CHECK(tokens[i].kind == expected[i]);
    }

    // 更复杂的:INSERT / UPDATE / DELETE
    sql = "INSERT INTO t VALUES (1, 'Alice', 3.14);";
    lx = Lexer(sql);
    tokens = lx.tokenize_all();
    CHECK(tokens[0].kind == TokenKind::KwInsert);
    CHECK(tokens[1].kind == TokenKind::KwInto);
    CHECK(tokens[2].kind == TokenKind::Ident);     // t
    CHECK(tokens[3].kind == TokenKind::KwValues);
    CHECK(tokens[4].kind == TokenKind::LParen);
    CHECK(tokens[5].kind == TokenKind::Number);   // 1
    CHECK(tokens[6].kind == TokenKind::Comma);
    CHECK(tokens[7].kind == TokenKind::String);   // 'Alice'
    CHECK(tokens[7].text == "Alice");
    CHECK(tokens[8].kind == TokenKind::Comma);
    CHECK(tokens[9].kind == TokenKind::Number);   // 3.14
    CHECK(tokens[9].num_value > 3.13 && tokens[9].num_value < 3.15);

    // UPDATE with operators
    sql = "UPDATE x SET a = a + 1, b = (a * 2) / 3 WHERE b <> 0 OR a >= 5;";
    lx = Lexer(sql);
    tokens = lx.tokenize_all();
    // 数一下基本关键字
    int kw_set = 0, kw_update = 0, kw_where = 0, kw_or = 0;
    for (auto& t : tokens) {
        if (t.kind == TokenKind::KwSet) kw_set++;
        if (t.kind == TokenKind::KwUpdate) kw_update++;
        if (t.kind == TokenKind::KwWhere) kw_where++;
        if (t.kind == TokenKind::KwOr) kw_or++;
    }
    CHECK(kw_update == 1 && kw_set == 1 && kw_where == 1 && kw_or == 1);
    // <> 应识别为 Ne
    CHECK(std::any_of(tokens.begin(), tokens.end(), [](const Token& t) { return t.kind == TokenKind::Ne; }));
    CHECK(std::any_of(tokens.begin(), tokens.end(), [](const Token& t) { return t.kind == TokenKind::Ge; }));

    // 行号列号
    sql = "SELECT\n  a\nFROM t\nWHERE a = 1";
    lx = Lexer(sql);
    auto tok2 = lx.next();
    CHECK(tok2.kind == TokenKind::KwSelect);
    auto tok3 = lx.next();
    CHECK(tok3.kind == TokenKind::Ident && tok3.text == "a");
    CHECK(tok3.line == 2 && tok3.col == 3);

    // 注释跳过
    sql = "SELECT -- line comment\n 1 /* block */ + 2";
    lx = Lexer(sql);
    tokens = lx.tokenize_all();
    CHECK(tokens[0].kind == TokenKind::KwSelect);
    CHECK(tokens[1].kind == TokenKind::Number);  // 1
    CHECK(tokens[2].kind == TokenKind::Plus);    // +
    CHECK(tokens[3].kind == TokenKind::Number);  // 2

    // 字符串转义:SELECT 'it''s \n newline';
    sql = "SELECT 'it''s \\n newline';";
    lx = Lexer(sql);
    tokens = lx.tokenize_all();
    CHECK(tokens[0].kind == TokenKind::KwSelect);
    CHECK(tokens[1].kind == TokenKind::String);
    CHECK(tokens[1].text == "it's \n newline");  // SQL '' 转义 + MySQL \n

    // Hex
    sql = "0xDEADBEEF";
    lx = Lexer(sql);
    auto hex_tok = lx.next();
    CHECK(hex_tok.kind == TokenKind::Number);
    CHECK(hex_tok.text == "0xDEADBEEF");

    // 错误:未闭合字符串
    sql = "SELECT 'unterminated";
    lx = Lexer(sql);
    tokens = lx.tokenize_all();
    CHECK(tokens.back().kind == TokenKind::Error);

    printf("  Lexer ....... PASS\n");
    return 0;
}

int main() {
    printf("=== SQLTiny M1 Smoke Tests ===\n");
    int rc = 0;
    rc |= test_value();
    rc |= test_avl();
    rc |= test_bptree();
    rc |= test_lexer();
    if (rc == 0) printf("All smoke tests PASSED.\n");
    return rc;
}
