#!/usr/bin/env python3
"""engined_test.py — 像 fcitx5 插件一样驱动原生服务（或 Wine 栈的 bridge），打印每一步的响应。

用法: engined_test.py [脚本 | --bench 基准文件] [--tcp 端口 | --unix 路径]
脚本是空格分隔的动作：
  nihao      逐个字母发 TEXT_INPUT
  <BS>       退格
  <ESC>      Esc
  <SEL:n>    选当前候选的第 n 个（从 0 起）
  <DEL:n>    删词：把第 n 个候选加进引擎的用户屏蔽表（自定义命令 205）
  <MORE>     要后面的候选（自定义命令 201，原生服务才认）
  <CFG:模糊音掩码,繁体,整句[,表情[,方案,双拼,五笔]]>  发前端配置（自定义命令 202），如 <CFG:0x200,1,1>；表情 0 Emoji / 1 颜文字 / 2 不出；
             方案 0 全拼 / 1 双拼 / 2 五笔，双拼 1..7（搜狗…智能ABC），五笔选项位 1 顶字 / 2 唯一上屏 / 4 拼音混输
  <WAIT:ms>  等待（期间继续读响应）
  <CTX:文字> 发光标前文字（STR_SEND_TEXT_AROUND_CURSOR，104）
  <PRIV:0|1> 发空上下文并带字段 303：1 = 进入隐私输入框（引擎不学选词），0 = 离开
缺省脚本："nihao <SEL:0>"
环境变量 CANDS_N=显示几个候选（默认 9），CANDS_COMMENT=1 在候选后面带上备注（✦ 整句、☁ 云候选、⬡ 常用语）
"""
import os
import socket
import struct
import sys
import time


def varint(v):
    out = b''
    while v >= 0x80:
        out += bytes([(v & 0x7f) | 0x80])
        v >>= 7
    return out + bytes([v])


def f_varint(f, v):
    return varint(f << 3) + varint(v)


def f_bytes(f, d):
    return varint((f << 3) | 2) + varint(len(d)) + d


def read_varint(b, i):
    v = s = 0
    while True:
        c = b[i]
        i += 1
        v |= (c & 0x7f) << s
        s += 7
        if not c & 0x80:
            return v, i


def fields(b):
    i = 0
    while i < len(b):
        tag, i = read_varint(b, i)
        f, w = tag >> 3, tag & 7
        if w == 0:
            v, i = read_varint(b, i)
        elif w == 2:
            n, i = read_varint(b, i)
            v = b[i:i + n]
            i += n
        elif w == 1:
            v = b[i:i + 8]
            i += 8
        elif w == 5:
            v = b[i:i + 4]
            i += 4
        else:
            raise ValueError(w)
        yield f, w, v


