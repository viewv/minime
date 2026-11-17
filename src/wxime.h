/* wxime.h — 微信输入法引擎 libwxhld.dll（Windows 版 3.0.1.35）的 C 接口声明，供 MinIME 与引擎互操作。
 *
 * 只声明 MinIME 用到的接口；名称和含义以实际行为为准，不是官方头文件。
 * 两个结构体的大小可以用 wxime_size_check(0xd0, 0x138) 自检。
 *
 * 调用流程：
 *   wxime_size_check(sizeof(InitInfo), sizeof(SessionConfig))   // 可选自检
 *   wxime_reset_user_id(uin, len)                              // 可选
 *   wxime_initialize(&InitInfo)
 *   wxime_config_dict(&DictConfig)
 *   sid = wxime_create_session(&SessionConfig)                 // 0 = 失败
 *   wxime_add_session_listener(sid, on_events, userdata)
 *   wxime_process_input(sid, "nihao", 5, NULL, 0)              // 结果异步回调
 *     -> on_events 里取事件 0 的 newIterator
 *     -> wxime_candidate_next(it, &cand) 循环取候选
 *     -> wxime_delete_candidate_iterator(it)                   // 用完必须归还
 *   wxime_destroy_session(sid); wxime_finalize()
 */
#ifndef WXIME_H
#define WXIME_H

#include <stdint.h>

/* 引擎是 Windows x64 代码：所有和它互相调用的函数指针都必须是 Windows 调用约定。
 * mingw 下这就是默认约定；Linux 原生装载时要显式标 ms_abi。 */
#ifdef _WIN32
#define WXIME_CALL
#else
#define WXIME_CALL __attribute__((ms_abi))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 常量 ---------- */
#define WXIME_INIT_INFO_SIZE   0xd0
#define WXIME_SESSION_CFG_SIZE 0x138
#define WXIME_CANDIDATE_SIZE   0x70

#define WXIME_PLATFORM_ANDROID 1
#define WXIME_PLATFORM_WINDOWS 6

/* 监听事件类型（一批事件里每个事件的类型）。 */
enum WximeEventType {
    WXIME_EVT_CANDIDATE_LIST_UPDATE = 0,  /* 候选就绪：newIterator 在 data+0x00 */
    WXIME_EVT_1 = 1,                      /* 未知（服务端走默认分支） */
    WXIME_EVT_EMIT = 2,                   /* 上屏：WximeEmitEvent */
    WXIME_EVT_PENDING = 5,                /* 预编辑：data 就是 WximePendingNode 链表头；NULL 表示预编辑已空 */
    WXIME_EVT_CLOUD_ITERATOR_UPDATE = 6,  /* 云候选更新：迭代器在 data+0x00，版本号 +0x18 */
    WXIME_EVT_10 = 10,
    WXIME_EVT_13 = 13,
};

/* ---------- InitInfo (0xd0 字节) ---------- */
/* 未用区域保持为 0 即可（先清零再填需要的字段）。 */
#pragma pack(push, 1)
typedef struct WximeInitInfo {
    /* 0x00 */ void (WXIME_CALL *log_cb)(int level, const char *msg); /* on_native_log */
    /* 0x08 */ uint8_t  flag_0x08;        /* 安卓恒 1 */
    /* 0x09 */ uint8_t  _pad09[7];
    /* 0x10 */ const char *debug_log_config; /* 安卓传 "{}" */
    /* 0x18 */ char *work_dir;            /* 引擎会建目录，把 '\' 换成 '/' */
    /* 0x20 */ uint8_t  _pad20[9];
    /* 0x29 */ uint8_t  safe_mode;        /* 1 = 只加载内置词库 */
    /* 0x2a */ uint8_t  _pad2a[6];
    /* 0x30 */ void (WXIME_CALL *cb_0x30)(void);     /* 安卓填，用途未明 */
    /* 0x38 */ int32_t  device_performance;  /* Windows: 0 或 -5 */
    /* 0x3c */ uint8_t  test_mode;
    /* 0x3d */ uint8_t  _pad3d[3];
    /* 0x40 */ const char *uin;          /* 纯数字串 */
    /* 0x48 */ int32_t  uin_len;
    /* 0x4c */ int32_t  log_level;
    /* 0x50 */ int32_t  platform;         /* 1=安卓 6=Windows */
    /* 0x54 */ uint8_t  _pad54[4];
    /* 0x58 */ const char *app_ver;
    /* 0x60 */ int32_t  app_ver_len;
    /* 0x64 */ uint8_t  _pad64[4];
    /* 0x68 */ const char *device_name;
    /* 0x70 */ int32_t  device_name_len;
    /* 0x74 */ uint8_t  _pad74[4];
    /* 0x78 */ const char *system_version;
    /* 0x80 */ int32_t  system_version_len;
    /* 0x84 */ uint8_t  _pad84[4];
    /* 0x88 */ void (WXIME_CALL *cb_0x88)(void);     /* 两边都填；Windows 服务进程在此放一个回调 */
    /* 0x90 */ void (WXIME_CALL *report_mirror_cb)(void); /* 仅开启镜像上报时填 */
    /* 0x98 */ uint8_t  report_100_percent;
    /* 0x99 */ uint8_t  _pad99[15];
    /* 0xa8 */ const char *expt_info;
    /* 0xb0 */ int32_t  expt_info_len;
    /* 0xb4 */ uint8_t  _padb4[4];
    /* 0xb8 */ float    wevis_cpu_score;  /* 安卓为 0 会崩；Windows 服务进程传 0 */
    /* 0xbc */ float    wevis_gpu_score;
    /* 0xc0 */ int32_t  package_type;
    /* 0xc4 */ uint8_t  _padc4[4];
    /* 0xc8 */ void    *ptr_0xc8;
} WximeInitInfo;
#pragma pack(pop)

