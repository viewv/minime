/* engined.c — 原生常驻服务：在 Linux 进程里装载微信输入法引擎，对前端提供会话。
 *
 * 对前端说的是现有插件已经会的协议（wetype.commands 的子集，见 开发笔记（未公开）），
 * 帧格式和 Wine 栈的 bridge 一样：uint32 LE 长度 + 负载，连接建立后服务先发 "PID:<n>"。
 * fcitx5 插件优先连 Unix socket，连不上再连 Wine 栈的 TCP 17890。
 *
 * 监听：
 *   Unix socket  $XDG_RUNTIME_DIR/minime/engine.sock（目录 0700，只接受同一用户）
 *   TCP          127.0.0.1:$MINIME_TCP_PORT（可选，默认关闭。给只会 TCP 的旧插件和测试用；
 *                本机任何用户都能连 TCP 回环端口，所以不默认开）
 *
 * 一条连接 = 一个引擎会话。引擎的结果在它自己的线程里经监听回调送来，
 * 回调里直接编码成响应写回该连接（每条连接一把写锁）。
 *
 * 命令和引擎调用的对应（照官方 wetype_server 的 WximeImpl::Impl）：
 *   CREATE_SESSION            -> wxime_create_session + add_session_listener
 *   SEND_KEY TEXT_INPUT       -> wxime_process_input(字符的 UTF-8)
 *   SEND_KEY BACKSPACE        -> wxime_drop_last
 *   SEND_KEY ESCAPE           -> wxime_reset_session
 *   SELECT_CANDIDATE(commit)  -> wxime_select_candidate(文字, id)
 *   STR_SEND_TEXT_AROUND_CURSOR(104，字段 112 光标前 / 113 光标后 / 114 原因)
 *                             -> wxime_set_text_around_cursor，同时存下来给 e2e 当上下文
 * 引擎事件到响应：
 *   事件 0 候选   -> type=2 candidate_data
 *   事件 5 预编辑 -> type=3 pending_data
 *   事件 2 上屏   -> type=1 emit_str
 *
 * e2e 整句候选（可选，docs/whole-sentence.md）：找到 decgnr.bin 和 mpd_e2e.bin 时启用。按键后引擎的候选照常立即发出；
 * 另一个线程在后台做拼音约束解码（几十毫秒），有结果就把整句插进候选列表再发一次。新按键会取消旧的解码。
 * 选中 e2e 候选时由服务自己上屏并重置引擎会话，再在后台的隐藏会话里让引擎自己把这句"选一次"，
 * 引擎因此学到并写进用户词库（MINIME_E2E_LEARN=0 关闭）。
 *
 * 运行: engined <安装目录(含 libwxhld.dll 和 BuildIn)> <工作目录(用户词库等)>
 * 环境变量: MINIME_DEBUG=1|2 打印按键/事件，MINIME_ENGINE_LOG=1 打印引擎日志（capi 回调），
 *           MINIME_ENGINE_STDERR=1 放出引擎自己的 stderr（loguru 日志，用户词库等问题看这里），
 *           MINIME_TRACE_FILES=<路径片段> 打印引擎对这些路径的文件操作（装载器），
 *           MINIME_MAX_CANDS 每次按键先给前端的候选数（默认 50，翻到末尾再取），MINIME_TCP_PORT 见上。
 *   e2e：MINIME_E2E=0 关闭；MINIME_E2E_DIR 模型目录（默认 <安装目录>/BuildIn）；
 *        MINIME_E2E_BEAM（默认 4）、MINIME_E2E_THREADS（默认 4）、MINIME_E2E_MS 超时（默认 300）、
 *        MINIME_E2E_MIN 最少字母数（默认 4）、MINIME_E2E_POS 固定插入位置（默认 -1：按下面的规则放第 1 或第 2 位）、
 *        MINIME_E2E_PTOP（默认 0.3）、MINIME_E2E_MARGIN（默认 3.0）：e2e 首选的整句概率 >= PTOP，
 *        或者模型给它的分数比引擎首选高出 MARGIN 时放第 1 位，否则第 2 位（依据见 docs/whole-sentence.md"出候选策略"）；
 *        简拼（切不成完整音节的输入，如 nh、wxsrf）：MINIME_E2E_JIANPIN=0 关闭，MINIME_E2E_MIN_JP 最少字母数（默认 2），
 *        MINIME_E2E_JP_PENALTY 每个缩写音节扣分（默认 1.0）；简拼的 e2e 结果一律放第 1 位（引擎的简拼首选较弱）。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include "e2e.h"
#include "engine.h"
#include "netcompat.h"
#include "platform.h"

/* ---------------- 协议常量 ---------------- */
enum { CMD_MORE_CANDIDATES = 201 };     /* 自定义：前端翻到已有候选的末尾时要后面的（官方协议没有） */
enum { CMD_BLOCK_CANDIDATE = 205 };     /* 自定义：删词（候选放在字段 5，和选词一样），加进引擎的用户屏蔽表 */
enum { CMD_SET_CONFIG = 202 };          /* 自定义：前端配置（字段 300 模糊音位掩码、301 繁体、302 整句候选） */
enum { CMD_SEND_KEY = 1, CMD_SELECT_CANDIDATE = 2, CMD_CREATE_SESSION = 3, CMD_DESTROY_SESSION = 4,
       CMD_TEXT_AROUND_CURSOR = 104 };
enum { CTX_MAX_CHARS = 32 };    /* 给 e2e 的上下文：光标前最后一行的最后这么多个字 */
enum { KEY_SPACE = 4, KEY_ENTER = 5, KEY_ESCAPE = 10, KEY_BACKSPACE = 12, KEY_TEXT_INPUT = 64 };
enum { RESP_EMIT = 1, RESP_CANDIDATES = 2, RESP_PENDING = 3 };

static int g_debug;
static int g_max_cands = 50;      /* 每次按键先取这么多候选，前端翻到末尾再要（CMD_MORE_CANDIDATES，每次 100 个） */
enum { MORE_BATCH = 100 };

/* e2e 设置 */
static E2EModel *g_e2e;
static int g_e2e_beam = 4, g_e2e_ms = 300, g_e2e_min = 4, g_e2e_pos = -1;
static float g_e2e_ptop = 0.3f, g_e2e_margin = 3.0f;
/* 简拼结果抢第一的条件：每个字母的平均分数不低于 -g_e2e_jp_per（正常简拼 ≤ 1.1/字母，
 * 错拼被当成简拼硬拼成怪句时 ≥ 2/字母，例如 zhonguo → 中偶你过 -15.3/7） */
static float g_e2e_jp_per = 1.5f;
static int g_e2e_jp = 1, g_e2e_min_jp = 2;
static int g_e2e_learn = 1;     /* 选中 e2e 结果后让引擎学习 */
static int g_e2e_user = 1;      /* 前端配置里的"整句候选"开关 */
static float g_e2e_jp_pen = 1.0f;
static const char E2E_ID[] = "\x01minime-e2e";      /* e2e 候选的 id，选词时据此识别 */
static const char HOT_ID[] = "\x01minime-hot";      /* 常用语候选的 id */
static const char FUNC_ID[] = "\x01minime-fn";      /* 展开后的日期/时间候选的 id */

/* ---------------- 常用语（跨设备同步） ----------------
 * minime-netd 把同步组里的常用语写成 ~/.local/share/minime/net/hot_words.tsv
 * （每行 `快捷字母<TAB>短语`，短语里 \\ \t \n 转义）。输入的字母恰好等于某个快捷字母时，
 * 短语排第一个候选。文件最多每秒检查一次，变了就重载；回调线程和主线程都会查，用锁保护。 */
static struct {
    pthread_mutex_t mu;
    struct HotWord { char key[64]; char *text; } *v;
    int n;
    struct timespec mtime;
    off_t size;
    double next_check;
    char path[512];
} g_hw = { .mu = PTHREAD_MUTEX_INITIALIZER };

static void hw_unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static void hw_reload_locked(void)
{
    double now = plat_now_us();
    if (now < g_hw.next_check)
        return;
    g_hw.next_check = now + 1e6;
    if (!g_hw.path[0]) {
        const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
        if (x && x[0])
            snprintf(g_hw.path, sizeof(g_hw.path), "%s/minime/net/hot_words.tsv", x);
        else
            snprintf(g_hw.path, sizeof(g_hw.path), "%s/.local/share/minime/net/hot_words.tsv", h ? h : "");
    }
    struct stat st;
    int ok = stat(g_hw.path, &st) == 0;
    if (ok && st.st_mtim.tv_sec == g_hw.mtime.tv_sec && st.st_mtim.tv_nsec == g_hw.mtime.tv_nsec &&
        st.st_size == g_hw.size)
        return;
    for (int i = 0; i < g_hw.n; i++)
        free(g_hw.v[i].text);
    free(g_hw.v);
    g_hw.v = NULL;
    g_hw.n = 0;
    memset(&g_hw.mtime, 0, sizeof(g_hw.mtime));
    g_hw.size = 0;
    if (!ok)
        return;
    g_hw.mtime = st.st_mtim;
    g_hw.size = st.st_size;
    FILE *f = fopen(g_hw.path, "r");
    if (!f)
        return;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        if (line[len - 1] == '\n')
            line[--len] = 0;
        char *tab = strchr(line, '\t');
        if (!tab || tab == line || (size_t)(tab - line) >= sizeof(g_hw.v[0].key) || !tab[1])
            continue;
        *tab = 0;
        g_hw.v = realloc(g_hw.v, sizeof(*g_hw.v) * (size_t)(g_hw.n + 1));
        struct HotWord *w = &g_hw.v[g_hw.n++];
        snprintf(w->key, sizeof(w->key), "%s", line);
        w->text = strdup(tab + 1);
        hw_unescape(w->text);
    }
    free(line);
    fclose(f);
    if (g_debug)
        fprintf(stderr, "[engined] 常用语 %d 条\n", g_hw.n);
}

/* 字母恰好是某个快捷字母时把短语拷到 out，返回 1 */
static int hw_lookup(const char *letters, char *out, size_t outn)
{
    int found = 0;
    pthread_mutex_lock(&g_hw.mu);
    hw_reload_locked();
    for (int i = 0; i < g_hw.n && !found; i++) {
        if (!strcmp(g_hw.v[i].key, letters)) {
            snprintf(out, outn, "%s", g_hw.v[i].text);
            found = 1;
        }
    }
    pthread_mutex_unlock(&g_hw.mu);
    return found;
}

/* ---------------- 云输入（实验，默认关） ----------------
 * 打开后：网络兼容层放行，用 minime-netd 的匿名设备身份调 wxime_network_login，
 * 之后引擎自己发 cmd 4329（云拼音 / 配置）等请求，经 netd 发到服务器。
 * 开关：MINIME_CLOUD=1，或 minime.conf 的 [Network] CloudInput=True。 */
static int g_cloud_active;      /* 云输入已打开（隐私输入框里临时关掉网络兼容层） */

/* minime.conf 的 [Network] 段里某个开关（True/False），读不到算关 */
static int net_conf(const char *key)
{
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    char p[1024];
    if (x && *x)
        snprintf(p, sizeof(p), "%s/fcitx5/conf/minime.conf", x);
    else
        snprintf(p, sizeof(p), "%s/.config/fcitx5/conf/minime.conf", h ? h : "");
    FILE *f = fopen(p, "r");
    if (!f)
        return 0;
    char line[512];
    int in_net = 0, on = 0;
    size_t kl = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '[')
            in_net = !strncmp(line, "[Network]", 9);
        else if (in_net && !strncmp(line, key, kl) && line[kl] == '=')
            on = !strncmp(line + kl + 1, "True", 4);
    }
    fclose(f);
    return on;
}

/* 从 netd 的身份文件里取一个字符串字段（简单 JSON，值不含引号） */
static int identity_field(const char *key, char *out, size_t n)
{
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    char p[1024];
    if (x && *x)
        snprintf(p, sizeof(p), "%s/minime/net/identity.json", x);
    else
        snprintf(p, sizeof(p), "%s/.local/share/minime/net/identity.json", h ? h : "");
    FILE *f = fopen(p, "r");
    if (!f)
        return 0;
    char buf[4096];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    char *k = strstr(buf, pat);
    char *q1 = k ? strchr(k + strlen(pat), '"') : NULL;
    char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
    if (!q2 || (size_t)(q2 - q1 - 1) >= n)
        return 0;
    memcpy(out, q1 + 1, (size_t)(q2 - q1 - 1));
    out[q2 - q1 - 1] = 0;
    return 1;
}

