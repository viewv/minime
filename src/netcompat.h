/* netcompat.h — ime_net.dll 兼容层（引擎网络请求转给 minime-netd），见 netcompat.c */
#pragma once

/* 在装载引擎之前调用：之后 libwxhld.dll 导入 ime_net.dll 时用我们的实现 */
void netcompat_install(void);
/* 「云输入」开关：关着时所有网络任务都返回 -1（默认关） */
void netcompat_set_enabled(int on);
/* 「同步个人词库」开关：关着时只放行云输入（若打开），开着时额外放行组同步请求和 COS */
void netcompat_set_sync(int on);
/* COS 任务（引擎的 client cos API）：转给 netd 上传/下载，完成后调 done(job, 0 成功 / -1 失败)。
 * 返回任务号（引擎当 job id），失败返回 -1 */
void netcompat_set_cos_done(void (*done)(int32_t job, int32_t result));
int32_t netcompat_cos(int upload, const char *url, int url_len, const char *path, int path_len);
