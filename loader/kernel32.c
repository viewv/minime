/* kernel32.c — Win32 兼容层（阶段 1：TFLite 和 CRT 初始化需要的部分，外加锁、线程、内核对象）
 *
 * 原则：只实现 Windows 的行为，不认识引擎。热路径函数（堆、错误码、锁、线程号、TLS）
 * 尽量写成不调用 glibc 的叶子函数，或只调一次 glibc。
 * 函数名统一加 k32_ 前缀，避免和 glibc 冲突；对外名字在文件末尾的表里。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/futex.h>
#include <malloc.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#elif defined(__FreeBSD__)
#include <malloc_np.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/umtx.h>
#elif defined(__NetBSD__) || defined(__DragonFly__)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#include "pe.h"
#include "winabi.h"
#include "k32_internal.h"

extern char **environ;

/* ================= 小工具 ================= */
#if defined(__linux__)
static long futex_wait(void *addr, uint32_t val, const struct timespec *ts)
{
    return syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, ts, NULL, 0);
}

static long futex_wake(void *addr, int count)
{
    return syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, (uint32_t)count, NULL, NULL, 0);
}
#elif defined(__FreeBSD__)
static long futex_wait(void *addr, uint32_t val, const struct timespec *ts)
{
    if (ts) {
        struct _umtx_time ut;
        memset(&ut, 0, sizeof(ut));
        ut._timeout = *ts;
        ut._flags = 0; /* 相对超时 */
        ut._clockid = CLOCK_MONOTONIC;
        return _umtx_op(addr, UMTX_OP_WAIT_UINT_PRIVATE, (u_long)val, (void *)(uintptr_t)sizeof(ut), &ut);
    }
    return _umtx_op(addr, UMTX_OP_WAIT_UINT_PRIVATE, (u_long)val, NULL, NULL);
}

static long futex_wake(void *addr, int count)
{
    return _umtx_op(addr, UMTX_OP_WAKE_PRIVATE, (u_long)count, NULL, NULL);
}
#else
#define FUTEX_B1 { PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER }
#define FUTEX_B4 FUTEX_B1, FUTEX_B1, FUTEX_B1, FUTEX_B1
#define FUTEX_B16 FUTEX_B4, FUTEX_B4, FUTEX_B4, FUTEX_B4
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
} g_futex_buckets[64] = { FUTEX_B16, FUTEX_B16, FUTEX_B16, FUTEX_B16 };
#undef FUTEX_B16
#undef FUTEX_B4
#undef FUTEX_B1

static inline size_t futex_bucket(const void *addr)
{
    return ((((uintptr_t)addr) >> 2) * 2654435761u) & 63u;
}

