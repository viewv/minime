/* engine.c — 引擎适配层的实现，初始化流程和 host.c 相同（见那里的注释）。 */
#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "platform.h"
#include "sesscfg_official.h"

WxEngine wx;

static int g_log_engine = 0;   /* MINIME_ENGINE_LOG：把引擎日志打到 stderr */

static WXIME_CALL void on_native_log(int level, const char *msg)
{
    if (g_log_engine)
        fprintf(stderr, "[engine:%d] %s\n", level, msg ? msg : "(null)");
}

/* SessionConfig+0xe8：官方 server 放的就是 return 1 桩 */
static WXIME_CALL int cb_always_yes(void)
{
    return 1;
}

uint64_t wx_wall_ms(void)
{
    return plat_wall_ms();
}

static void *sym(plat_lib h, const char *name)
{
    void *p = plat_sym(h, name);
    if (!p)
        fprintf(stderr, "[engine] 缺少导出 %s\n", name);
    return p;
}

/* BuildIn/index.json：取 in_ime_data 为 true 且文件存在的条目（name、type=词库 id） */
typedef struct { char name[128]; int id; int version; } DictEntry;

static int load_dict_list(const char *dir, DictEntry *out, int cap)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s%cindex.json", dir, PLAT_SEP);
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    n = (long)fread(buf, 1, n, f);
    buf[n] = 0;
    fclose(f);
    int count = 0;
    for (char *p = strchr(buf, '{'); p && count < cap; p = strchr(p + 1, '{')) {
        char *end = strchr(p, '}');
        if (!end)
            break;
        *end = 0;
        char *nm = strstr(p, "\"name\"");
        char *ty = strstr(p, "\"type\"");
        char *in = strstr(p, "\"in_ime_data\"");
        if (nm && ty && in && strstr(in, "true")) {
            char *q1 = strchr(strchr(nm + 6, ':'), '"');
            char *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 && q2 && q2 - q1 - 1 < (long)sizeof(out[count].name)) {
                memcpy(out[count].name, q1 + 1, q2 - q1 - 1);
                out[count].name[q2 - q1 - 1] = 0;
                out[count].id = atoi(strchr(ty + 6, ':') + 1);
                char *ve = strstr(p, "\"version\"");
                out[count].version = ve ? atoi(strchr(ve + 9, ':') + 1) : 0;
                char fp[MAX_PATH];
                snprintf(fp, sizeof(fp), "%s%c%s", dir, PLAT_SEP, out[count].name);
                FILE *t = fopen(fp, "rb");
                if (t) {
                    fclose(t);
                    count++;
                }
            }
        }
        p = end;
    }
    free(buf);
    return count;
}

/* 细胞词库类型：服务端清单的 typeId（index.json 里的 type 对 hf/sc 是 0，wrw/ssrc 缺省时按名字补） */
static uint32_t cell_dict_type(const char *name, int index_type)
{
    if (strstr(name, "cell_dict.wrw."))
        return 1;
    if (strstr(name, "cell_dict.ssrc."))
        return 7;
    return index_type > 0 ? (uint32_t)index_type : 0;
}

/* ---------------- 词库配置（设置程序「词库」页） ----------------
 * $XDG_DATA_HOME/minime/dicts/
 *   cells.tsv  用户配置：每行 `启用(0/1)<TAB>type<TAB>路径`，设置程序写，engined 读
 *   state.tsv  实际加载结果：`ok(0/1)<TAB>type<TAB>路径`，engined 写
 *   core.tsv   核心词库：`路径<TAB>typeId<TAB>版本`，只读展示 */
static struct { char path[MAX_PATH]; uint32_t type; } g_cell_builtin[16];
static int g_ncell_builtin;
static struct timespec g_cells_mtime;
static char (*g_off_list)[128];
static int g_noff;

