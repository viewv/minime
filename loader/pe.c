/* pe.c — 原生 PE 装载器：映射、重定位、导入、TLS、TEB */
#define _GNU_SOURCE
#include "pe.h"

#include <asm/prctl.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

/* ---------------- PE 结构（只列用到的） ---------------- */
#pragma pack(push, 1)
typedef struct { uint16_t e_magic; uint8_t pad[58]; int32_t e_lfanew; } DosHeader;
typedef struct {
    uint32_t Signature;
    uint16_t Machine, NumberOfSections;
    uint32_t TimeDateStamp, PointerToSymbolTable, NumberOfSymbols;
    uint16_t SizeOfOptionalHeader, Characteristics;
} FileHeader;
typedef struct { uint32_t VirtualAddress, Size; } DataDir;
typedef struct {
    uint16_t Magic; uint8_t MajorLinker, MinorLinker;
    uint32_t SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData, AddressOfEntryPoint, BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment, FileAlignment;
    uint16_t v[6];
    uint32_t Win32VersionValue, SizeOfImage, SizeOfHeaders, CheckSum;
    uint16_t Subsystem, DllCharacteristics;
    uint64_t StackReserve, StackCommit, HeapReserve, HeapCommit;
    uint32_t LoaderFlags, NumberOfRvaAndSizes;
    DataDir  Dir[16];
} OptHeader64;
typedef struct {
    char     Name[8];
    uint32_t VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData;
    uint32_t PtrReloc, PtrLine; uint16_t NReloc, NLine;
    uint32_t Characteristics;
} SectionHeader;
typedef struct {
    uint32_t OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk;
} ImportDesc;
typedef struct {
    uint32_t Characteristics, TimeDateStamp; uint16_t Major, Minor;
    uint32_t Name, Base, NumberOfFunctions, NumberOfNames;
    uint32_t AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals;
} ExportDir;
typedef struct {
    uint64_t StartAddressOfRawData, EndAddressOfRawData, AddressOfIndex, AddressOfCallBacks;
    uint32_t SizeOfZeroFill, Characteristics;
} TlsDir;
#pragma pack(pop)

#define DIR_EXPORT  0
#define DIR_IMPORT  1
#define DIR_RELOC   5
#define DIR_TLS     9

#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH  2
#define DLL_THREAD_DETACH  3

typedef BOOL (WINAPI *DllEntry)(void *hinst, DWORD reason, void *reserved);
typedef void (WINAPI *TlsCallback)(void *hinst, DWORD reason, void *reserved);

static pe_module *g_modules;
static pthread_mutex_t g_load_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_tls_count;
static uint8_t g_peb[0x1000] __attribute__((aligned(16)));

static int verbose(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("MINIME_LOADER_VERBOSE") != NULL;
    return v;
}
#define LOG(...) do { if (verbose()) fprintf(stderr, "[loader] " __VA_ARGS__); } while (0)

/* ---------------- 模块查找 ---------------- */
static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    const char *b = strrchr(p, '\\');
    if (b > s)
        s = b;
    return s ? s + 1 : p;
}

pe_module *pe_module_by_base(const void *base)
{
    for (pe_module *m = g_modules; m; m = m->next)
        if (m->base == base)
            return m;
    return NULL;
}

pe_module *pe_module_by_addr(const void *addr)
{
    for (pe_module *m = g_modules; m; m = m->next)
        if ((const uint8_t *)addr >= m->base && (const uint8_t *)addr < m->base + m->size)
            return m;
    return NULL;
}

pe_module *pe_module_by_name(const char *name)
{
    name = base_name(name);
    size_t n = strlen(name);
    for (pe_module *m = g_modules; m; m = m->next) {
        if (!strcasecmp(m->name, name))
            return m;
        /* 允许不带 .dll 后缀 */
        if (strlen(m->name) == n + 4 && !strncasecmp(m->name, name, n) && !strcasecmp(m->name + n, ".dll"))
            return m;
    }
    return NULL;
}

/* ---------------- 未实现导入的桩 ----------------
 * 每个未实现的导入生成一个 26 字节的桩：mov r10, <名字>; mov rax, <处理函数>; jmp rax。
 * 处理函数（stub.S）打印名字（同一个名字只打印一次）并返回 0。 */
extern void unknown_import_handler(void);

static uint8_t *g_thunk_page, *g_thunk_cur, *g_thunk_end;

