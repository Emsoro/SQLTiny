#include "parser/lexer.h"
#include <cctype>
#include <cstdlib>
#include <fmt/format.h>

namespace sqltiny::parser {

// ---------- 关键字表 ----------
struct KwEntry { std::string_view name; TokenKind kind; };
static const KwEntry k_keywords[] = {
    {"SELECT",     TokenKind::KwSelect},
    {"FROM",       TokenKind::KwFrom},
    {"WHERE",      TokenKind::KwWhere},
    {"AND",        TokenKind::KwAnd},
    {"OR",         TokenKind::KwOr},
    {"NOT",        TokenKind::KwNot},
    {"IS",         TokenKind::KwIs},
    {"NULL",       TokenKind::KwNull},
    {"TRUE",       TokenKind::KwTrue},
    {"FALSE",      TokenKind::KwFalse},
    {"IN",         TokenKind::KwIn},
    {"LIKE",       TokenKind::KwLike},
    {"BETWEEN",    TokenKind::KwBetween},
    {"AS",         TokenKind::KwAs},
    {"ON",         TokenKind::KwOn},
    {"JOIN",       TokenKind::KwJoin},
    {"INNER",      TokenKind::KwInner},
    {"LEFT",       TokenKind::KwLeft},
    {"RIGHT",      TokenKind::KwRight},
    {"OUTER",      TokenKind::KwOuter},
    {"FULL",       TokenKind::KwFull},
    {"CROSS",      TokenKind::KwCross},
    {"NATURAL",    TokenKind::KwNatural},
    {"GROUP",      TokenKind::KwGroup},
    {"BY",         TokenKind::KwBy},
    {"HAVING",     TokenKind::KwHaving},
    {"ORDER",      TokenKind::KwOrder},
    {"ASC",        TokenKind::KwAsc},
    {"DESC",       TokenKind::KwDesc},
    {"LIMIT",      TokenKind::KwLimit},
    {"OFFSET",     TokenKind::KwOffset},
    {"DISTINCT",   TokenKind::KwDistinct},
    {"ALL",        TokenKind::KwAll},
    {"UNION",      TokenKind::KwUnion},
    {"INSERT",     TokenKind::KwInsert},
    {"INTO",       TokenKind::KwInto},
    {"VALUES",     TokenKind::KwValues},
    {"DEFAULT",    TokenKind::KwDefault},
    {"UPDATE",     TokenKind::KwUpdate},
    {"SET",        TokenKind::KwSet},
    {"DELETE",     TokenKind::KwDelete},
    {"TRUNCATE",   TokenKind::KwTruncate},
    {"CREATE",     TokenKind::KwCreate},
    {"TABLE",      TokenKind::KwTable},
    {"DATABASE",   TokenKind::KwDatabase},
    {"SCHEMA",     TokenKind::KwSchema},
    {"DROP",       TokenKind::KwDrop},
    {"ALTER",      TokenKind::KwAlter},
    {"ADD",        TokenKind::KwAdd},
    {"COLUMN",     TokenKind::KwColumn},
    {"PRIMARY",    TokenKind::KwPrimary},
    {"KEY",        TokenKind::KwKey},
    {"UNIQUE",     TokenKind::KwUnique},
    {"FOREIGN",    TokenKind::KwForeign},
    {"REFERENCES", TokenKind::KwReferences},
    {"INDEX",      TokenKind::KwIndex},
    {"CONSTRAINT", TokenKind::KwConstraint},
    {"CHECK",      TokenKind::KwCheck},
    {"AUTO_INCREMENT", TokenKind::KwAutoIncrement},
    {"AUTOINCREMENT", TokenKind::KwAutoIncrement},
    {"IF",         TokenKind::KwIf},
    {"EXISTS",     TokenKind::KwExists},
    {"REPLACE",    TokenKind::KwReplace},
    {"INT",        TokenKind::KwInt},
    {"INTEGER",    TokenKind::KwInteger},
    {"TINYINT",    TokenKind::KwTinyInt},
    {"SMALLINT",   TokenKind::KwSmallInt},
    {"BIGINT",     TokenKind::KwBigInt},
    {"DECIMAL",   TokenKind::KwDecimal},
    {"NUMERIC",    TokenKind::KwNumeric},
    {"FLOAT",      TokenKind::KwFloat},
    {"DOUBLE",     TokenKind::KwDouble},
    {"REAL",       TokenKind::KwReal},
    {"CHAR",       TokenKind::KwChar},
    {"VARCHAR",    TokenKind::KwVarchar},
    {"TEXT",       TokenKind::KwText},
    {"BLOB",       TokenKind::KwBlob},
    {"DATE",       TokenKind::KwDate},
    {"TIME",       TokenKind::KwTime},
    {"DATETIME",   TokenKind::KwDateTime},
    {"TIMESTAMP",  TokenKind::KwTimestamp},
    {"BOOLEAN",    TokenKind::KwBoolean},
    {"JSON",       TokenKind::KwJson},
    {"ENUM",       TokenKind::KwEnum},
    {"UNSIGNED",   TokenKind::KwUnsigned},
    {"SIGNED",     TokenKind::KwSigned},
    {"BEGIN",      TokenKind::KwBegin},
    {"COMMIT",     TokenKind::KwCommit},
    {"ROLLBACK",   TokenKind::KwRollback},
    {"SAVEPOINT",  TokenKind::KwSavepoint},
    {"TRANSACTION",TokenKind::KwTransaction},
    {"SHOW",       TokenKind::KwShow},
    {"USE",        TokenKind::KwUse},
    {"DESCRIBE",   TokenKind::KwDescribe},
    {"EXPLAIN",    TokenKind::KwExplain},
    {"CASE",       TokenKind::KwCase},
    {"WHEN",       TokenKind::KwWhen},
    {"THEN",       TokenKind::KwThen},
    {"ELSE",       TokenKind::KwElse},
    {"END",        TokenKind::KwEnd},
    {"CAST",       TokenKind::KwCast},
    {"CONVERT",    TokenKind::KwConvert},
    {"ESCAPE",     TokenKind::KwEscape},
    {"CURRENT_TIMESTAMP", TokenKind::KwCurrentTimestamp},
    {"NOW",        TokenKind::KwNow},
    {"CURRENT_DATE",     TokenKind::KwCurrentDate},
    {"CURRENT_TIME",     TokenKind::KwCurrentTime},
    {"USER",       TokenKind::KwUser},
    {"DATABASE",   TokenKind::KwDatabaseKw},
    {"COUNT",      TokenKind::KwCount},
    {"SUM",        TokenKind::KwSum},
    {"AVG",        TokenKind::KwAvg},
    {"MIN",        TokenKind::KwMin},
    {"MAX",        TokenKind::KwMax},
    {"CONCAT",     TokenKind::KwConcat},
    {"COALESCE",   TokenKind::KwCoalesce},
    {"IFNULL",     TokenKind::KwIfnull},
    {"ROUND",      TokenKind::KwRound},
    {"ABS",        TokenKind::KwAbs},
    {"LENGTH",     TokenKind::KwLength},
    {"COMMENT",    TokenKind::KwComment},
    {"TEMPORARY",  TokenKind::KwTemporary},
    {"REPLACE",    TokenKind::KwReplace},
};

// 简单的大小写无关比较 (ASCII 范围)
static bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = std::toupper(static_cast<unsigned char>(a[i]));
        char y = std::toupper(static_cast<unsigned char>(b[i]));
        if (x != y) return false;
    }
    return true;
}

