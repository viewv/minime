# MinIME 的 RPM 包：只含本项目自己的程序。腾讯的官方文件由每个用户安装后运行 minime-fetch 下载。
#   rpmbuild -ba packaging/rpm/minime.spec   （源码包 minime-<版本>.tar.gz 放在 SOURCES/）
# 构建时 cargo 要联网取依赖；Rust 需要较新的 stable（发行版自带的太旧时用 rustup）。

Name:           minime
Version:        0.1.4
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
Recommends:     fcitx5-configtool fcitx5-qt fcitx5-gtk wl-clipboard pipewire-utils mesa-vulkan-drivers

# Rust 二进制不带调试信息分包
%global debug_package %{nil}

%description
MinIME loads the official WeType (微信输入法) x86-64 engine natively with its
own PE loader, without Wine. The package contains only MinIME's own programs;
after installing, each user runs minime-fetch to download the official engine,
dictionaries and models from Tencent's servers.

%prep
%autosetup

%build
make

%install
make install DESTDIR=%{buildroot} PREFIX=%{_prefix}

%post
cat <<'MSG'
MinIME 已安装。接下来以普通用户身份运行（不要用 sudo）：
  minime-fetch
它会从腾讯官方下载引擎和整句模型，启动引擎服务，并把 MinIME 加进 fcitx5 的输入法列表。
fcitx5 要先设为系统的输入法（KDE：系统设置 → 键盘 → 虚拟键盘 → Fcitx 5），然后按 Ctrl+Space 切换。
详见 https://github.com/viewv/minime#安装
MSG

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