static int cloud_enabled(void)
{
    if (getenv("MINIME_CLOUD"))
        return atoi(getenv("MINIME_CLOUD")) != 0;
    return net_conf("CloudInput");
}

/* ---------------- 同步个人词库（引擎的 UserDictSyncer，minime-netd 触发） ----------------
 * netd 连上/定期/收到推送时经控制通道发 `group <id> 4 0 0`，引擎自己发 4814/41、42、44、60 并要求
 * 上传/下载全量包；全量包走这里注册的 COS 接口，由 netd 实际传输。开关：[Network] Enabled + UserDictSync。 */
static int32_t cos_job(int upload, const WximeCosParam *p)
{
    int32_t t = netcompat_cos(upload, p->url.ptr, p->url.len, p->file_path.ptr, p->file_path.len);
    fprintf(stderr, "[engined] 词库同步：COS %s %.*s -> 任务 %d\n", upload ? "上传" : "下载", p->file_path.len,
            p->file_path.ptr ? p->file_path.ptr : "", t);
    return t;
}
static WXIME_CALL int32_t cos_download(const WximeCosParam *p) { return cos_job(0, p); }
static WXIME_CALL int32_t cos_upload(const WximeCosParam *p) { return cos_job(1, p); }
static void cos_done(int32_t job, int32_t result)
{
    fprintf(stderr, "[engined] 词库同步：COS 任务 %d %s\n", job, result == 0 ? "完成" : "失败");
    if (wx.client_cos_job_completed)
        wx.client_cos_job_completed(job, result);
}

static void dict_sync_setup(void)
{
    if (!wx.register_client_cos_api || !wx.client_cos_job_completed)
        return;
    static const WximeCosApi api = { cos_download, cos_upload, NULL, NULL };
    netcompat_set_cos_done(cos_done);
    wx.register_client_cos_api(&api);
}

/* 主循环定期调用：跟随配置打开 / 关闭个人词库同步的网络放行 */
static void dict_sync_poll(void)
{
    static double next;
    static int last = -1;
    double now = plat_now_us();
    if (now < next)
        return;
    next = now + 2e6;
    int on = net_conf("Enabled") && net_conf("UserDictSync");
    if (on == last)
        return;
    last = on;
    netcompat_set_sync(on);
    fprintf(stderr, "[engined] 同步个人词库：%s\n", on ? "打开" : "关闭");
}

/* ---------------- AB 参数（服务端 4814/28 下发的 mmkb_cli_engine_*） ----------------
 * minime-netd 拉到后写进 ~/.local/share/minime/net/ab_engine.tsv（键\t值），这里整份交给
 * wxime_reset_cli_ab_test_info。它是整体替换，所以每次都连同云结果的等待上限一起给。
 * 官方 Windows 客户端就是这样运行的（纠错检索 searcher_reduce_typo / rotate_typo、PC 新检索图等）。
 * MINIME_AB_OFF=1 只给等待上限（对照用），MINIME_AB_FILE 换文件。 */
static char g_ab_path[1024];

static const char *ab_path(void)
{
    if (!g_ab_path[0]) {
        const char *x = getenv("MINIME_AB_FILE"), *d = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
        if (x && *x)
            snprintf(g_ab_path, sizeof(g_ab_path), "%s", x);
        else if (d && *d)
            snprintf(g_ab_path, sizeof(g_ab_path), "%s/minime/net/ab_engine.tsv", d);
        else
            snprintf(g_ab_path, sizeof(g_ab_path), "%s/.local/share/minime/net/ab_engine.tsv", h ? h : "");
    }
    return g_ab_path;
}

static void ab_apply(void)
{
    if (!wx.reset_ab)
        return;
    enum { MAXAB = 64 };
    static char *keys[MAXAB], *vals[MAXAB];
    static WximeAbItem it[MAXAB];
    static WximeAbExpt ex;
    for (int i = 0; i < MAXAB; i++) {
        free(keys[i]);
        free(vals[i]);
        keys[i] = vals[i] = NULL;
    }
    int n = 0;
    FILE *f = getenv("MINIME_AB_OFF") ? NULL : fopen(ab_path(), "r");
    if (f) {
        char line[2048];
        while (n < MAXAB - 2 && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *tab = strchr(line, '\t');
            if (!tab || strncmp(line, "mmkb_cli_engine_", 16) != 0)
                continue;
            *tab = 0;
            /* 云结果的等待上限由我们自己定（下面），不用服务端按国内网络定的 200–300 ms */
            if (!strcmp(line, "mmkb_cli_engine_cloud_time_out_by_input_length") ||
                !strcmp(line, "mmkb_cli_engine_cloud_time_out_ms"))
                continue;
            keys[n] = strdup(line);
            vals[n] = strdup(tab + 1);
            n++;
        }
        fclose(f);
    }
    int from_file = n;
    /* 云结果的等待上限：海外往返常常 300 ms 以上，按服务端给的 200–300 ms 结果到了也被丢掉。
     * 调大到 MINIME_CLOUD_TIMEOUT_MS（默认 800）；云输入没开时这两项不起作用 */
    int ms = getenv("MINIME_CLOUD_TIMEOUT_MS") ? atoi(getenv("MINIME_CLOUD_TIMEOUT_MS")) : 800;
    char by_len[1024];
    size_t o = 0;
    for (int i = 1; i <= 40 && o + 16 < sizeof(by_len); i++)
        o += (size_t)snprintf(by_len + o, sizeof(by_len) - o, "%s%d,%d", i > 1 ? ";" : "", i, ms);
    char flat[16];
    snprintf(flat, sizeof(flat), "%d", ms);
    keys[n] = strdup("mmkb_cli_engine_cloud_time_out_by_input_length");
    vals[n++] = strdup(by_len);
    keys[n] = strdup("mmkb_cli_engine_cloud_time_out_ms");
    vals[n++] = strdup(flat);
    for (int i = 0; i < n; i++)
        it[i] = (WximeAbItem){ keys[i], (int32_t)strlen(keys[i]), 0, vals[i], (int32_t)strlen(vals[i]), 0 };
    ex = (WximeAbExpt){ 1, 0, it, n, 0 };
    WximeAbParam ab = { &ex, 1, 0 };
    wx.reset_ab(&ab);
    fprintf(stderr, "[engined] AB 参数：服务端 %d 项%s + 云等待上限 %d ms\n", from_file,
            from_file ? "" : "（没有 ab_engine.tsv）", ms);
}

/* 主循环定期调用：ab_engine.tsv 变了就重新灌 */
static void ab_poll(void)
{
    static double next;
    static time_t last_m = -1;
    double now = plat_now_us();
    if (now < next)
        return;
    next = now + 2e6;
    struct stat st;
    time_t m = stat(ab_path(), &st) == 0 ? st.st_mtime : 0;
    if (m == last_m)
        return;
    last_m = m;
    ab_apply();
}

/* 主循环定期调用：跟随配置打开 / 关闭云输入（登录只做一次） */
static void cloud_poll(void)
{
    static double next;
    static int logged_in, last = -1;
    double now = plat_now_us();
    if (now < next)
        return;
    next = now + 2e6;
    int on = cloud_enabled();
    if (on == last)
        return;
    last = on;
    g_cloud_active = 0;
    if (!on) {
        netcompat_set_enabled(0);
        fprintf(stderr, "[engined] 云输入：关闭\n");
        return;
    }
    if (logged_in) {
        g_cloud_active = 1;
        netcompat_set_enabled(1);
        fprintf(stderr, "[engined] 云输入：打开\n");
        return;
    }
    if (!wx.network_login) {
        fprintf(stderr, "[engined] 云输入：引擎没有 wxime_network_login\n");
        return;
    }
    static char code[128], name[128];
    if (!identity_field("device_code", code, sizeof(code)) || !identity_field("device_name", name, sizeof(name))) {
        fprintf(stderr, "[engined] 云输入：没有 netd 的设备身份（先在设置里打开联网）\n");
        last = -1;                              /* 身份还没生成：下次再试 */
        return;
    }
    g_cloud_active = 1;
    netcompat_set_enabled(1);
    static uint8_t lp[0x80];
    memset(lp, 0, sizeof(lp));
    /* 引擎日志对照得出：+0x20 channel_id，+0x30 device_code（app_ver / device_name /
     * system_version 用 wxime_initialize 给的） */
    static const char channel[] = "3";
    *(const char **)(lp + 0x20) = channel;
    *(int32_t *)(lp + 0x28) = 1;
    *(const char **)(lp + 0x30) = code;
    *(int32_t *)(lp + 0x38) = (int32_t)strlen(code);
    *(int32_t *)(lp + 0x40) = 3;
    int32_t r = wx.network_login(lp);
    logged_in = 1;
    ab_apply();                                 /* AB 参数整份重灌（含云结果的等待上限） */
    fprintf(stderr, "[engined] 云输入：打开，network_login -> %d（设备 %s）\n", r, name);
}

/* ---------------- 最小 protobuf 编码 ---------------- */
typedef struct { uint8_t *p; size_t n, cap; } Buf;

