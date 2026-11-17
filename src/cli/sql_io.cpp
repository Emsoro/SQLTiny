#include "cli/sql_io.h"

#include "core/session.h"
#include "storage/table.h"
#include "types/value.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace sqltiny::cli {

namespace {

bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string trim_copy(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string first_word(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    size_t b = i;
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) {
        char c = s[i];
        if (c == '(' || c == ';') break;
        ++i;
    }
    return s.substr(b, i - b);
}

std::string one_line_snippet(const std::string& sql, size_t max_len = 180) {
    std::string out;
    out.reserve(std::min(sql.size(), max_len));
    for (char c : sql) {
        if (out.size() >= max_len) { out += "..."; break; }
        out.push_back((c == '\n' || c == '\r' || c == '\t') ? ' ' : c);
    }
    return out;
}

/// mysqldump 会写成 `CREATE DATABASE /*!32312 IF NOT EXISTS*/ \`db\`;`,
/// 条件注释被跳过后需要把 `IF NOT EXISTS` 保留下来,否则导入已存在的库会失败。
bool conditional_has_if_not_exists(const std::string& content) {
    std::string t = trim_copy(content);
    static const char* kWord = "IF NOT EXISTS";
    constexpr size_t kLen = 13;
    if (t.size() < kLen) return false;
    for (size_t i = 0; i < kLen; ++i) {
        if (std::toupper(static_cast<unsigned char>(t[i])) != kWord[i]) return false;
    }
    return true;
}

/// 逐字符读取 SQL 脚本,负责注释/引号/自定义分隔符处理。
class ScriptReader {
public:
    struct Stmt {
        std::string sql;
        int line = 1;
    };

    explicit ScriptReader(std::istream& in) : in_(in) {}

    bool next(Stmt& out) {
        // 跳过语句之间的空白、注释和 DELIMITER 指令
        for (;;) {
            int c = peek();
            if (c < 0) return false;
            if (std::isspace(static_cast<unsigned char>(c))) { get(); continue; }
            if (c == '#' || (c == '-' && peek(1) == '-' &&
                             (peek(2) < 0 || std::isspace(static_cast<unsigned char>(peek(2)))))) {
                skip_line();
                continue;
            }
            if (c == '/' && peek(1) == '*') { (void)skip_block_comment(); continue; }
            if (try_delimiter_directive()) continue;
            break;
        }

        out.line = line_;
        out.sql.clear();

        for (;;) {
            int c = peek();
            if (c < 0) {
                out.sql = trim_copy(out.sql);
                return !out.sql.empty();
            }
            if (c == '\'' || c == '"' || c == '`') {
                read_quoted(out.sql, static_cast<char>(c));
                continue;
            }
            if (c == '#' || (c == '-' && peek(1) == '-' &&
                             (peek(2) < 0 || std::isspace(static_cast<unsigned char>(peek(2)))))) {
                skip_line();
                append_separator(out.sql);
                continue;
            }
            if (c == '/' && peek(1) == '*') {
                std::string conditional = skip_block_comment();
                if (conditional_has_if_not_exists(conditional)) {
                    append_separator(out.sql);
                    out.sql += " IF NOT EXISTS ";
                } else {
                    append_separator(out.sql);
                }
                continue;
            }
            if (starts_with(delimiter_)) {
                for (size_t i = 0; i < delimiter_.size(); ++i) get();
                out.sql = trim_copy(out.sql);
                if (!out.sql.empty()) return true;
                // 连续分隔符/空语句:继续找下一条
                out.line = line_;
                continue;
            }
            out.sql.push_back(static_cast<char>(get()));
        }
    }

private:
    static void append_separator(std::string& out) {
        if (!out.empty() && !std::isspace(static_cast<unsigned char>(out.back())))
            out.push_back(' ');
    }

