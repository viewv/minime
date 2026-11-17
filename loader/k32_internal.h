/* k32_internal.h — 兼容层各源文件共享的定义（不给 PE 代码用） */
#ifndef MINIME_K32_INTERNAL_H
#define MINIME_K32_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "winabi.h"

enum { KO_EVENT = 1, KO_SEMAPHORE, KO_MUTEX, KO_TIMER, KO_THREAD, KO_IOCP, KO_FILE, KO_FIND, KO_MAPPING };

typedef struct Apc { void (WINAPI *fn)(ULONG_PTR); ULONG_PTR arg; struct Apc *next; } Apc;
typedef struct Completion { DWORD bytes; ULONG_PTR key; void *ov; struct Completion *next; } Completion;

typedef struct KObj {
    uint32_t magic;
    int type;
    _Atomic int refs;
    /* 可等待对象的状态由全局锁 g_ko_lock 保护 */
    int signaled, manual;
    int count, max;                 /* 信号量 */
    uint64_t owner; int recursion;  /* 互斥体 */
    uint64_t due_ns;                /* 定时器（CLOCK_MONOTONIC），0 表示未设置 */
    uint32_t period_ms;
    /* 线程 */
    pthread_t th;
    DWORD tid, exit_code;
    void *start, *param;
    int suspended;
    Apc *apcs;
    /* 完成端口 */
    pthread_mutex_t qmu;
    pthread_cond_t qcv;
    Completion *qhead, *qtail;
    /* 文件、目录查找、文件映射 */
    int fd;
    char *path;                     /* 打开时的 Linux 路径（删除、改名、查询用） */
    int delete_on_close;
    void *dir;                      /* 目录查找：DIR* */
    char *pattern;                  /* 目录查找：通配模式 */
    uint64_t map_size;              /* 文件映射：大小 */
    uint32_t map_prot;              /* 文件映射：页保护 */
} KObj;

#define KO_MAGIC 0x4b4f424au

KObj *ko_new(int type);
KObj *ko(HANDLE h);
WINAPI BOOL k32_CloseHandle(HANDLE h);
void find_close_obj(KObj *o);

uint64_t now_ns(int clock);
size_t u16len(const WCHAR *s);
char *u16_to_utf8(const WCHAR *s, long n);
long utf8_to_u16(const unsigned char *s, long n, WCHAR *dst, long cap);
char *win_path_to_unix(const char *p);

#define FILETIME_UNIX_DIFF 116444736000000000ULL

typedef struct { const char *name; void *fn; } Shim;
extern const Shim g_file_shims[];
extern const size_t g_file_shims_n;

#endif