static void b_put(Buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void b_varint(Buf *b, uint64_t v)
{
    uint8_t t[10];
    int i = 0;
    while (v >= 0x80) {
        t[i++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    t[i++] = (uint8_t)v;
    b_put(b, t, i);
}
static void b_uint(Buf *b, int field, uint64_t v)
{
    b_varint(b, (uint64_t)field << 3);
    b_varint(b, v);
}
static void b_bytes(Buf *b, int field, const void *d, size_t n)
{
    b_varint(b, ((uint64_t)field << 3) | 2);
    b_varint(b, n);
    if (n)
        b_put(b, d, n);
}
static void b_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* ---------------- 最小 protobuf 解码 ---------------- */
typedef struct { const uint8_t *p, *end; } Rd;

static int r_varint(Rd *r, uint64_t *v)
{
    *v = 0;
    for (int s = 0; s < 64 && r->p < r->end; s += 7) {
        uint8_t c = *r->p++;
        *v |= (uint64_t)(c & 0x7f) << s;
        if (!(c & 0x80))
            return 1;
    }
    return 0;
}
/* 读下一个字段；长度型字段在 sub 里给出内容 */
static int r_field(Rd *r, int *field, int *wire, uint64_t *v, Rd *sub)
{
    uint64_t tag;
    if (r->p >= r->end || !r_varint(r, &tag))
        return 0;
    *field = (int)(tag >> 3);
    *wire = (int)(tag & 7);
    switch (*wire) {
    case 0: return r_varint(r, v);
    case 1: if (r->end - r->p < 8) return 0; r->p += 8; return 1;
    case 5: if (r->end - r->p < 4) return 0; r->p += 4; return 1;
    case 2:
        if (!r_varint(r, v) || (uint64_t)(r->end - r->p) < *v)
            return 0;
        sub->p = r->p;
        sub->end = r->p + *v;
        r->p += *v;
        return 1;
    }
    return 0;
}

/* ---------------- 连接 ---------------- */
typedef struct Conn {
    int fd;                    /* -1 表示已断开；回调里检查 */
    pthread_mutex_t wmu;       /* 写锁：引擎线程和连接线程都会写 */
    uint64_t sid;
    uint64_t last_ts;          /* 上一次给引擎的时间戳，保证递增 */
    int id;
    /* 最近一次发给前端的候选 id（每项 4 字节长度 + 内容），受 wmu 保护。
     * 选词只接受这里面的 id：引擎遇到非法 id 会抛 C++ 异常，而兼容层还不支持异常展开，
     * 一个出错的前端就会拖垮整个服务。 */
    Buf ids;
    /* 以下也受 wmu 保护 */
    struct Cand {
        char *text; uint8_t *id; uint32_t idn; char *comment; int cloud;
        int corrected;         /* 引擎纠错得来（拼写记录里有 xchg / insertion / skip / kb_neighbour / repeat prev） */
        int func;              /* 功能候选（日期/时间占位，ints[0]=65535、ints[3]=2048），前端显示前要展开 */
    } *cands;
    int ncands, capcands;
    void *iter;                /* 引擎最近一次的候选迭代器（留着给"要更多候选"用），wmu 保护 */
    int more;                  /* 迭代器里可能还有候选 */                /* 引擎最近一次给的候选（发给前端前要和 e2e 结果合并） */
    char letters[256];         /* 当前组合里输入的字母（给 e2e 用） */
    int nletters;
    int e2e_ok;                /* 0：这次组合里选过引擎的部分候选，剩下的拼音不再跑 e2e */
    uint64_t gen;              /* 组合每变一次加 1，过期的 e2e 结果据此丢弃 */
    char e2e_text[256];
    uint64_t e2e_gen;
    int e2e_first;             /* e2e 结果放第 1 位（否则第 2 位），e2e 线程按置信度和重排定 */
    char ctx[CTX_MAX_CHARS * 4 + 1];   /* 光标前文字（已截成最后一行的最后 CTX_MAX_CHARS 个字） */
    pthread_mutex_t snd;       /* 候选列表的发送互斥（引擎线程和 e2e 线程都会发） */
    int priv;                  /* 当前输入框标了敏感（隐私）：不学 e2e 整句、不发云请求（前端字段 303） */
} Conn;

static int conn_has_id(Conn *c, const uint8_t *id, uint32_t n)
{
    int found = 0;
    pthread_mutex_lock(&c->wmu);
    for (size_t off = 0; off + 4 <= c->ids.n;) {
        uint32_t l;
        memcpy(&l, c->ids.p + off, 4);
        if (l == n && memcmp(c->ids.p + off + 4, id, n) == 0) {
            found = 1;
            break;
        }
        off += 4 + l;
    }
    pthread_mutex_unlock(&c->wmu);
    return found;
}



static int write_all(int fd, const void *p, size_t n)
{
    const uint8_t *c = p;
    while (n) {
        ssize_t r = send(fd, c, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        c += r;
        n -= (size_t)r;
    }
    return 0;
}
static int read_all(int fd, void *p, size_t n)
{
    uint8_t *c = p;
    while (n) {
        ssize_t r = recv(fd, c, n, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        c += r;
        n -= (size_t)r;
    }
    return 0;
}

static void send_frame(Conn *c, const void *p, size_t n)
{
    uint8_t hdr[4] = { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24) };
    pthread_mutex_lock(&c->wmu);
    if (c->fd >= 0 && (write_all(c->fd, hdr, 4) < 0 || write_all(c->fd, p, n) < 0))
        shutdown(c->fd, SHUT_RDWR);   /* 让连接线程收尾 */
    pthread_mutex_unlock(&c->wmu);
}

/* 一个 RespBodyV1.Detail 包成 RespBase 发出去（插件每帧只取一个 detail） */
static void send_detail(Conn *c, Buf *detail)
{
    Buf body = {0}, base = {0};
    b_uint(&body, 1, 1);                        /* pid（插件不看） */
    b_bytes(&body, 2, detail->p, detail->n);
    b_uint(&base, 1, 1);                        /* version */
    b_bytes(&base, 2, body.p, body.n);
    send_frame(c, base.p, base.n);
    b_free(&body);
    b_free(&base);
}

static uint64_t next_ts(Conn *c)
{
    uint64_t t = wx_wall_ms();
    if (t <= c->last_ts)
        t = c->last_ts + 1;
    c->last_ts = t;
    return t;
}

/* ---------------- 引擎事件 -> 响应 ---------------- */
/* 从候选迭代器读至多 n 个追加到 out（容量自动扩），返回读到的个数 */
static int read_cands(void *iter, struct Cand **out, int *nout, int *cap, int n, int cid)
{
    WximeCandidate cand;
    int got = 0;
    memset(&cand, 0, sizeof(cand));
    while (got < n && wx.candidate_next(iter, &cand)) {
        if (*nout == *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *out = realloc(*out, sizeof(**out) * (size_t)*cap);
        }
        struct Cand *k = &(*out)[(*nout)++];
        k->text = strndup(cand.text ? cand.text : "", cand.text ? (size_t)cand.text_len : 0);
        k->idn = cand.id ? (uint32_t)cand.id_len : 0;
        k->id = malloc(k->idn ? k->idn : 1);
        if (k->idn)
            memcpy(k->id, cand.id, k->idn);
        k->comment = cand.comment && cand.comment_len > 0 ? strndup(cand.comment, (size_t)cand.comment_len) : NULL;
        k->cloud = 0;
        k->corrected = 0;
        if (cand.json_id_desc && cand.json_id_desc_len > 0) {
            static const char *const fix[] = { "\"type\":\"xchg\"", "\"type\":\"insertion\"", "\"type\":\"skip\"",
                                               "\"type\":\"kb_neighbour\"", "\"type\":\"repeat prev\"" };
            for (size_t j = 0; j < sizeof(fix) / sizeof(fix[0]) && !k->corrected; j++)
                k->corrected = memmem(cand.json_id_desc, (size_t)cand.json_id_desc_len, fix[j], strlen(fix[j])) != NULL;
        }
        k->func = cand.ints_0x44[0] == 65535 && cand.ints_0x44[3] == 2048;
        if (g_debug > 1)
            fprintf(stderr, "[engined] #%d   [%d] %s\n", cid, *nout - 1, k->text);
        if (getenv("MINIME_DUMP_CANDS") && got < 12) {
            fprintf(stderr, "[cand] %-16s ints:", k->text);
            for (int j = 0; j < 11; j++)
                fprintf(stderr, " %d", cand.ints_0x44[j]);
            fprintf(stderr, " json=%.*s\n", cand.json_id_desc ? cand.json_id_desc_len : 0,
                    cand.json_id_desc ? cand.json_id_desc : "");
        }
        got++;
        memset(&cand, 0, sizeof(cand));
    }
    return got;
}

static void clear_cands(Conn *c)
{
    for (int i = 0; i < c->ncands; i++) {
        free(c->cands[i].text);
        free(c->cands[i].id);
        free(c->cands[i].comment);
    }
    c->ncands = 0;
}

static int is_ascii_text(const char *s)
{
    return s && s[0] && (unsigned char)s[0] < 0x80;
}

/* ---------------- 学习：选中 e2e 整句后让引擎自己"选一次" ----------------
 * wxime_add_user_history 只是短期记忆（实测不写进用户词库，重启就没了），所以改用引擎自己的选词流程：
 * 在一个隐藏会话里把同样的字母重新打一遍，每次从候选里挑"正好是目标句开头一段"的最长候选选中，
 * 直到整句上屏——和用户手动逐段选词一样，引擎按自己的方式学习并写进用户词库。
 * 引擎只能在主线程调用，所以做成主循环驱动的状态机：隐藏会话的回调（引擎线程）只记下事件、
 * 写管道唤醒主循环，主循环调 learn_step 推进一步。队列只留最近几条。 */
enum { LEARN_MAXC = 200, LEARN_Q = 4, LEARN_TIMEOUT_MS = 500 };
enum { L_IDLE, L_TYPING, L_SELECTING };
static struct {
    pthread_mutex_t mu;                  /* 保护候选和事件计数（回调线程写） */
    uint64_t sid;
    int pipe[2];                         /* 回调 -> 主循环 */
    struct LCand { char *text; uint8_t *id; uint32_t idn; } cands[LEARN_MAXC];
    int ncands, ncand_ev, nemit_ev;
    /* 以下只在主线程用 */
    struct { char letters[256]; char text[512]; } q[LEARN_Q];
    int qn;
    int state, pos, c0, e0;              /* 打到第几个字母；发出动作时的事件数 */
    char letters[256], text[512];
    size_t done;                         /* 目标句已经选掉的字节数 */
    size_t pick;                         /* 这一步选的候选长度 */
    int steps;
    double t0, deadline;
    uint64_t last_ts;
} g_learn = { .mu = PTHREAD_MUTEX_INITIALIZER, .pipe = { -1, -1 } };

static WXIME_CALL void on_learn_events(uint64_t sid, const int32_t *types, void *const *datas,
                                       int32_t count, void *ud)
{
    (void)sid, (void)ud;
    int any = 0;
    for (int i = 0; i < count; i++) {
        if (types[i] == WXIME_EVT_CANDIDATE_LIST_UPDATE && datas[i]) {
            WximeCandidateListEvent *ev = datas[i];
            if (!ev->new_iterator)
                continue;
            pthread_mutex_lock(&g_learn.mu);
            for (int k = 0; k < g_learn.ncands; k++) {
                free(g_learn.cands[k].text);
                free(g_learn.cands[k].id);
            }
            int n = 0;
            WximeCandidate cand;
            memset(&cand, 0, sizeof(cand));
            while (n < LEARN_MAXC && wx.candidate_next((void *)ev->new_iterator, &cand)) {
                struct LCand *k = &g_learn.cands[n++];
                k->text = strndup(cand.text ? cand.text : "", cand.text ? (size_t)cand.text_len : 0);
                k->idn = cand.id ? (uint32_t)cand.id_len : 0;
                k->id = malloc(k->idn ? k->idn : 1);
                if (k->idn)
                    memcpy(k->id, cand.id, k->idn);
                memset(&cand, 0, sizeof(cand));
            }
            wx.delete_candidate_iterator((void *)ev->new_iterator);
            g_learn.ncands = n;
            g_learn.ncand_ev++;
            pthread_mutex_unlock(&g_learn.mu);
            any = 1;
        } else if (types[i] == WXIME_EVT_EMIT && datas[i]) {
            pthread_mutex_lock(&g_learn.mu);
            g_learn.nemit_ev++;
            pthread_mutex_unlock(&g_learn.mu);
            any = 1;
        }
    }
    if (any && g_learn.pipe[1] >= 0) {
        char b = 1;
        if (write(g_learn.pipe[1], &b, 1) < 0) {}
    }
}

static uint64_t learn_ts(void)
{
    uint64_t t = wx_wall_ms();
    if (t <= g_learn.last_ts)
        t = g_learn.last_ts + 1;
    return g_learn.last_ts = t;
}

static int g_learn_stale;      /* 会话选项变了，学习会话空闲时重建 */

static void learn_finish(int ok)
{
    /* 学成了要销毁会话：引擎上屏后的学习先暂存在会话里，reset 会把它丢掉（实测），destroy 才写进用户词库 */
    if (ok || g_learn_stale) {
        wx.destroy_session(g_learn.sid);
        g_learn.sid = wx_engine_new_session(on_learn_events, NULL);
        g_learn_stale = 0;
    } else {
        wx.reset_session(g_learn.sid);
    }
    if (g_debug)
        fprintf(stderr, "[engined] 学习 \"%s\"（%s）%s（%d 段，%.0f ms）\n", g_learn.text, g_learn.letters,
                ok ? "成功" : "失败：引擎候选里凑不出这句或超时", g_learn.steps, (plat_now_us() - g_learn.t0) / 1000);
    g_learn.state = L_IDLE;
}

/* 发一个动作（打字母或选候选）前记下事件数和超时 */
static void learn_arm(void)
{
    pthread_mutex_lock(&g_learn.mu);
    g_learn.c0 = g_learn.ncand_ev;
    g_learn.e0 = g_learn.nemit_ev;
    pthread_mutex_unlock(&g_learn.mu);
    g_learn.deadline = plat_now_us() + LEARN_TIMEOUT_MS * 1000.0;
}

/* 从当前候选里选"正好是剩下目标开头一段"的最长候选；没有返回 0 */
static int learn_select(void)
{
    const char *rest = g_learn.text + g_learn.done;
    pthread_mutex_lock(&g_learn.mu);
    int best = -1;
    size_t bl = 0;
    for (int k = 0; k < g_learn.ncands; k++) {
        size_t l = strlen(g_learn.cands[k].text);
        if (l > bl && !strncmp(rest, g_learn.cands[k].text, l))
            best = k, bl = l;
    }
    char *t = best >= 0 ? strdup(g_learn.cands[best].text) : NULL;
    uint32_t idn = best >= 0 ? g_learn.cands[best].idn : 0;
    uint8_t *id = malloc(idn ? idn : 1);
    if (best >= 0)
        memcpy(id, g_learn.cands[best].id, idn);
    pthread_mutex_unlock(&g_learn.mu);
    if (!t) {
        free(id);
        return 0;
    }
    learn_arm();
    g_learn.pick = bl;
    g_learn.steps++;
    g_learn.state = L_SELECTING;
    struct { uint64_t zero, now_ms; } extra = { 0, learn_ts() };
    wx.select_candidate(g_learn.sid, t, (uint32_t)bl, id, idn, NULL, 0, &extra, sizeof(extra), NULL, 0, NULL);
    free(t);
    free(id);
    return 1;
}

static void learn_type_next(void)
{
    learn_arm();
    g_learn.state = L_TYPING;
    struct { uint64_t zero, now_ms; } extra = { 0, learn_ts() };
    wx.process_input(g_learn.sid, g_learn.letters + g_learn.pos, 1, (const char *)&extra, sizeof(extra));
}

/* 主循环调用：有事件、超时或新任务时推进学习状态机 */
static void learn_step(void)
{
    char b[64];
    if (g_learn.pipe[0] >= 0)
        while (read(g_learn.pipe[0], b, sizeof(b)) > 0) {}
    for (int guard = 0; guard < 8; guard++) {
        if (g_learn.state == L_IDLE) {
            if (!g_learn.qn)
                return;
            memcpy(g_learn.letters, g_learn.q[0].letters, sizeof(g_learn.letters));
            memcpy(g_learn.text, g_learn.q[0].text, sizeof(g_learn.text));
            memmove(&g_learn.q[0], &g_learn.q[1], sizeof(g_learn.q[0]) * (size_t)(g_learn.qn - 1));
            g_learn.qn--;
            g_learn.pos = 0;
            g_learn.done = 0;
            g_learn.steps = 0;
            g_learn.t0 = plat_now_us();
            wx.reset_session(g_learn.sid);
            learn_type_next();
            return;
        }
        pthread_mutex_lock(&g_learn.mu);
        int cand = g_learn.ncand_ev > g_learn.c0, emit = g_learn.nemit_ev > g_learn.e0;
        pthread_mutex_unlock(&g_learn.mu);
        if (!cand && !emit) {
            if (plat_now_us() > g_learn.deadline)
                learn_finish(0);
            else
                return;                          /* 继续等 */
            continue;
        }
        if (g_learn.state == L_TYPING) {
            if (g_learn.letters[++g_learn.pos]) {
                learn_type_next();
                return;
            }
            if (!learn_select())
                learn_finish(0);
            else
                return;
            continue;
        }
        /* L_SELECTING */
        g_learn.done += g_learn.pick;
        if (emit) {
            learn_finish(g_learn.text[g_learn.done] == 0);
            continue;
        }
        if (!g_learn.text[g_learn.done] || g_learn.steps >= 40 || !learn_select())
            learn_finish(0);
        else
            return;
    }
}

/* 主循环 poll 的超时（毫秒）：学习在等事件时到超时为止，否则 -1 */
static int learn_poll_timeout(void)
{
    if (g_learn.state == L_IDLE)
        return -1;
    double ms = (g_learn.deadline - plat_now_us()) / 1000;
    return ms < 0 ? 0 : (int)ms + 1;
}

/* 选中了 e2e 整句（主线程）：排进学习队列（满了丢最早的），空闲时马上开始 */
static void learn_e2e(const char *letters, const char *text)
{
    if (!g_e2e_learn || !g_learn.sid || !letters[0] || !text[0])
        return;
    if (g_learn.qn == LEARN_Q) {
        memmove(&g_learn.q[0], &g_learn.q[1], sizeof(g_learn.q[0]) * (LEARN_Q - 1));
        g_learn.qn--;
    }
    snprintf(g_learn.q[g_learn.qn].letters, sizeof(g_learn.q[0].letters), "%s", letters);
    snprintf(g_learn.q[g_learn.qn].text, sizeof(g_learn.q[0].text), "%s", text);
    g_learn.qn++;
    learn_step();
}

/* 功能候选（引擎给「日期」「时间」「星期」占位，手机客户端换成当前值）：展开成几种格式，
 * 返回个数；不认识的占位返回 0（不显示，免得和普通候选重复） */
static int func_expand(const char *ph, char out[][64], int max)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    static const char *const wd[] = { "日", "一", "二", "三", "四", "五", "六" };
    int n = 0;
#define ADD(...) do { if (n < max) snprintf(out[n++], 64, __VA_ARGS__); } while (0)
    if (strstr(ph, "日期")) {
        ADD("%d年%d月%d日", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        ADD("%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        ADD("%d月%d日 星期%s", tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
    } else if (strstr(ph, "时间")) {
        int h12 = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
        ADD("%02d:%02d", tm.tm_hour, tm.tm_min);
        ADD("%s%d:%02d", tm.tm_hour < 12 ? "上午" : "下午", h12, tm.tm_min);
        ADD("%04d-%02d-%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    } else if (strstr(ph, "星期")) {
        ADD("星期%s", wd[tm.tm_wday]);
    }
#undef ADD
    return n;
}

/* 删过的词记一笔（~/.local/share/minime/blocked_words.txt，每行一个），以后设置里可以恢复 */
static void blocked_log(const char *w, size_t n)
{
    const char *d = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    char p[1024];
    if (d && *d)
        snprintf(p, sizeof(p), "%s/minime/blocked_words.txt", d);
    else
        snprintf(p, sizeof(p), "%s/.local/share/minime/blocked_words.txt", h ? h : "");
    FILE *f = fopen(p, "a");
    if (!f)
        return;
    fprintf(f, "%.*s\n", (int)n, w);
    fclose(f);
}

/* 按引擎候选 + e2e 结果拼出候选列表发给前端，同时记下可选的 id（调用者不持有 wmu） */
static void send_candidates(Conn *c)
{
    pthread_mutex_lock(&c->snd);
    pthread_mutex_lock(&c->wmu);
    int use_e2e = c->e2e_text[0] && c->e2e_gen == c->gen && c->e2e_ok && c->ncands > 0 &&
                  !is_ascii_text(c->cands[0].text) && strcmp(c->e2e_text, c->cands[0].text) != 0;
    int pos = g_e2e_pos >= 0 ? g_e2e_pos : (c->e2e_first ? 0 : 1);
    if (pos > c->ncands)
        pos = c->ncands;
    Buf list = {0}, ids = {0};
    int out = 0;
    /* 常用语：快捷字母完全匹配时排第一 */
    char hw[1024];
    int use_hw = 0;
    if (c->nletters > 0) {
        char letters[256];
        memcpy(letters, c->letters, (size_t)c->nletters);
        letters[c->nletters] = 0;
        use_hw = hw_lookup(letters, hw, sizeof(hw));
    }
    if (use_hw) {
        Buf w = {0};
        uint32_t idn = (uint32_t)sizeof(HOT_ID) - 1;
        b_bytes(&w, 1, hw, strlen(hw));
        b_bytes(&w, 2, HOT_ID, idn);
        b_bytes(&w, 9, "⬡", strlen("⬡"));      /* 前端显示成候选后的小标记（手机上是小立方体） */
        b_put(&ids, &idn, 4);
        b_put(&ids, HOT_ID, idn);
        b_bytes(&list, 1, w.p, w.n);
        b_free(&w);
        if (use_e2e && !strcmp(c->e2e_text, hw))
            use_e2e = 0;
    }
    for (int i = 0; i <= c->ncands; i++) {
        if (use_e2e && out == pos) {
            Buf w = {0};
            uint32_t idn = (uint32_t)sizeof(E2E_ID) - 1;
            b_bytes(&w, 1, c->e2e_text, strlen(c->e2e_text));
            b_bytes(&w, 2, E2E_ID, idn);
            b_bytes(&w, 9, "✦", strlen("✦"));    /* 整句候选的小标记（前端只给服务合成的候选显示） */
            b_put(&ids, &idn, 4);
            b_put(&ids, E2E_ID, idn);
            b_bytes(&list, 1, w.p, w.n);
            b_free(&w);
            out++;
        }
        if (i == c->ncands)
            break;
        struct Cand *k = &c->cands[i];
        if (k->func) {          /* 日期/时间：展开成当前值，选中时服务自己上屏 */
            char fx[4][64];
            int nf = func_expand(k->text, fx, 4);
            for (int j = 0; j < nf; j++) {
                Buf w = {0};
                uint32_t idn = (uint32_t)sizeof(FUNC_ID) - 1;
                b_bytes(&w, 1, fx[j], strlen(fx[j]));
                b_bytes(&w, 2, FUNC_ID, idn);
                b_put(&ids, &idn, 4);
                b_put(&ids, FUNC_ID, idn);
                b_bytes(&list, 1, w.p, w.n);
                b_free(&w);
                out++;
            }
            continue;
        }
        if (use_e2e && !strcmp(k->text, c->e2e_text))
            continue;           /* 和 e2e 结果相同的引擎候选不重复列 */
        if (use_hw && !strcmp(k->text, hw))
            continue;           /* 和常用语相同的也不重复 */
        Buf w = {0};
        b_bytes(&w, 1, k->text, strlen(k->text));
        b_bytes(&w, 2, k->id, k->idn);
        b_put(&ids, &k->idn, 4);
        if (k->idn)
            b_put(&ids, k->id, k->idn);
        if (k->cloud)
            b_bytes(&w, 9, "☁", strlen("☁"));   /* 云端带来的候选，前端显示小云朵 */
        else if (k->comment)
            b_bytes(&w, 9, k->comment, strlen(k->comment));
        b_bytes(&list, 1, w.p, w.n);
        b_free(&w);
        out++;
    }
    b_free(&c->ids);
    c->ids = ids;
    int more = c->more;
    pthread_mutex_unlock(&c->wmu);
    b_uint(&list, 2, 0);                        /* focused */
    if (more)
        b_uint(&list, 3, 1);                    /* 自定义：后面还有候选，前端翻到末尾时发 CMD_MORE_CANDIDATES */
    Buf d = {0};
    b_uint(&d, 1, RESP_CANDIDATES);
    b_bytes(&d, 2, list.p, list.n);
    send_detail(c, &d);
    b_free(&d);
    b_free(&list);
    pthread_mutex_unlock(&c->snd);
}

/* from_cloud：事件 6（云结果并进来之后的列表）。和上一份本地列表比，新出现的候选来自云端 */
static void on_candidates(Conn *c, WximeCandidateListEvent *ev, int from_cloud)
{
    if (!ev->new_iterator)
        return;
    /* 先取前 g_max_cands 个；迭代器留着，前端翻到末尾时再接着取（CMD_MORE_CANDIDATES） */
    struct Cand *tmp = NULL;
    int n = 0, cap = 0;
    int got = read_cands((void *)ev->new_iterator, &tmp, &n, &cap, g_max_cands, c->id);
    if (g_debug)
        fprintf(stderr, "[engined] #%d 候选 %d 个%s%s\n", c->id, n, n ? "，首选 " : "", n ? tmp[0].text : "");
    pthread_mutex_lock(&c->wmu);
    if (from_cloud) {
        int marked = 0;
        for (int i = 0; i < n; i++) {
            int seen = 0;
            for (int j = 0; j < c->ncands && !seen; j++)
                seen = !strcmp(tmp[i].text, c->cands[j].text) && !c->cands[j].cloud;
            tmp[i].cloud = !seen;
            marked += tmp[i].cloud;
        }
        if (g_debug)
            fprintf(stderr, "[engined] #%d 云候选 %d 个\n", c->id, marked);
    }
    clear_cands(c);
    free(c->cands);
    c->cands = tmp;
    c->ncands = n;
    c->capcands = cap;
    if (c->iter)
        wx.delete_candidate_iterator(c->iter);
    c->iter = (void *)ev->new_iterator;
    c->more = got == g_max_cands;
    pthread_mutex_unlock(&c->wmu);
    send_candidates(c);
}

/* 前端翻到已有候选的末尾：从留着的迭代器再取一批 */
static void more_candidates(Conn *c)
{
    pthread_mutex_lock(&c->wmu);
    int got = 0;
    if (c->iter && c->more) {
        got = read_cands(c->iter, &c->cands, &c->ncands, &c->capcands, MORE_BATCH, c->id);
        c->more = got == MORE_BATCH;
    }
    int n = c->ncands;
    pthread_mutex_unlock(&c->wmu);
    if (g_debug)
        fprintf(stderr, "[engined] #%d 再取候选 %d 个，共 %d 个\n", c->id, got, n);
    send_candidates(c);
}

/* 取光标前文字的最后一行、最后 CTX_MAX_CHARS 个字（按 UTF-8 字符计）写进 out */
static void trim_context(const char *s, size_t n, char *out, size_t cap)
{
    size_t start = 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] == '\n' || s[i] == '\r')
            start = i + 1;
    size_t chars = 0, i = n;
    while (i > start && chars < CTX_MAX_CHARS) {
        i--;
        if (((unsigned char)s[i] & 0xc0) != 0x80)
            chars++;
    }
    size_t len = n - i;
    if (len >= cap)
        len = cap - 1;
    memcpy(out, s + i, len);
    out[len] = 0;
}

/* 组合结束（上屏、清空）：e2e 状态复位 */
static void comp_reset(Conn *c)
{
    pthread_mutex_lock(&c->wmu);
    c->nletters = 0;
    c->e2e_ok = 1;
    c->gen++;
    pthread_mutex_unlock(&c->wmu);
}

/* ---------------- e2e 后台线程 ---------------- */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    Conn *c;
    uint64_t gen;
    char letters[256];
    char ctx[CTX_MAX_CHARS * 4 + 1];
    int has;
    volatile int cancel;
    uint32_t fuzzy;            /* 前端配置的模糊音（e2e 线程在两次解码之间换表） */
    int fuzzy_dirty;
    int traditional;           /* 出繁体：e2e 结果用 OpenCC 转 */
} g_job = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

/* OpenCC（dlopen，可选）：繁体模式下 e2e 的输入转简体、输出转繁体（s2tw，和引擎的繁体字形一致） */
static struct {
    int tried, ok;
    void *s2t, *t2s;
    void *(*open_)(const char *);
    char *(*conv)(void *, const char *, size_t);
    void (*free_)(char *);
} g_cc;

static int cc_init(void)
{
    if (g_cc.tried)
        return g_cc.ok;
    g_cc.tried = 1;
    void *lib = dlopen("libopencc.so.1.4", RTLD_NOW);
    if (!lib) lib = dlopen("libopencc.so", RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "[engined] 没有 libopencc，繁体模式下不出 e2e 整句候选\n");
        return 0;
    }
    g_cc.open_ = (void *(*)(const char *))dlsym(lib, "opencc_open");
    g_cc.conv = (char *(*)(void *, const char *, size_t))dlsym(lib, "opencc_convert_utf8");
    g_cc.free_ = (void (*)(char *))dlsym(lib, "opencc_convert_utf8_free");
    if (!g_cc.open_ || !g_cc.conv || !g_cc.free_)
        return 0;
    g_cc.s2t = g_cc.open_("s2tw.json");
    g_cc.t2s = g_cc.open_("tw2s.json");
    g_cc.ok = g_cc.s2t != (void *)-1 && g_cc.t2s != (void *)-1;
    if (!g_cc.ok)
        fprintf(stderr, "[engined] OpenCC 配置 s2tw/tw2s 打不开，繁体模式下不出 e2e 整句候选\n");
    return g_cc.ok;
}

static void cc_conv(void *od, const char *in, char *out, size_t cap)
{
    char *r = g_cc.conv(od, in, strlen(in));
    snprintf(out, cap, "%s", r ? r : in);
    if (r)
        g_cc.free_(r);
}

static void *e2e_main(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_job.mu);
        while (!g_job.has)
            pthread_cond_wait(&g_job.cv, &g_job.mu);
        Conn *c = g_job.c;
        uint64_t gen = g_job.gen;
        char letters[256], ctx[sizeof(g_job.ctx)];
        memcpy(letters, g_job.letters, sizeof(letters));
        memcpy(ctx, g_job.ctx, sizeof(ctx));
        g_job.has = 0;
        g_job.cancel = 0;
        int fuzzy_dirty = g_job.fuzzy_dirty, trad = g_job.traditional;
        uint32_t fuzzy = g_job.fuzzy;
        g_job.fuzzy_dirty = 0;
        pthread_mutex_unlock(&g_job.mu);
        if (fuzzy_dirty)
            e2e_set_fuzzy(g_e2e, fuzzy);           /* 只有这个线程用模糊拼写表，在这里换不会和解码冲突 */
        if (trad) {                                 /* 繁体：模型只认简体，上下文先转简体 */
            if (!cc_init())
                continue;
            char t[sizeof(ctx)];
            cc_conv(g_cc.t2s, ctx, t, sizeof(t));
            snprintf(ctx, sizeof(ctx), "%s", t);
        }

        double t0 = plat_now_us();
        E2EResult r[1];
        int n = e2e_decode(g_e2e, ctx, letters, g_e2e_beam, g_e2e_ms, &g_job.cancel, r, 1);
        if (g_debug)
            fprintf(stderr, "[engined] #%d e2e [%s] \"%s\" -> %s（%.0f ms）\n", c->id, ctx, letters,
                    n > 0 ? r[0].text : (n < 0 ? "（取消）" : "（无）"), (plat_now_us() - t0) / 1000);
        if (n <= 0)
            continue;
        /* 放第几位：整句概率够高（短输入、靠上下文分辨的词），或者模型认为它比引擎首选好得多
         * （长句：引擎首选的分数更高说明 e2e 的 beam 漏掉了它，这时 e2e 结果多半是错的） */
        int jp = e2e_is_jianpin(g_e2e, letters);
        char top0[256] = "";
        int top_fixed = 0;
        pthread_mutex_lock(&c->wmu);
        if (c->gen == gen && c->ncands > 0) {
            snprintf(top0, sizeof(top0), "%s", c->cands[0].text);
            top_fixed = c->cands[0].corrected;
        }
        pthread_mutex_unlock(&c->wmu);
        if (trad && top0[0]) {                      /* 引擎首选是繁体：转简体再和模型结果比、打分 */
            char t[sizeof(top0)];
            cc_conv(g_cc.t2s, top0, t, sizeof(t));
            snprintf(top0, sizeof(top0), "%s", t);
        }
        /* 引擎首选是纠错得来的（打错了字母）：模型会把错拼照字面拼成怪句子（zhognguo → 中哦给你过），
         * 简拼也一样，这时整句候选不抢第一 */
        int jp_ok = jp && r[0].score >= -g_e2e_jp_per * (float)strlen(letters);
        int first = !top_fixed && (jp_ok || expf(r[0].score) >= g_e2e_ptop);
        float se = 0;
        int scored = 0;
        if (!first && !top_fixed && top0[0] && !is_ascii_text(top0) && strcmp(top0, r[0].text) != 0 &&
            e2e_score(g_e2e, ctx, letters, top0, &se) == 0) {
            first = r[0].score - se >= g_e2e_margin;
            scored = 1;
        }
        if (g_debug) {
            char why[320] = "";
            if (scored)
                snprintf(why, sizeof(why), "，引擎首选「%s」%.2f", top0, se);
            fprintf(stderr, "[engined] #%d e2e%s 分数 %.2f（p=%.2f）%s%s → 第 %d 位\n", c->id, jp ? "（简拼）" : "",
                    r[0].score, expf(r[0].score), why, top_fixed ? "，引擎首选是纠错" : "", first ? 1 : 2);
        }
        int fresh = 0;
        char outtext[sizeof(c->e2e_text)];
        if (trad)
            cc_conv(g_cc.s2t, r[0].text, outtext, sizeof(outtext));
        else
            snprintf(outtext, sizeof(outtext), "%s", r[0].text);
        pthread_mutex_lock(&c->wmu);
        if (c->gen == gen && c->fd >= 0 && strcmp(outtext, c->ncands > 0 ? c->cands[0].text : "") != 0) {
            snprintf(c->e2e_text, sizeof(c->e2e_text), "%s", outtext);
            c->e2e_gen = gen;
            c->e2e_first = first;
            fresh = 1;
        }
        pthread_mutex_unlock(&c->wmu);
        if (fresh)
            send_candidates(c);
    }
    return NULL;
}

