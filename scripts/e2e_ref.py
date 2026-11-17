#!/usr/bin/env python3
"""e2e_ref.py — 微信输入法 e2e 整句模型（Mac 包里的 decgnr.bin）的 NumPy 参考实现。

模型格式和用法来自 Mac 二进制（arm64 切片，带符号）的静态分析，权重排列用统计检验确定，
见 docs/whole-sentence.md "Mac 二进制解剖（续）"：
  - 容器 XLLM：0x1000 处 protobuf 配置（量化种类 "q80"、结构 GPT2、张量表），
    内嵌 HuggingFace 的 tokenizer_config.json / tokenizer.json（BERT 中文 WordPiece 词表），
    0x36adf 起是 149 个张量：u64 名字长度、名字、u64 字节数，数据从下一个 4 KB 边界开始。
  - 线性层权重 int4：先全部 4 位数据，再全部 fp16 scale（每 32 个一组），值 = (q - 8) * scale。
    数据按 8 行一块交错：字节流是 [行/8][列/2][8]，每字节高 4 位是第 2c 列、低 4 位是第 2c+1 列；
    scale 同样按 8 行一块：[行/8][列/32][8]。
  - lm_head 是 int8：值 = q * scale，数据 [行/8][列][8]，scale [行/8][列/32][8]。
  - 嵌入和偏置 fp16，LayerNorm 参数 fp32。
  - 结构：GPT-2（前置 LayerNorm、可学习位置嵌入、qkv 合并投影 [q;k;v]、GELU、12 层、768 维、12 头）。
  - 输入（xllm::GPT2::BuildInput）：[CLS] 光标前文本 [SEP] 拼音 [SEP] [CLS]，然后逐个生成汉字，
    直到 [SEP]。拼音是**逐个字母用空格隔开**（"n i h a o"），每个字母是词表里的一个 token。

用法: e2e_ref.py [--context 光标前文本] [--beam N] 拼音...
例:   e2e_ref.py nihao
      e2e_ref.py --context 今天 tianqizhenbucuo
"""
import argparse
import json
import re
import struct
import sys
import time
import unicodedata

import numpy as np

MODEL = 'vendor/mac/imeData/decgnr.bin'


# ---------------- 容器 ----------------
def varint(b, i):
    v = s = 0
    while True:
        c = b[i]
        i += 1
        v |= (c & 0x7f) << s
        s += 7
        if not c & 0x80:
            return v, i


def load(path):
    d = open(path, 'rb').read()
    # 张量
    i = 0x36ad7
    n = struct.unpack_from('<Q', d, i)[0]
    i += 8
    raw = {}
    for _ in range(n):
        nl = struct.unpack_from('<Q', d, i)[0]
        name = d[i + 8:i + 8 + nl].decode()
        j = i + 8 + nl
        size = struct.unpack_from('<Q', d, j)[0]
        a = (j + 8 + 4095) // 4096 * 4096
        raw[name] = (a, size)
        i = a + size
    # 内嵌的 tokenizer.json：0x4000 起的 JSON
    j = d.index(b'\n}', 0x4000)
    while True:
        try:
            tok = json.loads(d[0x4000:j + 2].decode())
            break
        except json.JSONDecodeError:
            j = d.index(b'\n}', j + 2)
    return d, raw, tok


