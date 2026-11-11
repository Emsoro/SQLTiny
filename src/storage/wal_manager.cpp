#include "storage/wal_manager.h"
#include "catalog/catalog.h"
#include "storage/table.h"
#include "parser/ast.h"
#include <fmt/format.h>
#include <cstdio>
#include <atomic>

namespace sqltiny {

bool WalManager::open(const std::string& wal_path, catalog::Catalog& catalog) {
    wal_path_ = wal_path;
    if (wal_path.empty()) {
        fmt::print("[WAL] disabled (no data-dir specified)\n");
        return true;
    }

    writer_ = std::make_unique<persist::WalWriter>(wal_path);
    if (!writer_->open()) {
        fmt::print(stderr, "[WAL] failed to open/create file: {}\n", wal_path);
        writer_.reset();
        wal_path_.clear();
        return false;
    }

    // 尝试 replay 现有内容
    bool truncated = false;
    size_t applied = replay(catalog, truncated);
    if (applied > 0) {
        fmt::print("[WAL] replay complete: {} records applied {}\n",
                   applied, truncated ? "(truncated at crash point)" : "(clean)");
    } else {
        fmt::print("[WAL] new or empty: {}\n", wal_path);
    }
    return true;
}

void WalManager::close() {
    if (!writer_ || !writer_->is_open()) return;
    // 写 EndOfFile 作为干净关闭标记
    persist::WalRecord eof{ persist::RecordType::EndOfFile, 0, {} };
    append(eof);
    writer_->close();
    fmt::print("[WAL] closed cleanly\n");
}

// ============ Append helpers ============
// 每条都要构造 record + payload, 然后走 append()

bool WalManager::append(const persist::WalRecord& rec) {
    std::lock_guard<std::mutex> g(mu_);
    if (!writer_ || !writer_->is_open()) return false;
    return writer_->append(rec);
}

static persist::WalRecord _mk(persist::RecordType t, std::vector<uint8_t> payload) {
    return persist::WalRecord{ t, 0, std::move(payload) };
}

bool WalManager::append_create_database(const std::string& db) {
    auto p = persist::codec::encode_string(db);
    return append(_mk(persist::RecordType::CreateDatabase, std::move(p)));
}
bool WalManager::append_drop_database(const std::string& db) {
    auto p = persist::codec::encode_string(db);
    return append(_mk(persist::RecordType::DropDatabase, std::move(p)));
}
bool WalManager::append_use_database(const std::string& db) {
    auto p = persist::codec::encode_string(db);
    return append(_mk(persist::RecordType::UseDatabase, std::move(p)));
}
bool WalManager::append_create_table(const std::string& db, const storage::TableDef& def) {
    std::vector<uint8_t> p;
    auto dn = persist::codec::encode_string(db);
    auto tn = persist::codec::encode_table_def(def);
    p.insert(p.end(), dn.begin(), dn.end());
    p.insert(p.end(), tn.begin(), tn.end());
    return append(_mk(persist::RecordType::CreateTable, std::move(p)));
}
bool WalManager::append_drop_table(const std::string& db, const std::string& table) {
    std::vector<uint8_t> p;
    auto dn = persist::codec::encode_string(db);
    auto tn = persist::codec::encode_string(table);
    p.insert(p.end(), dn.begin(), dn.end());
    p.insert(p.end(), tn.begin(), tn.end());
    return append(_mk(persist::RecordType::DropTable, std::move(p)));
}
bool WalManager::append_insert_row(const std::string& db, const std::string& table,
                                   int64_t row_id, const storage::Row& row) {
    std::vector<uint8_t> p;
    auto dn = persist::codec::encode_string(db);
    auto tn = persist::codec::encode_string(table);
    auto rid = persist::codec::encode_u64(static_cast<uint64_t>(row_id));
    auto rowb = persist::codec::encode_row(row);
    p.insert(p.end(), dn.begin(), dn.end());
    p.insert(p.end(), tn.begin(), tn.end());
    p.insert(p.end(), rid.begin(), rid.end());
    p.insert(p.end(), rowb.begin(), rowb.end());
    return append(_mk(persist::RecordType::InsertRow, std::move(p)));
}
bool WalManager::append_delete_row(const std::string& db, const std::string& table,
                                   const storage::Row& pk_values) {
    std::vector<uint8_t> p;
    auto dn = persist::codec::encode_string(db);
    auto tn = persist::codec::encode_string(table);
    auto pk = persist::codec::encode_row(pk_values);
    p.insert(p.end(), dn.begin(), dn.end());
    p.insert(p.end(), tn.begin(), tn.end());
    p.insert(p.end(), pk.begin(), pk.end());
    return append(_mk(persist::RecordType::DeleteRow, std::move(p)));
}
bool WalManager::append_update_row(const std::string& db, const std::string& table,
                                   const storage::Row& pk_values, const storage::Row& new_row) {
    std::vector<uint8_t> p;
    auto dn = persist::codec::encode_string(db);
    auto tn = persist::codec::encode_string(table);
    auto pk = persist::codec::encode_row(pk_values);
    auto nr = persist::codec::encode_row(new_row);
    p.insert(p.end(), dn.begin(), dn.end());
    p.insert(p.end(), tn.begin(), tn.end());
    p.insert(p.end(), pk.begin(), pk.end());
    p.insert(p.end(), nr.begin(), nr.end());
    return append(_mk(persist::RecordType::UpdateRow, std::move(p)));
}
bool WalManager::append_auto_inc_update(const std::string& db, const std::string& table, uint64_t new_cursor) {
    std::vector<uint8_t> p;
    auto dn = persist::codec::encode_string(db);
    auto tn = persist::codec::encode_string(table);
    auto cur = persist::codec::encode_u64(new_cursor);
    p.insert(p.end(), dn.begin(), dn.end());
    p.insert(p.end(), tn.begin(), tn.end());
    p.insert(p.end(), cur.begin(), cur.end());
    return append(_mk(persist::RecordType::AutoIncUpdate, std::move(p)));
}

// ============ Replay ============
size_t WalManager::replay(catalog::Catalog& catalog, bool& out_truncated) {
    out_truncated = false;
    std::string path = wal_path_;
    persist::WalReader rd(path);
    if (!rd.open()) {
        // 没有现成 WAL 或格式坏 —— 视为全新启动
        return 0;
    }

    // 打开后 catalog 已经有一个 default DB (Catalog 构造时初始化了 default)
    // 但我们先把它清掉 —— replay 从 CreateDatabase 重建
    // 实际上 default 是 Catalog 构造函数硬编码创建的,我们保留它,但 replay 里如果有 DROP default 则会 drop
    // 更简单:replay 从头到尾应用,DROP default 会失败(if_exists)或被忽略。这里让 catalog 保留初始 default,
    // 所有 replay 的 CreateDatabase/DropDatabase/CreateTable/DropTable 正常走。

    size_t applied = 0;

    while (true) {
        bool truncated = false;
        auto rec = rd.read_next(&truncated);
        if (truncated) {
            out_truncated = true;
            break;
        }
        if (!rec) break;

        auto buf = std::span<const uint8_t>(rec->payload);

        switch (rec->type) {
            case persist::RecordType::CreateDatabase: {
                auto n = persist::codec::decode_string(buf);
                if (n) catalog.create_database(*n, /*if_not_exists=*/true);
                break;
            }
            case persist::RecordType::DropDatabase: {
                auto n = persist::codec::decode_string(buf);
                if (n) catalog.drop_database(*n, /*if_exists=*/true);
                break;
            }
            case persist::RecordType::UseDatabase: {
                auto n = persist::codec::decode_string(buf);
                if (n) catalog.switch_database(*n);
                break;
            }
            case persist::RecordType::CreateTable: {
                auto dn = persist::codec::decode_string(buf);
                auto tdef = persist::codec::decode_table_def(buf);
                if (dn && tdef) {
                    ast::CreateTableStmt stmt;
                    stmt.if_not_exists = true;  // replay 时幂等
                    stmt.table_name = dn->empty() ? tdef->name : (*dn + "." + tdef->name);
                    stmt.columns = tdef->columns;
                    stmt.primary_key = tdef->primary_key_cols;
                    catalog.create_table(stmt);
                }
                break;
            }
            case persist::RecordType::DropTable: {
                auto dn = persist::codec::decode_string(buf);
                auto tn = persist::codec::decode_string(buf);
                if (dn && tn) {
                    std::string qualified = *dn + "." + *tn;
                    catalog.drop_table(qualified, /*if_exists=*/true);
                }
                break;
            }
            case persist::RecordType::InsertRow: {
                auto dn = persist::codec::decode_string(buf);
                auto tn = persist::codec::decode_string(buf);
                if (!dn || !tn) break;
                uint64_t rid_u64 = persist::codec::decode_u64(buf);
                int64_t rid = static_cast<int64_t>(rid_u64);
                auto row = persist::codec::decode_row(buf);
                if (!row) break;
                auto* tbl = catalog.get_table(*dn + "." + *tn);
                if (tbl) {
                    tbl->insert(*row);
                }
                break;
            }
            case persist::RecordType::DeleteRow: {
                auto dn = persist::codec::decode_string(buf);
                auto tn = persist::codec::decode_string(buf);
                auto pk = persist::codec::decode_row(buf);
                if (dn && tn && pk) {
                    auto* tbl = catalog.get_table(*dn + "." + *tn);
                    if (tbl) {
                        tbl->delete_by_pk(*pk);
                    }
                }
                break;
            }
            case persist::RecordType::UpdateRow: {
                auto dn = persist::codec::decode_string(buf);
                auto tn = persist::codec::decode_string(buf);
                auto pk = persist::codec::decode_row(buf);
                auto nr = persist::codec::decode_row(buf);
                if (dn && tn && pk && nr) {
                    auto* tbl = catalog.get_table(*dn + "." + *tn);
                    if (tbl) {
                        tbl->delete_by_pk(*pk);
                        tbl->insert(*nr);
                    }
                }
                break;
            }
            case persist::RecordType::AutoIncUpdate: {
                auto dn = persist::codec::decode_string(buf);
                auto tn = persist::codec::decode_string(buf);
                if (!dn || !tn) break;
                uint64_t cur = persist::codec::decode_u64(buf);
                // AutoIncrement 的 cursor 调整 — Table 没有直接 setter,这里跳过
                // (auto_increment 不影响数据正确性,只是重启后从更大的值开始)
                break;
            }
            case persist::RecordType::EndOfFile: {
                // 干净关闭标记 — 不再应用后面的 record (理论上没有了)
                applied++;
                goto done;
            }
            default:
                break;  // 未知 record type 跳过
        }
        applied++;
    }

done:
    rd.close();
    return applied;
}

} // namespace sqltiny
