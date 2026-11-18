/* e2e.h — 微信输入法 e2e 整句模型（Mac 包的 decgnr.bin）的 C 推理和拼音约束解码。
 *
 * 模型格式、输入格式、权重排列见 docs/whole-sentence.md；NumPy 参考实现是 scripts/e2e_ref.py，
 * 这里的结果应和它的 ConstrainedDecoder 一致。
 *
 * 需要 x86-64 的 AVX2、FMA、F16C；不支持时 e2e_open 返回 NULL。
 * 一个 E2EModel 同一时间只能被一个线程调用（内部有自己的线程池）。
 */
#ifndef MINIME_E2E_H
#define MINIME_E2E_H

#include <stddef.h>
#include <stdint.h>

typedef struct E2EModel E2EModel;

typedef struct {
    char  text[256];   /* UTF-8 整句 */
    float score;       /* 对数概率之和（含结束标记） */
} E2EResult;

/* 打开模型。model：decgnr.bin；mpd：mpd_e2e.bin（单字读音）；threads<=0 时自动取 CPU 数（最多 8）。
 * 失败返回 NULL，原因写进 err。 */
E2EModel *e2e_open(const char *model, const char *mpd, int threads, char *err, size_t errlen);

/* 约束解码：context 是光标前文本（可为空串），letters 是输入的拼音字母（只取 a-z）。
 * 结果按分数从高到低写进 out，返回条数（0 = 解析不出来）。
 * time_limit_ms > 0 时超时返回已结束的假设；cancel 非空且变成非 0 时尽快返回 -1。 */
int e2e_decode(E2EModel *m, const char *context, const char *letters, int beam,
               int time_limit_ms, const volatile int *cancel, E2EResult *out, int max_out);

/* 给一句话打分：在同样的上下文和字母下，模型生成 text（再接结束标记）的对数概率之和，
 * 和 e2e_decode 的 score 可以直接比较（不检查 text 和拼音是否一致）。成功返回 0。 */
int e2e_score(E2EModel *m, const char *context, const char *letters, const char *text, float *score);

/* 简拼：on 非 0 时，输入切不成完整音节的（如 "nh"、"wxsrf"、"nihaosj"），一个字也可以只用声母
 * （首字母，或 zh/ch/sh）匹配，每个缩写的音节扣 penalty 分；能切成完整音节的照常按全拼解码。默认关。 */
void e2e_set_jianpin(E2EModel *m, int on, float penalty);
/* 模糊音：mask 的位和 SessionConfig 的模糊音开关一致（0 n/l、1 r/l、2 h/f、3 g/k、4 an/ang、5 ian/iang、
 * 6 uan/uang、7 c/ch、8 s/sh、9 z/zh、10 hui/fei、11 en/eng、12 in/ing、14 huang/wang）。打开后读音也接受模糊的拼写。 */
void e2e_set_fuzzy(E2EModel *m, uint32_t mask);
/* 这串字母会不会按简拼解码（打开了简拼、并且切不成完整音节）。只读读音表，可以和解码并发调用。 */
int e2e_is_jianpin(E2EModel *m, const char *letters);
/* 仅限持有模糊拼写表同一线程调用：把模糊音扩展也算进全拼切分判断（和 e2e_decode 保持一致） */
int e2e_is_jianpin_fuzzy(E2EModel *m, const char *letters);

void e2e_close(E2EModel *m);

/* 指定后端打开："cpu"、"vulkan"、"auto"（先试 Vulkan 再 CPU）；NULL 或空串时读环境变量 MINIME_E2E_BACKEND，默认 auto */
E2EModel *e2e_open_backend(const char *model, const char *mpd, const char *backend, int threads,
                           char *err, size_t errlen);
const char *e2e_backend_name(const E2EModel *m);

/* 测试用：把 tokens 当成从位置 0 开始的整段，算出每个位置的 logits（[n][10496]）。会作废提示缓存。 */
int e2e_debug_logits(E2EModel *m, const int *tokens, int n, float *logits);
int e2e_encode(E2EModel *m, const char *text, int *ids, int max);   /* 和模型同样的切词 */
const char *e2e_token(E2EModel *m, int id);

#endif
