# 裸 socket 测试 SQLTiny 的 COM_STMT_PREPARE / COM_STMT_EXECUTE / COM_STMT_CLOSE
import socket, struct, sys

HOST, PORT = "127.0.0.1", 3306

def read_packet(s):
    hdr = b""
    while len(hdr) < 4:
        chunk = s.recv(4 - len(hdr))
        if not chunk: raise IOError("closed")
        hdr += chunk
    ln = int.from_bytes(hdr[:3], "little")
    seq = hdr[3]
    payload = b""
    while len(payload) < ln:
        chunk = s.recv(ln - len(payload))
        if not chunk: raise IOError("closed")
        payload += chunk
    return payload, seq

def write_packet(s, payload, seq):
    s.sendall(len(payload).to_bytes(3, "little") + bytes([seq & 0xff]) + payload)

def handshake(user="root", password="", db=""):
    s = socket.create_connection((HOST, PORT), timeout=5)
    greet, seq = read_packet(s)
    assert greet[0] == 10, f"protocol {greet[0]}"
    caps = (0x01 << 0) | (0x01 << 9) | (0x01 << 15)  # LONG_PASSWORD | PROTOCOL_41 | SECURE_CONNECTION
    if db: caps |= (0x01 << 3)
    resp = struct.pack("<IIB", caps, 1 << 24, 8)
    resp += b"\x00" * 23
    resp += user.encode() + b"\x00"
    resp += bytes([len(password)]) + password.encode()  # 空密码 len=0
    if db: resp += db.encode() + b"\x00"
    write_packet(s, resp, seq + 1)
    ok, _ = read_packet(s)
    assert ok[0] == 0x00, f"handshake failed: {ok.hex()}"
    return s

def prepare(s, sql):
    write_packet(s, b"\x16" + sql.encode(), 0)
    pkt, _ = read_packet(s)
    assert pkt[0] == 0x00, f"prepare failed: {pkt.hex()}"
    stmt_id = int.from_bytes(pkt[1:5], "little")
    ncols = int.from_bytes(pkt[5:7], "little")
    nparams = int.from_bytes(pkt[7:9], "little")
    # 协议:参数定义*nparams + 1 个 EOF,列定义*ncols + 1 个 EOF
    seq = 1
    for _ in range(nparams):
        _, seq = read_packet(s)  # param def
    if nparams:
        _, seq = read_packet(s)  # EOF after params
    for _ in range(ncols):
        _, seq = read_packet(s)  # column def
    if ncols:
        _, seq = read_packet(s)  # EOF after cols
    return stmt_id, ncols, nparams