static const char *state_path(char *buf, size_t n, const char *file)
{
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    if (x && *x)
        snprintf(buf, n, "%s/minime/dicts", x);
    else
        snprintf(buf, n, "%s/.local/share/minime/dicts", h ? h : ".");
    /* 逐级建目录 */
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    mkdir(buf, 0755);
    size_t l = strlen(buf);
    snprintf(buf + l, n - l, "/%s", file);
    return buf;
}

static int entries_version_of(const DictEntry *e, int n, const char *path)
{
    const char *base = strrchr(path, PLAT_SEP);
    base = base ? base + 1 : path;
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].name, base))
            return e[i].version;
    return 0;
}

/* 按 cells.tsv 加载细胞词库（先清空）。主线程调用。 */
static void cells_load(void)
{
    if (!wx.add_cell_dict || getenv("MINIME_NO_CELL"))
        return;
    char cp[MAX_PATH], sp[MAX_PATH];
    state_path(cp, sizeof(cp), "cells.tsv");
    state_path(sp, sizeof(sp), "state.tsv");
    struct stat st;
    if (stat(cp, &st) != 0) {
        FILE *f = fopen(cp, "w");                      /* 默认：内置的全部启用 */
        if (f) {
            for (int i = 0; i < g_ncell_builtin; i++)
                fprintf(f, "1\t%u\t%s\n", g_cell_builtin[i].type, g_cell_builtin[i].path);
            fclose(f);
        }
        stat(cp, &st);
    }
    g_cells_mtime = st.st_mtim;
    if (wx.clear_cell_dicts)
        wx.clear_cell_dicts();
    FILE *f = fopen(cp, "r"), *out = fopen(sp, "w");
    char line[MAX_PATH + 32];
    int n = 0, nok = 0;
    while (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t1 || !t2)
            continue;
        *t1 = *t2 = 0;
        int en = atoi(line);
        uint32_t type = (uint32_t)strtoul(t1 + 1, NULL, 10);
        const char *path = t2 + 1;
        int ok = 0;
        if (en) {
            ok = wx.add_cell_dict(path, type, NULL) & 0xff;
            n++;
            nok += ok;
        }
        if (out)
            fprintf(out, "%d\t%u\t%s\n", en ? (ok ? 1 : -1) : 0, type, path);
    }
    /* 实验：MINIME_CELL_DICTS（冒号分隔的路径），不写进配置 */
    const char *extra = getenv("MINIME_CELL_DICTS");
    if (extra && *extra) {
        static char buf[4096];
        snprintf(buf, sizeof(buf), "%s", extra);
        for (char *p = strtok(buf, ":"); p; p = strtok(NULL, ":")) {
            const char *base = strrchr(p, '/');
            uint32_t t = cell_dict_type(base ? base + 1 : p, 0);
            int ok = wx.add_cell_dict(p, t, NULL) & 0xff;
            n++;
            nok += ok;
        }
    }
    if (f)
        fclose(f);
    if (out)
        fclose(out);
    fprintf(stderr, "[engine] 细胞词库 %d/%d 个加载成功（%s）\n", nok, n, cp);
}

/* 配置文件变了就重新加载（engined 主循环定期调用） */
void wx_engine_cells_poll(void)
{
    char cp[MAX_PATH];
    struct stat st;
    if (stat(state_path(cp, sizeof(cp), "cells.tsv"), &st) != 0)
        return;
    if (st.st_mtim.tv_sec == g_cells_mtime.tv_sec && st.st_mtim.tv_nsec == g_cells_mtime.tv_nsec)
        return;
    cells_load();
}