    void ensure(size_t n) {
        while (buf_.size() - pos_ < n && !eof_) {
            char tmp[65536];
            in_.read(tmp, static_cast<std::streamsize>(sizeof(tmp)));
            auto got = in_.gcount();
            if (got > 0) buf_.append(tmp, static_cast<size_t>(got));
            if (got < static_cast<std::streamsize>(sizeof(tmp))) eof_ = true;
        }
    }

    int peek(size_t off = 0) {
        ensure(off + 1);
        if (pos_ + off >= buf_.size()) return -1;
        return static_cast<unsigned char>(buf_[pos_ + off]);
    }

    int get() {
        int c = peek();
        if (c < 0) return -1;
        ++pos_;
        if (c == '\n') ++line_;
        if (pos_ > (1u << 20)) {
            buf_.erase(0, pos_);
            pos_ = 0;
        }
        return c;
    }

    bool starts_with(const std::string& s) {
        if (s.empty()) return false;
        ensure(s.size());
        if (pos_ + s.size() > buf_.size()) return false;
        return buf_.compare(pos_, s.size(), s) == 0;
    }

    void skip_line() {
        while (true) {
            int c = peek();
            if (c < 0 || c == '\n') break;
            get();
        }
    }

    std::string skip_block_comment() {
        get(); // '/'
        get(); // '*'
        bool conditional = (peek() == '!');
        std::string content;
        if (conditional) {
            get(); // '!'
            while (std::isdigit(static_cast<unsigned char>(peek()))) get(); // 版本号
        }
        while (true) {
            int c = get();
            if (c < 0) return content;
            if (c == '*' && peek() == '/') { get(); return content; }
            if (conditional) content.push_back(static_cast<char>(c));
        }
    }

    bool try_delimiter_directive() {
        static const char* kWord = "DELIMITER";
        constexpr size_t kLen = 9;
        ensure(kLen + 1);
        if (pos_ + kLen + 1 > buf_.size()) return false;
        for (size_t i = 0; i < kLen; ++i) {
            if (std::toupper(static_cast<unsigned char>(buf_[pos_ + i])) != kWord[i])
                return false;
        }
        int after = peek(kLen);
        if (after != ' ' && after != '\t') return false;

        for (size_t i = 0; i < kLen; ++i) get();
        while (peek() == ' ' || peek() == '\t') get();

        std::string d;
        while (true) {
            int c = peek();
            if (c < 0 || c == '\n' || c == '\r') break;
            d.push_back(static_cast<char>(get()));
        }
        if (peek() == '\r') get();
        if (peek() == '\n') get();
        d = trim_copy(d);
        if (!d.empty()) delimiter_ = d;
        return true;
    }

    void read_quoted(std::string& out, char quote) {
        out.push_back(static_cast<char>(get())); // opening quote
        while (true) {
            int c = peek();
            if (c < 0) return;
            if (c == '\\' && quote != '`') {
                out.push_back(static_cast<char>(get()));
                if (peek() >= 0) out.push_back(static_cast<char>(get()));
                continue;
            }
            if (c == quote) {
                out.push_back(static_cast<char>(get()));
                // '' / "" / `` 连续两个引号表示转义后的引号本身
                if (peek() == quote) {
                    out.push_back(static_cast<char>(get()));
                    continue;
                }
                return;
            }
            out.push_back(static_cast<char>(get()));
        }
    }