/* ---------- DictConfig ---------- */
#pragma pack(push, 1)
typedef struct WximeDictInfo {           /* 24 字节 */
    /* 0x00 */ int64_t     fd;           /* 安卓: AssetManager fd；Windows: 0 */
    /* 0x08 */ const char *path;         /* 文件绝对路径（fd=0 时用） */
    /* 0x10 */ int32_t     id;           /* index.json 的 "type" 字段 */
    /* 0x14 */ int32_t     version;      /* Windows 服务进程传 0 */
} WximeDictInfo;

typedef struct WximeDictConfig {         /* 实际用到 0x18，安卓清零 0x28 */
    /* 0x00 */ WximeDictInfo *dicts;
    /* 0x08 */ int32_t        count;
    /* 0x0c */ uint8_t        _pad0c[4];
    /* 0x10 */ const char    *user_dict;  /* 用户词库路径，可为 NULL */
    /* 0x18 */ uint8_t        _pad18[16]; /* 安卓清零 */
} WximeDictConfig;
#pragma pack(pop)

/* ---------- SessionConfig (0x138 字节) ---------- */
/* 大部分开关 0 = 关闭即可工作；标注 [官方必填] 的字段要按官方客户端的取值填写
 * （默认取值见 sesscfg_official.h）。 */