int wx_engine_open(const char *install_dir, const char *work_dir_in)
{
    g_log_engine = getenv("MINIME_ENGINE_LOG") != NULL;
    static char buildin_dir[MAX_PATH], work_dir[MAX_PATH];
    {
        /* 绝对路径：写进词库配置给设置程序用 */
        char abs[MAX_PATH];
        const char *base = realpath(install_dir, abs) ? abs : install_dir;
        snprintf(buildin_dir, sizeof(buildin_dir), "%s%cBuildIn", base, PLAT_SEP);
    }
    snprintf(work_dir, sizeof(work_dir), "%s", work_dir_in);
    plat_fix_sep(buildin_dir);
    plat_fix_sep(work_dir);
    plat_mkdir(work_dir);

    plat_lib h = plat_load_engine(install_dir);
    if (!h) {
        fprintf(stderr, "[engine] 装载 %s/libwxhld.dll 失败\n", install_dir);
        return -1;
    }

    pfn_wxime_size_check size_check = sym(h, "wxime_size_check");
    pfn_wxime_initialize initialize = sym(h, "wxime_initialize");
    pfn_wxime_config_dict config_dict = sym(h, "wxime_config_dict");
    pfn_wxime_get_version get_version = sym(h, "wxime_get_version");
    wx.create_session = sym(h, "wxime_create_session");
    wx.add_session_listener = sym(h, "wxime_add_session_listener");
    wx.process_input = sym(h, "wxime_process_input");
    wx.candidate_next = sym(h, "wxime_candidate_next");
    wx.delete_candidate_iterator = sym(h, "wxime_delete_candidate_iterator");
    wx.select_candidate = sym(h, "wxime_select_candidate");
    wx.drop_last = sym(h, "wxime_drop_last");
    wx.reset_session = sym(h, "wxime_reset_session");
    wx.free_pending_input = sym(h, "wxime_free_pending_input");
    wx.destroy_session = sym(h, "wxime_destroy_session");
    wx.set_text_around_cursor = sym(h, "wxime_set_text_around_cursor");   /* 可选，缺了只是没有上下文 */
    wx.finalize = sym(h, "wxime_finalize");
    /* 可选接口，缺了不影响输入法本体 */
    wx.export_user_data = plat_sym(h, "wxime_export_user_data");
    wx.import_user_data = plat_sym(h, "wxime_import_user_data");
    wx.add_cell_dict = plat_sym(h, "wxime_add_cell_dict");
    wx.network_login = plat_sym(h, "wxime_network_login");
    wx.reset_ab = plat_sym(h, "wxime_reset_cli_ab_test_info");
    wx.group_sync_info_changed = plat_sym(h, "wxime_group_sync_info_changed");
    wx.register_client_cos_api = plat_sym(h, "wxime_register_client_cos_api");
    wx.client_cos_job_completed = plat_sym(h, "wxime_client_cos_job_completed");
    wx.get_most_likely_sequel = plat_sym(h, "wxime_get_most_likely_sequel");
    wx.add_word_to_user_block_list = plat_sym(h, "wxime_add_word_to_user_block_list");
    wx.remove_word_from_user_block_list = plat_sym(h, "wxime_remove_word_from_user_block_list");
    wx.clear_cell_dicts = plat_sym(h, "wxime_clear_cell_dicts");
    if (!size_check || !initialize || !config_dict || !get_version || !wx.create_session ||
        !wx.add_session_listener || !wx.process_input || !wx.candidate_next ||
        !wx.delete_candidate_iterator || !wx.select_candidate || !wx.drop_last ||
        !wx.reset_session || !wx.free_pending_input || !wx.destroy_session || !wx.finalize)
        return -1;

    wx.version = get_version();
    if (!size_check(sizeof(WximeInitInfo), sizeof(WximeSessionConfig))) {
        fprintf(stderr, "[engine] 结构体尺寸检查失败（引擎 %s 的接口变了）\n", wx.version);
        return -1;
    }

    static char uin[] = "0";
    static char app_ver[] = "3.0.1.35";
    static char device_name[] = "wetype-linux";
    static char sys_ver[] = "10.0.26244";
    /* 必须带 logger.appender，否则引擎第一次写结构化日志就空指针崩溃（见 api.md） */
    static char debug_cfg[] = "{\"logger\":{\"appender\":{\"type\":\"capi\"}}}";
    WximeInitInfo init;
    memset(&init, 0, sizeof(init));
    init.log_cb = on_native_log;
    init.flag_0x08 = 1;
    init.debug_log_config = debug_cfg;
    init.work_dir = work_dir;
    init.uin = uin;
    init.uin_len = (int32_t)strlen(uin);
    init.log_level = g_log_engine ? 0 : 4;
    init.platform = WXIME_PLATFORM_WINDOWS;
    init.app_ver = app_ver;
    init.app_ver_len = (int32_t)strlen(app_ver);
    init.device_name = device_name;
    init.device_name_len = (int32_t)strlen(device_name);
    init.system_version = sys_ver;
    init.system_version_len = (int32_t)strlen(sys_ver);
    initialize(&init);

    static DictEntry entries[64];
    static WximeDictInfo dicts[64];
    static char paths[64][MAX_PATH];
    int ndict = load_dict_list(buildin_dir, entries, 64);
    if (ndict <= 0) {
        fprintf(stderr, "[engine] 读不到 %s/index.json\n", buildin_dir);
        return -1;
    }
    /* 细胞词库（cell_dict.*）单独用 add_cell_dict 加，其余走 config_dict */
    static char cell_paths[16][MAX_PATH];
    static uint32_t cell_types[16];
    int ncell = 0, nconf = 0, ndict0 = ndict;
    /* 核心词库覆盖配置（设置程序写）：每行
     *   off<TAB>名字               不加载这个内置包
     *   replace<TAB>名字<TAB>路径   换成别的文件（新版本 / 其他平台的同类包）
     *   add<TAB>typeId<TAB>路径     追加一个包
     * 状态写到 core_state.tsv：状态<TAB>typeId<TAB>版本<TAB>名字<TAB>路径（状态 1 内置 0 关 2 替换 3 追加） */
    static char conf_lines[64][MAX_PATH + 64];
    int nconf_lines = 0;
    {
        char cp[MAX_PATH];
        FILE *cf = fopen(state_path(cp, sizeof(cp), "core_conf.tsv"), "r");
        while (cf && nconf_lines < 64 && fgets(conf_lines[nconf_lines], sizeof(conf_lines[0]), cf)) {
            conf_lines[nconf_lines][strcspn(conf_lines[nconf_lines], "\r\n")] = 0;
            if (conf_lines[nconf_lines][0])
                nconf_lines++;
        }
        if (cf)
            fclose(cf);
    }
    static int core_state[64];
    static char core_name[64][128];
    for (int i = 0; i < ndict; i++) {
        if (!strncmp(entries[i].name, "cell_dict.", 10)) {
            if (ncell < 16) {
                snprintf(cell_paths[ncell], MAX_PATH, "%s%c%s", buildin_dir, PLAT_SEP, entries[i].name);
                cell_types[ncell++] = cell_dict_type(entries[i].name, entries[i].id);
            }
            continue;
        }
        snprintf(paths[nconf], MAX_PATH, "%s%c%s", buildin_dir, PLAT_SEP, entries[i].name);
        int st = 1, off = 0;
        for (int k = 0; k < nconf_lines; k++) {
            char *l = conf_lines[k], *t1 = strchr(l, '\t');
            if (!t1)
                continue;
            size_t nl = strlen(entries[i].name);
            if (!strncmp(l, "off\t", 4) && !strcmp(t1 + 1, entries[i].name))
                off = 1;
            if (!strncmp(l, "replace\t", 8) && !strncmp(t1 + 1, entries[i].name, nl) && t1[1 + nl] == '\t') {
                snprintf(paths[nconf], MAX_PATH, "%s", t1 + 2 + nl);
                st = 2;
            }
        }
        if (off) {
            if (getenv("MINIME_DEBUG"))
                fprintf(stderr, "[engine] 不加载 %s（设置里关了）\n", entries[i].name);
            /* 关掉的也要写进状态文件 */
            static char off_list[64][128];
            static int noff;
            if (noff < 64)
                snprintf(off_list[noff++], sizeof(off_list[0]), "%d\t%s", entries[i].id, entries[i].name);
            g_off_list = off_list;
            g_noff = noff;
            continue;
        }
        core_state[nconf] = st;
        snprintf(core_name[nconf], sizeof(core_name[0]), "%s", entries[i].name);
        dicts[nconf].fd = 0;
        dicts[nconf].path = paths[nconf];
        dicts[nconf].id = entries[i].id;
        dicts[nconf].version = 0;
        nconf++;
    }
    ndict = nconf;
    for (int k = 0; k < nconf_lines && ndict < 64; k++) {
        char *l = conf_lines[k];
        if (strncmp(l, "add\t", 4))
            continue;
        char *t2 = strchr(l + 4, '\t');
        if (!t2)
            continue;
        snprintf(paths[ndict], MAX_PATH, "%s", t2 + 1);
        dicts[ndict].fd = 0;
        dicts[ndict].path = paths[ndict];
        dicts[ndict].id = atoi(l + 4);
        dicts[ndict].version = 0;
        core_state[ndict] = 3;
        const char *b = strrchr(paths[ndict], '/');
        snprintf(core_name[ndict], sizeof(core_name[0]), "%s", b ? b + 1 : paths[ndict]);
        ndict++;
    }
    /* 实验：MINIME_EXTRA_DICTS="路径=typeId,路径=typeId"，追加到 config_dict（同 typeId 的后者覆盖前者） */
    const char *extra_dicts = getenv("MINIME_EXTRA_DICTS");
    if (extra_dicts && *extra_dicts) {
        static char ebuf[4096];
        snprintf(ebuf, sizeof(ebuf), "%s", extra_dicts);
        for (char *p = strtok(ebuf, ","); p && ndict < 64; p = strtok(NULL, ",")) {
            char *eq = strrchr(p, '=');
            if (!eq)
                continue;
            *eq = 0;
            snprintf(paths[ndict], MAX_PATH, "%s", p);
            dicts[ndict].fd = 0;
            dicts[ndict].path = paths[ndict];
            dicts[ndict].id = atoi(eq + 1);
            dicts[ndict].version = 0;
            fprintf(stderr, "[engine] 追加词库 %s id=%d\n", p, dicts[ndict].id);
            ndict++;
        }
    }
    /* 用户词库目录必须和工作目录下的 user_dict/ 分开：引擎启动时把 <工作目录>/user_dict 当成
     * 每实例 mmap 的临时目录清空（"Clean up …/user_dict"），放在那里的用户词库每次启动都被删掉。
     * 官方布局是工作目录 ImeDir/ 加独立的 DictDir2/（见 开发笔记（未公开））。 */
    static char user_dict_dir[MAX_PATH];
    snprintf(user_dict_dir, MAX_PATH, "%s%cDictDir2", work_dir, PLAT_SEP);
    plat_mkdir(user_dict_dir);
    /* 五笔词库同 typeId（524291），都加载时后加载的覆盖前面的（实测三个都开时用的是 98）。
     * 只交给引擎一个：按 86、98、新世纪的顺序取第一个没关的；另外两个状态照常写（设置里显示为开），
     * 只是不加载。设置程序「输入 → 五笔版本」按同样的顺序显示当前版本。 */
    static WximeDictInfo load[64];
    int nload = 0, wubi = -1;
    static const char *const wubi_pref[] = { "swbd86.bin", "swbd98.bin", "swbdnc.bin" };
    for (int p = 0; p < 3 && wubi < 0; p++)
        for (int i = 0; i < ndict; i++)
            if (!strcmp(core_name[i], wubi_pref[p])) {
                wubi = i;
                break;
            }
    for (int i = 0; i < ndict; i++) {
        if (i != wubi && !strncmp(core_name[i], "swbd", 4) && core_state[i] != 3) {
            if (getenv("MINIME_DEBUG"))
                fprintf(stderr, "[engine] 不加载 %s（五笔只用一个版本）\n", core_name[i]);
            continue;
        }
        load[nload++] = dicts[i];
    }
    WximeDictConfig dc;
    memset(&dc, 0, sizeof(dc));
    dc.dicts = load;
    dc.count = nload;
    dc.user_dict = user_dict_dir;   /* 用户二元词典目录：建会话必需 */
    config_dict(&dc);

    /* 核心词库清单写给设置程序看（只读展示） */
    {
        char cp[MAX_PATH];
        FILE *f = fopen(state_path(cp, sizeof(cp), "core_state.tsv"), "w");
        if (f) {
            for (int i = 0; i < ndict; i++)
                fprintf(f, "%d\t%d\t%d\t%s\t%s\n", core_state[i] ? core_state[i] : 3, dicts[i].id,
                        entries_version_of(entries, ndict0, core_name[i]), core_name[i], dicts[i].path);
            for (int i = 0; i < g_noff; i++) {
                const char *t = strchr(g_off_list[i], '\t');
                fprintf(f, "0\t%.*s\t%d\t%s\t\n", (int)(t - g_off_list[i]), g_off_list[i],
                        entries_version_of(entries, ndict0, t + 1), t + 1);
            }
            fclose(f);
        }
    }
    /* 细胞词库：按 ~/.local/share/minime/dicts/cells.tsv 加载（没有就用内置的生成默认配置） */
    g_ncell_builtin = ncell;
    for (int i = 0; i < ncell; i++) {
        snprintf(g_cell_builtin[i].path, MAX_PATH, "%s", cell_paths[i]);
        g_cell_builtin[i].type = cell_types[i];
    }
    cells_load();
    return 0;
}