/* 组合变了：记下新的 gen，够长就交给 e2e 线程（取消正在跑的） */
static void e2e_post(Conn *c)
{
    if (!g_e2e)
        return;
    pthread_mutex_lock(&c->wmu);
    uint64_t gen = c->gen;
    char letters[256], ctx[sizeof(c->ctx)];
    memcpy(letters, c->letters, (size_t)c->nletters);
    letters[c->nletters] = 0;
    int go = g_e2e_user && wx_opts.scheme == 0 && c->e2e_ok && c->nletters >= (e2e_is_jianpin(g_e2e, letters) ? g_e2e_min_jp : g_e2e_min);
    memcpy(ctx, c->ctx, sizeof(ctx));
    pthread_mutex_unlock(&c->wmu);
    pthread_mutex_lock(&g_job.mu);
    g_job.cancel = 1;
    if (go) {
        g_job.c = c;
        g_job.gen = gen;
        memcpy(g_job.letters, letters, sizeof(letters));
        memcpy(g_job.ctx, ctx, sizeof(ctx));
        g_job.has = 1;
        pthread_cond_signal(&g_job.cv);
    }
    pthread_mutex_unlock(&g_job.mu);
}

static void on_pending(Conn *c, WximePendingNode *head)
{
    Buf plist = {0};
    for (WximePendingNode *p = head; p; p = p->next) {
        char ch;
        const char *s = NULL;
        size_t len = 0;
        switch (p->type) {
        case 0: case 1:
            s = p->str;
            len = p->str ? (size_t)p->len : 0;
            break;
        case 2: case 4: case 5: case 6:
            ch = (char)(p->len & 0xff);
            /* 类型 6 是引擎自动切分的音节分隔（字符为 0），显示成 ' */
            if (p->type == 6 && ch == 0)
                ch = '\'';
            s = &ch;
            len = ch ? 1 : 0;
            break;
        default:        /* 3 跳过；7 是固定标记，先不显示 */
            break;
        }
        if (g_debug)
            fprintf(stderr, "[engined] #%d   pending node type=%d \"%.*s\"\n", c->id, p->type,
                    (int)len, s ? s : "");
        if (!s)
            continue;
        Buf w = {0};
        b_uint(&w, 1, (uint64_t)p->type);       /* 段类型（插件不看） */
        b_bytes(&w, 7, s, len);                 /* 插件取 field 7 拼成预编辑 */
        b_bytes(&plist, 1, w.p, w.n);
        b_free(&w);
    }
    if (head)
        wx.free_pending_input(head);
    Buf d = {0};
    b_uint(&d, 1, RESP_PENDING);
    b_bytes(&d, 3, plist.p, plist.n);
    send_detail(c, &d);
    b_free(&d);
    b_free(&plist);
}