    std::istream& in_;
    std::string buf_;
    size_t pos_ = 0;
    bool eof_ = false;
    int line_ = 1;
    std::string delimiter_ = ";";
};

// ============================================================
// dump literal helpers
// ============================================================

std::string quote_ident(const std::string& name) {
    std::string out = "`";
    for (char c : name) {
        if (c == '`') out.push_back('`');
        out.push_back(c);
    }
    out.push_back('`');
    return out;
}

void append_quoted(std::string& out, const std::string& s) {
    out.push_back('\'');
    for (unsigned char c : s) {
        switch (c) {
            case '\0': out += "\\0"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '"':  out += "\\\""; break;
            case 26:   out += "\\Z"; break;
            default:   out.push_back(static_cast<char>(c)); break;
        }
    }
    out.push_back('\'');
}

void append_literal(std::string& out, const types::Value& v) {
    using types::Value;
    switch (v.kind) {
        case Value::Kind::Null: out += "NULL"; break;
        case Value::Kind::Bool: out += v.v_bool ? "1" : "0"; break;
        case Value::Kind::Int64: out += fmt::format("{}", v.v_int); break;
        case Value::Kind::Double:
            if (std::isfinite(v.v_double)) out += fmt::format("{}", v.v_double);
            else out += "NULL";
            break;
        case Value::Kind::String: append_quoted(out, v.v_str); break;
        case Value::Kind::Blob:
            append_quoted(out, std::string(v.v_blob.begin(), v.v_blob.end()));
            break;
        default:
            append_quoted(out, v.to_string());
            break;
    }
}

std::string column_type_name(const types::TypeInfo& t) {
    using types::SqlType;
    switch (t.type) {
        case SqlType::Boolean:  return "tinyint(1)";
        case SqlType::TinyInt:  return "tinyint";
        case SqlType::SmallInt: return "smallint";
        case SqlType::Int:      return "int";
        case SqlType::BigInt:   return "bigint";
        case SqlType::Decimal:
            return fmt::format("decimal({},{})", t.length ? t.length : 10, t.scale);
        case SqlType::Float:    return "float";
        case SqlType::Double:   return "double";
        case SqlType::Char:     return fmt::format("char({})", t.length ? t.length : 1);
        case SqlType::Varchar:  return fmt::format("varchar({})", t.length ? t.length : 255);
        case SqlType::Text:     return "text";
        case SqlType::Binary:
        case SqlType::Varbinary:
        case SqlType::Blob:     return "blob";
        case SqlType::Date:     return "date";
        case SqlType::Time:     return "time";
        case SqlType::DateTime: return "datetime";
        case SqlType::Timestamp:return "timestamp";
        case SqlType::Json:     return "json";
        default:                return "varchar(255)";
    }
}

std::string create_table_sql(const storage::Table& tbl) {
    const auto& def = tbl.def();
    std::string sql = "CREATE TABLE " + quote_ident(def.name) + " (\n";

    std::vector<std::string> lines;
    lines.reserve(def.columns.size() + def.indexes.size() + 1);

    for (const auto& col : def.columns) {
        std::string line = "  " + quote_ident(col.name) + " " + column_type_name(col.type);
        if (col.type.unsigned_) line += " UNSIGNED";
        if (!col.nullable) line += " NOT NULL";
        if (col.is_auto_increment) line += " AUTO_INCREMENT";
        if (col.default_value && col.default_value->kind == ast::ExprKind::Literal) {
            line += " DEFAULT ";
            append_literal(line, col.default_value->literal);
        }
        if (!col.comment.empty()) {
            line += " COMMENT ";
            append_quoted(line, col.comment);
        }
        lines.push_back(std::move(line));
    }

    if (!def.primary_key_cols.empty() && !def.primary_key_cols[0].empty()) {
        std::string line = "  PRIMARY KEY (";
        for (size_t i = 0; i < def.primary_key_cols[0].size(); ++i) {
            if (i) line += ", ";
            line += quote_ident(def.primary_key_cols[0][i]);
        }
        line += ")";
        lines.push_back(std::move(line));
    }

    for (const auto& [name, idx] : def.indexes) {
        std::string line = idx.unique ? "  UNIQUE KEY " : "  KEY ";
        line += quote_ident(name.empty() ? idx.columns.front() : name);
        line += " (";
        for (size_t i = 0; i < idx.columns.size(); ++i) {
            if (i) line += ", ";
            line += quote_ident(idx.columns[i]);
        }
        line += ")";
        lines.push_back(std::move(line));
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        sql += lines[i];
        sql += (i + 1 == lines.size()) ? "\n" : ",\n";
    }
    sql += ")";

    bool has_auto_inc = false;
    for (const auto& col : def.columns) has_auto_inc = has_auto_inc || col.is_auto_increment;
    if (has_auto_inc) {
        sql += fmt::format(" AUTO_INCREMENT={}", tbl.auto_inc_cursor() + 1);
    }
    sql += " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;\n";
    return sql;
}

bool is_requested_table(const DumpOptions& opts, const std::string& db, const std::string& table) {
    if (opts.tables.empty()) return true;
    const std::string qualified = db + "." + table;
    for (const auto& t : opts.tables) {
        if (ieq(t, qualified) || ieq(t, table)) return true;
    }
    return false;
}

bool dump_table_data(std::ostream& out, const storage::Table& tbl, uint64_t scn,
                     uint64_t reader_txn, size_t rows_per_insert, DumpStats& stats) {
    const auto& def = tbl.def();
    std::string prefix = "INSERT INTO " + quote_ident(def.name) + " (";
    for (size_t i = 0; i < def.columns.size(); ++i) {
        if (i) prefix += ", ";
        prefix += quote_ident(def.columns[i].name);
    }
    prefix += ") VALUES\n";

    size_t in_batch = 0;
    bool open = false;
    uint64_t table_rows = 0;

    tbl.for_each_visible(scn, [&](int64_t, const storage::Row& row) -> bool {
        if (!open) {
            out << prefix;
            open = true;
        } else {
            out << ",\n";
        }
        out << '(';
        std::string cell;
        for (size_t i = 0; i < def.columns.size(); ++i) {
            if (i) out << ", ";
            cell.clear();
            if (i < row.size()) append_literal(cell, row[i]);
            else cell = "NULL";
            out << cell;
        }
        out << ')';
        ++in_batch;
        ++table_rows;
        if (in_batch >= rows_per_insert) {
            out << ";\n";
            in_batch = 0;
            open = false;
        }
        return static_cast<bool>(out.good());
    }, reader_txn);

    if (open) out << ";\n";
    stats.rows += table_rows;
    return out.good();
}

} // namespace

