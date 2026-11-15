// 常用 SQL 关键词 / 词法层基本用例 —— 无 GTest 依赖,直接断言
//
// 覆盖:
//   1. 关键字表分组清单(SQL92 + MySQL 常用词),逐个校验 lookup_keyword 与 Lexer
//   2. 大小写不敏感(全大写 / 全小写 / 混合大小写都必须是同一个 TokenKind)
//   3. 非关键字标识符(含下划线、$、中文)不能被误判成关键字
//   4. 运算符 / 标点 / 字面量 / 引号标识符 / 占位符 / 系统变量
//   5. 三类注释、行列号、词法错误
//   6. Parser 消费关键字:大小写混合语句、多语句、CASE、错误语句抛 ParseError
//
// 说明:词法层"认识"某个词不等于执行层支持该语法,执行层覆盖见 smoke_sql_syntax.cpp。

#include <cstdio>
#include <cctype>
#include <string>
#include <vector>
#include <algorithm>

#include "parser/lexer.h"
#include "parser/parser.h"
#include "parser/ast.h"

using namespace sqltiny;

static int g_pass = 0, g_fail = 0;

static void check(bool cond, const std::string& label, const std::string& detail = "") {
    if (cond) { ++g_pass; }
    else {
        ++g_fail;
        printf("  [FAIL] %s %s\n", label.c_str(), detail.c_str());
    }
}

static bool is_keyword(parser::TokenKind k) {
    return static_cast<int>(k) > static_cast<int>(parser::TokenKind::Kw_START) &&
           static_cast<int>(k) < static_cast<int>(parser::TokenKind::Kw_END);
}

static std::string upper_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
static std::string lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
/// SeLeCt 风格:奇偶位交替大小写
static std::string mixed_copy(const std::string& s) {
    std::string o = s;
    for (size_t i = 0; i < o.size(); ++i)
        o[i] = (i % 2 == 0) ? static_cast<char>(std::toupper(static_cast<unsigned char>(o[i])))
                            : static_cast<char>(std::tolower(static_cast<unsigned char>(o[i])));
    return o;
}

struct KwGroup { const char* name; std::vector<std::string> words; };

/// 常用关键词清单:新增词法能力时同步往这里加,保证「清单 == 实际可识别集合」有回归保护
static const std::vector<KwGroup> k_groups = {
    {"DML / 查询子句", {
        "SELECT", "FROM", "WHERE", "AND", "OR", "NOT", "IS", "NULL", "TRUE", "FALSE",
        "IN", "LIKE", "BETWEEN", "AS", "ON", "GROUP", "BY", "HAVING", "ORDER", "ASC", "DESC",
        "LIMIT", "OFFSET", "DISTINCT", "ALL", "INSERT", "INTO", "VALUES", "VALUE", "DEFAULT",
        "UPDATE", "SET", "DELETE", "TRUNCATE", "REPLACE", "IGNORE", "DUPLICATE", "USING",
    }},
    {"集合 / 连接", {
        "UNION", "INTERSECT", "EXCEPT", "JOIN", "INNER", "LEFT", "RIGHT", "OUTER",
        "FULL", "CROSS", "NATURAL",
    }},
    {"DDL / 约束", {
        "CREATE", "TABLE", "DATABASE", "SCHEMA", "DROP", "ALTER", "ADD", "COLUMN",
        "PRIMARY", "KEY", "KEYS", "UNIQUE", "FOREIGN", "REFERENCES", "INDEX", "CONSTRAINT",
        "CHECK", "AUTO_INCREMENT", "AUTOINCREMENT", "COMMENT", "TEMPORARY", "IF", "EXISTS",
        "COLLATE", "ENGINE", "COLLATION", "CHARSET", "CHARACTER", "UNSIGNED", "SIGNED",
        "ZEROFILL", "CASCADE", "RESTRICT", "ACTION", "LOCK", "UNLOCK",
    }},
    {"数据类型", {
        "INT", "INTEGER", "TINYINT", "SMALLINT", "BIGINT", "DECIMAL", "NUMERIC",
        "FLOAT", "DOUBLE", "REAL", "CHAR", "VARCHAR", "TEXT", "BLOB", "DATE", "TIME",
        "DATETIME", "TIMESTAMP", "BOOLEAN", "JSON", "ENUM",
    }},
    {"事务 / 会话", {
        "BEGIN", "COMMIT", "ROLLBACK", "SAVEPOINT", "RELEASE", "TRANSACTION", "WORK",
        "START", "TO", "ISOLATION", "LEVEL", "READ", "WRITE", "COMMITTED", "UNCOMMITTED",
        "REPEATABLE", "SERIALIZABLE", "AUTOCOMMIT", "NAMES", "GLOBAL", "SESSION",
    }},
    {"SHOW / 元数据", {
        "SHOW", "TABLES", "DATABASES", "SCHEMAS", "COLUMNS", "FIELDS", "STATUS",
        "VARIABLES", "ENGINES", "PLUGINS", "DUAL", "PROCESSLIST", "WARNINGS", "ERRORS",
        "USE", "DESCRIBE", "EXPLAIN", "TRIGGERS",
    }},
    {"表达式 / 函数", {
        "CASE", "WHEN", "THEN", "ELSE", "END", "CAST", "CONVERT", "ESCAPE",
        "CURRENT_TIMESTAMP", "NOW", "CURRENT_DATE", "CURRENT_TIME", "USER",
        "COUNT", "SUM", "AVG", "MIN", "MAX", "CONCAT", "COALESCE", "IFNULL",
        "ROUND", "ABS", "LENGTH",
    }},
    {"运算符类 / 其它补充词", {
        "REGEXP", "RLIKE", "XOR", "DIV", "MOD", "WITH", "RECURSIVE", "VIEW",
        "TRIGGER", "OUT", "OUTFILE",
    }},
};

