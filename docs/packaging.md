# 打包

MinIME 的软件包**只包含本项目自己的程序**。腾讯的官方文件（引擎、词库、模型）不进软件包，
由每个用户安装后运行 `minime-fetch`，从腾讯官方地址下载到自己的数据目录。

## 安装布局（`make install PREFIX=/usr`）

| 文件 | 说明 |
|---|---|
| `/usr/bin/minime-engined` | 引擎服务（`src/` 的 `build/native/engined`） |
| `/usr/bin/minime-netd`、`minime-asrd`、`minime-settings` | 网络服务、离线语音、设置程序（Rust） |
| `/usr/bin/minime-fetch` | 下载官方文件（`scripts/minime-fetch`，Python 标准库 + 7z） |
| `/usr/lib/fcitx5/libminime.so`、`/usr/share/fcitx5/{addon,inputmethod}/minime.conf` | fcitx5 插件 |
| `/usr/lib/systemd/user/minime-*.{service,socket}` | 用户服务（`packaging/systemd/`），由用户自己 `systemctl --user enable` |
| `/usr/share/applications/minime-settings.desktop` | 设置程序的菜单项 |

构建依赖：gcc、cmake、Rust、fcitx5 开发文件、shaderc（`glslc`，把整句模型的计算着色器编进程序）、Vulkan 头文件；
有 jemalloc 时自动链接（推荐）。运行时依赖：fcitx5、jemalloc、python3、7z、curl（下载语音模型）；可选 vulkan 驱动（整句模型走 GPU）、
wl-clipboard（剪贴板同步）、PipeWire 或 PulseAudio（语音录音）。

| 发行版 | 打包文件 | 怎么打 |
|---|---|---|
| Arch | `packaging/arch/PKGBUILD` | `makepkg -si` |
| Debian / Ubuntu | `packaging/deb/build-deb.sh` | `make && packaging/deb/build-deb.sh`，产物在 `dist/` |
| Fedora | `packaging/rpm/minime.spec` | 把 `minime-<版本>.tar.gz`（顶层目录 `minime-<版本>/`）放进 `~/rpmbuild/SOURCES/`，`rpmbuild -bb` |

- 版本号在仓库根目录的 `VERSION`。
- 只支持当前的发行版。fcitx5 插件用到较新的接口（候选备注、候选右键菜单、自定义提示、`FCITX_ADDON_FACTORY_V2`），
  CMake 逐个探测，fcitx5 太旧（例如 Ubuntu 24.04 自带的）时直接报错，不做兼容。
- fcitx5 插件目录取 fcitx5 自己的 `pkg-config --variable=libdir Fcitx5Core`：Arch `/usr/lib/fcitx5`、
  Debian/Ubuntu `/usr/lib/x86_64-linux-gnu/fcitx5`、Fedora `/usr/lib64/fcitx5`。插件要按各发行版自己的 fcitx5 编译，
  所以每个发行版（和大版本）各打一次包。
- 构建依赖（容器测试里用的就是这些）：
  - Arch：`base-devel cmake rust shaderc vulkan-headers fcitx5`（PKGBUILD 里写好了）
  - Debian / Ubuntu：`build-essential cmake pkg-config git curl file dpkg-dev python3 fcitx5 libfcitx5core-dev
    libfcitx5config-dev libfcitx5utils-dev glslc libvulkan-dev libjemalloc-dev libxkbcommon-dev libxkbcommon-x11-dev
    libxcb1-dev libwayland-dev libfontconfig-dev`
  - Fedora：`gcc gcc-c++ make cmake pkgconf git curl file rpm-build python3 fcitx5-devel systemd-rpm-macros glslc
    vulkan-headers jemalloc-devel libxkbcommon-devel libxkbcommon-x11-devel libxcb-devel wayland-devel fontconfig-devel`
  - Rust：Debian / Ubuntu / Fedora 都用 rustup 装的 stable（发行版自带的可能太旧）。
- deb 的库依赖由 `dpkg-shlibdeps` 从实际二进制算出，rpm 由 rpmbuild 自动生成；工具类依赖（python3、7z、curl）和设置程序运行时 dlopen 的库（Vulkan、Wayland、fontconfig）手写。
- 设置程序（GPUI）需要较新的 Rust stable，发行版自带的 Rust 太旧时用 rustup。cargo 构建时要联网取依赖。

## 官方文件（`minime-fetch`）

放在 `$XDG_DATA_HOME/minime/official/`（缺省 `~/.local/share/minime/official/`）：

| 命令 | 来源 | 取哪些 | 校验 |
|---|---|---|---|
| `minime-fetch engine` | Windows 安装包 `WeTypeSetup_3.0.1.35_3.exe`（约 218 MB） | `libwxhld.dll` 和它递归导入的同目录 DLL（XNet、tensorflowlite_c、dbghelp、ime_net、wcwss）、`BuildIn/` 词库，解包后约 270 MB | 安装包 SHA-256 |
| `minime-fetch e2e` | macOS 安装包（325 MB，不整包下载） | 用 HTTP Range 只取 `imeData.bundle` 里的 `index.json`、`mpd_e2e.bin`、`decgnr.bin`（约 78 MB） | zip CRC + 每个文件的 MD5 |
| `minime-fetch voice` | 官方离线语音模型包 `ime_asr_decoder_model`（输入法按需从腾讯 CDN 下载的资源包，约 103 MB，文件扩展名是 .apk 但不是安卓应用） | 交给 `minime-asrd fetch-model` 解包到 `~/.local/share/minime/asr/model/` | 资源包 MD5 |

不带参数时下载引擎和整句模型。`minime-engined` 不带参数启动时就用这里的文件；找不到引擎时以退出码 2 退出，
用户服务里 `RestartPreventExitStatus=2`，不会反复重启。设置程序「关于」页会提示先运行 `minime-fetch`。

## 官方文件保持原样

`minime-fetch` 下载的官方文件解包后不做任何修改。引擎需要的兼容处理（例如跳过安装包里没有的整句组件）
都在装载时于内存中完成，见 [engine](engine.md)。

## 干净环境测试

在全新的 Arch 容器里：用 PKGBUILD 构建并安装 → `minime-fetch engine` → 不带参数启动 `minime-engined` →
用 `scripts/engined_test.py` 打字（全拼、五笔）。容器里没有桌面，所以不测 fcitx5 和设置程序的界面。
