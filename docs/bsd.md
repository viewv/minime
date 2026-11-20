# 在 FreeBSD / GhostBSD 上使用（实验性）

> 状态：实验性。已在 GhostBSD（FreeBSD amd64，X11 + MATE）上实机验证：引擎、整句候选（CPU 后端）、fcitx5 打字、`minime-netd` 云输入。
> 只支持 **amd64**。NetBSD 只有核心组件可编译（未实机验证），OpenBSD 内核不允许用户态设置 `%gs` 基址，无法运行。

和 Linux 的差别主要是：没有 systemd（后台服务要自己拉起）、没有 Wayland 剪贴板、设置程序 `minime-settings` 暂不可用（上游 GPUI 在 FreeBSD 上编不过），改用 fcitx5 自带的配置面板。

## 1. 安装依赖

```sh
sudo pkg install gmake cmake pkgconf python3 7-zip curl rust \
    fcitx5 fcitx5-configtool fcitx5-gtk3 \
    noto-sans-sc noto-emoji
# 可选：GTK4 / Qt 应用的输入法模块
# sudo pkg install fcitx5-gtk4 fcitx5-qt6 fcitx5-qt5
# 可选：整句模型用 Vulkan 加速（没装会自动退回 CPU）
# sudo pkg install shaderc vulkan-headers vulkan-loader
# 可选：离线语音录音
# sudo pkg install sox
fc-cache -fv
```

GTK、Qt 的 fcitx5 模块在 FreeBSD 里按版本分包（`fcitx5-gtk3`、`fcitx5-qt6` 等）。默认系统没有中文字体，不装 `noto-sans-sc` 候选框里是方块。

## 2. 安装或编译

### 使用预编译包（推荐）

