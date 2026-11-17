/* e2e.c — 微信输入法 e2e 整句模型：模型文件、词表、读音、拼音约束的 beam search。说明见 e2e.h 和 docs/whole-sentence.md。
 *
 * 结构：GPT-2，12 层，768 维，12 头，FFN 3072，词表 10496，可学习位置嵌入（512）。
 * 前向计算交给后端（e2e_internal.h）：CPU（e2e_cpu.c）或 Vulkan（e2e_vk.c）。
 */
#define _GNU_SOURCE
#include "e2e.h"

#include <ctype.h>
#include <fcntl.h>
#include <immintrin.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "e2e_internal.h"

enum { H = E2E_H, FF = E2E_FF, V = E2E_V, NL = E2E_NL, PMAX = E2E_PMAX };
enum { TOK_UNK = 100, TOK_CLS = 101, TOK_SEP = 102 };
enum { MAXB = 64 };             /* 一次前向的最大 token 数（预填按 64 个一批） */
enum { MAXBEAM = 16 };

/* ================= 小工具 ================= */
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static float h2f(uint16_t h) { return _cvtsh_ss(h); }

static const uint8_t *map_file(const char *path, size_t *size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
        return NULL;
    *size = (size_t)st.st_size;
    return p;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* UTF-8 解一个码点，返回字节数 */
static int utf8_dec(const char *s, uint32_t *cp)
{
    const uint8_t *u = (const uint8_t *)s;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] & 0xe0) == 0xc0 && u[1]) { *cp = (uint32_t)(u[0] & 0x1f) << 6 | (u[1] & 0x3f); return 2; }
    if ((u[0] & 0xf0) == 0xe0 && u[1] && u[2]) {
        *cp = (uint32_t)(u[0] & 0x0f) << 12 | (uint32_t)(u[1] & 0x3f) << 6 | (u[2] & 0x3f);
        return 3;
    }
    if ((u[0] & 0xf8) == 0xf0 && u[1] && u[2] && u[3]) {
        *cp = (uint32_t)(u[0] & 0x07) << 18 | (uint32_t)(u[1] & 0x3f) << 12 | (uint32_t)(u[2] & 0x3f) << 6 | (u[3] & 0x3f);
        return 4;
    }
    *cp = 0xfffd;
    return 1;
}

static int utf8_enc(uint32_t cp, char *o)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xe0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    o[0] = (char)(0xf0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

static int is_cjk(uint32_t c)
{
    return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x20000 && c <= 0x2A6DF) ||
           (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x2F800 && c <= 0x2FA1F);
}

/* ================= CityHash（abseil，只实现用得到的短键） ================= */
static const uint32_t C1 = 0xcc9e2d51, C2 = 0x1b873593;
static const uint64_t K0 = 0xc3a5c85c97cb3127ULL, K2 = 0x9ae16a3b2f90404fULL;

static uint32_t rot32(uint32_t v, int s) { return s ? (v >> s) | (v << (32 - s)) : v; }
static uint64_t rot64(uint64_t v, int s) { return s ? (v >> s) | (v << (64 - s)) : v; }
static uint32_t fmix(uint32_t h)
{
    h ^= h >> 16; h *= 0x85ebca6b; h ^= h >> 13; h *= 0xc2b2ae35; h ^= h >> 16;
    return h;
}
static uint32_t mur(uint32_t a, uint32_t h)
{
    a *= C1; a = rot32(a, 17); a *= C2; h ^= a; h = rot32(h, 19);
    return h * 5 + 0xe6546b64;
}

static uint32_t city32(const uint8_t *s, size_t n)
{
    if (n <= 4) {
        uint32_t b = 0, c = 9;
        for (size_t i = 0; i < n; i++) {
            b = b * C1 + (uint32_t)(int32_t)(int8_t)s[i];
            c ^= b;
        }
        return fmix(mur(b, mur((uint32_t)n, c)));
    }
    if (n <= 12) {
        uint32_t a = (uint32_t)n, b = (uint32_t)n * 5, c = 9, d = b;
        a += rd32(s); b += rd32(s + n - 4); c += rd32(s + ((n >> 1) & 4));
        return fmix(mur(c, mur(b, mur(a, d))));
    }
    if (n <= 24) {
        uint32_t a = rd32(s - 4 + (n >> 1)), b = rd32(s + 4), c = rd32(s + n - 8), d = rd32(s + (n >> 1)),
                 e = rd32(s), f = rd32(s + n - 4), h = (uint32_t)n;
        return fmix(mur(f, mur(e, mur(d, mur(c, mur(b, mur(a, h)))))));
    }
    return 0;   /* 单字用不到 */
}

static uint64_t hlen16(uint64_t u, uint64_t v, uint64_t mul)
{
    uint64_t a = (u ^ v) * mul; a ^= a >> 47;
    uint64_t b = (v ^ a) * mul; b ^= b >> 47;
    return b * mul;
}

static uint64_t city64(const uint8_t *s, size_t n)
{
    if (n >= 8) {
        uint64_t mul = K2 + n * 2, a = rd64(s) + K2, b = rd64(s + n - 8);
        uint64_t c = rot64(b, 37) * mul + a, d = (rot64(a, 25) + b) * mul;
        return hlen16(c, d, mul);
    }
    if (n >= 4) {
        uint64_t mul = K2 + n * 2, a = rd32(s);
        return hlen16(n + (a << 3), rd32(s + n - 4), mul);
    }
    if (n > 0) {
        uint8_t a = s[0], b = s[n >> 1], c = s[n - 1];
        uint32_t y = (uint32_t)a + ((uint32_t)b << 8), z = (uint32_t)n + ((uint32_t)c << 2);
        uint64_t v = (uint64_t)y * K2 ^ (uint64_t)z * K0;
        return (v ^ (v >> 47)) * K2;
    }
    return K2;
}

static uint64_t city64_seed(const uint8_t *s, size_t n, uint64_t seed)
{
    return hlen16(city64(s, n) - K2, seed, 0x9ddfea08eb382d69ULL);
}