static void *make_unknown_thunk(const char *dll, const char *name)
{
    if (!g_thunk_cur || g_thunk_cur + 32 > g_thunk_end) {
        size_t sz = 1 << 20;
        g_thunk_page = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (g_thunk_page == MAP_FAILED) {
            perror("mmap thunk");
            abort();
        }
        g_thunk_cur = g_thunk_page;
        g_thunk_end = g_thunk_page + sz;
    }
    char *label = malloc(strlen(dll) + strlen(name) + 2);
    sprintf(label, "%s!%s", dll, name);
    uint8_t *p = g_thunk_cur;
    void *handler = (void *)unknown_import_handler;
    *p++ = 0x49; *p++ = 0xBA; memcpy(p, &label, 8); p += 8;       /* mov r10, imm64 */
    *p++ = 0x48; *p++ = 0xB8; memcpy(p, &handler, 8); p += 8;     /* mov rax, imm64 */
    *p++ = 0xFF; *p++ = 0xE0;                                     /* jmp rax */
    void *ret = g_thunk_cur;
    g_thunk_cur += 32;
    return ret;
}

WINAPI void unknown_import_log(const char *label)
{
    /* 同一个函数只报一次，避免刷屏 */
    static const char *seen[512];
    static int nseen;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < nseen; i++)
        if (seen[i] == label) {
            pthread_mutex_unlock(&mu);
            return;
        }
    if (nseen < 512)
        seen[nseen++] = label;
    pthread_mutex_unlock(&mu);
    fprintf(stderr, "[loader] 调用了未实现的函数 %s（返回 0）\n", label);
}

/* ---------------- 系统 DLL 判断 ---------------- */
/* 宿主提供的 DLL：导入这些 DLL 时不装载真文件，按名字向宿主要实现
 * （engined 用它把 ime_net.dll 换成自己的网络兼容层，见 src/netcompat.c） */
static struct { char dll[64]; void *(*lookup)(const char *name); } g_host_dlls[4];
static int g_nhost;

void pe_register_host_dll(const char *dll, void *(*lookup)(const char *name))
{
    if (g_nhost < 4) {
        snprintf(g_host_dlls[g_nhost].dll, sizeof(g_host_dlls[0].dll), "%s", dll);
        g_host_dlls[g_nhost++].lookup = lookup;
    }
}

static void *(*host_dll(const char *dll))(const char *)
{
    for (int i = 0; i < g_nhost; i++)
        if (!strcasecmp(g_host_dlls[i].dll, dll))
            return g_host_dlls[i].lookup;
    return NULL;
}

static int is_system_dll(const char *dll)
{
    static const char *sys[] = {
        "kernel32.dll", "kernelbase.dll", "ntdll.dll", "advapi32.dll", "ws2_32.dll", "pdh.dll",
        "iphlpapi.dll", "dbghelp.dll", "imagehlp.dll", "user32.dll", "ole32.dll", "shell32.dll",
        "bcrypt.dll", "crypt32.dll", "mswsock.dll", "rpcrt4.dll", "msvcrt.dll",
    };
    if (!strncasecmp(dll, "api-ms-win-", 11))
        return 1;
    for (size_t i = 0; i < sizeof(sys) / sizeof(sys[0]); i++)
        if (!strcasecmp(dll, sys[i]))
            return 1;
    /* MINIME_STUB_DLLS="ime_net.dll,wcwss.dll"：这些模块不装载，所有导入都给桩 */
    const char *stub = getenv("MINIME_STUB_DLLS");
    if (stub) {
        size_t n = strlen(dll);
        for (const char *p = stub; *p;) {
            const char *e = strchr(p, ',');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            if (len == n && !strncasecmp(p, dll, n))
                return 1;
            if (!e)
                break;
            p = e + 1;
        }
    }
    return 0;
}

/* ---------------- 导出查找 ---------------- */
static void *resolve_forwarder(const char *fwd);