TokenKind lookup_keyword(std::string_view s) {
    for (auto& e : k_keywords) {
        if (iequals(e.name, s)) return e.kind;
    }
    return TokenKind::Ident;
}

// ---------- Lexer ----------
Lexer::Lexer(std::string_view source) : src_(source) {}

void Lexer::advance(size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (pos_ >= src_.size()) return;
        char c = src_[pos_];
        if (c == '\n') { line_++; col_ = 1; }
        else            { col_++; }
        pos_++;
    }
}

void Lexer::skip_ws_and_comments() {
    while (!eof()) {
        char c = peek_char();
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            advance();
        } else if (c == '-' && peek_char(1) == '-') {
            // SQL 单行注释 --
            advance(2);
            while (!eof() && peek_char() != '\n') advance();
        } else if (c == '#' ) {
            // MySQL 风格单行注释
            advance();
            while (!eof() && peek_char() != '\n') advance();
        } else if (c == '/' && peek_char(1) == '*') {
            advance(2);
            while (!eof() && !(peek_char() == '*' && peek_char(1) == '/')) advance();
            if (!eof()) advance(2); // consume */
        } else {
            break;
        }
    }
}

Token Lexer::next() {
    if (peeked_) {
        peeked_ = false;
        return peek_buf_;
    }
    skip_ws_and_comments();
    if (eof()) {
        Token t; t.kind = TokenKind::Eof; t.line = line_; t.col = col_; return t;
    }
    int start_line = line_, start_col = col_;
    char c = peek_char();

    // 十六进制字面量 0x... (必须在数字检查之前,否则 0 会被 read_number 吃掉)
    if (c == '0' && (peek_char(1) == 'x' || peek_char(1) == 'X')) {
        advance(2);
        size_t start = pos_;
        while (!eof() && std::isxdigit(static_cast<unsigned char>(peek_char()))) advance();
        Token t; t.kind = TokenKind::Number; t.line = start_line; t.col = start_col;
        t.text = "0x" + std::string(src_.substr(start, pos_ - start));
        return t;
    }

    // 数字或以数字开头 (但标识符不能以数字开头, 所以这是数字/点号判断)
    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '.' && std::isdigit(static_cast<unsigned char>(peek_char(1))))) {
        return read_number();
    }
    // 字符串: 单引号 / 双引号 (双引号在 ANSI_QUOTES 模式下是标识符, 先当字符串处理, Parser 再决定)
    if (c == '\'') return read_string('\'');
    if (c == '"')  return read_string('"');

    // MySQL 反引号标识符
    if (c == '`') return read_quoted_ident('`');

    // ? 占位符
    if (c == '?') { advance(); Token t; t.kind = TokenKind::Placeholder; t.line = start_line; t.col = start_col; return t; }

    // 标识符 / 关键字
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80) {
        return read_ident_or_keyword();
    }

    // 多字符运算符
    if (c == '<' && peek_char(1) == '=') { advance(2); return { TokenKind::Le,   "<=", start_line, start_col }; }
    if (c == '>' && peek_char(1) == '=') { advance(2); return { TokenKind::Ge,   ">=", start_line, start_col }; }
    if (c == '<' && peek_char(1) == '>') { advance(2); return { TokenKind::Ne,   "<>", start_line, start_col }; }
    if (c == '!' && peek_char(1) == '=') { advance(2); return { TokenKind::Ne,   "!=", start_line, start_col }; }
    if (c == ':' && peek_char(1) == '=') { advance(2); return { TokenKind::Assign,":=", start_line, start_col }; }
    if (c == '|' && peek_char(1) == '|') { advance(2); return { TokenKind::DoublePipe, "||", start_line, start_col }; }

    // 单字符运算符/标点
    advance();
    switch (c) {
        case '+':  return { TokenKind::Plus,      "+",  start_line, start_col };
        case '-':  return { TokenKind::Minus,     "-",  start_line, start_col };
        case '*':  return { TokenKind::Star,      "*",  start_line, start_col };
        case '/':  return { TokenKind::Slash,     "/",  start_line, start_col };
        case '%':  return { TokenKind::Percent,   "%",  start_line, start_col };
        case '=':  return { TokenKind::Eq,        "=",  start_line, start_col };
        case '<':  return { TokenKind::Lt,        "<",  start_line, start_col };
        case '>':  return { TokenKind::Gt,        ">",  start_line, start_col };
        case '.':  return { TokenKind::Dot,       ".",  start_line, start_col };
        case ',':  return { TokenKind::Comma,     ",",  start_line, start_col };
        case ';':  return { TokenKind::Semi,      ";",  start_line, start_col };
        case '(':  return { TokenKind::LParen,    "(",  start_line, start_col };
        case ')':  return { TokenKind::RParen,    ")",  start_line, start_col };
        case '[':  return { TokenKind::LBracket,  "[",  start_line, start_col };
        case ']':  return { TokenKind::RBracket,  "]",  start_line, start_col };
        case ':':  return { TokenKind::Colon,     ":",  start_line, start_col };
        case '&':  return { TokenKind::BitAnd,    "&",  start_line, start_col };
        case '|':  return { TokenKind::BitOr,     "|",  start_line, start_col };
        case '^':  return { TokenKind::BitXor,    "^",  start_line, start_col };
        case '~':  return { TokenKind::BitNot,    "~",  start_line, start_col };
        case '!':  return { TokenKind::LogNot,    "!",  start_line, start_col };
        default: {
            Token t; t.kind = TokenKind::Error;
            t.text = fmt::format("unexpected char '{}' (U+{:04X})", c, (unsigned char)c);
            t.line = start_line; t.col = start_col;
            return t;
        }
    }
}

