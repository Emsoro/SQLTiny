#!/usr/bin/env python3
"""修复验证:① Navicat 式握手(CLIENT_CONNECT_ATTRS+PLUGIN_AUTH)不再断连
② DATE 的 MIN/MAX 返回完整日期 ③ 整型 SUM 返回数值类型而非字符串。"""
import socket, struct, pymysql

HOST, PORT = '127.0.0.1', 3307

# ---------- ① 裸 socket 模拟 Navicat 握手(caps=0x00ffa684,带 CONNECT_ATTRS) ----------
def recv_packet(s):
    hdr = b''
    while len(hdr) < 4:
        hdr += s.recv(4 - len(hdr))
    ln = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16)
    seq = hdr[3]
    body = b''
    while len(body) < ln:
        body += s.recv(ln - len(body))
    return seq, body

def send_packet(s, seq, payload):
    s.sendall(struct.pack('<I', len(payload))[:3] + bytes([seq]) + payload)

s = socket.create_connection((HOST, PORT), timeout=5)
seq, greeting = recv_packet(s)
caps = 0x00ffa684          # 与 Navicat 实测一致: PLUGIN_AUTH|CONNECT_ATTRS|SESSION_TRACK 位段
plugin = b'mysql_native_password\x00'
auth = b'\x00'             # 空密码: lenenc 长度 0
attrs = b'\x1a'            # lenenc: 26 字节属性串
attrs += b'\x0c_client_name\x00Navicat\x00\x0f_client_version\x0016.1.0\x00'
hs = struct.pack('<IIB23x', caps, 16777216, 45) + b'root\x00' + auth + plugin + attrs
send_packet(s, seq + 1, hs)
seq, resp = recv_packet(s)
ok = resp[0] == 0x00
print(f"handshake(caps=0x{caps:08x} + CONNECT_ATTRS) -> {'OK' if ok else 'FAIL: ' + resp[:60].decode(errors='replace')}")
# 紧接着发查询 —— 若握手残留字节未消费,这里会错位/被掐
send_packet(s, seq + 1, b'\x03select 1')
seq2, r2 = recv_packet(s)
n_cols = r2[0]
print(f"COM_QUERY select 1 -> column_count={n_cols} ({'OK' if n_cols == 1 else 'FAIL'})")
send_packet(s, seq2 + 1, b'\x01')   # COM_QUIT
s.close()
print("TEST1_DONE")

# ---------- ②③ DATE MIN/MAX 与 SUM 类型 ----------
c = pymysql.connect(host=HOST, port=PORT, user='root', password='', database='employees')
cur = c.cursor()
cur.execute("SELECT MIN(hire_date), MAX(hire_date) FROM employees")
mn, mx = cur.fetchone()
print(f"MIN(hire_date)={mn} type={type(mn).__name__}")
print(f"MAX(hire_date)={mx} type={type(mx).__name__}")
cur.execute("SELECT SUM(salary) FROM salaries")
sv = cur.fetchone()[0]
print(f"SUM(salary)={sv} type={type(sv).__name__}")
cur.execute("SELECT MIN(salary), MAX(salary) FROM salaries")
mns, mxs = cur.fetchone()
print(f"MIN(salary)={mns} type={type(mns).__name__}  MAX(salary)={mxs} type={type(mxs).__name__}")
cur.execute("SELECT COALESCE(SUM(salary),0) FROM salaries WHERE emp_no=10001")
cv = cur.fetchone()[0]
print(f"COALESCE(SUM(salary),0) emp10001={cv} type={type(cv).__name__}")
c.close()
print("TEST23_DONE")