void *pe_export(pe_module *m, const char *name)
{
    if (!m || !m->export_rva)
        return NULL;
    ExportDir *ed = (ExportDir *)(m->base + m->export_rva);
    uint32_t *names = (uint32_t *)(m->base + ed->AddressOfNames);
    uint16_t *ords = (uint16_t *)(m->base + ed->AddressOfNameOrdinals);
    uint32_t *funcs = (uint32_t *)(m->base + ed->AddressOfFunctions);
    uint32_t idx = UINT32_MAX;
    if ((uintptr_t)name < 0x10000) {
        idx = (uint32_t)(uintptr_t)name - ed->Base;
    } else {
        /* 名字表按字典序排好，二分查找 */
        uint32_t lo = 0, hi = ed->NumberOfNames;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            int c = strcmp((const char *)(m->base + names[mid]), name);
            if (!c) {
                idx = ords[mid];
                break;
            }
            if (c < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
    }
    if (idx >= ed->NumberOfFunctions)
        return NULL;
    uint32_t rva = funcs[idx];
    if (rva >= m->export_rva && rva < m->export_rva + m->export_size)
        return resolve_forwarder((const char *)(m->base + rva));
    return m->base + rva;
}

static void *resolve_forwarder(const char *fwd)
{
    char dll[128];
    const char *dot = strrchr(fwd, '.');
    if (!dot || (size_t)(dot - fwd) >= sizeof(dll) - 5)
        return NULL;
    memcpy(dll, fwd, dot - fwd);
    strcpy(dll + (dot - fwd), ".dll");
    if (is_system_dll(dll))
        return shim_lookup(dot + 1);
    return pe_export(pe_module_by_name(dll), dot + 1);
}

/* ---------------- 静态 TLS ---------------- */
static void thread_setup_tls(uint8_t *teb)
{
    void **arr = *(void ***)(teb + TEB_TLS_POINTER);
    if (!arr) {
        arr = calloc(64, sizeof(void *));
        *(void ***)(teb + TEB_TLS_POINTER) = arr;
    }
    for (pe_module *m = g_modules; m; m = m->next) {
        if (m->tls_index < 0 || arr[m->tls_index])
            continue;
        uint8_t *blk = calloc(1, m->tls_template_size + m->tls_zero_fill + 16);
        memcpy(blk, m->tls_template, m->tls_template_size);
        arr[m->tls_index] = blk;
    }
}

/* ---------------- TEB ---------------- */
static __thread uint8_t *t_teb;

static uint8_t *teb_create(void)
{
    uint8_t *teb = aligned_alloc(4096, TEB_SIZE);
    memset(teb, 0, TEB_SIZE);
    *(uint8_t **)(teb + TEB_SELF) = teb;
    *(uint64_t *)(teb + TEB_PID) = (uint64_t)getpid();
    *(uint64_t *)(teb + TEB_TID) = (uint64_t)syscall(SYS_gettid);
    *(uint8_t **)(teb + TEB_PEB) = g_peb;
    *(void **)(teb + TEB_FLS_DATA) = calloc(128, sizeof(void *));
    pthread_attr_t a;
    if (!pthread_getattr_np(pthread_self(), &a)) {
        void *lo;
        size_t sz;
        pthread_attr_getstack(&a, &lo, &sz);
        *(uint8_t **)(teb + TEB_STACK_LIMIT) = (uint8_t *)lo;
        *(uint8_t **)(teb + TEB_STACK_BASE) = (uint8_t *)lo + sz;
        pthread_attr_destroy(&a);
    }
    return teb;
}

void pe_thread_attach(void)
{
    if (t_teb)
        return;
    t_teb = teb_create();
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, t_teb) != 0) {
        perror("arch_prctl(ARCH_SET_GS)");
        abort();
    }
    pthread_mutex_lock(&g_load_lock);
    thread_setup_tls(t_teb);
    pthread_mutex_unlock(&g_load_lock);
}

static void call_tls_callbacks(pe_module *m, DWORD reason)
{
    if (!m->tls_callbacks)
        return;
    for (uint64_t *cb = m->tls_callbacks; *cb; cb++)
        ((TlsCallback)(uintptr_t)*cb)(m->base, reason, NULL);
}

void pe_thread_start_hooks(void)
{
    pe_thread_attach();
    for (pe_module *m = g_modules; m; m = m->next) {
        call_tls_callbacks(m, DLL_THREAD_ATTACH);
        if (m->entry_rva)
            ((DllEntry)(m->base + m->entry_rva))(m->base, DLL_THREAD_ATTACH, NULL);
    }
}