static void test_keyword_table() {
    size_t total = 0;
    for (const auto& g : k_groups) {
        int group_fail = g_fail;
        for (const auto& w : g.words) {
            const auto up = parser::lookup_keyword(upper_copy(w));
            const auto lo = parser::lookup_keyword(lower_copy(w));
            const auto mx = parser::lookup_keyword(mixed_copy(w));
            check(is_keyword(up), "keyword " + w, "lookup 返回 Ident");
            check(up == lo && up == mx, "keyword case " + w,
                  "upper/lower/mixed 得到不同 TokenKind");

            // Lexer 必须保留原始文本,并给出与 lookup_keyword 一致的 kind
            // 注意:Lexer 持有 string_view,源字符串必须是有名字的左值(不能用临时量)
            std::string mx_text = mixed_copy(w);
            parser::Lexer lx(mx_text);
            auto t = lx.next();
            check(t.kind == up && t.text == mx_text, "lexer keyword " + w,
                  "kind 或 text 不一致");
            ++total;
        }
        if (g_fail == group_fail)
            printf("  [PASS] %-22s %zu 词 × 4 断言\n", g.name, g.words.size());
    }
    printf("  -> 关键词清单共 %zu 词\n", total);
}

static void test_non_keywords() {
    // 普通标识符、关键字前缀/后缀、下划线、$、非 ASCII 字符都必须是 Ident
    static const char* idents[] = {
        "users", "user_id", "select_all", "order_no", "wherever", "counted",
        "_id", "_select", "$var", "t1", "v2_col", "\xE5\xA7\x93\xE5\x90\x8D",
    };
    for (const char* s : idents) {
        check(parser::lookup_keyword(s) == parser::TokenKind::Ident,
              std::string("non-keyword ") + s, "被误判为关键字");
        parser::Lexer lx(s);
        auto t = lx.next();
        check(t.kind == parser::TokenKind::Ident && t.text == s,
              std::string("lexer ident ") + s, "kind/text 不符");
    }
    printf("  [PASS] 非关键字标识符 %zu 个\n", sizeof(idents) / sizeof(idents[0]));
}

