#!/usr/bin/env python3
"""e2e_check.py — 拿 NumPy 参考实现核对 C 后端算出的 logits。

    build/native/e2e_test dump /tmp/x.bin cpu      # 或 vulkan
    python3 scripts/e2e_check.py /tmp/x.bin

逐个位置比：最大绝对差、相对差（除以该位置 logits 的最大绝对值）、首选 token 是否一致。
"""
import struct
import sys

import numpy as np

import e2e_ref as R


def main():
    d = open(sys.argv[1], 'rb').read()
    ns = struct.unpack_from('<i', d, 0)[0]
    off = 4
    m = R.Model(R.MODEL)
    maxabs = maxrel = 0.0
    pos = same = 0
    for i in range(ns):
        n = struct.unpack_from('<i', d, off)[0]
        off += 4
        toks = list(struct.unpack_from('<%di' % n, d, off))
        off += 4 * n
        c = np.frombuffer(d, np.float32, n * 10496, off).reshape(n, 10496)
        off += 4 * n * 10496
        p = m.logits_all(toks)
        diff = np.abs(c - p).max(axis=1)
        scale = np.abs(p).max(axis=1)
        maxabs = max(maxabs, float(diff.max()))
        maxrel = max(maxrel, float((diff / (scale + 1e-6)).max()))
        same += int((c.argmax(1) == p.argmax(1)).sum())
        pos += n
    print('%d 段 %d 个位置：最大绝对差 %.5f，最大相对差 %.2e，首选 token 一致 %d/%d' % (ns, pos, maxabs, maxrel, same, pos))
    sys.exit(0 if same == pos and maxrel < 1e-3 else 1)


if __name__ == '__main__':
    sys.path.insert(0, 'scripts')
    main()
