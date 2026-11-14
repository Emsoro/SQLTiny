#!/usr/bin/env python3
"""pymysql seq 插桩(文件日志版):定位第一个包序错位点。"""
import runpy, sys, traceback
from pymysql.connections import Connection

log = open('d:/Cplusplus/SQLTiny/_trace.log', 'w', buffering=1, encoding='utf-8')

orig_read = Connection._read_packet
orig_exec = Connection._execute_command
_n = [0]

def read(self, *a, **kw):
    expected = self._next_seq_id
    try:
        pkt = orig_read(self, *a, **kw)
        _n[0] += 1
        seq = getattr(pkt, 'seq_id', getattr(pkt, 'seq', None))
        head = b''
        try:
            head = bytes(pkt._data[:1]) if hasattr(pkt, '_data') else b''
        except Exception:
            pass
        log.write("  <- #%d expected=%s got=%s head=%s\n" % (_n[0], expected, seq, head.hex()))
        return pkt
    except Exception as e:
        log.write("  !! READ EXCEPTION expected=%s: %r\n" % (expected, e))
        raise

def exec_(self, command, sql):
    log.write("  -> CMD=%s next_seq=%s sql=%r\n" % (command, self._next_seq_id, sql[:80]))
    return orig_exec(self, command, sql)

Connection._read_packet = read
Connection._execute_command = exec_

try:
    runpy.run_path('tests/test_full_flow.py', run_name='__main__')
except SystemExit as e:
    log.write("SystemExit %s\n" % e)
except BaseException:
    log.write("TRACEBACK:\n" + traceback.format_exc())
finally:
    log.close()
