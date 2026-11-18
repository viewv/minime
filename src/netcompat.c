/* netcompat.c — ime_net.dll 的兼容层：引擎的网络请求转给 minime-netd。
 *
 * 引擎（libwxhld.dll）只从 ime_net.dll 导入 5 个函数，接口约定如下（详见 docs/cross-device.md）：
 *
 *   void *create_net_api_context(a, b, c, d)                    返回非空 ctx
 *   void  net_initialize(ctx, param, user, cb)                  异步：cb(user, result)，0 = 成功
 *                                                               （引擎把结果存进 user+0xd4，IsValid 要求 ==0）
 *   int   net_start_task_with_option(ctx, cmd, sub, body, len, opt, user, cb)
 *                                                               返回任务号；完成后 cb(user, task, err, resp, len)
 *                                                               opt+0 是超时毫秒，没有 host/path
 *   int   net_start_custom_host_ws_task_with_auth(...8 个参数) 暂不支持
 *   int   destroy_net_api_context(ctx)
 *
 * 和 ime_net 一样，cmd 只是业务号：4814/4777 由 netd 包 BaseReq，其他 cmd 原样作为
 * /api_v2 的 body（之前 4329 一直 -51，就是因为多包了一层 BaseReq）。
 *
 * 与 netd 之间：$XDG_RUNTIME_DIR/minime-netd-net.sock，小端二进制帧
 *   请求  u32 len | u32 task | i32 cmd | i32 sub | u32 timeout_ms | body
 *   响应  u32 len | u32 task | i32 err | body
 * 回调都在一个附加了 TEB 的专用线程里调用（引擎代码要求有 TEB）。
 *
 * 隐私：上屏内容上报（4814/17、4814/91）和日志上报（4959）在这里直接拒绝，不出本进程；
 * 没打开「云输入」时（netcompat_set_enabled(0)）所有任务都返回 -1。
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../loader/pe.h"
#include "wxime.h"
#include "netcompat.h"

typedef void (WXIME_CALL *task_cb)(void *user, int32_t task, int32_t err, const void *resp, int32_t len);
typedef void (WXIME_CALL *init_cb)(void *user, int32_t result);

enum { MAXPEND = 256 };
static struct {
    pthread_mutex_t mu;
    int fd;                       /* 到 netd 的连接，-1 = 未连 */
    int enabled;                  /* 云输入 */
    int sync;                     /* 同步个人词库 */
    int32_t next_task;
    struct { int32_t task; void *user; task_cb cb; int cos; } pend[MAXPEND];
    int npend;
    pthread_t reader;
    int reader_started;
    /* 初始化回调：在读线程里补发（引擎在 net_initialize 返回后才开始等） */
    void *init_user;
    init_cb init_fn;
    int stats_sent, stats_blocked;
} g_nc = { .mu = PTHREAD_MUTEX_INITIALIZER, .fd = -1, .next_task = 1 };

static int g_ctx_dummy = 1;

#define NCLOG(...) do { if (getenv("MINIME_DEBUG")) fprintf(stderr, "[netcompat] " __VA_ARGS__); } while (0)

void netcompat_set_enabled(int on)
{
    pthread_mutex_lock(&g_nc.mu);
    g_nc.enabled = on;
    pthread_mutex_unlock(&g_nc.mu);
}

void netcompat_set_sync(int on)
{
    pthread_mutex_lock(&g_nc.mu);
    g_nc.sync = on;
    pthread_mutex_unlock(&g_nc.mu);
}

static void (*g_cos_done)(int32_t job, int32_t result);
void netcompat_set_cos_done(void (*done)(int32_t job, int32_t result)) { g_cos_done = done; }

/* 个人词库同步用到的 4814 子命令（和 netd 的 USER_DICT_SUBS 一致） */
static int sync_sub(int32_t cmd, int32_t sub)
{
    static const int subs[] = { 34, 41, 42, 43, 44, 45, 46, 47, 48, 49, 60 };
    if (cmd != 4814)
        return 0;
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++)
        if (subs[i] == sub)
            return 1;
    return 0;
}

static int connect_netd(void)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s/minime-netd-net.sock", rt && *rt ? rt : "/tmp");
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* 取出并移除一个等待中的任务（找不到返回 0） */
static int take_pending(int32_t task, void **user, task_cb *cb, int *cos)
{
    for (int i = 0; i < g_nc.npend; i++)
        if (g_nc.pend[i].task == task) {
            *user = g_nc.pend[i].user;
            *cb = g_nc.pend[i].cb;
            if (cos)
                *cos = g_nc.pend[i].cos;
            g_nc.pend[i] = g_nc.pend[--g_nc.npend];
            return 1;
        }
    return 0;
}