#pragma pack(push, 1)
typedef struct WximeSessionConfig {
    /* 0x00 */ uint8_t  with_async_session_wrapper;          /* [官方必填] */
    /* 0x01 */ uint8_t  with_search_candidate_instantly;    /* [官方必填] */
    /* 0x02 */ uint8_t  _pad02[2];
    /* 0x04 */ int32_t  keyboard_type;                       /* [官方必填] 0=拼音 */
    /* 0x08 */ uint8_t  enable_shift_key_correction;          /* 以下 24 个模糊音/键盘开关 */
    /* 0x09 */ uint8_t  enable_exchange;
    /* 0x0a */ uint8_t  enable_keyboard_neighbour;
    /* 0x0b */ uint8_t  enable_insert_at_middle;
    /* 0x0c */ uint8_t  enable_skip;
    /* 0x0d */ uint8_t  enable_nl;
    /* 0x0e */ uint8_t  enable_rl;
    /* 0x0f */ uint8_t  enable_hf;
    /* 0x10 */ uint8_t  enable_gk;
    /* 0x11 */ uint8_t  enable_ang;
    /* 0x12 */ uint8_t  enable_iang;
    /* 0x13 */ uint8_t  enable_uang;
    /* 0x14 */ uint8_t  enable_c_ch;
    /* 0x15 */ uint8_t  enable_s_sh;
    /* 0x16 */ uint8_t  enable_z_zh;
    /* 0x17 */ uint8_t  enable_hui_fei;
    /* 0x18 */ uint8_t  enable_eng;
    /* 0x19 */ uint8_t  enable_ing;
    /* 0x1a */ uint8_t  enable_ong;
    /* 0x1b */ uint8_t  enable_huang_wang;
    /* 0x1c */ uint8_t  enable_un_ong;
    /* 0x1d */ uint8_t  enable_un_iong;
    /* 0x1e */ uint8_t  enable_an_ai;
    /* 0x1f */ uint8_t  enable_eng_ong;
    /* 0x20 */ int32_t  double_pin_solution;
    /* 0x24 */ uint8_t  enable_pinyin;                       /* 五笔配置里的拼音开关 */
    /* 0x25 */ uint8_t  emit_unique_candidate;
    /* 0x26 */ uint8_t  emit_when_input_next_rune;
    /* 0x27 */ uint8_t  wildcard_comment;
    /* 0x28 */ uint8_t  enable_comment;
    /* 0x29 */ uint8_t  enable_candidate;
    /* 0x2a */ uint8_t  _pad2a[2];
    /* 0x2c */ int32_t  most_likely_filter_a;               /* local_most_likely_config */
    /* 0x30 */ int16_t  most_likely_filter_b;
    /* 0x32 */ uint8_t  most_likely_filter_c;
    /* 0x33 */ uint8_t  _pad33;
    /* 0x34 */ uint8_t  emoji_a;                             /* emoji 相关开关组 */
    /* 0x35 */ uint8_t  emoji_b;
    /* 0x36 */ uint8_t  _pad36;
    /* 0x37 */ uint8_t  env_probe_result;                     /* 官方填环境探测结果 */
    /* 0x38 */ uint8_t  voice_input;
    /* 0x39 */ uint8_t  always_1;                             /* [官方必填] 恒 1 */
    /* 0x3a */ uint8_t  enable_pre_input;
    /* 0x3b */ uint8_t  safe_mode;
    /* 0x3c */ uint8_t  enable_auto_most_likely;
    /* 0x3d */ uint8_t  enable_hw_rare_words;
    /* 0x3e */ uint8_t  enable_english_assisted_spelling_correction;
    /* 0x3f */ uint8_t  _pad3f[2];
    /* 0x41 */ uint8_t  flag_0x41;
    /* 0x42 */ uint8_t  _pad42[2];
    /* 0x44 */ uint8_t  flag_0x44;
    /* 0x45 */ uint8_t  _pad45[11];
    /* 0x50 */ int32_t  int_0x50;                            /* [官方必填] */
    /* 0x54 */ int32_t  int_0x54;                            /* [官方必填] */
    /* 0x58 */ uint8_t  _pad58[4];
    /* 0x5c */ uint8_t  wechat_related;
    /* 0x5d */ uint8_t  _pad5d[71];
    /* 0xa4 */ uint8_t  _pada4[4];
    /* 0xa8 */ uint8_t  disable_user_dict;
    /* 0xa9 */ uint8_t  debug_trace;
    /* 0xaa */ uint8_t  debug_trace_with_id;
    /* 0xab */ uint8_t  report_not_production_log;
    /* 0xac */ uint8_t  enable_user_hot_word_recommend;
    /* 0xad */ uint8_t  enable_user_hot_area_record;
    /* 0xae */ uint8_t  enable_local_correction;
    /* 0xaf */ uint8_t  experimental_mode;
    /* 0xb0 */ uint8_t  is_wifi;
    /* 0xb1 */ uint8_t  enable_hw_syllable_mark;
    /* 0xb2 */ uint8_t  _padb2[2];
    /* 0xb4 */ int32_t  return_key_type;
    /* 0xb8 */ int32_t  simplified_to_traditional;          /* [官方必填] */
    /* 0xbc */ uint8_t  _padbc[4];
    /* 0xc0 */ const char *app_name;                         /* [官方必填] */
    /* 0xc8 */ void    *enable_tags;                         /* WordTag 数组，可 NULL */
    /* 0xd0 */ const char *text_place_holder;                /* 可 NULL */
    /* 0xd8 */ int32_t  enable_tags_count;
    /* 0xdc */ int32_t  app_name_len;                        /* [官方必填] */
    /* 0xe0 */ int32_t  text_place_holder_len;
    /* 0xe4 */ int32_t  app_scene;
    /* 0xe8 */ int    (WXIME_CALL *cfg_cb_0xe8)(void);                  /* [官方必填] Windows 服务进程放 return-1 桩 */
    /* 0xf0 */ int    (WXIME_CALL *cfg_cb_0xf0)(void);                  /* 安卓填，Windows 留 NULL */
    /* 0xf8 */ uint8_t  flag_0xf8;                            /* 以下来自会话标识结构 */
    /* 0xf9 */ uint8_t  flag_0xf9;
    /* 0xfa */ uint8_t  _padfa[6];
    /* 0x100 */ void *qword_0x100;
    /* 0x108 */ void *qword_0x108;
    /* 0x110 */ void *qword_0x110;
    /* 0x118 */ const char *group_sync_info;                 /* 可 NULL */
    /* 0x120 */ int32_t  e2e_client_enable;                     /* 2(或1)=启用 e2e（日志 "client enable"） */
    /* 0x124 */ uint8_t  _pad124[1];
    /* 0x125 */ uint8_t  flag_0x125;                          /* [官方必填] 恒 1 */
    /* 0x126 */ uint8_t  _pad126;
    /* 0x127 */ uint8_t  flag_0x127;                          /* [官方必填] 恒 1 */
    /* 0x128 */ uint8_t  flag_0x128;                          /* [官方必填] 恒 1 */
    /* 0x129 */ uint8_t  _pad129[15];
} WximeSessionConfig;
#pragma pack(pop)

