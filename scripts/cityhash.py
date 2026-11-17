"""cityhash.py — abseil 里的 CityHash32 / CityHash64WithSeed（absl/hash/internal/city.cc）的 Python 版。

微信输入法引擎的 HashMapReader<DefaultKeyHash, DefaultKeyCheckHash> 用它们：
  桶号     = CityHash32(键) mod 桶数
  校验哈希 = CityHash64WithSeed(键, 0x1dbba5) 的低 32 位
"""
import struct

M32 = 0xFFFFFFFF
M64 = 0xFFFFFFFFFFFFFFFF
k0 = 0xc3a5c85c97cb3127
k1 = 0xb492b66fbe98f273
k2 = 0x9ae16a3b2f90404f
c1 = 0xcc9e2d51
c2 = 0x1b873593


def f32(s, i):
    return struct.unpack_from('<I', s, i)[0]


def f64(s, i):
    return struct.unpack_from('<Q', s, i)[0]


def rot32(v, s):
    return v if s == 0 else ((v >> s) | (v << (32 - s))) & M32


def rot64(v, s):
    return v if s == 0 else ((v >> s) | (v << (64 - s))) & M64


def fmix(h):
    h ^= h >> 16
    h = (h * 0x85ebca6b) & M32
    h ^= h >> 13
    h = (h * 0xc2b2ae35) & M32
    h ^= h >> 16
    return h


def mur(a, h):
    a = (a * c1) & M32
    a = rot32(a, 17)
    a = (a * c2) & M32
    h ^= a
    h = rot32(h, 19)
    return (h * 5 + 0xe6546b64) & M32


def _h32_0to4(s):
    b, c = 0, 9
    for ch in s:
        v = ch - 256 if ch >= 128 else ch          # signed char
        b = (b * c1 + v) & M32
        c ^= b
    return fmix(mur(b, mur(len(s), c)))


def _h32_5to12(s):
    n = len(s)
    a = n
    b = (n * 5) & M32
    c = 9
    d = b
    a = (a + f32(s, 0)) & M32
    b = (b + f32(s, n - 4)) & M32
    c = (c + f32(s, (n >> 1) & 4)) & M32
    return fmix(mur(c, mur(b, mur(a, d))))


def _h32_13to24(s):
    n = len(s)
    a = f32(s, (n >> 1) - 4)
    b = f32(s, 4)
    c = f32(s, n - 8)
    d = f32(s, n >> 1)
    e = f32(s, 0)
    f = f32(s, n - 4)
    h = n
    return fmix(mur(f, mur(e, mur(d, mur(c, mur(b, mur(a, h)))))))


def city32(s: bytes) -> int:
    n = len(s)
    if n <= 24:
        if n <= 12:
            return _h32_0to4(s) if n <= 4 else _h32_5to12(s)
        return _h32_13to24(s)
    h = n
    g = (c1 * n) & M32
    f = g
    a0 = (rot32((f32(s, n - 4) * c1) & M32, 17) * c2) & M32
    a1 = (rot32((f32(s, n - 8) * c1) & M32, 17) * c2) & M32
    a2 = (rot32((f32(s, n - 16) * c1) & M32, 17) * c2) & M32
    a3 = (rot32((f32(s, n - 12) * c1) & M32, 17) * c2) & M32
    a4 = (rot32((f32(s, n - 20) * c1) & M32, 17) * c2) & M32
    h ^= a0
    h = rot32(h, 19)
    h = (h * 5 + 0xe6546b64) & M32
    h ^= a2
    h = rot32(h, 19)
    h = (h * 5 + 0xe6546b64) & M32
    g ^= a1
    g = rot32(g, 19)
    g = (g * 5 + 0xe6546b64) & M32
    g ^= a3
    g = rot32(g, 19)
    g = (g * 5 + 0xe6546b64) & M32
    f = (f + a4) & M32
    f = rot32(f, 19)
    f = (f * 5 + 0xe6546b64) & M32
    iters = (n - 1) // 20
    p = 0
    for _ in range(iters):
        a0 = (rot32((f32(s, p) * c1) & M32, 17) * c2) & M32
        a1 = f32(s, p + 4)
        a2 = (rot32((f32(s, p + 8) * c1) & M32, 17) * c2) & M32
        a3 = (rot32((f32(s, p + 12) * c1) & M32, 17) * c2) & M32
        a4 = f32(s, p + 16)
        h ^= a0
        h = rot32(h, 18)
        h = (h * 5 + 0xe6546b64) & M32
        f = (f + a1) & M32
        f = rot32(f, 19)
        f = (f * c1) & M32
        g = (g + a2) & M32
        g = rot32(g, 18)
        g = (g * 5 + 0xe6546b64) & M32
        h ^= (a3 + a1) & M32
        h = rot32(h, 19)
        h = (h * 5 + 0xe6546b64) & M32
        g ^= a4
        g = int.from_bytes(g.to_bytes(4, 'little'), 'big')   # bswap
        g = (g * 5) & M32
        h = (h + a4 * 5) & M32
        h = int.from_bytes(h.to_bytes(4, 'little'), 'big')
        f = (f + a0) & M32
        f, h, g = g, f, h                                     # PERMUTE3(f, h, g)
        p += 20
    g = (rot32(g, 11) * c1) & M32
    g = (rot32(g, 17) * c1) & M32
    f = (rot32(f, 11) * c1) & M32
    f = (rot32(f, 17) * c1) & M32
    h = rot32((h + g) & M32, 19)
    h = (h * 5 + 0xe6546b64) & M32
    h = (rot32(h, 17) * c1) & M32
    h = rot32((h + f) & M32, 19)
    h = (h * 5 + 0xe6546b64) & M32
    h = (rot32(h, 17) * c1) & M32
    return h


