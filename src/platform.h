/* platform.h — 宿主的平台层：同一份宿主代码既能用 mingw 编成 Windows 程序（在 Wine 下跑），
 * 也能在 Linux 上原生编译、通过 loader/ 的原生装载器装载引擎。
 *
 * 宿主里和平台有关的只有这些：装载引擎、取导出、建目录、时间、事件、改代码字节。
 */
#ifndef WXIME_PLATFORM_H
#define WXIME_PLATFORM_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

#define PLAT_SEP '\\'
typedef HMODULE plat_lib;

static void plat_fix_sep(char *s)
{
    for (; *s; s++)
        if (*s == '/')
            *s = '\\';
}

static plat_lib plat_load_engine(const char *install_dir)
{
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\libwxhld.dll", install_dir);
    plat_fix_sep(p);
    /* 同目录的 XNet.dll / tensorflowlite_c.dll / ime_net.dll 按 DLL 所在目录查找 */
    return LoadLibraryExA(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}
static void *plat_sym(plat_lib h, const char *n) { return (void *)GetProcAddress(h, n); }
static uint8_t *plat_base(plat_lib h) { return (uint8_t *)h; }
static void plat_mkdir(const char *p) { CreateDirectoryA(p, NULL); }
static void plat_sleep_ms(int ms) { Sleep(ms); }
static unsigned long plat_last_error(void) { return GetLastError(); }

/* 引擎的时间戳和官方 server 一致：1601 年起的毫秒数 */
static uint64_t plat_wall_ms(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000;
}

static double plat_now_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
}

typedef HANDLE plat_event;
static plat_event plat_event_new(void) { return CreateEventA(NULL, FALSE, FALSE, NULL); }
static void plat_event_set(plat_event e) { SetEvent(e); }
static void plat_event_reset(plat_event e) { ResetEvent(e); }
static void plat_event_wait(plat_event e, int ms) { WaitForSingleObject(e, (DWORD)ms); }

static int plat_patch(void *addr, const void *bytes, size_t n)
{
    DWORD old;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old))
        return -1;
    memcpy(addr, bytes, n);
    VirtualProtect(addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    return 0;
}

#else  /* Linux 原生 */
#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../loader/pe.h"

#define PLAT_SEP '/'
#ifndef MAX_PATH
#define MAX_PATH 4096
#endif
typedef pe_module *plat_lib;

static void plat_fix_sep(char *s) { (void)s; }

static plat_lib plat_load_engine(const char *install_dir)
{
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s/libwxhld.dll", install_dir);
    /* ime_net.dll（网络、登录）及其依赖不装载，导入全部给桩 */
    setenv("MINIME_STUB_DLLS", "ime_net.dll", 0);
    pe_module *m = pe_load(p);
    if (m) {
        /* 兼容处理：Windows 安装包不带引擎内置的整句组件，让引擎在创建会话时跳过它
         * （和官方 Windows 客户端的行为一致）。只在内存里处理，磁盘上的官方文件不改；
         * 只对 3.0.1.35 生效，别的版本不做处理。 */
        enum { RVA = 0x21e190 };
        uint8_t *at = m->base + RVA;
        if ((size_t)RVA + 2 <= m->size && at[0] == 0x75 && at[1] == 0x7f) {
            long pg = sysconf(_SC_PAGESIZE);
            uint8_t *page = (uint8_t *)((uintptr_t)at & ~(uintptr_t)(pg - 1));
            if (mprotect(page, (size_t)pg * 2, PROT_READ | PROT_WRITE) == 0) {
                at[0] = 0xeb;
                mprotect(page, (size_t)pg * 2, PROT_READ | PROT_EXEC);
            } else {
                fprintf(stderr, "[engine] 整句组件兼容处理失败（mprotect）\n");
            }
        } else if (!((size_t)RVA + 2 <= m->size && at[0] == 0xeb && at[1] == 0x7f)) {
            fprintf(stderr, "[engine] 引擎版本不是 3.0.1.35（+0x%x），没有做整句组件兼容处理\n", RVA);
        }
    }
    return m;
}
static void *plat_sym(plat_lib h, const char *n) { return pe_export(h, n); }
static uint8_t *plat_base(plat_lib h) { return h->base; }
static void plat_mkdir(const char *p) { mkdir(p, 0755); }
static void plat_sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}
static unsigned long plat_last_error(void) { return (unsigned long)errno; }

static uint64_t plat_wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000 + 11644473600000ULL;
}

static double plat_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

typedef struct { pthread_mutex_t mu; pthread_cond_t cv; int set; } *plat_event;
static plat_event plat_event_new(void)
{
    plat_event e = calloc(1, sizeof(*e));
    pthread_mutex_init(&e->mu, NULL);
    pthread_cond_init(&e->cv, NULL);
    return e;
}
static void plat_event_set(plat_event e)
{
    pthread_mutex_lock(&e->mu);
    e->set = 1;
    pthread_cond_signal(&e->cv);
    pthread_mutex_unlock(&e->mu);
}
static void plat_event_reset(plat_event e)
{
    pthread_mutex_lock(&e->mu);
    e->set = 0;
    pthread_mutex_unlock(&e->mu);
}
static void plat_event_wait(plat_event e, int ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&e->mu);
    while (!e->set)
        if (pthread_cond_timedwait(&e->cv, &e->mu, &ts) == ETIMEDOUT)
            break;
    e->set = 0;   /* 自动复位 */
    pthread_mutex_unlock(&e->mu);
}

static int plat_patch(void *addr, const void *bytes, size_t n)
{
    uintptr_t a = (uintptr_t)addr & ~4095UL;
    size_t len = ((uintptr_t)addr + n - a + 4095) & ~4095UL;
    if (mprotect((void *)a, len, PROT_READ | PROT_WRITE) != 0)
        return -1;
    memcpy(addr, bytes, n);
    mprotect((void *)a, len, PROT_READ | PROT_EXEC);
    return 0;
}
#endif

#endif
