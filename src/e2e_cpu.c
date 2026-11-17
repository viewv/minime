/* e2e_cpu.c — e2e 模型的 CPU 后端：AVX2/FMA/F16C，int4/int8 权重不解包直接算，线程池按 8 行块切分。
 * 接口见 e2e_internal.h。 */
#define _GNU_SOURCE
#include <immintrin.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "e2e_internal.h"

enum { H = E2E_H, NH = E2E_NH, HD = E2E_HD, FF = E2E_FF, V = E2E_V, NL = E2E_NL };
enum { MAXB = 64 };             /* 一次前向的最大 token 数 */
#define SLOT_FLOATS ((size_t)NL * 2 * H)

/* ================= 线程池 ================= */
typedef void (*TaskFn)(void *arg, int begin, int end);
typedef struct Pool Pool;
typedef struct { Pool *p; int idx; } Worker;
struct Pool {
    int n;                        /* 工作线程数（不含调用线程） */
    pthread_t th[16];
    Worker w[16];
    pthread_mutex_t mu;
    pthread_cond_t cv, done;
    unsigned gen;
    int pending, quit;
    TaskFn fn;
    void *arg;
    int total;
};

static void pool_part(Pool *p, int idx, int *b, int *e)
{
    int parts = p->n + 1;
    *b = (int)((long)p->total * idx / parts);
    *e = (int)((long)p->total * (idx + 1) / parts);
}

static void *pool_main(void *a)
{
    Worker *w = a;
    Pool *p = w->p;
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (p->gen == seen && !p->quit)
            pthread_cond_wait(&p->cv, &p->mu);
        if (p->quit) {
            pthread_mutex_unlock(&p->mu);
            return NULL;
        }
        seen = p->gen;
        pthread_mutex_unlock(&p->mu);
        int b, e;
        pool_part(p, w->idx + 1, &b, &e);
        if (b < e)
            p->fn(p->arg, b, e);
        pthread_mutex_lock(&p->mu);
        if (--p->pending == 0)
            pthread_cond_signal(&p->done);
        pthread_mutex_unlock(&p->mu);
    }
}

static void pool_run(Pool *p, TaskFn fn, void *arg, int total)
{
    if (p->n == 0 || total < 2) {
        fn(arg, 0, total);
        return;
    }
    pthread_mutex_lock(&p->mu);
    p->fn = fn;
    p->arg = arg;
    p->total = total;
    p->pending = p->n;
    p->gen++;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    int b, e;
    pool_part(p, 0, &b, &e);
    if (b < e)
        fn(arg, b, e);
    pthread_mutex_lock(&p->mu);
    while (p->pending)
        pthread_cond_wait(&p->done, &p->mu);
    pthread_mutex_unlock(&p->mu);
}

/* ================= 矩阵 × 向量组 =================
 * Y[b][r] = sum_c W[r][c] * X[b][c] + bias[r]，b < B。按 8 行一块分给线程。 */
typedef struct {
    const E2EQMat *w;
    const float *X;   /* [B][C] */
    float *Y;         /* [B][R] */
    const float *bias;
    int B;
} MatArg;

