> [!WARNING]
> **测试版**：MinIME 还处在早期测试阶段，可能存在大量问题，不建议作为唯一的输入法使用。
> **Beta**: MinIME is at an early testing stage and may have many problems.

## 本版更新（0.2.0）

- **输入体验与整句模型**：修复长拼音输入位置编码越界、同时开启多组模糊音（如 `n=l` 与 `r=l`）漏匹配、`sendContext` 切分卡顿、双拼退格及语音停录窗口吞键问题；修复多个核心资源与 FD 泄漏。
- **离线语音（`minime-asrd`）**：保留启动预加载的语音模型避免每次重加载（仍保持空闲 10 分钟自动退出省内存），支持随时打断卡住的录音进程；单次说话时长上限提升至 5 分钟并在候选栏实时展示计时。
- **跨设备同步与网络服务（`minime-netd`）**：修复断连重连卡死、图片剪贴板回声、配对码泄漏及配对后同步触发；补充帧长、Snappy/ZIP 解压上限与 ECDH 公钥校验等安全加固。
- **设置程序（`minime-settings`）**：移除 UI 线程同步阻塞与子进程僵尸泄漏，修复配置/词库同步竞态与快捷键清空问题。
- **安装与打包脚本**：`minime-uninstall`/`minime-fetch` 增加严格参数校验防误删；适配 Debian `libopencc.so.1.1` 与多发行版 fcitx5 插件路径。

## 下载

| 发行版 | 文件 | 安装 |
|---|---|---|
| Arch Linux | `minime-git-*.pkg.tar.zst` | `sudo pacman -U minime-git-*.pkg.tar.zst` |
| Debian 13 | `minime_*debian13_amd64.deb` | `sudo apt install ./minime_*debian13_amd64.deb` |
| Ubuntu 26.10 | `minime_*ubuntu26.10_amd64.deb` | `sudo apt install ./minime_*ubuntu26.10_amd64.deb` |
| Fedora 44 | `minime-*.fc44.x86_64.rpm` | `sudo dnf install ./minime-*.fc44.x86_64.rpm` |

软件包只包含 MinIME 自己的程序，只支持 x86-64。`SHA256SUMS` 是各文件的校验值。

## 安装之后

1. **把 fcitx5 设为输入法**：KDE Plasma 在「系统设置 → 键盘 → 虚拟键盘」选 Fcitx 5；
   其他桌面：Debian / Ubuntu 运行 `im-config -n fcitx5`，Fedora 安装 `fcitx5-autostart`。然后注销重新登录。
2. **以普通用户身份运行**（不要加 sudo）：

   ```bash
   minime-fetch
   ```

   它从腾讯官方下载引擎、词库和整句模型（整句模型的服务器较慢，可能要几分钟），启动引擎服务，并把 MinIME 加进 fcitx5 的输入法列表。
3. 按 **Ctrl+Space** 切换到 MinIME 开始打字。

离线语音、跨设备、常见问题见 [README](https://github.com/viewv/minime#安装)。遇到问题欢迎提交 Issue。