static void on_emit(Conn *c, WximeEmitEvent *ev)
{
    if (!ev->str || ev->len <= 0)
        return;
    if (g_debug)
        fprintf(stderr, "[engined] #%d   emit \"%.*s\"\n", c->id, ev->len, ev->str);
    Buf d = {0};
    b_uint(&d, 1, RESP_EMIT);
    b_bytes(&d, 4, ev->str, (size_t)ev->len);
    send_detail(c, &d);
    b_free(&d);
}

static void hexdump(const void *p, int n)
{
    const uint8_t *b = p;
    for (int i = 0; i < n; i++)
        fprintf(stderr, "%02x%s", b[i], (i % 16 == 15) ? "\n" : " ");
}

static WXIME_CALL void on_events(uint64_t sid, const int32_t *types, void *const *datas,
                                 int32_t count, void *ud)
{
    Conn *c = ud;
    for (int i = 0; i < count; i++) {
        if (g_debug)
            fprintf(stderr, "[engined] #%d event type=%d data=%p\n", c->id, types[i], datas[i]);
        if (!datas[i]) {
            /* 事件 5 的数据为空指针表示预编辑已清空（例如删掉了最后一个字母），要告诉前端 */
            if (types[i] == WXIME_EVT_PENDING) {
                on_pending(c, NULL);
                comp_reset(c);
            }
            continue;
        }
        switch (types[i]) {
        case WXIME_EVT_CANDIDATE_LIST_UPDATE:
            on_candidates(c, datas[i], 0);
            break;
        case WXIME_EVT_CLOUD_ITERATOR_UPDATE:
            /* 云候选回来了：引擎把云端结果并进候选，迭代器同样在 data+0x00 */
            if (g_debug)
                fprintf(stderr, "[engined] #%d 云候选更新\n", c->id);
            on_candidates(c, datas[i], 1);
            break;
        case WXIME_EVT_PENDING:
            on_pending(c, (WximePendingNode *)datas[i]);
            break;
        case WXIME_EVT_EMIT:
            on_emit(c, datas[i]);
            comp_reset(c);
            break;
        default:
            if (g_debug > 1)
                hexdump(datas[i], 64);
            break;
        }
    }
    (void)sid;
}

