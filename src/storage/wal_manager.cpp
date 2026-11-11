#include "storage/wal_manager.h"
#include "storage/snapshot.h"
#include "catalog/catalog.h"
#include "storage/table.h"
#include "parser/ast.h"
#include <fmt/format.h>
#include <cstdio>
#include <atomic>

namespace sqltiny {

std::string WalManager::snapshot_path() const {
    // sql.wal → sql.data
    auto dot = wal_path_.rfind('.');
    if (dot == std::string::npos) return wal_path_ + ".data";
    return wal_path_.substr(0, dot) + ".data";
}

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

    // bootstrap: snapshot + WAL 增量
    bootstrap(catalog);
    return true;
}

void WalManager::bootstrap(catalog::Catalog& catalog) {
    std::string snap_path = snapshot_path();
    persist::WalSnapshot snap(snap_path);
    uint64_t snap_wal_offset = 0;
    int64_t rows_rebuilt = -1;

    if (snap.file_exists() && snap.is_valid()) {
        rows_rebuilt = snap.read_and_rebuild(catalog, &snap_wal_offset);
        if (rows_rebuilt >= 0) {
            fmt::print("[Snapshot] rebuilt OK: {} rows (wal_offset={})\n", rows_rebuilt, snap_wal_offset);
            checkpoint_wal_offset_ = snap_wal_offset;
        } else {
            fmt::print("[Snapshot] CRC failed, ignoring — replay WAL from scratch\n");
            checkpoint_wal_offset_ = 0;
        }
    } else {
        fmt::print("[Snapshot] no snapshot — replay WAL from scratch\n");
        checkpoint_wal_offset_ = 0;
    }

    // 现在 replay 从 checkpoint_wal_offset_ 之后的 WAL 增量
    // 简化实现:WalReader 从头读,但跳过 checkpoint_wal_offset_ 之前的字节
    // (M5 第一版简单做:从头 replay — 如果有 snapshot 就先清,如果没有就直接 replay)
    if (rows_rebuilt >= 0) {
        // 有 snapshot 重建过了 — 追加 replay WAL
        // 但现在 replay() 是从头读,我们需要让它 seek 到 checkpoint_wal_offset_ 之后
        // 临时方案:直接从头 replay,用幂等的 CreateTable/DropTable
        // (replay 里 if_not_exists / if_exists 已经幂等,重复应用 DDL 不破坏)
        bool truncated = false;
        size_t applied = replay(catalog, truncated);
        fmt::print("[WAL] post-snapshot replay: {} records applied {}\n",
                   applied, truncated ? "(truncated)" : "(clean)");
    } else {
        bool truncated = false;
        size_t applied = replay(catalog, truncated);
        fmt::print("[WAL] replay from scratch: {} records applied {}\n",
                   applied, truncated ? "(truncated)" : "(clean)");
    }
}

bool WalManager::do_checkpoint(catalog::Catalog& catalog) {
    if (!enabled()) return true;
    std::lock_guard<std::mutex> g(mu_);

    // 1. 记录当前 WAL 偏移
    FILE* fh = std::fopen(wal_path_.c_str(), "rb+");
    if (!fh) return false;
    std::fseek(fh, 0, SEEK_END);
    uint64_t wal_offset = static_cast<uint64_t>(std::ftell(fh));
    std::fclose(fh);

    // 2. 写 snapshot
    persist::WalSnapshot snap(snapshot_path());
    if (!snap.write(catalog, wal_offset)) return false;

    // 3. 写 CheckpointStart + EndOfFile
    persist::WalRecord cps{ persist::RecordType::CheckpointStart, 0, persist::codec::encode_u64(wal_offset) };
    writer_->append(cps);
    persist::WalRecord eof{ persist::RecordType::EndOfFile, 0, {} };
    writer_->append(eof);
    writer_->close(); writer_.reset();

    // 4. 截断 WAL (重开从头写)
    fh = std::fopen(wal_path_.c_str(), "w+b");
    if (!fh) return false;
    std::fclose(fh);

    // 5. 重开 writer
    writer_ = std::make_unique<persist::WalWriter>(wal_path_);
    if (!writer_->open()) return false;
    checkpoint_wal_offset_ = 0;
    checkpoint_pending_.store(false);
    fmt::print("[WAL] checkpoint done: WAL truncated, snapshot at {}\n", snapshot_path());
    return true;
}

void WalManager::close() {
    if (!writer_ || !writer_->is_open()) return;
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
                auto* tbl = catalog.get_table(*dn + "." + *tn);
                if (tbl) {
                    tbl->set_auto_inc_cursor(static_cast<int64_t>(cur));
                }
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