def _shiftmix(v):
    return v ^ (v >> 47)


def _hlen16(u, v, mul=0x9ddfea08eb382d69):
    a = ((u ^ v) * mul) & M64
    a ^= a >> 47
    b = ((v ^ a) * mul) & M64
    b ^= b >> 47
    return (b * mul) & M64


def _h64_0to16(s):
    n = len(s)
    if n >= 8:
        mul = (k2 + n * 2) & M64
        a = (f64(s, 0) + k2) & M64
        b = f64(s, n - 8)
        c = (rot64(b, 37) * mul + a) & M64
        d = ((rot64(a, 25) + b) * mul) & M64
        return _hlen16(c, d, mul)
    if n >= 4:
        mul = (k2 + n * 2) & M64
        a = f32(s, 0)
        return _hlen16((n + (a << 3)) & M64, f32(s, n - 4), mul)
    if n > 0:
        a, b, c = s[0], s[n >> 1], s[n - 1]
        y = (a + (b << 8)) & M32
        z = (n + (c << 2)) & M32
        return (_shiftmix(((y * k2) ^ (z * k0)) & M64) * k2) & M64
    return k2


def _h64_17to32(s):
    n = len(s)
    mul = (k2 + n * 2) & M64
    a = (f64(s, 0) * k1) & M64
    b = f64(s, 8)
    c = (f64(s, n - 8) * mul) & M64
    d = (f64(s, n - 16) * k2) & M64
    return _hlen16((rot64((a + b) & M64, 43) + rot64(c, 30) + d) & M64,
                   (a + rot64((b + k2) & M64, 18) + c) & M64, mul)


def _h64_33to64(s):
    n = len(s)
    mul = (k2 + n * 2) & M64
    a = (f64(s, 0) * k2) & M64
    b = f64(s, 8)
    c = f64(s, n - 24)
    d = f64(s, n - 32)
    e = (f64(s, 16) * k2) & M64
    f = (f64(s, 24) * 9) & M64
    g = f64(s, n - 8)
    h = (f64(s, n - 16) * mul) & M64
    u = (rot64((a + g) & M64, 43) + (rot64(b, 30) + c) * 9) & M64
    v = (((a + g) ^ d) + f + 1) & M64
    w = int.from_bytes((((u + v) * mul) & M64).to_bytes(8, 'little'), 'big') + h
    w &= M64
    x = (rot64((e + f) & M64, 42) + c) & M64
    y = ((int.from_bytes((((v + w) * mul) & M64).to_bytes(8, 'little'), 'big') + g) * mul) & M64
    z = (e + f + c) & M64
    a = int.from_bytes((((x + z) * mul + y) & M64).to_bytes(8, 'little'), 'big')
    a = (a + b) & M64
    b = (_shiftmix(((z + a) * mul + d + h) & M64) * mul) & M64
    return (b + x) & M64


def city64(s: bytes) -> int:
    n = len(s)
    if n <= 16:
        return _h64_0to16(s)
    if n <= 32:
        return _h64_17to32(s)
    if n <= 64:
        return _h64_33to64(s)
    raise NotImplementedError('超过 64 字节的键用不到')


def city64_with_seed(s: bytes, seed: int) -> int:
    # CityHash64WithSeeds(s, len, k2, seed) = HashLen16(CityHash64(s) - k2, seed)
    return _hlen16((city64(s) - k2) & M64, seed & M64)