从 [Releases](https://github.com/viewv/minime/releases) 下载对应 FreeBSD 版本的实验包（仅支持 amd64）：

```sh
# 方式一：FreeBSD 原生 pkg（自动检查基础依赖）
sudo pkg install ./minime-*_freebsd14_amd64.pkg

# 方式二：通用压缩包（适用于 GhostBSD 或不同小版本，需自行先装好第 1 节的依赖）
sudo tar -C / -xf minime-*_freebsd14_amd64.tar.xz
```

### 从源码编译

BSD 上必须用 GNU Make（`gmake`）。顶层 Makefile 会自动选 `PREFIX=/usr/local`，跳过 `settings` 与 systemd 单元。

```sh
gmake                                   # 等同于 gmake engined addon netd asrd
sudo gmake install                      # 或 DESTDIR=… 打包
```

- 没有 `glslc` / Vulkan 头文件时，整句模型自动用 CPU 后端（要求 CPU 有 AVX2、FMA、F16C；虚拟机里要开 CPU 直通）。也可以显式 `gmake MINIME_NO_VULKAN=1 engined`。
- `settings` 在 FreeBSD 上目前编不过：`gpui-pre 0.3.12` 的 Wayland layer-shell 部分只为 Linux 写了 cfg。用下面第 5 节的 fcitx5 配置面板代替。

## 3. 下载官方文件

```sh
minime-fetch --no-setup        # 引擎 + 整句模型；--no-setup 跳过 systemctl / busctl
```

文件放在 `~/.local/share/minime/official/`。如果另一台机器已经下载过，也可以直接把整个 `~/.local/share/minime/official/` 目录复制过来。

## 4. 环境变量和开机自启

### 输入法环境变量

在 `~/.xprofile` 里写（LightDM 登录时会读取）：

```sh
export GTK_IM_MODULE=fcitx
export QT_IM_MODULE=fcitx
export XMODIFIERS=@im=fcitx
```

> **必须先设好这些变量、再启动应用。** 设置之前已经打开的程序接不上 fcitx5，需要注销重新登录（或重启这些程序）。

### 后台服务自启

没有 systemd，用桌面的 XDG 自启（MATE、Xfce、KDE 等都认 `~/.config/autostart/`）：

```sh
mkdir -p ~/.config/autostart

# fcitx5（包里自带的话直接复制）
cp /usr/local/share/applications/org.fcitx.Fcitx5.desktop ~/.config/autostart/

# 输入法引擎（必需）
cat > ~/.config/autostart/minime-engined.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=MinIME Engine
Exec=/usr/local/bin/minime-engined
EOF

# 网络服务（只有要用云输入、跨设备、词库同步时才需要）
cat > ~/.config/autostart/minime-netd.desktop <<'EOF'
[Desktop Entry]
Type=Application
Name=MinIME Net
Exec=/usr/local/bin/minime-netd
EOF

# 离线语音（可选，常驻模式，没有 socket 激活；需要 sox 或 PulseAudio）
# cat > ~/.config/autostart/minime-asrd.desktop <<'EOF'
# [Desktop Entry]
# Type=Application
# Name=MinIME ASR
# Exec=/usr/local/bin/minime-asrd daemon
# EOF
```

`fcitx5-configtool` 不用自启，它只是配置界面。

没有桌面自启机制的环境（只用 X 会话脚本）可以把 `minime-engined &`、`fcitx5 -d` 写进 `~/.xinitrc`。

## 5. 添加输入法和配置

```sh
fcitx5-configtool
```

1. 「输入法」页点左下角 `+`，取消「只显示当前语言」，搜 `MinIME` 加入列表，应用。
2. 「附加组件」页搜 `MinIME`，点右边的齿轮：就是 MinIME 的配置面板（模糊音、繁体、整句候选、每页候选数等，选项说明见 [input](input.md)）。

配置存在 `~/.config/fcitx5/conf/minime.conf`，面板和手改是同一份文件。改文件后让 fcitx5 重读：

```sh
fcitx5-remote -r
```

## 6. 打开云输入（云拼音）

云输入默认关闭，只有你主动打开才会联网。需要两步：

1. **先启动 `minime-netd`**（第 4 节自启，或手动 `minime-netd &`）。它负责生成匿名设备身份并代引擎发请求。
2. 打开开关，在 `~/.config/fcitx5/conf/minime.conf` 里写：

   ```ini
   [Network]
   Enabled=True
   CloudInput=True
   ```

   如果还要同步个人词库，再加 `UserDictSync=True`。

`minime-engined` 每 2 秒读一次这个文件，不用重启。成功时引擎日志会出现 `云输入：打开，network_login -> 0`。查看日志要前台运行一次：

```sh
pkill minime-engined; minime-engined
```

云候选在候选框里带 `☁` 标记。如果提示「没有 netd 的设备身份」，说明 `minime-netd` 还没运行或还没生成 `~/.local/share/minime/net/identity.json`，启动它即可（开关会自动重试）。

关闭云输入：把 `CloudInput` 改回 `False`（或删掉 `Enabled=True`）。

隐私边界和 Linux 一致：上屏文字、拼音的上报以及日志上报在引擎里一直被拦截；见 [privacy](privacy.md)。

BSD 上的限制：剪贴板同步依赖 `wl-clipboard`（Wayland），X11 下不工作；其余联网功能不受影响。

## 7. 常见问题

| 现象 | 原因和办法 |
|---|---|
| fcitx5 里搜不到 MinIME | 插件没装到 fcitx5 的目录。检查 `/usr/local/lib/fcitx5/libminime.so`、`/usr/local/share/fcitx5/addon/minime.conf`、`.../inputmethod/minime.conf`，然后 `fcitx5-remote -r` 或重启 fcitx5 |
| 只有新开的程序能打字 | 环境变量要在应用启动前设好，见第 4 节 |
| 能切到 MinIME 但没有候选 | `minime-engined` 没在运行，或官方文件没下载。手动前台运行看日志 |
| 候选框、Emoji 是方块 | 没装字体：`pkg install noto-sans-sc noto-emoji && fc-cache -fv` |
| 整句候选没有 | 没有整句模型（`minime-fetch`），或 CPU 不支持 AVX2/FMA/F16C。日志里搜「e2e」 |
| 启动时报 `engined: 已有服务在 … 运行` | 已经有一个实例，`pkill minime-engined` 后重启 |
| 编译 `settings` 失败 | 已知问题，见第 2 节，用 fcitx5 配置面板 |

## 8. 已知差异

- 没有 `minime-settings`，所有选项通过 fcitx5 面板或 `minime.conf` 设置；`minime-fetch` 的下载也要用命令行。
- 没有 systemd 的服务管理和 socket 激活；`minime-asrd` 以常驻模式运行。
- Vulkan 后端没有实机验证，默认和自动回退都是 CPU。