void pe_thread_exit_hooks(void)
{
    for (pe_module *m = g_modules; m; m = m->next) {
        call_tls_callbacks(m, DLL_THREAD_DETACH);
        if (m->entry_rva)
            ((DllEntry)(m->base + m->entry_rva))(m->base, DLL_THREAD_DETACH, NULL);
    }
    if (t_teb) {
        void **tls = *(void ***)(t_teb + TEB_TLS_POINTER);
        if (tls) {
            for (int i = 0; i < 64; i++)
                free(tls[i]);
            free(tls);
        }
        free(*(void **)(t_teb + TEB_FLS_DATA));
        free(t_teb);
        t_teb = NULL;
    }
}

/* ---------------- 崩溃报告 ---------------- */
/* 栈上找落在模块里的返回地址，粗略回溯 */
void pe_dump_stack(void *spv, int max)
{
    uint64_t *sp = spv;
    int n = 0;
    for (int i = 0; i < 1024 && n < max; i++) {
        pe_module *r = pe_module_by_addr((void *)sp[i]);
        if (r) {
            fprintf(stderr, "[loader]   栈 %s+%#lx\n", r->name, (unsigned long)(sp[i] - (uint64_t)r->base));
            n++;
        }
    }
}

static void crash_handler(int sig, siginfo_t *si, void *ctx)
{
    ucontext_t *uc = ctx;
    uint64_t rip = uc->uc_mcontext.gregs[REG_RIP];
    pe_module *m = pe_module_by_addr((void *)rip);
    fprintf(stderr, "\n[loader] 信号 %d，地址 %p，rip=%#lx", sig, si->si_addr, (unsigned long)rip);
    if (m)
        fprintf(stderr, "（%s+%#lx）", m->name, (unsigned long)(rip - (uint64_t)m->base));
    fprintf(stderr, "\n[loader] rax=%#llx rbx=%#llx rcx=%#llx rdx=%#llx rsi=%#llx rdi=%#llx r8=%#llx r9=%#llx\n",
            uc->uc_mcontext.gregs[REG_RAX], uc->uc_mcontext.gregs[REG_RBX], uc->uc_mcontext.gregs[REG_RCX],
            uc->uc_mcontext.gregs[REG_RDX], uc->uc_mcontext.gregs[REG_RSI], uc->uc_mcontext.gregs[REG_RDI],
            uc->uc_mcontext.gregs[REG_R8], uc->uc_mcontext.gregs[REG_R9]);
    pe_dump_stack((void *)uc->uc_mcontext.gregs[REG_RSP], 12);
    signal(sig, SIG_DFL);
    raise(sig);
}

void pe_process_init(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    /* PEB：只填会被读到的字段 */
    /* +0x20 ProcessParameters（RTL_USER_PROCESS_PARAMETERS）：CRT 会直接读里面的 Flags，
     * 再填上 ImagePathName（+0x60）和 CommandLine（+0x70）两个 UNICODE_STRING */
    static uint8_t params[0x440] __attribute__((aligned(16)));
    static uint16_t cmdline[] = { 'w', 'e', 't', 'y', 'p', 'e', '-', 'e', 'n', 'g', 'i', 'n', 'e', 'd', 0 };
    for (int off = 0x60; off <= 0x70; off += 0x10) {
        *(uint16_t *)(params + off) = (uint16_t)(sizeof(cmdline) - 2);
        *(uint16_t *)(params + off + 2) = (uint16_t)sizeof(cmdline);
        *(uint16_t **)(params + off + 8) = cmdline;
    }
    *(uint8_t **)(g_peb + 0x20) = params;
    *(uint64_t *)(g_peb + 0x30) = 0x1000;  /* ProcessHeap：和 GetProcessHeap 一致的假句柄 */
    *(uint32_t *)(g_peb + 0xb8) = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN); /* NumberOfProcessors */
    *(uint32_t *)(g_peb + 0x118) = 10;    /* OSMajorVersion */
    *(uint32_t *)(g_peb + 0x11c) = 0;     /* OSMinorVersion */
    *(uint16_t *)(g_peb + 0x120) = 19045; /* OSBuildNumber */
    *(uint32_t *)(g_peb + 0x124) = 2;     /* OSPlatformId = VER_PLATFORM_WIN32_NT */
    static uint8_t alt_stack[32768] __attribute__((aligned(16)));
    stack_t ss = { .ss_sp = alt_stack, .ss_size = sizeof(alt_stack), .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    pe_thread_attach();
}

/* ---------------- 装载 ---------------- */
typedef struct {
    pe_module *mods[128];
    int count;
} InitList;

