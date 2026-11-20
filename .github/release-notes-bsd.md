> [!WARNING]
> **FreeBSD 实验版**：本发布来自 `exp/bsd` 实验分支，仅支持 **FreeBSD / GhostBSD amd64（x86-64）**，
> 目前处于早期实验阶段，可能存在未发现的问题，不建议作为唯一的输入法使用。
>
> **Experimental FreeBSD Build**: This pre-release is built from the `exp/bsd` branch for **FreeBSD / GhostBSD amd64 (x86-64)** only.

## 本版说明（0.2.1-bsd.1）

这是 MinIME 首个 FreeBSD 实验版本，基于 MinIME 0.2.1：

- **PE 装载器与 Win32 兼容层适配 FreeBSD amd64**：支持 `amd64_set_gsbase`、`_umtx_op`（替代 Linux futex）、FreeBSD libc 内置 jemalloc、`sysctlbyname` / `arc4random_buf`、`F_KINFO`、`SHM_ANON` 与 W^X 安全的 thunk/切页。
- **核心组件与服务适配**：`minime-engined`（含整句模型 CPU AVX2 与 Vulkan 动态加载）、`fcitx5-minime` 插件、`minime-netd`（云输入与同步）、`minime-asrd`（离线语音，支持 SoX `rec` OSS 录音回退）均已适配 `getpeereid` 与无 `/proc`、无 `systemd` 环境。
- **已在 GhostBSD（FreeBSD amd64，X11 + MATE）实机验证**：引擎装载、整句候选（CPU AVX2）、fcitx5 桌面打字与 `minime-netd` 云输入。

### 已知限制

- 暂不包含图形设置程序 `minime-settings`（上游 GPUI 在 FreeBSD 上暂无法编译），请使用 `fcitx5-configtool` 或直接编辑 `~/.config/fcitx5/conf/minime.conf`。
- 无 systemd 服务管理与 socket 激活，需要通过 `~/.config/autostart/` 或会话脚本启动后台服务（`minime-asrd` 使用常驻模式 `minime-asrd daemon`）。
- 跨设备剪贴板同步在 X11 下不可用（依赖 Wayland `wl-clipboard`）；其余联网功能不受影响。
- Vulkan 后端尚未在 FreeBSD 实机验证；没有 Vulkan 驱动时引擎会自动回退到 CPU 后端（要求 CPU 支持 AVX2 / FMA / F16C）。

## 下载

| 目标系统 | 原生 `.pkg`（推荐） | 通用压缩包 `.tar.xz` |
|---|---|---|
| FreeBSD 14 / GhostBSD (amd64) | `minime-0.2.1_freebsd14_amd64.pkg` | `minime-0.2.1_freebsd14_amd64.tar.xz` |
| FreeBSD 15 (amd64) | `minime-0.2.1_freebsd15_amd64.pkg` | `minime-0.2.1_freebsd15_amd64.tar.xz` |

软件包只包含 MinIME 自己的程序，只支持 amd64。`SHA256SUMS` 是各文件的校验值。

## 安装与使用

1. **安装软件包与建议的桌面依赖**：

   ```sh
   # 安装 MinIME（FreeBSD 14 / GhostBSD 示例；FreeBSD 15 请换用 freebsd15 包）
   sudo pkg install ./minime-0.2.1_freebsd14_amd64.pkg
   # 如果因 GhostBSD / 仓库 ABI 差异不方便用 pkg，也可以先装依赖再直接解压：
   # sudo pkg install fcitx5 python3 7-zip curl
   # sudo tar -C / -xf minime-0.2.1_freebsd14_amd64.tar.xz

   # 建议安装的界面、输入模块和中文字体
   sudo pkg install fcitx5-configtool fcitx5-gtk3 noto-sans-sc noto-emoji
   fc-cache -fv
   ```

2. **以普通用户身份下载官方文件**（不要加 sudo）：

   ```sh
   minime-fetch --no-setup
   ```

3. **配置环境变量、开机自启与添加输入法**：
   在 `~/.xprofile` 中设置 `GTK_IM_MODULE=fcitx`、`QT_IM_MODULE=fcitx`、`XMODIFIERS=@im=fcitx`，在 `~/.config/autostart/` 中自启 `fcitx5` 与 `minime-engined`，注销重登录后用 `fcitx5-configtool` 添加 `MinIME`。

完整步骤、云输入开启方法和常见问题见 [FreeBSD / GhostBSD 使用文档](https://github.com/viewv/minime/blob/exp/bsd/docs/bsd.md)。