/* ---------- Candidate (0x70 字节) ---------- */
#pragma pack(push, 1)
typedef struct WximeCandidate {
    /* 0x00 */ const char *text;
    /* 0x08 */ int32_t    text_len;
    /* 0x0c */ uint8_t    _pad0c[4];
    /* 0x10 */ const char *comment;
    /* 0x18 */ int32_t    comment_len;
    /* 0x1c */ uint8_t    _pad1c[4];
    /* 0x20 */ const void *id;            /* 字节串，选词时原样传回 */
    /* 0x28 */ int32_t    id_len;
    /* 0x2c */ uint8_t    _pad2c[4];
    /* 0x30 */ void      *detail;         /* 指向明细结构（model_info 等） */
    /* 0x38 */ const char *json_id_desc;
    /* 0x40 */ int32_t    json_id_desc_len;
    /* 0x44 */ int32_t    ints_0x44[11];  /* type/flag 等，未逐个对应 */
} WximeCandidate;
#pragma pack(pop)

/* ---------- 事件（onBatchEvent 的数据） ---------- */
#pragma pack(push, 1)
/* 事件 0：候选就绪（CandidateListUpdate）。 */
typedef struct WximeCandidateListEvent {
    /* 0x00 */ int64_t   new_iterator;      /* 迭代器！ */
    /* 0x08 */ int32_t   type;
    /* 0x0c */ uint8_t   _pad0c[4];
    /* 0x10 */ int64_t   cloud_iterator;
    /* 0x18 */ int32_t   version;
    /* 0x1c */ uint8_t   _pad1c[4];
    /* 0x20 */ int64_t   user_latest_action_ts;
    /* 0x28 */ int32_t   int_0x28;
    /* 0x2c */ uint8_t   _pad2c[4];
    /* 0x30 */ const char *last_sent_out_language;
    /* 0x38 */ int32_t   last_sent_out_language_len;
    /* 0x3c */ uint8_t   _pad3c[4];
    /* 0x40 */ const char *str_0x40;
    /* 0x48 */ int32_t   str_0x40_len;
    /* 0x4c */ int32_t   process_total_time_ms;
} WximeCandidateListEvent;
#pragma pack(pop)

/* ---------- 监听回调 ---------- */
/* 一次回调带一批事件，逐个处理 types[i] / datas[i]。 */
typedef void (WXIME_CALL *WximeListener)(uint64_t sid, const int32_t *types,
                              void *const *datas, int32_t count,
                              void *userdata);

/* ---------- 函数 ---------- */
/* 返回 C++ bool 的接口只有低 8 位有效，高位是垃圾，所以返回类型写成 uint8_t */
/* 用户数据导入/导出（换机助手）：参数是两个 C 字符串，都不能为空。
 * encrypt_key 是换机助手会话下发的 32 位十六进制文本。 */