Token Lexer::peek() {
    if (!peeked_) { peek_buf_ = next(); peeked_ = true; }
    return peek_buf_;
}

std::vector<Token> Lexer::tokenize_all() {
    std::vector<Token> out;
    while (true) {
        Token t = next();
        out.push_back(t);
        if (t.kind == TokenKind::Eof || t.kind == TokenKind::Error) break;
    }
    return out;
}

Token Lexer::read_number() {
    int start_line = line_, start_col = col_;
    size_t start = pos_;
    bool is_float = false;

    if (peek_char() == '.') {
        // 从 . 开头说明前面不会是数字 (已在入口处理)
        // 但这里万一有 1.2.3 的情况, 暂时当作简单浮点
    }

    while (!eof() && std::isdigit(static_cast<unsigned char>(peek_char()))) advance();

    if (!eof() && peek_char() == '.') {
        is_float = true;
        advance();
        while (!eof() && std::isdigit(static_cast<unsigned char>(peek_char()))) advance();
    }

    if (!eof() && (peek_char() == 'e' || peek_char() == 'E')) {
        is_float = true;
        advance();
        if (!eof() && (peek_char() == '+' || peek_char() == '-')) advance();
        while (!eof() && std::isdigit(static_cast<unsigned char>(peek_char()))) advance();
    }

    std::string text{ src_.substr(start, pos_ - start) };
    Token t;
    t.text = text;
    t.line = start_line; t.col = start_col;

    if (is_float) {
        t.kind = TokenKind::Number;
        t.num_value = std::stod(text);
    } else {
        t.kind = TokenKind::Number;
        try {
            t.int_value = std::stoll(text);
        } catch (...) {
            // 大数用 double 兜底
            t.num_value = std::stod(text);
        }
    }
    return t;
}

