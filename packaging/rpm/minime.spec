# MinIME 的 RPM 包：只含本项目自己的程序。腾讯的官方文件由每个用户安装后运行 minime-fetch 下载。
#   rpmbuild -ba packaging/rpm/minime.spec   （源码包 minime-<版本>.tar.gz 放在 SOURCES/）
# 构建时 cargo 要联网取依赖；Rust 需要较新的 stable（发行版自带的太旧时用 rustup）。

Name:           minime
Version:        0.1.1
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
Recommends:     wl-clipboard pipewire-utils mesa-vulkan-drivers

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
MinIME 已安装。以普通用户身份继续（不要用 sudo）：
  1. minime-fetch                                   从腾讯官方下载引擎和整句模型
  2. systemctl --user enable --now minime-engined    启动引擎服务
  3. 重启 fcitx5，在 fcitx5 配置里添加「MinIME」
MSG

%files
%license %{_datadir}/licenses/minime/LICENSE
%{_bindir}/minime-engined
%{_bindir}/minime-netd
%{_bindir}/minime-settings
%{_bindir}/minime-asrd
%{_bindir}/minime-fetch
%{_libdir}/fcitx5/libminime.so
%{_datadir}/fcitx5/addon/minime.conf
%{_datadir}/fcitx5/inputmethod/minime.conf
%{_userunitdir}/minime-engined.service
%{_userunitdir}/minime-netd.service
%{_userunitdir}/minime-asrd.service
%{_userunitdir}/minime-asrd.socket
%{_datadir}/applications/minime-settings.desktop

%changelog
* Mon Nov 16 2026 MinIME contributors - 0.1.1-1
- Disable the engine's own word learning in sensitive (privacy) input fields

* Mon Nov 16 2026 MinIME contributors - 0.1.0-1
- First package