static void test_operators() {
    using parser::TokenKind;
    struct Op { const char* text; TokenKind kind; };
    static const Op ops[] = {
        {"+", TokenKind::Plus},     {"-", TokenKind::Minus},   {"*", TokenKind::Star},
        {"/", TokenKind::Slash},    {"%", TokenKind::Percent}, {"=", TokenKind::Eq},
        {"<", TokenKind::Lt},       {">", TokenKind::Gt},      {"<=", TokenKind::Le},
        {">=", TokenKind::Ge},      {"<>", TokenKind::Ne},     {"!=", TokenKind::Ne},
        {":=", TokenKind::Assign},  {"||", TokenKind::DoublePipe},
        {".", TokenKind::Dot},      {",", TokenKind::Comma},   {";", TokenKind::Semi},
        {"(", TokenKind::LParen},   {")", TokenKind::RParen},
        {"[", TokenKind::LBracket}, {"]", TokenKind::RBracket}, {":", TokenKind::Colon},
        {"&", TokenKind::BitAnd},   {"|", TokenKind::BitOr},   {"^", TokenKind::BitXor},
        {"~", TokenKind::BitNot},   {"!", TokenKind::LogNot},  {"?", TokenKind::Placeholder},
    };
    for (const auto& op : ops) {
        parser::Lexer lx(op.text);
        auto t = lx.next();
        check(t.kind == op.kind, std::string("operator ") + op.text, "TokenKind 不符");
        check(lx.next().kind == TokenKind::Eof, std::string("operator eof ") + op.text,
              "多余 token");
    }
    // 多字符运算符不能被拆开
    parser::Lexer lx("a<=b AND c!=d");
    auto ts = lx.tokenize_all();
    check(std::count_if(ts.begin(), ts.end(), [](const parser::Token& t) {
              return t.kind == TokenKind::Le; }) == 1 &&
          std::count_if(ts.begin(), ts.end(), [](const parser::Token& t) {
              return t.kind == TokenKind::Ne; }) == 1,
          "a<=b AND c!=d 复合运算符", "被拆成单字符");
    printf("  [PASS] 运算符 / 标点 %zu 个\n", sizeof(ops) / sizeof(ops[0]));
}

static void test_literals() {
    using parser::TokenKind;
    {
        parser::Lexer lx("123"); auto t = lx.next();
        check(t.kind == TokenKind::Number && t.int_value == 123, "整数 123");
    }
    {
        parser::Lexer lx("3.14"); auto t = lx.next();
        check(t.kind == TokenKind::Number && t.num_value > 3.13 && t.num_value < 3.15,
              "浮点数 3.14");
    }
    {
        parser::Lexer lx("1.5e2"); auto t = lx.next();
        check(t.kind == TokenKind::Number && t.num_value > 149.9 && t.num_value < 150.1,
              "科学计数 1.5e2");
    }
    {
        parser::Lexer lx("0xDEADBEEF"); auto t = lx.next();
        check(t.kind == TokenKind::Number && t.text == "0xDEADBEEF", "十六进制 0xDEADBEEF");
    }
    {
        parser::Lexer lx("'it''s'"); auto t = lx.next();
        check(t.kind == TokenKind::String && t.text == "it's", "SQL '' 转义");
    }
    {
        parser::Lexer lx("'a\\nb\\t\\\\'"); auto t = lx.next();
        check(t.kind == TokenKind::String && t.text == "a\nb\t\\", "MySQL 反斜杠转义");
    }
    {
        parser::Lexer lx("\"alias\""); auto t = lx.next();
        check(t.kind == TokenKind::String && t.text == "alias", "双引号字符串/别名");
    }
    {
        parser::Lexer lx("`order`"); auto t = lx.next();
        check(t.kind == TokenKind::QuotedIdent && t.text == "order", "反引号标识符");
    }
    {
        parser::Lexer lx("`a``b`"); auto t = lx.next();
        check(t.kind == TokenKind::QuotedIdent && t.text == "a`b", "反引号内转义");
    }
    {
        parser::Lexer lx("@@tx_isolation @user_name ?"); auto ts = lx.tokenize_all();
        check(ts.size() == 4 && ts[0].kind == TokenKind::Ident && ts[0].text == "@@tx_isolation" &&
              ts[1].kind == TokenKind::Ident && ts[1].text == "@user_name" &&
              ts[2].kind == TokenKind::Placeholder,
              "系统变量 / 用户变量 / 占位符");
    }
    printf("  [PASS] 字面量 / 引号 / 变量\n");
}

static void test_comments_position_errors() {
    using parser::TokenKind;
    // -- / # / /* */ 三类注释
    parser::Lexer lx("SELECT -- line\n 1 # hash\n /* block */ + 2");
    auto ts = lx.tokenize_all();
    check(ts.size() == 5 && ts[0].kind == TokenKind::KwSelect &&
          ts[1].kind == TokenKind::Number && ts[2].kind == TokenKind::Plus &&
          ts[3].kind == TokenKind::Number && ts[4].kind == TokenKind::Eof,
          "三类注释跳过", "token 序列不符");

    // 行列号
    parser::Lexer l2("SELECT\n  a\nFROM t");
    auto t0 = l2.next();
    auto t1 = l2.next();
    check(t0.kind == TokenKind::KwSelect && t1.kind == TokenKind::Ident &&
          t1.line == 2 && t1.col == 3, "多行行列号", "第 2 行第 3 列不符");

    // 词法错误
    parser::Lexer e1("SELECT 'unterminated");
    check(e1.tokenize_all().back().kind == TokenKind::Error, "未闭合字符串 -> Error");
    parser::Lexer e2("SELECT `unterminated");
    check(e2.tokenize_all().back().kind == TokenKind::Error, "未闭合反引号 -> Error");
    parser::Lexer e3("SELECT \\");
    check(e3.tokenize_all().back().kind == TokenKind::Error, "非法字符 -> Error");
    printf("  [PASS] 注释 / 位置 / 词法错误\n");
}