class Client:
    def __init__(self, sock):
        self.s = sock
        self.pid = 0
        self.cands = []
        hello = self.recv(2.0)
        assert hello and hello.startswith(b'PID:'), hello
        self.pid = int(hello[4:])

    def recv(self, timeout):
        self.s.settimeout(timeout)
        try:
            hdr = self._read(4)
        except socket.timeout:
            return None
        n = struct.unpack('<I', hdr)[0]
        self.s.settimeout(2.0)
        return self._read(n)

    def _read(self, n):
        out = b''
        while len(out) < n:
            c = self.s.recv(n - len(out))
            if not c:
                raise EOFError
            out += c
        return out

    def send(self, details):
        app = f_varint(1, self.pid) + f_bytes(4, b"engined_test") + f_varint(5, 1)
        v1 = b''.join(f_bytes(1, d) for d in details) + f_bytes(2, app)
        base = f_varint(1, 1) + f_bytes(2, v1)
        self.s.sendall(struct.pack('<I', len(base)) + base)

    def key(self, code, special):
        self.send([f_varint(1, 1) + f_bytes(2, f_varint(1, code) + f_varint(2, special))])

    def context(self, before, after='', priv=0):
        self.send([f_varint(1, 104) + f_bytes(112, before.encode()) + f_bytes(113, after.encode())
                   + f_varint(303, priv)])

    def select(self, i):
        value, cid = self.cands[i][:2]
        self.send([f_varint(1, 2) + f_bytes(5, f_bytes(1, value) + f_bytes(2, cid))])

    def block(self, i):
        value, cid = self.cands[i][:2]
        self.send([f_varint(1, 205) + f_bytes(5, f_bytes(1, value) + f_bytes(2, cid))])

    def drain(self, settle=0.25):
        """读完已到达的响应，打印摘要"""
        out = []
        while True:
            m = self.recv(settle)
            if m is None:
                return out
            body = next((v for f, w, v in fields(m) if f == 2), b'')
            for f, w, det in fields(body):
                if f != 2:
                    continue
                d = {f2: v2 for f2, w2, v2 in fields(det)}
                t = d.get(1)
                if t == 1:
                    out.append('上屏 ' + d[4].decode())
                elif t == 2:
                    self.cands = []
                    more = 0
                    for f3, w3, cw in fields(d.get(2, b'')):
                        if f3 == 1:
                            cf = {a: b for a, _, b in fields(cw)}
                            self.cands.append((cf.get(1, b''), cf.get(2, b''), cf.get(9, b'')))
                        elif f3 == 3:
                            more = cw
                    show = (lambda c: c[0].decode() + c[2].decode()) if os.environ.get('CANDS_COMMENT') else (lambda c: c[0].decode())
                    out.append('候选 ' + '/'.join(show(c) for c in self.cands[:int(os.environ.get('CANDS_N', '9'))]) +
                               f'（共 {len(self.cands)} 个{"，还有更多" if more else ""}）')
                elif t == 3:
                    segs = []
                    for f3, w3, pw in fields(d.get(3, b'')):
                        if f3 == 1:
                            pf = {a: b for a, _, b in fields(pw)}
                            segs.append(pf.get(7, b'').decode())
                    out.append('预编辑 ' + repr(''.join(segs)) + (f' 分段{segs}' if len(segs) > 1 else ''))
                else:
                    out.append(f'type={t}')


def bench(connect, baseline_path):
    """跑候选基准：逐键发送并等候选，比对前 10 个候选，统计按键往返延迟"""
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    expect = {}
    for line in open(baseline_path, encoding='utf-8'):
        k, _, v = line.rstrip('\n').partition('\t')
        expect[k] = v
    words = open(os.path.join(root, 'tests', 'baseline_inputs.txt')).read().split()
    lat, bad = [], 0
    for w in words:
        # 每段输入一条新连接（= 新会话），和插件按 Esc 的做法一样；官方 server 不认 ESC 键
        for attempt in range(50):    # Wine 栈的管道实例要等一会儿才能复用
            try:
                c = Client(connect())
                break
            except (EOFError, OSError):
                time.sleep(0.1)
        c.send([f_varint(1, 3)])
        c.drain(0.1)
        for ch in w:
            t0 = time.perf_counter()
            c.key(ord(ch), 64)
            while True:          # 等这个键的候选帧（预编辑帧先到）
                m = c.recv(5.0)
                assert m is not None, '5 秒内没收到候选'
                body = next((v for f, _, v in fields(m) if f == 2), b'')
                det = next((v for f, _, v in fields(body) if f == 2), b'')
                d = {f2: v2 for f2, _, v2 in fields(det)}
                if d.get(1) == 2:
                    lat.append(time.perf_counter() - t0)
                    cands = [dict((a, b) for a, _, b in fields(cw)).get(1, b'').decode()
                             for f3, _, cw in fields(d.get(2, b'')) if f3 == 1]
                    break
        # 基准记前 10 个；官方 server 一页只发 9 个，按两边都有的长度比
        k = min(10, len(cands))
        got = '|'.join(cands[:k])
        want = '|'.join((expect.get(w) or '').split('|')[:k])
        if got != want:
            bad += 1
            print(f'不同 {w}: {got}\n  基准 {want}')
        c.s.close()
    lat.sort()
    n = len(lat)
    ms = lambda x: x * 1000
    print(f'{len(words)} 段输入，{len(words) - bad} 段候选与基准一致')
    print(f'按键往返 {n} 次：平均 {ms(sum(lat) / n):.2f} ms，p50 {ms(lat[n // 2]):.2f}，'
          f'p90 {ms(lat[n * 9 // 10]):.2f}，p99 {ms(lat[n * 99 // 100]):.2f}，最大 {ms(lat[-1]):.2f}')
    return bad == 0


