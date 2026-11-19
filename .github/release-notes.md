> [!WARNING]
> **测试版**：MinIME 还处在早期测试阶段，可能存在大量问题，不建议作为唯一的输入法使用。
> **Beta**: MinIME is at an early testing stage and may have many problems.

## 本版更新（0.2.1）

- **输入体验与整句缓存**：修复连续打字或退格时候选焦点未归零的问题；为整句解码器增加 256 槽位 LRU 缓存，减少重复推理延迟。
- **整句模型 Vulkan 跨显卡适配**：新增通用子组归约着色器，兼容 Intel 核显及可变子组宽度 GPU；完善 Vulkan 1.1+ 与子组能力检测及工作组上限回退，避免部分显卡上初始化失败或输出异常。
- **Wayland 剪贴板同步恢复**：支持动态解析 `WAYLAND_DISPLAY` 并在桌面合成器重启后自动恢复剪贴板监听；修复剪贴板监听就绪前过早推进版本导致漏收早期消息的问题。
- **打包、安装与构建加固**：自动检测各发行版 fcitx5 插件与 systemd 用户单元目录；为 PE 装载器和多指令集 CPU 后端禁用 LTO 避免跨发行版构建失败；顶层串行构建防止打包环境内存溢出，并完善安装/卸载脚本校验。

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