static pe_module *load_one(const char *path, InitList *init);

static int bind_imports(pe_module *m, OptHeader64 *oh, InitList *init)
{
    DataDir d = oh->Dir[DIR_IMPORT];
    if (!d.VirtualAddress)
        return 0;
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", m->path);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = 0;
    else
        strcpy(dir, ".");
    int unknown = 0;
    for (ImportDesc *id = (ImportDesc *)(m->base + d.VirtualAddress); id->Name; id++) {
        const char *dll = (const char *)(m->base + id->Name);
        pe_module *dep = NULL;
        void *(*host)(const char *) = host_dll(dll);
        int sys = host ? 1 : is_system_dll(dll);
        if (!sys) {
            dep = pe_module_by_name(dll);
            if (!dep) {
                char p[1024];
                snprintf(p, sizeof(p), "%s/%s", dir, dll);
                dep = load_one(p, init);
                if (!dep) {
                    fprintf(stderr, "[loader] %s 依赖的 %s 装载失败\n", m->name, dll);
                    return -1;
                }
            }
        }
        uint64_t *names = (uint64_t *)(m->base + (id->OriginalFirstThunk ? id->OriginalFirstThunk : id->FirstThunk));
        uint64_t *iat = (uint64_t *)(m->base + id->FirstThunk);
        for (; *names; names++, iat++) {
            const char *fn;
            char ordname[16];
            if (*names & (1ULL << 63)) {
                snprintf(ordname, sizeof(ordname), "#%u", (unsigned)(*names & 0xffff));
                fn = sys ? ordname : (const char *)(uintptr_t)(*names & 0xffff);
            } else {
                fn = (const char *)(m->base + (uint32_t)*names + 2);
            }
            void *addr = host ? ((uintptr_t)fn < 0x10000 ? NULL : host(fn)) : sys ? shim_lookup(fn) : pe_export(dep, fn);
            if (!addr) {
                const char *label = (uintptr_t)fn < 0x10000 ? ordname : fn;
                if (!sys)
                    fprintf(stderr, "[loader] %s 从 %s 导入的 %s 找不到\n", m->name, dll, label);
                addr = make_unknown_thunk(dll, label);
                unknown++;
            }
            *iat = (uint64_t)(uintptr_t)addr;
        }
    }
    LOG("%s: 导入绑定完成，%d 个未实现（调用时报告）\n", m->name, unknown);
    return 0;
}

/* 把模块 m 从 dll 导入的 fn 换成 newfn（调试用：替换引擎从某个 DLL 导入的函数）。
 * 返回原来的地址，没找到返回 NULL。 */
void *pe_patch_import(pe_module *m, const char *dll, const char *fn, void *newfn)
{
    FileHeader *fh = (FileHeader *)(m->base + ((DosHeader *)m->base)->e_lfanew);
    OptHeader64 *oh = (OptHeader64 *)(fh + 1);
    DataDir d = oh->Dir[DIR_IMPORT];
    if (!d.VirtualAddress)
        return NULL;
    for (ImportDesc *id = (ImportDesc *)(m->base + d.VirtualAddress); id->Name; id++) {
        if (strcasecmp((const char *)(m->base + id->Name), dll) != 0)
            continue;
        uint64_t *names = (uint64_t *)(m->base + (id->OriginalFirstThunk ? id->OriginalFirstThunk : id->FirstThunk));
        uint64_t *iat = (uint64_t *)(m->base + id->FirstThunk);
        for (; *names; names++, iat++) {
            if (*names & (1ULL << 63) || strcmp((const char *)(m->base + (uint32_t)*names + 2), fn) != 0)
                continue;
            void *old = (void *)(uintptr_t)*iat;
            uintptr_t pg = (uintptr_t)iat & ~(uintptr_t)4095;
            mprotect((void *)pg, 4096, PROT_READ | PROT_WRITE);
            *iat = (uint64_t)(uintptr_t)newfn;
            return old;
        }
    }
    return NULL;
}

