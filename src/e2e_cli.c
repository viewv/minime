/* e2e_cli.c — e2e 整句模型的命令行工具（测试和测速用）
 *
 *   e2e_cli [-b beam] [-t 线程数] [-c 光标前文本] [-n 前几个] [-j 简拼扣分] 拼音...
 *           -j 打开简拼（一个字可以只打声母），参数是每个缩写音节扣的分
 *           -f 模糊音开关（位掩码，见 e2e.h 的 e2e_set_fuzzy，如 0x200 = z/zh）
 *   e2e_cli -s [...]          从标准输入每行读一段拼音（批量），每行输出 "拼音\t候选1|候选2|…\t毫秒\t分数1|分数2|…"
 *                             行写成 "光标前文字\t拼音" 时这一行用自己的上下文（不写就用 -c 的）；
 *                             写成 "光标前文字\t拼音\t句子1|句子2…" 时不解码，输出这些句子的分数（e2e_score）
 *
 * 模型文件默认 vendor/mac/imeData/{decgnr.bin,mpd_e2e.bin}，可用 MINIME_E2E_DIR 改目录。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <unistd.h>

#include "e2e.h"

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    int beam = 4, threads = 0, top = 3, batch = 0, opt;
    float jp = -1;
    unsigned fuzzy = 0;
    const char *ctx = "";
    while ((opt = getopt(argc, argv, "b:t:c:n:sj:f:")) != -1) {
        switch (opt) {
        case 'b': beam = atoi(optarg); break;
        case 't': threads = atoi(optarg); break;
        case 'c': ctx = optarg; break;
        case 'n': top = atoi(optarg); break;
        case 's': batch = 1; break;
        case 'j': jp = (float)atof(optarg); break;
        case 'f': fuzzy = (unsigned)strtoul(optarg, NULL, 0); break;
        default:
            fprintf(stderr, "用法: %s [-b beam] [-t 线程] [-c 上下文] [-n 前几个] [-s] 拼音...\n", argv[0]);
            return 2;
        }
    }
    const char *dir = getenv("MINIME_E2E_DIR") ? getenv("MINIME_E2E_DIR") : "vendor/mac/imeData";
    char model[1024], mpd[1024], err[256];
    snprintf(model, sizeof model, "%s/decgnr.bin", dir);
    snprintf(mpd, sizeof mpd, "%s/mpd_e2e.bin", dir);
    double t0 = now_ms();
    E2EModel *m = e2e_open(model, mpd, threads, err, sizeof err);
    if (!m) {
        fprintf(stderr, "打开模型失败: %s\n", err);
        return 1;
    }
    fprintf(stderr, "模型加载 %.0f ms\n", now_ms() - t0);
    if (jp >= 0)
        e2e_set_jianpin(m, 1, jp);
    if (fuzzy)
        e2e_set_fuzzy(m, fuzzy);
    E2EResult res[16];
    if (top > 16)
        top = 16;
    if (batch) {
        char line[1024];
        double total = 0;
        int cnt = 0;
        while (fgets(line, sizeof line, stdin)) {
            line[strcspn(line, "\r\n")] = 0;
            const char *lctx = ctx, *py = line;
            char *tab = strchr(line, '\t');
            char *score_list = NULL;
            if (tab) {
                *tab = 0;
                lctx = line;
                py = tab + 1;
                char *tab2 = strchr(py, '\t');
                if (tab2) {
                    *tab2 = 0;
                    score_list = tab2 + 1;
                }
            }
            if (score_list) {           /* 打分模式：输出 拼音\t句子1|…\t分数1|… */
                printf("%s\t%s\t", py, score_list);
                int first = 1;
                for (char *tx = strtok(score_list, "|"); tx; tx = strtok(NULL, "|")) {
                    float sc;
                    if (e2e_score(m, lctx, py, tx, &sc) < 0)
                        sc = -1e30f;
                    printf("%s%.3f", first ? "" : "|", sc);
                    first = 0;
                }
                printf("\n");
                fflush(stdout);
                continue;
            }
            if (!py[0])
                continue;
            double t = now_ms();
            int n = e2e_decode(m, lctx, py, beam, 0, NULL, res, top);
            double dt = now_ms() - t;
            total += dt;
            cnt++;
            printf("%s\t", py);
            for (int i = 0; i < n; i++)
                printf("%s%s", i ? "|" : "", res[i].text);
            printf("\t%.1f\t", dt);
            for (int i = 0; i < n; i++)
                printf("%s%.3f", i ? "|" : "", res[i].score);
            printf("\n");
            fflush(stdout);
        }
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        double cpu = ru.ru_utime.tv_sec * 1e3 + ru.ru_utime.tv_usec / 1e3 + ru.ru_stime.tv_sec * 1e3 + ru.ru_stime.tv_usec / 1e3;
        if (cnt)
            fprintf(stderr, "%d 段，平均 %.1f ms；进程 CPU 时间 %.1f s（解码期间平均占 %.2f 个核）\n", cnt, total / cnt,
                    cpu / 1e3, cpu / total);
    } else {
        char inp[1024] = "";
        for (int i = optind; i < argc; i++)
            strncat(inp, argv[i], sizeof inp - strlen(inp) - 1);
        double t = now_ms();
        int n = e2e_decode(m, ctx, inp, beam, 0, NULL, res, top);
        double dt = now_ms() - t;
        for (int i = 0; i < n; i++)
            printf("%s\t%.3f\n", res[i].text, res[i].score);
        if (n == 0)
            printf("（没有结果）\n");
        fprintf(stderr, "解码 %.1f ms\n", dt);
    }
    e2e_close(m);
    return 0;
}
