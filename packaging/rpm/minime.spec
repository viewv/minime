# MinIME 的 RPM 包：只含本项目自己的程序。腾讯的官方文件由每个用户安装后运行 minime-fetch 下载。
#   rpmbuild -ba packaging/rpm/minime.spec   （源码包 minime-<版本>.tar.gz 放在 SOURCES/）
# 构建时 cargo 要联网取依赖；Rust 需要较新的 stable（发行版自带的太旧时用 rustup）。

Name:           minime
Version:        0.2.1
Release:        1%{?dist}
Summary:        Native Linux fcitx5 input method running the WeType engine
License:        MPL-2.0
URL:            https://github.com/viewv/minime
Source0:        %{name}-%{version}.tar.gz
ExclusiveArch:  x86_64

BuildRequires:  gcc gcc-c++ make cmake pkgconfig systemd-rpm-macros
BuildRequires:  fcitx5-devel
BuildRequires:  glslc vulkan-headers jemalloc-devel
BuildRequires:  libxkbcommon-devel libxkbcommon-x11-devel libxcb-devel wayland-devel fontconfig-devel
Requires:       fcitx5 python3 curl vulkan-loader libwayland-client fontconfig
Requires:       (7zip or p7zip-plugins)
Recommends:     fcitx5-configtool fcitx5-qt fcitx5-gtk wl-clipboard pipewire-utils mesa-vulkan-drivers opencc

# Rust 二进制不带调试信息分包
%global debug_package %{nil}
# PE 装载器（含汇编桩）与多指令集 e2e_cpu.c 禁用 LTO（与 Arch PKGBUILD 对齐）
%global _lto_cflags %{nil}

%description
MinIME loads the official WeType (微信输入法) x86-64 engine natively with its
own PE loader, without Wine. The package contains only MinIME's own programs;
after installing, each user runs minime-fetch to download the official engine,
dictionaries and models from Tencent's servers.

%prep
%autosetup

%build
make %{?_smp_mflags}

%install
make install DESTDIR=%{buildroot} PREFIX=%{_prefix} LIBDIR=%{_libdir} USERUNITDIR=%{_userunitdir}

%post
if [ "$1" -eq 1 ]; then
cat <<'MSG'
MinIME 已安装。接下来以普通用户身份运行（不要用 sudo）：
  minime-fetch
它会从腾讯官方下载引擎和整句模型，启动引擎服务，并把 MinIME 加进 fcitx5 的输入法列表。
fcitx5 要先设为系统的输入法（KDE：系统设置 → 键盘 → 虚拟键盘 → Fcitx 5），然后按 Ctrl+Space 切换。
详见 https://github.com/viewv/minime#安装
MSG
else
  echo "MinIME 已更新：运行 minime-fetch setup（重启引擎服务），再重新登录或重启 fcitx5 生效"
fi

%files
%license %{_datadir}/licenses/minime/LICENSE
%{_bindir}/minime-engined
%{_bindir}/minime-netd
%{_bindir}/minime-settings
%{_bindir}/minime-asrd
%{_bindir}/minime-fetch
%{_bindir}/minime-uninstall
%{_libdir}/fcitx5/libminime.so
%{_datadir}/fcitx5/addon/minime.conf
%{_datadir}/fcitx5/inputmethod/minime.conf
%{_userunitdir}/minime-engined.service
%{_userunitdir}/minime-netd.service
%{_userunitdir}/minime-asrd.service
%{_userunitdir}/minime-asrd.socket
%{_datadir}/applications/minime-settings.desktop

%changelog
* Thu Nov 19 2026 MinIME contributors - 0.2.1-1
- Fix candidate focus reset on typing/backspace and add 256-slot LRU cache for whole-sentence decoding
- Add generic subgroup matmul shader and Vulkan 1.1+ capability checks for Intel iGPUs and variable-subgroup GPUs
- Auto-recover Wayland clipboard monitoring across compositor restarts and fix early clipboard version sync race
- Harden cross-distro packaging, LTO handling, fcitx5 plugin path detection, and top-level build serialization

* Wed Nov 18 2026 MinIME contributors - 0.2.0-1
- Improve typing responsiveness, voice input (model reuse, 5-minute limit, interruptible recording), and settings UI fluidity
- Fix PE loader dependency order/resource leaks, e2e long-input & fuzzy pinyin bugs, and netd/settings sync races
- Harden netd/asrd/scripts boundaries, argument checks, and Debian/Fedora/systemd packaging

* Wed Nov 18 2026 MinIME contributors - 0.1.4-1
- First-run setup wizard in the settings app: choose files, download with progress, configure
- minime-fetch --json event stream for the settings app
* Tue Nov 17 2026 MinIME contributors - 0.1.3-1
- Add minime-uninstall (per-user cleanup) and make uninstall
- Settings app draws its own title bar on desktops without server-side decorations (GNOME)
* Tue Nov 17 2026 MinIME contributors - 0.1.2-1
- minime-fetch now enables the engine service and adds MinIME to fcitx5 after downloading
- Download progress; settings show the downloaded whole-sentence model
- Recommend fcitx5 config tool and Qt/GTK frontends; protocol constants loaded from ime_net.dll
* Mon Nov 16 2026 MinIME contributors - 0.1.1-1
- Disable the engine's own word learning in sensitive (privacy) input fields

* Mon Nov 16 2026 MinIME contributors - 0.1.0-1
- First package