/* ---------------- 映像缓存 ----------------
 * PE 文件里的节按 0x200 对齐，不能直接按页映射，所以以前整段复制进匿名内存：
 * 这部分（三个 DLL 约 32 MB）不能共享，内存紧张时只能换到交换分区。
 * 现在第一次装载时把重定位好的映像（按页对齐，和内存里的布局一样）写到
 * ~/.cache/minime/pe/，以后 MAP_PRIVATE 映射它：只读的页按需读入、系统可以随时丢掉再读回；
 * 写过的页（导入表、.data）照样变成私有副本。对 DLL 来说内容和地址都不变。
 * 文件名带原 DLL 的 dev/ino/大小/修改时间、基址和缓存格式版本，任何一样变了就是另一个文件；
 * 更新只写新文件再改名，不原地改（映射着的文件被改会让内存内容跟着变）。
 * MINIME_PE_CACHE=0 关掉。 */
#define PE_CACHE_VER 1

static void cache_path(char *out, size_t n, const char *path, const struct stat *st, uintptr_t base)
{
    out[0] = 0;
    const char *off = getenv("MINIME_PE_CACHE");
    if (off && !strcmp(off, "0"))
        return;
    const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    char dir[900];
    if (x && *x)
        snprintf(dir, sizeof(dir), "%s/minime/pe", x);
    else if (h && *h)
        snprintf(dir, sizeof(dir), "%s/.cache/minime/pe", h);
    else
        return;
    /* 逐级建目录（0700） */
    for (char *q = dir + 1; *q; q++)
        if (*q == '/') {
            *q = 0;
            mkdir(dir, 0700);
            *q = '/';
        }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return;
    snprintf(out, n, "%s/%s-v%d-%lx-%lx-%lx-%lx.%09ld-%lx.img", dir, base_name(path), PE_CACHE_VER,
             (unsigned long)st->st_dev, (unsigned long)st->st_ino, (unsigned long)st->st_size,
             (unsigned long)st->st_mtim.tv_sec, (long)st->st_mtim.tv_nsec, (unsigned long)base);
}