typedef struct WximeUserDataParam {
    const char *data_path;      /* +0x00 */
    int32_t     data_path_len;  /* +0x08 字节长度 */
    int32_t     _pad0;
    const char *encrypt_key;    /* +0x10 */
    int32_t     encrypt_key_len;/* +0x18 */
    int32_t     _pad1;
} WximeUserDataParam;
typedef uint8_t (WXIME_CALL *pfn_wxime_export_user_data)(const WximeUserDataParam *param);
typedef uint8_t (WXIME_CALL *pfn_wxime_import_user_data)(const WximeUserDataParam *param);

/* 细胞词库（cell_dict.*.zh）：不能走 config_dict，要单独加。
 * bool (const char *path, unsigned type, fd)；fd 传 NULL 时按路径读文件。
 * type 是词库的类型号（hf/sc=0，wrw=1，ssrc=7）。 */
/* 云功能登录（之后引擎自己发起云输入等请求）。参数块：
 *   +0x20 channel_id / +0x28 长度，+0x30 device_code / +0x38 长度，+0x3c use_debug_net */
typedef int32_t (WXIME_CALL *pfn_wxime_network_login)(const void *param);
/* AB 实验参数（服务端下发的 mmkb_cli_* 键值），两层：
 *   WximeAbParam { WximeAbExpt *expts; int count }
 *   WximeAbExpt  { 16 字节头（实验 id 等）; WximeAbItem *items @+0x10; int count @+0x18 }   步长 0x20
 *   WximeAbItem  { key, key_len, value, value_len }                                         步长 0x20 */
typedef struct WximeAbItem {
    const char *key;   int32_t key_len;   int32_t _pad0;
    const char *value; int32_t value_len; int32_t _pad1;
} WximeAbItem;
typedef struct WximeAbExpt {
    int64_t            expt_id;
    int64_t            _hdr8;
    const WximeAbItem *items; int32_t count; int32_t _pad;
} WximeAbExpt;
typedef struct WximeAbParam { const WximeAbExpt *expts; int32_t count; int32_t _pad; } WximeAbParam;
typedef void (WXIME_CALL *pfn_wxime_reset_cli_ab_test_info)(const WximeAbParam *param);

/* 同步组信息（个人词库/常用语同步的开关和版本），按指针传。 */
typedef struct WximeGroupSyncInfo {
    uint8_t info_unknown;           /* +0x00 */
    uint8_t fast_sync_for_debug;    /* +0x01 */
    uint8_t _pad[6];
    int64_t group_id;               /* +0x08 */
    int64_t func_switch;            /* +0x10  1 剪贴板 / 2 常用语 / 4 个人词库 / 8 语音速记 */
    int64_t hot_word_current_ver;   /* +0x18 */
    int64_t user_dict_current_ver;  /* +0x20 */
} WximeGroupSyncInfo;
typedef void (WXIME_CALL *pfn_wxime_group_sync_info_changed)(const WximeGroupSyncInfo *info);

/* 客户端 COS 接口（引擎同步个人词库全量包时用）：引擎调回调要求上传/下载，回调返回 job id，
 * 传输完成后调 wxime_client_cos_job_completed(job, 0 成功 / -1 失败)。
 * 回调参数是 4 个 {ptr, len} 字符串（file_path / url / bucket_name / key），用 url 直接 PUT / GET。 */
typedef struct WximeCosStr { const char *ptr; int32_t len; int32_t _pad; } WximeCosStr;
typedef struct WximeCosParam {
    WximeCosStr file_path;          /* +0x00 本地文件 */
    WximeCosStr url;                /* +0x10 预签名链接 */
    WximeCosStr bucket_name;        /* +0x20 */
    WximeCosStr key;                /* +0x30 */
} WximeCosParam;
typedef int32_t (WXIME_CALL *pfn_wxime_cos_cb)(const WximeCosParam *p);
typedef struct WximeCosApi {
    pfn_wxime_cos_cb download;      /* +0x00 */
    pfn_wxime_cos_cb upload;        /* +0x08 */
    void *_r0, *_r1;
} WximeCosApi;
typedef void (WXIME_CALL *pfn_wxime_register_client_cos_api)(const WximeCosApi *api);
typedef void (WXIME_CALL *pfn_wxime_client_cos_job_completed)(int32_t job, int32_t result);
/* 联想（上屏后的「最可能的下文」） */
typedef int32_t (WXIME_CALL *pfn_wxime_get_most_likely_sequel)(uint64_t sid);
/* 删词（手机「长按候选 → 删除」）：把词加进用户屏蔽表，refresh 非 0 时引擎随即发新的候选列表。
 * 参数 (sid, word, len, id, id_len, refresh)；remove_word_from_user_block_list 参数相同（撤销） */
