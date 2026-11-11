#include "storage/snapshot.h"
#include "storage/persist.h"
#include <fmt/format.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace sqltiny::persist {

WalSnapshot::WalSnapshot(const std::string& data_path) : path_(data_path) {}

bool WalSnapshot::file_exists() const {
    FILE* fh = std::fopen(path_.c_str(), "rb");
    if (!fh) return false;
    std::fclose(fh);
    return true;
}

bool WalSnapshot::is_valid() const {
    FILE* fh = std::fopen(path_.c_str(), "rb");
    if (!fh) return false;
    SnapshotHeader hdr;
    bool ok = (std::fread(&hdr, sizeof(hdr), 1, fh) == 1);
    std::fclose(fh);
    if (!ok) return false;
    return std::memcmp(hdr.magic, "SQTSNAP\0", 8) == 0 && hdr.version == 1;
}

bool WalSnapshot::write(const catalog::Catalog& catalog, uint64_t wal_offset, uint64_t scn) {
    FILE* fh = std::fopen(tmp_path().c_str(), "w+b");
    if (!fh) { fmt::print(stderr, "[Snapshot] cannot open {}\n", tmp_path()); return false; }

    if (!_write_to(const_cast<catalog::Catalog&>(catalog), fh, wal_offset, scn)) {
        std::fclose(fh);
        std::remove(tmp_path().c_str());
        return false;
    }
    std::fclose(fh);

    if (!atomic_replace(path_)) {
        fmt::print(stderr, "[Snapshot] atomic replace failed\n");
        return false;
    }
    fmt::print("[Snapshot] written OK: {}\n", path_);
    return true;
}

int64_t WalSnapshot::read_and_rebuild(catalog::Catalog& out_catalog, uint64_t* out_wal_offset) {
    FILE* fh = std::fopen(path_.c_str(), "rb");
    if (!fh) return -1;
    int64_t rc = _read_from(out_catalog, fh, out_wal_offset);
    std::fclose(fh);
    return rc;
}

bool WalSnapshot::atomic_replace(const std::string& final_path) {
    // Windows 没有 rename 的覆盖语义,先删再 rename
    std::remove(final_path.c_str());
    return std::rename((final_path + ".tmp").c_str(), final_path.c_str()) == 0;
}

// ============== 内部实现 ==============
static void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((v >> (i*8)) & 0xFF);
}
static void put_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back((v >> (i*8)) & 0xFF);
}
static uint32_t get_u32(const uint8_t* p) {
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i) r |= uint32_t(p[i]) << (i*8);
    return r;
}
static uint64_t get_u64(const uint8_t* p) {
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) r |= uint64_t(p[i]) << (i*8);
    return r;
}
static bool fputs_bytes(FILE* fh, const std::vector<uint8_t>& bytes) {
    return std::fwrite(bytes.data(), 1, bytes.size(), fh) == bytes.size();
}

bool WalSnapshot::_write_to(catalog::Catalog& catalog, FILE* fh, uint64_t wal_offset, uint64_t scn) {
    // 先写 header 占位 (后面回填 db_count/record_count)
    SnapshotHeader hdr;
    hdr.version = 1;
    hdr.scn = scn;
    hdr.wal_offset = wal_offset;
    if (std::fwrite(&hdr, sizeof(hdr), 1, fh) != 1) return false;

    uint64_t total_rows = 0;
    uint64_t db_count = 0;

    catalog.for_each_database([&](const std::string& db_name) {
        db_count++;
        auto dn = codec::encode_string(db_name);
        // 写入 db_name
        std::fwrite(dn.data(), 1, dn.size(), fh);
        // 先写 table_count 占位
        long tables_pos = std::ftell(fh);
        uint32_t placeholder = 0;
        std::fwrite(&placeholder, 4, 1, fh);
        uint32_t table_count = 0;

        catalog.for_each_table_in(db_name, [&](const std::string& tname, const storage::Table* tbl) {
            // [TableDef]
            auto tn = codec::encode_string(tname);
            auto td = codec::encode_table_def(tbl->def());
            std::fwrite(tn.data(), 1, tn.size(), fh);
            std::fwrite(td.data(), 1, td.size(), fh);

            // [row_count 4B][Row N]...
            uint32_t row_cnt = static_cast<uint32_t>(tbl->row_count());
            std::fwrite(&row_cnt, 4, 1, fh);
            tbl->for_each([&](int64_t row_id, const storage::Row& row) {
                // [row_id 8B][Row]
                auto idb = codec::encode_u64(static_cast<uint64_t>(row_id));
                auto rb = codec::encode_row(row);
                std::fwrite(idb.data(), 1, idb.size(), fh);
                std::fwrite(rb.data(), 1, rb.size(), fh);
                return true;
            });

            // [auto_inc_cursor 8B]
            auto aib = codec::encode_u64(static_cast<uint64_t>(tbl->auto_inc_cursor()));
            std::fwrite(aib.data(), 1, aib.size(), fh);

            total_rows += row_cnt;
            table_count++;
        });

        // 回填 table_count
        long cur = std::ftell(fh);
        std::fseek(fh, tables_pos, SEEK_SET);
        uint32_t tc = table_count;
        std::fwrite(&tc, 4, 1, fh);
        std::fseek(fh, cur, SEEK_SET);
    });

    // 回填 header
    hdr.db_count = db_count;
    hdr.record_count = total_rows;
    std::fseek(fh, 0, SEEK_SET);
    std::fwrite(&hdr, sizeof(hdr), 1, fh);
    std::fseek(fh, 0, SEEK_END);

    // CRC 尾部
    long before = std::ftell(fh);
    std::fseek(fh, 0, SEEK_SET);
    std::vector<uint8_t> all(static_cast<size_t>(before));
    std::fread(all.data(), 1, all.size(), fh);
    std::fseek(fh, before, SEEK_SET);
    uint32_t crc = crc32(all);
    std::fwrite(&crc, 4, 1, fh);
    return true;
}