def main():
    args = sys.argv[1:]
    script = "nihao <SEL:0>"
    sock = None
    baseline = None
    i = 0
    while i < len(args):
        if args[i] == '--bench':
            baseline = args[i + 1]
            i += 2
        elif args[i] == '--tcp':
            sock = socket.create_connection(('127.0.0.1', int(args[i + 1])))
            i += 2
        elif args[i] == '--unix':
            sock = socket.socket(socket.AF_UNIX)
            sock.connect(args[i + 1])
            i += 2
        else:
            script = args[i]
            i += 1
    if sock is None:
        sock = socket.socket(socket.AF_UNIX)
        sock.connect(os.path.join(os.environ.get('XDG_RUNTIME_DIR', '/tmp'), 'minime', 'engine.sock'))
    if baseline:
        fam, addr = sock.family, sock.getpeername()
        sock.close()

        def connect():
            s2 = socket.socket(fam)
            s2.connect(addr)
            return s2
        sys.exit(0 if bench(connect, baseline) else 1)
    c = Client(sock)
    c.send([f_varint(1, 3)])            # CREATE_SESSION
    for line in c.drain():
        print('  ', line)
    for act in script.split():
        if act.startswith('<PRIV:'):
            print(f'[隐私输入框 {act[6:-1]}]')
            c.context('', priv=int(act[6:-1]))
            continue
        if act.startswith('<CTX:'):
            print(f'[上下文 {act[5:-1]}]')
            c.context(act[5:-1])
            continue
        if act == '<BS>':
            print('[退格]')
            c.key(0, 12)
        elif act == '<ESC>':
            print('[Esc]')
            c.key(0, 10)
        elif act.startswith('<CFG:'):
            parts = act[5:-1].split(',')
            fz, tr, e2, em, sc, sp, wb = (parts + ['0', '0', '1', '0', '0', '0', '1'][len(parts):])[:7]
            print(f'[配置 模糊音={fz} 繁体={tr} 整句={e2} 表情={em} 方案={sc} 双拼={sp} 五笔={wb}]')
            c.send([f_varint(1, 202) + f_varint(300, int(fz, 0)) + f_varint(301, int(tr)) + f_varint(302, int(e2))
                    + f_varint(304, int(em)) + f_varint(305, int(sc)) + f_varint(306, int(sp))
                    + f_varint(307, int(wb, 0))])
        elif act == '<MORE>':
            print('[要更多候选]')
            c.send([f_varint(1, 201)])
        elif act.startswith('<DEL:'):
            n = int(act[5:-1])
            print(f'[删第 {n} 个: {c.cands[n][0].decode() if n < len(c.cands) else "?"}]')
            c.block(n)
        elif act.startswith('<SEL:'):
            n = int(act[5:-1])
            print(f'[选第 {n} 个: {c.cands[n][0].decode() if n < len(c.cands) else "?"}]')
            c.select(n)
        elif act.startswith('<WAIT:'):
            # 等待期间照样读响应（e2e 整句候选是后到的），候选列表以最后一次为准
            end = time.time() + int(act[6:-1]) / 1000
            while time.time() < end:
                for line in c.drain(max(0.01, end - time.time())):
                    print('  ', line)
            continue
        else:
            for ch in act:
                print(f'[{ch}]')
                c.key(ord(ch), 64)
                for line in c.drain(0.15):
                    print('  ', line)
            continue
        for line in c.drain():
            print('  ', line)


if __name__ == '__main__':
    main()
