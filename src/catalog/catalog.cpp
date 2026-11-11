#include "catalog/catalog.h"
#include <fmt/format.h>
#include <algorithm>

namespace sqltiny::catalog {

bool Catalog::create_table(const ast::CreateTableStmt& stmt) {
    std::string lower; lower.resize(stmt.table_name.size());
    std::transform(stmt.table_name.begin(), stmt.table_name.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (table_exists(lower)) {
        if (stmt.if_not_exists) return true;  // 假装成功
        return false;
    }

    storage::TableDef def;
    def.name = lower;
    def.columns = stmt.columns;
    def.primary_key_cols = stmt.primary_key;

    auto t = std::make_unique<storage::Table>(def);
    tables_[lower] = std::move(t);
    return true;
}

bool Catalog::drop_table(const std::string& name, bool if_exists) {
    std::string lower; lower.resize(name.size());
    std::transform(name.begin(), name.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto it = tables_.find(lower);
    if (it == tables_.end()) {
        if (if_exists) return true;
        return false;
    }
    tables_.erase(it);
    return true;
}

bool Catalog::table_exists(const std::string& name) const {
    std::string lower; lower.resize(name.size());
    std::transform(name.begin(), name.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return tables_.count(lower) > 0;
}

storage::Table* Catalog::get_table(const std::string& name) {
    std::string lower; lower.resize(name.size());
    std::transform(name.begin(), name.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto it = tables_.find(lower);
    if (it == tables_.end()) return nullptr;
    return it->second.get();
}
const storage::Table* Catalog::get_table(const std::string& name) const {
    std::string lower; lower.resize(name.size());
    std::transform(name.begin(), name.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto it = tables_.find(lower);
    if (it == tables_.end()) return nullptr;
    return it->second.get();
}

std::vector<std::string> Catalog::table_names() const {
    std::vector<std::string> out;
    out.reserve(tables_.size());
    for (auto& [name, _] : tables_) out.push_back(name);
    return out;
}

} // namespace sqltiny::catalog
