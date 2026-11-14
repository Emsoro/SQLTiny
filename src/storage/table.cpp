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
    if (!def_.primary_key_cols.empty()) pk_index_ = std::make_unique<IndexBPlusTree>(true /*unique*/);
}

static Table::VersionInfo make_version(uint64_t createSCN, uint64_t pendingTxn) {
    return Table::VersionInfo{createSCN, 0, pendingTxn, pendingTxn != 0, false};
}

bool Table::insert(const Row& row, int64_t* out_row_id, uint64_t createSCN,
                   uint64_t pendingTxn) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (def_.primary_key_cols.empty()) {
        int64_t rid = next_row_id_++;
        row_map_[rid] = row;
        versions_[rid] = make_version(createSCN, pendingTxn);
        if (out_row_id) *out_row_id = rid;
        return true;
    }
    Row pk = pk_from_row(row);
    Key key = pk_key(pk);
    bool inserted = pk_index_->insert(key, next_row_id_);
    if (!inserted) return false;
    int64_t rid = next_row_id_++;
    row_map_[rid] = row;
    versions_[rid] = make_version(createSCN, pendingTxn);
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

bool Table::delete_by_pk(const Row& pk_values, uint64_t deleteSCN,
                         uint64_t pendingTxn) {
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
        auto& vi = versions_[rid];
        vi.deleteSCN = deleteSCN;
        if (pendingTxn != 0) {
            vi.pendingTxn = pendingTxn;
            vi.pendingDelete = true;
        }
    }
    return true;
}

void Table::erase_row(int64_t row_id) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    auto it = row_map_.find(row_id);
    if (it == row_map_.end()) return;
    if (pk_index_) pk_index_->remove(Key(pk_from_row(it->second)), row_id);
    row_map_.erase(it);
    versions_.erase(row_id);
}

void Table::restore_row(int64_t row_id) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    auto it = row_map_.find(row_id);
    if (it == row_map_.end()) return;
    if (pk_index_) {
        Key key(pk_from_row(it->second));
        // 回滚期间可能已有另一个事务插入了同主键的行,不能重复占用唯一索引
        if (!pk_index_->find(key)) pk_index_->insert(key, row_id);
    }
    auto& vi = versions_[row_id];
    vi.deleteSCN = 0;
    vi.pendingDelete = false;
    if (!vi.pendingInsert) vi.pendingTxn = 0;
}

void Table::finalize_commit(int64_t row_id, uint64_t commitSCN, bool is_delete) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    auto& vi = versions_[row_id];
    if (is_delete) {
        vi.deleteSCN = commitSCN;
        vi.pendingDelete = false;
    } else {
        vi.createSCN = commitSCN;
        vi.pendingInsert = false;
    }
    if (!vi.pendingInsert && !vi.pendingDelete) vi.pendingTxn = 0;
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

/// MVCC 可见性 (参考 HSQLDB 2.7.4 RowAction.canRead)
///  - 已提交版本: createSCN <= querySCN < deleteSCN
///  - 本事务未提交写: 自己的 INSERT 可见、自己的 DELETE 不可见
///  - 其它事务未提交写: 一律忽略 (四个隔离级别都不允许脏读)
static bool row_visible(const Table::VersionInfo& vi, uint64_t querySCN,
                        uint64_t readerTxn) {
    bool created = (querySCN == 0) || vi.createSCN == 0 || vi.createSCN <= querySCN;
    bool deleted = (querySCN != 0) && vi.deleteSCN != 0 && vi.deleteSCN <= querySCN;
    if (vi.pendingTxn != 0) {
        if (vi.pendingTxn == readerTxn) {
            if (vi.pendingInsert) { created = true;  deleted = false; }
            if (vi.pendingDelete) { created = false; }
        } else {
            if (vi.pendingInsert) created = false;
            if (vi.pendingDelete) deleted = false;
        }
    }
    return created && !deleted;
}

void Table::for_each_visible(uint64_t querySCN, const RowCallback& cb,
                             uint64_t readerTxn) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    for (auto& [rid, row] : row_map_) {
        auto it = versions_.find(rid);
        if (it != versions_.end() && !row_visible(it->second, querySCN, readerTxn)) continue;
        if (!cb(rid, row)) break;
    }
}

void Table::for_each_by_pk_prefix(const types::Value& first_pk_value, uint64_t querySCN,
                                  const RowCallback& cb, uint64_t readerTxn) const {
    if (!pk_index_ || first_pk_value.is_null()) return;
    std::shared_lock<std::shared_mutex> lock(mu_);
    pk_index_->prefix_scan(Key(first_pk_value), [&](const Key&, int64_t rid) {
        auto vit = versions_.find(rid);
        if (vit != versions_.end() && !row_visible(vit->second, querySCN, readerTxn)) return true;
        auto it = row_map_.find(rid);
        if (it == row_map_.end()) return true;
        return cb(rid, it->second);
    });
}

std::vector<int64_t> Table::all_row_ids(uint64_t querySCN, uint64_t readerTxn) const {
    std::shared_lock<std::shared_mutex> lock(mu_);
    std::vector<int64_t> out;
    for (auto& [rid, row] : row_map_) {
        auto it = versions_.find(rid);
        if (it != versions_.end() && !row_visible(it->second, querySCN, readerTxn)) continue;
        out.push_back(rid);
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
