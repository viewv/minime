#!/usr/bin/env python3
"""e2e_strategy.py — 用评测集（tests/e2e_eval.tsv）比较"引擎候选 + e2e 整句"的出候选策略。

  e2e_strategy.py collect      录引擎候选（build/native/host）和 e2e 结果（build/native/e2e_cli，beam 4 和 8），
                               再让模型给引擎前三打分（e2e_score），存进 work/e2e_eval/；有上下文的条目再录一份无上下文的
  e2e_strategy.py sim [-v]     离线模拟各种策略，按类别统计首选/前三正确率和相对纯引擎的得失；-v 列出得失的条目
  e2e_strategy.py jp [-v]      简拼：在 tests/e2e_eval_jp.tsv 上比引擎和 e2e（几种缩写扣分），
                               并检查打开简拼后全拼评测集有没有变差

要在仓库根目录运行。只用标准库。
"""
import math
import os
import subprocess
import sys

EVAL = 'tests/e2e_eval.tsv'
OUT = 'work/e2e_eval'
ENGINE_DIR = 'vendor/win/3.0.1.35'
BEAMS = (4, 8)


def load_eval():
    items = []
    for line in open(EVAL, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        cat, py, truth, ctx = (line.rstrip('\n').split('\t') + [''])[:4]
        items.append(dict(cat=cat, py=py, truth=truth, ctx=ctx))
    return items


def runs(items):
    """每条评测要跑的 (上下文, 拼音) 组合：本身的上下文；有上下文的再加一份无上下文"""
    out = []
    for it in items:
        out.append((it['ctx'], it['py']))
        if it['ctx']:
            out.append(('', it['py']))
    return out


def collect():
    items = load_eval()
    rs = runs(items)
    os.makedirs(OUT, exist_ok=True)
    wd = os.path.join(OUT, 'wd')                # 引擎用独立的用户目录，不碰真实词库
    os.makedirs(wd, exist_ok=True)
    base = os.path.join(OUT, 'engine.tsv')
    env = dict(os.environ, WXIME_QUIET='1', WXIME_BASELINE=base)
    args = ['%s|%s' % (c, p) for c, p in rs]
    print('引擎：%d 段' % len(args), file=sys.stderr)
    subprocess.run(['build/native/host', ENGINE_DIR, wd] + args, env=env, check=True,
                   stdout=subprocess.DEVNULL)
    inp = ''.join('%s\t%s\n' % (c, p) for c, p in rs)
    for b in BEAMS:
        print('e2e beam %d：%d 段' % (b, len(rs)), file=sys.stderr)
        r = subprocess.run(['build/native/e2e_cli', '-s', '-b', str(b), '-n', '4'], input=inp,
                           capture_output=True, text=True, check=True)
        lines = r.stdout.splitlines()
        assert len(lines) == len(rs), (len(lines), len(rs))
        with open(os.path.join(OUT, 'e2e_b%d.tsv' % b), 'w', encoding='utf-8') as f:
            for (c, p), l in zip(rs, lines):
                f.write('%s\t%s\n' % (c, l))      # 上下文 拼音 候选 毫秒 分数
    eng, _ = load_runs(scores=False)
    print('模型给引擎前三打分：%d 段' % len(rs), file=sys.stderr)
    inp = ''.join('%s\t%s\t%s\n' % (c, p, '|'.join(eng[(c, p)][:3])) for c, p in rs)
    r = subprocess.run(['build/native/e2e_cli', '-s'], input=inp, capture_output=True, text=True, check=True)
    lines = r.stdout.splitlines()
    assert len(lines) == len(rs)
    with open(os.path.join(OUT, 'engine_scores.tsv'), 'w', encoding='utf-8') as f:
        for (c, p), l in zip(rs, lines):
            f.write('%s\t%s\n' % (c, l))          # 上下文 拼音 句子 分数
    print('已写进 %s/' % OUT, file=sys.stderr)


def load_runs(scores=True):
    eng = {}
    for line in open(os.path.join(OUT, 'engine.tsv'), encoding='utf-8'):
        key, _, cands = line.rstrip('\n').partition('\t')
        ctx, _, py = key.partition('|')
        eng[(ctx, py)] = [x for x in cands.split('|') if x]
    e2e = {}
    for b in BEAMS:
        d = {}
        for line in open(os.path.join(OUT, 'e2e_b%d.tsv' % b), encoding='utf-8'):
            f = line.rstrip('\n').split('\t')
            ctx, py, cands, ms, sc = (f + [''] * 5)[:5]
            c = [x for x in cands.split('|') if x]
            s = [float(x) for x in sc.split('|') if x]
            d[(ctx, py)] = dict(c=c, s=s, ms=float(ms or 0))
        e2e[b] = d
    if scores:                                  # 引擎前三的模型分数，挂在每个 e2e 结果上（M['es']）
        for line in open(os.path.join(OUT, 'engine_scores.tsv'), encoding='utf-8'):
            ctx, py, texts, sc = line.rstrip('\n').split('\t')
            es = dict(zip(texts.split('|'), (float(x) for x in sc.split('|'))))
            for b in BEAMS:
                if (ctx, py) in e2e[b]:
                    e2e[b][(ctx, py)]['es'] = es
    return eng, e2e


# ---------------- 策略 ----------------
# 每个策略：(引擎候选, e2e 结果{c,s}, 字母数) -> 最终候选列表

def merge(E, text, pos):
    rest = [x for x in E if x != text]
    pos = min(pos, len(rest))
    return rest[:pos] + [text] + rest[pos:]


def usable(E, M, L, min_letters):
    return (L >= min_letters and E and M['c'] and not E[0][:1].isascii()
            and M['c'][0] != E[0])


def s_engine(E, M, L):
    return E


def s_rule(min_letters=4, long_from=12, short_pos=1):
    """旧规则（2026-11-14 之前服务的做法）：字母数 >= min 才出；>= long_from 放第 1 位，否则放第 short_pos+1 位"""
    def f(E, M, L):
        if not usable(E, M, L, min_letters):
            return E
        return merge(E, M['c'][0], 0 if L >= long_from else short_pos)
    return f


def s_conf(min_letters=4, p_top=0.5, short_pos=1, long_from=999):
    """按置信度：e2e 首选的概率 exp(score) >= p_top（或字母数 >= long_from）放第 1 位，否则放 short_pos"""
    def f(E, M, L):
        if not usable(E, M, L, min_letters):
            return E
        top = math.exp(M['s'][0]) >= p_top or L >= long_from
        return merge(E, M['c'][0], 0 if top else short_pos)
    return f


def s_agree(min_letters=4, p_top=0.5, short_pos=1):
    """置信度 + 一致性：e2e 首选就在引擎候选里时只提前到 short_pos（引擎的排序常常有道理），
    不在引擎候选里且置信度高才放第 1 位"""
    def f(E, M, L):
        if not usable(E, M, L, min_letters):
            return E
        t = M['c'][0]
        top = math.exp(M['s'][0]) >= p_top and t not in E[:5]
        return merge(E, t, 0 if top else short_pos)
    return f


def s_rescore(min_letters=4, margin=0.0, short_pos=1, k=3):
    """合并重排：引擎前 k 个和 e2e 结果放在一起按模型分数比；e2e 首选比引擎首选高出 margin 以上才放第 1 位，
    否则放 short_pos（引擎首选的分数更高说明 e2e 的 beam 漏掉了它）"""
    def f(E, M, L):
        if not usable(E, M, L, min_letters):
            return E
        t, st = M['c'][0], M['s'][0]
        se = M.get('es', {}).get(E[0], -1e30)
        top = st - se >= margin
        return merge(E, t, 0 if top else short_pos)
    return f


def s_combo(min_letters=4, p_top=0.3, margin=2.0, short_pos=1):
    """组合（现在服务用的，p_top=0.3、margin=3.0）：e2e 首选的整句概率 >= p_top（短输入、靠上下文的场景），或者模型给它的分数比引擎首选高出 margin
    （长句：引擎首选更高说明 e2e 的 beam 漏掉了它）时放第 1 位，否则放 short_pos"""
    def f(E, M, L):
        if not usable(E, M, L, min_letters):
            return E
        t, st = M['c'][0], M['s'][0]
        se = M.get('es', {}).get(E[0], -1e30)
        top = math.exp(st) >= p_top or st - se >= margin
        return merge(E, t, 0 if top else short_pos)
    return f


def s_rescore_pick(min_letters=4, margin=0.0, k=3):
    """合并重排（选引擎候选也行）：引擎前 k 个和 e2e 首选里模型分数最高的放第 1 位（要比引擎首选高出 margin），
    e2e 首选如果没放到第 1 位就放第 2 位"""
    def f(E, M, L):
        if not usable(E, M, L, min_letters):
            return E
        es = M.get('es', {})
        pool = [(es.get(x, -1e30), x) for x in E[:k]] + [(M['s'][0], M['c'][0])]
        best_s, best = max(pool)
        se = es.get(E[0], -1e30)
        out = list(E)
        if best != E[0] and best_s - se >= margin:
            out = merge(out, best, 0)
        if out[0] != M['c'][0]:
            out = merge(out, M['c'][0], 1)
        return out
    return f


# ---------------- 统计 ----------------

def evaluate(items, eng, e2e, beam, strat, use_ctx=True):
    st = {}
    detail = []
    for it in items:
        ctx = it['ctx'] if use_ctx else ''
        key = (ctx, it['py'])
        E = eng[key]
        M = e2e[beam][key]
        F = strat(E, M, len(it['py']))
        t = it['truth']
        for cat in (it['cat'], '全部'):
            s = st.setdefault(cat, dict(n=0, top1=0, top3=0, eng1=0, help=0, harm=0))
            s['n'] += 1
            s['top1'] += F[:1] == [t]
            s['top3'] += t in F[:3]
            s['eng1'] += E[:1] == [t]
            s['help'] += E[:1] != [t] and F[:1] == [t]
            s['harm'] += E[:1] == [t] and F[:1] != [t]
        if (E[:1] == [t]) != (F[:1] == [t]):
            detail.append((it, E, M, F))
    return st, detail


CATS = ['短词', '短语', '长句', '上下文', '英文', '全部']


def fmt(st):
    out = []
    for c in CATS:
        if c in st:
            s = st[c]
            out.append('%s %d/%d' % (c, s['top1'], s['n']))
    a = st['全部']
    return '  '.join(out) + '  | 前三 %d  帮 %d 坑 %d' % (a['top3'], a['help'], a['harm'])


def sim(verbose):
    items = load_eval()
    eng, e2e = load_runs()
    n = len(items)

    # 上下文本身的作用（只看有上下文的条目）
    ci = [it for it in items if it['ctx']]
    print('== 上下文的作用（%d 条带上下文的条目，首选正确数）' % len(ci))
    for use in (False, True):
        e1 = sum(eng[(it['ctx'] if use else '', it['py'])][:1] == [it['truth']] for it in ci)
        m1 = {b: sum(e2e[b][(it['ctx'] if use else '', it['py'])]['c'][:1] == [it['truth']] for it in ci) for b in BEAMS}
        print('  %s：引擎 %d，e2e beam4 %d，beam8 %d' % ('有上下文' if use else '无上下文', e1, m1[4], m1[8]))

    print('\n== 单独看（%d 条，首选正确数；e2e 前四含正确数）' % n)
    for name, get in [('引擎', lambda it: eng[(it['ctx'], it['py'])])] + \
            [('e2e beam%d' % b, (lambda b: lambda it: e2e[b][(it['ctx'], it['py'])]['c'])(b)) for b in BEAMS]:
        by = {}
        for it in items:
            c = get(it)
            for cat in (it['cat'], '全部'):
                s = by.setdefault(cat, [0, 0, 0])
                s[0] += 1
                s[1] += c[:1] == [it['truth']]
                s[2] += it['truth'] in c[:4]
        print('  %-10s ' % name + '  '.join('%s %d/%d' % (c, by[c][1], by[c][0]) for c in CATS if c in by) +
              '  | 前四含正确 %d' % by['全部'][2])
    for b in BEAMS:
        ms = [e2e[b][(it['ctx'], it['py'])]['ms'] for it in items]
        print('  e2e beam%d 平均 %.1f ms，最长 %.1f ms' % (b, sum(ms) / len(ms), max(ms)))

    print('\n== 策略（首选正确数，按类别）')
    strategies = [('只用引擎', s_engine, 4)]
    for b in BEAMS:
        strategies += [
            ('旧规则：≥4 字母，≥12 放第1 否则第2（beam%d）' % b, s_rule(4, 12, 1), b),
            ('≥4 字母，≥8 放第1 否则第2（beam%d）' % b, s_rule(4, 8, 1), b),
            ('≥4 字母，一律放第1（beam%d）' % b, s_rule(4, 0, 1), b),
            ('≥4 字母，一律放第2（beam%d）' % b, s_rule(4, 999, 1), b),
        ]
        for p in (0.3, 0.5, 0.7, 0.9):
            strategies.append(('置信度 p≥%.1f 放第1 否则第2（beam%d）' % (p, b), s_conf(4, p, 1), b))
        for p in (0.3, 0.5, 0.7):
            strategies.append(('置信度 p≥%.1f 且不在引擎前五 放第1 否则第2（beam%d）' % (p, b), s_agree(4, p, 1), b))
        for mg in (0.0, 0.5, 1.0, 2.0):
            strategies.append(('重排：e2e 比引擎首选高 %.1f 放第1 否则第2（beam%d）' % (mg, b), s_rescore(4, mg, 1), b))
        for mg in (0.0, 1.0):
            strategies.append(('重排选优：引擎前三和 e2e 里分数最高者 +%.1f 放第1（beam%d）' % (mg, b), s_rescore_pick(4, mg), b))
        for p, mg in ((0.3, 1.0), (0.3, 2.0), (0.3, 3.0), (0.5, 2.0)):
            strategies.append(('组合：p≥%.1f 或 比引擎首选高 %.1f 放第1 否则第2（beam%d）' % (p, mg, b), s_combo(4, p, mg), b))
        for p in (0.5,):
            for lf in (8, 12):
                strategies.append(('置信度 p≥%.1f 或 ≥%d 字母 放第1 否则第2（beam%d）' % (p, lf, b), s_conf(4, p, 1, lf), b))
    width = max(len(s[0]) for s in strategies)
    best = None
    for name, f, b in strategies:
        st, detail = evaluate(items, eng, e2e, b, f)
        print('  %s  %s' % (name.ljust(width - len(name) // 2), fmt(st)))
        key = (st['全部']['top1'], -st['全部']['harm'])
        if best is None or key > best[0]:
            best = (key, name, detail)
    if verbose and best:
        print('\n== 最好的策略（%s）相对纯引擎的得失' % best[1])
        for it, E, M, F in best[2]:
            ok = F[:1] == [it['truth']]
            print('  %s %s [%s] %s：正确 %s；引擎 %s；e2e %s（p=%.2f）' % (
                '帮' if ok else '坑', it['cat'], it['ctx'], it['py'], it['truth'], '/'.join(E[:3]),
                '/'.join(M['c'][:2]), math.exp(M['s'][0]) if M['s'] else 0))


JP_EVAL = 'tests/e2e_eval_jp.tsv'
JP_PENALTIES = (1.0,)


def run_e2e(rows, extra):
    """rows: [(上下文, 拼音)] -> [(候选列表, 分数列表)]"""
    inp = ''.join('%s\t%s\n' % r for r in rows)
    r = subprocess.run(['build/native/e2e_cli', '-s', '-b', '4', '-n', '4'] + extra, input=inp,
                       capture_output=True, text=True, check=True)
    out = []
    for l in r.stdout.splitlines():
        f = (l.split('\t') + [''] * 4)[:4]
        out.append(([x for x in f[1].split('|') if x], [float(x) for x in f[3].split('|') if x]))
    assert len(out) == len(rows)
    return out


def run_engine(rows):
    os.makedirs(OUT, exist_ok=True)
    base = os.path.join(OUT, 'engine_jp.tsv')
    wd = os.path.join(OUT, 'wd')
    os.makedirs(wd, exist_ok=True)
    env = dict(os.environ, WXIME_QUIET='1', WXIME_BASELINE=base)
    subprocess.run(['build/native/host', ENGINE_DIR, wd] + ['%s|%s' % r for r in rows], env=env, check=True,
                   stdout=subprocess.DEVNULL)
    d = {}
    for line in open(base, encoding='utf-8'):
        key, _, cands = line.rstrip('\n').partition('\t')
        ctx, _, py = key.partition('|')
        d[(ctx, py)] = [x for x in cands.split('|') if x]
    return [d[r] for r in rows]


def jp(verbose):
    items = []
    for line in open(JP_EVAL, encoding='utf-8'):
        if line.startswith('#') or not line.strip():
            continue
        cat, a, b, truth, ctx = (line.rstrip('\n').split('\t') + [''])[:5]
        items.append(dict(cat=cat, jp={'首字母': a, '声母': b}, truth=truth, ctx=ctx))
    cats = ['短词', '短语', '长句', '上下文', '全部']

    def report(name, got, its):
        by = {}
        for it, c in zip(its, got):
            for cat in (it['cat'], '全部'):
                s = by.setdefault(cat, [0, 0, 0])
                s[0] += 1
                s[1] += c[:1] == [it['truth']]
                s[2] += it['truth'] in c[:3]
        print('  %-22s ' % name + '  '.join('%s %d/%d' % (c, by[c][1], by[c][0]) for c in cats if c in by) +
              '  | 前三含正确 %d' % by['全部'][2])

    for var in ('首字母', '声母'):
        rows = [(it['ctx'], it['jp'][var]) for it in items]
        print('== 简拼（%s，%d 条，首选正确数）' % (var, len(rows)))
        eng = run_engine(rows)
        report('引擎', eng, items)
        res = {}
        for pen in JP_PENALTIES:
            res[pen] = run_e2e(rows, ['-j', str(pen)])
            report('e2e 扣分 %.1f' % pen, [c for c, _ in res[pen]], items)
        # 放置策略（和服务一样：置信度或重排决定第 1/2 位）在简拼上的效果，最少字母数取 2/3/4
        sc_in = ''.join('%s\t%s\t%s\n' % (r[0], r[1], '|'.join(e[:3]) or '-') for r, e in zip(rows, eng))
        out = subprocess.run(['build/native/e2e_cli', '-s'], input=sc_in, capture_output=True, text=True,
                             check=True).stdout.splitlines()
        es = []
        for l in out:
            f = l.split('\t')
            es.append(dict(zip(f[1].split('|'), (float(x) for x in f[2].split('|') if x))))
        for name, f in [('全拼的策略 ≥4 字母', s_combo(4, 0.3, 3.0)), ('全拼的策略 ≥2 字母', s_combo(2, 0.3, 3.0)),
                        ('≥2 字母 一律第1', s_rule(2, 0, 1)),
                        ('≥2 p≥0.1 或高 1 放第1', s_combo(2, 0.1, 1.0)),
                        ('≥2 p≥0.05 或高 0 放第1', s_combo(2, 0.05, 0.0)),
                        ('≥2 比引擎首选高 0 放第1', s_combo(2, 1.1, 0.0)),
                        ('≥2 比引擎首选高 -1 放第1', s_combo(2, 1.1, -1.0)),
                        ('≥3 比引擎首选高 0 放第1', s_combo(3, 1.1, 0.0))]:
            got = [f(e, dict(c=c, s=sc, es=x), len(r[1])) if c else e
                   for r, e, (c, sc), x in zip(rows, eng, res[1.0], es)]
            report(name, got, items)
        union = [e if e[:1] == [it['truth']] else r[0] for it, e, r in zip(items, eng, res[1.0])]
        report('引擎或 e2e(1.0) 有一个对', union, items)
        if verbose:
            for it, e, (c, sc) in zip(items, eng, res[1.0]):
                if (e[:1] == [it['truth']]) != (c[:1] == [it['truth']]):
                    print('    %s %s [%s] %s：正确 %s；引擎 %s；e2e %s' % (
                        'e2e 对' if c[:1] == [it['truth']] else '引擎对', it['cat'], it['ctx'], it['jp'][var],
                        it['truth'], '/'.join(e[:3]), '/'.join(c[:3])))

    print('\n== 打开简拼后的全拼评测集（e2e 首选正确数，有上下文）')
    full = load_eval()
    rows = [(it['ctx'], it['py']) for it in full]
    for extra, name in [([], '不开简拼')] + [(['-j', str(p)], '简拼 扣分 %.1f' % p) for p in JP_PENALTIES]:
        got = run_e2e(rows, extra)
        report(name, [c for c, _ in got], full)


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == 'collect':
        collect()
    elif len(sys.argv) >= 2 and sys.argv[1] == 'sim':
        sim('-v' in sys.argv)
    elif len(sys.argv) >= 2 and sys.argv[1] == 'jp':
        jp('-v' in sys.argv)
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == '__main__':
    main()