/* mpd_e2e.bin 查单字读音，结果写进 out（每个读音一个 NUL 结尾串），返回个数 */
static int mpd_lookup(const uint8_t *d, size_t size, const char *word, char out[][8], int maxo)
{
    uint32_t nb = rd32(d + 4);
    size_t wl = strlen(word);
    uint32_t bi = city32((const uint8_t *)word, wl) % nb;
    uint32_t ch = (uint32_t)city64_seed((const uint8_t *)word, wl, 0x1dbba5);
    size_t a = rd32(d + 8 + 4 * (size_t)bi), b = bi + 1 < nb ? rd32(d + 8 + 4 * (size_t)(bi + 1)) : size;
    if (a >= size || b > size || b < a + 4)
        return 0;
    const uint8_t *blob = d + a;
    uint32_t n = rd32(blob);
    for (uint32_t i = 0; i < n; i++) {
        if (rd32(blob + 4 + 4 * i) != ch)
            continue;
        const uint8_t *ends = blob + 4 + 4 * n;
        size_t base = 4 + 6 * (size_t)n;
        size_t st = base + (i ? (size_t)(ends[2 * (i - 1)] | ends[2 * (i - 1) + 1] << 8) : 0);
        size_t en = base + (size_t)(ends[2 * i] | ends[2 * i + 1] << 8);
        uint8_t p[256];
        int pl = 0;
        for (size_t j = st; j < en && pl < (int)sizeof(p); j++)
            if ((j - st) % 5)
                p[pl++] = blob[j];          /* 每第 5 个字节是填充的 0 */
        if (pl < 1)
            return 0;
        int cnt = p[0], k = 1, no = 0;
        for (int r = 0; r < cnt && k < pl && no < maxo; r++) {
            int L = p[k];
            if (k + 1 + L > pl || L >= 8)
                break;
            memcpy(out[no], p + k + 1, (size_t)L);
            out[no][L] = 0;
            no++;
            k += 1 + L;
        }
        return no;
    }
    return 0;
}

/* ================= 词表 ================= */
typedef struct {
    char   **str;      /* id -> token（已去掉 JSON 转义） */
    int      n;
    uint32_t *hkey;    /* 开放寻址：hash -> id+1 */
    int     *hval;
    uint32_t hcap;
} Vocab;

