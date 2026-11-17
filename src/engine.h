/* engine.h — 引擎适配层：装载 libwxhld.dll、初始化、配词库、建会话。
 *
 * 这是服务里唯一知道引擎结构体布局的地方（布局定义在 wxime.h）。
 * 服务本身（engined.c）只通过这里的函数指针和事件结构跟引擎打交道。
 */
#ifndef WX_ENGINE_H
#define WX_ENGINE_H

#include <stdint.h>

#include "wxime.h"

typedef struct WxEngine {
    pfn_wxime_create_session        create_session;
    pfn_wxime_add_session_listener  add_session_listener;
    pfn_wxime_process_input         process_input;
    pfn_wxime_candidate_next        candidate_next;
    pfn_wxime_delete_candidate_iterator delete_candidate_iterator;
    pfn_wxime_select_candidate      select_candidate;
    pfn_wxime_drop_last             drop_last;
    pfn_wxime_reset_session         reset_session;
    pfn_wxime_free_pending_input    free_pending_input;
    pfn_wxime_destroy_session       destroy_session;
    /* 光标周围文字：(sid, 光标前, 长度, 光标后, 长度, 选区, 长度, 原因)，签名从 Mac Swift 逆出 */
    int (WXIME_CALL *set_text_around_cursor)(uint64_t sid, const char *before, int before_len,
                                             const char *after, int after_len,
                                             const char *selection, int selection_len, int reason);
    pfn_wxime_finalize              finalize;
    /* 换机助手的用户数据导入/导出（可选：3.0.1.35 起才有） */
    pfn_wxime_export_user_data      export_user_data;
    pfn_wxime_import_user_data      import_user_data;
    /* 细胞词库（可选） */
    pfn_wxime_add_cell_dict         add_cell_dict;
    pfn_wxime_network_login         network_login;
    pfn_wxime_reset_cli_ab_test_info reset_ab;
    pfn_wxime_group_sync_info_changed group_sync_info_changed;
    pfn_wxime_register_client_cos_api register_client_cos_api;
    pfn_wxime_client_cos_job_completed client_cos_job_completed;
    pfn_wxime_get_most_likely_sequel get_most_likely_sequel;
    pfn_wxime_block_word            add_word_to_user_block_list;
    pfn_wxime_block_word            remove_word_from_user_block_list;
    pfn_wxime_clear_cell_dicts      clear_cell_dicts;
    const char *version;            /* 引擎自报版本 */
} WxEngine;

extern WxEngine wx;

/* 装载并初始化引擎。install_dir 含 libwxhld.dll 和 BuildIn/，work_dir 放用户词库等。
 * 失败返回 -1 并在 stderr 说明原因。 */
int wx_engine_open(const char *install_dir, const char *work_dir);
/* 细胞词库配置（~/.local/share/minime/dicts/cells.tsv）变了就重新加载；主线程调用 */
void wx_engine_cells_poll(void);

/* 用官方会话配置建一个会话并挂上监听回调；失败返回 0 */
/* 会话选项（用户配置）：fuzzy 第 i 位 = SessionConfig+0x0d+i 的模糊音开关
 * （0 n/l、1 r/l、2 h/f、3 g/k、4 an/ang、5 ian/iang、6 uan/uang、7 c/ch、8 s/sh、9 z/zh、
 *  10 hui/fei、11 en/eng、12 in/ing、14 huang/wang，实测见 docs/input.md）；traditional 非 0 出繁体 */
/* emoji：表情候选，0 = Emoji（官方默认）、1 = 颜文字、2 = 不出（SessionConfig +0x34 Emoji / +0x35 颜文字；
 * 两个都开时那个位置给 Emoji，实测见 docs/input.md「表情候选」） */
/* scheme：输入方案，0 全拼、1 双拼、2 五笔。shuangpin：双拼方案（SessionConfig +0x20），1 搜狗、2 微软、
 * 3 小鹤、4 拼音加加、5 自然码、6 紫光、7 智能 ABC。wubi：五笔选项位，bit0 五码顶字（+0x26）、
 * bit1 四码唯一自动上屏（+0x25）、bit2 拼音混输（+0x24）；五笔用 keyboard_type=5（+0x04）。
 * 五笔 86/98/新世纪 取决于加载了哪个 swbd 词库（实测见 docs/input.md「双拼与五笔」） */
typedef struct { uint32_t fuzzy; int traditional; int emoji; int scheme; int shuangpin; int wubi; } WxSessOpts;
extern WxSessOpts wx_opts;           /* 新建会话用的选项（engined 按前端配置改） */
uint64_t wx_engine_new_session(WximeListener cb, void *userdata);
/* no_learn=1：隐私输入框用的会话，引擎不学选词（会话配置 disable_user_dict） */
uint64_t wx_engine_new_session_ex(WximeListener cb, void *userdata, int no_learn);

/* 1601 年起的毫秒数，给 process_input / select_candidate 的 extra 用 */
uint64_t wx_wall_ms(void);

#endif
