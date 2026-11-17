/* tflite_smoke.c — 阶段 1 验证：装载 tensorflowlite_c.dll，加载引擎的 NNLM 模型，跑一次推理
 *
 * 同一份源码编两次：
 *   原生：gcc，链接 loader/，用 pe_load 装载 DLL
 *   Wine：x86_64-w64-mingw32-gcc，用 LoadLibrary
 * 两边打印的结果应该逐字节一致。
 * 用法: tflite_smoke <tensorflowlite_c.dll 路径> <nlrgpt2_nnlm.bin 路径>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define CALL
static void *open_dll(const char *p) { return LoadLibraryA(p); }
static void *sym(void *h, const char *n) { return (void *)GetProcAddress((HMODULE)h, n); }
static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
#else
#include <time.h>
#include "../pe.h"
#define CALL __attribute__((ms_abi))
static void *open_dll(const char *p)
{
    pe_module *m = pe_load(p);
    return m;
}
static void *sym(void *h, const char *n) { return pe_export((pe_module *)h, n); }
static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
#endif

typedef const char *(CALL *pfn_version)(void);
typedef void *(CALL *pfn_model_create)(const void *, size_t);
typedef void *(CALL *pfn_opts_create)(void);
typedef void (CALL *pfn_opts_threads)(void *, int32_t);
typedef void *(CALL *pfn_interp_create)(const void *, const void *);
typedef int (CALL *pfn_status)(void *);
typedef int32_t (CALL *pfn_count)(const void *);
typedef void *(CALL *pfn_get_tensor)(const void *, int32_t);
typedef int (CALL *pfn_ttype)(const void *);
typedef int32_t (CALL *pfn_ndims)(const void *);
typedef int32_t (CALL *pfn_dim)(const void *, int32_t);
typedef size_t (CALL *pfn_bytes)(const void *);
typedef void *(CALL *pfn_data)(const void *);
typedef const char *(CALL *pfn_tname)(const void *);
typedef int (CALL *pfn_resize)(void *, int32_t, const int *, int32_t);

#define GET(T, n) T n = (T)sym(h, "TfLite" #n); if (!n) { printf("缺少导出 TfLite%s\n", #n); return 1; }

static void describe(const char *kind, int i, const void *t, pfn_ttype Type, pfn_ndims NumDims, pfn_dim Dim,
                     pfn_bytes Bytes, pfn_tname Name)
{
    printf("%s[%d] %s type=%d dims=[", kind, i, Name(t), Type(t));
    for (int d = 0; d < NumDims(t); d++)
        printf("%s%d", d ? "," : "", Dim(t, d));
    printf("] bytes=%zu\n", Bytes(t));
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <tensorflowlite_c.dll> <nlrgpt2_nnlm.bin>\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    double t0 = now_ms();
    void *h = open_dll(argv[1]);
    if (!h) {
        printf("装载失败\n");
        return 1;
    }
    double t_load = now_ms() - t0;
    GET(pfn_version, Version);
    GET(pfn_model_create, ModelCreate);
    GET(pfn_opts_create, InterpreterOptionsCreate);
    GET(pfn_opts_threads, InterpreterOptionsSetNumThreads);
    GET(pfn_interp_create, InterpreterCreate);
    GET(pfn_status, InterpreterAllocateTensors);
    GET(pfn_status, InterpreterInvoke);
    GET(pfn_count, InterpreterGetInputTensorCount);
    GET(pfn_count, InterpreterGetOutputTensorCount);
    GET(pfn_get_tensor, InterpreterGetInputTensor);
    GET(pfn_get_tensor, InterpreterGetOutputTensor);
    GET(pfn_ttype, TensorType);
    GET(pfn_ndims, TensorNumDims);
    GET(pfn_dim, TensorDim);
    GET(pfn_bytes, TensorByteSize);
    GET(pfn_data, TensorData);
    GET(pfn_tname, TensorName);
    GET(pfn_resize, InterpreterResizeInputTensor);
    printf("TfLiteVersion = %s\n", Version());

    FILE *f = fopen(argv[2], "rb");
    if (!f) {
        printf("打不开模型\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n)
        return 1;
    fclose(f);
    /* 引擎的模型文件前 5 字节是自己的头，之后是标准 TFLite flatbuffer（"TFL3" 在偏移 9） */
    size_t off = memcmp(buf + 9, "TFL3", 4) == 0 ? 5 : 0;
    /* 模型数据（权重直接在这块内存里用）按 64 字节对齐放置，排除两边分配器对齐方式不同的影响 */
    unsigned char *data = buf + off;
    if (getenv("SMOKE_ALIGN")) {
        size_t a = (size_t)atoi(getenv("SMOKE_ALIGN"));
        unsigned char *raw = malloc(n + 4096);
        data = (unsigned char *)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095) + a;
        memcpy(data, buf + off, n - off);
    }
    fprintf(stderr, "[对齐] 模型数据地址 %% 64 = %u\n", (unsigned)((uintptr_t)data % 64));
    void *model = ModelCreate(data, n - off);
    printf("ModelCreate(偏移 %zu) -> %s\n", off, model ? "成功" : "失败");
    if (!model)
        return 1;
    void *opts = InterpreterOptionsCreate();
    InterpreterOptionsSetNumThreads(opts, 1);
    void *it = InterpreterCreate(model, opts);
    printf("InterpreterCreate -> %s\n", it ? "成功" : "失败");
    if (!it)
        return 1;
    int nin = InterpreterGetInputTensorCount(it), nout = InterpreterGetOutputTensorCount(it);
    /* 动态维度（-1 或 0）按 1x8 序列处理 */
    for (int i = 0; i < nin; i++) {
        const void *t = InterpreterGetInputTensor(it, i);
        int nd = TensorNumDims(t), dims[8];
        int dyn = 0;
        for (int d = 0; d < nd && d < 8; d++) {
            dims[d] = TensorDim(t, d);
            if (dims[d] <= 0) {
                dims[d] = d == 0 ? 1 : 8;
                dyn = 1;
            }
        }
        if (dyn)
            InterpreterResizeInputTensor(it, i, dims, nd);
    }
    printf("AllocateTensors -> %d\n", InterpreterAllocateTensors(it));
    for (int i = 0; i < nin; i++) {
        void *t = InterpreterGetInputTensor(it, i);
        describe("input", i, t, TensorType, TensorNumDims, TensorDim, TensorByteSize, TensorName);
        /* 确定性的输入：整数张量填 1..k 的小 id，浮点张量填 0.01*k */
        size_t bytes = TensorByteSize(t);
        int type = TensorType(t);
        unsigned char *d = TensorData(t);
        for (size_t k = 0; k < bytes / 4; k++) {
            if (type == 1) {
                float v = 0.01f * (float)(k % 17);
                memcpy(d + k * 4, &v, 4);
            } else {
                int32_t v = (int32_t)(k % 17) + 1;
                memcpy(d + k * 4, &v, 4);
            }
        }
        if (type == 4)   /* int64 */
            for (size_t k = 0; k < bytes / 8; k++) {
                int64_t v = (int64_t)(k % 17) + 1;
                memcpy(d + k * 8, &v, 8);
            }
    }
    double t1 = now_ms();
    int st = InterpreterInvoke(it);
    double t_inv = now_ms() - t1;
    printf("Invoke -> %d\n", st);
    for (int i = 0; i < nout; i++) {
        const void *t = InterpreterGetOutputTensor(it, i);
        describe("output", i, t, TensorType, TensorNumDims, TensorDim, TensorByteSize, TensorName);
        if (TensorType(t) == 1) {
            const float *v = TensorData(t);
            size_t cnt = TensorByteSize(t) / 4;
            double sum = 0;
            uint32_t hash = 2166136261u;
            for (size_t k = 0; k < cnt; k++) {
                sum += v[k];
                uint32_t bits;
                memcpy(&bits, &v[k], 4);
                hash = (hash ^ bits) * 16777619u;
            }
            printf("  前 4 个: %.6f %.6f %.6f %.6f  和=%.6f  哈希=%08x\n", v[0], cnt > 1 ? v[1] : 0, cnt > 2 ? v[2] : 0,
                   cnt > 3 ? v[3] : 0, sum, hash);
        }
    }
    /* 重复推理计时（第 3 个参数是次数，默认 300）：取中位数和最小值，排除第一次的预热 */
    int reps = argc > 3 ? atoi(argv[3]) : 300;
    double *ts = malloc(sizeof(double) * (reps > 0 ? reps : 1));
    for (int r = 0; r < reps; r++) {
        double a = now_ms();
        InterpreterInvoke(it);
        ts[r] = now_ms() - a;
    }
    for (int i = 1; i < reps; i++)
        for (int j = i; j > 0 && ts[j - 1] > ts[j]; j--) {
            double x = ts[j]; ts[j] = ts[j - 1]; ts[j - 1] = x;
        }
    fprintf(stderr, "[计时] 装载 %.1f ms，首次推理 %.2f ms，之后 %d 次：中位数 %.3f ms，最小 %.3f ms\n",
            t_load, t_inv, reps, reps ? ts[reps / 2] : 0, reps ? ts[0] : 0);
    return 0;
}