static void q4_task(void *a, int blk0, int blk1)
{
    const MatArg *m = a;
    const int C = m->w->C, R = m->w->R, G = C / 32;
    const __m256i m15 = _mm256_set1_epi32(15), m8 = _mm256_set1_epi32(8);
    for (int b0 = 0; b0 < m->B; b0 += 4) {
        int nb = m->B - b0 < 4 ? m->B - b0 : 4;
        const float *x0 = m->X + (size_t)b0 * C;
        for (int blk = blk0; blk < blk1; blk++) {
            const uint8_t *q = m->w->q + (size_t)blk * (C / 2) * 8;
            const uint16_t *s = m->w->s + (size_t)blk * G * 8;
            __m256 acc0 = _mm256_setzero_ps(), acc1 = acc0, acc2 = acc0, acc3 = acc0;
            for (int g = 0; g < G; g++) {
                __m256 p0 = _mm256_setzero_ps(), p1 = p0, p2 = p0, p3 = p0;
                const uint8_t *qg = q + (size_t)g * 16 * 8;
                const float *xg = x0 + g * 32;
                for (int cp = 0; cp < 16; cp++) {
                    __m256i v = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *)(qg + cp * 8)));
                    __m256 hi = _mm256_cvtepi32_ps(_mm256_sub_epi32(_mm256_srli_epi32(v, 4), m8));
                    __m256 lo = _mm256_cvtepi32_ps(_mm256_sub_epi32(_mm256_and_si256(v, m15), m8));
                    const float *xc = xg + cp * 2;
                    p0 = _mm256_fmadd_ps(hi, _mm256_broadcast_ss(xc), p0);
                    p0 = _mm256_fmadd_ps(lo, _mm256_broadcast_ss(xc + 1), p0);
                    if (nb > 1) {
                        p1 = _mm256_fmadd_ps(hi, _mm256_broadcast_ss(xc + C), p1);
                        p1 = _mm256_fmadd_ps(lo, _mm256_broadcast_ss(xc + C + 1), p1);
                    }
                    if (nb > 2) {
                        p2 = _mm256_fmadd_ps(hi, _mm256_broadcast_ss(xc + 2 * C), p2);
                        p2 = _mm256_fmadd_ps(lo, _mm256_broadcast_ss(xc + 2 * C + 1), p2);
                    }
                    if (nb > 3) {
                        p3 = _mm256_fmadd_ps(hi, _mm256_broadcast_ss(xc + 3 * C), p3);
                        p3 = _mm256_fmadd_ps(lo, _mm256_broadcast_ss(xc + 3 * C + 1), p3);
                    }
                }
                __m256 sc = _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(s + g * 8)));
                acc0 = _mm256_fmadd_ps(p0, sc, acc0);
                acc1 = _mm256_fmadd_ps(p1, sc, acc1);
                acc2 = _mm256_fmadd_ps(p2, sc, acc2);
                acc3 = _mm256_fmadd_ps(p3, sc, acc3);
            }
            __m256 bias = m->bias ? _mm256_loadu_ps(m->bias + blk * 8) : _mm256_setzero_ps();
            __m256 accs[4] = { acc0, acc1, acc2, acc3 };
            for (int k = 0; k < nb; k++)
                _mm256_storeu_ps(m->Y + (size_t)(b0 + k) * R + blk * 8, _mm256_add_ps(accs[k], bias));
        }
    }
}

static void q8_task(void *a, int blk0, int blk1)
{
    const MatArg *m = a;
    const int C = m->w->C, R = m->w->R, G = C / 32;
    for (int b0 = 0; b0 < m->B; b0 += 4) {
        int nb = m->B - b0 < 4 ? m->B - b0 : 4;
        const float *x0 = m->X + (size_t)b0 * C;
        for (int blk = blk0; blk < blk1; blk++) {
            const uint8_t *q = m->w->q + (size_t)blk * C * 8;
            const uint16_t *s = m->w->s + (size_t)blk * G * 8;
            __m256 acc0 = _mm256_setzero_ps(), acc1 = acc0, acc2 = acc0, acc3 = acc0;
            for (int g = 0; g < G; g++) {
                __m256 p0 = _mm256_setzero_ps(), p1 = p0, p2 = p0, p3 = p0;
                for (int c = g * 32; c < g * 32 + 32; c++) {
                    __m256 w = _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_loadl_epi64((const __m128i *)(q + (size_t)c * 8))));
                    p0 = _mm256_fmadd_ps(w, _mm256_broadcast_ss(x0 + c), p0);
                    if (nb > 1) p1 = _mm256_fmadd_ps(w, _mm256_broadcast_ss(x0 + C + c), p1);
                    if (nb > 2) p2 = _mm256_fmadd_ps(w, _mm256_broadcast_ss(x0 + 2 * C + c), p2);
                    if (nb > 3) p3 = _mm256_fmadd_ps(w, _mm256_broadcast_ss(x0 + 3 * C + c), p3);
                }
                __m256 sc = _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(s + g * 8)));
                acc0 = _mm256_fmadd_ps(p0, sc, acc0);
                acc1 = _mm256_fmadd_ps(p1, sc, acc1);
                acc2 = _mm256_fmadd_ps(p2, sc, acc2);
                acc3 = _mm256_fmadd_ps(p3, sc, acc3);
            }
            __m256 accs[4] = { acc0, acc1, acc2, acc3 };
            for (int k = 0; k < nb; k++)
                _mm256_storeu_ps(m->Y + (size_t)(b0 + k) * R + blk * 8, accs[k]);
        }
    }
}

