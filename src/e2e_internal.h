/* e2e_internal.h — e2e 模型内部共用的定义：权重视图和计算后端接口。
 *
 * e2e.c 负责模型文件、词表、读音、beam search；具体的前向计算交给后端（e2e_cpu.c、e2e_vk.c）。
 *
 * KV 缓存按"槽位"管理：一个槽位放一个位置在全部层的 K 和 V（NL × 2 × H 个数）。
 * 提示的第 i 个位置用槽位 i（i < PMAX），生成的 token 用 PMAX 之后的槽位。
 * 后端只管存储和计算，哪个 token 能看到哪些槽位由调用方给出。
 */
#ifndef MINIME_E2E_INTERNAL_H
#define MINIME_E2E_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

enum { E2E_H = 768, E2E_NH = 12, E2E_HD = 64, E2E_FF = 3072, E2E_V = 10496, E2E_NL = 12, E2E_PMAX = 512 };

/* 量化矩阵（文件里的原样数据，mmap 进来的指针） */
typedef struct {
    const uint8_t  *q;     /* int4：[R/8][C/2][8] 字节，高 4 位第 2c 列、低 4 位第 2c+1 列；int8：[R/8][C][8] */
    const uint16_t *s;     /* fp16 scale：[R/8][C/32][8] */
    int R, C, bits;
    size_t qbytes;         /* q 的字节数（scale 紧跟其后） */
} E2EQMat;

typedef struct {
    const float *ln1w, *ln1b, *ln2w, *ln2b;      /* LayerNorm（fp32，在文件里） */
    E2EQMat qkv, o, up, down;
    const float *qkvb, *ob, *upb, *downb;        /* 偏置（加载时从 fp16 转成 fp32） */
} E2ELayer;

typedef struct {
    const uint16_t *wte, *wpe;                   /* fp16 嵌入 [V][H]、[PMAX][H] */
    const float *lnfw, *lnfb;
    E2EQMat head;                                /* int8 [V][H] */
    E2ELayer L[E2E_NL];
} E2EWeights;

/* 一批 token 一起前向 */
typedef struct {
    int n;                      /* token 数 */
    const int *tok;             /* [n] token id */
    const int *pos;             /* [n] 位置（位置嵌入用） */
    const int *slot;            /* [n] 这个 token 的 K/V 写进哪个槽位 */
    const int *ctx_off;         /* [n] 第 i 个 token 能看到的槽位是 ctx[ctx_off[i] .. ctx_off[i]+ctx_len[i])，按位置顺序，含自己 */
    const int *ctx_len;
    const int *ctx;
    int logits_from;            /* 只给 [logits_from, n) 这些 token 算 logits；>= n 表示都不算 */
    float *logits;              /* [(n - logits_from)][V] */
} E2EBatch;

typedef struct E2EBackend E2EBackend;
struct E2EBackend {
    const char *name;
    /* KV 池至少要有 nslots 个槽位（已有的内容要保留） */
    int  (*reserve)(E2EBackend *b, int nslots);
    /* 同一批里后面的 token 可以看到前面 token 刚写的槽位（逐层先写 K/V 再算注意力） */
    int  (*forward)(E2EBackend *b, const E2EBatch *batch);
    void (*destroy)(E2EBackend *b);
};

E2EBackend *e2e_cpu_new(const E2EWeights *w, int threads, char *err, size_t errlen);
E2EBackend *e2e_vk_new(const E2EWeights *w, char *err, size_t errlen);   /* 没有 Vulkan 或失败返回 NULL */

#endif