def tensor(d, raw, name, shape):
    off, size = raw[name]
    count = int(np.prod(shape))
    if size == count * 4:
        return np.frombuffer(d, np.float32, count, off).reshape(shape).astype(np.float32)
    if size == count * 2:
        return np.frombuffer(d, np.float16, count, off).reshape(shape).astype(np.float32)
    groups = count // 32
    R_, C_ = shape
    def scales(soff):
        s = np.frombuffer(d, np.float16, groups, soff).astype(np.float32)
        return np.repeat(s.reshape(R_ // 8, C_ // 32, 8).transpose(0, 2, 1).reshape(R_, C_ // 32), 32, axis=1)
    if size == count // 2 + groups * 2:          # int4，先数据后 scale
        b = np.frombuffer(d, np.uint8, count // 2, off).reshape(R_ // 8, C_ // 2, 8)
        lo = (b & 15).astype(np.float32) - 8
        hi = (b >> 4).astype(np.float32) - 8
        v = np.stack([hi, lo], axis=2).reshape(R_ // 8, C_, 8).transpose(0, 2, 1).reshape(R_, C_)
        return v * scales(off + count // 2)
    if size == count + groups * 2:               # int8
        v = np.frombuffer(d, np.int8, count, off).astype(np.float32)
        v = v.reshape(R_ // 8, C_, 8).transpose(0, 2, 1).reshape(R_, C_)
        return v * scales(off + count)
    raise ValueError('%s: 无法识别的大小 %d（%s）' % (name, size, shape))


# ---------------- BERT WordPiece ----------------
class Tokenizer:
    def __init__(self, tok):
        self.vocab = tok['model']['vocab']
        for t in tok.get('added_tokens', []):
            self.vocab.setdefault(t['content'], t['id'])
        self.inv = {i: t for t, i in self.vocab.items()}
        self.unk = self.vocab['[UNK]']

    @staticmethod
    def _is_cjk(ch):
        cp = ord(ch)
        return (0x4E00 <= cp <= 0x9FFF or 0x3400 <= cp <= 0x4DBF or 0x20000 <= cp <= 0x2A6DF or
                0xF900 <= cp <= 0xFAFF or 0x2F800 <= cp <= 0x2FA1F)

    def encode(self, text):
        text = text.lower()
        words, cur = [], ''
        for ch in text:
            if ch.isspace():
                if cur:
                    words.append(cur)
                cur = ''
            elif self._is_cjk(ch) or unicodedata.category(ch).startswith('P'):
                if cur:
                    words.append(cur)
                words.append(ch)
                cur = ''
            else:
                cur += ch
        if cur:
            words.append(cur)
        ids = []
        for w in words:
            start, pieces = 0, []
            while start < len(w):
                end = len(w)
                while end > start:
                    sub = w[start:end] if start == 0 else '##' + w[start:end]
                    if sub in self.vocab:
                        pieces.append(self.vocab[sub])
                        break
                    end -= 1
                if end == start:
                    pieces = [self.unk]
                    break
                start = end
            ids.extend(pieces)
        return ids

    def decode(self, ids):
        return ''.join(self.inv.get(i, '?').replace('##', '') for i in ids)


# ---------------- GPT-2 ----------------
class Model:
    def __init__(self, path):
        d, raw, tok = load(path)
        self.tok = Tokenizer(tok)
        H, V, F, P = 768, 10496, 3072, 512
        t = lambda n, s: tensor(d, raw, n, s)
        self.wte = t('tok_embeddings.weight', (V, H))
        self.wpe = t('pos_embeddings.weight', (P, H))
        self.lnf = (t('final_norm.weight', (H,)), t('final_norm.bias', (H,)))
        self.head = t('lm_head.weight', (V, H))
        self.layers = []
        for i in range(12):
            g = lambda n, s: t('%s.%d' % (n, i), s)
            self.layers.append(dict(
                ln1=(g('attention_norm.weight', (H,)), g('attention_norm.bias', (H,))),
                qkv=(g('attention.qkv_proj.weight', (3 * H, H)), g('attention.qkv_proj.bias', (3 * H,))),
                o=(g('attention.o_proj.weight', (H, H)), g('attention.o_proj.bias', (H,))),
                ln2=(g('ffn_norm.weight', (H,)), g('ffn_norm.bias', (H,))),
                up=(g('mlp.up_proj.weight', (F, H)), g('mlp.up_proj.bias', (F,))),
                down=(g('mlp.down_proj.weight', (H, F)), g('mlp.down_proj.bias', (H,))),
            ))

    @staticmethod
    def ln(x, wb, eps=1e-5):
        m = x.mean(-1, keepdims=True)
        v = ((x - m) ** 2).mean(-1, keepdims=True)
        return (x - m) / np.sqrt(v + eps) * wb[0] + wb[1]

    @staticmethod
    def gelu(x):
        return 0.5 * x * (1 + np.tanh(0.7978845608028654 * (x + 0.044715 * x ** 3)))

    def forward(self, ids, cache=None):
        """增量前向：cache 是每层的 (K, V)，返回最后一个位置的 logits 和新 cache"""
        start = 0 if cache is None else cache[0][0].shape[1]
        T = len(ids)
        x = self.wte[ids] + self.wpe[np.arange(start, start + T)]
        new_cache = []
        total = start + T
        mask = np.triu(np.full((T, total), -1e9, np.float32), 1 + start)
        for li, L in enumerate(self.layers):
            h = self.ln(x, L['ln1'])
            qkv = h @ L['qkv'][0].T + L['qkv'][1]
            q, k, v = np.split(qkv, 3, axis=-1)
            q = q.reshape(T, 12, 64).transpose(1, 0, 2)
            k = k.reshape(T, 12, 64).transpose(1, 0, 2)
            v = v.reshape(T, 12, 64).transpose(1, 0, 2)
            if cache is not None:
                k = np.concatenate([cache[li][0], k], axis=1)
                v = np.concatenate([cache[li][1], v], axis=1)
            new_cache.append((k, v))
            a = q @ k.transpose(0, 2, 1) / 8.0 + mask
            a = np.exp(a - a.max(-1, keepdims=True))
            a /= a.sum(-1, keepdims=True)
            o = (a @ v).transpose(1, 0, 2).reshape(T, 768)
            x = x + o @ L['o'][0].T + L['o'][1]
            h = self.ln(x, L['ln2'])
            x = x + self.gelu(h @ L['up'][0].T + L['up'][1]) @ L['down'][0].T + L['down'][1]
        x = self.ln(x[-1], self.lnf)
        return x @ self.head.T, new_cache

    def logits(self, ids):
        return self.forward(ids)[0]

    def logits_all(self, ids):
        """整段从位置 0 开始，返回每个位置的 logits（[len(ids)][V]），对照测试用"""
        T = len(ids)
        x = self.wte[ids] + self.wpe[np.arange(T)]
        mask = np.triu(np.full((T, T), -1e9, np.float32), 1)
        for L in self.layers:
            h = self.ln(x, L['ln1'])
            q, k, v = np.split(h @ L['qkv'][0].T + L['qkv'][1], 3, axis=-1)
            q = q.reshape(T, 12, 64).transpose(1, 0, 2)
            k = k.reshape(T, 12, 64).transpose(1, 0, 2)
            v = v.reshape(T, 12, 64).transpose(1, 0, 2)
            a = q @ k.transpose(0, 2, 1) / 8.0 + mask
            a = np.exp(a - a.max(-1, keepdims=True))
            a /= a.sum(-1, keepdims=True)
            x = x + (a @ v).transpose(1, 0, 2).reshape(T, 768) @ L['o'][0].T + L['o'][1]
            h = self.ln(x, L['ln2'])
            x = x + self.gelu(h @ L['up'][0].T + L['up'][1]) @ L['down'][0].T + L['down'][1]
        return self.ln(x, self.lnf) @ self.head.T

    def generate(self, context, pinyin, max_new=None, beam=1):
        CLS, SEP = 101, 102
        letters = ' '.join(c for c in pinyin.lower() if c.isalpha())
        prompt = [CLS] + self.tok.encode(context) + [SEP] + self.tok.encode(letters) + [SEP, CLS]
        max_new = max_new or len(letters.split()) // 2 + 4
        lg, cache0 = self.forward(prompt)
        beams = [(0.0, [], lg, cache0)]
        for _ in range(max_new):
            cand = []
            for score, out, lg, cache in beams:
                if out and out[-1] == SEP:
                    cand.append((score, out, lg, cache))
                    continue
                lp = lg - lg.max()
                lp = lp - np.log(np.exp(lp).sum())
                for t in np.argsort(-lp)[:beam]:
                    cand.append((score + float(lp[t]), out + [int(t)], None, cache))
            cand.sort(key=lambda c: -c[0])
            beams = []
            for score, out, lg, cache in cand[:beam]:
                if lg is None:
                    lg, cache = self.forward([out[-1]], cache)
                beams.append((score, out, lg, cache))
            if all(o and o[-1] == SEP for _, o, _, _ in beams):
                break
        res = []
        for score, out, _, _ in beams:
            if out and out[-1] == SEP:
                out = out[:-1]
            res.append((self.tok.decode(out), score))
        return prompt, res


class ConstrainedDecoder:
    """按拼音约束的 beam search（docs/whole-sentence.md "我们的设计"）。

    每条假设记着已消耗的输入字母数 p。下一个 token 只能是读音 r 满足以下之一的汉字：
      - 剩余输入以 r 开头（全拼，对应 PinyinScene 场景 0）→ p += len(r)
      - 剩余输入是 r 的真前缀（最后一个音节没打完，场景 1）→ p = 输入末尾
    输入用完时只允许 [SEP]，没用完时禁止 [SEP]。读音来自 mpd_e2e.bin（和官方读音校验同源）。
    """

    def __init__(self, model, mpd_path='vendor/mac/imeData/mpd_e2e.bin', english=False):
        # english：允许拼写和输入完全相同的英文 token。默认关闭，和官方一致（decgnr 是纯中文模型，
        # 官方遇到英文子词整条丢弃）；打开后会出现"离nux"这类中英混拼，效果不好。
        self.english = english
        from pinyin_table import MultiPinyinDict
        self.m = model
        mpd = MultiPinyinDict(mpd_path)
        self.by_reading = {}            # 读音 -> [token id]
        for tok, tid in model.tok.vocab.items():
            if len(tok) == 1 and Tokenizer._is_cjk(tok):
                for r in mpd.lookup(tok) or []:
                    self.by_reading.setdefault(r, []).append(tid)
        self.by_reading = {r: np.array(v) for r, v in self.by_reading.items()}
        # 英文/字母 token（如 linux、##ux）：拼写必须和输入字母逐字相同
        self.words = {}                 # 单词 -> token id（词首）
        self.pieces = {}                # 续接片段（去掉 ##）-> token id
        for tok, tid in model.tok.vocab.items():
            if re.fullmatch(r'[a-z]{2,}', tok):
                self.words[tok] = tid
            elif re.fullmatch(r'##[a-z]+', tok):
                self.pieces[tok[2:]] = tid

    def allowed(self, rest, in_word=False):
        """剩余输入 rest -> [(token 数组, 消耗字母数, 是否英文)]。in_word：上一个 token 是英文，可以接 ## 片段"""
        out = []
        for w, tid in (self.words.items() if self.english else ()):
            if rest.startswith(w):
                out.append((np.array([tid]), len(w), True))
        if in_word and self.english:
            for w, tid in self.pieces.items():
                if rest.startswith(w):
                    out.append((np.array([tid]), len(w), True))
        for r, ids in self.by_reading.items():
            if rest.startswith(r):
                out.append((ids, len(r), False))
            elif r.startswith(rest):
                out.append((ids, len(rest), False))
        return out

    def generate(self, context, pinyin, beam=4, max_steps=None):
        CLS, SEP = 101, 102
        inp = ''.join(c for c in pinyin.lower() if c.isalpha())
        prompt = [CLS] + self.m.tok.encode(context) + [SEP] + self.m.tok.encode(' '.join(inp)) + [SEP, CLS]
        lg, cache = self.m.forward(prompt)
        beams = [(0.0, [], 0, lg, cache, False, False)]  # 分数, 输出, 已消耗字母, logits, 缓存, 已结束, 上一个是英文
        for _ in range(max_steps or len(inp) + 1):
            cand = []
            for score, out, p, lg, cache, done, eng in beams:
                if done:
                    cand.append((score, out, p, lg, cache, True, eng))
                    continue
                lp = lg - lg.max()
                lp = lp - np.log(np.exp(lp).sum())
                if p == len(inp):
                    cand.append((score + float(lp[SEP]), out, p, None, cache, True, eng))
                    continue
                for ids, n, is_eng in self.allowed(inp[p:], eng):
                    sc = lp[ids]
                    for k in np.argsort(-sc)[:beam]:
                        cand.append((score + float(sc[k]), out + [int(ids[k])], p + n, None, cache, False, is_eng))
            cand.sort(key=lambda c: -c[0])
            beams = []
            seen = set()
            for score, out, p, lg, cache, done, eng in cand:
                key = tuple(out)
                if key in seen:
                    continue
                seen.add(key)
                if lg is None and not done:
                    lg, cache = self.m.forward([out[-1]], cache)
                beams.append((score, out, p, lg, cache, done, eng))
                if len(beams) == beam:
                    break
            if all(b[5] for b in beams):
                break
        return [(self.m.tok.decode(out), score) for score, out, p, _, _, done, _ in beams if done]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--model', default=MODEL)
    ap.add_argument('--context', default='')
    ap.add_argument('--beam', type=int, default=1)
    ap.add_argument('pinyin', nargs='+')
    a = ap.parse_args()
    t0 = time.time()
    m = Model(a.model)
    t1 = time.time()
    prompt, res = m.generate(a.context, ''.join(a.pinyin), beam=a.beam)
    t2 = time.time()
    print('输入 token:', ' '.join(m.tok.inv[i] for i in prompt))
    for text, score in res:
        print('输出: %s  (对数概率 %.2f)' % (text, score))
    print('加载 %.1f s，生成 %.2f s' % (t1 - t0, t2 - t1), file=sys.stderr)


if __name__ == '__main__':
    main()
