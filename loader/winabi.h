/* winabi.h — 原生装载器里用到的 Windows 类型、常量和 TEB 访问
 *
 * 所有给 PE 代码调用的函数都必须标 WINAPI（ms_abi）。
 * WCHAR 是 16 位，不能用 Linux 的 wchar_t（32 位）。
 */
#ifndef MINIME_WINABI_H
#define MINIME_WINABI_H

#include <stddef.h>
#include <stdint.h>

#define WINAPI __attribute__((ms_abi))

typedef int32_t  BOOL;
typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t  LONG;
typedef uint32_t UINT;
typedef uint64_t ULONGLONG;
typedef int64_t  LONGLONG;
typedef uint64_t ULONG_PTR;
typedef uint64_t SIZE_T;
typedef uint64_t DWORD_PTR;
typedef uint16_t WCHAR;
typedef void    *HANDLE;
typedef void    *HMODULE;
typedef void    *LPVOID;
typedef const void *LPCVOID;
typedef char    *LPSTR;
typedef const char *LPCSTR;
typedef WCHAR   *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef void (*FARPROC)(void);

#define TRUE  1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)

/* 错误码 */
#define ERROR_SUCCESS               0
#define ERROR_FILE_NOT_FOUND        2
#define ERROR_PATH_NOT_FOUND        3
#define ERROR_ACCESS_DENIED         5
#define ERROR_INVALID_HANDLE        6
#define ERROR_NOT_ENOUGH_MEMORY     8
#define ERROR_INVALID_PARAMETER     87
#define ERROR_INSUFFICIENT_BUFFER   122
#define ERROR_MOD_NOT_FOUND         126
#define ERROR_PROC_NOT_FOUND        127
#define ERROR_ALREADY_EXISTS        183
#define ERROR_NO_MORE_FILES         18
#define ERROR_CALL_NOT_IMPLEMENTED  120
#define ERROR_TIMEOUT               1460
#define WAIT_OBJECT_0               0
#define WAIT_TIMEOUT                258
#define WAIT_FAILED                 0xffffffffu
#define INFINITE                    0xffffffffu

/* ---- TEB 布局（x64，只列用到的字段） ----
 * gs 指向每个线程自己的 TEB；PE 代码直接用 gs:[偏移] 读这些字段。 */
#define TEB_STACK_BASE     0x08
#define TEB_STACK_LIMIT    0x10
#define TEB_SELF           0x30
#define TEB_PID            0x40
#define TEB_TID            0x48
#define TEB_TLS_POINTER    0x58   /* 各模块静态 TLS 块的指针数组 */
#define TEB_PEB            0x60
#define TEB_LAST_ERROR     0x68
#define TEB_TLS_SLOTS      0x1480 /* TlsAlloc 的 64 个槽 */
#define TEB_FLS_DATA       0x17c8 /* 我们自己的 FLS 数组指针（真实 Windows 里也在这个位置） */
#define TEB_SIZE           0x2000
#define TLS_MINIMUM_AVAILABLE 64

static inline uint8_t *cur_teb(void)
{
    uint8_t *t;
    __asm__ volatile("mov %%gs:0x30, %0" : "=r"(t));
    return t;
}

#define TEB_FIELD(type, off) (*(type *)(cur_teb() + (off)))

static inline void set_last_error(DWORD e)
{
    __asm__ volatile("movl %0, %%gs:0x68" : : "r"(e) : "memory");
}

static inline DWORD get_last_error(void)
{
    DWORD e;
    __asm__ volatile("movl %%gs:0x68, %0" : "=r"(e));
    return e;
}

#endif