/* ================= 后端 ================= */
typedef struct {
    E2EBackend base;
    const E2EWeights *w;
    Pool pool;
    float *kv;                  /* [cap][NL][2][H] */
    int cap;
    float *x, *h, *qkv, *att, *up, *tmp;
    const float **keys, **vals;
    float *sc;
    int ctxcap;
} Cpu;

static void matmul(Cpu *c, const E2EQMat *w, const float *X, int B, float *Y, const float *bias)
{
    MatArg a = { w, X, Y, bias, B };
    pool_run(&c->pool, w->bits == 4 ? q4_task : q8_task, &a, w->R / 8);
}

static void layernorm(const float *x, const float *w, const float *b, float *o)
{
    float mean = 0, var = 0;
    for (int i = 0; i < H; i++)
        mean += x[i];
    mean /= H;
    for (int i = 0; i < H; i++)
        var += (x[i] - mean) * (x[i] - mean);
    float inv = 1.0f / sqrtf(var / H + 1e-5f);
    for (int i = 0; i < H; i++)
        o[i] = (x[i] - mean) * inv * w[i] + b[i];
}

static float dot64(const float *a, const float *b)
{
    __m256 s = _mm256_setzero_ps();
    for (int i = 0; i < HD; i += 8)
        s = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), s);
    __m128 r = _mm_add_ps(_mm256_castps256_ps128(s), _mm256_extractf128_ps(s, 1));
    r = _mm_hadd_ps(r, r);
    r = _mm_hadd_ps(r, r);
    return _mm_cvtss_f32(r);
}

static void attend(const float *q, const float *const *keys, const float *const *vals, int n, float *out, float *sc)
{
    for (int hd = 0; hd < NH; hd++) {
        float mx = -1e30f;
        for (int i = 0; i < n; i++) {
            sc[i] = dot64(q + hd * HD, keys[i] + hd * HD) * 0.125f;
            if (sc[i] > mx)
                mx = sc[i];
        }
        float sum = 0;
        for (int i = 0; i < n; i++) {
            sc[i] = expf(sc[i] - mx);
            sum += sc[i];
        }
        float *o = out + hd * HD;
        memset(o, 0, sizeof(float) * HD);
        for (int i = 0; i < n; i++) {
            float w = sc[i] / sum;
            const float *v = vals[i] + hd * HD;
            for (int k = 0; k < HD; k++)
                o[k] += w * v[k];
        }
    }
}

static float gelu(float x)
{
    return 0.5f * x * (1.0f + tanhf(0.7978845608028654f * (x + 0.044715f * x * x * x)));
}

static int cpu_reserve(E2EBackend *b, int nslots)
{
    Cpu *c = (Cpu *)b;
    if (nslots <= c->cap)
        return 0;
    float *kv = realloc(c->kv, sizeof(float) * SLOT_FLOATS * (size_t)nslots);
    if (!kv)
        return -1;
    c->kv = kv;
    c->cap = nslots;
    return 0;
}

