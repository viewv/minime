"""pinyin_table.py — 从 mpd_e2e.bin（MultiPinyinDict）查词的全部读音。

文件：u32 0x80000001、u32 桶数 N、N 个 u32 桶偏移，然后是各桶。
桶：u32 项数 n、n 个 u32 校验哈希（升序）、n 个 u16 累计结束偏移（相对数据区，数据区紧跟其后）。
项：一串字节，每第 5 个字节（下标 0、5、10…）是填充的 0；去掉后是
    u8 读音个数，然后每个读音 u8 长度 + 字母（多字词用逗号分隔音节，如 "qu,zao,le"）。
桶号 = CityHash32(词) mod N；校验哈希 = CityHash64WithSeed(词, 0x1dbba5) 的低 32 位。
"""
import struct

from cityhash import city32, city64_with_seed

SEED = 0x1dbba5


class MultiPinyinDict:
    def __init__(self, path='vendor/mac/imeData/mpd_e2e.bin'):
        self.d = open(path, 'rb').read()
        self.n = struct.unpack_from('<I', self.d, 4)[0]
        self.offs = struct.unpack_from('<%dI' % self.n, self.d, 8)

    def lookup(self, word):
        """返回读音列表（如 ['chang', 'zhang']），查不到返回 None"""
        k = word.encode()
        bi = city32(k) % self.n
        ch = city64_with_seed(k, SEED) & 0xffffffff
        a = self.offs[bi]
        b = self.offs[bi + 1] if bi + 1 < self.n else len(self.d)
        blob = self.d[a:b]
        n = struct.unpack_from('<I', blob, 0)[0]
        hs = struct.unpack_from('<%dI' % n, blob, 4)
        if ch not in hs:
            return None
        i = hs.index(ch)
        ends = struct.unpack_from('<%dH' % n, blob, 4 + 4 * n)
        base = 4 + 6 * n
        raw = blob[base + (ends[i - 1] if i else 0):base + ends[i]]
        p = bytes(c for j, c in enumerate(raw) if j % 5)
        cnt, j, out = p[0], 1, []
        for _ in range(cnt):
            L = p[j]
            out.append(p[j + 1:j + 1 + L].decode())
            j += 1 + L
        return out