/* ---------------- 命令处理 ---------------- */
static void encode_utf8(uint32_t cp, char *out, uint32_t *n)
{
    if (cp < 0x80) { out[0] = (char)cp; *n = 1; }
    else if (cp < 0x800) { out[0] = (char)(0xc0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3f)); *n = 2; }
    else if (cp < 0x10000) {
        out[0] = (char)(0xe0 | cp >> 12); out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f)); *n = 3;
    } else {
        out[0] = (char)(0xf0 | cp >> 18); out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[3] = (char)(0x80 | (cp & 0x3f)); *n = 4;
    }
}

static void ensure_session(Conn *c)
{
    if (c->sid)
        return;
    c->sid = wx_engine_new_session_ex(on_events, c, c->priv);
    if (!c->sid)
        fprintf(stderr, "[engined] #%d 建会话失败\n", c->id);
}

static void handle_detail(Conn *c, Rd d)
{
    int f, w, type = 0;
    uint64_t v;
    Rd sub, key = {0}, commit = {0}, before = {0}, after = {0};
    int reason = 0;
    uint32_t cfg_fuzzy = 0;
    int cfg_trad = 0, cfg_e2e = 1, cfg_priv = 0, cfg_emoji = 0, cfg_scheme = 0, cfg_sp = 0, cfg_wubi = 1;
    while (r_field(&d, &f, &w, &v, &sub)) {
        if (f == 1 && w == 0) type = (int)v;
        else if (f == 2 && w == 2) key = sub;
        else if (f == 5 && w == 2) commit = sub;
        else if (f == 112 && w == 2) before = sub;
        else if (f == 113 && w == 2) after = sub;
        else if (f == 114 && w == 0) reason = (int)v;
        else if (f == 300 && w == 0) cfg_fuzzy = (uint32_t)v;
        else if (f == 301 && w == 0) cfg_trad = (int)v;
        else if (f == 302 && w == 0) cfg_e2e = (int)v;
        else if (f == 303 && w == 0) cfg_priv = (int)v;
        else if (f == 304 && w == 0) cfg_emoji = (int)v;   /* 0 Emoji / 1 颜文字 / 2 不出 */
        else if (f == 305 && w == 0) cfg_scheme = (int)v;  /* 0 全拼 / 1 双拼 / 2 五笔 */
        else if (f == 306 && w == 0) cfg_sp = (int)v;      /* 双拼方案 1..7 */
        else if (f == 307 && w == 0) cfg_wubi = (int)v;    /* 五笔选项位（engine.h WxSessOpts） */
    }
    switch (type) {
    case CMD_CREATE_SESSION:
        if (c->sid)
            wx.reset_session(c->sid);
        ensure_session(c);
        break;
    case CMD_TEXT_AROUND_CURSOR: {
        size_t bl = before.p ? (size_t)(before.end - before.p) : 0, al = after.p ? (size_t)(after.end - after.p) : 0;
        pthread_mutex_lock(&c->wmu);
        trim_context(bl ? (const char *)before.p : "", bl, c->ctx, sizeof(c->ctx));
        pthread_mutex_unlock(&c->wmu);
        /* 每次新组合开始时前端都发这条；字段 303 表示这个输入框是隐私的 */
        if (c->priv != cfg_priv && g_debug)
            fprintf(stderr, "[engined] #%d %s隐私输入框\n", c->id, cfg_priv ? "进入" : "离开");
        if (c->priv != cfg_priv && c->sid) {     /* 引擎学不学选词是会话配置，只能重建；这时还没有组合 */
            wx.destroy_session(c->sid);
            c->sid = 0;
            comp_reset(c);
        }
        c->priv = cfg_priv;
        ensure_session(c);
        if (g_cloud_active)
            netcompat_set_enabled(!c->priv && wx_opts.scheme == 0);   /* 云输入请求里是全拼字母 */
        if (g_debug)
            fprintf(stderr, "[engined] #%d 上下文 [%s]（光标后 %zu 字节）\n", c->id, c->ctx, al);
        if (c->sid && wx.set_text_around_cursor)
            wx.set_text_around_cursor(c->sid, bl ? (const char *)before.p : "", (int)bl,
                                      al ? (const char *)after.p : "", (int)al, "", 0, reason);
        break;
    }
    case CMD_SET_CONFIG: {
        if (cfg_emoji < 0 || cfg_emoji > 2)
            cfg_emoji = 0;
        if (cfg_scheme < 0 || cfg_scheme > 2 || (cfg_scheme == 1 && (cfg_sp < 1 || cfg_sp > 7)))
            cfg_scheme = 0;
        if (cfg_scheme != 1)
            cfg_sp = 0;
        if (cfg_scheme != 2)
            cfg_wubi = 0;
        int sess = cfg_fuzzy != wx_opts.fuzzy || (cfg_trad != 0) != (wx_opts.traditional != 0) ||
                   cfg_emoji != wx_opts.emoji || cfg_scheme != wx_opts.scheme || cfg_sp != wx_opts.shuangpin ||
                   cfg_wubi != wx_opts.wubi;
        static const char *const emo[] = { "Emoji", "颜文字", "不出表情" };
        static const char *const sch[] = { "全拼", "双拼", "五笔" };
        fprintf(stderr, "[engined] #%d 配置：%s%.0d，模糊音 0x%x，%s，整句候选%s，%s%s\n", c->id, sch[cfg_scheme],
                cfg_scheme == 1 ? cfg_sp : cfg_scheme == 2 ? cfg_wubi : 0, cfg_fuzzy,
                cfg_trad ? "繁体" : "简体", cfg_e2e ? "开" : "关", emo[cfg_emoji], sess ? "（重建会话）" : "");
        g_e2e_user = cfg_e2e;
        if (g_e2e) {
            pthread_mutex_lock(&g_job.mu);
            if (g_job.fuzzy != cfg_fuzzy) {
                g_job.fuzzy = cfg_fuzzy;
                g_job.fuzzy_dirty = 1;
            }
            g_job.traditional = cfg_trad;
            pthread_mutex_unlock(&g_job.mu);
        }
        if (!sess)
            break;
        wx_opts.fuzzy = cfg_fuzzy;
        wx_opts.traditional = cfg_trad;
        wx_opts.emoji = cfg_emoji;
        wx_opts.scheme = cfg_scheme;
        wx_opts.shuangpin = cfg_sp;
        wx_opts.wubi = cfg_wubi;
        if (c->sid) {                           /* 会话选项只在建会话时生效：重建 */
            wx.destroy_session(c->sid);
            c->sid = 0;
            comp_reset(c);
            ensure_session(c);
        }
        if (g_learn.sid) {
            g_learn_stale = 1;
            if (g_learn.state == L_IDLE) {
                wx.destroy_session(g_learn.sid);
                g_learn.sid = wx_engine_new_session(on_learn_events, NULL);
                g_learn_stale = 0;
            }
        }
        break;
    }
    case CMD_MORE_CANDIDATES:
        more_candidates(c);
        break;
    case CMD_BLOCK_CANDIDATE: {
        Rd text = {0}, id = {0};
        while (commit.p && r_field(&commit, &f, &w, &v, &sub)) {
            if (f == 1 && w == 2) text = sub;
            else if (f == 2 && w == 2) id = sub;
        }
        uint32_t idn = id.p ? (uint32_t)(id.end - id.p) : 0;
        if (!c->sid || !text.p || !id.p || !conn_has_id(c, id.p, idn))
            break;
        size_t tl = (size_t)(text.end - text.p);
        if (idn == sizeof(E2E_ID) - 1 && !memcmp(id.p, E2E_ID, idn)) {
            /* 整句候选不是引擎里的词：这次组合里不再出（模型下次输入还会算，删不掉） */
            pthread_mutex_lock(&c->wmu);
            c->e2e_ok = 0;
            c->e2e_text[0] = 0;
            pthread_mutex_unlock(&c->wmu);
            send_candidates(c);
            break;
        }
        if ((idn == sizeof(HOT_ID) - 1 && !memcmp(id.p, HOT_ID, idn)) ||
            (idn == sizeof(FUNC_ID) - 1 && !memcmp(id.p, FUNC_ID, idn)))
            break;              /* 常用语在设置里删；日期/时间不是词 */
        if (!wx.add_word_to_user_block_list)
            break;
        uint8_t ok = wx.add_word_to_user_block_list(c->sid, (const char *)text.p, (uint32_t)tl, id.p, idn, 1);
        fprintf(stderr, "[engined] #%d 删词「%.*s」-> %d\n", c->id, (int)tl, (const char *)text.p, ok);
        if (ok)
            blocked_log((const char *)text.p, tl);
        break;
    }
    case CMD_DESTROY_SESSION:
        if (c->sid) {
            wx.destroy_session(c->sid);
            c->sid = 0;
        }
        break;
    case CMD_SEND_KEY: {
        uint32_t code = 0, special = 0;
        while (key.p && r_field(&key, &f, &w, &v, &sub)) {
            if (f == 1 && w == 0) code = (uint32_t)v;
            else if (f == 2 && w == 0) special = (uint32_t)v;
        }
        ensure_session(c);
        if (!c->sid)
            break;
        if (g_debug)
            fprintf(stderr, "[engined] #%d key code=%u special=%u\n", c->id, code, special);
        if (special == KEY_TEXT_INPUT && code) {
            char u[4];
            uint32_t n;
            encode_utf8(code, u, &n);
            pthread_mutex_lock(&c->wmu);
            uint32_t lc = code >= 'A' && code <= 'Z' ? code + 32 : code;
            if (lc >= 'a' && lc <= 'z' && c->nletters < (int)sizeof(c->letters) - 1)
                c->letters[c->nletters++] = (char)lc;
            if (code >= 'A' && code <= 'Z')
                c->e2e_ok = 0;          /* 打了大写字母（iPhone、PDF）：是英文，整句模型只认拼音，这次组合不跑 */
            c->gen++;
            pthread_mutex_unlock(&c->wmu);
            struct { uint64_t zero, now_ms; } extra = { 0, next_ts(c) };
            wx.process_input(c->sid, u, n, &extra, sizeof(extra));
            e2e_post(c);
        } else if (special == KEY_BACKSPACE) {
            pthread_mutex_lock(&c->wmu);
            if (c->nletters > 0)
                c->nletters--;
            c->gen++;
            pthread_mutex_unlock(&c->wmu);
            wx.drop_last(c->sid, 1, NULL, 0);   /* 第二个参数是删几个字符（drop_n） */
            e2e_post(c);
        } else if (special == KEY_ESCAPE) {
            comp_reset(c);
            e2e_post(c);                        /* 取消正在跑的 e2e */
            wx.reset_session(c->sid);
            on_pending(c, NULL);                /* 引擎不发空预编辑，替它发 */
        }
        break;
    }
    case CMD_SELECT_CANDIDATE: {
        Rd text = {0}, id = {0};
        while (commit.p && r_field(&commit, &f, &w, &v, &sub)) {
            if (f == 1 && w == 2) text = sub;
            else if (f == 2 && w == 2) id = sub;
        }
        uint32_t idn = id.p ? (uint32_t)(id.end - id.p) : 0;
        if (c->sid && text.p && id.p && conn_has_id(c, id.p, idn) &&
            ((idn == sizeof(HOT_ID) - 1 && !memcmp(id.p, HOT_ID, idn)) ||
             (idn == sizeof(FUNC_ID) - 1 && !memcmp(id.p, FUNC_ID, idn)))) {
            /* 常用语、展开后的日期/时间：服务自己上屏，引擎会话清空 */
            Buf d = {0};
            b_uint(&d, 1, RESP_EMIT);
            b_bytes(&d, 4, text.p, (size_t)(text.end - text.p));
            send_detail(c, &d);
            b_free(&d);
            comp_reset(c);
            e2e_post(c);
            wx.reset_session(c->sid);
            on_pending(c, NULL);
            break;
        }
        if (c->sid && text.p && idn == sizeof(E2E_ID) - 1 && !memcmp(id.p, E2E_ID, idn) &&
            conn_has_id(c, id.p, idn)) {
            /* e2e 整句：服务自己上屏，引擎会话清空 */
            if (g_debug)
                fprintf(stderr, "[engined] #%d 选 e2e \"%.*s\"\n", c->id, (int)(text.end - text.p), (const char *)text.p);
            Buf d = {0};
            b_uint(&d, 1, RESP_EMIT);
            b_bytes(&d, 4, text.p, (size_t)(text.end - text.p));
            send_detail(c, &d);
            b_free(&d);
            char sel[512], letters[256];
            snprintf(sel, sizeof(sel), "%.*s", (int)(text.end - text.p), (const char *)text.p);
            pthread_mutex_lock(&c->wmu);
            memcpy(letters, c->letters, (size_t)c->nletters);
            letters[c->nletters] = 0;
            pthread_mutex_unlock(&c->wmu);
            if (!c->priv)
                learn_e2e(letters, sel);
            comp_reset(c);
            e2e_post(c);
            wx.reset_session(c->sid);
            on_pending(c, NULL);
            break;
        }
        if (!c->sid || !text.p || !id.p || !conn_has_id(c, id.p, (uint32_t)(id.end - id.p))) {
            fprintf(stderr, "[engined] #%d 拒绝选词：id 不在最近的候选里\n", c->id);
            break;
        }
        if (g_debug)
            fprintf(stderr, "[engined] #%d select \"%.*s\" id_len=%d\n", c->id,
                    (int)(text.end - text.p), (const char *)text.p, (int)(id.end - id.p));
        /* 选了引擎的候选：整词上屏会经 emit 复位；部分选词后剩下的拼音不再跑 e2e */
        pthread_mutex_lock(&c->wmu);
        c->e2e_ok = 0;
        c->gen++;
        pthread_mutex_unlock(&c->wmu);
        e2e_post(c);
        struct { uint64_t zero, now_ms; } extra = { 0, next_ts(c) };
        wx.select_candidate(c->sid, (const char *)text.p, (uint32_t)(text.end - text.p),
                            id.p, (uint32_t)(id.end - id.p), NULL, 0, &extra, sizeof(extra),
                            NULL, 0, NULL);
        if (getenv("MINIME_ASSOC") && wx.get_most_likely_sequel) {      /* 实验：上屏后要联想 */
            int32_t r = wx.get_most_likely_sequel(c->sid);
            fprintf(stderr, "[engined] #%d 联想 get_most_likely_sequel -> %d\n", c->id, r);
        }
        break;
    }
    default:
        if (g_debug)
            fprintf(stderr, "[engined] #%d 忽略命令 %d\n", c->id, type);
        break;
    }
}

