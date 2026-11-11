#include "storage/table.h"
#include <algorithm>

namespace sqltiny::storage {

Row Table::pk_from_row(const Row& row) const {
    Row pk;
    if (def_.primary_key_cols.empty()) return pk;
    for (auto& col_list : def_.primary_key_cols) {
        for (auto& cname : col_list) {
            int idx = def_.column_index(cname);
            if (idx >= 0 && idx < static_cast<int>(row.size())) pk.push_back(row[idx]);
        }
    }
    return pk;
}

Key Table::pk_key(const Row& pk) const { return Key(pk); }

Table::Table(TableDef def) : def_(std::move(def)) {
    if (!def_.primary_key_cols.empty()) pk_index_ = std::make_unique<IndexAvl>(true /*unique*/);
}

bool Table::insert(const Row& row, int64_t* out_row_id, uint64_t createSCN) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (def_.primary_key_cols.empty()) {
        int64_t rid = next_row_id_++;
        row_map_[rid] = row;
        versions_[rid] = {createSCN, 0};
        if (out_row_id) *out_row_id = rid;
        return true;
    }
    Row pk = pk_from_row(row);
    Key key = pk_key(pk);
    bool inserted = pk_index_->insert(key, next_row_id_);
    if (!inserted) return false;
    int64_t rid = next_row_id_++;
    row_map_[rid] = row;
    versions_[rid] = {createSCN, 0};
    for (auto& col_list : def_.primary_key_cols) {
        for (auto& cname : col_list) {
            int idx = def_.column_index(cname);
            if (idx < 0 || idx >= static_cast<int>(row.size())) continue;
            if (def_.columns[idx].is_auto_increment && row[idx].is_int()) {
                auto_inc_cursor_ = std::max(auto_inc_cursor_, row[idx].v_int);
            }
        }
    }
    if (out_row_id) *out_row_id = rid;
    return true;
}

int64_t Table::next_auto_increment() {
    std::unique_lock<std::shared_mutex> lock(mu_);
    return ++auto_inc_cursor_;
}

bool Table::delete_by_pk(const Row& pk_values, uint64_t deleteSCN) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (!pk_index_) return false;
    Key key(pk_values);
    auto rid_opt = pk_index_->find(key);
    if (!rid_opt) return false;
    int64_t rid = *rid_opt;
    if (deleteSCN == 0) {
        // 物理删除 (M1-M4 兼容)
        pk_index_->remove(key, rid);
        row_map_.erase(rid);
        versions_.erase(rid);
    } else {
        // MVCC 逻辑删除:标记 deleteSCN,物理保留到 checkpoint 压缩
        pk_index_->remove(key, rid);           // 从索引拿掉,新查询看不到
        versions_[rid].deleteSCN = deleteSCN;   // 但 row_map 里还在
    }
    return true;
}

std::optional<int64_t> Table::find_row_id_by_pk(const Row& pk_values) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (!pk_index_) return std::nullopt;
    return pk_index_->find(Key(pk_values));
}

void Table::for_each(const RowCallback& cb) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    for (auto& [rid, row] : row_map_) {
        if (!cb(rid, row)) break;
    }
}

void Table::for_each_visible(uint64_t querySCN, const RowCallback& cb) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (querySCN == 0) {
        // 退化:遍历全部可见行 (等价 for_each, 但不调 for_each 避免同线程重复加锁)
        for (auto& [rid, row] : row_map_) {
            if (!cb(rid, row)) break;
        }
        return;
    }
    for (auto& [rid, row] : row_map_) {
        auto it = versions_.find(rid);
        uint64_t cs = (it != versions_.end()) ? it->second.createSCN : 0;
        uint64_t ds = (it != versions_.end()) ? it->second.deleteSCN : 0;
        if (cs == 0 || cs <= querySCN) {
            if (ds == 0 || ds > querySCN) {
                if (!cb(rid, row)) break;
            }
        }
    }
}

std::vector<int64_t> Table::all_row_ids(uint64_t querySCN) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    std::vector<int64_t> out;
    if (querySCN == 0) {
        out.reserve(row_map_.size());
        for (auto& [rid, _] : row_map_) out.push_back(rid);
        return out;
    }
    for (auto& [rid, row] : row_map_) {
        auto it = versions_.find(rid);
        uint64_t cs = (it != versions_.end()) ? it->second.createSCN : 0;
        uint64_t ds = (it != versions_.end()) ? it->second.deleteSCN : 0;
        if ((cs == 0 || cs <= querySCN) && (ds == 0 || ds > querySCN)) {
            out.push_back(rid);
        }
    }
    return out;
}

std::optional<Row> Table::get_row(int64_t row_id) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    auto it = row_map_.find(row_id);
    if (it == row_map_.end()) return std::nullopt;
    return it->second;
}

Table::VersionInfo Table::get_version(int64_t row_id) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    auto it = versions_.find(row_id);
    return (it != versions_.end()) ? it->second : VersionInfo{0, 0};
}
void Table::set_version(int64_t row_id, VersionInfo vi) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    versions_[row_id] = vi;
}

} // namespace sqltiny::storage