static uint32_t fnv(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static int vocab_get(const Vocab *v, const char *s, size_t n)
{
    uint32_t h = fnv(s, n), i = h & (v->hcap - 1);
    while (v->hval[i]) {
        int id = v->hval[i] - 1;
        if (v->hkey[i] == h && strlen(v->str[id]) == n && !memcmp(v->str[id], s, n))
            return id;
        i = (i + 1) & (v->hcap - 1);
    }
    return -1;
}

static void vocab_put(Vocab *v, int id, char *s)
{
    if (id < 0 || id >= v->n || v->str[id])
        return;
    v->str[id] = s;
    uint32_t h = fnv(s, strlen(s)), i = h & (v->hcap - 1);
    while (v->hval[i])
        i = (i + 1) & (v->hcap - 1);
    v->hkey[i] = h;
    v->hval[i] = id + 1;
}

/* 解析 JSON 字符串（p 指向开头的引号），返回新分配的 UTF-8 串，*end 指向结尾引号之后 */
static char *json_str(const char *p, const char *lim, const char **end)
{
    char *o = malloc((size_t)(lim - p) < 1024 ? (size_t)(lim - p) + 1 : 1024), *q = o;
    p++;
    while (p < lim && *p != '"' && q - o < 1000) {
        if (*p == '\\' && p + 1 < lim) {
            p++;
            switch (*p) {
            case 'n': *q++ = '\n'; p++; break;
            case 't': *q++ = '\t'; p++; break;
            case 'r': *q++ = '\r'; p++; break;
            case 'b': *q++ = '\b'; p++; break;
            case 'f': *q++ = '\f'; p++; break;
            case 'u': {
                uint32_t cp = (uint32_t)strtoul((char[5]){ p[1], p[2], p[3], p[4], 0 }, NULL, 16);
                p += 5;
                if (cp >= 0xd800 && cp < 0xdc00 && p + 6 <= lim && p[0] == '\\' && p[1] == 'u') {
                    uint32_t lo = (uint32_t)strtoul((char[5]){ p[2], p[3], p[4], p[5], 0 }, NULL, 16);
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    p += 6;
                }
                q += utf8_enc(cp, q);
                break;
            }
            default: *q++ = *p++; break;
            }
        } else {
            *q++ = *p++;
        }
    }
    *q = 0;
    *end = p < lim ? p + 1 : p;
    return o;
}

static int vocab_load(Vocab *v, const uint8_t *d, size_t size)
{
    v->n = V;
    v->str = calloc(V, sizeof(char *));
    v->hcap = 1u << 15;
    v->hkey = calloc(v->hcap, sizeof(uint32_t));
    v->hval = calloc(v->hcap, sizeof(int));
    const char *base = (const char *)d + 0x4000, *lim = (const char *)d + (size < 0x40000 ? size : 0x40000);
    /* added_tokens：[PAD] [T1].. [UNK] [CLS] [SEP] [MASK] */
    const char *at = memmem(base, (size_t)(lim - base), "\"added_tokens\"", 14);
    const char *vb = memmem(base, (size_t)(lim - base), "\"vocab\": {", 10);
    if (!vb)
        return -1;
    if (at && at < vb) {
        const char *p = at;
        const char *stop = memmem(at, (size_t)(lim - at), "\"normalizer\"", 12);
        while (p && stop && p < stop) {
            const char *idp = memmem(p, (size_t)(stop - p), "\"id\":", 5);
            const char *cp = idp ? memmem(idp, (size_t)(stop - idp), "\"content\":", 10) : NULL;
            if (!idp || !cp)
                break;
            int id = atoi(idp + 5);
            const char *q = strchr(cp + 10, '"'), *e;
            vocab_put(v, id, json_str(q, stop, &e));
            p = e;
        }
    }
    const char *p = vb + 10;
    int count = 0;
    while (p < lim) {
        while (p < lim && *p != '"' && *p != '}')
            p++;
        if (p >= lim || *p == '}')
            break;
        const char *e;
        char *s = json_str(p, lim, &e);
        p = e;
        while (p < lim && *p != ':')
            p++;
        int id = atoi(p + 1);
        if (v->str[id] || id < 0 || id >= V)
            free(s);
        else
            vocab_put(v, id, s), count++;
        while (p < lim && *p != ',' && *p != '}')
            p++;
    }
    for (int i = 0; i < V; i++)
        if (!v->str[i])
            v->str[i] = strdup("[UNK]");
    return count > 10000 ? 0 : -1;
}

/* BERT 式切词 + WordPiece（和 e2e_ref.py 的 Tokenizer.encode 一致） */
static int encode(const Vocab *v, const char *text, int *ids, int max)
{
    int n = 0;
    char word[256];
    int wl = 0;
#define FLUSH_WORD()                                                                  \
    do {                                                                              \
        if (wl) {                                                                     \
            int st = 0, start_n = n;                                                  \
            while (st < wl && n < max) {                                              \
                int en = wl, id = -1;                                                 \
                char buf[300];                                                        \
                while (en > st) {                                                     \
                    int bl = 0;                                                       \
                    if (st) { buf[0] = '#'; buf[1] = '#'; bl = 2; }                   \
                    memcpy(buf + bl, word + st, (size_t)(en - st));                   \
                    bl += en - st;                                                    \
                    id = vocab_get(v, buf, (size_t)bl);                               \
                    if (id >= 0) break;                                               \
                    en--;                                                             \
                }                                                                     \
                if (en == st) { n = start_n; if (n < max) ids[n++] = TOK_UNK; break; } \
                ids[n++] = id;                                                        \
                st = en;                                                              \
            }                                                                         \
            wl = 0;                                                                   \
        }                                                                             \
    } while (0)
    for (const char *p = text; *p && n < max;) {
        uint32_t cp;
        int l = utf8_dec(p, &cp);
        if (cp < 0x80 && (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r')) {
            FLUSH_WORD();
        } else if (is_cjk(cp) || (cp < 0x80 && ispunct((int)cp)) || (cp >= 0x80 && !is_cjk(cp))) {
            /* 汉字、ASCII 标点、其他非 ASCII 字符（中文标点等）各自成词 */
            FLUSH_WORD();
            char c[5];
            int cl = utf8_enc(cp, c);
            int id = vocab_get(v, c, (size_t)cl);
            if (n < max)
                ids[n++] = id >= 0 ? id : TOK_UNK;
        } else if (wl < (int)sizeof(word) - 1) {
            word[wl++] = (char)(cp >= 'A' && cp <= 'Z' ? cp + 32 : cp);
        }
        p += l;
    }
    FLUSH_WORD();
#undef FLUSH_WORD
    return n;
}


/* ================= 模型 ================= */
typedef struct Node {         /* 生成出来的一个 token；beam 之间共享前缀 */
    struct Node *parent;
    int tok, depth, slot;     /* slot：这个 token 的 K/V 在后端 KV 池里的槽位 */
} Node;

struct E2EModel {
    const uint8_t *map, *mpd;
    size_t map_size, mpd_size;
    Vocab vocab;
    E2EWeights w;
    float *bias_mem[NL * 4];
    E2EBackend *be;
    struct Reading {
        char r[8]; int len; int *ids; int n, cap;
        char alt[8][8]; int nalt;           /* 模糊音下这个读音还接受的拼写（e2e_set_fuzzy 生成） */
    } *readings;
    int nreadings;
    int jianpin;              /* 简拼：一个字也可以只用声母（首字母，或 zh/ch/sh）匹配 */
    float jp_penalty;         /* 每个缩写的音节扣的分（让能按全拼解析的输入优先按全拼） */
    int prompt[PMAX], plen;   /* 上次的提示（按键之间复用公共前缀） */
    float last_logits[V];
    float *logits;            /* [MAXBEAM][V] */
    Node *nodes;
    int nnodes, capnodes;
    int *ctx;                 /* 前向用的槽位列表 */
    size_t ctxcap;
};

static const uint8_t *find_tensor(const uint8_t *d, size_t size, const char *name, size_t *bytes)
{
    size_t i = 0x36ad7;
    uint64_t n = rd64(d + i);
    i += 8;
    for (uint64_t k = 0; k < n && i + 16 < size; k++) {
        uint64_t nl = rd64(d + i);
        const char *nm = (const char *)d + i + 8;
        size_t j = i + 8 + nl;
        uint64_t sz = rd64(d + j);
        size_t a = (j + 8 + 4095) / 4096 * 4096;
        if (nl == strlen(name) && !memcmp(nm, name, nl)) {
            *bytes = sz;
            return a + sz <= size ? d + a : NULL;
        }
        i = a + sz;
    }
    return NULL;
}

static int qmat(E2EModel *m, const char *name, int R, int C, int bits, E2EQMat *out)
{
    size_t sz;
    const uint8_t *p = find_tensor(m->map, m->map_size, name, &sz);
    size_t data = (size_t)R * C * bits / 8, sc = (size_t)R * C / 32 * 2;
    if (!p || sz != data + sc)
        return -1;
    *out = (E2EQMat){ p, (const uint16_t *)(p + data), R, C, bits, data };
    return 0;
}

static const float *f32t(E2EModel *m, const char *name, size_t n)
{
    size_t sz;
    const uint8_t *p = find_tensor(m->map, m->map_size, name, &sz);
    return p && sz == n * 4 ? (const float *)p : NULL;
}

static float *f16t(E2EModel *m, const char *name, size_t n)
{
    size_t sz;
    const uint8_t *p = find_tensor(m->map, m->map_size, name, &sz);
    if (!p || sz != n * 2)
        return NULL;
    float *o = malloc(n * 4);
    for (size_t i = 0; i < n; i++)
        o[i] = h2f(((const uint16_t *)p)[i]);
    return o;
}

static void add_reading(E2EModel *m, const char *r, int id)
{
    int k;
    for (k = 0; k < m->nreadings; k++)
        if (!strcmp(m->readings[k].r, r))
            break;
    if (k == m->nreadings) {
        m->readings = realloc(m->readings, sizeof(*m->readings) * (size_t)(k + 1));
        memset(&m->readings[k], 0, sizeof(m->readings[k]));
        strcpy(m->readings[k].r, r);
        m->readings[k].len = (int)strlen(r);
        m->nreadings++;
    }
    struct Reading *g = &m->readings[k];
    if (g->n == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 8;
        g->ids = realloc(g->ids, sizeof(int) * (size_t)g->cap);
    }
    g->ids[g->n++] = id;
}

E2EModel *e2e_open_backend(const char *model, const char *mpd, const char *backend, int threads,
                           char *err, size_t errlen)
{
#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); e2e_close(m); return NULL; } while (0)
    E2EModel *m = calloc(1, sizeof(*m));
    m->map = map_file(model, &m->map_size);
    if (!m->map || m->map_size < 0x40000 || memcmp(m->map, "XLLM", 4))
        FAIL("读不了模型 %s", model);
    m->mpd = map_file(mpd, &m->mpd_size);
    if (!m->mpd || rd32(m->mpd) != 0x80000001u)
        FAIL("读不了读音表 %s", mpd);
    if (vocab_load(&m->vocab, m->map, m->map_size) < 0)
        FAIL("模型里的词表解析失败");

    E2EWeights *w = &m->w;
    size_t sz;
    w->wte = (const uint16_t *)find_tensor(m->map, m->map_size, "tok_embeddings.weight", &sz);
    if (!w->wte || sz != (size_t)V * H * 2) FAIL("tok_embeddings 不对");
    w->wpe = (const uint16_t *)find_tensor(m->map, m->map_size, "pos_embeddings.weight", &sz);
    if (!w->wpe || sz != (size_t)PMAX * H * 2) FAIL("pos_embeddings 不对");
    w->lnfw = f32t(m, "final_norm.weight", H);
    w->lnfb = f32t(m, "final_norm.bias", H);
    if (!w->lnfw || !w->lnfb || qmat(m, "lm_head.weight", V, H, 8, &w->head)) FAIL("final_norm/lm_head 不对");
    int nb = 0;
    for (int i = 0; i < NL; i++) {
        char n[64];
        E2ELayer *L = &w->L[i];
        float *b;
#define T32(f, nm) (snprintf(n, sizeof n, nm ".%d", i), L->f = f32t(m, n, H))
#define T16(f, nm, cnt) (snprintf(n, sizeof n, nm ".%d", i), b = f16t(m, n, cnt), m->bias_mem[nb++] = b, L->f = b)
#define TQ(f, nm, R, C) (snprintf(n, sizeof n, nm ".%d", i), qmat(m, n, R, C, 4, &L->f))
        if (!T32(ln1w, "attention_norm.weight") || !T32(ln1b, "attention_norm.bias") ||
            !T32(ln2w, "ffn_norm.weight") || !T32(ln2b, "ffn_norm.bias") ||
            TQ(qkv, "attention.qkv_proj.weight", 3 * H, H) || !T16(qkvb, "attention.qkv_proj.bias", 3 * H) ||
            TQ(o, "attention.o_proj.weight", H, H) || !T16(ob, "attention.o_proj.bias", H) ||
            TQ(up, "mlp.up_proj.weight", FF, H) || !T16(upb, "mlp.up_proj.bias", FF) ||
            TQ(down, "mlp.down_proj.weight", H, FF) || !T16(downb, "mlp.down_proj.bias", H))
            FAIL("第 %d 层张量不对", i);
#undef T32
#undef T16
#undef TQ
    }

    for (int id = 0; id < V; id++) {
        const char *s = m->vocab.str[id];
        uint32_t cp;
        int l = utf8_dec(s, &cp);
        if (s[l] || !is_cjk(cp))
            continue;
        char rs[8][8];
        int nr = mpd_lookup(m->mpd, m->mpd_size, s, rs, 8);
        for (int k = 0; k < nr; k++)
            add_reading(m, rs[k], id);
    }
    if (m->nreadings < 300)
        FAIL("读音表太小（%d 个读音），mpd_e2e.bin 不对？", m->nreadings);

    /* 后端：cpu / vulkan / auto（先试 Vulkan，不行用 CPU） */
    if (!backend || !*backend)
        backend = getenv("MINIME_E2E_BACKEND") ? getenv("MINIME_E2E_BACKEND") : "auto";
    char e1[256] = "", e2[256] = "";
    if (!strcmp(backend, "vulkan") || !strcmp(backend, "auto"))
        m->be = e2e_vk_new(w, e1, sizeof e1);
    /* 权重已经整份上传到显存，CPU 这边只剩词表等零星访问：把模型文件映射的页还给系统
     * （只读文件映射，之后真用到哪页会从文件重新读入）。省约 70 MB 常驻内存 */
    if (m->be && getenv("MINIME_E2E_KEEP_MAP") == NULL)
        madvise((void *)m->map, m->map_size, MADV_DONTNEED);
    if (!m->be && (!strcmp(backend, "cpu") || !strcmp(backend, "auto")))
        m->be = e2e_cpu_new(w, threads, e2, sizeof e2);
    if (m->be && *e1)       /* auto 退回 CPU 时说明原因（engined 的日志里能看到） */
        fprintf(stderr, "[e2e] Vulkan 不可用（%s），改用 CPU\n", e1);
    if (!m->be)
        FAIL("没有可用的计算后端（%s%s%s）", e1, *e1 && *e2 ? "；" : "", e2);
    m->logits = malloc(sizeof(float) * MAXBEAM * V);
    if (m->be->reserve(m->be, PMAX + 64) < 0)
        FAIL("分配 KV 缓存失败");
    return m;
#undef FAIL
}

E2EModel *e2e_open(const char *model, const char *mpd, int threads, char *err, size_t errlen)
{
    return e2e_open_backend(model, mpd, NULL, threads, err, errlen);
}

const char *e2e_backend_name(const E2EModel *m)
{
    return m && m->be ? m->be->name : "";
}

void e2e_close(E2EModel *m)
{
    if (!m)
        return;
    if (m->be)
        m->be->destroy(m->be);
    for (int i = 0; i < NL * 4; i++)
        free(m->bias_mem[i]);
    for (int k = 0; k < m->nreadings; k++)
        free(m->readings[k].ids);
    free(m->readings);
    if (m->vocab.str)
        for (int i = 0; i < V; i++)
            free(m->vocab.str[i]);
    free(m->vocab.str); free(m->vocab.hkey); free(m->vocab.hval);
    free(m->logits);
    free(m->nodes);
    free(m->ctx);
    if (m->map) munmap((void *)m->map, m->map_size);
    if (m->mpd) munmap((void *)m->mpd, m->mpd_size);
    free(m);
}

/* ================= 前向（交给后端） ================= */
static int *ctx_buf(E2EModel *m, size_t n)
{
    if (n > m->ctxcap) {
        m->ctxcap = n * 2;
        m->ctx = realloc(m->ctx, sizeof(int) * m->ctxcap);
    }
    return m->ctx;
}

/* 预填提示 [from, plen)：第 t 个位置写槽位 t，能看到槽位 0..t。
 * logits_all 非空时给每个位置都算 logits（调试用，[plen-from][V]）；否则只把最后一个位置的存进 last_logits。 */
static int prefill(E2EModel *m, int from, float *logits_all)
{
    for (int t0 = from; t0 < m->plen; t0 += MAXB) {
        int B = m->plen - t0 < MAXB ? m->plen - t0 : MAXB;
        int tok[MAXB], pos[MAXB], slot[MAXB], off[MAXB], len[MAXB];
        size_t total = 0;
        for (int b = 0; b < B; b++)
            total += (size_t)(t0 + b + 1);
        int *ctx = ctx_buf(m, total);
        size_t o = 0;
        for (int b = 0; b < B; b++) {
            tok[b] = m->prompt[t0 + b];
            pos[b] = slot[b] = t0 + b;
            off[b] = (int)o;
            len[b] = t0 + b + 1;
            for (int i = 0; i <= t0 + b; i++)
                ctx[o++] = i;
        }
        int last = t0 + B == m->plen;
        E2EBatch bt = { B, tok, pos, slot, off, len, ctx,
                        logits_all ? 0 : (last ? B - 1 : B),
                        logits_all ? logits_all + (size_t)(t0 - from) * V : m->last_logits };
        if (m->be->forward(m->be, &bt) < 0)
            return -1;
    }
    return 0;
}

/* 一步：B 个新节点（tok、slot 已填好）一起前向，logits 写到 m->logits[b] */
static int step(E2EModel *m, Node **nd, int B)
{
    int tok[MAXBEAM], pos[MAXBEAM], slot[MAXBEAM], off[MAXBEAM], len[MAXBEAM];
    size_t per = (size_t)m->plen + (size_t)nd[0]->depth + 1;
    int *ctx = ctx_buf(m, per * (size_t)B);
    size_t o = 0;
    for (int b = 0; b < B; b++) {
        tok[b] = nd[b]->tok;
        pos[b] = m->plen + nd[b]->depth;
        slot[b] = nd[b]->slot;
        off[b] = (int)o;
        for (int i = 0; i < m->plen; i++)
            ctx[o++] = i;
        size_t start = o;
        for (Node *c = nd[b]; c; c = c->parent)
            ctx[o++] = c->slot;
        for (size_t i = start, j = o - 1; i < j; i++, j--) {     /* 父链是倒序的，翻成位置顺序 */
            int t = ctx[i]; ctx[i] = ctx[j]; ctx[j] = t;
        }
        len[b] = (int)(o - (size_t)off[b]);
    }
    E2EBatch bt = { B, tok, pos, slot, off, len, ctx, 0, m->logits };
    return m->be->forward(m->be, &bt);
}

int e2e_debug_logits(E2EModel *m, const int *tokens, int n, float *logits)
{
    if (n < 1 || n > PMAX)
        return -1;
    memcpy(m->prompt, tokens, sizeof(int) * (size_t)n);
    m->plen = n;
    int r = prefill(m, 0, logits);
    m->plen = 0;              /* 提示缓存作废，下次 decode 从头算 */
    return r;
}

int e2e_encode(E2EModel *m, const char *text, int *ids, int max)
{
    return encode(&m->vocab, text, ids, max);
}

const char *e2e_token(E2EModel *m, int id)
{
    return id >= 0 && id < V ? m->vocab.str[id] : "";
}

/* ================= 约束解码 ================= */
static Node *new_node(E2EModel *m, Node *parent, int tok)
{
    if (m->nnodes == m->capnodes)
        return NULL;
    Node *n = &m->nodes[m->nnodes];
    n->parent = parent;
    n->tok = tok;
    n->depth = parent ? parent->depth + 1 : 0;
    n->slot = PMAX + m->nnodes;
    m->nnodes++;
    return n;
}

typedef struct {
    float score;
    Node *node;        /* 最后一个 token（NULL = 还没生成） */
    int p;             /* 已消耗字母数 */
    int done;
    int tok;           /* 候选阶段：新 token（-1 = 结束） */
    Node *parent;
    float *lg;         /* 这条假设下一步的 logits */
} Hyp;

static int cmp_hyp(const void *a, const void *b)
{
    float x = ((const Hyp *)a)->score, y = ((const Hyp *)b)->score;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* 8 路 float exp（Cephes expf 的多项式，相对误差约 2e-7）；x 要 <= 0（这里是 logit - max） */
static inline __m256 exp8(__m256 x)
{
    x = _mm256_max_ps(x, _mm256_set1_ps(-87.0f));
    __m256 fx = _mm256_round_ps(_mm256_mul_ps(x, _mm256_set1_ps(1.44269504088896341f)), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    x = _mm256_fnmadd_ps(fx, _mm256_set1_ps(0.693359375f), x);
    x = _mm256_fnmadd_ps(fx, _mm256_set1_ps(-2.12194440e-4f), x);
    __m256 y = _mm256_set1_ps(1.9875691500E-4f);
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(1.3981999507E-3f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(8.3334519073E-3f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(4.1665795894E-2f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(1.6666665459E-1f));
    y = _mm256_fmadd_ps(y, x, _mm256_set1_ps(5.0000001201E-1f));
    y = _mm256_fmadd_ps(y, _mm256_mul_ps(x, x), _mm256_add_ps(x, _mm256_set1_ps(1.0f)));
    __m256i e = _mm256_slli_epi32(_mm256_add_epi32(_mm256_cvtps_epi32(fx), _mm256_set1_epi32(127)), 23);
    return _mm256_mul_ps(y, _mm256_castsi256_ps(e));
}

static float logsumexp(const float *lg)      /* V 是 8 的倍数 */
{
    __m256 m8 = _mm256_loadu_ps(lg);
    for (int i = 8; i < V; i += 8)
        m8 = _mm256_max_ps(m8, _mm256_loadu_ps(lg + i));
    float t[8];
    _mm256_storeu_ps(t, m8);
    float mx = t[0];
    for (int i = 1; i < 8; i++)
        if (t[i] > mx) mx = t[i];
    __m256 vm = _mm256_set1_ps(mx), s8 = _mm256_setzero_ps();
    for (int i = 0; i < V; i += 8)
        s8 = _mm256_add_ps(s8, exp8(_mm256_sub_ps(_mm256_loadu_ps(lg + i), vm)));
    _mm256_storeu_ps(t, s8);
    double s = 0;
    for (int i = 0; i < 8; i++)
        s += t[i];
    return mx + (float)log(s);
}

static void decode_text(const E2EModel *m, const Node *n, char *out, size_t cap)
{
    const Node *ch[128];
    int d = 0;
    for (; n && d < 128; n = n->parent)
        ch[d++] = n;
    size_t o = 0;
    while (d--) {
        const char *s = m->vocab.str[ch[d]->tok];
        if (!strncmp(s, "##", 2))
            s += 2;
        size_t l = strlen(s);
        if (o + l + 1 >= cap)
            break;
        memcpy(out + o, s, l);
        o += l;
    }
    out[o] = 0;
}

/* 取出字母（转小写，只留 a-z）写进 inp，拼好提示 [CLS] 上下文 [SEP] 逐个字母 [SEP] [CLS] 并预填。
 * 返回字母数；0 = 没有字母，-1 = 前向失败 */
static int setup_prompt(E2EModel *m, const char *context, const char *letters, char *inp)
{
    int L = 0;
    for (const char *p = letters; *p && L < 255; p++) {          /* inp 至少 256 字节 */
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c >= 'a' && c <= 'z') inp[L++] = c;
    }
    inp[L] = 0;
    if (!L)
        return 0;

    /* 提示：[CLS] 上下文 [SEP] 逐个字母 [SEP] [CLS] */
    int prompt[PMAX], n = 0;
    prompt[n++] = TOK_CLS;
    n += encode(&m->vocab, context ? context : "", prompt + n, PMAX / 2);
    prompt[n++] = TOK_SEP;
    for (int i = 0; i < L && n < PMAX - 64; i++) {
        char c[2] = { inp[i], 0 };
        int id = vocab_get(&m->vocab, c, 1);
        prompt[n++] = id >= 0 ? id : TOK_UNK;
    }
    prompt[n++] = TOK_SEP;
    prompt[n++] = TOK_CLS;
    /* 和上次的提示找公共前缀，只预填新的部分（最后一个位置总是重算，要它的 logits） */
    int common = 0;
    while (common < n && common < m->plen && prompt[common] == m->prompt[common])
        common++;
    if (common >= n)
        common = n - 1;
    memcpy(m->prompt, prompt, sizeof(int) * (size_t)n);
    m->plen = n;
    if (prefill(m, common, NULL) < 0) {
        m->plen = 0;
        return -1;
    }
    return L;

}

static float logsoftmax_at(const float *lg, int id)
{
    return lg[id] - logsumexp(lg);
}

/* 输入能不能整个切成完整音节（最后一个音节允许只打了前半）。n、m、ng 这类没有元音的读音不算，
 * 否则 "nh" 会被当成"嗯 + h…"。切不开时才按简拼解码，能切开的全拼输入不受简拼影响。 */
static int full_pinyin(const E2EModel *m, const char *inp, int L, int use_alt)
{
    char ok[257] = { 1 };
    for (int i = 0; i < L; i++) {
        if (!ok[i])
            continue;
        for (int k = 0; k < m->nreadings; k++) {
            const struct Reading *g = &m->readings[k];
            if (!strpbrk(g->r, "aeiouv"))
                continue;
            for (int a = -1; a < (use_alt ? g->nalt : 0); a++) {
                const char *r = a < 0 ? g->r : g->alt[a];
                int len = (int)strlen(r);
                if (len <= L - i && !memcmp(inp + i, r, (size_t)len))
                    ok[i + len] = 1;
                else if (len > L - i && !memcmp(r, inp + i, (size_t)(L - i)))
                    ok[L] = 1;
            }
        }
        if (ok[L])
            return 1;
    }
    return ok[L];
}

int e2e_is_jianpin(E2EModel *m, const char *letters)
{
    char inp[256];
    int L = 0;
    for (const char *p = letters; *p && L < 255; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c >= 'a' && c <= 'z') inp[L++] = c;
    }
    return m->jianpin && L > 0 && !full_pinyin(m, inp, L, 0);   /* 不看模糊拼写：可以和换模糊音表的解码线程并发 */
}

/* 按模糊音开关给读音 r 生成其他拼写（不含 r 本身），写进 out，返回个数 */
static int fuzzy_spellings(const char *r, uint32_t mask, char out[][8], int max)
{
    /* 声母：原样 + 模糊的另一个 */
    char ini[2][3] = { "", "" }, fin[2][8] = { "", "" };
    int ni = 1, nf = 1;
    int il = (r[1] == 'h' && (r[0] == 'z' || r[0] == 'c' || r[0] == 's')) ? 2 :
             strchr("bpmfdtnlgkhjqxrzcsyw", r[0]) ? 1 : 0;
    memcpy(ini[0], r, (size_t)il);
    ini[0][il] = 0;
    const char *f = r + il;
    snprintf(fin[0], sizeof(fin[0]), "%s", f);
    static const struct { int bit; const char *a, *b; } I[] = {
        { 9, "z", "zh" }, { 7, "c", "ch" }, { 8, "s", "sh" }, { 0, "n", "l" }, { 1, "r", "l" }, { 2, "h", "f" }, { 3, "g", "k" },
    };
    for (size_t k = 0; k < sizeof(I) / sizeof(I[0]) && ni < 2; k++)
        if (mask >> I[k].bit & 1) {
            if (!strcmp(ini[0], I[k].a)) snprintf(ini[ni++], 3, "%s", I[k].b);
            else if (!strcmp(ini[0], I[k].b)) snprintf(ini[ni++], 3, "%s", I[k].a);
        }
    /* 韵母：ian/iang、uan/uang 优先于 an/ang */
    static const struct { int bit; const char *a, *b; } F[] = {
        { 5, "ian", "iang" }, { 6, "uan", "uang" }, { 4, "an", "ang" }, { 11, "en", "eng" }, { 12, "in", "ing" },
    };
    size_t fl = strlen(f);
    for (size_t k = 0; k < sizeof(F) / sizeof(F[0]) && nf < 2; k++) {
        size_t la = strlen(F[k].a), lb = strlen(F[k].b);
        int ends_b = fl >= lb && !strcmp(f + fl - lb, F[k].b);
        int ends_a = !ends_b && fl >= la && !strcmp(f + fl - la, F[k].a);
        if (!(ends_a || ends_b))
            continue;
        if (k >= 2 && ((ends_a && fl > la && (f[fl - la - 1] == 'i' || f[fl - la - 1] == 'u')) ||
                       (ends_b && fl > lb && (f[fl - lb - 1] == 'i' || f[fl - lb - 1] == 'u'))) &&
            (F[k].a[0] == 'a'))
            continue;                                      /* ian/uan 归上面两条管 */
        if (!(mask >> F[k].bit & 1))
            break;
        size_t keep = fl - (ends_b ? lb : la);
        snprintf(fin[nf++], 8, "%.*s%s", (int)keep, f, ends_b ? F[k].a : F[k].b);
    }
    int n = 0;
    for (int a = 0; a < ni; a++)
        for (int b = 0; b < nf; b++) {
            if (!a && !b)
                continue;
            if (n < max && strlen(ini[a]) + strlen(fin[b]) < 8)
                snprintf(out[n++], 8, "%s%s", ini[a], fin[b]);
        }
    /* 整个音节：hui/fei、huang/wang */
    static const struct { int bit; const char *a, *b; } W[] = { { 10, "hui", "fei" }, { 14, "huang", "wang" } };
    for (size_t k = 0; k < 2; k++)
        if (mask >> W[k].bit & 1 && n < max) {
            if (!strcmp(r, W[k].a)) snprintf(out[n++], 8, "%s", W[k].b);
            else if (!strcmp(r, W[k].b)) snprintf(out[n++], 8, "%s", W[k].a);
        }
    return n;
}

void e2e_set_fuzzy(E2EModel *m, uint32_t mask)
{
    for (int k = 0; k < m->nreadings; k++)
        m->readings[k].nalt = mask ? fuzzy_spellings(m->readings[k].r, mask, m->readings[k].alt, 8) : 0;
}

void e2e_set_jianpin(E2EModel *m, int on, float penalty)
{
    m->jianpin = on;
    m->jp_penalty = penalty;
}

int e2e_score(E2EModel *m, const char *context, const char *letters, const char *text, float *score)
{
    char inp[256];
    if (setup_prompt(m, context, letters, inp) <= 0)
        return -1;
    int tok[256];
    int n = encode(&m->vocab, text, tok, 255);
    if (n <= 0)
        return -1;
    if (m->be->reserve(m->be, PMAX + n) < 0)
        return -1;
    double s = logsoftmax_at(m->last_logits, tok[0]);
    float *lg = malloc(sizeof(float) * MAXB * V);
    int pos[MAXB], slot[MAXB], off[MAXB], len[MAXB];
    for (int t0 = 0; t0 < n; t0 += MAXB) {          /* 整句一起前向（每段至多 MAXB 个），第 i 个看到提示和前 i 个字 */
        int B = n - t0 < MAXB ? n - t0 : MAXB;
        size_t total = 0;
        for (int b = 0; b < B; b++)
            total += (size_t)m->plen + (size_t)(t0 + b) + 1;
        int *ctx = ctx_buf(m, total);
        size_t o = 0;
        for (int b = 0; b < B; b++) {
            pos[b] = m->plen + t0 + b;
            slot[b] = PMAX + t0 + b;
            off[b] = (int)o;
            for (int i = 0; i < m->plen; i++)
                ctx[o++] = i;
            for (int i = 0; i <= t0 + b; i++)
                ctx[o++] = PMAX + i;
            len[b] = (int)(o - (size_t)off[b]);
        }
        E2EBatch bt = { B, tok + t0, pos, slot, off, len, ctx, 0, lg };
        if (m->be->forward(m->be, &bt) < 0) {
            free(lg);
            return -1;
        }
        for (int b = 0; b < B; b++)
            s += logsoftmax_at(lg + (size_t)b * V, t0 + b + 1 < n ? tok[t0 + b + 1] : TOK_SEP);
    }
    free(lg);
    *score = (float)s;
    return 0;
}

int e2e_decode(E2EModel *m, const char *context, const char *letters, int beam,
               int time_limit_ms, const volatile int *cancel, E2EResult *out, int max_out)
{
    double t0 = now_ms();
    if (beam < 1) beam = 1;
    if (beam > MAXBEAM) beam = MAXBEAM;
    char inp[256];
    int L = setup_prompt(m, context, letters, inp);
    if (L <= 0)
        return 0;

    /* 节点池：最多 (L+1) 步 × beam 个；节点的 K/V 用 PMAX 之后的槽位 */
    int need = (L + 2) * beam;
    if (m->capnodes < need) {
        if (m->be->reserve(m->be, PMAX + need) < 0)
            return 0;
        free(m->nodes);
        m->nodes = calloc((size_t)need, sizeof(Node));
        m->capnodes = need;
    }
    m->nnodes = 0;

    Hyp beams[MAXBEAM];
    int nb = 1;
    beams[0] = (Hyp){ 0.0f, NULL, 0, 0, 0, NULL, m->last_logits };
    int jp = m->jianpin && !full_pinyin(m, inp, L, 1);            /* 这次按简拼解码 */
    int cap = beam * (jp ? 400 : 64) * beam + 16;    /* 简拼时一个字母能匹配几十个读音 */
    Hyp *cand = malloc(sizeof(Hyp) * (size_t)cap);
    int result = 0;

    for (int stepi = 0; stepi <= L; stepi++) {
        if (cancel && *cancel) {
            free(cand);
            return -1;
        }
        if (time_limit_ms > 0 && now_ms() - t0 > time_limit_ms)
            break;
        int nc = 0;
        for (int b = 0; b < nb; b++) {
            Hyp *h = &beams[b];
            if (h->done) {
                if (nc < cap) cand[nc++] = *h;
                continue;
            }
            float lse = logsumexp(h->lg);
            if (h->p == L) {
                if (nc < cap) cand[nc++] = (Hyp){ h->score + h->lg[TOK_SEP] - lse, h->node, h->p, 1, -1, h->node, NULL };
                continue;
            }
            const char *rest = inp + h->p;
            int rl = L - h->p;
            for (int k = 0; k < m->nreadings; k++) {
                const struct Reading *g = &m->readings[k];
                /* 这个读音能消耗几个字母：全拼；输入末尾的半个音节；简拼时还可以只用声母
                 * （首字母，或 zh/ch/sh 两个字母，要比读音短、比剩下的字母短——等于剩下的已算在"半个音节"里） */
                /* 模糊音时读音的每个拼写都试（-1 = 原拼写） */
                int opt[24], pen[24], no = 0;
                for (int a = -1; a < g->nalt && no < 21; a++) {
                    const char *r = a < 0 ? g->r : g->alt[a];
                    int len = (int)strlen(r), c = -1;
                    if (len <= rl && !memcmp(rest, r, (size_t)len))
                        c = len;
                    else if (len > rl && !memcmp(r, rest, (size_t)rl))
                        c = rl;
                    if (c > 0) {
                        int dup = 0;
                        for (int q = 0; q < no; q++) dup |= opt[q] == c && !pen[q];
                        if (!dup) opt[no] = c, pen[no++] = 0;
                    }
                    if (jp && rest[0] == r[0]) {
                        int zcs = r[1] == 'h' && (r[0] == 'z' || r[0] == 'c' || r[0] == 's');
                        for (int kk = 1; kk <= 2; kk++)
                            if (kk < len && kk < rl && (kk == 1 || (zcs && rest[1] == 'h'))) {
                                int dup = 0;
                                for (int q = 0; q < no; q++) dup |= opt[q] == kk;
                                if (!dup) opt[no] = kk, pen[no++] = 1;
                            }
                    }
                }
                if (!no)
                    continue;
                /* 组内取前 beam 个 */
                int top[MAXBEAM], nt = 0;
                for (int i = 0; i < g->n; i++) {
                    int id = g->ids[i];
                    float v = h->lg[id];
                    if (nt < beam) {
                        int j = nt++;
                        while (j > 0 && h->lg[top[j - 1]] < v) { top[j] = top[j - 1]; j--; }
                        top[j] = id;
                    } else if (v > h->lg[top[nt - 1]]) {
                        int j = nt - 1;
                        while (j > 0 && h->lg[top[j - 1]] < v) { top[j] = top[j - 1]; j--; }
                        top[j] = id;
                    }
                }
                for (int o = 0; o < no; o++)
                    for (int i = 0; i < nt && nc < cap; i++)
                        cand[nc++] = (Hyp){ h->score + h->lg[top[i]] - lse - (pen[o] ? m->jp_penalty : 0.0f), NULL,
                                            h->p + opt[o], 0, top[i], h->node, NULL };
            }
        }
        if (!nc)
            break;
        qsort(cand, (size_t)nc, sizeof(Hyp), cmp_hyp);
        /* 去重（同一父节点 + 同一 token + 消耗的字母数相同）后取前 beam 个 */
        Hyp nbm[MAXBEAM];
        int nn = 0;
        for (int i = 0; i < nc && nn < beam; i++) {
            int dup = 0;
            for (int j = 0; j < nn; j++)
                if (nbm[j].parent == cand[i].parent && nbm[j].tok == cand[i].tok && nbm[j].done == cand[i].done &&
                    nbm[j].p == cand[i].p)
                    dup = 1;
            if (!dup)
                nbm[nn++] = cand[i];
        }
        /* 新节点一起前向 */
        Node *nds[MAXBEAM];
        int nnew = 0;
        for (int i = 0; i < nn; i++) {
            if (nbm[i].done) {
                nbm[i].lg = NULL;
                continue;
            }
            Node *nd = new_node(m, nbm[i].parent, nbm[i].tok);
            if (!nd) { nbm[i].done = 1; nbm[i].score = -1e30f; continue; }
            nbm[i].node = nd;
            nds[nnew++] = nd;
        }
        if (nnew) {
            if (step(m, nds, nnew) < 0) {
                free(cand);
                return 0;
            }
            int k = 0;
            for (int i = 0; i < nn; i++)
                if (!nbm[i].done)
                    nbm[i].lg = m->logits + (size_t)(k++) * V;
        }
        memcpy(beams, nbm, sizeof(Hyp) * (size_t)nn);
        nb = nn;
        int all = 1;
        for (int i = 0; i < nb; i++)
            if (!beams[i].done)
                all = 0;
        if (all)
            break;
    }
    free(cand);
    for (int i = 0; i < nb && result < max_out; i++) {
        if (!beams[i].done || beams[i].score < -1e29f)
            continue;
        decode_text(m, beams[i].node, out[result].text, sizeof(out[result].text));
        out[result].score = beams[i].score;
        result++;
    }
    return result;
}