/* CommandBase{version=1, cmd=CommandV1{repeated Detail cmd=1; app_info=2}} */
static void handle_frame(Conn *c, const uint8_t *p, size_t n)
{
    Rd r = { p, p + n }, sub, v1 = {0};
    int f, w;
    uint64_t v, version = 0;
    while (r_field(&r, &f, &w, &v, &sub)) {
        if (f == 1 && w == 0) version = v;
        else if (f == 2 && w == 2) v1 = sub;
    }
    if (version != 1 || !v1.p)
        return;
    while (r_field(&v1, &f, &w, &v, &sub))
        if (f == 1 && w == 2)
            handle_detail(c, sub);
}

/* 读一帧并处理；连接断开或出错返回 -1。消息都很小，客户端是本机进程，阻塞读整帧即可。 */
static int conn_read(Conn *c)
{
    static uint8_t *buf;
    static size_t cap;
    uint8_t hdr[4];
    if (read_all(c->fd, hdr, 4) < 0)
        return -1;
    uint32_t len = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (uint32_t)hdr[3] << 24;
    if (len > (1u << 20))
        return -1;
    if (len > cap) {
        cap = len;
        buf = realloc(buf, cap);
    }
    if (len && read_all(c->fd, buf, len) < 0)
        return -1;
    handle_frame(c, buf, len);
    return 0;
}

static void conn_close(Conn *c)
{
    if (g_debug)
        fprintf(stderr, "[engined] #%d 断开\n", c->id);
    pthread_mutex_lock(&c->wmu);
    close(c->fd);
    c->fd = -1;
    if (c->iter)
        wx.delete_candidate_iterator(c->iter);
    c->iter = NULL;
    c->more = 0;
    pthread_mutex_unlock(&c->wmu);
    if (c->sid)
        wx.destroy_session(c->sid);
    /* Conn 不释放：会话销毁后引擎线程里可能还有排队的回调会用到它（几十字节，可以接受） */
}

/* ---------------- 监听 ---------------- */
static int listen_unix(char *path_out, size_t cap)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char dir[256];
    if (rt && *rt)
        snprintf(dir, sizeof(dir), "%s/minime", rt);
    else
        snprintf(dir, sizeof(dir), "/tmp/minime-%d", (int)getuid());
    mkdir(dir, 0700);
    chmod(dir, 0700);
    snprintf(path_out, cap, "%s/engine.sock", dir);

    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path_out);
    /* 单实例：能连上说明已有服务在跑 */
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connect(probe, (struct sockaddr *)&a, sizeof(a)) == 0) {
        close(probe);
        fprintf(stderr, "[engined] 已有服务在 %s 运行\n", path_out);
        exit(1);
    }
    close(probe);
    unlink(path_out);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 16) < 0) {
        perror("[engined] unix socket");
        exit(1);
    }
    chmod(path_out, 0600);
    return fd;
}