int64_t WalSnapshot::_read_from(catalog::Catalog& catalog, FILE* fh, uint64_t* out_wal_offset) {
    SnapshotHeader hdr;
    if (std::fread(&hdr, sizeof(hdr), 1, fh) != 1) return -1;
    if (std::memcmp(hdr.magic, "SQTSNAP\0", 8) != 0 || hdr.version != 1) return -1;

    // 读完整块算 CRC
    std::fseek(fh, 0, SEEK_END);
    long end = std::ftell(fh);
    long before_crc = end - 4;
    if (before_crc <= 0) return -1;
    std::fseek(fh, 0, SEEK_SET);
    std::vector<uint8_t> all(static_cast<size_t>(before_crc));
    if (std::fread(all.data(), 1, all.size(), fh) != static_cast<size_t>(before_crc)) return -1;
    uint32_t actual_crc;
    std::fread(&actual_crc, 4, 1, fh);
    uint32_t calc_crc = crc32(all);
    if (calc_crc != actual_crc) { fmt::print(stderr, "[Snapshot] CRC mismatch, ignoring\n"); return -1; }

    // 重置 catalog
    catalog.clear_all_except_default();
    std::fseek(fh, sizeof(hdr), SEEK_SET);

    // 现在用 buf 逐块读 (避免反复 fseek)
    std::span<const uint8_t> buf = all;
    // 跳过 header
    buf = buf.subspan(sizeof(hdr));

    int64_t total_rows = 0;
    for (uint64_t db_idx = 0; db_idx < hdr.db_count; ++db_idx) {
        auto db_name = codec::decode_string(buf);
        if (!db_name) return -1;
        catalog.create_database(*db_name, /*if_not_exists=*/true);

        if (buf.size() < 4) return -1;
        uint32_t tcnt = get_u32(buf.data());
        buf = buf.subspan(4);

        for (uint32_t ti = 0; ti < tcnt; ++ti) {
            auto tname = codec::decode_string(buf);
            auto tdef = codec::decode_table_def(buf);
            if (!tname || !tdef) return -1;

            // 构造 CreateTableStmt,加到 catalog
            ast::CreateTableStmt stmt;
            stmt.table_name = *db_name + "." + tdef->name;
            stmt.columns = tdef->columns;
            stmt.primary_key = tdef->primary_key_cols;
            stmt.if_not_exists = true;
            catalog.create_table(stmt);

            auto* tbl = catalog.get_table(stmt.table_name);
            if (!tbl) return -1;

            if (buf.size() < 4) return -1;
            uint32_t row_cnt = get_u32(buf.data());
            buf = buf.subspan(4);

            for (uint32_t ri = 0; ri < row_cnt; ++ri) {
                if (buf.size() < 8) return -1;
                buf = buf.subspan(8);  // 跳过 row_id (snapshot replay 让 Table 自动分配也行)
                auto row = codec::decode_row(buf);
                if (!row) return -1;
                tbl->insert(*row);
                total_rows++;
            }

            // auto_inc_cursor
            if (buf.size() < 8) return -1;
            uint64_t aic = get_u64(buf.data());
            buf = buf.subspan(8);
            tbl->set_auto_inc_cursor(static_cast<int64_t>(aic));
        }
    }

    if (out_wal_offset) *out_wal_offset = hdr.wal_offset;
    return total_rows;
}

} // namespace sqltiny::persist