static void test_parser_keywords() {
    using parser::Parser;
    using parser::TokenKind;
    // 大小写混合的完整 SELECT 语句,覆盖子句级关键字
    {
        Parser p("sElEcT dIsTiNcT a As total FROM `users` "
                 "WHERE a >= 10 AND b IS NOT NULL "
                 "GROUP BY a HAVING COUNT(*) > 1 "
                 "ORDER BY a DESC LIMIT 5 OFFSET 2");
        auto st = p.parse_stmt();
        check(st && st->kind == ast::StmtKind::Select, "混合大小写 SELECT 解析");
        if (st && st->select_) {
            const auto& s = *st->select_;
            check(s.distinct, "DISTINCT 生效");
            check(s.columns.size() == 1 && s.columns[0].second == "total", "AS 别名");
            check(s.from_tables.size() == 1 && s.from_tables[0] == "users", "反引号表名");
            check(s.where != nullptr && s.having != nullptr, "WHERE / HAVING");
            check(s.group_by.size() == 1 && s.order_by.size() == 1 &&
                  !s.order_by[0].second, "GROUP BY / ORDER BY DESC");
            check(s.limit && *s.limit == 5 && s.offset && *s.offset == 2, "LIMIT / OFFSET");
        }
    }
    // 分号分隔多语句
    {
        Parser p("CREATE TABLE t (id INT PRIMARY KEY); "
                 "INSERT INTO t VALUES (1); "
                 "SELECT * FROM t;");
        auto stmts = p.parse_all();
        check(stmts.size() == 3, "parse_all 三条语句",
              "实际 " + std::to_string(stmts.size()));
        if (stmts.size() == 3) {
            check(stmts[0]->kind == ast::StmtKind::CreateTable &&
                  stmts[1]->kind == ast::StmtKind::Insert &&
                  stmts[2]->kind == ast::StmtKind::Select, "多语句 kind");
        }
    }
    // CASE / WHEN / THEN / ELSE / END 与新增词在别名位置
    {
        Parser p("SELECT CASE WHEN 1 = 1 THEN 'y' ELSE 'n' END AS value");
        auto st = p.parse_stmt();
        check(st && st->kind == ast::StmtKind::Select && st->select_->columns.size() == 1 &&
              st->select_->columns[0].second == "value",
              "CASE 表达式 + VALUE 关键字作别名");
    }
    // 新增关键词不得破坏「裸关键字作别名」(information_schema 反射会用 `ENGINE` engine ...)
    {
        Parser p("SELECT 1 engine FROM t");
        auto st = p.parse_stmt();
        check(st && st->kind == ast::StmtKind::Select &&
              st->select_->columns.size() == 1 &&
              st->select_->columns[0].second == "engine",
              "ENGINE 等新增关键词可作裸别名");
    }
    // SHOW / 事务关键字分发
    {
        Parser p("START TRANSACTION ISOLATION LEVEL READ COMMITTED");
        auto st = p.parse_stmt();
        check(st && st->kind == ast::StmtKind::StartTransaction,
              "START TRANSACTION ... READ COMMITTED");
    }
    // 错误语句必须抛 ParseError(Session 层会转成 MySQL 1064)
    {
        bool threw = false;
        try { Parser p("SELECT FROM"); (void)p.parse_stmt(); }
        catch (const parser::ParseError&) { threw = true; }
        check(threw, "非法语句抛 ParseError");
    }
    {
        bool threw = false;
        try { Parser p("FOO BAR"); (void)p.parse_stmt(); }
        catch (const parser::ParseError&) { threw = true; }
        check(threw, "未知起始关键字抛 ParseError");
    }
    printf("  [PASS] Parser 关键字消费\n");
}

int main() {
    printf("=== SQLTiny 常用 SQL 关键词 / 词法基础用例 ===\n");
    test_keyword_table();
    test_non_keywords();
    test_operators();
    test_literals();
    test_comments_position_errors();
    test_parser_keywords();
    printf("\n=== %d PASS / %d FAIL ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
