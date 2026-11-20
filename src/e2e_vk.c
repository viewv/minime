/* e2e_vk.c — e2e 模型的 Vulkan 后端。接口见 e2e_internal.h，着色器在 src/shaders/。
 *
 * - libvulkan.so.1 用 dlopen 装载：没有 Vulkan 的机器上返回 NULL，调用方退回 CPU 后端。
 * - 整个模型文件原样传到显存的一个缓冲（W），着色器按偏移自己解 int4/int8/fp16。
 * - KV 池在显存（不够时换更大的缓冲并拷贝旧内容）；激活值、logits 和每批的整数输入放在主机可见内存。
 * - 一次前向（12 层，约 100 次派发）录进一个命令缓冲，提交一次、等一次。
 */
#define _GNU_SOURCE
#ifdef MINIME_NO_VULKAN
#include <stdio.h>

#include "e2e_internal.h"

E2EBackend *e2e_vk_new(const E2EWeights *w, char *err, size_t errlen)
{
    (void)w;
    if (err && errlen)
        snprintf(err, errlen, "编译时未启用 Vulkan");
    return NULL;
}
#else
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#include "e2e_internal.h"

enum { H = E2E_H, FF = E2E_FF, V = E2E_V, NL = E2E_NL, PMAX = E2E_PMAX };
enum { MAXB = 64, MAXCTX_TOTAL = 64 * 1024 };
#define SLOT_BYTES ((VkDeviceSize)NL * 2 * H * 4)

/* 激活区布局（按 float） */
enum {
    A_X = 0,
    A_HN = A_X + MAXB * H,
    A_QKV = A_HN + MAXB * H,
    A_ATT = A_QKV + MAXB * 3 * H,
    A_UP = A_ATT + MAXB * H,
    A_TOTAL = A_UP + MAXB * FF,
};
enum { IN_TOK = 0, IN_POS = 64, IN_SLOT = 128, IN_OFF = 192, IN_LEN = 256, IN_CTX = 320 };

/* 编译好的着色器（glslc -mfmt=c 生成） */
static const uint32_t spv_embed[] =
#include "embed.spv.h"
    ;
static const uint32_t spv_layernorm[] =
#include "layernorm.spv.h"
    ;
static const uint32_t spv_mq4[] =
#include "matmul_q4.spv.h"
    ;
static const uint32_t spv_mq8[] =
#include "matmul_q8.spv.h"
    ;
static const uint32_t spv_mq4_sg[] =
#include "matmul_q4_sg.spv.h"
    ;
static const uint32_t spv_mq8_sg[] =
#include "matmul_q8_sg.spv.h"
    ;
static const uint32_t spv_attn[] =
#include "attn.spv.h"
    ;

enum { P_EMBED, P_LN, P_MQ4, P_MQ8, P_ATTN, P_COUNT };
/* 矩阵乘按批大小特化：一个工作组算 NB 个 token（1、4、8） */
enum { NVAR = 3 };
static const uint32_t var_nb[NVAR] = { 1, 4, 8 };
static int var_of(int B) { return B <= 1 ? 0 : B <= 4 ? 1 : 2; }

#define VKFN(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateComputePipelines) \
    X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) \
    X(vkResetFences) X(vkDeviceWaitIdle) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) \
    X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
    void *map;
    int coherent;
} Buf;

typedef struct { const uint16_t *qkvb, *ob, *upb, *downb; } Bias16;

typedef struct {
    E2EBackend base;
    const E2EWeights *w;
    const uint8_t *wbase;          /* 模型文件起点（算偏移用） */
    void *lib;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkCreateInstance vkCreateInstance;
#define DECL(f) PFN_##f f;
    VKFN(DECL)
#undef DECL
    VkInstance inst;
    VkPhysicalDevice pd;
    VkPhysicalDeviceMemoryProperties memp;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    Buf W, act, kv, in, lg;      /* lg：logits，放主机可缓存内存（核显上设备本地的主机可见内存对 CPU 不缓存，读很慢） */
    int cap;                       /* KV 槽位数 */
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline pipe[P_COUNT];
    VkPipeline mq4[NVAR], mq8[NVAR];
    uint32_t cl, max_sg;           /* 矩阵乘：每个 8 行块一个工作组（32*cl 线程），max_sg 为 CL 或共享内存子组槽数 */
    int use_cluster32;             /* 1: 用 32 宽簇内归约（AMD/NV）；0: 用通用 subgroupAdd（Intel 等可变 SIMD 宽度） */
    VkDescriptorPool dpool;
    VkDescriptorSet ds;
    VkCommandPool cpool;
    VkCommandBuffer cb;
    VkFence fence;
    char name[160];
    /* 性能剖析（MINIME_E2E_VK_PROF=1）：每次派发后打时间戳，按着色器累计 GPU 时间 */
    int prof;
    unsigned skip;                 /* 测速实验：跳过这些着色器（位掩码，按 P_* 编号；结果会是错的） */
    VkQueryPool qp;
    float ts_period;               /* 纳秒/刻度 */
    int nts, ts_kind[512];
    double gpu_ns[8], host_ms, wait_ms, rec_ms, submit_ms;
    long ncalls, ndisp[8];
    Bias16 bias16[NL];
} Vk;

