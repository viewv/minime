> [!WARNING]
> **测试版**：MinIME 还处在早期测试阶段，可能存在大量问题，不建议作为唯一的输入法使用。
> **Beta**: MinIME is at an early testing stage and may have many problems.

## 本版更新（0.1.4）

- **首次运行引导**：第一次打开「MinIME 设置」会显示引导页：选择要下载的官方文件（引擎必需、整句模型推荐、离线语音可选），
  点「开始下载」后每一项都有进度条和速度，下载完自动启动引擎服务、把 MinIME 加进 fcitx5，不再需要在终端运行 `minime-fetch`。
  可以取消，失败了可以重试。以后在「设置 → 模型 → 官方文件」里也能下载缺少的文件或重新配置。
- 词库页在引擎还没运行过时给出说明，不再是空白。
- `minime-fetch` 加了 `--json`（每行一个事件），供设置程序显示进度；终端里的用法不变。

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
