/* files.c — 兼容层：文件、目录、查找、文件映射、路径
 *
 * 路径约定：PE 代码给的路径可能是 "Z:\home\x"、"Z:/home/x"、"/home/x"、"\\?\Z:\..."，
 * 统一换成 Linux 路径。返回给 PE 代码的路径用 Linux 写法（斜杠），引擎对两种分隔符都认。
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#ifdef __FreeBSD__
#include <sys/param.h>
#include <sys/user.h>
#endif
#include <time.h>
#include <unistd.h>

#include "k32_internal.h"
#include "pe.h"
#include "winabi.h"

#define GENERIC_READ        0x80000000u
#define GENERIC_WRITE       0x40000000u
#define GENERIC_ALL         0x10000000u
#define FILE_READ_DATA      0x0001
#define FILE_WRITE_DATA     0x0002
#define FILE_APPEND_DATA    0x0004
#define CREATE_NEW          1
#define CREATE_ALWAYS       2
#define OPEN_EXISTING       3
#define OPEN_ALWAYS         4
#define TRUNCATE_EXISTING   5
#define FILE_FLAG_BACKUP_SEMANTICS   0x02000000u
#define FILE_FLAG_DELETE_ON_CLOSE    0x04000000u
#define FILE_ATTRIBUTE_READONLY      0x01
#define FILE_ATTRIBUTE_DIRECTORY     0x10
#define FILE_ATTRIBUTE_NORMAL        0x80
#define INVALID_FILE_ATTRIBUTES      0xffffffffu
#define ERROR_FILE_EXISTS            80
#define ERROR_HANDLE_EOF             38
#define ERROR_DIR_NOT_EMPTY          145
#define ERROR_NOT_SAME_DEVICE        17

/* ---------------- 路径 ---------------- */
static char *wpath(LPCWSTR w)
{
    if (!w)
        return NULL;
    char *a = u16_to_utf8(w, -1);
    const char *p = a;
    if (!strncmp(p, "\\\\?\\", 4) || !strncmp(p, "//?/", 4))
        p += 4;
    char *u = win_path_to_unix(p);
    free(a);
    /* 去掉末尾多余的斜杠（根目录除外） */
    size_t n = strlen(u);
    while (n > 1 && u[n - 1] == '/')
        u[--n] = 0;
    return u;
}

static DWORD errno_to_win(int e)
{
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case EACCES: case EPERM: case EISDIR: case EROFS: return ERROR_ACCESS_DENIED;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case EXDEV: return ERROR_NOT_SAME_DEVICE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EBADF: return ERROR_INVALID_HANDLE;
    default: return ERROR_INVALID_PARAMETER;
    }
}

/* 文件不存在时，Windows 区分"文件没有"和"路径（上级目录）没有" */
static DWORD missing_error(const char *path)
{
    char *d = strdup(path);
    char *s = strrchr(d, '/');
    DWORD e = ERROR_FILE_NOT_FOUND;
    if (s && s != d) {
        *s = 0;
        struct stat st;
        if (stat(d, &st) != 0 || !S_ISDIR(st.st_mode))
            e = ERROR_PATH_NOT_FOUND;
    }
    free(d);
    return e;
}

static void fail_errno(const char *path)
{
    DWORD e = errno_to_win(errno);
    if (errno == ENOENT && path)
        e = missing_error(path);
    set_last_error(e);
}

static uint64_t ts_to_filetime(struct timespec ts)
{
    return (uint64_t)ts.tv_sec * 10000000ULL + ts.tv_nsec / 100 + FILETIME_UNIX_DIFF;
}

static DWORD stat_attrs(const struct stat *st)
{
    DWORD a = 0;
    if (S_ISDIR(st->st_mode))
        a |= FILE_ATTRIBUTE_DIRECTORY;
    if (!(st->st_mode & 0222))
        a |= FILE_ATTRIBUTE_READONLY;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}

/* 调试：MINIME_TRACE_FILES=<路径片段> 时打印路径含该片段的文件操作 */
static void ftrace(const char *op, const char *path, long r)
{
    static const char *filt;
    static int init;
    if (!init) {
        filt = getenv("MINIME_TRACE_FILES");
        init = 1;
    }
    if (filt && path && strstr(path, filt))
        fprintf(stderr, "[files] %s %s -> %ld%s%s\n", op, path, r, r < 0 ? " errno=" : "", r < 0 ? strerror(errno) : "");
}