static void *reader_main(void *arg)
{
    (void)arg;
    pe_thread_attach();                   /* 回调进引擎前必须有 TEB */
    for (;;) {
        /* 初始化回调 */
        pthread_mutex_lock(&g_nc.mu);
        init_cb icb = g_nc.init_fn;
        void *iuser = g_nc.init_user;
        g_nc.init_fn = NULL;
        int fd = g_nc.fd;
        pthread_mutex_unlock(&g_nc.mu);
        if (icb) {
            struct timespec ts = { 0, 30 * 1000000 };
            nanosleep(&ts, NULL);
            NCLOG("net_initialize 回调 result=0\n");
            icb(iuser, 0);
        }
        if (fd < 0) {
            struct timespec ts = { 0, 100 * 1000000 };
            nanosleep(&ts, NULL);
            continue;
        }
        uint32_t hdr[3];
        if (read_full(fd, hdr, sizeof(hdr)) < 0) {
        disconnected:
            /* 连接断了：由读线程统一关 FD 并把所有等待中的任务按失败回调 */
            pthread_mutex_lock(&g_nc.mu);
            if (g_nc.fd >= 0) {
                close(g_nc.fd);
                g_nc.fd = -1;
            }
            int n = g_nc.npend;
            typeof(g_nc.pend[0]) copy[MAXPEND];
            memcpy(copy, g_nc.pend, sizeof(copy[0]) * (size_t)n);
            g_nc.npend = 0;
            pthread_mutex_unlock(&g_nc.mu);
            for (int i = 0; i < n; i++) {
                if (copy[i].cos) {
                    if (g_cos_done)
                        g_cos_done(copy[i].task, -1);
                } else {
                    copy[i].cb(copy[i].user, copy[i].task, -1, NULL, 0);
                }
            }
            continue;
        }
        uint32_t len = hdr[0];
        int32_t task = (int32_t)hdr[1], err = (int32_t)hdr[2];
        if (len < 8 || len - 8 > (16u << 20))
            goto disconnected;
        uint32_t blen = len - 8;
        uint8_t *body = blen ? malloc(blen) : NULL;
        if (blen && read_full(fd, body, blen) < 0) {
            free(body);
            goto disconnected;
        }
        void *user;
        task_cb cb;
        int cos = 0;
        pthread_mutex_lock(&g_nc.mu);
        int found = take_pending(task, &user, &cb, &cos);
        pthread_mutex_unlock(&g_nc.mu);
        NCLOG("任务 %d 完成 err=%d len=%u%s%s\n", task, err, blen, cos ? "（COS）" : "", found ? "" : "（没人等）");
        if (found && cos) {
            if (g_cos_done)
                g_cos_done(task, err == 0 ? 0 : -1);
        } else if (found) {
            cb(user, task, err, body, (int32_t)blen);
        }
        free(body);
    }
    return NULL;
}

static void ensure_reader(void)
{
    if (!g_nc.reader_started) {
        g_nc.reader_started = 1;
        pthread_create(&g_nc.reader, NULL, reader_main, NULL);
        pthread_detach(g_nc.reader);
    }
}

/* ---------------- 导出给引擎的 5 个函数（Windows x64 调用约定） ---------------- */

static WXIME_CALL void *nc_create_net_api_context(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    NCLOG("create_net_api_context\n");
    return &g_ctx_dummy;
}

static WXIME_CALL int32_t nc_destroy_net_api_context(void *ctx)
{
    return 0;
}

static WXIME_CALL int32_t nc_net_initialize(void *ctx, void *param, void *user, init_cb cb)
{
    NCLOG("net_initialize user=%p cb=%p\n", user, (void *)cb);
    pthread_mutex_lock(&g_nc.mu);
    g_nc.init_user = user;
    g_nc.init_fn = cb;
    ensure_reader();
    pthread_mutex_unlock(&g_nc.mu);
    return 0;
}

