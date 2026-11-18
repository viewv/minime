/* pe.h — 原生 PE 装载器
 *
 * 把 Windows x64 DLL 映射进 Linux 进程：节映射、重定位、导入绑定（系统 DLL 绑到兼容层，
 * 同目录的 DLL 递归装载）、静态 TLS、TLS 回调和 DllMain。
 * 每个调用 PE 代码的线程都要先有 TEB（pe_thread_attach）。
 */
#ifndef MINIME_PE_H
#define MINIME_PE_H

#include <stddef.h>
#include <stdint.h>

#include "winabi.h"

typedef struct pe_module {
    char      name[64];        /* 文件名，如 tensorflowlite_c.dll */
    char      path[512];
    uint8_t  *base;
    size_t    size;
    uint32_t  entry_rva;
    uint32_t  export_rva, export_size;
    /* 静态 TLS */
    int       tls_index;       /* -1 表示没有 TLS */
    uint8_t  *tls_template;
    size_t    tls_template_size;
    size_t    tls_zero_fill;
    uint64_t *tls_callbacks;   /* 以 0 结尾 */
    struct pe_module *next;
} pe_module;

/* 装载一个 DLL（及其同目录依赖）并完成初始化（TLS 回调、DllMain）。失败返回 NULL。 */
pe_module *pe_load(const char *path);
/* 宿主提供的 DLL：之后装载的模块导入它时，不读文件，用 lookup(函数名) 的结果（返回 NULL 则给报告桩） */
void pe_register_host_dll(const char *dll, void *(*lookup)(const char *name));

/* 按名字找导出；找不到返回 NULL。 */
void *pe_export(pe_module *m, const char *name);

/* 把模块 m 从 dll 导入的 fn 换成 newfn（调试用）；返回原地址，没找到返回 NULL。 */
void *pe_patch_import(pe_module *m, const char *dll, const char *fn, void *newfn);

/* 按基址找模块（HMODULE 就是基址）。 */
pe_module *pe_module_by_base(const void *base);
pe_module *pe_module_by_name(const char *name);
pe_module *pe_module_by_addr(const void *addr);

/* 让当前线程可以调用 PE 代码：分配并安装 TEB、静态 TLS。重复调用无害。 */
void pe_thread_attach(void);

/* 进程初始化：主线程 TEB、PEB、崩溃时打印模块+偏移。pe_load 会自动调用。 */
void pe_process_init(void);

/* 兼容层：按名字查找实现（kernel32 等系统 DLL 的函数）。没有实现返回 NULL。 */
void *shim_lookup(const char *name);

/* 线程创建时由兼容层调用：给新线程建 TEB 并对所有模块发 DLL_THREAD_ATTACH。 */
void pe_thread_start_hooks(void);
void pe_thread_exit_hooks(void);

/* 从 sp 往上扫栈，打印落在 PE 模块里的返回地址（粗略回溯） */
void pe_dump_stack(void *sp, int max);

#endif