def execute(s, stmt_id, ncols, nparams, params, types):
    # params: list of (already-encoded bytes) 与 types 对应
    nullbm = bytearray((nparams + 7) // 8)
    types_bytes = b""
    values = b""
    for i, (t, val) in enumerate(zip(types, params)):
        if val is None:
            nullbm[i // 8] |= 1 << (i % 8)
        types_bytes += bytes([t, 0x00])  # unsigned flag 0
        if val is not None: values += val
    payload = struct.pack("<IBI", stmt_id, 0, 1)   # stmt_id, flags, iteration_count
    if nparams:
        payload += bytes(nullbm) + b"\x01" + types_bytes + values
    write_packet(s, b"\x17" + payload, 0)
    # 读响应:列数包 → 列定义*ncols → EOF → 行* → EOF
    pkt, _ = read_packet(s)
    if pkt[0] == 0xff:
        errno = int.from_bytes(pkt[1:3], "little")
        return ("ERR", errno, pkt[9:].decode(errors="replace"))
    if ncols == 0:
        assert pkt[0] == 0x00, f"expected OK: {pkt.hex()}"
        affected = pkt[1]
        return ("OK", affected, None)
    colcount = pkt[0]  # <251 lenenc
    assert colcount == ncols, f"colcount {colcount} != {ncols}"
    seq = 1
    for _ in range(ncols):
        _, seq = read_packet(s)
    _, seq = read_packet(s)  # EOF after cols
    rows = []
    while True:
        pkt, _ = read_packet(s)
        if pkt[0] == 0xfe and len(pkt) < 9:
            break
        assert pkt[0] == 0x00, f"expected row: {pkt.hex()}"
        n = ncols
        nullbm = pkt[1:1 + (n + 9) // 8]
        off = 1 + (n + 9) // 8
        row = []
        for i in range(n):
            bit = i + 2
            if nullbm[bit // 8] & (1 << (bit % 8)):
                row.append(None)
                continue
            # 简化:按已知类型解码 (测试里都按 string/long 处理)
            row.append(pkt[off:])
            break
        rows.append(row)
    return ("ROWS", len(rows), rows)

def close_stmt(s, stmt_id):
    write_packet(s, b"\x19" + struct.pack("<I", stmt_id), 0)

def enc_str(v): return len(v).to_bytes(1, "little") + v.encode() if len(v) < 251 else len(v).to_bytes(3, "little") + v.encode()
def enc_i64(v): return struct.pack("<q", v)

def main():
    ok_count = 0
    # 1) 建表 + 预编译 INSERT + EXECUTE 两次
    s = handshake()
    stmt_id, ncols, nparams = prepare(s, "DROP TABLE IF EXISTS ps_wer")
    execute(s, stmt_id, 0, 0, [], [])
    close_stmt(s, stmt_id)
    stmt_id, ncols, nparams = prepare(s, "CREATE TABLE ps_wer(id INT PRIMARY KEY, name VARCHAR(20))")
    execute(s, stmt_id, 0, 0, [], [])
    close_stmt(s, stmt_id)
    stmt_id, ncols, nparams = prepare(s, "INSERT INTO ps_wer VALUES (?, ?)")
    assert nparams == 2, nparams
    r = execute(s, stmt_id, 0, 2, [enc_i64(1), enc_str("wer")], [0x08, 0xfd])
    assert r[0] == "OK" and r[1] == 1, r
    ok_count += 1
    r = execute(s, stmt_id, 0, 2, [enc_i64(2), enc_str("x'y\\z")], [0x08, 0xfd])
    assert r[0] == "OK" and r[1] == 1, r
    ok_count += 1
    close_stmt(s, stmt_id)

    # 2) 预编译 SELECT * WHERE id=?
    stmt_id, ncols, nparams = prepare(s, "SELECT id, name FROM ps_wer WHERE id = ?")
    assert nparams == 1 and ncols == 2, (nparams, ncols)
    r = execute(s, stmt_id, 2, 1, [enc_i64(1)], [0x08])
    assert r[0] == "ROWS" and r[1] == 1, r
    ok_count += 1
    # 3) 无参数 SELECT
    stmt_id2, ncols, nparams = prepare(s, "SELECT COUNT(*) FROM ps_wer")
    assert nparams == 0, nparams
    r = execute(s, stmt_id2, 1, 0, [], [])
    assert r[0] == "ROWS" and r[1] == 1, r
    ok_count += 1
    # 4) NULL 参数
    stmt_id3, ncols, nparams = prepare(s, "SELECT id FROM ps_wer WHERE name = ?")
    r = execute(s, stmt_id3, 1, 1, [None], [0xfd])
    assert r[0] == "ROWS" and r[1] == 0, r
    ok_count += 1
    # 5) UPDATE 带参数
    stmt_id4, ncols, nparams = prepare(s, "UPDATE ps_wer SET name = ? WHERE id = ?")
    r = execute(s, stmt_id4, 0, 2, [enc_str("updated"), enc_i64(2)], [0xfd, 0x08])
    assert r[0] == "OK" and r[1] == 1, r
    ok_count += 1
    # 6) 预编译时语法错误 → ERR
    write_packet(s, b"\x16" + b"SELEC nonsense", 0)
    pkt, _ = read_packet(s)
    assert pkt[0] == 0xff, "expected ERR for bad syntax"
    ok_count += 1
    close_stmt(s, stmt_id)
    close_stmt(s, stmt_id2)
    close_stmt(s, stmt_id3)
    close_stmt(s, stmt_id4)
    # 7) 清理
    stmt_id, ncols, nparams = prepare(s, "DROP TABLE ps_wer")
    execute(s, stmt_id, 0, 0, [], [])
    close_stmt(s, stmt_id)
    s.close()
    print(f"ALL {ok_count} prepared-stmt tests PASSED")

if __name__ == "__main__":
    main()
