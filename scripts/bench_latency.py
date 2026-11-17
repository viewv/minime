#!/usr/bin/env python3
"""bench_latency.py — 测量 bridge→引擎 的端到端延迟与内存基线

测：
  1. CREATE_SESSION 往返
  2. 逐键延迟（每次发一个按键，等到候选更新）
  3. 批量按键（一次发 n 个）到首个候选
  4. 选词往返（到 emit 上屏）
  5. Wine 栈的常驻内存（RSS）
"""
import os
import socket
import struct
import subprocess
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 17890
N = int(sys.argv[2]) if len(sys.argv) > 2 else 5


def varint(v):
    out = b''
    while v >= 0x80:
        out += bytes([(v & 0x7f) | 0x80])
        v >>= 7
    return out + bytes([v])


def f_varint(f, v):
    return bytes([(f << 3) | 0]) + varint(v)


def f_bytes(f, d):
    return bytes([(f << 3) | 2]) + varint(len(d)) + d


class Conn:
    def __init__(self):
        self.pid = 12345
        self.s = socket.create_connection(('127.0.0.1', PORT), timeout=5)
        meta = self.recv(2.0)
        if meta and meta.startswith(b'PID:'):
            self.pid = int(meta[4:])
        self.send_cmd(3)  # CREATE_SESSION
        self.drain(1.0)

    def app_info(self):
        return f_varint(1, self.pid) + f_bytes(4, b'bench') + f_varint(5, 1)

    def send_cmd(self, cmd_type, key=None, commit=None):
        d = f_varint(1, cmd_type)
        if key is not None:
            d += f_bytes(2, key)
        if commit is not None:
            value, cid = commit  # (str, bytes)
            cw = f_bytes(1, value.encode()) + f_bytes(2, cid)
            d += f_bytes(5, cw)
        v1 = f_bytes(1, d) + f_bytes(2, self.app_info())
        payload = f_varint(1, 1) + f_bytes(2, v1)
        self.s.sendall(struct.pack('<I', len(payload)) + payload)

    def recv(self, timeout):
        self.s.settimeout(timeout)
        try:
            hdr = b''
            while len(hdr) < 4:
                c = self.s.recv(4 - len(hdr))
                if not c:
                    return None
                hdr += c
            n = struct.unpack('<I', hdr)[0]
            data = b''
            while len(data) < n:
                c = self.s.recv(n - len(data))
                if not c:
                    return None
                data += c
            return data
        except socket.timeout:
            return None

    def drain(self, seconds):
        end = time.time() + seconds
        got = 0
        while time.time() < end:
            if not self.recv(0.2):
                continue
            got += 1
        return got

    def wait_for(self, want_type, timeout):
        """等一条指定 type 的响应，返回 (耗时, payload)"""
        t0 = time.perf_counter()
        end = t0 + timeout
        while time.perf_counter() < end:
            f = self.recv(max(0.01, end - time.perf_counter()))
            if not f:
                continue
            from google.protobuf import descriptor_pb2
            from google.protobuf import message_factory
            # 轻量: 直接按 protobuf 读 RespBodyV1.detail.type
            try:
                base = RespBase()
                base.ParseFromString(f)
                body = RespBodyV1()
                body.ParseFromString(base.resp)
                for d in body.detail:
                    if d.type == want_type or (want_type == 2 and d.type == 2):
                        return time.perf_counter() - t0, f
            except Exception as e:
                sys.stderr.write('parse err: %s (%d bytes)\n' % (e, len(f)))
        return None, None


def load_desc():
    global RespBase, RespBodyV1
    from google.protobuf import descriptor_pb2
    from google.protobuf import message_factory
    base = os.path.join(os.path.dirname(__file__), '..', 'build')
    fd = descriptor_pb2.FileDescriptorProto()
    fd.ParseFromString(open(os.path.join(base, 'commands.desc'), 'rb').read())
    pool = __import__('google.protobuf.symbol_database', fromlist=['Default']).Default().pool
    for dep, pkg in (('WtStatKeyboardActiveReport.proto', 'com.tencent.wrbus.pb'),
                     ('WtStatCandidateClickReport.proto', 'com.tencent.wrbus.pb')):
        e = descriptor_pb2.FileDescriptorProto()
        e.name = dep
        e.package = pkg
        m = e.message_type.add()
        m.name = dep.replace('.proto', '')
        pool.Add(e)
    pool.Add(fd)
    RespBase = message_factory.GetMessageClass(pool.FindMessageTypeByName('wetype.commands.RespBase'))
    RespBodyV1 = message_factory.GetMessageClass(pool.FindMessageTypeByName('wetype.commands.RespBodyV1'))


def stats(vals):
    if not vals:
        return 'n/a'
    return 'min %.1f / avg %.1f / max %.1f ms' % (
        min(vals) * 1000, sum(vals) / len(vals) * 1000, max(vals) * 1000)


def main():
    load_desc()
    os.system('wineserver -k 2>/dev/null') if False else None

    print('=== 逐键延迟（每键等候选更新） ===')
    conn = Conn()
    word = 'nihao' * 2
    per_key = []
    for ch in word:
        conn.send_cmd(1, key=bytes([(1 << 3) | 0, ord(ch)]) + b'\x10\x40')
        dt, _ = conn.wait_for(2, 3.0)
        per_key.append(dt if dt else 0)
        # 清空响应
        conn.drain(0.05)
    print('  ' + stats(per_key))

    print('=== 批量 5 键（一次性发）到首个候选 ===')
    batch = []
    for _ in range(N):
        conn.send_cmd(1, key=bytes([(1 << 3) | 0, ord('n')]) + b'\x10\x40')
        conn.drain(0.3)
        # 一次发 5 个键
        t0 = time.perf_counter()
        for ch in 'ihao':
            conn.send_cmd(1, key=bytes([(1 << 3) | 0, ord(ch)]) + b'\x10\x40')
        dt, _ = conn.wait_for(2, 3.0)
        batch.append(dt if dt else 0)
        conn.drain(0.3)
    print('  ' + stats(batch))

    print('=== 选词往返（到 emit） ===')
    sel = []
    for _ in range(N):
        conn.send_cmd(1, key=bytes([(1 << 3) | 0, ord('n')]) + b'\x10\x40')
        conn.drain(0.4)
        conn.send_cmd(2, commit=(b'\xe4\xbd\xa0'.decode(), b'\x02'))
        dt, _ = conn.wait_for(1, 3.0)
        sel.append(dt if dt else 0)
        conn.drain(0.2)
    print('  ' + stats(sel))
    conn.s.close()

    print('=== Wine 栈内存 (RSS) ===')
    out = subprocess.run(['ps', '-eo', 'pid,rss,comm,args'], capture_output=True, text=True).stdout
    total = 0
    for line in out.splitlines():
        if any(k in line for k in ('wetype_server.e', 'bridge.exe', 'wineserver', 'winedevice',
                                    'wine-preloader', 'explorer.exe', 'svchost.exe', 'rpcss')):
            p = line.split(None, 3)
            if len(p) >= 3 and p[0].isdigit():
                rss = int(p[1])
                total += rss
                print('  %6d KB  %s' % (rss, line[line.find(p[2]):][:70]))
    print('  合计约 %.0f MB' % (total / 1024.0))


if __name__ == '__main__':
    main()