/* 把缓存文件映射到 base（替换掉那里的匿名映射）。文件尺寸或头不对就不用 */
static int map_cached(const char *cpath, uint8_t *base, size_t size)
{
    int fd = open(cpath, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    struct stat cs;
    uint16_t mz = 0;
    if (fstat(fd, &cs) != 0 || (size_t)cs.st_size != size || pread(fd, &mz, 2, 0) != 2 || mz != 0x5a4d) {
        close(fd);
        return 0;
    }
    void *p = mmap(base, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        /* MAP_FIXED 失败时原映射可能已经没了：补一块匿名映射，调用方按没缓存处理（重新复制） */
        mmap(base, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        return 0;
    }
    return 1;
}

static int save_cache(const char *cpath, const uint8_t *base, size_t size)
{
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", cpath, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return 0;
    size_t done = 0;
    while (done < size) {
        ssize_t w = write(fd, base + done, size - done);
        if (w <= 0)
            break;
        done += (size_t)w;
    }
    int ok = done == size && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (!ok || rename(tmp, cpath) != 0) {
        unlink(tmp);
        return 0;
    }
    LOG("映像缓存写入 %s\n", cpath);
    /* 同一个 DLL 的旧缓存（DLL 更新过、基址变了）删掉；别的进程还映射着也没关系，inode 要等它解除映射才释放 */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", cpath);
    char *slash = strrchr(dir, '/'), *dash;
    if (slash && (dash = strstr(slash + 1, "-v")) != NULL) {
        size_t plen = (size_t)(dash - (slash + 1)) + 2;          /* "名字-v" */
        char prefix[300];
        snprintf(prefix, sizeof(prefix), "%.*s", (int)plen, slash + 1);
        const char *mine = strrchr(cpath, '/') + 1;
        *slash = 0;
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL)
            if (!strncmp(e->d_name, prefix, plen) && strcmp(e->d_name, mine) && !strstr(e->d_name, ".tmp")) {
                char old[1400];
                snprintf(old, sizeof(old), "%s/%s", dir, e->d_name);
                unlink(old);
            }
        if (d)
            closedir(d);
    }
    return 1;
}

static void unlink_module(pe_module *m)
{
    pe_module **pp = &g_modules;
    while (*pp) {
        if (*pp == m) {
            *pp = m->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

static pe_module *load_one(const char *path, InitList *init)
{
    pe_module *exist = pe_module_by_name(path);
    if (exist)
        return exist;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[loader] 打不开 %s: %s\n", path, strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)(sizeof(DosHeader) + sizeof(FileHeader))) {
        close(fd);
        return NULL;
    }
    uint8_t *file = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (file == MAP_FAILED)
        return NULL;
    DosHeader *dos = (DosHeader *)file;
    if (dos->e_magic != 0x5a4d || dos->e_lfanew < 0 ||
        (off_t)dos->e_lfanew + (off_t)sizeof(FileHeader) > st.st_size) {
        fprintf(stderr, "[loader] %s 不是有效的 x64 PE\n", path);
        munmap(file, (size_t)st.st_size);
        return NULL;
    }
    FileHeader *fh = (FileHeader *)(file + dos->e_lfanew);
    if (fh->Signature != 0x4550 || fh->Machine != 0x8664) {
        fprintf(stderr, "[loader] %s 不是 x64 PE\n", path);
        munmap(file, (size_t)st.st_size);
        return NULL;
    }
    OptHeader64 *oh = (OptHeader64 *)(fh + 1);
    SectionHeader *sec = (SectionHeader *)((uint8_t *)oh + fh->SizeOfOptionalHeader);

    /* 基址：先试首选基址，被占了就按固定步长往后试（每次运行地址都一样，缓存才能复用），
     * 都不行才随便放（这时不用缓存） */
    uint8_t *base = MAP_FAILED;
    int fixed = 0;
    for (int k = 0; k < 64 && base == MAP_FAILED; k++) {
        uint64_t want = oh->ImageBase + (uint64_t)k * 0x10000000ULL;
        if (want < oh->ImageBase || want + oh->SizeOfImage > 0x7ff000000000ULL)
            break;
        base = mmap((void *)(uintptr_t)want, oh->SizeOfImage, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (base != MAP_FAILED && base != (uint8_t *)(uintptr_t)want) {   /* 老内核不认 NOREPLACE */
            munmap(base, oh->SizeOfImage);
            base = MAP_FAILED;
        }
        fixed = base != MAP_FAILED;
    }
    if (base == MAP_FAILED)
        base = mmap(NULL, oh->SizeOfImage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        munmap(file, st.st_size);
        return NULL;
    }
    uint64_t pref = oh->ImageBase;
    char cpath[1024] = "";
    if (fixed)
        cache_path(cpath, sizeof(cpath), path, &st, (uintptr_t)base);
    int cached = cpath[0] && map_cached(cpath, base, oh->SizeOfImage);
    if (!cached) {
        memcpy(base, file, oh->SizeOfHeaders);
        for (int i = 0; i < fh->NumberOfSections; i++) {
            size_t n = sec[i].SizeOfRawData < sec[i].VirtualSize ? sec[i].SizeOfRawData : sec[i].VirtualSize;
            if (!sec[i].VirtualSize)
                n = sec[i].SizeOfRawData;
            if (n)
                memcpy(base + sec[i].VirtualAddress, file + sec[i].PointerToRawData, n);
        }
    }
    munmap(file, st.st_size);
    fh = (FileHeader *)(base + ((DosHeader *)base)->e_lfanew);
    oh = (OptHeader64 *)(fh + 1);
    sec = (SectionHeader *)((uint8_t *)oh + fh->SizeOfOptionalHeader);

    /* 重定位：x64 DLL 只有 DIR64（类型 10）和填充（类型 0）。缓存里已经是重定位好的映像 */
    int64_t delta = (int64_t)((uintptr_t)base - pref);
    if (!cached && delta && oh->Dir[DIR_RELOC].VirtualAddress) {
        uint8_t *p = base + oh->Dir[DIR_RELOC].VirtualAddress;
        uint8_t *end = p + oh->Dir[DIR_RELOC].Size;
        while (p < end) {
            uint32_t page = *(uint32_t *)p, size = *(uint32_t *)(p + 4);
            if (!size)
                break;
            for (uint16_t *e = (uint16_t *)(p + 8); (uint8_t *)e < p + size; e++) {
                int type = *e >> 12;
                if (type == 10)
                    *(uint64_t *)(base + page + (*e & 0xfff)) += delta;
                else if (type != 0) {
                    fprintf(stderr, "[loader] %s 有不支持的重定位类型 %d\n", path, type);
                    munmap(base, oh->SizeOfImage);
                    return NULL;
                }
            }
            p += size;
        }
    }
    oh->ImageBase = (uintptr_t)base;
    /* 第一次：把重定位好、还没绑定导入的映像存成缓存，再换成按文件映射（这次运行也省内存） */
    if (cpath[0] && !cached && save_cache(cpath, base, oh->SizeOfImage))
        cached = map_cached(cpath, base, oh->SizeOfImage);

    pe_module *m = calloc(1, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "%s", base_name(path));
    snprintf(m->path, sizeof(m->path), "%s", path);
    m->base = base;
    m->size = oh->SizeOfImage;
    m->entry_rva = oh->AddressOfEntryPoint;
    m->export_rva = oh->Dir[DIR_EXPORT].VirtualAddress;
    m->export_size = oh->Dir[DIR_EXPORT].Size;
    m->tls_index = -1;
    /* 先挂进链表，循环依赖时能找到自己 */
    m->next = g_modules;
    g_modules = m;
    LOG("%s 映射在 %p（%s首选基址，%s），大小 %#zx\n", m->name, base, delta ? "非" : "",
        cached ? "按文件映射" : "复制进内存", m->size);

    if (bind_imports(m, oh, init) < 0) {
        unlink_module(m);
        munmap(m->base, m->size);
        free(m);
        return NULL;
    }

    /* 静态 TLS：给模块分配索引，写回模块里的索引变量 */
    if (oh->Dir[DIR_TLS].VirtualAddress &&
        (uint64_t)oh->Dir[DIR_TLS].VirtualAddress + sizeof(TlsDir) <= m->size) {
        TlsDir *td = (TlsDir *)(base + oh->Dir[DIR_TLS].VirtualAddress);
        m->tls_index = g_tls_count++;
        m->tls_template = (uint8_t *)(uintptr_t)td->StartAddressOfRawData;
        m->tls_template_size = td->EndAddressOfRawData - td->StartAddressOfRawData;
        m->tls_zero_fill = td->SizeOfZeroFill;
        m->tls_callbacks = (uint64_t *)(uintptr_t)td->AddressOfCallBacks;
        if (td->AddressOfIndex >= (uint64_t)(uintptr_t)base &&
            td->AddressOfIndex + sizeof(uint32_t) <= (uint64_t)(uintptr_t)(base + m->size))
            *(uint32_t *)(uintptr_t)td->AddressOfIndex = (uint32_t)m->tls_index;
        LOG("%s: TLS 索引 %d，模板 %zu 字节\n", m->name, m->tls_index, m->tls_template_size);
    }

    /* 节保护属性 */
    for (int i = 0; i < fh->NumberOfSections; i++) {
        uint32_t c = sec[i].Characteristics;
        int prot = 0;
        if (c & 0x40000000) prot |= PROT_READ;
        if (c & 0x80000000) prot |= PROT_WRITE;
        if (c & 0x20000000) prot |= PROT_EXEC | PROT_READ;
        size_t vs = (sec[i].VirtualSize + 4095) & ~4095UL;
        if (vs && mprotect(base + sec[i].VirtualAddress, vs, prot) != 0)
            fprintf(stderr, "[loader] mprotect %s 失败\n", sec[i].Name);
    }
    mprotect(base, 4096, PROT_READ);
    if (init && init->count < (int)(sizeof(init->mods) / sizeof(init->mods[0])))
        init->mods[init->count++] = m;
    return m;
}

static int init_module(pe_module *m)
{
    /* 已有线程（至少当前线程）补上这个模块的 TLS 块 */
    thread_setup_tls(cur_teb());
    call_tls_callbacks(m, DLL_PROCESS_ATTACH);
    if (m->entry_rva) {
        LOG("%s: 调用 DllMain(PROCESS_ATTACH)\n", m->name);
        BOOL ok = ((DllEntry)(m->base + m->entry_rva))(m->base, DLL_PROCESS_ATTACH, NULL);
        if (!ok) {
            fprintf(stderr, "[loader] %s 的 DllMain 返回失败\n", m->name);
            return -1;
        }
    }
    return 0;
}

pe_module *pe_load(const char *path)
{
    pe_process_init();
    pthread_mutex_lock(&g_load_lock);
    InitList init = { .count = 0 };
    pe_module *m = load_one(path, &init);
    pthread_mutex_unlock(&g_load_lock);
    if (!m)
        return NULL;
    /* 后序装载：依赖在 bind_imports 完成后先入队，正序遍历即为拓扑依赖序。 */
    for (int i = 0; i < init.count; i++)
        if (init_module(init.mods[i]) < 0)
            return NULL;
    return m;
}
