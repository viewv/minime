#!/usr/bin/env python3
"""e2e_eval.py — 拿引擎首选和 e2e（自由贪心 / 拼音约束 beam）对照人工标注的正确句子。
用法: e2e_eval.py <标注 tsv：拼音\\t正确句子> <引擎基准 tsv：拼音\\t候选1|候选2|…> [beam]"""
import sys
import time

import e2e_ref as R


def main():
    truth = dict(l.rstrip('\n').split('\t') for l in open(sys.argv[1], encoding='utf-8') if l.strip())
    eng = {}
    for l in open(sys.argv[2], encoding='utf-8'):
        k, _, v = l.rstrip('\n').partition('\t')
        eng[k] = v.split('|')
    beam = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    m = R.Model(R.MODEL)
    cd = R.ConstrainedDecoder(m)
    stat = dict(eng1=0, free1=0, con1=0, con3=0, union=0, n=0, ms=0.0)
    for py, ok in truth.items():
        e = eng.get(py, [''])
        _, free = m.generate('', py, beam=1)
        t0 = time.time()
        con = cd.generate('', py, beam=beam)
        stat['ms'] += (time.time() - t0) * 1000
        c = [x[0] for x in con]
        stat['n'] += 1
        stat['eng1'] += e[0] == ok
        stat['free1'] += free[0][0] == ok
        stat['con1'] += bool(c) and c[0] == ok
        stat['con3'] += ok in c[:3]
        stat['union'] += e[0] == ok or (bool(c) and c[0] == ok)
        mark = lambda x: '✓' if x == ok else '✗'
        print('%s\n  正确   %s\n  引擎   %s %s\n  自由   %s %s\n  约束   %s %s  前三: %s' % (
            py, ok, mark(e[0]), e[0], mark(free[0][0]), free[0][0],
            mark(c[0] if c else ''), c[0] if c else '(无)', '/'.join(c[:3])))
    n = stat['n']
    print('\n%d 句：引擎首选对 %d，自由贪心对 %d，约束首选对 %d，约束前三含正确 %d，引擎或约束首选有一个对 %d；'
          '约束解码平均 %.0f ms（NumPy）' % (n, stat['eng1'], stat['free1'], stat['con1'], stat['con3'],
                                       stat['union'], stat['ms'] / n))


if __name__ == '__main__':
    main()
