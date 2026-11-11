#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace sqltiny::parser {

/// Token 类型枚举,覆盖 MySQL 常用 + 标准 SQL
enum class TokenKind : uint16_t {
    // ---- 字面量 ----
    Number,          // 123 / 123.45 / 0x1A
    String,          // 'abc' (单引号)
    QuotedIdent,     // `users` (MySQL 反引号) / "users" (标准双引号,ANSI_QUOTES 模式)
    Placeholder,     // ?

    // ---- 标识符 ----
    Ident,           // foo / _bar / table_name

    // ---- 运算符 ----
    Plus,            // +
    Minus,           // -
    Star,            // *
    Slash,           // /
    Percent,         // %
    Eq,              // =
    Lt,              // <
    Gt,              // >
    Le,              // <=
    Ge,              // >=
    Ne,              // <> / !=
    Assign,          // := (MySQL 用户变量)
    Dot,             // .
    Comma,           // ,
    Semi,            // ;
    LParen,          // (
    RParen,          // )
    LBracket,        // [
    RBracket,        // ]
    Colon,           // :
    BitAnd,          // &
    BitOr,           // |
    BitXor,          // ^
    BitNot,          // ~
    LogNot,          // !
    DoublePipe,      // ||

    // ---- 关键字 ----
    // 这个列表是 SQL92 + MySQL 常用,后续可以继续扩充
    // (注意:关键字不是 enum 的独立成员,而是用 "Kw_" 前缀的 TokenKind 范围)
    // 为了方便,我们直接按整数段来区分,下面用 if/switch 处理
    Kw_START,

    // DML
    KwSelect, KwFrom, KwWhere, KwAnd, KwOr, KwNot, KwIs, KwNull,
    KwTrue, KwFalse, KwIn, KwLike, KwBetween, KwAs, KwOn, KwJoin,
    KwInner, KwLeft, KwRight, KwOuter, KwFull, KwCross, KwNatural,
    KwGroup, KwBy, KwHaving, KwOrder, KwAsc, KwDesc, KwLimit,
    KwOffset, KwDistinct, KwAll, KwUnion, KwIntersect, KwExcept,
    KwInsert, KwInto, KwValues, KwDefault, KwUpdate, KwSet, KwDelete,
    KwTruncate,

    // DDL
    KwCreate, KwTable, KwDatabase, KwSchema, KwDrop, KwAlter,
    KwAdd, KwColumn, KwPrimary, KwKey, KwUnique, KwForeign, KwReferences,
    KwIndex, KwConstraint, KwCheck, KwDefaultKw, KwAutoIncrement,
    KwComment, KwTemporary, KwIf, KwExists, KwReplace,
    KwInt, KwInteger, KwTinyInt, KwSmallInt, KwBigInt, KwDecimal,
    KwNumeric, KwFloat, KwDouble, KwReal, KwChar, KwVarchar,
    KwText, KwBlob, KwDate, KwTime, KwDateTime, KwTimestamp,
    KwBoolean, KwJson, KwEnum, KwUnsigned, KwSigned, KwZerofill,

    // TCL
    KwBegin, KwCommit, KwRollback, KwSavepoint, KwRelease,
    KwTransaction, KwWork, KwStart, KwTo,

    // Show / Misc
    KwShow, KwTables, KwDatabases, KwColumns, KwUse, KwDescribe, KwExplain, KwCase, KwWhen, KwThen,
    KwElse, KwEnd, KwCast, KwConvert, KwIntoKw, KwOut, KwEscape,
    KwCurrentTimestamp, KwNow, KwCurrentDate, KwCurrentTime,
    KwUser, KwDatabaseKw, KwCount, KwSum, KwAvg, KwMin, KwMax,
    KwConcat, KwCoalesce, KwIfnull, KwRound, KwAbs, KwLength,
    KwSchemas, KwFields, KwStatus, KwVariables, KwGlobal, KwSession,

    Kw_END,  // 哨兵

    // ---- 结束 ----
    Eof,
    Error,           // 词法错误
};

struct Token {
    TokenKind   kind = TokenKind::Eof;
    std::string text;        // 原始文本(字符串/标识符会在这里)
    int64_t     int_value = 0;
    double      num_value = 0.0;
    int         line = 1;
    int         col  = 1;

    Token() = default;
    Token(TokenKind k, std::string t, int ln, int c)
        : kind(k), text(std::move(t)), line(ln), col(c) {}
    Token(TokenKind k, const char* t, int ln, int c)
        : kind(k), text(t), line(ln), col(c) {}
};

/// 关键字查找:返回对应的 TokenKind,否则 Ident
TokenKind lookup_keyword(std::string_view s);

/// Lexer 入口
class Lexer {
public:
    explicit Lexer(std::string_view source);

    /// 下一个 token
    Token next();

    /// 预览(不消费)
    Token peek();

    /// 全部扫出来(调试用)
    std::vector<Token> tokenize_all();

    int line() const { return line_; }
    int col()  const { return col_;  }

private:
    std::string_view src_;
    size_t pos_ = 0;
    int line_ = 1, col_ = 1;
    bool peeked_ = false;
    Token peek_buf_;

    bool eof() const { return pos_ >= src_.size(); }
    char peek_char(size_t offset = 0) const {
        size_t p = pos_ + offset;
        return p < src_.size() ? src_[p] : '\0';
    }
    void advance(size_t n = 1);
    void skip_ws_and_comments();
    Token read_number();
    Token read_string(char quote);
    Token read_quoted_ident(char quote);
    Token read_ident_or_keyword();
};

} // namespace sqltiny::parser