static WXIME_CALL int32_t nc_net_start_task_with_option(void *ctx, int32_t cmd, int32_t sub, const void *body,
                                                        int32_t len, const void *opt, void *user, task_cb cb)
{
    uint32_t timeout = opt ? *(const uint32_t *)opt : 5000;
    if (timeout == 0 || timeout > 60000)
        timeout = 5000;
    /* 上屏内容上报（17；91 = ReportEmitInput，带拼音和上屏文字，2026-11-16 抓到）、日志上报：不出本进程 */
    if ((cmd == 4814 && (sub == 17 || sub == 91)) || cmd == 4959) {
        pthread_mutex_lock(&g_nc.mu);
        g_nc.stats_blocked++;
        pthread_mutex_unlock(&g_nc.mu);
        NCLOG("不发送 cmd=%d sub=%d（上报类请求）\n", cmd, sub);
        return -1;
    }
    pthread_mutex_lock(&g_nc.mu);
    if (!g_nc.enabled && !(g_nc.sync && sync_sub(cmd, sub))) {
        pthread_mutex_unlock(&g_nc.mu);
        NCLOG("云输入/词库同步未开启，拒绝 cmd=%d sub=%d\n", cmd, sub);
        return -1;
    }
    if (g_nc.fd < 0)
        g_nc.fd = connect_netd();
    if (g_nc.fd < 0 || g_nc.npend >= MAXPEND || len < 0) {
        pthread_mutex_unlock(&g_nc.mu);
        NCLOG("netd 不可用，cmd=%d sub=%d 失败\n", cmd, sub);
        return -1;
    }
    ensure_reader();
    int32_t task = g_nc.next_task++;
    if (g_nc.next_task <= 0)
        g_nc.next_task = 1;
    uint32_t hdr[5] = { 16 + (uint32_t)len, (uint32_t)task, (uint32_t)cmd, (uint32_t)sub, timeout };
    g_nc.pend[g_nc.npend].task = task;
    g_nc.pend[g_nc.npend].user = user;
    g_nc.pend[g_nc.npend].cb = cb;
    g_nc.pend[g_nc.npend].cos = 0;
    g_nc.npend++;
    int ok = write_full(g_nc.fd, hdr, sizeof(hdr)) == 0 && (len == 0 || write_full(g_nc.fd, body, (size_t)len) == 0);
    if (!ok) {
        void *u;
        task_cb c;
        take_pending(task, &u, &c, NULL);
        if (g_nc.fd >= 0)
            shutdown(g_nc.fd, SHUT_RDWR);   /* 让 reader_main 统一关 FD 并失败其余等待中的任务 */
        pthread_mutex_unlock(&g_nc.mu);
        return -1;
    }
    g_nc.stats_sent++;
    pthread_mutex_unlock(&g_nc.mu);
    NCLOG("任务 %d：cmd=%d sub=%d len=%d timeout=%u\n", task, cmd, sub, len, timeout);
    return task;
}

int32_t netcompat_cos(int upload, const char *url, int url_len, const char *path, int path_len)
{
    if (!url || !path || url_len <= 0 || path_len <= 0)
        return -1;
    pthread_mutex_lock(&g_nc.mu);
    if (!g_nc.sync) {
        pthread_mutex_unlock(&g_nc.mu);
        NCLOG("词库同步未开启，拒绝 COS %s\n", upload ? "上传" : "下载");
        return -1;
    }
    if (g_nc.fd < 0)
        g_nc.fd = connect_netd();
    if (g_nc.fd < 0 || g_nc.npend >= MAXPEND) {
        pthread_mutex_unlock(&g_nc.mu);
        return -1;
    }
    ensure_reader();
    int32_t task = g_nc.next_task++;
    if (g_nc.next_task <= 0)
        g_nc.next_task = 1;
    uint32_t blen = (uint32_t)url_len + 1 + (uint32_t)path_len;
    uint32_t hdr[5] = { 16 + blen, (uint32_t)task, (uint32_t)(upload ? -100 : -101), 0, 120000 };
    g_nc.pend[g_nc.npend].task = task;
    g_nc.pend[g_nc.npend].user = NULL;
    g_nc.pend[g_nc.npend].cb = NULL;
    g_nc.pend[g_nc.npend].cos = 1;
    g_nc.npend++;
    static const char nul = 0;
    int ok = write_full(g_nc.fd, hdr, sizeof(hdr)) == 0 && write_full(g_nc.fd, url, (size_t)url_len) == 0 &&
             write_full(g_nc.fd, &nul, 1) == 0 && write_full(g_nc.fd, path, (size_t)path_len) == 0;
    if (!ok) {
        void *u;
        task_cb c;
        take_pending(task, &u, &c, NULL);
        if (g_nc.fd >= 0)
            shutdown(g_nc.fd, SHUT_RDWR);   /* 让 reader_main 统一关 FD 并失败其余等待中的任务 */
        pthread_mutex_unlock(&g_nc.mu);
        return -1;
    }
    pthread_mutex_unlock(&g_nc.mu);
    NCLOG("任务 %d：COS %s %.*s\n", task, upload ? "上传" : "下载", path_len, path);
    return task;
}

static WXIME_CALL int32_t nc_ws_task(void *a, void *b, void *c, void *d, void *e, void *f, void *g, void *h)
{
    NCLOG("net_start_custom_host_ws_task_with_auth（暂不支持）\n");
    return -1;
}

static void *lookup(const char *name)
{
    static const struct { const char *n; void *f; } t[] = {
        { "create_net_api_context", (void *)nc_create_net_api_context },
        { "destroy_net_api_context", (void *)nc_destroy_net_api_context },
        { "net_initialize", (void *)nc_net_initialize },
        { "net_start_task_with_option", (void *)nc_net_start_task_with_option },
        { "net_start_custom_host_ws_task_with_auth", (void *)nc_ws_task },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strcmp(t[i].n, name))
            return t[i].f;
    return NULL;
}

void netcompat_install(void)
{
    pe_register_host_dll("ime_net.dll", lookup);
}