static int cpu_forward(E2EBackend *b, const E2EBatch *bt)
{
    Cpu *c = (Cpu *)b;
    const E2EWeights *w = c->w;
    const int B = bt->n;
    if (B < 1 || B > MAXB)
        return -1;
    int maxctx = 0;
    for (int i = 0; i < B; i++) {
        if (bt->slot[i] < 0 || bt->slot[i] >= c->cap)
            return -1;
        if (bt->ctx_len[i] > maxctx)
            maxctx = bt->ctx_len[i];
    }
    if (maxctx > c->ctxcap) {
        c->ctxcap = maxctx;
        c->keys = realloc(c->keys, sizeof(float *) * (size_t)maxctx);
        c->vals = realloc(c->vals, sizeof(float *) * (size_t)maxctx);
        c->sc = realloc(c->sc, sizeof(float) * (size_t)maxctx);
    }
    for (int i = 0; i < B; i++)
        for (int k = 0; k < H; k += 8) {
            __m256 a = _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(w->wte + (size_t)bt->tok[i] * H + k)));
            __m256 p = _mm256_cvtph_ps(_mm_loadu_si128((const __m128i *)(w->wpe + (size_t)bt->pos[i] * H + k)));
            _mm256_storeu_ps(c->x + i * H + k, _mm256_add_ps(a, p));
        }
    for (int l = 0; l < NL; l++) {
        const E2ELayer *L = &w->L[l];
        for (int i = 0; i < B; i++)
            layernorm(c->x + i * H, L->ln1w, L->ln1b, c->h + i * H);
        matmul(c, &L->qkv, c->h, B, c->qkv, L->qkvb);
        for (int i = 0; i < B; i++) {
            float *kv = c->kv + (size_t)bt->slot[i] * SLOT_FLOATS + (size_t)l * 2 * H;
            memcpy(kv, c->qkv + i * 3 * H + H, sizeof(float) * H);
            memcpy(kv + H, c->qkv + i * 3 * H + 2 * H, sizeof(float) * H);
        }
        for (int i = 0; i < B; i++) {
            const int *ctx = bt->ctx + bt->ctx_off[i];
            for (int j = 0; j < bt->ctx_len[i]; j++) {
                c->keys[j] = c->kv + (size_t)ctx[j] * SLOT_FLOATS + (size_t)l * 2 * H;
                c->vals[j] = c->keys[j] + H;
            }
            attend(c->qkv + i * 3 * H, c->keys, c->vals, bt->ctx_len[i], c->att + i * H, c->sc);
        }
        matmul(c, &L->o, c->att, B, c->tmp, L->ob);
        for (int i = 0; i < B * H; i++)
            c->x[i] += c->tmp[i];
        for (int i = 0; i < B; i++)
            layernorm(c->x + i * H, L->ln2w, L->ln2b, c->h + i * H);
        matmul(c, &L->up, c->h, B, c->up, L->upb);
        for (int i = 0; i < B * FF; i++)
            c->up[i] = gelu(c->up[i]);
        matmul(c, &L->down, c->up, B, c->tmp, L->downb);
        for (int i = 0; i < B * H; i++)
            c->x[i] += c->tmp[i];
    }
    int nl = B - bt->logits_from;
    if (nl > 0) {
        for (int i = 0; i < nl; i++)
            layernorm(c->x + (bt->logits_from + i) * H, w->lnfw, w->lnfb, c->h + i * H);
        matmul(c, &w->head, c->h, nl, bt->logits, NULL);
    }
    return 0;
}

static void cpu_destroy(E2EBackend *b)
{
    Cpu *c = (Cpu *)b;
    Pool *p = &c->pool;
    if (p->n) {
        pthread_mutex_lock(&p->mu);
        p->quit = 1;
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->mu);
        for (int i = 0; i < p->n; i++)
            pthread_join(p->th[i], NULL);
    }
    free(c->kv); free(c->x); free(c->h); free(c->qkv); free(c->att); free(c->up); free(c->tmp);
    free(c->keys); free(c->vals); free(c->sc);
    free(c);
}

E2EBackend *e2e_cpu_new(const E2EWeights *w, int threads, char *err, size_t errlen)
{
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("fma") || !__builtin_cpu_supports("f16c")) {
        snprintf(err, errlen, "CPU 不支持 AVX2/FMA/F16C");
        return NULL;
    }
    Cpu *c = calloc(1, sizeof(*c));
    c->base.name = "cpu";
    c->base.reserve = cpu_reserve;
    c->base.forward = cpu_forward;
    c->base.destroy = cpu_destroy;
    c->w = w;
    c->x = malloc(sizeof(float) * MAXB * H);
    c->h = malloc(sizeof(float) * MAXB * H);
    c->qkv = malloc(sizeof(float) * MAXB * 3 * H);
    c->att = malloc(sizeof(float) * MAXB * H);
    c->up = malloc(sizeof(float) * MAXB * FF);
    c->tmp = malloc(sizeof(float) * MAXB * H);
    if (threads <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        threads = n > 8 ? 8 : (int)n;
    }
    if (threads > 16)
        threads = 16;
    Pool *p = &c->pool;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    pthread_cond_init(&p->done, NULL);
    p->n = threads > 1 ? threads - 1 : 0;
    for (int i = 0; i < p->n; i++) {
        p->w[i] = (Worker){ p, i };
        pthread_create(&p->th[i], NULL, pool_main, &p->w[i]);
    }
    return &c->base;
}