/* ---------- 内存 ---------- */
static int find_mem(Vk *v, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer)
{
    for (uint32_t i = 0; i < v->memp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (v->memp.memoryTypes[i].propertyFlags & (want | prefer)) == (want | prefer))
            return (int)i;
    for (uint32_t i = 0; i < v->memp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (v->memp.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

enum { MEM_DEVICE, MEM_UPLOAD, MEM_READBACK };
static void buf_free(Vk *v, Buf *b);
static int buf_new(Vk *v, Buf *b, VkDeviceSize size, VkBufferUsageFlags usage, int host)
{
    memset(b, 0, sizeof(*b));
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (v->vkCreateBuffer(v->dev, &bi, NULL, &b->buf) != VK_SUCCESS)
        return -1;
    VkMemoryRequirements mr;
    v->vkGetBufferMemoryRequirements(v->dev, b->buf, &mr);
    /* 上传用：CPU 只写，优先设备本地；回读用：CPU 要读，优先可缓存（非 coherent 时在读回前 invalidate） */
    int mt;
    if (host == MEM_UPLOAD) {
        mt = find_mem(v, mr.memoryTypeBits,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    } else if (host == MEM_READBACK) {
        mt = find_mem(v, mr.memoryTypeBits,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt < 0)
            mt = find_mem(v, mr.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    } else {
        mt = find_mem(v, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    }
    if (mt < 0) {
        buf_free(v, b);
        return -1;
    }
    b->coherent = (v->memp.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = (uint32_t)mt };
    if (v->vkAllocateMemory(v->dev, &ai, NULL, &b->mem) != VK_SUCCESS) {
        buf_free(v, b);
        return -1;
    }
    v->vkBindBufferMemory(v->dev, b->buf, b->mem, 0);
    b->size = size;
    if (host && v->vkMapMemory(v->dev, b->mem, 0, size, 0, &b->map) != VK_SUCCESS) {
        buf_free(v, b);
        return -1;
    }
    return 0;
}

static void buf_free(Vk *v, Buf *b)
{
    if (b->map) v->vkUnmapMemory(v->dev, b->mem);
    if (b->buf) v->vkDestroyBuffer(v->dev, b->buf, NULL);
    if (b->mem) v->vkFreeMemory(v->dev, b->mem, NULL);
    memset(b, 0, sizeof(*b));
}

/* 录一段命令并同步执行（上传、拷贝用） */
static double mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int run_once(Vk *v, void (*rec)(Vk *, void *), void *arg)
{
    double t0 = mono_ms();
    v->vkResetCommandBuffer(v->cb, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    v->vkBeginCommandBuffer(v->cb, &bi);
    rec(v, arg);
    v->vkEndCommandBuffer(v->cb);
    double t1 = mono_ms();
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &v->cb };
    v->vkResetFences(v->dev, 1, &v->fence);
    if (v->vkQueueSubmit(v->queue, 1, &si, v->fence) != VK_SUCCESS)
        return -1;
    double t2 = mono_ms();
    int r = v->vkWaitForFences(v->dev, 1, &v->fence, VK_TRUE, 10ull * 1000 * 1000 * 1000) == VK_SUCCESS ? 0 : -1;
    v->rec_ms += t1 - t0;
    v->submit_ms += t2 - t1;
    v->wait_ms += mono_ms() - t1;
    return r;
}

static void rec_nothing(Vk *v, void *a) { (void)v; (void)a; }
static void dispatch(Vk *v, int p, const uint32_t *pc, uint32_t npc, uint32_t gx, uint32_t gy);
static void rec_ndisp(Vk *v, void *a)     /* 实验：N 次空派发（embed，n=0 立即返回），每次后面一个屏障 */
{
    int n = *(int *)a;
    v->vkCmdBindDescriptorSets(v->cb, VK_PIPELINE_BIND_POINT_COMPUTE, v->pl, 0, 1, &v->ds, 0, NULL);
    uint32_t pc[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++)
        dispatch(v, 0, pc, 4, 1, 1);
}

typedef struct { VkBuffer src, dst; VkDeviceSize size; } CopyArg;
static void rec_copy(Vk *v, void *a)
{
    CopyArg *c = a;
    VkBufferCopy r = { 0, 0, c->size };
    v->vkCmdCopyBuffer(v->cb, c->src, c->dst, 1, &r);
}

static void update_ds(Vk *v)
{
    VkDescriptorBufferInfo bi[5] = { { v->W.buf, 0, VK_WHOLE_SIZE }, { v->act.buf, 0, VK_WHOLE_SIZE },
                                     { v->kv.buf, 0, VK_WHOLE_SIZE }, { v->in.buf, 0, VK_WHOLE_SIZE },
                                     { v->lg.buf, 0, VK_WHOLE_SIZE } };
    VkWriteDescriptorSet wr[5];
    for (int i = 0; i < 5; i++)
        wr[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = v->ds,
                                        .dstBinding = (uint32_t)i, .descriptorCount = 1,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[i] };
    v->vkUpdateDescriptorSets(v->dev, 5, wr, 0, NULL);
}

/* ---------- 后端接口 ---------- */
static int vk_reserve(E2EBackend *b, int nslots)
{
    Vk *v = (Vk *)b;
    if (nslots <= v->cap)
        return 0;
    int ncap = nslots > v->cap * 2 ? nslots : v->cap * 2;
    Buf nb;
    if (buf_new(v, &nb, SLOT_BYTES * (VkDeviceSize)ncap,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MEM_DEVICE) < 0)
        return -1;
    if (v->cap) {
        CopyArg c = { v->kv.buf, nb.buf, SLOT_BYTES * (VkDeviceSize)v->cap };
        if (run_once(v, rec_copy, &c) < 0) {
            buf_free(v, &nb);
            return -1;
        }
        buf_free(v, &v->kv);
    }
    v->kv = nb;
    v->cap = ncap;
    update_ds(v);
    return 0;
}

static uint32_t woff(Vk *v, const void *p) { return (uint32_t)(((const uint8_t *)p - v->wbase) / 4); }  /* uint 下标 */
static uint32_t hoff(Vk *v, const void *p) { return (uint32_t)(((const uint8_t *)p - v->wbase) / 2); }  /* fp16 下标 */

static void barrier(Vk *v)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    v->vkCmdPipelineBarrier(v->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            0, 1, &mb, 0, NULL, 0, NULL);
}

static void dispatch_pipe(Vk *v, int p, VkPipeline pipe, const uint32_t *pc, uint32_t npc, uint32_t gx, uint32_t gy)
{
    if (v->skip >> p & 1)
        return;
    v->vkCmdBindPipeline(v->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    v->vkCmdPushConstants(v->cb, v->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, npc * 4, pc);
    v->vkCmdDispatch(v->cb, gx, gy, 1);
    if (v->prof && v->nts < 511) {
        v->ts_kind[v->nts] = p;
        v->vkCmdWriteTimestamp(v->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, v->qp, (uint32_t)++v->nts);
    }
    barrier(v);
}

static void dispatch(Vk *v, int p, const uint32_t *pc, uint32_t npc, uint32_t gx, uint32_t gy)
{
    dispatch_pipe(v, p, v->pipe[p], pc, npc, gx, gy);
}

/* mode 见 matmul_q4.comp；mode 3 时 l 是写 KV 的层号 */
static void mm_q4(Vk *v, const E2EQMat *m, const uint16_t *bias16, uint32_t xoff, uint32_t yoff, int B, uint32_t mode, int l)
{
    uint32_t pc[10] = { (uint32_t)m->R, (uint32_t)m->C, woff(v, m->q), woff(v, m->s),
                        bias16 ? hoff(v, bias16) : 0xffffffffu, xoff, yoff, (uint32_t)B, mode, (uint32_t)l };
    int k = var_of(B);
    dispatch_pipe(v, P_MQ4, v->mq4[k], pc, 10, (uint32_t)(m->R / 8), ((uint32_t)B + var_nb[k] - 1) / var_nb[k]);
}

typedef struct { const E2EBatch *bt; } FwdArg;

static void rec_forward(Vk *v, void *a)
{
    const E2EBatch *bt = ((FwdArg *)a)->bt;
    const E2EWeights *w = v->w;
    const int B = bt->n;
    v->vkCmdBindDescriptorSets(v->cb, VK_PIPELINE_BIND_POINT_COMPUTE, v->pl, 0, 1, &v->ds, 0, NULL);
    if (v->prof) {
        v->vkCmdResetQueryPool(v->cb, v->qp, 0, 512);
        v->vkCmdWriteTimestamp(v->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, v->qp, 0);
        v->nts = 0;
    }
    uint32_t pe[4] = { (uint32_t)B, hoff(v, w->wte), hoff(v, w->wpe), A_X };
    dispatch(v, P_EMBED, pe, 4, ((uint32_t)B * H + 255) / 256, 1);
    for (int l = 0; l < NL; l++) {
        const E2ELayer *L = &w->L[l];
        uint32_t ln1[5] = { A_X, 0, A_HN, woff(v, L->ln1w), woff(v, L->ln1b) };
        dispatch(v, P_LN, ln1, 5, (uint32_t)B, 1);
        mm_q4(v, &L->qkv, v->bias16[l].qkvb, A_HN, A_QKV, B, 3, l);          /* mode 3：顺带把 K、V 写进槽位 */
        uint32_t at[3] = { (uint32_t)l, A_QKV, A_ATT };
        dispatch(v, P_ATTN, at, 3, (uint32_t)B, 12);
        mm_q4(v, &L->o, v->bias16[l].ob, A_ATT, A_X, B, 1, 0);
        uint32_t ln2[5] = { A_X, 0, A_HN, woff(v, L->ln2w), woff(v, L->ln2b) };
        dispatch(v, P_LN, ln2, 5, (uint32_t)B, 1);
        mm_q4(v, &L->up, v->bias16[l].upb, A_HN, A_UP, B, 2, 0);
        mm_q4(v, &L->down, v->bias16[l].downb, A_UP, A_X, B, 1, 0);
    }
    int nl = B - bt->logits_from;
    if (nl > 0) {
        uint32_t lf[5] = { A_X, (uint32_t)bt->logits_from, A_HN, woff(v, w->lnfw), woff(v, w->lnfb) };
        dispatch(v, P_LN, lf, 5, (uint32_t)nl, 1);
        uint32_t hq[7] = { V, H, woff(v, w->head.q), woff(v, w->head.s), A_HN, 0, (uint32_t)nl };
        int k = var_of(nl);
        dispatch_pipe(v, P_MQ8, v->mq8[k], hq, 7, V / 8, ((uint32_t)nl + var_nb[k] - 1) / var_nb[k]);
    }
}

static int vk_forward(E2EBackend *b, const E2EBatch *bt)
{
    Vk *v = (Vk *)b;
    const int B = bt->n;
    if (B < 1 || B > MAXB)
        return -1;
    int32_t *in = v->in.map;
    size_t total = 0;
    for (int i = 0; i < B; i++) {
        if ((unsigned)bt->tok[i] >= V || (unsigned)bt->pos[i] >= PMAX ||
            bt->slot[i] < 0 || bt->slot[i] >= v->cap ||
            bt->ctx_len[i] < 0 || bt->ctx_len[i] > 1024)
            return -1;
        if (total + (size_t)bt->ctx_len[i] > MAXCTX_TOTAL)
            return -1;
        const int *src_ctx = bt->ctx + bt->ctx_off[i];
        for (int j = 0; j < bt->ctx_len[i]; j++)
            if (src_ctx[j] < 0 || src_ctx[j] >= v->cap)
                return -1;
        in[IN_TOK + i] = bt->tok[i];
        in[IN_POS + i] = bt->pos[i];
        in[IN_SLOT + i] = bt->slot[i];
        in[IN_OFF + i] = (int32_t)total;
        in[IN_LEN + i] = bt->ctx_len[i];
        memcpy(in + IN_CTX + total, src_ctx, sizeof(int) * (size_t)bt->ctx_len[i]);
        total += (size_t)bt->ctx_len[i];
    }
    FwdArg a = { bt };
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (run_once(v, rec_forward, &a) < 0)
        return -1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (v->prof) {
        v->host_ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        v->ncalls++;
        uint64_t ts[512];
        if (v->vkGetQueryPoolResults(v->dev, v->qp, 0, (uint32_t)v->nts + 1, sizeof ts, ts, 8,
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
            for (int i = 0; i < v->nts; i++) {
                v->gpu_ns[v->ts_kind[i]] += (double)(ts[i + 1] - ts[i]) * v->ts_period;
                v->ndisp[v->ts_kind[i]]++;
            }
    }
    int nl = B - bt->logits_from;
    if (nl > 0) {
        if (!v->lg.coherent) {
            VkMappedMemoryRange mr = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                       .memory = v->lg.mem, .offset = 0, .size = VK_WHOLE_SIZE };
            if (v->vkInvalidateMappedMemoryRanges(v->dev, 1, &mr) != VK_SUCCESS)
                return -1;
        }
        memcpy(bt->logits, v->lg.map, sizeof(float) * (size_t)nl * V);
    }
    return 0;
}

static void vk_destroy(E2EBackend *b)
{
    Vk *v = (Vk *)b;
    if (v->prof && v->ncalls) {
        static const char *nm[] = { "embed", "layernorm", "matmul_q4", "matmul_q8", "attn" };
        double tot = 0;
        for (int i = 0; i < P_COUNT; i++)
            tot += v->gpu_ns[i];
        fprintf(stderr, "[vk] %ld 次前向：主机端平均 %.2f ms/次（录命令 %.2f，提交到完成 %.2f，其中 vkQueueSubmit %.2f），GPU 合计 %.2f ms/次\n",
                v->ncalls, v->host_ms / v->ncalls, v->rec_ms / v->ncalls, v->wait_ms / v->ncalls, v->submit_ms / v->ncalls,
                tot / 1e6 / v->ncalls);
        for (int i = 0; i < P_COUNT; i++)
            fprintf(stderr, "[vk]   %-10s %6.3f ms/次（%5.1f%%），%ld 次派发，平均 %.1f us\n", nm[i],
                    v->gpu_ns[i] / 1e6 / v->ncalls, 100 * v->gpu_ns[i] / (tot + 1), v->ndisp[i],
                    v->ndisp[i] ? v->gpu_ns[i] / 1e3 / v->ndisp[i] : 0);
    }
    if (v->dev) {
        v->vkDeviceWaitIdle(v->dev);
        if (v->qp) v->vkDestroyQueryPool(v->dev, v->qp, NULL);
        for (int i = 0; i < P_COUNT; i++)
            if (v->pipe[i]) v->vkDestroyPipeline(v->dev, v->pipe[i], NULL);
        for (int i = 0; i < NVAR; i++) {
            if (v->mq4[i]) v->vkDestroyPipeline(v->dev, v->mq4[i], NULL);
            if (v->mq8[i]) v->vkDestroyPipeline(v->dev, v->mq8[i], NULL);
        }
        if (v->pl) v->vkDestroyPipelineLayout(v->dev, v->pl, NULL);
        if (v->dpool) v->vkDestroyDescriptorPool(v->dev, v->dpool, NULL);
        if (v->dsl) v->vkDestroyDescriptorSetLayout(v->dev, v->dsl, NULL);
        if (v->fence) v->vkDestroyFence(v->dev, v->fence, NULL);
        if (v->cpool) v->vkDestroyCommandPool(v->dev, v->cpool, NULL);
        buf_free(v, &v->W); buf_free(v, &v->act); buf_free(v, &v->kv); buf_free(v, &v->in); buf_free(v, &v->lg);
        v->vkDestroyDevice(v->dev, NULL);
    }
    if (v->inst && v->vkDestroyInstance) v->vkDestroyInstance(v->inst, NULL);
    if (v->lib) dlclose(v->lib);
    free(v);
}

/* 偏置张量：E2EWeights 里是 fp32 副本，这里在模型文件里按名字找回原始 fp16 位置 */
static const uint16_t *find_f16(const uint8_t *d, const char *name)
{
    size_t i = 0x36ad7;
    uint64_t n;
    memcpy(&n, d + i, 8);
    i += 8;
    for (uint64_t k = 0; k < n; k++) {
        uint64_t nl, sz;
        memcpy(&nl, d + i, 8);
        const char *nm = (const char *)d + i + 8;
        size_t j = i + 8 + nl;
        memcpy(&sz, d + j, 8);
        size_t a = (j + 8 + 4095) / 4096 * 4096;
        if (nl == strlen(name) && !memcmp(nm, name, nl))
            return (const uint16_t *)(d + a);
        i = a + sz;
    }
    return NULL;
}

E2EBackend *e2e_vk_new(const E2EWeights *w, char *err, size_t errlen)
{
#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); vk_destroy(&v->base); return NULL; } while (0)
    Vk *v = calloc(1, sizeof(*v));
    v->base.name = v->name;
    v->base.reserve = vk_reserve;
    v->base.forward = vk_forward;
    v->base.destroy = vk_destroy;
    v->w = w;
    v->skip = getenv("MINIME_E2E_VK_SKIP") ? (unsigned)strtoul(getenv("MINIME_E2E_VK_SKIP"), NULL, 0) : 0;
    v->cl = getenv("MINIME_E2E_VK_CL") ? (uint32_t)atoi(getenv("MINIME_E2E_VK_CL")) : 2;
    if (v->cl < 1 || v->cl > 4) v->cl = 2;
    /* 模型文件起点：wte 所在页往前找 "XLLM" 魔数（张量都在同一个 mmap 里） */
    const uint8_t *p = (const uint8_t *)((uintptr_t)w->wte & ~(uintptr_t)4095);
    while (memcmp(p, "XLLM", 4))
        p -= 4096;
    v->wbase = p;
    size_t fsize = (size_t)((const uint8_t *)w->head.s + (size_t)V * H / 32 * 2 - p);  /* lm_head 是最后一个张量 */
    /* 矩阵乘着色器按 16 字节（uvec4）读权重和 scale，要求它们在文件里 16 字节对齐 */
    for (int l = 0; l <= NL; l++) {
        const E2EQMat *ms[4] = { &w->head, &w->head, &w->head, &w->head };
        if (l < NL) { ms[0] = &w->L[l].qkv; ms[1] = &w->L[l].o; ms[2] = &w->L[l].up; ms[3] = &w->L[l].down; }
        for (int k = 0; k < 4; k++)
            if ((((const uint8_t *)ms[k]->q - p) | ((const uint8_t *)ms[k]->s - p)) & 15)
                FAIL("权重没有 16 字节对齐（层 %d 矩阵 %d）", l, k);
    }

    v->lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!v->lib)
        FAIL("没有 libvulkan.so.1");
    v->gipa = (PFN_vkGetInstanceProcAddr)dlsym(v->lib, "vkGetInstanceProcAddr");
    if (!v->gipa)
        FAIL("找不到 vkGetInstanceProcAddr");
    v->vkCreateInstance = (PFN_vkCreateInstance)v->gipa(NULL, "vkCreateInstance");
    if (!v->vkCreateInstance)
        FAIL("Vulkan 装载器不可用");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "minime-e2e",
                              .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    if (v->vkCreateInstance(&ici, NULL, &v->inst) != VK_SUCCESS)
        FAIL("vkCreateInstance 失败");
#define LOADI(f) do { v->f = (PFN_##f)v->gipa(v->inst, #f); if (!v->f) FAIL("Vulkan 驱动缺少 %s（需要 Vulkan 1.1）", #f); } while (0);
    VKFN(LOADI)
#undef LOADI
    uint32_t n = 0;
    v->vkEnumeratePhysicalDevices(v->inst, &n, NULL);
    if (!n)
        FAIL("没有 Vulkan 设备");
    VkPhysicalDevice devs[16];
    if (n > 16) n = 16;
    v->vkEnumeratePhysicalDevices(v->inst, &n, devs);
    /* 选设备：跳过 Vulkan 1.0 老设备；独显 > 核显 > 其他（不用软件渲染的 llvmpipe，除非设了 MINIME_E2E_VK_CPU） */
    int best = -1, bestscore = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pr;
        v->vkGetPhysicalDeviceProperties(devs[i], &pr);
        if (pr.apiVersion < VK_API_VERSION_1_1)
            continue;
        int s = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
              : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
              : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? (getenv("MINIME_E2E_VK_CPU") ? 1 : -1) : 0;
        if (s > bestscore) { bestscore = s; best = (int)i; }
    }
    if (best < 0 || bestscore < 0)
        FAIL("没有可用的 Vulkan 1.1 GPU");
    v->pd = devs[best];
    VkPhysicalDeviceProperties pr;
    v->vkGetPhysicalDeviceProperties(v->pd, &pr);
    snprintf(v->name, sizeof v->name, "vulkan（%s）", pr.deviceName);
    /* 矩阵乘默认用 32 宽簇内归约（AMD/NV 原生满速）；Intel 核显及可变子组宽度设备用通用 subgroupAdd */
    VkPhysicalDeviceSubgroupSizeControlProperties sscp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES
    };
    VkPhysicalDeviceSubgroupProperties sgp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
        .pNext = pr.apiVersion >= VK_API_VERSION_1_3 ? &sscp : NULL
    };
    VkPhysicalDeviceProperties2 pr2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sgp };
    v->vkGetPhysicalDeviceProperties2(v->pd, &pr2);
    if (sgp.subgroupSize < 4 || !(sgp.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
        !(sgp.supportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT))
        FAIL("%s 的子组不满足要求（宽 %u，操作 0x%x）", pr.deviceName, sgp.subgroupSize, sgp.supportedOperations);
    uint32_t min_sg = (pr.apiVersion >= VK_API_VERSION_1_3 && sscp.minSubgroupSize >= 4)
        ? sscp.minSubgroupSize
        : (pr.vendorID == 0x8086 ? 8u : (sgp.subgroupSize > 32u ? 32u : sgp.subgroupSize));
    if (min_sg < 4)
        FAIL("%s 的最小子组过小（%u）", pr.deviceName, min_sg);
    int has_arith = (sgp.supportedOperations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) != 0;
    int has_clust = (sgp.supportedOperations & VK_SUBGROUP_FEATURE_CLUSTERED_BIT) != 0;
    v->use_cluster32 = pr.vendorID != 0x8086 && min_sg >= 32 && sgp.subgroupSize >= 32 &&
                       has_clust && getenv("MINIME_E2E_VK_GENERIC_SG") == NULL;
    if (!v->use_cluster32 && !has_arith)
        FAIL("%s 的子组不支持算术归约（宽 %u，操作 0x%x）", pr.deviceName, sgp.subgroupSize, sgp.supportedOperations);
    for (;;) {
        uint32_t wg = 32u * v->cl;
        v->max_sg = v->use_cluster32 ? v->cl : (wg + min_sg - 1u) / min_sg;
        size_t shm = (size_t)v->max_sg * 8u * 8u * sizeof(float);
        if (wg <= pr.limits.maxComputeWorkGroupSize[0] &&
            wg <= pr.limits.maxComputeWorkGroupInvocations &&
            shm <= pr.limits.maxComputeSharedMemorySize)
            break;
        if (v->cl > 1)
            v->cl = 1;
        else
            FAIL("%s 的计算工作组或共享内存上限不足", pr.deviceName);
    }
    v->vkGetPhysicalDeviceMemoryProperties(v->pd, &v->memp);
    uint32_t nq = 0;
    v->vkGetPhysicalDeviceQueueFamilyProperties(v->pd, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    v->vkGetPhysicalDeviceQueueFamilyProperties(v->pd, &nq, qf);
    /* 优先用不带图形的计算队列：图形队列和桌面合成器共用，计算会排在每一帧后面（实测每次提交多等约一帧） */
    v->qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)
        if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { v->qfam = i; break; }
    if (v->qfam == UINT32_MAX || getenv("MINIME_E2E_VK_GFXQUEUE"))
        for (uint32_t i = 0; i < nq; i++)
            if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { v->qfam = i; break; }
    if (v->qfam == UINT32_MAX)
        FAIL("设备没有计算队列");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = v->qfam,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci };
    if (v->vkCreateDevice(v->pd, &dci, NULL, &v->dev) != VK_SUCCESS)
        FAIL("vkCreateDevice 失败");
    v->vkGetDeviceQueue(v->dev, v->qfam, 0, &v->queue);

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = v->qfam };
    v->vkCreateCommandPool(v->dev, &cpi, NULL, &v->cpool);
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = v->cpool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    v->vkAllocateCommandBuffers(v->dev, &cai, &v->cb);
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    v->vkCreateFence(v->dev, &fci, NULL, &v->fence);

    /* 缓冲：模型文件（经暂存缓冲传到显存）、激活、输入、KV */
    Buf stage;
    if (buf_new(v, &v->W, fsize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MEM_DEVICE) < 0 ||
        buf_new(v, &stage, fsize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MEM_UPLOAD) < 0)
        FAIL("分配模型显存失败（%zu MB）", fsize >> 20);
    memcpy(stage.map, p, fsize);
    CopyArg c = { stage.buf, v->W.buf, fsize };
    int ok = run_once(v, rec_copy, &c);
    buf_free(v, &stage);
    if (ok < 0)
        FAIL("上传模型失败");
    if (buf_new(v, &v->act, sizeof(float) * A_TOTAL, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MEM_DEVICE) < 0 ||
        buf_new(v, &v->in, sizeof(int32_t) * (IN_CTX + MAXCTX_TOTAL), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MEM_UPLOAD) < 0 ||
        buf_new(v, &v->lg, sizeof(float) * MAXB * V, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MEM_READBACK) < 0)
        FAIL("分配激活缓冲失败");

    /* 描述符和管线 */
    VkDescriptorSetLayoutBinding lb[5];
    for (int i = 0; i < 5; i++)
        lb[i] = (VkDescriptorSetLayoutBinding){ (uint32_t)i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 5, .pBindings = lb };
    v->vkCreateDescriptorSetLayout(v->dev, &dli, NULL, &v->dsl);
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 64 };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                       .pSetLayouts = &v->dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    v->vkCreatePipelineLayout(v->dev, &pli, NULL, &v->pl);
    struct { const uint32_t *code; size_t size; } sh[P_COUNT] = {
        { spv_embed, sizeof spv_embed },
        { spv_layernorm, sizeof spv_layernorm },
        { v->use_cluster32 ? spv_mq4 : spv_mq4_sg, v->use_cluster32 ? sizeof spv_mq4 : sizeof spv_mq4_sg },
        { v->use_cluster32 ? spv_mq8 : spv_mq8_sg, v->use_cluster32 ? sizeof spv_mq8 : sizeof spv_mq8_sg },
        { spv_attn, sizeof spv_attn },
    };
    for (int i = 0; i < P_COUNT; i++) {
        VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sh[i].size, .pCode = sh[i].code };
        VkShaderModule mod;
        if (v->vkCreateShaderModule(v->dev, &smi, NULL, &mod) != VK_SUCCESS)
            FAIL("着色器 %d 创建失败", i);
        /* 矩阵乘的特化常量：0 NB、1 CL/MAX_SG、2 工作组大小（32*CL） */
        int mm = i == P_MQ4 || i == P_MQ8;
        for (int k = 0; k < (mm ? NVAR : 1); k++) {
            uint32_t spec[3] = { var_nb[k], v->max_sg, 32u * v->cl };
            VkSpecializationMapEntry me[3] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 } };
            VkSpecializationInfo si = { 3, me, sizeof spec, spec };
            VkComputePipelineCreateInfo pci = {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                           .module = mod, .pName = "main", .pSpecializationInfo = mm ? &si : NULL },
                .layout = v->pl };
            VkPipeline *dst = !mm ? &v->pipe[i] : i == P_MQ4 ? &v->mq4[k] : &v->mq8[k];
            if (v->vkCreateComputePipelines(v->dev, VK_NULL_HANDLE, 1, &pci, NULL, dst) != VK_SUCCESS) {
                v->vkDestroyShaderModule(v->dev, mod, NULL);
                FAIL("管线 %d 创建失败", i);
            }
        }
        v->vkDestroyShaderModule(v->dev, mod, NULL);
    }
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    v->vkCreateDescriptorPool(v->dev, &dpi, NULL, &v->dpool);
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = v->dpool,
                                        .descriptorSetCount = 1, .pSetLayouts = &v->dsl };
    v->vkAllocateDescriptorSets(v->dev, &dai, &v->ds);

    for (int l = 0; l < NL; l++) {
        char nm[64];
#define B16(f, s) (snprintf(nm, sizeof nm, s ".%d", l), v->bias16[l].f = find_f16(p, nm))
        if (!B16(qkvb, "attention.qkv_proj.bias") || !B16(ob, "attention.o_proj.bias") ||
            !B16(upb, "mlp.up_proj.bias") || !B16(downb, "mlp.down_proj.bias"))
            FAIL("找不到第 %d 层偏置", l);
#undef B16
    }
    if (vk_reserve(&v->base, PMAX + 64) < 0)
        FAIL("分配 KV 显存失败");
    if (getenv("MINIME_E2E_VK_EMPTY")) {     /* 实验：空命令缓冲提交—等待一次要多久 */
        int n = atoi(getenv("MINIME_E2E_VK_EMPTY"));
        double t = mono_ms();
        for (int i = 0; i < n; i++)
            run_once(v, rec_nothing, NULL);
        fprintf(stderr, "[vk] 空提交 %d 次，平均 %.3f ms/次\n", n, (mono_ms() - t) / n);
    }
    if (getenv("MINIME_E2E_VK_NDISP")) {
        int nd = atoi(getenv("MINIME_E2E_VK_NDISP"));
        double t = mono_ms();
        int gap = getenv("MINIME_E2E_VK_GAP_US") ? atoi(getenv("MINIME_E2E_VK_GAP_US")) : 0;
        double busy = 0;
        for (int i = 0; i < 20; i++) {
            if (gap) {
                struct timespec ts = { 0, gap * 1000L };
                nanosleep(&ts, NULL);
            }
            double t1 = mono_ms();
            run_once(v, rec_ndisp, &nd);
            busy += mono_ms() - t1;
        }
        (void)t;
        fprintf(stderr, "[vk] %d 次空派发 + 屏障，间隔 %d us：平均 %.3f ms/次提交\n", nd, gap, busy / 20);
    }
    if (getenv("MINIME_E2E_VK_PROF")) {
        VkQueryPoolCreateInfo qpi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                      .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 512 };
        if (v->vkCreateQueryPool(v->dev, &qpi, NULL, &v->qp) == VK_SUCCESS) {
            v->prof = 1;
            v->ts_period = pr.limits.timestampPeriod;
        }
    }
    return &v->base;
#undef FAIL
}
#endif
