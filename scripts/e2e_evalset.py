#!/usr/bin/env python3
"""e2e_evalset.py — 从 tests/e2e_eval_src.tsv 生成带拼音的评测集 tests/e2e_eval.tsv，
以及简拼评测集 tests/e2e_eval_jp.tsv（每个音节只取首字母 / 保留 zh、ch、sh 两种写法）。

拼音用 pypinyin 标（只有生成时需要它：pip install pypinyin）；含多音字的条目打印出来供人工核对，
标错的写进下面的 OVERRIDE。输出格式：类别<TAB>拼音<TAB>正确结果<TAB>光标前文字
"""
import sys

from pypinyin import Style, lazy_pinyin, pinyin

SRC, OUT, OUT_JP = 'tests/e2e_eval_src.tsv', 'tests/e2e_eval.tsv', 'tests/e2e_eval_jp.tsv'
OVERRIDE = {}          # 正确结果 -> 拼音（人工改正）
OVERRIDE_SYLL = {}     # 正确结果 -> 音节列表（人工改正，简拼用）


def initials(sylls, keep_zcs):
    out = ''
    for x in sylls:
        x = x.replace('ü', 'v')
        out += x[:2] if keep_zcs and x[:2] in ('zh', 'ch', 'sh') else x[:1]
    return out


def main():
    rows = []
    for line in open(SRC, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        cat, text, ctx = (line.split('\t') + ['', ''])[:3]
        if cat == '英文':
            py = text.lower()
        else:
            py = OVERRIDE.get(text) or ''.join(lazy_pinyin(text, style=Style.NORMAL, v_to_u=False)).replace('ü', 'v')
            multi = [c for c, r in zip(text, pinyin(text, style=Style.NORMAL, heteronym=True)) if len(set(r)) > 1]
            if multi and '-q' not in sys.argv:
                print('多音字 %s：%s -> %s' % (''.join(multi), text, py))
        sylls = OVERRIDE_SYLL.get(text) or lazy_pinyin(text, style=Style.NORMAL)
        rows.append((cat, py, text, ctx, sylls))
    with open(OUT, 'w', encoding='utf-8') as f:
        f.write('# 由 scripts/e2e_evalset.py 从 e2e_eval_src.tsv 生成：类别\t拼音\t正确结果\t光标前文字\n')
        for r in rows:
            f.write('\t'.join(r[:4]) + '\n')
    with open(OUT_JP, 'w', encoding='utf-8') as f:
        f.write('# 由 scripts/e2e_evalset.py 生成的简拼评测集：类别\t首字母简拼\t声母简拼（zh/ch/sh 两个字母）\t正确结果\t光标前文字\n')
        for cat, py, text, ctx, sylls in rows:
            if cat == '英文':
                continue
            f.write('\t'.join((cat, initials(sylls, False), initials(sylls, True), text, ctx)) + '\n')
    print('%d 条写进 %s' % (len(rows), OUT), file=sys.stderr)


if __name__ == '__main__':
    main()
