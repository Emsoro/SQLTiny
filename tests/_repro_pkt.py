#!/usr/bin/env python3
"""精确复现 test_full_flow 的查询序列,检测每个响应是否带多余包(leftover)。"""
import socket, struct, sys, select
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HOST, PORT = '127.0.0.1', 3306

def recv_packet(s):
    hdr = b''
    while len(hdr) < 4:
        b = s.recv(4 - len(hdr))
        if not b: raise ConnectionError('EOF')
        hdr += b
    ln = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16)
    seq = hdr[3]
    body = b''
    while len(body) < ln:
        b = s.recv(min(65536, ln - len(body)))
        if not b: raise ConnectionError('EOF')
        body += b
    return seq, body

def send_cmd(s, sql):
    payload = b'\x03' + sql.encode()
    s.sendall(struct.pack('<I', len(payload))[:3] + b'\x00' + payload)  # 客户端每命令 seq 重置 0

def read_response(s, label):
    seq, body = recv_packet(s)
    print(f"  [{label}] p1 seq={seq} len={len(body)} head=0x{body[0]:02x}")
    if body[0] in (0x00, 0xff):
        check_leftover(s, label); return
    ncols = body[0]
    for i in range(ncols):
        seq, body = recv_packet(s)
        print(f"  [{label}] coldef seq={seq} len={len(body)}")
    seq, body = recv_packet(s)          # 列阶段 EOF
    print(f"  [{label}] col-eof seq={seq} len={len(body)} head=0x{body[0]:02x}")
    nrows = 0
    while True:
        seq, body = recv_packet(s)
        if body[0] == 0xfe and len(body) < 9:
            print(f"  [{label}] term-eof seq={seq} len={len(body)} rows={nrows}")
            break
        nrows += 1
        if nrows <= 2:
            print(f"  [{label}] row seq={seq} len={len(body)} {body[:30].decode(errors='replace')!r}")
    check_leftover(s, label)

def check_leftover(s, label):
    r, _, _ = select.select([s], [], [], 0.3)
    if r:
        extra = s.recv(65536)
        print(f"  !! [{label}] LEFTOVER {len(extra)} bytes: {extra[:60].hex()}")
    else:
        print(f"  [{label}] no leftover")

s = socket.create_connection((HOST, PORT), timeout=5)
seq, _ = recv_packet(s)
caps = 0x00000200 | 0x00008000
hs = struct.pack('<IIB23x', caps, 16777216, 45) + b'root\x00' + b'\x00' + b'mysql_native_password\x00'
s.sendall(struct.pack('<I', len(hs))[:3] + bytes([seq + 1]) + hs)
seq, resp = recv_packet(s)
print(f"auth -> 0x{resp[0]:02x}")

for ddl in ["DROP TABLE IF EXISTS orders",
            "CREATE TABLE orders(order_id INT PRIMARY KEY AUTO_INCREMENT, user_id INT, total DECIMAL(10,2))",
            "INSERT INTO orders(user_id,total) VALUES (1,100.50),(1,200.00),(2,50.00),(3,300.00),(4,75.25)"]:
    send_cmd(s, ddl); read_response(s, ddl[:28])

QUERIES = [
    "SELECT COUNT(user_id) FROM orders",                                   # 1col1row
    "SELECT user_id, SUM(total) FROM orders GROUP BY user_id ORDER BY user_id",  # check_group
    "SELECT user_id FROM orders GROUP BY user_id HAVING SUM(total) > 100 ORDER BY user_id",  # 失败查询
    "SELECT 1",
]
for q in QUERIES:
    send_cmd(s, q); read_response(s, q[:52])
s.close()
print("REPRO2_DONE")