// ============================================================
// import
// ============================================================

bool import_sql(std::istream& in, core::Session& session, const ImportOptions& opts,
                ImportStats& stats, std::ostream& log) {
    ScriptReader reader(in);
    ScriptReader::Stmt stmt;
    size_t reported_errors = 0;
    constexpr size_t kMaxReportedErrors = 20;

    while (reader.next(stmt)) {
        ++stats.statements;

        std::string word = first_word(stmt.sql);
        // mysqldump 会生成 LOCK TABLES / UNLOCK TABLES,引擎无表锁语句,直接跳过。
        if (ieq(word, "LOCK") || ieq(word, "UNLOCK")) {
            ++stats.skipped;
            continue;
        }

        executor::ResultSet r;
        try {
            r = session.execute(stmt.sql);
        } catch (const std::exception& e) {
            r.ok = false;
            r.mysql_errno = 1105;
            r.error_message = e.what();
        }

        if (!r.ok) {
            ++stats.failed;
            if (stats.first_error.empty()) {
                stats.first_error = fmt::format("line {}: {} (errno {}): {}",
                    stmt.line, r.error_message, r.mysql_errno, one_line_snippet(stmt.sql));
            }
            if (reported_errors < kMaxReportedErrors) {
                log << fmt::format("[import] line {}: {} (errno {}): {}\n",
                    stmt.line, r.error_message, r.mysql_errno, one_line_snippet(stmt.sql));
                ++reported_errors;
                if (reported_errors == kMaxReportedErrors)
                    log << "[import] further errors suppressed\n";
            }
            if (!opts.continue_on_error) return false;
        } else {
            ++stats.succeeded;
            if (r.affected_rows > 0) stats.affected_rows += r.affected_rows;
        }

        if (!opts.quiet && opts.progress_every > 0 &&
            stats.statements % opts.progress_every == 0) {
            log << fmt::format("[import] {} statements, {} ok, {} failed, {} rows affected\n",
                stats.statements, stats.succeeded, stats.failed, stats.affected_rows);
        }
    }

    if (in.bad()) {
        log << "[import] read error while reading SQL script\n";
        return false;
    }
    return stats.failed == 0;
}