typedef uint8_t (WXIME_CALL *pfn_wxime_block_word)(uint64_t sid, const char *word, uint32_t word_len,
                                                    const void *id, uint32_t id_len, uint8_t refresh);

typedef uint8_t (WXIME_CALL *pfn_wxime_add_cell_dict)(const char *path, uint32_t type, void *fd);
typedef void (WXIME_CALL *pfn_wxime_clear_cell_dicts)(void);

typedef uint8_t (WXIME_CALL *pfn_wxime_size_check)(uint32_t init_size, uint32_t session_cfg_size);
typedef int  (WXIME_CALL *pfn_wxime_reset_user_id)(const char *uin, int len);
typedef int  (WXIME_CALL *pfn_wxime_initialize)(WximeInitInfo *info);
typedef int  (WXIME_CALL *pfn_wxime_config_dict)(WximeDictConfig *cfg);
typedef uint64_t (WXIME_CALL *pfn_wxime_create_session)(WximeSessionConfig *cfg);
typedef void (WXIME_CALL *pfn_wxime_add_session_listener)(uint64_t sid, WximeListener cb, void *userdata);
typedef uint8_t (WXIME_CALL *pfn_wxime_process_input)(uint64_t sid, const char *text, uint32_t len,
                                        const void *extra, uint32_t extra_len);
typedef uint8_t (WXIME_CALL *pfn_wxime_candidate_next)(void *iterator, WximeCandidate *out); /* out 先清零 */
typedef uint8_t (WXIME_CALL *pfn_wxime_get_candidates)(void *iterator, int (WXIME_CALL *visit)(WximeCandidate *, void *), void *ud);
typedef void (WXIME_CALL *pfn_wxime_delete_candidate_iterator)(void *iterator);
/* 选词参数：
 *   (sid, 候选文字, 长度, 候选 id, 长度, 后缀串, 长度,
 *    &{int, 当前时间毫秒}（16 字节，同 process_input 的 extra）, 0x10,
 *    0x20 字节一项的分段数组, 项数, NULL)
 * 普通选词时后缀和分段数组都可以为空。 */
typedef uint8_t (WXIME_CALL *pfn_wxime_select_candidate)(uint64_t sid, const char *text, uint32_t text_len,
                                           const void *id, uint32_t id_len,
                                           const char *suffix, int suffix_len, const void *extra,
                                           int extra_len, void *segs, int nsegs, void *obj);
/* 退格：(sid, 删除字符数, NULL, 0)；删除数必须大于 0 */
typedef uint8_t (WXIME_CALL *pfn_wxime_drop_last)(uint64_t sid, uint8_t drop_n, const void *p, int n);
typedef uint8_t (WXIME_CALL *pfn_wxime_reset_session)(uint64_t sid);
typedef void (WXIME_CALL *pfn_wxime_free_pending_input)(void *head);

/* 事件 5（PendingInputUpdate）：data 本身就是预编辑链表头，用完交给 wxime_free_pending_input。
 * 节点类型：
 *   类型 0/1：字符串段（str/len）；2/4/5/6：单个字符（放在 len 的低字节）；
 *   3：跳过；7：固定标记。 */
#pragma pack(push, 1)
typedef struct WximePendingNode {
    /* 0x00 */ int32_t type;
    /* 0x04 */ uint8_t _pad04[4];
    /* 0x08 */ struct WximePendingNode *next;
    /* 0x10 */ uint8_t _pad10[8];
    /* 0x18 */ int32_t len;               /* 字符串长度，或单字符节点的字符 */
    /* 0x1c */ uint8_t _pad1c[4];
    /* 0x20 */ const char *str;
} WximePendingNode;

/* 事件 2（EmitInputToScreen）：上屏文字 {str, len} */
typedef struct WximeEmitEvent {
    /* 0x00 */ const char *str;
    /* 0x08 */ int32_t len;
} WximeEmitEvent;
#pragma pack(pop)
typedef uint8_t (WXIME_CALL *pfn_wxime_destroy_session)(uint64_t sid);
typedef void (WXIME_CALL *pfn_wxime_finalize)(void);
typedef const char *(WXIME_CALL *pfn_wxime_get_version)(void);

#ifdef __cplusplus
}
#endif
#endif /* WXIME_H */
