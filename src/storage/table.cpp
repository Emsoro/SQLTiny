#include "storage/table.h"
#include <algorithm>

namespace sqltiny::storage {

Row Table::pk_from_row(const Row& row) const {
    Row pk;
    if (def_.primary_key_cols.empty()) {
        // 无主键 —— 用 row_id 当虚拟 PK
        return pk;
    }
    for (auto& col_list : def_.primary_key_cols) {
        for (auto& cname : col_list) {
            int idx = def_.column_index(cname);
            if (idx >= 0 && idx < static_cast<int>(row.size())) pk.push_back(row[idx]);
        }
    }
    return pk;
}

Key Table::pk_key(const Row& pk) const {
    return Key(pk);
}

Table::Table(TableDef def) : def_(std::move(def)) {
    // 无主键 —— 不建 AVL 索引,后续按 row_id 查找
    if (!def_.primary_key_cols.empty()) {
        pk_index_ = std::make_unique<IndexAvl>(true /*unique*/);
    }
}

bool Table::insert(const Row& row, int64_t* out_row_id) {
    if (def_.primary_key_cols.empty()) {
        // 无主键 —— 直接追加
        int64_t rid = next_row_id_++;
        row_map_[rid] = row;
        if (out_row_id) *out_row_id = rid;
        return true;
    }
    Row pk = pk_from_row(row);
    Key key = pk_key(pk);
    bool inserted = pk_index_->insert(key, next_row_id_);
    if (!inserted) return false;
    int64_t rid = next_row_id_++;
    row_map_[rid] = row;
    // 自增游标
    for (auto& col_list : def_.primary_key_cols) {
        for (auto& cname : col_list) {
            int idx = def_.column_index(cname);
            if (idx < 0 || idx >= static_cast<int>(row.size())) continue;
            auto& col_def = def_.columns[idx];
            if (col_def.is_auto_increment && row[idx].is_int()) {
                auto_inc_cursor_ = std::max(auto_inc_cursor_, row[idx].v_int);
            }
        }
    }
    if (out_row_id) *out_row_id = rid;
    return true;
}

int64_t Table::next_auto_increment() {
    return ++auto_inc_cursor_;
}

bool Table::delete_by_pk(const Row& pk_values) {
    if (!pk_index_) return false;
    Key key(pk_values);
    auto rid_opt = pk_index_->find(key);
    if (!rid_opt) return false;
    int64_t rid = *rid_opt;
    pk_index_->remove(key, rid);
    row_map_.erase(rid);
    return true;
}

std::optional<int64_t> Table::find_row_id_by_pk(const Row& pk_values) const {
    if (!pk_index_) return std::nullopt;
    return pk_index_->find(Key(pk_values));
}

void Table::for_each(const RowCallback& cb) const {
    // 按 row_id 有序扫描
    for (auto& [rid, row] : row_map_) {
        if (!cb(rid, row)) break;
    }
}

std::vector<int64_t> Table::all_row_ids() const {
    std::vector<int64_t> out;
    out.reserve(row_map_.size());
    for (auto& [rid, _] : row_map_) out.push_back(rid);
    return out;
}

std::optional<Row> Table::get_row(int64_t row_id) const {
    auto it = row_map_.find(row_id);
    if (it == row_map_.end()) return std::nullopt;
    return it->second;
}

} // namespace sqltiny::storage