bool import_sql_file(const std::string& path, core::Session& session,
                     const ImportOptions& opts, ImportStats& stats, std::ostream& log) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        log << fmt::format("[import] cannot open file: {}\n", path);
        return false;
    }
    return import_sql(in, session, opts, stats, log);
}

// ============================================================
// dump
// ============================================================

bool dump_sql(core::Session& session, const DumpOptions& opts,
              std::ostream& out, DumpStats& stats, std::ostream& log) {
    if (!opts.include_schema && !opts.include_data) {
        log << "[dump] --dump-no-create and --dump-no-data cannot be used together\n";
        return false;
    }

    auto& catalog = core::Database::instance().catalog();

    std::vector<std::string> databases;
    if (opts.databases.empty()) {
        databases = catalog.list_databases();
    } else {
        for (const auto& db : opts.databases) {
            if (!catalog.database_exists(db)) {
                log << fmt::format("[dump] unknown database: {}\n", db);
                return false;
            }
            databases.push_back(db);
        }
    }

    out << "-- SQLTiny dump\n";
    out << "-- MySQL-compatible SQL script, re-import with: sqltiny_server --import <file>\n";
    out << "SET NAMES utf8mb4;\n";
    out << "SET FOREIGN_KEY_CHECKS=0;\n";

    uint64_t scn = session.readSCN();
    uint64_t reader_txn = session.txnId();

    std::vector<std::string> matched_tables;
    const size_t rows_per_insert = opts.rows_per_insert ? opts.rows_per_insert : 1000;

    for (const auto& db : databases) {
        out << "\n--\n-- Database: " << quote_ident(db) << "\n--\n";
        out << "CREATE DATABASE IF NOT EXISTS " << quote_ident(db) << ";\n";
        out << "USE " << quote_ident(db) << ";\n";
        ++stats.databases;

        for (const auto& table_name : catalog.table_names(db)) {
            if (!is_requested_table(opts, db, table_name)) continue;
            const storage::Table* tbl = catalog.get_table_in_db(db, table_name);
            if (!tbl) continue;
            matched_tables.push_back(db + "." + table_name);

            out << "\n--\n-- Table: " << quote_ident(db) << "." << quote_ident(table_name) << "\n--\n";
            if (opts.include_schema) {
                if (opts.drop_tables)
                    out << "DROP TABLE IF EXISTS " << quote_ident(table_name) << ";\n";
                out << create_table_sql(*tbl);
            }
            if (opts.include_data && !dump_table_data(out, *tbl, scn, reader_txn,
                                                       rows_per_insert, stats)) {
                log << "[dump] write error\n";
                return false;
            }
            ++stats.tables;
        }
    }

    for (const auto& requested : opts.tables) {
        bool found = false;
        for (const auto& actual : matched_tables) {
            if (ieq(requested, actual) ||
                (requested.find('.') == std::string::npos &&
                 ieq(requested, actual.substr(actual.find('.') + 1)))) {
                found = true;
                break;
            }
        }
        if (!found) {
            log << fmt::format("[dump] table not found: {}\n", requested);
            return false;
        }
    }

    out << "\nSET FOREIGN_KEY_CHECKS=1;\n";
    out.flush();
    return out.good();
}

bool dump_sql_file(const std::string& path, core::Session& session,
                   const DumpOptions& opts, DumpStats& stats, std::ostream& log) {
    if (path == "-") {
        return dump_sql(session, opts, std::cout, stats, log);
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        log << fmt::format("[dump] cannot open file for writing: {}\n", path);
        return false;
    }
    bool ok = dump_sql(session, opts, out, stats, log);
    out.close();
    if (!out) {
        log << fmt::format("[dump] write error: {}\n", path);
        return false;
    }
    return ok;
}

} // namespace sqltiny::cli