static long futex_wait(void *addr, uint32_t val, const struct timespec *ts)
{
    size_t idx = futex_bucket(addr);
    pthread_mutex_lock(&g_futex_buckets[idx].mu);
    if (atomic_load((_Atomic uint32_t *)addr) != val) {
        pthread_mutex_unlock(&g_futex_buckets[idx].mu);
        errno = EAGAIN;
        return -1;
    }
    int r;
    if (ts) {
        uint64_t rt = now_ns(CLOCK_REALTIME) + (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
        struct timespec abs_ts = { (time_t)(rt / 1000000000ULL), (long)(rt % 1000000000ULL) };
        r = pthread_cond_timedwait(&g_futex_buckets[idx].cv, &g_futex_buckets[idx].mu, &abs_ts);
    } else {
        r = pthread_cond_wait(&g_futex_buckets[idx].cv, &g_futex_buckets[idx].mu);
    }
    pthread_mutex_unlock(&g_futex_buckets[idx].mu);
    if (r == ETIMEDOUT) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static long futex_wake(void *addr, int count)
{
    (void)count;
    size_t idx = futex_bucket(addr);
    pthread_mutex_lock(&g_futex_buckets[idx].mu);
    pthread_cond_broadcast(&g_futex_buckets[idx].cv);
    pthread_mutex_unlock(&g_futex_buckets[idx].mu);
    return 0;
}
#endif

static void ms_to_timespec(DWORD ms, struct timespec *ts)
{
    ts->tv_sec = ms / 1000;
    ts->tv_nsec = (long)(ms % 1000) * 1000000L;
}

uint64_t now_ns(int clock)
{
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

size_t u16len(const WCHAR *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* UTF-16 -> UTF-8；n<0 表示到 0 为止。返回 malloc 的字符串，可选输出字节长度（含嵌入 \0） */
static char *u16_to_utf8_len(const WCHAR *s, long n, size_t *out_len)
{
    if (!s) {
        if (out_len)
            *out_len = 0;
        return NULL;
    }
    if (n < 0)
        n = (long)u16len(s);
    char *out = malloc(n * 3 + 1), *o = out;
    for (long i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
        if (c < 0x80) *o++ = (char)c;
        else if (c < 0x800) { *o++ = 0xc0 | (c >> 6); *o++ = 0x80 | (c & 0x3f); }
        else if (c < 0x10000) { *o++ = 0xe0 | (c >> 12); *o++ = 0x80 | ((c >> 6) & 0x3f); *o++ = 0x80 | (c & 0x3f); }
        else { *o++ = 0xf0 | (c >> 18); *o++ = 0x80 | ((c >> 12) & 0x3f); *o++ = 0x80 | ((c >> 6) & 0x3f); *o++ = 0x80 | (c & 0x3f); }
    }
    if (out_len)
        *out_len = (size_t)(o - out);
    *o = 0;
    return out;
}

char *u16_to_utf8(const WCHAR *s, long n)
{
    return u16_to_utf8_len(s, n, NULL);
}

/* UTF-8 -> UTF-16，写入 dst（可为 NULL 只计数），返回 WCHAR 数 */
long utf8_to_u16(const unsigned char *s, long n, WCHAR *dst, long cap)
{
    long w = 0;
    for (long i = 0; i < n;) {
        uint32_t c = s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        if (i + len > n)
            len = 1;
        if (len == 2) c = ((c & 0x1f) << 6) | (s[i + 1] & 0x3f);
        else if (len == 3) c = ((c & 0x0f) << 12) | ((s[i + 1] & 0x3f) << 6) | (s[i + 2] & 0x3f);
        else if (len == 4) c = ((c & 0x07) << 18) | ((s[i + 1] & 0x3f) << 12) | ((s[i + 2] & 0x3f) << 6) | (s[i + 3] & 0x3f);
        else if (c >= 0x80) c = 0xfffd;
        i += len;
        if (c >= 0x10000) {
            if (dst && w + 2 <= cap) {
                dst[w] = 0xd800 + ((c - 0x10000) >> 10);
                dst[w + 1] = 0xdc00 + ((c - 0x10000) & 0x3ff);
            }
            w += 2;
        } else {
            if (dst && w < cap)
                dst[w] = (WCHAR)c;
            w++;
        }
    }
    return w;
}

/* Windows 路径 -> Linux 路径："Z:\a\b" / "Z:/a/b" -> "/a/b"，反斜杠换成斜杠 */
char *win_path_to_unix(const char *p)
{
    if (!p)
        return NULL;
    if (((p[0] | 0x20) == 'z') && p[1] == ':')
        p += 2;
    char *out = strdup(p);
    for (char *c = out; *c; c++)
        if (*c == '\\')
            *c = '/';
    return out;
}

/* ================= 错误码 ================= */
static WINAPI DWORD k32_GetLastError(void) { return get_last_error(); }
static WINAPI void k32_SetLastError(DWORD e) { set_last_error(e); }

/* ================= 堆：直接接 malloc ================= */
#define FAKE_PROCESS_HEAP ((HANDLE)0x1000)
#define HEAP_ZERO_MEMORY 0x08
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10

static WINAPI HANDLE k32_GetProcessHeap(void) { return FAKE_PROCESS_HEAP; }

#if defined(__linux__) || defined(__FreeBSD__)
static WINAPI void *k32_HeapAlloc(HANDLE h, DWORD flags, SIZE_T n)
{
    (void)h;
    void *p = (flags & HEAP_ZERO_MEMORY) ? calloc(1, n ? n : 1) : malloc(n ? n : 1);
    if (!p)
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return p;
}

static WINAPI BOOL k32_HeapFree(HANDLE h, DWORD flags, void *p)
{
    (void)h; (void)flags;
    free(p);
    return TRUE;
}

static WINAPI SIZE_T k32_HeapSize(HANDLE h, DWORD flags, const void *p)
{
    (void)h; (void)flags;
    return malloc_usable_size((void *)p);
}

static WINAPI void *k32_HeapReAlloc(HANDLE h, DWORD flags, void *p, SIZE_T n)
{
    (void)h;
    size_t old = malloc_usable_size(p);
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY)
        return n <= old ? p : NULL;
    void *q = realloc(p, n ? n : 1);
    if (q && (flags & HEAP_ZERO_MEMORY) && n > old)
        memset((char *)q + old, 0, n - old);
    return q;
}

static WINAPI void *k32_LocalFree(void *p) { free(p); return NULL; }
#else
#define HEAP_HDR_MAGIC 0x4d48454150363455ULL
typedef struct {
    uint64_t magic;
    size_t size;
} HeapHdr;
_Static_assert(sizeof(HeapHdr) == 16, "HeapHdr must preserve 16-byte alignment");

static WINAPI void *k32_HeapAlloc(HANDLE h, DWORD flags, SIZE_T n)
{
    (void)h;
    size_t sz = n ? (size_t)n : 1;
    HeapHdr *hdr = (flags & HEAP_ZERO_MEMORY) ? calloc(1, sizeof(HeapHdr) + sz) : malloc(sizeof(HeapHdr) + sz);
    if (!hdr) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    hdr->magic = HEAP_HDR_MAGIC;
    hdr->size = sz;
    return hdr + 1;
}

static WINAPI BOOL k32_HeapFree(HANDLE h, DWORD flags, void *p)
{
    (void)h; (void)flags;
    if (!p)
        return TRUE;
    HeapHdr *hdr = ((HeapHdr *)p) - 1;
    if (hdr->magic == HEAP_HDR_MAGIC) {
        hdr->magic = 0;
        free(hdr);
    } else {
        free(p);
    }
    return TRUE;
}

static WINAPI SIZE_T k32_HeapSize(HANDLE h, DWORD flags, const void *p)
{
    (void)h; (void)flags;
    if (!p)
        return 0;
    const HeapHdr *hdr = ((const HeapHdr *)p) - 1;
    return hdr->magic == HEAP_HDR_MAGIC ? (SIZE_T)hdr->size : 0;
}

static WINAPI void *k32_HeapReAlloc(HANDLE h, DWORD flags, void *p, SIZE_T n)
{
    (void)h;
    if (!p)
        return (flags & HEAP_REALLOC_IN_PLACE_ONLY) ? NULL : k32_HeapAlloc(h, flags, n);
    HeapHdr *hdr = ((HeapHdr *)p) - 1;
    if (hdr->magic != HEAP_HDR_MAGIC)
        return NULL;
    size_t old = hdr->size;
    size_t sz = n ? (size_t)n : 1;
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY)
        return sz <= old ? p : NULL;
    HeapHdr *nhdr = realloc(hdr, sizeof(HeapHdr) + sz);
    if (!nhdr) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    nhdr->size = sz;
    void *q = nhdr + 1;
    if ((flags & HEAP_ZERO_MEMORY) && sz > old)
        memset((char *)q + old, 0, sz - old);
    return q;
}

static WINAPI void *k32_LocalFree(void *p) { k32_HeapFree(FAKE_PROCESS_HEAP, 0, p); return NULL; }
#endif

static WINAPI BOOL k32_HeapValidate(HANDLE h, DWORD f, const void *p) { (void)h; (void)f; (void)p; return TRUE; }
static WINAPI BOOL k32_HeapQueryInformation(HANDLE h, int c, void *i, SIZE_T l, SIZE_T *r)
{
    (void)h; (void)c; (void)i; (void)l; (void)r;
    set_last_error(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}

/* ================= 虚拟内存 ================= */
#if defined(MAP_FIXED_NOREPLACE)
#define PE_MAP_FIXED_NOREPLACE MAP_FIXED_NOREPLACE
#elif defined(__FreeBSD__) && defined(MAP_EXCL)
#define PE_MAP_FIXED_NOREPLACE (MAP_FIXED | MAP_EXCL)
#elif defined(__NetBSD__) && defined(MAP_TRYFIXED)
#define PE_MAP_FIXED_NOREPLACE MAP_TRYFIXED
#else
#define PE_MAP_FIXED_NOREPLACE 0
#endif

#define MEM_COMMIT  0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define MEM_DECOMMIT 0x4000

static int page_prot(DWORD p)
{
    switch (p & 0xff) {
    case 0x01: return PROT_NONE;
    case 0x02: return PROT_READ;
    case 0x04: return PROT_READ | PROT_WRITE;
    case 0x08: return PROT_READ | PROT_WRITE;
    case 0x10: return PROT_EXEC;
    case 0x20: return PROT_READ | PROT_EXEC;
    case 0x40: return PROT_READ | PROT_WRITE | PROT_EXEC;
    case 0x80: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE;
    }
}

static struct { void *p; size_t n; } g_vmem[4096];
static pthread_mutex_t g_vmem_lock = PTHREAD_MUTEX_INITIALIZER;

static WINAPI void *k32_VirtualAlloc(void *addr, SIZE_T n, DWORD type, DWORD prot)
{
    if (!n) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (addr && !(type & MEM_RESERVE)) {
        /* 在已保留的区域里提交 */
        if (mprotect((void *)((uintptr_t)addr & ~4095UL), n + ((uintptr_t)addr & 4095), page_prot(prot)) == 0)
            return addr;
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    int pr = (type & MEM_COMMIT) ? page_prot(prot) : PROT_NONE;
    void *p = mmap(addr, n, pr, MAP_PRIVATE | MAP_ANONYMOUS | (addr ? PE_MAP_FIXED_NOREPLACE : 0), -1, 0);
    if (p != MAP_FAILED && addr && p != addr) {
        munmap(p, n);
        p = MAP_FAILED;
    }
    if (p == MAP_FAILED) {
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    int slot = -1;
    pthread_mutex_lock(&g_vmem_lock);
    for (int i = 0; i < 4096; i++)
        if (!g_vmem[i].p) {
            g_vmem[i].p = p;
            g_vmem[i].n = n;
            slot = i;
            break;
        }
    pthread_mutex_unlock(&g_vmem_lock);
    if (slot < 0) {
        munmap(p, n);
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return p;
}

static WINAPI BOOL k32_VirtualFree(void *addr, SIZE_T n, DWORD type)
{
    if (type & MEM_DECOMMIT) {
#ifdef __linux__
        return madvise(addr, n, MADV_DONTNEED) == 0 && mprotect(addr, n, PROT_NONE) == 0;
#else
        uintptr_t a = (uintptr_t)addr & ~4095UL;
        size_t len = ((uintptr_t)addr + n - a + 4095) & ~4095UL;
        return mmap((void *)a, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED;
#endif
    }
    pthread_mutex_lock(&g_vmem_lock);
    for (int i = 0; i < 4096; i++)
        if (g_vmem[i].p == addr) {
            munmap(addr, g_vmem[i].n);
            g_vmem[i].p = NULL;
            pthread_mutex_unlock(&g_vmem_lock);
            return TRUE;
        }
    pthread_mutex_unlock(&g_vmem_lock);
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

static WINAPI BOOL k32_VirtualProtect(void *addr, SIZE_T n, DWORD prot, DWORD *old)
{
    if (old)
        *old = 0x04;
    uintptr_t a = (uintptr_t)addr & ~4095UL;
    return mprotect((void *)a, n + ((uintptr_t)addr - a), page_prot(prot)) == 0;
}

/* ================= 线程局部存储 ================= */
static atomic_uint_least64_t g_tls_bitmap;
static WINAPI DWORD k32_TlsAlloc(void)
{
    for (;;) {
        uint64_t b = atomic_load(&g_tls_bitmap);
        if (b == UINT64_MAX)
            return 0xffffffffu;
        int i = __builtin_ctzll(~b);
        if (atomic_compare_exchange_weak(&g_tls_bitmap, &b, b | (1ULL << i)))
            return (DWORD)i;
    }
}
static WINAPI BOOL k32_TlsFree(DWORD i)
{
    if (i >= 64) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    atomic_fetch_and(&g_tls_bitmap, ~(1ULL << i));
    return TRUE;
}
static WINAPI void *k32_TlsGetValue(DWORD i)
{
    if (i >= 64) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    set_last_error(0);
    return ((void **)(cur_teb() + TEB_TLS_SLOTS))[i];
}
static WINAPI BOOL k32_TlsSetValue(DWORD i, void *v)
{
    if (i >= 64) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    ((void **)(cur_teb() + TEB_TLS_SLOTS))[i] = v;
    return TRUE;
}

typedef void (WINAPI *FlsCallback)(void *);
static FlsCallback g_fls_cb[128];
static atomic_uint_least64_t g_fls_bitmap[2] = { 1, 0 }; /* bit 0 保留，避免返回 0 */
static WINAPI DWORD k32_FlsAlloc(FlsCallback cb)
{
    for (int w = 0; w < 2; w++) {
        for (;;) {
            uint64_t b = atomic_load(&g_fls_bitmap[w]);
            if (b == UINT64_MAX)
                break;
            int bit = __builtin_ctzll(~b);
            if (atomic_compare_exchange_weak(&g_fls_bitmap[w], &b, b | (1ULL << bit))) {
                int i = w * 64 + bit;
                g_fls_cb[i] = cb;
                return (DWORD)i;
            }
        }
    }
    set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return 0xffffffffu;
}
static WINAPI BOOL k32_FlsFree(DWORD i)
{
    if (i == 0 || i >= 128) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    g_fls_cb[i] = NULL;
    atomic_fetch_and(&g_fls_bitmap[i / 64], ~(1ULL << (i % 64)));
    return TRUE;
}
static WINAPI void *k32_FlsGetValue(DWORD i)
{
    if (i == 0 || i >= 128) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    set_last_error(0);
    return (*(void ***)(cur_teb() + TEB_FLS_DATA))[i];
}
static WINAPI BOOL k32_FlsSetValue(DWORD i, void *v)
{
    if (i == 0 || i >= 128) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    (*(void ***)(cur_teb() + TEB_FLS_DATA))[i] = v;
    return TRUE;
}

/* 线程退出时调用 FLS 回调（CRT 用它释放每线程数据） */
static void fls_run_callbacks(void)
{
    void **arr = *(void ***)(cur_teb() + TEB_FLS_DATA);
    for (int i = 1; i < 128; i++)
        if (arr[i] && g_fls_cb[i]) {
            void *v = arr[i];
            arr[i] = NULL;
            g_fls_cb[i](v);
        }
}

/* ================= 临界区（递归锁，无竞争时一次原子操作） =================
 * CRITICAL_SECTION 40 字节：+0 DebugInfo，+8 LockCount（用作 futex 字），+12 RecursionCount，
 * +16 OwningThread，+24 LockSemaphore，+32 SpinCount。 */
typedef struct {
    void *debug;
    _Atomic int32_t word;    /* 0 空闲，1 占用，2 占用且有人等 */
    int32_t recursion;
    _Atomic uint64_t owner;
    uint64_t sem, spin;
} CritSec;

static inline uint64_t cur_tid(void)
{
    uint64_t t;
    __asm__ volatile("mov %%gs:0x48, %0" : "=r"(t));
    return t;
}

static WINAPI void k32_InitializeCriticalSection(CritSec *cs) { memset(cs, 0, sizeof(*cs)); }
static WINAPI BOOL k32_InitializeCriticalSectionAndSpinCount(CritSec *cs, DWORD n) { (void)n; memset(cs, 0, sizeof(*cs)); return TRUE; }
static WINAPI BOOL k32_InitializeCriticalSectionEx(CritSec *cs, DWORD n, DWORD f) { (void)n; (void)f; memset(cs, 0, sizeof(*cs)); return TRUE; }
static WINAPI void k32_DeleteCriticalSection(CritSec *cs) { (void)cs; }

static WINAPI void k32_EnterCriticalSection(CritSec *cs)
{
    uint64_t me = cur_tid();
    if (atomic_load_explicit(&cs->owner, memory_order_relaxed) == me) {
        cs->recursion++;
        return;
    }
    int32_t z = 0;
    if (!atomic_compare_exchange_strong(&cs->word, &z, 1)) {
        while (atomic_exchange(&cs->word, 2) != 0)
            futex_wait(&cs->word, 2, NULL);
    }
    atomic_store_explicit(&cs->owner, me, memory_order_relaxed);
    cs->recursion = 1;
}

static WINAPI BOOL k32_TryEnterCriticalSection(CritSec *cs)
{
    uint64_t me = cur_tid();
    if (atomic_load_explicit(&cs->owner, memory_order_relaxed) == me) {
        cs->recursion++;
        return TRUE;
    }
    int32_t z = 0;
    if (!atomic_compare_exchange_strong(&cs->word, &z, 1))
        return FALSE;
    atomic_store_explicit(&cs->owner, me, memory_order_relaxed);
    cs->recursion = 1;
    return TRUE;
}

static WINAPI void k32_LeaveCriticalSection(CritSec *cs)
{
    if (--cs->recursion)
        return;
    atomic_store_explicit(&cs->owner, 0, memory_order_relaxed);
    if (atomic_exchange(&cs->word, 0) == 2)
        futex_wake(&cs->word, 1);
}

/* ================= SRW 锁（8 字节：状态 + 等待者数） ================= */
#define SRW_WRITER 0x40000000
typedef struct { _Atomic int32_t state; _Atomic int32_t waiters; } SrwLock;

static WINAPI void k32_InitializeSRWLock(SrwLock *l) { atomic_store(&l->state, 0); atomic_store(&l->waiters, 0); }

static WINAPI void k32_AcquireSRWLockExclusive(SrwLock *l)
{
    for (;;) {
        int32_t s = 0;
        if (atomic_compare_exchange_weak(&l->state, &s, SRW_WRITER))
            return;
        if (s == 0)
            continue;
        atomic_fetch_add(&l->waiters, 1);
        futex_wait(&l->state, (uint32_t)s, NULL);
        atomic_fetch_sub(&l->waiters, 1);
    }
}

static WINAPI BOOL k32_TryAcquireSRWLockExclusive(SrwLock *l)
{
    int32_t s = 0;
    return atomic_compare_exchange_strong(&l->state, &s, SRW_WRITER);
}

static WINAPI void k32_ReleaseSRWLockExclusive(SrwLock *l)
{
    atomic_store(&l->state, 0);
    if (atomic_load(&l->waiters))
        futex_wake(&l->state, INT32_MAX);
}

static WINAPI void k32_AcquireSRWLockShared(SrwLock *l)
{
    for (;;) {
        int32_t s = atomic_load(&l->state);
        if (!(s & SRW_WRITER)) {
            if (atomic_compare_exchange_weak(&l->state, &s, s + 1))
                return;
            continue;
        }
        atomic_fetch_add(&l->waiters, 1);
        futex_wait(&l->state, (uint32_t)s, NULL);
        atomic_fetch_sub(&l->waiters, 1);
    }
}

static WINAPI BOOL k32_TryAcquireSRWLockShared(SrwLock *l)
{
    int32_t s = atomic_load(&l->state);
    return !(s & SRW_WRITER) && atomic_compare_exchange_strong(&l->state, &s, s + 1);
}

static WINAPI void k32_ReleaseSRWLockShared(SrwLock *l)
{
    if (atomic_fetch_sub(&l->state, 1) == 1 && atomic_load(&l->waiters))
        futex_wake(&l->state, INT32_MAX);
}

/* ================= 条件变量（8 字节：序号） ================= */
typedef struct { _Atomic uint32_t seq; uint32_t pad; } CondVar;
#define CONDITION_VARIABLE_LOCKMODE_SHARED 1

static WINAPI void k32_InitializeConditionVariable(CondVar *c) { atomic_store(&c->seq, 0); }
static WINAPI void k32_WakeConditionVariable(CondVar *c)
{
    atomic_fetch_add(&c->seq, 1);
    futex_wake(&c->seq, 1);
}
static WINAPI void k32_WakeAllConditionVariable(CondVar *c)
{
    atomic_fetch_add(&c->seq, 1);
    futex_wake(&c->seq, INT32_MAX);
}

static WINAPI BOOL k32_SleepConditionVariableSRW(CondVar *c, SrwLock *l, DWORD ms, DWORD flags)
{
    uint32_t s = atomic_load(&c->seq);
    int shared = flags & CONDITION_VARIABLE_LOCKMODE_SHARED;
    if (shared) k32_ReleaseSRWLockShared(l); else k32_ReleaseSRWLockExclusive(l);
    struct timespec ts, *pts = NULL;
    if (ms != INFINITE) {
        ms_to_timespec(ms, &ts);
        pts = &ts;
    }
    long r = futex_wait(&c->seq, s, pts);
    int timed_out = r != 0 && errno == ETIMEDOUT;
    if (shared) k32_AcquireSRWLockShared(l); else k32_AcquireSRWLockExclusive(l);
    if (timed_out) {
        set_last_error(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

static WINAPI BOOL k32_SleepConditionVariableCS(CondVar *c, CritSec *cs, DWORD ms)
{
    uint32_t s = atomic_load(&c->seq);
    int rec = cs->recursion;
    cs->recursion = 1;
    k32_LeaveCriticalSection(cs);
    struct timespec ts, *pts = NULL;
    if (ms != INFINITE) {
        ms_to_timespec(ms, &ts);
        pts = &ts;
    }
    long r = futex_wait(&c->seq, s, pts);
    int timed_out = r != 0 && errno == ETIMEDOUT;
    k32_EnterCriticalSection(cs);
    cs->recursion = rec;
    if (timed_out) {
        set_last_error(ERROR_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

/* ================= 一次性初始化 ================= */
#define INIT_ONCE_CHECK_ONLY 1
#define INIT_ONCE_INIT_FAILED 4
typedef struct { _Atomic uint32_t state; uint32_t pad; } InitOnce;  /* 0 未初始化，1 进行中，2 完成 */
typedef BOOL (WINAPI *InitOnceFn)(InitOnce *, void *, void **);

static WINAPI BOOL k32_InitOnceBeginInitialize(InitOnce *o, DWORD flags, BOOL *pending, void **ctx)
{
    if (ctx)
        *ctx = NULL;
    for (;;) {
        uint32_t s = atomic_load(&o->state);
        if (s == 2) {
            *pending = FALSE;
            return TRUE;
        }
        if (flags & INIT_ONCE_CHECK_ONLY) {
            set_last_error(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        if (s == 0) {
            if (atomic_compare_exchange_strong(&o->state, &s, 1)) {
                *pending = TRUE;
                return TRUE;
            }
            continue;
        }
        futex_wait(&o->state, 1, NULL);
    }
}

static WINAPI BOOL k32_InitOnceComplete(InitOnce *o, DWORD flags, void *ctx)
{
    (void)ctx;
    atomic_store(&o->state, (flags & INIT_ONCE_INIT_FAILED) ? 0 : 2);
    futex_wake(&o->state, INT32_MAX);
    return TRUE;
}

static WINAPI BOOL k32_InitOnceExecuteOnce(InitOnce *o, InitOnceFn fn, void *param, void **ctx)
{
    BOOL pending;
    if (!k32_InitOnceBeginInitialize(o, 0, &pending, ctx))
        return FALSE;
    if (!pending)
        return TRUE;
    BOOL ok = fn(o, param, ctx);
    k32_InitOnceComplete(o, ok ? 0 : INIT_ONCE_INIT_FAILED, NULL);
    return ok;
}

/* ================= 单链表（SList），用一把全局锁保证正确 ================= */
typedef struct SListEntry { struct SListEntry *next; } SListEntry;
typedef struct { SListEntry *first; uint16_t depth; uint16_t seq; uint32_t pad; } SListHeader;
static pthread_mutex_t g_slist_lock = PTHREAD_MUTEX_INITIALIZER;

static WINAPI void k32_InitializeSListHead(SListHeader *h) { memset(h, 0, sizeof(*h)); }
static WINAPI SListEntry *k32_InterlockedPushEntrySList(SListHeader *h, SListEntry *e)
{
    pthread_mutex_lock(&g_slist_lock);
    SListEntry *old = h->first;
    e->next = old;
    h->first = e;
    h->depth++;
    pthread_mutex_unlock(&g_slist_lock);
    return old;
}
static WINAPI SListEntry *k32_InterlockedPopEntrySList(SListHeader *h)
{
    pthread_mutex_lock(&g_slist_lock);
    SListEntry *e = h->first;
    if (e) {
        h->first = e->next;
        h->depth--;
    }
    pthread_mutex_unlock(&g_slist_lock);
    return e;
}
static WINAPI SListEntry *k32_InterlockedFlushSList(SListHeader *h)
{
    pthread_mutex_lock(&g_slist_lock);
    SListEntry *e = h->first;
    h->first = NULL;
    h->depth = 0;
    pthread_mutex_unlock(&g_slist_lock);
    return e;
}

/* ================= 内核对象：事件、信号量、互斥体、定时器、线程、完成端口、文件 ================= */
static pthread_mutex_t g_ko_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ko_cond = PTHREAD_COND_INITIALIZER;
static KObj *g_std[3];

KObj *ko_new(int type)
{
    KObj *o = calloc(1, sizeof(*o));
    o->magic = KO_MAGIC;
    o->type = type;
    o->refs = 1;
    o->fd = -1;
    return o;
}

KObj *ko(HANDLE h)
{
    KObj *o = (KObj *)h;
    if ((uintptr_t)h < 0x10000 || (intptr_t)h < 0 || o->magic != KO_MAGIC)
        return NULL;
    return o;
}

static __thread KObj *t_self;     /* 当前线程的线程对象（我们建的线程才有） */
static __thread jmp_buf t_exit_jb;

void ko_unref(KObj *o)
{
    if (!o)
        return;
    if (atomic_fetch_sub_explicit(&o->refs, 1, memory_order_acq_rel) != 1)
        return;
    o->magic = 0;
    free(o->path);
    free(o->pattern);
    while (o->apcs) {
        Apc *a = o->apcs;
        o->apcs = a->next;
        free(a);
    }
    if (o->type == KO_IOCP) {
        while (o->qhead) {
            Completion *c = o->qhead;
            o->qhead = c->next;
            free(c);
        }
        pthread_mutex_destroy(&o->qmu);
        pthread_cond_destroy(&o->qcv);
    }
    free(o);
}

WINAPI BOOL k32_CloseHandle(HANDLE h)
{
    KObj *o = ko(h);
    if (!o) {
        if ((intptr_t)h < 0)
            return TRUE;   /* 伪句柄 */
        set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if ((o->type == KO_FILE || o->type == KO_MAPPING) && o->fd > 2) {
        close(o->fd);
        o->fd = -1;
    }
    if (o->type == KO_FILE && o->delete_on_close && o->path) {
        if (unlink(o->path) != 0)
            rmdir(o->path);
        o->delete_on_close = 0;
    }
    if (o->type == KO_FIND)
        find_close_obj(o);
    ko_unref(o);
    return TRUE;
}

static WINAPI HANDLE k32_CreateEventExW(void *sa, LPCWSTR name, DWORD flags, DWORD access)
{
    (void)sa; (void)name; (void)access;
    KObj *o = ko_new(KO_EVENT);
    o->manual = (flags & 1) != 0;       /* CREATE_EVENT_MANUAL_RESET */
    o->signaled = (flags & 2) != 0;     /* CREATE_EVENT_INITIAL_SET */
    return o;
}
static WINAPI HANDLE k32_CreateEventW(void *sa, BOOL manual, BOOL initial, LPCWSTR name)
{
    (void)sa; (void)name;
    KObj *o = ko_new(KO_EVENT);
    o->manual = manual;
    o->signaled = initial;
    return o;
}
static WINAPI HANDLE k32_CreateEventA(void *sa, BOOL manual, BOOL initial, LPCSTR name)
{
    (void)name;
    return k32_CreateEventW(sa, manual, initial, NULL);
}

static WINAPI BOOL k32_SetEvent(HANDLE h)
{
    KObj *o = ko(h);
    if (!o)
        return FALSE;
    pthread_mutex_lock(&g_ko_lock);
    o->signaled = 1;
    pthread_cond_broadcast(&g_ko_cond);
    pthread_mutex_unlock(&g_ko_lock);
    return TRUE;
}
static WINAPI BOOL k32_ResetEvent(HANDLE h)
{
    KObj *o = ko(h);
    if (!o)
        return FALSE;
    pthread_mutex_lock(&g_ko_lock);
    o->signaled = 0;
    pthread_mutex_unlock(&g_ko_lock);
    return TRUE;
}

static WINAPI HANDLE k32_CreateSemaphoreW(void *sa, LONG init, LONG max, LPCWSTR name)
{
    (void)sa; (void)name;
    KObj *o = ko_new(KO_SEMAPHORE);
    o->count = init;
    o->max = max;
    return o;
}
static WINAPI HANDLE k32_CreateSemaphoreExW(void *sa, LONG init, LONG max, LPCWSTR name, DWORD f, DWORD a)
{
    (void)f; (void)a;
    return k32_CreateSemaphoreW(sa, init, max, name);
}
static WINAPI BOOL k32_ReleaseSemaphore(HANDLE h, LONG n, LONG *prev)
{
    KObj *o = ko(h);
    if (!o)
        return FALSE;
    pthread_mutex_lock(&g_ko_lock);
    if (prev)
        *prev = o->count;
    o->count += n;
    if (o->count > o->max)
        o->count = o->max;
    pthread_cond_broadcast(&g_ko_cond);
    pthread_mutex_unlock(&g_ko_lock);
    return TRUE;
}

static WINAPI HANDLE k32_CreateMutexW(void *sa, BOOL owned, LPCWSTR name)
{
    (void)sa; (void)name;
    KObj *o = ko_new(KO_MUTEX);
    if (owned) {
        o->owner = cur_tid();
        o->recursion = 1;
    }
    return o;
}
static WINAPI BOOL k32_ReleaseMutex(HANDLE h)
{
    KObj *o = ko(h);
    if (!o || o->owner != cur_tid())
        return FALSE;
    pthread_mutex_lock(&g_ko_lock);
    if (--o->recursion == 0)
        o->owner = 0;
    pthread_cond_broadcast(&g_ko_cond);
    pthread_mutex_unlock(&g_ko_lock);
    return TRUE;
}

static WINAPI HANDLE k32_CreateWaitableTimerW(void *sa, BOOL manual, LPCWSTR name)
{
    (void)sa; (void)name;
    KObj *o = ko_new(KO_TIMER);
    o->manual = manual;
    return o;
}
static WINAPI HANDLE k32_CreateWaitableTimerA(void *sa, BOOL manual, LPCSTR name)
{
    (void)name;
    return k32_CreateWaitableTimerW(sa, manual, NULL);
}
#define FILETIME_UNIX_DIFF 116444736000000000ULL
static WINAPI BOOL k32_SetWaitableTimer(HANDLE h, const int64_t *due, LONG period, void *cb, void *arg, BOOL resume)
{
    (void)cb; (void)arg; (void)resume;
    KObj *o = ko(h);
    if (!o)
        return FALSE;
    uint64_t now = now_ns(CLOCK_MONOTONIC);
    uint64_t d;
    if (*due < 0) {
        d = now + (uint64_t)(-*due) * 100;
    } else {
        /* 绝对时间（1601 年起的 100ns）。asio 关闭时传 1 表示"立刻触发"，
         * 早于 1970 年的值直接当作已经到期，避免乘 100 时溢出 */
        uint64_t real = now_ns(CLOCK_REALTIME);
        if ((uint64_t)*due <= FILETIME_UNIX_DIFF) {
            d = now;
        } else {
            uint64_t abs_ns = ((uint64_t)*due - FILETIME_UNIX_DIFF) * 100;
            d = abs_ns > real ? now + (abs_ns - real) : now;
        }
    }
    pthread_mutex_lock(&g_ko_lock);
    o->due_ns = d;
    o->period_ms = (uint32_t)period;
    o->signaled = 0;
    pthread_cond_broadcast(&g_ko_cond);
    pthread_mutex_unlock(&g_ko_lock);
    return TRUE;
}
static WINAPI BOOL k32_CancelWaitableTimer(HANDLE h)
{
    KObj *o = ko(h);
    if (!o)
        return FALSE;
    pthread_mutex_lock(&g_ko_lock);
    o->due_ns = 0;
    pthread_mutex_unlock(&g_ko_lock);
    return TRUE;
}

/* 在 g_ko_lock 下检查对象是否可以被"获取"；acquire=1 时顺带消耗它 */
static int ko_ready(KObj *o, int acquire, uint64_t now, uint64_t *next_due)
{
    switch (o->type) {
    case KO_EVENT:
        if (!o->signaled)
            return 0;
        if (acquire && !o->manual)
            o->signaled = 0;
        return 1;
    case KO_SEMAPHORE:
        if (o->count <= 0)
            return 0;
        if (acquire)
            o->count--;
        return 1;
    case KO_MUTEX:
        if (o->owner && o->owner != cur_tid())
            return 0;
        if (acquire) {
            o->owner = cur_tid();
            o->recursion++;
        }
        return 1;
    case KO_TIMER:
        if (o->due_ns && now >= o->due_ns) {
            o->signaled = 1;
            o->due_ns = o->period_ms ? now + (uint64_t)o->period_ms * 1000000ULL : 0;
        }
        if (o->due_ns && (!*next_due || o->due_ns < *next_due))
            *next_due = o->due_ns;
        if (!o->signaled)
            return 0;
        if (acquire && !o->manual)
            o->signaled = 0;
        return 1;
    case KO_THREAD:
        return o->signaled;
    default:
        return 1;
    }
}

#define WAIT_IO_COMPLETION 0xc0

static int run_apcs_locked(void)
{
    if (!t_self || !t_self->apcs)
        return 0;
    Apc *a = t_self->apcs;
    t_self->apcs = NULL;
    pthread_mutex_unlock(&g_ko_lock);
    while (a) {
        Apc *n = a->next;
        a->fn(a->arg);
        free(a);
        a = n;
    }
    pthread_mutex_lock(&g_ko_lock);
    return 1;
}

static DWORD wait_objects(DWORD n, HANDLE *hs, BOOL all, DWORD ms, BOOL alertable)
{
    KObj *objs[64];
    if (n == 0 || n > 64) {
        set_last_error(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    for (DWORD i = 0; i < n; i++) {
        objs[i] = ko(hs[i]);
        if (!objs[i] && hs[i] == (HANDLE)(intptr_t)-2)
            objs[i] = t_self;  /* GetCurrentThread() 伪句柄：等自己永远不会返回 */
        if (!objs[i]) {
            set_last_error(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
    }
    uint64_t deadline = ms == INFINITE ? 0 : now_ns(CLOCK_MONOTONIC) + (uint64_t)ms * 1000000ULL;
    pthread_mutex_lock(&g_ko_lock);
    for (;;) {
        if (alertable && run_apcs_locked()) {
            pthread_mutex_unlock(&g_ko_lock);
            return WAIT_IO_COMPLETION;
        }
        uint64_t now = now_ns(CLOCK_MONOTONIC), next_due = 0;
        if (all) {
            int ok = 1;
            for (DWORD i = 0; i < n; i++)
                ok &= ko_ready(objs[i], 0, now, &next_due);
            if (ok) {
                for (DWORD i = 0; i < n; i++)
                    ko_ready(objs[i], 1, now, &next_due);
                pthread_mutex_unlock(&g_ko_lock);
                return WAIT_OBJECT_0;
            }
        } else {
            for (DWORD i = 0; i < n; i++)
                if (ko_ready(objs[i], 1, now, &next_due)) {
                    pthread_mutex_unlock(&g_ko_lock);
                    return WAIT_OBJECT_0 + i;
                }
        }
        if (deadline && now >= deadline) {
            pthread_mutex_unlock(&g_ko_lock);
            return WAIT_TIMEOUT;
        }
        uint64_t until = deadline;
        if (next_due && (!until || next_due < until))
            until = next_due;
        if (until) {
            /* pthread_cond_timedwait 用 CLOCK_REALTIME，换算一下 */
            uint64_t rt = now_ns(CLOCK_REALTIME) + (until - now);
            struct timespec ts = { (time_t)(rt / 1000000000ULL), (long)(rt % 1000000000ULL) };
            pthread_cond_timedwait(&g_ko_cond, &g_ko_lock, &ts);
        } else {
            pthread_cond_wait(&g_ko_cond, &g_ko_lock);
        }
    }
}

static void wait_trace(const char *fn, DWORD n, HANDLE *hs, DWORD ms, void *ret)
{
    static int v = -1;
    if (v < 0)
        v = getenv("MINIME_TRACE_WAIT") != NULL;
    if (!v)
        return;
    pe_module *m = pe_module_by_addr(ret);
    fprintf(stderr, "[wait] %s n=%u ms=%d 调用方 %s+%#lx 对象:", fn, n, (int)ms, m ? m->name : "?",
            m ? (unsigned long)((uint8_t *)ret - m->base) : 0UL);
    for (DWORD i = 0; i < n && i < 8; i++) {
        KObj *o = ko(hs[i]);
        fprintf(stderr, " %p(类型%d%s)", hs[i], o ? o->type : -1, o && o->type == KO_THREAD ? (o->signaled ? ",已退出" : ",运行中") : "");
    }
    fprintf(stderr, " 线程 %lu\n", (unsigned long)cur_tid());
}

static WINAPI DWORD k32_WaitForSingleObject(HANDLE h, DWORD ms)
{
    wait_trace("WaitForSingleObject", 1, &h, ms, __builtin_return_address(0));
    return wait_objects(1, &h, FALSE, ms, FALSE);
}
static WINAPI DWORD k32_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    wait_trace("WaitForSingleObjectEx", 1, &h, ms, __builtin_return_address(0));
    return wait_objects(1, &h, FALSE, ms, alertable);
}
static WINAPI DWORD k32_WaitForMultipleObjects(DWORD n, HANDLE *hs, BOOL all, DWORD ms)
{
    wait_trace(all ? "WaitForMultipleObjects(全部)" : "WaitForMultipleObjects(任一)", n, hs, ms, __builtin_return_address(0));
    return wait_objects(n, hs, all, ms, FALSE);
}
static WINAPI DWORD k32_WaitForMultipleObjectsEx(DWORD n, HANDLE *hs, BOOL all, DWORD ms, BOOL alertable) { return wait_objects(n, hs, all, ms, alertable); }

static WINAPI void k32_Sleep(DWORD ms)
{
    if (!ms) {
        sched_yield();
        return;
    }
    struct timespec ts;
    ms_to_timespec(ms, &ts);
    while (nanosleep(&ts, &ts) && errno == EINTR) {
    }
}

static WINAPI DWORD k32_SleepEx(DWORD ms, BOOL alertable)
{
    if (!alertable) {
        k32_Sleep(ms);
        return 0;
    }
    /* 可警醒的睡眠：等一个永远不会触发的事件，期间执行 APC */
    static KObj *never;
    if (!never)
        never = ko_new(KO_EVENT);
    HANDLE h = never;
    DWORD r = wait_objects(1, &h, FALSE, ms, TRUE);
    return r == WAIT_IO_COMPLETION ? WAIT_IO_COMPLETION : 0;
}

static WINAPI BOOL k32_SwitchToThread(void) { sched_yield(); return TRUE; }

static WINAPI DWORD k32_QueueUserAPC(void (WINAPI *fn)(ULONG_PTR), HANDLE th, ULONG_PTR arg)
{
    if (getenv("MINIME_TRACE_WAIT"))
        fprintf(stderr, "[wait] QueueUserAPC 目标 %p\n", th);
    KObj *o = ko(th);
    if (!o && th == (HANDLE)(intptr_t)-2)
        o = t_self;
    if (!o || o->type != KO_THREAD)
        return 0;
    Apc *a = calloc(1, sizeof(*a));
    a->fn = fn;
    a->arg = arg;
    pthread_mutex_lock(&g_ko_lock);
    Apc **tail = &o->apcs;
    while (*tail)
        tail = &(*tail)->next;
    *tail = a;
    pthread_cond_broadcast(&g_ko_cond);
    pthread_mutex_unlock(&g_ko_lock);
    return 1;
}

/* ---- 线程 ---- */
typedef DWORD (WINAPI *ThreadProc)(void *);
#define CREATE_SUSPENDED 4
#define STILL_ACTIVE 259

static void *thread_trampoline(void *arg)
{
    KObj *o = arg;
    t_self = o;
    /* 先公布线程号再跑 DLL 通知：创建者在等线程号，期间可能持有别的锁 */
    __atomic_store_n(&o->tid, (DWORD)pe_host_tid(), __ATOMIC_RELEASE);
    pe_thread_start_hooks();
    pthread_mutex_lock(&g_ko_lock);
    while (o->suspended)
        pthread_cond_wait(&g_ko_cond, &g_ko_lock);
    pthread_mutex_unlock(&g_ko_lock);
    DWORD code;
    if (!setjmp(t_exit_jb))
        code = ((ThreadProc)o->start)(o->param);
    else
        code = o->exit_code;
    fls_run_callbacks();
    pe_thread_exit_hooks();
    pthread_mutex_lock(&g_ko_lock);
    o->exit_code = code;
    o->signaled = 1;
    pthread_cond_broadcast(&g_ko_cond);
    pthread_mutex_unlock(&g_ko_lock);
    t_self = NULL;
    ko_unref(o);
    return NULL;
}

static WINAPI HANDLE k32_CreateThread(void *sa, SIZE_T stack, ThreadProc start, void *param, DWORD flags, DWORD *tid)
{
    (void)sa;
    KObj *o = ko_new(KO_THREAD);
    o->refs = 2;                    /* 1 份归调用者句柄，1 份归 thread_trampoline */
    o->start = (void *)start;
    o->param = param;
    o->suspended = (flags & CREATE_SUSPENDED) != 0;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack > (1 << 20) ? stack : (2 << 20));
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&o->th, &a, thread_trampoline, o) != 0) {
        pthread_attr_destroy(&a);
        o->refs = 1;
        ko_unref(o);
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    pthread_attr_destroy(&a);
    /* 线程号在新线程里才知道；等它填好（很快） */
    while (!__atomic_load_n(&o->tid, __ATOMIC_ACQUIRE))
        sched_yield();
    if (tid)
        *tid = o->tid;
    return o;
}

static WINAPI DWORD k32_ResumeThread(HANDLE h)
{
    KObj *o = ko(h);
    if (!o)
        return (DWORD)-1;
    pthread_mutex_lock(&g_ko_lock);
    DWORD prev = (DWORD)o->suspended;
    o->suspended = 0;
    pthread_cond_broadcast(&g_ko_cond);
    pthread_mutex_unlock(&g_ko_lock);
    return prev;
}

static WINAPI void k32_ExitThread(DWORD code)
{
    if (t_self) {
        t_self->exit_code = code;
        longjmp(t_exit_jb, 1);
    }
    pthread_exit(NULL);
}

static WINAPI void k32_FreeLibraryAndExitThread(HMODULE m, DWORD code)
{
    (void)m;
    k32_ExitThread(code);
}

static WINAPI BOOL k32_GetExitCodeThread(HANDLE h, DWORD *code)
{
    KObj *o = ko(h);
    if (!o)
        return FALSE;
    pthread_mutex_lock(&g_ko_lock);
    *code = o->signaled ? o->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&g_ko_lock);
    return TRUE;
}

static WINAPI HANDLE k32_GetCurrentThread(void) { return (HANDLE)(intptr_t)-2; }
static WINAPI DWORD k32_GetCurrentThreadId(void) { return (DWORD)cur_tid(); }
static WINAPI HANDLE k32_GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
static WINAPI DWORD k32_GetCurrentProcessId(void) { return (DWORD)getpid(); }
static WINAPI DWORD k32_GetCurrentProcessorNumber(void)
{
#ifdef __linux__
    int c = sched_getcpu();
    return c < 0 ? 0 : (DWORD)c;
#else
    return 0;
#endif
}
static WINAPI DWORD_PTR k32_SetThreadAffinityMask(HANDLE h, DWORD_PTR m) { (void)h; (void)m; return 1; }
static WINAPI BOOL k32_SetThreadPriority(HANDLE h, int p) { (void)h; (void)p; return TRUE; }
static WINAPI int k32_GetThreadPriority(HANDLE h) { (void)h; return 0; }

/* ---- 完成端口（引擎的任务队列；每个端口自己的锁和条件变量） ---- */
static WINAPI HANDLE k32_CreateIoCompletionPort(HANDLE file, HANDLE existing, ULONG_PTR key, DWORD threads)
{
    (void)key; (void)threads;
    if (file != INVALID_HANDLE_VALUE) {
        /* 把文件关联到端口：引擎没这么用，先不支持 */
        set_last_error(ERROR_CALL_NOT_IMPLEMENTED);
        return existing;
    }
    KObj *o = ko_new(KO_IOCP);
    pthread_mutex_init(&o->qmu, NULL);
    pthread_cond_init(&o->qcv, NULL);
    return o;
}

static WINAPI BOOL k32_PostQueuedCompletionStatus(HANDLE port, DWORD bytes, ULONG_PTR key, void *ov)
{
    KObj *o = ko(port);
    if (!o || o->type != KO_IOCP)
        return FALSE;
    Completion *c = malloc(sizeof(*c));
    c->bytes = bytes;
    c->key = key;
    c->ov = ov;
    c->next = NULL;
    pthread_mutex_lock(&o->qmu);
    if (o->qtail)
        o->qtail->next = c;
    else
        o->qhead = c;
    o->qtail = c;
    pthread_cond_signal(&o->qcv);
    pthread_mutex_unlock(&o->qmu);
    return TRUE;
}

static WINAPI BOOL k32_GetQueuedCompletionStatus(HANDLE port, DWORD *bytes, ULONG_PTR *key, void **ov, DWORD ms)
{
    KObj *o = ko(port);
    if (!o || o->type != KO_IOCP)
        return FALSE;
    pthread_mutex_lock(&o->qmu);
    if (!o->qhead && ms != INFINITE) {
        uint64_t rt = now_ns(CLOCK_REALTIME) + (uint64_t)ms * 1000000ULL;
        struct timespec ts = { (time_t)(rt / 1000000000ULL), (long)(rt % 1000000000ULL) };
        while (!o->qhead)
            if (pthread_cond_timedwait(&o->qcv, &o->qmu, &ts) == ETIMEDOUT)
                break;
    } else {
        while (!o->qhead)
            pthread_cond_wait(&o->qcv, &o->qmu);
    }
    Completion *c = o->qhead;
    if (c) {
        o->qhead = c->next;
        if (!o->qhead)
            o->qtail = NULL;
    }
    pthread_mutex_unlock(&o->qmu);
    if (!c) {
        *ov = NULL;
        set_last_error(WAIT_TIMEOUT);
        return FALSE;
    }
    *bytes = c->bytes;
    *key = c->key;
    *ov = c->ov;
    free(c);
    return TRUE;
}

/* ================= 文件（阶段 1 只做最基本的；目录、查找、映射放到阶段 2） ================= */
static KObj *std_handle(int fd)
{
    if (!g_std[fd]) {
        KObj *o = ko_new(KO_FILE);
        o->fd = fd;
        g_std[fd] = o;
    }
    return g_std[fd];
}

static WINAPI HANDLE k32_GetStdHandle(DWORD which)
{
    switch ((int32_t)which) {
    case -10: return std_handle(0);
    case -11: return std_handle(1);
    case -12: return std_handle(2);
    }
    return INVALID_HANDLE_VALUE;
}
static WINAPI BOOL k32_SetStdHandle(DWORD which, HANDLE h) { (void)which; (void)h; return TRUE; }

#define FILE_TYPE_DISK 1
#define FILE_TYPE_CHAR 2
#define FILE_TYPE_PIPE 3
static WINAPI DWORD k32_GetFileType(HANDLE h)
{
    KObj *o = ko(h);
    if (!o || o->type != KO_FILE)
        return 0;
    struct stat st;
    if (fstat(o->fd, &st) != 0)
        return 0;
    if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))
        return FILE_TYPE_PIPE;
    if (S_ISCHR(st.st_mode))
        return FILE_TYPE_CHAR;
    return FILE_TYPE_DISK;
}

static int g_quiet_std = -1;
/* OVERLAPPED：+0 Internal，+8 InternalHigh，+16 Offset，+20 OffsetHigh，+24 hEvent。
 * 同步句柄带 OVERLAPPED 时，偏移就是读写位置（相当于 pread/pwrite）。 */
static int64_t ov_offset(void *ov)
{
    uint32_t lo = *(uint32_t *)((uint8_t *)ov + 16), hi = *(uint32_t *)((uint8_t *)ov + 20);
    return (int64_t)(((uint64_t)hi << 32) | lo);
}

static WINAPI BOOL k32_WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov)
{
    KObj *o = ko(h);
    if (!o || o->type != KO_FILE) {
        set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (o->fd == 1 || o->fd == 2) {
        /* 引擎往标准输出/错误写日志，默认丢弃（每个按键几百次，见 docs/engine.md 的性能一节） */
        if (g_quiet_std < 0)
            g_quiet_std = getenv("MINIME_ENGINE_STDERR") == NULL;
        if (g_quiet_std) {
            if (written)
                *written = n;
            return TRUE;
        }
    }
    ssize_t r;
    if (ov && ov_offset(ov) != -1)
        r = pwrite(o->fd, buf, n, ov_offset(ov));
    else
        r = write(o->fd, buf, n);
    if (r < 0) {
        set_last_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (written)
        *written = (DWORD)r;
    if (ov)
        *(uint64_t *)((uint8_t *)ov + 8) = (uint64_t)r;
    return TRUE;
}

#define ERROR_HANDLE_EOF 38
static WINAPI BOOL k32_ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov)
{
    KObj *o = ko(h);
    if (!o || o->type != KO_FILE) {
        set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    ssize_t r = ov ? pread(o->fd, buf, n, ov_offset(ov)) : read(o->fd, buf, n);
    if (r < 0) {
        set_last_error(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (got)
        *got = (DWORD)r;
    if (ov) {
        *(uint64_t *)((uint8_t *)ov + 8) = (uint64_t)r;
        if (r == 0 && n) {
            /* 带偏移读到文件尾：Windows 返回失败 + ERROR_HANDLE_EOF */
            set_last_error(ERROR_HANDLE_EOF);
            return FALSE;
        }
    }
    return TRUE;
}

static WINAPI BOOL k32_GetConsoleMode(HANDLE h, DWORD *m) { (void)h; (void)m; set_last_error(ERROR_INVALID_HANDLE); return FALSE; }
static WINAPI BOOL k32_SetConsoleMode(HANDLE h, DWORD m) { (void)h; (void)m; return FALSE; }
static WINAPI BOOL k32_FlushFileBuffers(HANDLE h) { KObj *o = ko(h); return o && o->type == KO_FILE && (o->fd <= 2 || fsync(o->fd) == 0); }

/* ================= 模块 ================= */
/* kernel32/kernelbase/ntdll/api-ms-* 没有真实映像，用一个假句柄代表，GetProcAddress 在兼容层里查 */
static uint8_t g_pseudo_sys[16];
#define PSEUDO_SYSTEM ((HMODULE)g_pseudo_sys)
static uint8_t g_pseudo_exe[16];
#define PSEUDO_EXE ((HMODULE)g_pseudo_exe)

static int is_system_module_name(const char *n)
{
    const char *b = strrchr(n, '\\');
    n = b ? b + 1 : n;
    return !strncasecmp(n, "api-ms-win-", 11) || !strncasecmp(n, "kernel32", 8) ||
           !strncasecmp(n, "kernelbase", 10) || !strncasecmp(n, "ntdll", 5);
}

static HMODULE module_by_name(const char *name)
{
    if (!name)
        return PSEUDO_EXE;
    if (is_system_module_name(name))
        return PSEUDO_SYSTEM;
    char *u = win_path_to_unix(name);
    pe_module *m = pe_module_by_name(u);
    free(u);
    return m ? m->base : NULL;
}

static WINAPI HMODULE k32_GetModuleHandleA(LPCSTR name)
{
    HMODULE h = module_by_name(name);
    if (!h)
        set_last_error(ERROR_MOD_NOT_FOUND);
    return h;
}

static WINAPI HMODULE k32_GetModuleHandleW(LPCWSTR name)
{
    if (!name)
        return PSEUDO_EXE;
    char *a = u16_to_utf8(name, -1);
    HMODULE h = k32_GetModuleHandleA(a);
    free(a);
    return h;
}

#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4
static WINAPI BOOL k32_GetModuleHandleExW(DWORD flags, LPCWSTR name, HMODULE *out)
{
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        pe_module *m = pe_module_by_addr(name);
        *out = m ? m->base : NULL;
    } else {
        *out = k32_GetModuleHandleW(name);
    }
    if (!*out) {
        set_last_error(ERROR_MOD_NOT_FOUND);
        return FALSE;
    }
    return TRUE;
}

static HMODULE load_library(const char *name)
{
    HMODULE h = module_by_name(name);
    if (h)
        return h;
    char *u = win_path_to_unix(name);
    /* 只认识"已装载模块所在目录"里的 DLL；系统里其他 DLL 一律不存在 */
    pe_module *m = NULL;
    if (u[0] == '/') {
        if (access(u, R_OK) == 0)
            m = pe_load(u);
    } else {
        for (pe_module *p = pe_module_by_name("libwxhld.dll") ? pe_module_by_name("libwxhld.dll") : NULL; p; p = NULL) {
            char path[1024];
            snprintf(path, sizeof(path), "%s", p->path);
            char *s = strrchr(path, '/');
            if (s) {
                snprintf(s + 1, sizeof(path) - (s + 1 - path), "%s", u);
                if (access(path, R_OK) == 0)
                    m = pe_load(path);
            }
        }
    }
    free(u);
    if (!m) {
        set_last_error(ERROR_MOD_NOT_FOUND);
        return NULL;
    }
    return m->base;
}

static WINAPI HMODULE k32_LoadLibraryA(LPCSTR n) { return load_library(n); }
static WINAPI HMODULE k32_LoadLibraryExA(LPCSTR n, HANDLE f, DWORD fl) { (void)f; (void)fl; return load_library(n); }
static WINAPI HMODULE k32_LoadLibraryW(LPCWSTR n)
{
    char *a = u16_to_utf8(n, -1);
    HMODULE h = load_library(a);
    free(a);
    return h;
}
static WINAPI HMODULE k32_LoadLibraryExW(LPCWSTR n, HANDLE f, DWORD fl) { (void)f; (void)fl; return k32_LoadLibraryW(n); }
static WINAPI BOOL k32_FreeLibrary(HMODULE h) { (void)h; return TRUE; }

static WINAPI FARPROC k32_GetProcAddress(HMODULE h, LPCSTR name)
{
    void *p = NULL;
    if (h == PSEUDO_SYSTEM) {
        if ((uintptr_t)name >= 0x10000)
            p = shim_lookup(name);
    } else {
        p = pe_export(pe_module_by_base(h), name);
    }
    if (!p)
        set_last_error(ERROR_PROC_NOT_FOUND);
    return (FARPROC)p;
}

static DWORD module_file_name(HMODULE h, char *out, DWORD n)
{
    char tmp[1024];
    pe_module *m = h ? pe_module_by_base(h) : NULL;
    const char *p = m ? m->path : "/usr/libexec/minime/minime-engined";
    /* 按 Windows 习惯给 Z:\ 开头、反斜杠分隔的路径（和 Wine 一致） */
    snprintf(tmp, sizeof(tmp), "Z:%s", p);
    for (char *c = tmp; *c; c++)
        if (*c == '/')
            *c = '\\';
    size_t len = strlen(tmp);
    if (len >= n) {
        memcpy(out, tmp, n ? n - 1 : 0);
        if (n)
            out[n - 1] = 0;
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return n;
    }
    memcpy(out, tmp, len + 1);
    return (DWORD)len;
}

static WINAPI DWORD k32_GetModuleFileNameA(HMODULE h, LPSTR out, DWORD n) { return module_file_name(h, out, n); }
static WINAPI DWORD k32_GetModuleFileNameW(HMODULE h, LPWSTR out, DWORD n)
{
    char tmp[1024];
    module_file_name(h, tmp, sizeof(tmp));
    long w = utf8_to_u16((const unsigned char *)tmp, (long)strlen(tmp), out, n);
    if ((DWORD)w >= n) {
        if (n)
            out[n - 1] = 0;
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return n;
    }
    out[w] = 0;
    return (DWORD)w;
}

/* RtlPcToFileHeader：地址所在模块的基址 */
static WINAPI void *k32_RtlPcToFileHeader(void *pc, void **base)
{
    pe_module *m = pe_module_by_addr(pc);
    *base = m ? m->base : NULL;
    return *base;
}

/* ================= 字符与区域：ANSI 代码页就当 UTF-8 ================= */
#define CP_UTF8 65001
static WINAPI UINT k32_GetACP(void) { return CP_UTF8; }
static WINAPI UINT k32_GetOEMCP(void) { return CP_UTF8; }
static WINAPI BOOL k32_IsValidCodePage(UINT cp) { return cp == CP_UTF8 || cp == 1252 || cp == 437 || cp == 0; }
static WINAPI BOOL k32_AreFileApisANSI(void) { return TRUE; }

typedef struct { UINT MaxCharSize; BYTE DefaultChar[2]; BYTE LeadByte[12]; } CPINFO;
static WINAPI BOOL k32_GetCPInfo(UINT cp, CPINFO *ci)
{
    memset(ci, 0, sizeof(*ci));
    ci->MaxCharSize = (cp == CP_UTF8 || cp == 0 || cp == 3) ? 4 : 1;
    ci->DefaultChar[0] = '?';
    return TRUE;
}

static int is_latin1_cp(UINT cp) { return cp == 1252 || cp == 28591 || cp == 437; }

static WINAPI int k32_MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR src, int n, LPWSTR dst, int cap)
{
    (void)flags;
    if (n < 0)
        n = (int)strlen(src) + 1;
    long w;
    if (is_latin1_cp(cp)) {
        w = n;
        if (dst) {
            if (cap < n) {
                set_last_error(ERROR_INSUFFICIENT_BUFFER);
                return 0;
            }
            for (int i = 0; i < n; i++)
                dst[i] = (unsigned char)src[i];
        }
        return (int)w;
    }
    w = utf8_to_u16((const unsigned char *)src, n, NULL, 0);
    if (!dst || !cap)
        return (int)w;
    if (w > cap) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    utf8_to_u16((const unsigned char *)src, n, dst, cap);
    return (int)w;
}

static WINAPI int k32_WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int n, LPSTR dst, int cap,
                                          LPCSTR defchar, BOOL *useddef)
{
    (void)flags; (void)defchar;
    if (useddef)
        *useddef = FALSE;
    if (n < 0)
        n = (int)u16len(src) + 1;
    char *s;
    size_t len;
    if (is_latin1_cp(cp)) {
        s = malloc(n + 1);
        for (int i = 0; i < n; i++)
            s[i] = src[i] < 256 ? (char)src[i] : '?';
        len = n;
    } else {
        s = u16_to_utf8_len(src, n, &len);
    }
    if (!dst || !cap) {
        free(s);
        return (int)len;
    }
    if ((int)len > cap) {
        free(s);
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(dst, s, len);
    free(s);
    return (int)len;
}

#define LCMAP_LOWERCASE 0x100
#define LCMAP_UPPERCASE 0x200
#define LCMAP_SORTKEY   0x400
static int lcmap(DWORD flags, LPCWSTR src, int n, LPWSTR dst, int cap)
{
    if (n < 0)
        n = (int)u16len(src) + 1;
    if (flags & LCMAP_SORTKEY) {
        /* 排序键：按 UTF-16 大端字节输出，最后一个 0（不精确，但可比较） */
        int need = n * 2 + 1;
        if (!cap)
            return need;
        if (cap < need)
            return 0;
        uint8_t *o = (uint8_t *)dst;
        for (int i = 0; i < n; i++) {
            o[2 * i] = src[i] >> 8;
            o[2 * i + 1] = src[i] & 0xff;
        }
        o[2 * n] = 0;
        return need;
    }
    if (!cap)
        return n;
    if (cap < n) {
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (int i = 0; i < n; i++) {
        WCHAR c = src[i];
        if (c < 128) {
            if (flags & LCMAP_LOWERCASE) c = (WCHAR)tolower(c);
            if (flags & LCMAP_UPPERCASE) c = (WCHAR)toupper(c);
        }
        dst[i] = c;
    }
    return n;
}
static WINAPI int k32_LCMapStringEx(LPCWSTR loc, DWORD flags, LPCWSTR src, int n, LPWSTR dst, int cap, void *a, void *b, LPVOID c)
{
    (void)loc; (void)a; (void)b; (void)c;
    return lcmap(flags, src, n, dst, cap);
}
static WINAPI int k32_LCMapStringW(DWORD lcid, DWORD flags, LPCWSTR src, int n, LPWSTR dst, int cap)
{
    (void)lcid;
    return lcmap(flags, src, n, dst, cap);
}

#define CT_CTYPE1 1
static WINAPI BOOL k32_GetStringTypeW(DWORD type, LPCWSTR src, int n, WORD *out)
{
    if (n < 0)
        n = (int)u16len(src) + 1;
    for (int i = 0; i < n; i++) {
        WCHAR c = src[i];
        WORD t = 0;
        if (type == CT_CTYPE1) {
            if (c < 128) {
                if (isupper(c)) t |= 0x001;
                if (islower(c)) t |= 0x002;
                if (isdigit(c)) t |= 0x004;
                if (isspace(c)) t |= 0x008;
                if (ispunct(c)) t |= 0x010;
                if (iscntrl(c)) t |= 0x020;
                if (c == ' ' || c == '\t') t |= 0x040;
                if (isxdigit(c)) t |= 0x080;
                if (isalpha(c)) t |= 0x100;
                t |= 0x200;
            } else {
                t = 0x100 | 0x200;
            }
        }
        out[i] = t;
    }
    return TRUE;
}

#define CSTR_LESS_THAN 1
#define CSTR_EQUAL 2
#define CSTR_GREATER_THAN 3
static int cmpstr(DWORD flags, LPCWSTR a, int na, LPCWSTR b, int nb)
{
    if (na < 0) na = (int)u16len(a);
    if (nb < 0) nb = (int)u16len(b);
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++) {
        WCHAR x = a[i], y = b[i];
        if ((flags & 1) && x < 128 && y < 128) {   /* NORM_IGNORECASE */
            x = (WCHAR)tolower(x);
            y = (WCHAR)tolower(y);
        }
        if (x != y)
            return x < y ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
    }
    return na == nb ? CSTR_EQUAL : (na < nb ? CSTR_LESS_THAN : CSTR_GREATER_THAN);
}
static WINAPI int k32_CompareStringEx(LPCWSTR loc, DWORD f, LPCWSTR a, int na, LPCWSTR b, int nb, void *x, void *y, LPVOID z)
{
    (void)loc; (void)x; (void)y; (void)z;
    return cmpstr(f, a, na, b, nb);
}
static WINAPI int k32_CompareStringW(DWORD lcid, DWORD f, LPCWSTR a, int na, LPCWSTR b, int nb)
{
    (void)lcid;
    return cmpstr(f, a, na, b, nb);
}

/* ================= 进程环境 ================= */
static WCHAR g_cmdline_w[] = { 'w', 'e', 't', 'y', 'p', 'e', '-', 'e', 'n', 'g', 'i', 'n', 'e', 'd', 0 };
static char g_cmdline_a[] = "minime-engined";
static WINAPI LPSTR k32_GetCommandLineA(void) { return g_cmdline_a; }
static WINAPI LPWSTR k32_GetCommandLineW(void) { return g_cmdline_w; }

static WINAPI LPWSTR k32_GetEnvironmentStringsW(void)
{
    size_t total = 1;
    for (char **e = environ; *e; e++)
        total += utf8_to_u16((const unsigned char *)*e, (long)strlen(*e), NULL, 0) + 1;
    WCHAR *block = malloc(total * sizeof(WCHAR)), *p = block;
    for (char **e = environ; *e; e++) {
        p += utf8_to_u16((const unsigned char *)*e, (long)strlen(*e), p, (long)total);
        *p++ = 0;
    }
    *p = 0;
    return block;
}
static WINAPI BOOL k32_FreeEnvironmentStringsW(LPWSTR p) { free(p); return TRUE; }

static WINAPI DWORD k32_GetEnvironmentVariableW(LPCWSTR name, LPWSTR out, DWORD n)
{
    char *a = u16_to_utf8(name, -1);
    const char *v = getenv(a);
    free(a);
    if (!v) {
        set_last_error(203);   /* ERROR_ENVVAR_NOT_FOUND */
        return 0;
    }
    long w = utf8_to_u16((const unsigned char *)v, (long)strlen(v), NULL, 0);
    if ((DWORD)w + 1 > n)
        return (DWORD)w + 1;
    utf8_to_u16((const unsigned char *)v, (long)strlen(v), out, n);
    out[w] = 0;
    return (DWORD)w;
}
static WINAPI BOOL k32_SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value)
{
    char *a = u16_to_utf8(name, -1), *b = value ? u16_to_utf8(value, -1) : NULL;
    int r = b ? setenv(a, b, 1) : unsetenv(a);
    free(a);
    free(b);
    return r == 0;
}

static WINAPI void k32_GetStartupInfoW(void *si)
{
    memset(si, 0, 104);
    *(DWORD *)si = 104;
}

static WINAPI void *k32_EncodePointer(void *p) { return p; }
static WINAPI void *k32_DecodePointer(void *p) { return p; }

static WINAPI BOOL k32_IsDebuggerPresent(void) { return FALSE; }

static WINAPI BOOL k32_IsProcessorFeaturePresent(DWORD f)
{
    switch (f) {
    case 6: case 10: case 13: return TRUE;            /* SSE、SSE2、SSE3 */
    case 23: return TRUE;                              /* PF_FASTFAIL_AVAILABLE */
    case 36: return __builtin_cpu_supports("ssse3");
    case 37: return __builtin_cpu_supports("sse4.1");
    case 38: return __builtin_cpu_supports("sse4.2");
    case 39: return __builtin_cpu_supports("avx");
    case 40: return __builtin_cpu_supports("avx2");
    case 41: return __builtin_cpu_supports("avx512f");
    }
    return FALSE;
}

static WINAPI void k32_OutputDebugStringA(LPCSTR s) { if (getenv("MINIME_LOADER_VERBOSE")) fprintf(stderr, "[debug] %s", s); }
static WINAPI void k32_OutputDebugStringW(LPCWSTR s)
{
    if (!getenv("MINIME_LOADER_VERBOSE"))
        return;
    char *a = u16_to_utf8(s, -1);
    fprintf(stderr, "[debug] %s", a);
    free(a);
}

static WINAPI void *k32_SetUnhandledExceptionFilter(void *f) { (void)f; return NULL; }
static WINAPI LONG k32_UnhandledExceptionFilter(void *e) { (void)e; return 0; }

static WINAPI void k32_RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args)
{
    (void)flags;
    fprintf(stderr, "[loader] RaiseException(%#x)%s，异常展开尚未实现，中止\n", code,
            code == 0xE06D7363 ? "：C++ 异常" : "");
    if (code == 0xE06D7363 && n >= 3) {
        fprintf(stderr, "[loader]   抛出对象 %#lx，类型信息 %#lx\n", (unsigned long)args[1], (unsigned long)args[2]);
        /* MSVC x64：args[2] 是 ThrowInfo，args[3] 是模块基址；取第一个可捕获类型的名字 */
        if (n >= 4 && args[2] && args[3]) {
            const uint8_t *base = (const uint8_t *)args[3];
            const int32_t *ti = (const int32_t *)args[2];
            const int32_t *cta = (const int32_t *)(base + ti[3]);
            const int32_t *ct = (const int32_t *)(base + cta[1]);
            fprintf(stderr, "[loader]   类型 %s\n", (const char *)(base + ct[1] + 16));
        }
        /* std::exception 的布局是 {vptr, const char *what, bool do_free} */
        const char *what = args[1] ? *(const char **)(args[1] + 8) : NULL;
        if (what && pe_module_by_addr((void *)what) == NULL && (uintptr_t)what > 0x10000)
            fprintf(stderr, "[loader]   what(): %.300s\n", what);
        /* 引擎自己的异常类把消息放在别处（常见是对象里的 MSVC std::string：
         * 容量 > 15 时前 8 字节是堆指针，否则内联）。扫对象前 0x80 字节，打印像文本的串。 */
        if (args[1]) {
            const uint8_t *o = (const uint8_t *)args[1];
            for (int off = 8; off < 0x80; off += 8) {
                const char *cands[2] = { (const char *)o + off, *(const char *const *)(o + off) };
                for (int k = 0; k < 2; k++) {
                    const char *t = cands[k];
                    if ((uintptr_t)t < 0x10000 || (k == 1 && (msync((void *)((uintptr_t)t & ~4095UL), 4096, MS_ASYNC) != 0)))
                        continue;
                    int n = 0;
                    while (n < 300 && t[n] && ((unsigned char)t[n] >= 0x20 || t[n] == '\n'))
                        n++;
                    if (n >= 6 && !t[n])
                        fprintf(stderr, "[loader]   消息(+%#x%s): %.*s\n", off, k ? " 指针" : "", n, t);
                }
            }
        }
    }
    pe_dump_stack(__builtin_frame_address(0), 16);
    abort();
}

static WINAPI BOOL k32_TerminateProcess(HANDLE h, UINT code) { (void)h; _exit((int)code); }
static WINAPI void k32_ExitProcess(UINT code) { exit((int)code); }

/* ================= 时间 ================= */
static WINAPI BOOL k32_QueryPerformanceCounter(int64_t *c) { *c = (int64_t)now_ns(CLOCK_MONOTONIC); return TRUE; }
static WINAPI BOOL k32_QueryPerformanceFrequency(int64_t *f) { *f = 1000000000LL; return TRUE; }
static WINAPI ULONGLONG k32_GetTickCount64(void) { return now_ns(CLOCK_MONOTONIC) / 1000000ULL; }
static WINAPI DWORD k32_GetTickCount(void) { return (DWORD)(now_ns(CLOCK_MONOTONIC) / 1000000ULL); }

static WINAPI void k32_GetSystemTimeAsFileTime(uint64_t *ft) { *ft = now_ns(CLOCK_REALTIME) / 100 + FILETIME_UNIX_DIFF; }

typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
static void fill_systemtime(SYSTEMTIME *st, int local)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    if (local)
        localtime_r(&ts.tv_sec, &tm);
    else
        gmtime_r(&ts.tv_sec, &tm);
    st->wYear = (WORD)(tm.tm_year + 1900);
    st->wMonth = (WORD)(tm.tm_mon + 1);
    st->wDayOfWeek = (WORD)tm.tm_wday;
    st->wDay = (WORD)tm.tm_mday;
    st->wHour = (WORD)tm.tm_hour;
    st->wMinute = (WORD)tm.tm_min;
    st->wSecond = (WORD)tm.tm_sec;
    st->wMilliseconds = (WORD)(ts.tv_nsec / 1000000);
}
static WINAPI void k32_GetLocalTime(SYSTEMTIME *st) { fill_systemtime(st, 1); }
static WINAPI void k32_GetSystemTime(SYSTEMTIME *st) { fill_systemtime(st, 0); }

static WINAPI BOOL k32_FileTimeToSystemTime(const uint64_t *ft, SYSTEMTIME *st)
{
    int64_t ns100 = (int64_t)*ft - (int64_t)FILETIME_UNIX_DIFF;
    time_t t = ns100 / 10000000;
    struct tm tm;
    gmtime_r(&t, &tm);
    st->wYear = (WORD)(tm.tm_year + 1900);
    st->wMonth = (WORD)(tm.tm_mon + 1);
    st->wDayOfWeek = (WORD)tm.tm_wday;
    st->wDay = (WORD)tm.tm_mday;
    st->wHour = (WORD)tm.tm_hour;
    st->wMinute = (WORD)tm.tm_min;
    st->wSecond = (WORD)tm.tm_sec;
    st->wMilliseconds = (WORD)((ns100 / 10000) % 1000);
    return TRUE;
}

static WINAPI DWORD k32_GetTimeZoneInformation(void *tzi)
{
    memset(tzi, 0, 172);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    *(LONG *)tzi = (LONG)(-tm.tm_gmtoff / 60);   /* Bias，分钟，UTC = 本地 + Bias */
    return 0;                                     /* TIME_ZONE_ID_UNKNOWN */
}

/* ================= 系统信息 ================= */
static DWORD ncpu(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (DWORD)n : 1; }

static WINAPI void k32_GetSystemInfo(uint8_t *si)
{
    memset(si, 0, 48);
    *(WORD *)(si + 0) = 9;                       /* PROCESSOR_ARCHITECTURE_AMD64 */
    *(DWORD *)(si + 4) = 4096;
    *(uint64_t *)(si + 8) = 0x10000;
    *(uint64_t *)(si + 16) = 0x7ffffffeffffULL;
    *(uint64_t *)(si + 24) = ncpu() >= 64 ? UINT64_MAX : ((1ULL << ncpu()) - 1);
    *(DWORD *)(si + 32) = ncpu();
    *(DWORD *)(si + 36) = 8664;
    *(DWORD *)(si + 40) = 65536;
    *(WORD *)(si + 44) = 6;
}

static WINAPI DWORD k32_GetMaximumProcessorCount(WORD g) { (void)g; return ncpu(); }
static WINAPI DWORD k32_GetActiveProcessorCount(WORD g) { (void)g; return ncpu(); }
static WINAPI WORD k32_GetActiveProcessorGroupCount(void) { return 1; }
static WINAPI WORD k32_GetMaximumProcessorGroupCount(void) { return 1; }
static WINAPI BOOL k32_GetProcessAffinityMask(HANDLE h, DWORD_PTR *pm, DWORD_PTR *sm)
{
    (void)h;
    DWORD_PTR m = ncpu() >= 64 ? UINT64_MAX : ((1ULL << ncpu()) - 1);
    *pm = m;
    *sm = m;
    return TRUE;
}

/* 旧接口：每个逻辑处理器报一个核 */
static WINAPI BOOL k32_GetLogicalProcessorInformation(uint8_t *buf, DWORD *len)
{
    if (getenv("MINIME_LOADER_VERBOSE"))
        fprintf(stderr, "[loader] GetLogicalProcessorInformation(len=%u)\n", *len);
    DWORD n = ncpu(), need = n * 32;
    if (*len < need) {
        *len = need;
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(buf, 0, need);
    for (DWORD i = 0; i < n; i++) {
        *(uint64_t *)(buf + i * 32) = 1ULL << (i & 63);   /* ProcessorMask */
        *(int32_t *)(buf + i * 32 + 8) = 0;        /* RelationProcessorCore */
    }
    *len = need;
    return TRUE;
}

/* 新接口：核（0）、封装（3）、组（4）；缓存和 NUMA 不报 */
static WINAPI BOOL k32_GetLogicalProcessorInformationEx(int rel, uint8_t *buf, DWORD *len)
{
    if (getenv("MINIME_LOADER_VERBOSE"))
        fprintf(stderr, "[loader] GetLogicalProcessorInformationEx(rel=%d, len=%u)\n", rel, *len);
    DWORD n = ncpu();
    int want_core = rel == 0 || rel == 0xffff, want_pkg = rel == 3 || rel == 0xffff, want_grp = rel == 4 || rel == 0xffff;
    DWORD need = (want_core ? n * 48 : 0) + (want_pkg ? 48 : 0) + (want_grp ? 80 : 0);
    if (*len < need) {
        *len = need;
        set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(buf, 0, need);
    uint8_t *p = buf;
    uint64_t all = n >= 64 ? UINT64_MAX : ((1ULL << n) - 1);
    if (want_core)
        for (DWORD i = 0; i < n; i++, p += 48) {
            *(int32_t *)p = 0;
            *(DWORD *)(p + 4) = 48;
            *(WORD *)(p + 8 + 22) = 1;            /* GroupCount */
            *(uint64_t *)(p + 8 + 24) = 1ULL << (i & 63);  /* GroupMask[0].Mask */
        }
    if (want_pkg) {
        *(int32_t *)p = 3;
        *(DWORD *)(p + 4) = 48;
        *(WORD *)(p + 8 + 22) = 1;
        *(uint64_t *)(p + 8 + 24) = all;
        p += 48;
    }
    if (want_grp) {
        /* GROUP_RELATIONSHIP：MaximumGroupCount、ActiveGroupCount、Reserved[20]、GroupInfo[1]
         * PROCESSOR_GROUP_INFO：MaximumProcessorCount(1B)、ActiveProcessorCount(1B)、Reserved[38]、ActiveProcessorMask(8) */
        *(int32_t *)p = 4;
        *(DWORD *)(p + 4) = 80;
        *(WORD *)(p + 8) = 1;
        *(WORD *)(p + 10) = 1;
        uint8_t *gi = p + 8 + 24;
        gi[0] = (uint8_t)n;
        gi[1] = (uint8_t)n;
        *(uint64_t *)(gi + 40) = all;
    }
    *len = need;
    return TRUE;
}

static WINAPI BOOL k32_GlobalMemoryStatusEx(uint8_t *ms)
{
    uint64_t total = 0, avail = 0;
#if defined(__linux__)
    struct sysinfo si;
    sysinfo(&si);
    total = (uint64_t)si.totalram * si.mem_unit;
    avail = (uint64_t)si.freeram * si.mem_unit;
#elif defined(__FreeBSD__)
    size_t sz = sizeof(total);
    if (sysctlbyname("hw.physmem", &total, &sz, NULL, 0) != 0) {
        long pages = sysconf(_SC_PHYS_PAGES);
        long psize = sysconf(_SC_PAGESIZE);
        total = (pages > 0 && psize > 0) ? (uint64_t)pages * (uint64_t)psize : (4ULL << 30);
    }
    unsigned int v_free = 0, v_inact = 0;
    size_t fsz = sizeof(v_free), isz = sizeof(v_inact);
    long psize = sysconf(_SC_PAGESIZE);
    if (psize > 0 &&
        sysctlbyname("vm.stats.vm.v_free_count", &v_free, &fsz, NULL, 0) == 0 &&
        sysctlbyname("vm.stats.vm.v_inactive_count", &v_inact, &isz, NULL, 0) == 0) {
        avail = ((uint64_t)v_free + (uint64_t)v_inact) * (uint64_t)psize;
    } else {
        avail = total / 2;
    }
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    total = (pages > 0 && psize > 0) ? (uint64_t)pages * (uint64_t)psize : (4ULL << 30);
    avail = total / 2;
#endif
    if (avail > total)
        avail = total;
    *(DWORD *)(ms + 4) = total ? (DWORD)(100 - avail * 100 / total) : 0;
    *(uint64_t *)(ms + 8) = total;
    *(uint64_t *)(ms + 16) = avail;
    *(uint64_t *)(ms + 24) = total;
    *(uint64_t *)(ms + 32) = avail;
    *(uint64_t *)(ms + 40) = 0x7ffffffe0000ULL;
    *(uint64_t *)(ms + 48) = 0x7ffffffe0000ULL;
    return TRUE;
}

static WINAPI BOOL k32_GetSystemTimes(uint64_t *idle, uint64_t *kernel, uint64_t *user)
{
    if (idle) *idle = 0;
    if (kernel) *kernel = 0;
    if (user) *user = 0;
    return TRUE;
}

static WINAPI BOOL k32_VerifyVersionInfoW(void *vi, DWORD mask, ULONGLONG cond) { (void)vi; (void)mask; (void)cond; return TRUE; }
static WINAPI ULONGLONG k32_VerSetConditionMask(ULONGLONG m, DWORD t, BYTE c) { return m | ((ULONGLONG)c << ((t & 7) * 3)); }

static WINAPI DWORD k32_FormatMessageA(DWORD f, LPCVOID s, DWORD id, DWORD l, LPSTR b, DWORD n, void *a)
{
    (void)f; (void)s; (void)id; (void)l; (void)b; (void)n; (void)a;
    return 0;
}

/* ================= 其他 DLL ================= */
static WINAPI BOOL adv_SystemFunction036(void *buf, ULONG_PTR n)
{
#ifdef __linux__
    return getrandom(buf, n, 0) == (ssize_t)n;
#else
    arc4random_buf(buf, n);
    return TRUE;
#endif
}
static WINAPI LONG adv_RegOpenKeyExW(HANDLE k, LPCWSTR s, DWORD o, DWORD a, HANDLE *r) { (void)k; (void)s; (void)o; (void)a; if (r) *r = NULL; return ERROR_FILE_NOT_FOUND; }
static WINAPI LONG adv_RegQueryValueExW(HANDLE k, LPCWSTR n, DWORD *r, DWORD *t, BYTE *d, DWORD *l) { (void)k; (void)n; (void)r; (void)t; (void)d; (void)l; return ERROR_FILE_NOT_FOUND; }
static WINAPI LONG adv_RegGetValueW(HANDLE k, LPCWSTR s, LPCWSTR v, DWORD f, DWORD *t, void *d, DWORD *l) { (void)k; (void)s; (void)v; (void)f; (void)t; (void)d; (void)l; return ERROR_FILE_NOT_FOUND; }
static WINAPI LONG adv_RegCloseKey(HANDLE k) { (void)k; return 0; }
static WINAPI LONG pdh_fail(void) { return (LONG)0xC0000BB8; }   /* PDH_CSTATUS_NO_OBJECT */
static WINAPI int ws2_WSAStartup(WORD ver, void *data) { (void)ver; memset(data, 0, 408); return 0; }

/* ================= 名字表 ================= */
#define S(n) { #n, (void *)k32_##n }
static const Shim g_shims[] = {
    S(GetLastError), S(SetLastError),
    S(GetProcessHeap), S(HeapAlloc), S(HeapFree), S(HeapSize), S(HeapReAlloc), S(HeapValidate),
    S(HeapQueryInformation), S(LocalFree),
    S(VirtualAlloc), S(VirtualFree), S(VirtualProtect),
    S(TlsAlloc), S(TlsFree), S(TlsGetValue), S(TlsSetValue),
    S(FlsAlloc), S(FlsFree), S(FlsGetValue), S(FlsSetValue),
    S(InitializeCriticalSection), S(InitializeCriticalSectionAndSpinCount), S(InitializeCriticalSectionEx),
    S(DeleteCriticalSection), S(EnterCriticalSection), S(TryEnterCriticalSection), S(LeaveCriticalSection),
    S(InitializeSRWLock), S(AcquireSRWLockExclusive), S(TryAcquireSRWLockExclusive), S(ReleaseSRWLockExclusive),
    S(AcquireSRWLockShared), S(TryAcquireSRWLockShared), S(ReleaseSRWLockShared),
    S(InitializeConditionVariable), S(WakeConditionVariable), S(WakeAllConditionVariable),
    S(SleepConditionVariableSRW), S(SleepConditionVariableCS),
    S(InitOnceBeginInitialize), S(InitOnceComplete), S(InitOnceExecuteOnce),
    S(InitializeSListHead), S(InterlockedPushEntrySList), S(InterlockedPopEntrySList), S(InterlockedFlushSList),
    S(CloseHandle), S(CreateEventW), S(CreateEventA), S(CreateEventExW), S(SetEvent), S(ResetEvent),
    S(CreateSemaphoreW), S(CreateSemaphoreExW), S(ReleaseSemaphore), S(CreateMutexW), S(ReleaseMutex),
    S(CreateWaitableTimerW), S(CreateWaitableTimerA), S(SetWaitableTimer), S(CancelWaitableTimer),
    S(WaitForSingleObject), S(WaitForSingleObjectEx), S(WaitForMultipleObjects), S(WaitForMultipleObjectsEx),
    S(Sleep), S(SleepEx), S(SwitchToThread), S(QueueUserAPC),
    S(CreateThread), S(ResumeThread), S(ExitThread), S(FreeLibraryAndExitThread), S(GetExitCodeThread),
    S(GetCurrentThread), S(GetCurrentThreadId), S(GetCurrentProcess), S(GetCurrentProcessId),
    S(GetCurrentProcessorNumber), S(SetThreadAffinityMask), S(SetThreadPriority), S(GetThreadPriority),
    S(CreateIoCompletionPort), S(PostQueuedCompletionStatus), S(GetQueuedCompletionStatus),
    S(GetStdHandle), S(SetStdHandle), S(GetFileType), S(WriteFile), S(ReadFile), S(GetConsoleMode),
    S(SetConsoleMode), S(FlushFileBuffers),
    S(GetModuleHandleA), S(GetModuleHandleW), S(GetModuleHandleExW), S(LoadLibraryA), S(LoadLibraryExA),
    S(LoadLibraryW), S(LoadLibraryExW), S(FreeLibrary), S(GetProcAddress), S(GetModuleFileNameA),
    S(GetModuleFileNameW), S(RtlPcToFileHeader),
    S(GetACP), S(GetOEMCP), S(IsValidCodePage), S(AreFileApisANSI), S(GetCPInfo), S(MultiByteToWideChar),
    S(WideCharToMultiByte), S(LCMapStringEx), S(LCMapStringW), S(GetStringTypeW), S(CompareStringEx),
    S(CompareStringW),
    S(GetCommandLineA), S(GetCommandLineW), S(GetEnvironmentStringsW), S(FreeEnvironmentStringsW),
    S(GetEnvironmentVariableW), S(SetEnvironmentVariableW), S(GetStartupInfoW), S(EncodePointer),
    S(DecodePointer), S(IsDebuggerPresent), S(IsProcessorFeaturePresent), S(OutputDebugStringA),
    S(OutputDebugStringW), S(SetUnhandledExceptionFilter), S(UnhandledExceptionFilter), S(RaiseException),
    S(TerminateProcess), S(ExitProcess),
    S(QueryPerformanceCounter), S(QueryPerformanceFrequency), S(GetTickCount64), S(GetTickCount),
    S(GetSystemTimeAsFileTime), S(GetLocalTime), S(GetSystemTime), S(FileTimeToSystemTime),
    S(GetTimeZoneInformation),
    S(GetSystemInfo), S(GetMaximumProcessorCount), S(GetActiveProcessorCount), S(GetActiveProcessorGroupCount),
    S(GetMaximumProcessorGroupCount), S(GetProcessAffinityMask), S(GetLogicalProcessorInformation),
    S(GetLogicalProcessorInformationEx), S(GlobalMemoryStatusEx), S(GetSystemTimes), S(VerifyVersionInfoW),
    S(VerSetConditionMask), S(FormatMessageA),
    { "GetNativeSystemInfo", (void *)k32_GetSystemInfo },
    { "GetSystemTimePreciseAsFileTime", (void *)k32_GetSystemTimeAsFileTime },
    { "VerifyVersionInfoA", (void *)k32_VerifyVersionInfoW },
    { "RtlEncodePointer", (void *)k32_EncodePointer },
    { "RtlDecodePointer", (void *)k32_DecodePointer },
    { "SystemFunction036", (void *)adv_SystemFunction036 },
    { "RegOpenKeyExW", (void *)adv_RegOpenKeyExW },
    { "RegQueryValueExW", (void *)adv_RegQueryValueExW },
    { "RegGetValueW", (void *)adv_RegGetValueW },
    { "RegCloseKey", (void *)adv_RegCloseKey },
    { "PdhOpenQueryA", (void *)pdh_fail },
    { "PdhAddEnglishCounterW", (void *)pdh_fail },
    { "PdhCollectQueryData", (void *)pdh_fail },
    { "WSAStartup", (void *)ws2_WSAStartup },
    /* WS2_32 大多按序号导入：115=WSAStartup，116=WSACleanup */
    { "#115", (void *)ws2_WSAStartup },
    { "WSACleanup", (void *)adv_RegCloseKey },
    { "#116", (void *)adv_RegCloseKey },
};

void *shim_lookup(const char *name)
{
    for (size_t i = 0; i < sizeof(g_shims) / sizeof(g_shims[0]); i++)
        if (!strcmp(g_shims[i].name, name))
            return g_shims[i].fn;
    for (size_t i = 0; i < g_file_shims_n; i++)
        if (!strcmp(g_file_shims[i].name, name))
            return g_file_shims[i].fn;
    return NULL;
}
