/* userdata.c — 换机助手用户数据的导入/导出小工具（走本地引擎）
 *
 * 用途：检查 export_engine_file.bin，以及把手机迁移包里的引擎数据
 * import 进本地引擎。格式说明见 开发笔记（未公开）。
 *
 *   userdata <安装目录> <工作目录> export <输出文件> <encrypt_key>
 *   userdata <安装目录> <工作目录> import <输入文件> <encrypt_key>
 *
 * encrypt_key 是迁移协议里 51 响应 field5 的 32 位十六进制文本。
 */
#include <stdio.h>
#include <string.h>

#include "engine.h"

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr,
                "用法:\n"
                "  %s <安装目录> <工作目录> export <输出文件> <encrypt_key>\n"
                "  %s <安装目录> <工作目录> import <输入文件> <encrypt_key>\n",
                argv[0], argv[0]);
        return 2;
    }
    const char *install = argv[1];
    const char *work = argv[2];
    const char *cmd = argv[3];
    const char *path = argv[4];
    const char *key = argv[5];

    if (strlen(key) != 32) {
        fprintf(stderr, "[userdata] encrypt_key 应为 32 个十六进制字符，实际 %zu\n",
                strlen(key));
        return 2;
    }
    if (wx_engine_open(install, work) != 0) {
        fprintf(stderr, "[userdata] 引擎初始化失败\n");
        return 3;
    }
    WximeUserDataParam param;
    memset(&param, 0, sizeof(param));
    param.data_path = path;
    param.data_path_len = (int32_t)strlen(path);
    param.encrypt_key = key;
    param.encrypt_key_len = (int32_t)strlen(key);

    int ok = -1;
    if (strcmp(cmd, "export") == 0) {
        if (!wx.export_user_data) {
            fprintf(stderr, "[userdata] 引擎没有导出 wxime_export_user_data\n");
            return 3;
        }
        ok = wx.export_user_data(&param) & 0xff;
    } else if (strcmp(cmd, "import") == 0) {
        if (!wx.import_user_data) {
            fprintf(stderr, "[userdata] 引擎没有导出 wxime_import_user_data\n");
            return 3;
        }
        ok = wx.import_user_data(&param) & 0xff;
    } else {
        fprintf(stderr, "[userdata] 未知动作 %s\n", cmd);
        return 2;
    }
    fprintf(stderr, "[userdata] %s(%s) -> %d (engine %s)\n",
            cmd, path, ok, wx.version ? wx.version : "?");
    return ok ? 0 : 1;
}