Token Lexer::read_string(char quote) {
    int start_line = line_, start_col = col_;
    advance(); // consume opening quote
    std::string buf;
    while (!eof()) {
        char c = peek_char();
        if (c == quote) {
            // SQL 风格 escape: 'it''s' → it's
            if (peek_char(1) == quote) {
                buf.push_back(quote);
                advance(2);
                continue;
            }
            advance(); // consume closing quote
            Token t; t.kind = TokenKind::String; t.text = buf;
            t.line = start_line; t.col = start_col;
            return t;
        }
        // MySQL \escape (只在单引号模式下启用,双引号按标准 SQL 语义不处理)
        if (quote == '\'' && c == '\\') {
            advance();
            if (eof()) break;
            char esc = peek_char(); advance();
            switch (esc) {
                case 'n': buf.push_back('\n'); break;
                case 't': buf.push_back('\t'); break;
                case 'r': buf.push_back('\r'); break;
                case '0': buf.push_back('\0'); break;
                case '\\': buf.push_back('\\'); break;
                case '\'': buf.push_back('\''); break;
                case '"': buf.push_back('"'); break;
                default:  buf.push_back(esc);
            }
            continue;
        }
        buf.push_back(c);
        advance();
    }
    // eof 未闭合 — 当作 Error
    Token t; t.kind = TokenKind::Error;
    t.text = "unterminated string literal";
    t.line = start_line; t.col = start_col;
    return t;
}

Token Lexer::read_quoted_ident(char quote) {
    int start_line = line_, start_col = col_;
    advance(); // consume opening quote
    std::string buf;
    while (!eof()) {
        char c = peek_char();
        if (c == quote) {
            if (peek_char(1) == quote) { buf.push_back(quote); advance(2); continue; }
            advance();
            Token t; t.kind = TokenKind::QuotedIdent; t.text = buf;
            t.line = start_line; t.col = start_col;
            return t;
        }
        buf.push_back(c);
        advance();
    }
    Token t; t.kind = TokenKind::Error;
    t.text = "unterminated identifier";
    t.line = start_line; t.col = start_col;
    return t;
}

Token Lexer::read_ident_or_keyword() {
    int start_line = line_, start_col = col_;
    size_t start = pos_;
    while (!eof()) {
        char c = peek_char();
        bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
        if (!ok) {
            // 还允许非 ASCII(中文标识符等),暂放开
            if (static_cast<unsigned char>(c) < 0x80) break;
        }
        advance();
    }
    std::string text{ src_.substr(start, pos_ - start) };
    Token t;
    t.text = text;
    t.line = start_line; t.col = start_col;

    // 关键字查找(大小写无关)
    TokenKind k = lookup_keyword(text);
    t.kind = k;
    return t;
}

} // namespace sqltiny::parser
