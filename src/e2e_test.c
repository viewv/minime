/* e2e_test.c — e2e 计算后端的数值对照测试
 *
 *   e2e_test dump <输出文件> [后端]          按标注句子构造 token 序列，算出每个位置的 logits 写进文件，
 *                                             给 scripts/e2e_check.py 和 NumPy 参考实现比
 *   e2e_test compare <后端A> <后端B>          两个后端逐位置比 logits，再比 45 段输入的解码结果
 *   e2e_test bench <后端>...                  固定批大小（1/4/16/64 个 token）的前向各跑多次取平均
 *
 * 序列来自 tests/e2e_truth.tsv（拼音\t正确句子）：[CLS] 上下文 [SEP] 逐个字母 [SEP] [CLS] 正确句子 [SEP]，
 * 每句做"无上下文"和"有上下文"两份。基准输入来自 tests/baseline_inputs.txt。
 * 模型目录默认 vendor/mac/imeData（MINIME_E2E_DIR 可改）。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "e2e.h"

enum { V = 10496, MAXSEQ = 64, MAXTOK = 256 };

typedef struct { int n; int tok[MAXTOK]; } Seq;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static E2EModel *open_model(const char *backend)
{
    const char *dir = getenv("MINIME_E2E_DIR") ? getenv("MINIME_E2E_DIR") : "vendor/mac/imeData";
    char model[1024], mpd[1024], err[256];
    snprintf(model, sizeof model, "%s/decgnr.bin", dir);
    snprintf(mpd, sizeof mpd, "%s/mpd_e2e.bin", dir);
    E2EModel *m = e2e_open_backend(model, mpd, backend, 0, err, sizeof err);
    if (!m) {
        fprintf(stderr, "打开模型（后端 %s）失败: %s\n", backend, err);
        exit(1);
    }
    fprintf(stderr, "后端 %s 已就绪（实际：%s）\n", backend, e2e_backend_name(m));
    return m;
}

static int build_seqs(E2EModel *m, Seq *seqs)
{
    static const char *ctxs[] = { "", "我在写一份工作报告，" };
    FILE *f = fopen("tests/e2e_truth.tsv", "r");
    if (!f) {
        fprintf(stderr, "读不了 tests/e2e_truth.tsv（要在仓库根目录运行）\n");
        exit(1);
    }
    char line[1024];
    int ns = 0;
    while (fgets(line, sizeof line, f) && ns + 2 <= MAXSEQ) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = 0;
        const char *py = line, *truth = tab + 1;
        for (int c = 0; c < 2; c++) {
            Seq *s = &seqs[ns++];
            s->n = 0;
            s->tok[s->n++] = 101;
            s->n += e2e_encode(m, ctxs[c], s->tok + s->n, 64);
            s->tok[s->n++] = 102;
            for (const char *p = py; *p && s->n < MAXTOK - 80; p++) {
                char l[2] = { *p, 0 };
                int id;
                e2e_encode(m, l, &id, 1);
                s->tok[s->n++] = id;
            }
            s->tok[s->n++] = 102;
            s->tok[s->n++] = 101;
            s->n += e2e_encode(m, truth, s->tok + s->n, MAXTOK - s->n - 1);
            s->tok[s->n++] = 102;
        }
    }
    fclose(f);
    return ns;
}

static int argmax(const float *x)
{
    int k = 0;
    for (int i = 1; i < V; i++)
        if (x[i] > x[k])
            k = i;
    return k;
}

static int cmd_dump(const char *out, const char *backend)
{
    E2EModel *m = open_model(backend);
    static Seq seqs[MAXSEQ];
    int ns = build_seqs(m, seqs);
    FILE *f = fopen(out, "wb");
    fwrite(&ns, 4, 1, f);
    float *lg = malloc(sizeof(float) * MAXTOK * V);
    double t = now_ms();
    for (int i = 0; i < ns; i++) {
        if (e2e_debug_logits(m, seqs[i].tok, seqs[i].n, lg) < 0) {
            fprintf(stderr, "第 %d 段前向失败\n", i);
            return 1;
        }
        fwrite(&seqs[i].n, 4, 1, f);
        fwrite(seqs[i].tok, 4, (size_t)seqs[i].n, f);
        fwrite(lg, sizeof(float) * V, (size_t)seqs[i].n, f);
    }
    fclose(f);
    fprintf(stderr, "%d 段序列的 logits 已写进 %s（%.0f ms）\n", ns, out, now_ms() - t);
    free(lg);
    e2e_close(m);
    return 0;
}

static int cmd_compare(const char *ba, const char *bb)
{
    E2EModel *a = open_model(ba), *b = open_model(bb);
    static Seq seqs[MAXSEQ];
    int ns = build_seqs(a, seqs);
    float *la = malloc(sizeof(float) * MAXTOK * V), *lb = malloc(sizeof(float) * MAXTOK * V);
    double maxabs = 0, maxrel = 0;
    long pos = 0, same_top = 0;
    double ta = 0, tb = 0;
    for (int i = 0; i < ns; i++) {
        double t = now_ms();
        e2e_debug_logits(a, seqs[i].tok, seqs[i].n, la);
        ta += now_ms() - t;
        t = now_ms();
        e2e_debug_logits(b, seqs[i].tok, seqs[i].n, lb);
        tb += now_ms() - t;
        for (int p = 0; p < seqs[i].n; p++) {
            const float *x = la + (size_t)p * V, *y = lb + (size_t)p * V;
            double scale = 0, d = 0;
            for (int k = 0; k < V; k++) {
                double e = fabs((double)x[k] - y[k]);
                if (e > d) d = e;
                if (fabs(x[k]) > scale) scale = fabs(x[k]);
            }
            if (d > maxabs) maxabs = d;
            if (d / (scale + 1e-6) > maxrel) maxrel = d / (scale + 1e-6);
            same_top += argmax(x) == argmax(y);
            pos++;
        }
    }
    printf("logits：%d 段 %ld 个位置，最大绝对差 %.5f，最大相对差 %.2e，首选 token 一致 %ld/%ld；"
           "整段前向 %s %.0f ms，%s %.0f ms\n", ns, pos, maxabs, maxrel, same_top, pos,
           e2e_backend_name(a), ta, e2e_backend_name(b), tb);

    FILE *f = fopen("tests/baseline_inputs.txt", "r");
    char w[512];
    int n = 0, same1 = 0, same3 = 0;
    E2EResult ra[3], rb[3];
    ta = tb = 0;
    while (f && fscanf(f, "%511s", w) == 1) {
        double t = now_ms();
        int na = e2e_decode(a, "", w, 4, 0, NULL, ra, 3);
        ta += now_ms() - t;
        t = now_ms();
        int nb = e2e_decode(b, "", w, 4, 0, NULL, rb, 3);
        tb += now_ms() - t;
        n++;
        int s1 = (na == 0 && nb == 0) || (na > 0 && nb > 0 && !strcmp(ra[0].text, rb[0].text));
        int s3 = na == nb;
        for (int k = 0; s3 && k < na; k++)
            s3 = !strcmp(ra[k].text, rb[k].text);
        same1 += s1;
        same3 += s3;
        if (!s3)
            printf("  不同：%s\n    %s: %s / %s / %s\n    %s: %s / %s / %s\n", w,
                   e2e_backend_name(a), na > 0 ? ra[0].text : "", na > 1 ? ra[1].text : "", na > 2 ? ra[2].text : "",
                   e2e_backend_name(b), nb > 0 ? rb[0].text : "", nb > 1 ? rb[1].text : "", nb > 2 ? rb[2].text : "");
    }
    if (f)
        fclose(f);
    printf("解码：%d 段，首选相同 %d，前三完全相同 %d；平均 %s %.1f ms，%s %.1f ms\n", n, same1, same3,
           e2e_backend_name(a), ta / n, e2e_backend_name(b), tb / n);
    e2e_close(a);
    e2e_close(b);
    return same1 == n ? 0 : 1;
}

/* 固定批大小的前向反复跑，取平均（看内核速度用，比整段解码稳定） */
static int cmd_bench(int argc, char **argv)
{
    static int sizes[] = { 1, 4, 16, 64 };
    int nsz = 4;
    if (getenv("E2E_BENCH_N")) {           /* 只测一个批大小（配合 MINIME_E2E_VK_PROF 看单个批大小的剖析） */
        sizes[0] = atoi(getenv("E2E_BENCH_N"));
        nsz = 1;
    }
    float *lg = malloc(sizeof(float) * 64 * V);
    int tok[64];
    for (int i = 0; i < 64; i++)
        tok[i] = 101 + (i % 50) * 37;
    for (int a = 0; a < argc; a++) {
        E2EModel *m = open_model(argv[a]);
        printf("%-12s", e2e_backend_name(m));
        /* GPU 频率随负载爬升：先连续跑约 1 秒预热，每个批大小跑 5 轮取最快的一轮 */
        for (double t0 = now_ms(); now_ms() - t0 < 1000;)
            e2e_debug_logits(m, tok, 16, lg);
        for (int k = 0; k < nsz; k++) {
            int n = sizes[k], reps = n >= 16 ? 20 : 100;
            double best = 1e30;
            for (int trial = 0; trial < 5; trial++) {
                double t = now_ms();
                for (int r = 0; r < reps; r++)
                    e2e_debug_logits(m, tok, n, lg);
                double dt = (now_ms() - t) / reps;
                if (dt < best) best = dt;
            }
            printf("  %2d 个 token：%7.2f ms", n, best);
            fflush(stdout);
        }
        printf("\n");
        e2e_close(m);
    }
    free(lg);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "bench"))
        return cmd_bench(argc - 2, argv + 2);
    if (argc >= 3 && !strcmp(argv[1], "dump"))
        return cmd_dump(argv[2], argc > 3 ? argv[3] : "cpu");
    if (argc >= 4 && !strcmp(argv[1], "compare"))
        return cmd_compare(argv[2], argv[3]);
    fprintf(stderr, "用法: %s dump <输出文件> [后端] | compare <后端A> <后端B> | bench <后端>...\n", argv[0]);
    return 2;
}
