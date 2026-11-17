> [!WARNING]
> **测试版**：MinIME 还处在早期测试阶段，可能存在大量问题，不建议作为唯一的输入法使用。
> **Beta**: MinIME is at an early testing stage and may have many problems.

## 本版更新（0.1.3）

- **卸载**：新增 `minime-uninstall`，以普通用户身份运行，清理 MinIME 的服务、fcitx5 里的输入法设置、下载的官方文件和数据；
  `--dry-run` 只列出要删的内容，`--keep-data` 保留用户词库、常用语和设备身份。软件包本身仍用包管理器卸载。
- **GNOME**：设置程序在 GNOME 这类不替程序画标题栏的桌面上，自己画标题栏（关闭、最小化、最大化、拖动）和可以拖动改大小的边框。
  之前在 GNOME 上窗口没有关闭按钮，也拖不动。

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