WxSessOpts wx_opts;

uint64_t wx_engine_new_session(WximeListener cb, void *userdata)
{
    return wx_engine_new_session_ex(cb, userdata, 0);
}

uint64_t wx_engine_new_session_ex(WximeListener cb, void *userdata, int no_learn)
{
    static char app_name[] = "minime-engined";
    WximeSessionConfig cfg;
    _Static_assert(sizeof(cfg) == sizeof(wxime_official_sesscfg), "SessionConfig 尺寸");
    memcpy(&cfg, wxime_official_sesscfg, sizeof(cfg));
    cfg.cfg_cb_0xe8 = cb_always_yes;
    for (int i = 0; i < 19; i++)                       /* 模糊音：+0x0d..+0x1f */
        if (wx_opts.fuzzy >> i & 1)
            ((uint8_t *)&cfg)[0x0d + i] = 1;
    cfg.simplified_to_traditional = wx_opts.traditional ? 1 : 0;
    cfg.emoji_a = wx_opts.emoji == 0;                  /* +0x34 Emoji */
    cfg.emoji_b = wx_opts.emoji != 2;                  /* +0x35 颜文字（Emoji 开着时让给 Emoji） */
    cfg.double_pin_solution = wx_opts.scheme == 1 ? wx_opts.shuangpin : 0;
    if (wx_opts.scheme == 2) {
        cfg.keyboard_type = 5;                         /* 五笔 */
        cfg.emit_when_input_next_rune = (wx_opts.wubi & 1) != 0;
        cfg.emit_unique_candidate = (wx_opts.wubi & 2) != 0;
        cfg.enable_pinyin = (wx_opts.wubi & 4) != 0;
    }
    cfg.app_name = app_name;
    cfg.app_name_len = (int32_t)strlen(app_name);
    /* 隐私输入框：+0xa8 disable_user_dict 只停掉写入（选词不学、不进用户词库），已学的词照常出 */
    cfg.disable_user_dict = no_learn ? 1 : 0;
    /* 调试：MINIME_SESSCFG="0x3c=1,0x2c=3" 逐字节改会话配置（试开关用） */
    const char *ov = getenv("MINIME_SESSCFG");
    while (ov && *ov) {
        char *end;
        unsigned long off = strtoul(ov, &end, 0);
        if (*end != '=' || off >= sizeof(cfg))
            break;
        unsigned long v = strtoul(end + 1, &end, 0);
        ((uint8_t *)&cfg)[off] = (uint8_t)v;
        ov = *end == ',' ? end + 1 : end;
    }
    uint64_t sid = wx.create_session(&cfg);
    if (sid)
        wx.add_session_listener(sid, cb, userdata);
    return sid;
}