/* ---------------- 打开、关闭 ---------------- */
static WINAPI HANDLE f_CreateFileW(LPCWSTR name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    (void)share; (void)sa; (void)tmpl;
    char *path = wpath(name);
    if (!path || !*path) {
        free(path);
        set_last_error(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    int rd = (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA)) != 0;
    int wr = (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) != 0;
    int append_only = (access & FILE_APPEND_DATA) && !(access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA));
    struct stat st;
    int existed = stat(path, &st) == 0;
    int isdir = existed && S_ISDIR(st.st_mode);
    int oflags = O_CLOEXEC;
    if (isdir) {
        if (!(flags & FILE_FLAG_BACKUP_SEMANTICS)) {
            free(path);
            set_last_error(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        oflags |= O_RDONLY | O_DIRECTORY;
    } else {
        oflags |= wr ? (rd ? O_RDWR : O_WRONLY) : O_RDONLY;
        if (append_only)
            oflags |= O_APPEND;
        switch (disp) {
        case CREATE_NEW: oflags |= O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: oflags |= O_CREAT | O_TRUNC; break;
        case OPEN_ALWAYS: oflags |= O_CREAT; break;
        case TRUNCATE_EXISTING: oflags |= O_TRUNC; break;
        default: break;
        }
        if ((oflags & O_TRUNC) && !wr)
            oflags = (oflags & ~(O_RDONLY | O_WRONLY)) | O_RDWR;
    }
    int fd = open(path, oflags, 0644);
    ftrace(wr ? "open(写)" : "open(读)", path, fd);
    if (fd < 0) {
        if (errno == EEXIST)
            set_last_error(ERROR_FILE_EXISTS);
        else
            fail_errno(path);
        free(path);
        return INVALID_HANDLE_VALUE;
    }
    KObj *o = ko_new(KO_FILE);
    o->fd = fd;
    o->path = path;
    o->delete_on_close = (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0;
    /* OPEN_ALWAYS / CREATE_ALWAYS 打开已存在的文件时，Windows 会把错误码设成 ERROR_ALREADY_EXISTS */
    set_last_error((existed && (disp == OPEN_ALWAYS || disp == CREATE_ALWAYS)) ? ERROR_ALREADY_EXISTS : 0);
    return o;
}

static WINAPI HANDLE f_CreateFileA(LPCSTR name, DWORD access, DWORD share, void *sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    WCHAR w[1024];
    long n = utf8_to_u16((const unsigned char *)name, (long)strlen(name), w, 1023);
    w[n < 1023 ? n : 1023] = 0;
    return f_CreateFileW(w, access, share, sa, disp, flags, tmpl);
}

static KObj *file_obj(HANDLE h)
{
    KObj *o = ko(h);
    if (!o || o->type != KO_FILE) {
        set_last_error(ERROR_INVALID_HANDLE);
        return NULL;
    }
    return o;
}

static WINAPI BOOL f_GetFileSizeEx(HANDLE h, int64_t *size)
{
    KObj *o = file_obj(h);
    struct stat st;
    if (!o || fstat(o->fd, &st) != 0)
        return FALSE;
    *size = st.st_size;
    return TRUE;
}

static WINAPI DWORD f_GetFileSize(HANDLE h, DWORD *high)
{
    int64_t s = 0;
    if (!f_GetFileSizeEx(h, &s))
        return 0xffffffffu;
    if (high)
        *high = (DWORD)(s >> 32);
    return (DWORD)s;
}

static WINAPI BOOL f_SetFilePointerEx(HANDLE h, int64_t dist, int64_t *newpos, DWORD method)
{
    KObj *o = file_obj(h);
    if (!o)
        return FALSE;
    off_t r = lseek(o->fd, dist, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) {
        fail_errno(NULL);
        return FALSE;
    }
    if (newpos)
        *newpos = r;
    return TRUE;
}

static WINAPI DWORD f_SetFilePointer(HANDLE h, LONG lo, LONG *hi, DWORD method)
{
    int64_t d = hi ? (((int64_t)*hi << 32) | (uint32_t)lo) : lo, np = 0;
    if (!f_SetFilePointerEx(h, d, &np, method))
        return 0xffffffffu;
    if (hi)
        *hi = (LONG)(np >> 32);
    return (DWORD)np;
}

static WINAPI BOOL f_SetEndOfFile(HANDLE h)
{
    KObj *o = file_obj(h);
    if (!o)
        return FALSE;
    off_t cur = lseek(o->fd, 0, SEEK_CUR);
    if (cur < 0 || ftruncate(o->fd, cur) != 0) {
        fail_errno(NULL);
        return FALSE;
    }
    return TRUE;
}

static WINAPI BOOL f_LockFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) { (void)a; (void)b; (void)c; (void)d; return file_obj(h) != NULL; }
static WINAPI BOOL f_UnlockFile(HANDLE h, DWORD a, DWORD b, DWORD c, DWORD d) { (void)a; (void)b; (void)c; (void)d; return file_obj(h) != NULL; }
static WINAPI BOOL f_LockFileEx(HANDLE h, DWORD f, DWORD r, DWORD a, DWORD b, void *ov) { (void)f; (void)r; (void)a; (void)b; (void)ov; return file_obj(h) != NULL; }
static WINAPI BOOL f_UnlockFileEx(HANDLE h, DWORD r, DWORD a, DWORD b, void *ov) { (void)r; (void)a; (void)b; (void)ov; return file_obj(h) != NULL; }

/* ---------------- 属性与信息 ---------------- */
static WINAPI DWORD f_GetFileAttributesW(LPCWSTR name)
{
    char *p = wpath(name);
    struct stat st;
    if (!p || stat(p, &st) != 0) {
        ftrace("attrs", p, -1);
        if (p)
            fail_errno(p);
        free(p);
        return INVALID_FILE_ATTRIBUTES;
    }
    ftrace("attrs", p, 0);
    free(p);
    return stat_attrs(&st);
}

static WINAPI BOOL f_GetFileAttributesExW(LPCWSTR name, int level, uint8_t *out)
{
    (void)level;
    char *p = wpath(name);
    struct stat st;
    if (!p || stat(p, &st) != 0) {
        if (p)
            fail_errno(p);
        free(p);
        return FALSE;
    }
    free(p);
    /* WIN32_FILE_ATTRIBUTE_DATA：属性、三个 FILETIME（4 字节对齐）、大小高低位 */
    DWORD attrs = stat_attrs(&st);
    uint64_t c = ts_to_filetime(st.st_ctim), a = ts_to_filetime(st.st_atim), m = ts_to_filetime(st.st_mtim);
    DWORD hi = (DWORD)((uint64_t)st.st_size >> 32), lo = (DWORD)st.st_size;
    if (S_ISDIR(st.st_mode))
        hi = lo = 0;
    memcpy(out, &attrs, 4);
    memcpy(out + 4, &c, 8);
    memcpy(out + 12, &a, 8);
    memcpy(out + 20, &m, 8);
    memcpy(out + 28, &hi, 4);
    memcpy(out + 32, &lo, 4);
    return TRUE;
}

static WINAPI BOOL f_GetFileInformationByHandle(HANDLE h, uint8_t *out)
{
    KObj *o = file_obj(h);
    struct stat st;
    if (!o || fstat(o->fd, &st) != 0)
        return FALSE;
    memset(out, 0, 52);
    DWORD attrs = stat_attrs(&st);
    uint64_t c = ts_to_filetime(st.st_ctim), a = ts_to_filetime(st.st_atim), m = ts_to_filetime(st.st_mtim);
    DWORD vol = (DWORD)st.st_dev, hi = (DWORD)((uint64_t)st.st_size >> 32), lo = (DWORD)st.st_size;
    DWORD links = (DWORD)st.st_nlink, ihi = (DWORD)((uint64_t)st.st_ino >> 32), ilo = (DWORD)st.st_ino;
    memcpy(out, &attrs, 4);
    memcpy(out + 4, &c, 8);
    memcpy(out + 12, &a, 8);
    memcpy(out + 20, &m, 8);
    memcpy(out + 28, &vol, 4);
    memcpy(out + 32, &hi, 4);
    memcpy(out + 36, &lo, 4);
    memcpy(out + 40, &links, 4);
    memcpy(out + 44, &ihi, 4);
    memcpy(out + 48, &ilo, 4);
    return TRUE;
}

enum { FileBasicInfo = 0, FileStandardInfo = 1, FileRenameInfo = 3, FileDispositionInfo = 4,
       FileEndOfFileInfo = 6, FileAttributeTagInfo = 9, FileIdInfo = 18, FileDispositionInfoEx = 21 };

static WINAPI BOOL f_GetFileInformationByHandleEx(HANDLE h, int cls, uint8_t *out, DWORD size)
{
    KObj *o = file_obj(h);
    struct stat st;
    if (!o || fstat(o->fd, &st) != 0)
        return FALSE;
    DWORD attrs = stat_attrs(&st);
    switch (cls) {
    case FileBasicInfo: {
        if (size < 40) break;
        uint64_t c = ts_to_filetime(st.st_ctim), a = ts_to_filetime(st.st_atim), m = ts_to_filetime(st.st_mtim);
        memset(out, 0, 40);
        memcpy(out, &c, 8);
        memcpy(out + 8, &a, 8);
        memcpy(out + 16, &m, 8);
        memcpy(out + 24, &c, 8);
        memcpy(out + 32, &attrs, 4);
        return TRUE;
    }
    case FileStandardInfo: {
        if (size < 24) break;
        memset(out, 0, 24);
        int64_t alloc = (int64_t)st.st_blocks * 512, eof = S_ISDIR(st.st_mode) ? 0 : st.st_size;
        DWORD links = (DWORD)st.st_nlink;
        memcpy(out, &alloc, 8);
        memcpy(out + 8, &eof, 8);
        memcpy(out + 16, &links, 4);
        out[21] = S_ISDIR(st.st_mode) ? 1 : 0;
        return TRUE;
    }
    case FileAttributeTagInfo:
        if (size < 8) break;
        memcpy(out, &attrs, 4);
        memset(out + 4, 0, 4);
        return TRUE;
    case FileIdInfo: {
        if (size < 24) break;
        uint64_t vol = st.st_dev, ino = st.st_ino, zero = 0;
        memcpy(out, &vol, 8);
        memcpy(out + 8, &ino, 8);
        memcpy(out + 16, &zero, 8);
        return TRUE;
    }
    }
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

static int remove_path(const char *p)
{
    struct stat st;
    if (lstat(p, &st) != 0)
        return -1;
    return S_ISDIR(st.st_mode) ? rmdir(p) : unlink(p);
}

static WINAPI BOOL f_SetFileInformationByHandle(HANDLE h, int cls, uint8_t *in, DWORD size)
{
    (void)size;
    KObj *o = file_obj(h);
    if (!o)
        return FALSE;
    switch (cls) {
    case FileBasicInfo:
        return TRUE;   /* 时间戳和属性：不处理 */
    case FileDispositionInfo:
    case FileDispositionInfoEx: {
        int del = cls == FileDispositionInfo ? in[0] != 0 : (*(uint32_t *)in & 1) != 0;
        if (!del) {
            o->delete_on_close = 0;
            return TRUE;
        }
        /* Linux 可以删掉正在打开的文件，直接删 */
        if (o->path && remove_path(o->path) != 0) {
            fail_errno(o->path);
            return FALSE;
        }
        return TRUE;
    }
    case FileEndOfFileInfo:
        if (ftruncate(o->fd, *(int64_t *)in) != 0) {
            fail_errno(NULL);
            return FALSE;
        }
        return TRUE;
    case FileRenameInfo: {
        /* FILE_RENAME_INFO：+0 ReplaceIfExists（或 Flags），+8 RootDirectory，+16 长度（字节），+20 名字 */
        DWORD len = *(DWORD *)(in + 16);
        WCHAR *w = calloc(len / 2 + 1, sizeof(WCHAR));
        memcpy(w, in + 20, len);
        char *to = wpath(w);
        free(w);
        int replace = *(uint32_t *)in != 0;
        struct stat st;
        if (!replace && stat(to, &st) == 0) {
            free(to);
            set_last_error(ERROR_ALREADY_EXISTS);
            return FALSE;
        }
        if (rename(o->path, to) != 0) {
            fail_errno(to);
            free(to);
            return FALSE;
        }
        free(o->path);
        o->path = to;
        return TRUE;
    }
    }
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

static WINAPI BOOL f_GetFileTime(HANDLE h, uint64_t *c, uint64_t *a, uint64_t *m)
{
    KObj *o = file_obj(h);
    struct stat st;
    if (!o || fstat(o->fd, &st) != 0)
        return FALSE;
    if (c) *c = ts_to_filetime(st.st_ctim);
    if (a) *a = ts_to_filetime(st.st_atim);
    if (m) *m = ts_to_filetime(st.st_mtim);
    return TRUE;
}

static WINAPI DWORD f_GetFinalPathNameByHandleW(HANDLE h, LPWSTR out, DWORD n, DWORD flags)
{
    (void)flags;
    KObj *o = file_obj(h);
    if (!o)
        return 0;
    char buf[PATH_MAX];
    int ok = 0;
#if defined(__linux__)
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", o->fd);
    ssize_t r = readlink(link, buf, sizeof(buf) - 1);
    if (r >= 0) {
        buf[r] = 0;
        ok = 1;
    }
#elif defined(__FreeBSD__) && defined(F_KINFO)
    struct kinfo_file kf;
    memset(&kf, 0, sizeof(kf));
    kf.kf_structsize = sizeof(kf);
    if (fcntl(o->fd, F_KINFO, &kf) == 0 && kf.kf_path[0]) {
        snprintf(buf, sizeof(buf), "%s", kf.kf_path);
        ok = 1;
    }
#endif
    if (!ok) {
        if (!o->path)
            return 0;
        if (!realpath(o->path, buf))
            snprintf(buf, sizeof(buf), "%s", o->path);
    }
    size_t blen = strlen(buf);
    long w = utf8_to_u16((const unsigned char *)buf, (long)blen, NULL, 0);
    if (!out || (DWORD)w + 1 > n)
        return (DWORD)w + 1;
    utf8_to_u16((const unsigned char *)buf, (long)blen, out, n);
    out[w] = 0;
    return (DWORD)w;
}

/* ---------------- 目录、删除、改名、复制 ---------------- */
static WINAPI BOOL f_CreateDirectoryW(LPCWSTR name, void *sa)
{
    (void)sa;
    char *p = wpath(name);
    int r = mkdir(p, 0755);
    ftrace("mkdir", p, r);
    if (r != 0)
        fail_errno(p);
    free(p);
    return r == 0;
}
static WINAPI BOOL f_CreateDirectoryA(LPCSTR name, void *sa)
{
    (void)sa;
    char *p = win_path_to_unix(name);
    int r = mkdir(p, 0755);
    if (r != 0)
        fail_errno(p);
    free(p);
    return r == 0;
}

static WINAPI BOOL f_RemoveDirectoryW(LPCWSTR name)
{
    char *p = wpath(name);
    int r = rmdir(p);
    ftrace("rmdir", p, r);
    if (r != 0)
        fail_errno(p);
    free(p);
    return r == 0;
}

static WINAPI BOOL f_DeleteFileW(LPCWSTR name)
{
    char *p = wpath(name);
    int r = unlink(p);
    ftrace("unlink", p, r);
    if (r != 0)
        fail_errno(p);
    free(p);
    return r == 0;
}

#define MOVEFILE_REPLACE_EXISTING 1
static BOOL do_move(LPCWSTR from, LPCWSTR to, int replace)
{
    char *a = wpath(from), *b = wpath(to);
    struct stat st;
    BOOL ok = FALSE;
    if (!replace && stat(b, &st) == 0)
        set_last_error(ERROR_ALREADY_EXISTS);
    else if (rename(a, b) != 0) {
        ftrace("rename", a, -1);
        fail_errno(a);
    }
    else
        ok = TRUE;
    free(a);
    free(b);
    return ok;
}
static WINAPI BOOL f_MoveFileW(LPCWSTR a, LPCWSTR b) { return do_move(a, b, 0); }
static WINAPI BOOL f_MoveFileExW(LPCWSTR a, LPCWSTR b, DWORD flags) { return do_move(a, b, flags & MOVEFILE_REPLACE_EXISTING); }

static int copy_file(const char *a, const char *b, int fail_if_exists)
{
    int in = open(a, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    int out = open(b, O_WRONLY | O_CREAT | O_CLOEXEC | (fail_if_exists ? O_EXCL : O_TRUNC), 0644);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        if (write(out, buf, n) != n) {
            n = -1;
            break;
        }
    close(in);
    close(out);
    return n < 0 ? -1 : 0;
}

static WINAPI BOOL f_CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists)
{
    char *a = wpath(from), *b = wpath(to);
    int r = copy_file(a, b, fail_if_exists);
    if (r != 0)
        fail_errno(a);
    free(a);
    free(b);
    return r == 0;
}

static WINAPI BOOL f_ReplaceFileW(LPCWSTR replaced, LPCWSTR replacement, LPCWSTR backup, DWORD flags, void *x, void *y)
{
    (void)flags; (void)x; (void)y;
    char *a = wpath(replaced), *b = wpath(replacement);
    if (backup) {
        char *c = wpath(backup);
        copy_file(a, c, 0);
        free(c);
    }
    int r = rename(b, a);
    if (r != 0)
        fail_errno(b);
    free(a);
    free(b);
    return r == 0;
}

static WINAPI BOOL f_CreateHardLinkW(LPCWSTR link_name, LPCWSTR target, void *sa)
{
    (void)sa;
    char *a = wpath(link_name), *b = wpath(target);
    int r = link(b, a);
    if (r != 0)
        fail_errno(b);
    free(a);
    free(b);
    return r == 0;
}

static WINAPI BOOL f_CreateSymbolicLinkW(LPCWSTR link_name, LPCWSTR target, DWORD flags)
{
    (void)flags;
    char *a = wpath(link_name), *b = wpath(target);
    int r = symlink(b, a);
    if (r != 0)
        fail_errno(b);
    free(a);
    free(b);
    return r == 0;
}

static WINAPI BOOL f_DeviceIoControl(HANDLE h, DWORD code, void *in, DWORD inn, void *out, DWORD outn, DWORD *ret, void *ov)
{
    (void)h; (void)code; (void)in; (void)inn; (void)out; (void)outn; (void)ret; (void)ov;
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

/* ---------------- 查找 ---------------- */
/* WIN32_FIND_DATAW：属性、三个 FILETIME、大小高低位、两个保留、cFileName[260]、cAlternateFileName[14] */
static void fill_find_data(uint8_t *out, const char *dir, const char *name)
{
    memset(out, 0, 592);
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", dir, name);
    struct stat st;
    if (stat(full, &st) == 0) {
        DWORD attrs = stat_attrs(&st);
        uint64_t c = ts_to_filetime(st.st_ctim), a = ts_to_filetime(st.st_atim), m = ts_to_filetime(st.st_mtim);
        DWORD hi = (DWORD)((uint64_t)st.st_size >> 32), lo = (DWORD)st.st_size;
        if (S_ISDIR(st.st_mode))
            hi = lo = 0;
        memcpy(out, &attrs, 4);
        memcpy(out + 4, &c, 8);
        memcpy(out + 12, &a, 8);
        memcpy(out + 20, &m, 8);
        memcpy(out + 28, &hi, 4);
        memcpy(out + 32, &lo, 4);
    }
    long w = utf8_to_u16((const unsigned char *)name, (long)strlen(name), (WCHAR *)(out + 44), 259);
    ((WCHAR *)(out + 44))[w < 259 ? w : 259] = 0;
}

static int find_next(KObj *o, uint8_t *out)
{
    struct dirent *e;
    while ((e = readdir((DIR *)o->dir))) {
        if (fnmatch(o->pattern, e->d_name, 0) == 0 || !strcmp(o->pattern, "*.*")) {
            fill_find_data(out, o->path, e->d_name);
            return 1;
        }
    }
    return 0;
}

void find_close_obj(KObj *o)
{
    if (o->dir) {
        closedir((DIR *)o->dir);
        o->dir = NULL;
    }
}

static WINAPI HANDLE f_FindFirstFileExW(LPCWSTR spec, int level, uint8_t *out, int op, void *filter, DWORD flags)
{
    (void)level; (void)op; (void)filter; (void)flags;
    char *p = wpath(spec);
    char *slash = strrchr(p, '/');
    char *dir, *pat;
    if (slash) {
        *slash = 0;
        dir = strdup(*p ? p : "/");
        pat = strdup(slash + 1);
    } else {
        dir = strdup(".");
        pat = strdup(p);
    }
    free(p);
    DIR *d = opendir(dir);
    ftrace("finddir", dir, d ? 0 : -1);
    if (!d) {
        DWORD e = errno == ENOENT ? ERROR_PATH_NOT_FOUND : errno_to_win(errno);
        set_last_error(e);
        free(dir);
        free(pat);
        return INVALID_HANDLE_VALUE;
    }
    KObj *o = ko_new(KO_FIND);
    o->dir = d;
    o->path = dir;
    o->pattern = pat;
    if (!find_next(o, out)) {
        find_close_obj(o);
        ko_unref(o);
        set_last_error(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return o;
}

static WINAPI HANDLE f_FindFirstFileW(LPCWSTR spec, uint8_t *out) { return f_FindFirstFileExW(spec, 0, out, 0, NULL, 0); }

static WINAPI BOOL f_FindNextFileW(HANDLE h, uint8_t *out)
{
    KObj *o = ko(h);
    if (!o || o->type != KO_FIND || !o->dir) {
        set_last_error(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!find_next(o, out)) {
        set_last_error(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    return TRUE;
}

static WINAPI BOOL f_FindClose(HANDLE h)
{
    KObj *o = ko(h);
    if (!o || o->type != KO_FIND)
        return FALSE;
    find_close_obj(o);
    ko_unref(o);
    return TRUE;
}

/* ---------------- 文件映射 ---------------- */
#define PAGE_READONLY  0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define FILE_MAP_COPY  0x01
#define FILE_MAP_WRITE 0x02
#define FILE_MAP_READ  0x04
#define FILE_MAP_EXECUTE 0x20

static struct { void *p; size_t n; } g_views[4096];
static pthread_mutex_t g_views_lock = PTHREAD_MUTEX_INITIALIZER;

static WINAPI HANDLE f_CreateFileMappingW(HANDLE file, void *sa, DWORD prot, DWORD hi, DWORD lo, LPCWSTR name)
{
    (void)sa; (void)name;
    uint64_t size = ((uint64_t)hi << 32) | lo;
    int fd;
    if (file == INVALID_HANDLE_VALUE) {
#if defined(__linux__)
        fd = memfd_create("minime-mapping", MFD_CLOEXEC);
#elif defined(SHM_ANON)
        fd = shm_open(SHM_ANON, O_RDWR | O_CLOEXEC, 0600);
#elif defined(MFD_CLOEXEC)
        fd = memfd_create("minime-mapping", MFD_CLOEXEC);
#else
        const char *tmp = getenv("TMPDIR");
        char tmpl[PATH_MAX];
        snprintf(tmpl, sizeof(tmpl), "%s/minime-map-XXXXXX", (tmp && *tmp) ? tmp : "/tmp");
        fd = mkstemp(tmpl);
        if (fd >= 0) {
            unlink(tmpl);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
#endif
        if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
            if (fd >= 0)
                close(fd);
            set_last_error(ERROR_NOT_ENOUGH_MEMORY);
            return NULL;
        }
    } else {
        KObj *f = file_obj(file);
        if (!f)
            return NULL;
        fd = dup(f->fd);
        struct stat st;
        fstat(fd, &st);
        if (!size)
            size = st.st_size;
        else if (size > (uint64_t)st.st_size && (prot & 0xff) == PAGE_READWRITE && ftruncate(fd, (off_t)size) != 0) {
            close(fd);
            fail_errno(NULL);
            return NULL;
        }
    }
    KObj *o = ko_new(KO_MAPPING);
    o->fd = fd;
    o->map_size = size;
    o->map_prot = prot;
    set_last_error(0);
    return o;
}

static WINAPI HANDLE f_CreateFileMappingA(HANDLE file, void *sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name)
{
    (void)name;
    return f_CreateFileMappingW(file, sa, prot, hi, lo, NULL);
}

static WINAPI void *f_MapViewOfFile(HANDLE map, DWORD access, DWORD hi, DWORD lo, SIZE_T n)
{
    KObj *o = ko(map);
    if (!o || o->type != KO_MAPPING) {
        set_last_error(ERROR_INVALID_HANDLE);
        return NULL;
    }
    uint64_t off = ((uint64_t)hi << 32) | lo;
    if (!n)
        n = o->map_size - off;
    int prot = PROT_READ, flags = MAP_SHARED;
    if (access & FILE_MAP_WRITE)
        prot |= PROT_WRITE;
    if (access & FILE_MAP_COPY) {
        prot |= PROT_WRITE;
        flags = MAP_PRIVATE;
    }
    if (access & FILE_MAP_EXECUTE)
        prot |= PROT_EXEC;
    void *p = mmap(NULL, n, prot, flags, o->fd, (off_t)off);
    if (p == MAP_FAILED) {
        fail_errno(NULL);
        return NULL;
    }
    int slot = -1;
    pthread_mutex_lock(&g_views_lock);
    for (int i = 0; i < 4096; i++)
        if (!g_views[i].p) {
            g_views[i].p = p;
            g_views[i].n = n;
            slot = i;
            break;
        }
    pthread_mutex_unlock(&g_views_lock);
    if (slot < 0) {
        munmap(p, n);
        set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    return p;
}

static WINAPI void *f_MapViewOfFileEx(HANDLE map, DWORD access, DWORD hi, DWORD lo, SIZE_T n, void *base)
{
    (void)base;
    return f_MapViewOfFile(map, access, hi, lo, n);
}

static WINAPI BOOL f_UnmapViewOfFile(const void *p)
{
    pthread_mutex_lock(&g_views_lock);
    for (int i = 0; i < 4096; i++)
        if (g_views[i].p == p) {
            munmap((void *)p, g_views[i].n);
            g_views[i].p = NULL;
            pthread_mutex_unlock(&g_views_lock);
            return TRUE;
        }
    pthread_mutex_unlock(&g_views_lock);
    set_last_error(ERROR_INVALID_PARAMETER);
    return FALSE;
}

static WINAPI BOOL f_FlushViewOfFile(const void *p, SIZE_T n)
{
    uintptr_t a = (uintptr_t)p & ~4095UL;
    return msync((void *)a, n ? n + ((uintptr_t)p - a) : 4096, MS_SYNC) == 0;
}

/* ---------------- 路径查询 ---------------- */
static DWORD put_wstr(const char *s, LPWSTR out, DWORD n)
{
    long w = utf8_to_u16((const unsigned char *)s, (long)strlen(s), NULL, 0);
    if (!out || (DWORD)w + 1 > n)
        return (DWORD)w + 1;
    utf8_to_u16((const unsigned char *)s, (long)strlen(s), out, n);
    out[w] = 0;
    return (DWORD)w;
}

static WINAPI DWORD f_GetFullPathNameW(LPCWSTR name, DWORD n, LPWSTR out, LPWSTR *filepart)
{
    char *a = u16_to_utf8(name, -1);
    char buf[PATH_MAX * 2];
    int absolute = a[0] == '/' || a[0] == '\\' || (a[0] && a[1] == ':');
    if (absolute) {
        snprintf(buf, sizeof(buf), "%s", a);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, "/");
        snprintf(buf, sizeof(buf), "%s/%s", cwd, a);
    }
    free(a);
    DWORD r = put_wstr(buf, out, n);
    if (filepart && out && r < n) {
        *filepart = NULL;
        for (DWORD i = 0; i < r; i++)
            if (out[i] == '/' || out[i] == '\\')
                *filepart = out + i + 1;
    }
    return r;
}

static WINAPI DWORD f_GetFullPathNameA(LPCSTR name, DWORD n, LPSTR out, LPSTR *filepart)
{
    char buf[PATH_MAX * 2];
    if (name[0] == '/' || name[0] == '\\' || (name[0] && name[1] == ':')) {
        snprintf(buf, sizeof(buf), "%s", name);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd)))
            strcpy(cwd, "/");
        snprintf(buf, sizeof(buf), "%s/%s", cwd, name);
    }
    size_t len = strlen(buf);
    if (len + 1 > n)
        return (DWORD)len + 1;
    memcpy(out, buf, len + 1);
    if (filepart) {
        char *s = strrchr(out, '/'), *b = strrchr(out, '\\');
        if (b > s)
            s = b;
        *filepart = s ? s + 1 : out;
    }
    return (DWORD)len;
}

static WINAPI DWORD f_GetCurrentDirectoryW(DWORD n, LPWSTR out)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        return 0;
    return put_wstr(cwd, out, n);
}

static WINAPI BOOL f_SetCurrentDirectoryW(LPCWSTR name)
{
    char *p = wpath(name);
    int r = chdir(p);
    if (r != 0)
        fail_errno(p);
    free(p);
    return r == 0;
}

static WINAPI DWORD f_GetTempPathW(DWORD n, LPWSTR out)
{
    const char *t = getenv("TMPDIR");
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s/", t && *t ? t : "/tmp");
    return put_wstr(buf, out, n);
}

static WINAPI BOOL f_GetDiskFreeSpaceExW(LPCWSTR name, uint64_t *avail, uint64_t *total, uint64_t *free_)
{
    char *p = name ? wpath(name) : strdup(".");
    struct statvfs sv;
    int r = statvfs(p, &sv);
    free(p);
    if (r != 0) {
        set_last_error(ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    if (avail) *avail = (uint64_t)sv.f_bavail * sv.f_frsize;
    if (total) *total = (uint64_t)sv.f_blocks * sv.f_frsize;
    if (free_) *free_ = (uint64_t)sv.f_bfree * sv.f_frsize;
    return TRUE;
}

static WINAPI UINT f_GetDriveTypeW(LPCWSTR root) { (void)root; return 3; /* DRIVE_FIXED */ }

#define F(n) { #n, (void *)f_##n }
const Shim g_file_shims[] = {
    F(CreateFileW), F(CreateFileA), F(GetFileSizeEx), F(GetFileSize), F(SetFilePointerEx), F(SetFilePointer),
    F(SetEndOfFile), F(LockFile), F(UnlockFile), F(LockFileEx), F(UnlockFileEx),
    F(GetFileAttributesW), F(GetFileAttributesExW), F(GetFileInformationByHandle), F(GetFileInformationByHandleEx),
    F(SetFileInformationByHandle), F(GetFileTime), F(GetFinalPathNameByHandleW),
    F(CreateDirectoryW), F(CreateDirectoryA), F(RemoveDirectoryW), F(DeleteFileW), F(MoveFileW), F(MoveFileExW),
    F(CopyFileW), F(ReplaceFileW), F(CreateHardLinkW), F(CreateSymbolicLinkW), F(DeviceIoControl),
    F(FindFirstFileExW), F(FindFirstFileW), F(FindNextFileW), F(FindClose),
    F(CreateFileMappingW), F(CreateFileMappingA), F(MapViewOfFile), F(MapViewOfFileEx), F(UnmapViewOfFile),
    F(FlushViewOfFile),
    F(GetFullPathNameW), F(GetFullPathNameA), F(GetCurrentDirectoryW), F(SetCurrentDirectoryW), F(GetTempPathW),
    F(GetDiskFreeSpaceExW), F(GetDriveTypeW),
    { "GetTempPath2W", (void *)f_GetTempPathW },
};
const size_t g_file_shims_n = sizeof(g_file_shims) / sizeof(g_file_shims[0]);
