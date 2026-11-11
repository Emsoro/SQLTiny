#include "catalog/catalog.h"
#include <fmt/format.h>
#include <algorithm>

namespace sqltiny::catalog {

std::string Catalog::lower_name(const std::string& s) {
    std::string lower; lower.resize(s.size());
    std::transform(s.begin(), s.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower;
}

Catalog::Catalog() {
    databases_["default"] = DbTables{};
}

Catalog::DbTables* Catalog::get_db(const std::string& name) {
    auto it = databases_.find(lower_name(name));
    if (it == databases_.end()) return nullptr;
    return &it->second;
}
const Catalog::DbTables* Catalog::get_db(const std::string& name) const {
    auto it = databases_.find(lower_name(name));
    if (it == databases_.end()) return nullptr;
    return &it->second;
}

Catalog::DbTables* Catalog::current_db() {
    auto it = databases_.find(current_database_);
    if (it == databases_.end()) return nullptr;
    return &it->second;
}
const Catalog::DbTables* Catalog::current_db() const {
    auto it = databases_.find(current_database_);
    if (it == databases_.end()) return nullptr;
    return &it->second;
}

// ---------- Database ----------

bool Catalog::create_database(const std::string& name, bool if_not_exists) {
    std::string low = lower_name(name);
    if (databases_.count(low)) {
        if (if_not_exists) return true;
        return false;
    }
    databases_[low] = DbTables{};
    return true;
}

bool Catalog::drop_database(const std::string& name, bool if_exists) {
    std::string low = lower_name(name);
    auto it = databases_.find(low);
    if (it == databases_.end()) {
        if (if_exists) return true;
        return false;
    }
    // 不能删当前库
    if (low == current_database_) {
        return false;
    }
    databases_.erase(it);
    return true;
}

bool Catalog::database_exists(const std::string& name) const {
    return databases_.count(lower_name(name)) > 0;
}

bool Catalog::switch_database(const std::string& name) {
    std::string low = lower_name(name);
    if (!databases_.count(low)) return false;
    current_database_ = low;
    return true;
}

std::vector<std::string> Catalog::list_databases() const {
    std::vector<std::string> out;
    out.reserve(databases_.size());
    for (auto& [name, _] : databases_) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

// ---------- Table ----------

bool Catalog::create_table(const ast::CreateTableStmt& stmt) {
    // 解析 db.table 格式:指定了 db 就到那个库建,没指定就到 current_database
    std::string db_name = current_database_;
    std::string table_name = stmt.table_name;
    auto dot = table_name.find('.');
    if (dot != std::string::npos) {
        db_name = lower_name(table_name.substr(0, dot));
        table_name = table_name.substr(dot + 1);
    }
    DbTables* db = get_db(db_name);
    if (!db) return false;

    std::string lower = lower_name(table_name);
    if (db->tables.count(lower)) {
        if (stmt.if_not_exists) return true;
        return false;
    }

    storage::TableDef def;
    def.name = lower;
    def.columns = stmt.columns;
    def.primary_key_cols = stmt.primary_key;

    auto t = std::make_unique<storage::Table>(def);
    db->tables[lower] = std::move(t);
    return true;
}

bool Catalog::drop_table(const std::string& name, bool if_exists) {
    // 支持 db.table 格式
    std::string db_name = current_database_;
    std::string table_name = name;
    auto dot = table_name.find('.');
    if (dot != std::string::npos) {
        db_name = lower_name(table_name.substr(0, dot));
        table_name = table_name.substr(dot + 1);
    }
    DbTables* db = get_db(db_name);
    if (!db) return if_exists;

    std::string lower = lower_name(table_name);
    auto it = db->tables.find(lower);
    if (it == db->tables.end()) {
        if (if_exists) return true;
        return false;
    }
    db->tables.erase(it);
    return true;
}

bool Catalog::table_exists(const std::string& name) const {
    const DbTables* db = current_db();
    if (!db) return false;
    return db->tables.count(lower_name(name)) > 0;
}

storage::Table* Catalog::get_table(const std::string& name) {
    DbTables* db = current_db();
    if (!db) return nullptr;
    std::string lower = lower_name(name);

    // 支持 db.table 格式
    auto dot = lower.find('.');
    if (dot != std::string::npos) {
        std::string db_name = lower.substr(0, dot);
        std::string tbl_name = lower.substr(dot + 1);
        DbTables* target = get_db(db_name);
        if (!target) return nullptr;
        auto it = target->tables.find(tbl_name);
        if (it == target->tables.end()) return nullptr;
        return it->second.get();
    }

    auto it = db->tables.find(lower);
    if (it == db->tables.end()) return nullptr;
    return it->second.get();
}

const storage::Table* Catalog::get_table(const std::string& name) const {
    const DbTables* db = current_db();
    if (!db) return nullptr;
    std::string lower = lower_name(name);

    auto dot = lower.find('.');
    if (dot != std::string::npos) {
        std::string db_name = lower.substr(0, dot);
        std::string tbl_name = lower.substr(dot + 1);
        const DbTables* target = get_db(db_name);
        if (!target) return nullptr;
        auto it = target->tables.find(tbl_name);
        if (it == target->tables.end()) return nullptr;
        return it->second.get();
    }

    auto it = db->tables.find(lower);
    if (it == db->tables.end()) return nullptr;
    return it->second.get();
}

std::vector<std::string> Catalog::table_names() const {
    std::vector<std::string> out;
    const DbTables* db = current_db();
    if (!db) return out;
    out.reserve(db->tables.size());
    for (auto& [name, _] : db->tables) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace sqltiny::catalog