static int listen_tcp(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 16) < 0) {
        fprintf(stderr, "[engined] TCP 127.0.0.1:%d 监听失败: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* ---------------- 控制通道：用户数据导出/导入（换机助手用，minime-netd 调） ----------------
 *
 * $XDG_RUNTIME_DIR/minime/ctl.sock，一次连接一行命令、一行回复：
 *   export <文件> <encrypt_key>   → ok | err <原因>
 *   import <文件> <encrypt_key>
 * encrypt_key 是迁移会话的 32 位十六进制密钥（引擎用前 16 个字符做 AES-128-ECB）。
 * 在主线程里同步执行：引擎调用必须在主线程。只接受同一用户的连接。 */
static void ctl_reply(int fd, const char *msg)
{
    size_t n = strlen(msg);
    if (write(fd, msg, n) < 0) { /* 对方已断开：忽略 */ }
}

static void ctl_handle(int fd)
{
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char buf[2048];
    size_t n = 0;
    while (n < sizeof(buf) - 1) {
        ssize_t r = read(fd, buf + n, sizeof(buf) - 1 - n);
        if (r <= 0)
            break;
        n += (size_t)r;
        if (memchr(buf, '\n', n))
            break;
    }
    buf[n] = 0;
    char *nl = strchr(buf, '\n');
    if (nl)
        *nl = 0;
    /* group <group_id> <func_switch> <常用语版本> <词库版本> [fast]：同步组信息变了（netd 调） */
    if (!strncmp(buf, "group ", 6)) {
        WximeGroupSyncInfo gi;
        memset(&gi, 0, sizeof(gi));
        int fast = 0;
        long long g, f, hv, uv;
        if (sscanf(buf + 6, "%lld %lld %lld %lld %d", &g, &f, &hv, &uv, &fast) < 4 || !wx.group_sync_info_changed) {
            ctl_reply(fd, "err 用法：group <id> <func_switch> <hw_ver> <ud_ver> [fast]\n");
            return;
        }
        gi.group_id = g;
        gi.func_switch = f;
        gi.hot_word_current_ver = hv;
        gi.user_dict_current_ver = uv;
        gi.fast_sync_for_debug = (uint8_t)fast;
        wx.group_sync_info_changed(&gi);
        fprintf(stderr, "[engined] 同步组信息：group %lld switch %lld 常用语 v%lld 词库 v%lld%s\n", g, f, hv, uv,
                fast ? "（fast）" : "");
        ctl_reply(fd, "ok\n");
        return;
    }
    /* 文件名放中间、密钥在最后：密钥是 hex 不含空格，文件名可以有空格 */
    char *sp1 = strchr(buf, ' ');
    char *sp2 = strrchr(buf, ' ');
    if (!sp1 || sp1 == sp2) {
        ctl_reply(fd, "err 用法：export|import <文件> <密钥>\n");
        return;
    }
    *sp1 = 0;
    *sp2 = 0;
    const char *verb = buf, *path = sp1 + 1, *key = sp2 + 1;
    if (strlen(key) != 32) {
        ctl_reply(fd, "err 密钥应为 32 位十六进制\n");
        return;
    }
    WximeUserDataParam param;
    memset(&param, 0, sizeof(param));
    param.data_path = path;
    param.data_path_len = (int32_t)strlen(path);
    param.encrypt_key = key;
    param.encrypt_key_len = 32;
    int ok;
    double t0 = plat_now_us();
    if (!strcmp(verb, "export") && wx.export_user_data)
        ok = wx.export_user_data(&param) & 0xff;
    else if (!strcmp(verb, "import") && wx.import_user_data)
        ok = wx.import_user_data(&param) & 0xff;
    else {
        ctl_reply(fd, "err 未知命令或引擎不支持\n");
        return;
    }
    fprintf(stderr, "[engined] 用户数据 %s -> %s（%.0f ms）\n", verb, ok ? "成功" : "失败",
            (plat_now_us() - t0) / 1000);
    ctl_reply(fd, ok ? "ok\n" : "err 引擎返回失败\n");
}

static int listen_ctl(void)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (rt && *rt)
        snprintf(a.sun_path, sizeof(a.sun_path), "%s/minime/ctl.sock", rt);
    else
        snprintf(a.sun_path, sizeof(a.sun_path), "/tmp/minime-%d/ctl.sock", (int)getuid());
    unlink(a.sun_path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4) < 0) {
        fprintf(stderr, "[engined] 控制通道 %s 监听失败: %s\n", a.sun_path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(a.sun_path, 0600);
    return fd;
}

/* ---------------- 改名迁移（wetype → minime，2026-11） ----------------
 * 旧版数据在 ~/.local/share/wetype、配置在 ~/.config/fcitx5/conf/wetype.conf。新目录不存在时整个改名过去，
 * 再把词库配置里写死的旧路径换成新路径。netd / 设置程序里有同样的逻辑（谁先启动谁做，rename 是原子的）。 */
static void rewrite_path_in(const char *file, const char *from, const char *to)
{
    FILE *f = fopen(file, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return;
    }
    fclose(f);
    buf[n] = 0;
    if (!strstr(buf, from)) {                   /* 没有旧路径：不动文件 */
        free(buf);
        return;
    }
    size_t fl = strlen(from), tl = strlen(to);
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", file);
    FILE *o = fopen(tmp, "wb");
    if (!o) {
        free(buf);
        return;
    }
    for (char *p = buf, *q; *p; p = q + fl) {
        q = strstr(p, from);
        if (!q) {
            fputs(p, o);
            break;
        }
        fwrite(p, 1, (size_t)(q - p), o);
        fwrite(to, 1, tl, o);
    }
    fclose(o);
    free(buf);
    rename(tmp, file);
}

static void migrate_legacy(void)
{
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME"), *c = getenv("XDG_CONFIG_HOME");
    char base[512], oldd[600], newd[600];
    if (x && *x)
        snprintf(base, sizeof(base), "%s", x);
    else
        snprintf(base, sizeof(base), "%s/.local/share", h ? h : "");
    snprintf(oldd, sizeof(oldd), "%s/wetype", base);
    snprintf(newd, sizeof(newd), "%s/minime", base);
    struct stat st;
    if (stat(newd, &st) != 0 && stat(oldd, &st) == 0 && rename(oldd, newd) == 0) {
        fprintf(stderr, "[engined] 数据目录已从 %s 迁到 %s\n", oldd, newd);
        char from[620], to[620], file[1000];
        snprintf(from, sizeof(from), "%s/", oldd);
        snprintf(to, sizeof(to), "%s/", newd);
        static const char *const files[] = { "dicts/core_conf.tsv", "dicts/core_state.tsv", "dicts/cells.tsv",
                                             "dicts/state.tsv", "dicts/library/catalog.json" };
        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
            snprintf(file, sizeof(file), "%s/%s", newd, files[i]);
            rewrite_path_in(file, from, to);
        }
    }
    char cbase[512], oldc[600], newc[600];
    if (c && *c)
        snprintf(cbase, sizeof(cbase), "%s", c);
    else
        snprintf(cbase, sizeof(cbase), "%s/.config", h ? h : "");
    snprintf(oldc, sizeof(oldc), "%s/fcitx5/conf/wetype.conf", cbase);
    snprintf(newc, sizeof(newc), "%s/fcitx5/conf/minime.conf", cbase);
    if (stat(newc, &st) != 0 && stat(oldc, &st) == 0 && rename(oldc, newc) == 0)
        fprintf(stderr, "[engined] 配置已从 %s 迁到 %s\n", oldc, newc);
}

/* 缺省路径：minime-fetch 把官方文件放在 $XDG_DATA_HOME/minime/official/ 下，工作目录是 …/minime/userdata */
static void data_path(char *out, size_t n, const char *rel)
{
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    if (x && *x)
        snprintf(out, n, "%s/minime/%s", x, rel);
    else
        snprintf(out, n, "%s/.local/share/minime/%s", h ? h : "", rel);
}

int main(int argc, char **argv)
{
    if (argc == 2 || argc > 3 || (argc > 1 && argv[1][0] == '-')) {
        fprintf(stderr, "用法: %s [<引擎目录(含 libwxhld.dll 和 BuildIn)> <工作目录>]\n"
                        "不带参数时用 minime-fetch 下载的引擎（~/.local/share/minime/official/win/…）\n", argv[0]);
        return 1;
    }
    g_debug = getenv("MINIME_DEBUG") ? atoi(getenv("MINIME_DEBUG")) : 0;
    if (getenv("MINIME_MAX_CANDS"))
        g_max_cands = atoi(getenv("MINIME_MAX_CANDS"));
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, NULL, _IOLBF, 0);
    migrate_legacy();                         /* 要在打开引擎（读工作目录、词库配置）之前 */

    static char engine_dir[1024], work_dir[1024];
    if (argc == 3) {
        snprintf(engine_dir, sizeof(engine_dir), "%s", argv[1]);
        snprintf(work_dir, sizeof(work_dir), "%s", argv[2]);
    } else {
        if (getenv("MINIME_ENGINE_DIR"))
            snprintf(engine_dir, sizeof(engine_dir), "%s", getenv("MINIME_ENGINE_DIR"));
        else
            data_path(engine_dir, sizeof(engine_dir), "official/win/3.0.1.35");
        data_path(work_dir, sizeof(work_dir), "userdata");
        char dll[1100];
        snprintf(dll, sizeof(dll), "%s/libwxhld.dll", engine_dir);
        if (access(dll, R_OK) != 0) {
            fprintf(stderr, "[engined] 没有找到官方引擎（%s）。先运行 minime-fetch 从腾讯官方下载\n", dll);
            return 2;
        }
        char ud[1100];
        data_path(ud, sizeof(ud), "");
        mkdir(ud, 0700);
    }

    double t0 = plat_now_us();
    netcompat_install();                        /* 引擎的 ime_net.dll 导入换成我们的兼容层 */
    if (wx_engine_open(engine_dir, work_dir) < 0)
        return 1;
    fprintf(stderr, "[engined] 引擎 %s 就绪（%.0f ms）\n", wx.version, (plat_now_us() - t0) / 1000);
    cloud_poll();
    ab_poll();
    dict_sync_setup();
    dict_sync_poll();

    if (!getenv("MINIME_E2E") || atoi(getenv("MINIME_E2E")) != 0) {
        char dir[1024], model[1100], mpd[1100], err[256];
        char fetched[1024], probe[1100];
        data_path(fetched, sizeof(fetched), "official/mac/imeData");      /* minime-fetch e2e */
        snprintf(probe, sizeof(probe), "%s/decgnr.bin", fetched);
        if (getenv("MINIME_E2E_DIR"))
            snprintf(dir, sizeof(dir), "%s", getenv("MINIME_E2E_DIR"));
        else if (access(probe, R_OK) == 0)
            snprintf(dir, sizeof(dir), "%s", fetched);
        else
            snprintf(dir, sizeof(dir), "%s/BuildIn", engine_dir);
        snprintf(model, sizeof(model), "%s/decgnr.bin", dir);
        snprintf(mpd, sizeof(mpd), "%s/mpd_e2e.bin", dir);
#define ENVI(v, name) if (getenv(name)) v = atoi(getenv(name))
        int threads = 4;
        ENVI(threads, "MINIME_E2E_THREADS");
        ENVI(g_e2e_beam, "MINIME_E2E_BEAM");
        ENVI(g_e2e_ms, "MINIME_E2E_MS");
        ENVI(g_e2e_min, "MINIME_E2E_MIN");
        ENVI(g_e2e_pos, "MINIME_E2E_POS");
#undef ENVI
        if (getenv("MINIME_E2E_PTOP")) g_e2e_ptop = (float)atof(getenv("MINIME_E2E_PTOP"));
        if (getenv("MINIME_E2E_MARGIN")) g_e2e_margin = (float)atof(getenv("MINIME_E2E_MARGIN"));
        if (getenv("MINIME_E2E_JP_PER")) g_e2e_jp_per = (float)atof(getenv("MINIME_E2E_JP_PER"));
        if (getenv("MINIME_E2E_JIANPIN")) g_e2e_jp = atoi(getenv("MINIME_E2E_JIANPIN"));
        if (getenv("MINIME_E2E_MIN_JP")) g_e2e_min_jp = atoi(getenv("MINIME_E2E_MIN_JP"));
        if (getenv("MINIME_E2E_LEARN")) g_e2e_learn = atoi(getenv("MINIME_E2E_LEARN"));
        if (getenv("MINIME_E2E_JP_PENALTY")) g_e2e_jp_pen = (float)atof(getenv("MINIME_E2E_JP_PENALTY"));
        if (access(model, R_OK) == 0) {
            t0 = plat_now_us();
            g_e2e = e2e_open(model, mpd, threads, err, sizeof(err));
            if (g_e2e) {
                e2e_set_jianpin(g_e2e, g_e2e_jp, g_e2e_jp_pen);
                if (g_e2e_learn && pipe2(g_learn.pipe, O_NONBLOCK | O_CLOEXEC) == 0) {   /* 学习用的隐藏会话 */
                    g_learn.sid = wx_engine_new_session(on_learn_events, NULL);
                    if (!g_learn.sid)
                        fprintf(stderr, "[engined] 学习会话建不起来，选 e2e 结果后不学习\n");
                }
                pthread_t th;
                pthread_create(&th, NULL, e2e_main, NULL);
                pthread_detach(th);
                const char *be = e2e_backend_name(g_e2e);
                char thr[32] = "";
                if (!strncmp(be, "cpu", 3))
                    snprintf(thr, sizeof thr, "，%d 线程", threads);
                fprintf(stderr, "[engined] e2e 整句模型就绪（%s，%.0f ms，后端 %s，beam %d%s）\n", dir,
                        (plat_now_us() - t0) / 1000, be, g_e2e_beam, thr);
            } else {
                fprintf(stderr, "[engined] e2e 模型加载失败：%s\n", err);
            }
        } else if (g_debug) {
            fprintf(stderr, "[engined] 没有 e2e 模型（%s），不出整句候选\n", model);
        }
    }

    char upath[300];
    int ufd = listen_unix(upath, sizeof(upath));
    int port = getenv("MINIME_TCP_PORT") ? atoi(getenv("MINIME_TCP_PORT")) : 0;
    int tfd = port > 0 ? listen_tcp(port) : -1;
    int cfd = listen_ctl();                     /* 在 listen_unix 之后：目录已建好 */
    fprintf(stderr, "[engined] 监听 %s%s\n", upath, tfd >= 0 ? "，127.0.0.1 TCP" : "");

    /* 单线程事件循环：调用引擎的始终是主线程（装载时已经有 TEB）。
     * 引擎的回调在它自己的线程里来，只做编码和写 socket。 */
    enum { MAXC = 256 };
    static Conn *conns[MAXC];
    int nconn = 0, next_id = 1;
    char hello[32];
    int hello_n = snprintf(hello, sizeof(hello), "PID:%d", (int)getpid());
    for (;;) {
        struct pollfd pf[4 + MAXC];
        pf[0] = (struct pollfd){ ufd, POLLIN, 0 };
        pf[1] = (struct pollfd){ tfd, POLLIN, 0 };
        for (int i = 0; i < nconn; i++)
            pf[2 + i] = (struct pollfd){ conns[i]->fd, POLLIN, 0 };
        pf[2 + nconn] = (struct pollfd){ g_learn.pipe[0], POLLIN, 0 };
        pf[3 + nconn] = (struct pollfd){ cfd, POLLIN, 0 };
        int to = learn_poll_timeout();
        if (to < 0 || to > 2000)
            to = 2000;                          /* 至少每 2 秒醒一次：检查词库配置有没有变 */
        if (poll(pf, 4 + (nfds_t)nconn, to) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        wx_engine_cells_poll();                 /* 设置程序改了词库开关：清空后重新加载（主线程） */
        cloud_poll();                           /* 云输入开关（主线程：登录要调引擎） */
        dict_sync_poll();
        ab_poll();                              /* netd 更新了服务端 AB 参数：重灌 */
        if (g_learn.sid)
            learn_step();                       /* 学习：事件到了或超时了就推进一步 */
        if (cfd >= 0 && (pf[3 + nconn].revents & POLLIN)) {
            int fd = accept4(cfd, NULL, NULL, SOCK_CLOEXEC);
            struct ucred cr;
            socklen_t l = sizeof(cr);
            if (fd >= 0 && getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &l) == 0 && cr.uid == getuid())
                ctl_handle(fd);
            if (fd >= 0)
                close(fd);
        }
        /* 先处理已有连接（倒序，便于删除） */
        for (int i = nconn - 1; i >= 0; i--) {
            if (!pf[2 + i].revents)
                continue;
            if (conn_read(conns[i]) < 0) {
                conn_close(conns[i]);
                conns[i] = conns[--nconn];
            }
        }
        for (int i = 0; i < 2; i++) {
            if (pf[i].fd < 0 || !(pf[i].revents & POLLIN))
                continue;
            int fd = accept4(pf[i].fd, NULL, NULL, SOCK_CLOEXEC);
            if (fd < 0)
                continue;
            if (i == 0) {
                struct ucred cr;
                socklen_t l = sizeof(cr);
                if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &l) < 0 || cr.uid != getuid()) {
                    close(fd);
                    continue;
                }
            } else {
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            }
            if (nconn == MAXC) {
                close(fd);
                continue;
            }
            Conn *c = calloc(1, sizeof(*c));
            c->fd = fd;
            c->id = next_id++;
            pthread_mutex_init(&c->wmu, NULL);
            pthread_mutex_init(&c->snd, NULL);
            c->e2e_ok = 1;
            conns[nconn++] = c;
            if (g_debug)
                fprintf(stderr, "[engined] #%d 连接（%s）\n", c->id, i == 0 ? "unix" : "tcp");
            send_frame(c, hello, (size_t)hello_n);
        }
    }
    return 0;
}
