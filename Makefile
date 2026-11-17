# MinIME 顶层构建 / 安装（给打包用；开发时也可以继续用 scripts/install_*.sh）
#
#   make                       构建全部组件
#   make install DESTDIR=… PREFIX=/usr
#
# 只安装本项目自己的程序。腾讯的官方文件由每个用户运行 minime-fetch 下载到自己的数据目录。

PREFIX     ?= /usr
BINDIR     ?= $(PREFIX)/bin
LIBDIR     ?= $(PREFIX)/lib
DATADIR    ?= $(PREFIX)/share
USERUNITDIR ?= $(LIBDIR)/systemd/user
# fcitx5 插件目录跟着 fcitx5 自己的 libdir 走（Arch /usr/lib、Debian /usr/lib/x86_64-linux-gnu、Fedora /usr/lib64）
FCITX5LIBDIR ?= $(shell pkg-config --variable=libdir Fcitx5Core 2>/dev/null || echo $(LIBDIR))/fcitx5
VERSION    := $(shell cat VERSION)
CARGO      ?= cargo
CARGOFLAGS ?= --release --locked

ADDON_BUILD := fcitx5-minime/build

.PHONY: all engined addon netd settings asrd install clean

all: engined addon netd settings asrd

engined:
	$(MAKE) -C src native

addon:
	cmake -S fcitx5-minime -B $(ADDON_BUILD) -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$(PREFIX)
	cmake --build $(ADDON_BUILD)

netd:
	cd netd && $(CARGO) build $(CARGOFLAGS)

settings:
	cd settings && $(CARGO) build $(CARGOFLAGS)

asrd:
	cd asrd && $(CARGO) build $(CARGOFLAGS)

install:
	install -Dm755 build/native/engined            $(DESTDIR)$(BINDIR)/minime-engined
	install -Dm755 netd/target/release/minime-netd $(DESTDIR)$(BINDIR)/minime-netd
	install -Dm755 settings/target/release/minime-settings $(DESTDIR)$(BINDIR)/minime-settings
	install -Dm755 asrd/target/release/minime-asrd $(DESTDIR)$(BINDIR)/minime-asrd
	install -Dm755 scripts/minime-fetch            $(DESTDIR)$(BINDIR)/minime-fetch
	install -Dm755 $(ADDON_BUILD)/libminime.so     $(DESTDIR)$(FCITX5LIBDIR)/libminime.so
	install -Dm644 fcitx5-minime/minime-addon.conf $(DESTDIR)$(DATADIR)/fcitx5/addon/minime.conf
	sed -i 's|^Library=.*|Library=libminime|'      $(DESTDIR)$(DATADIR)/fcitx5/addon/minime.conf
	install -Dm644 fcitx5-minime/minime-inputmethod.conf $(DESTDIR)$(DATADIR)/fcitx5/inputmethod/minime.conf
	install -Dm644 -t $(DESTDIR)$(USERUNITDIR) packaging/systemd/minime-engined.service \
		packaging/systemd/minime-netd.service packaging/systemd/minime-asrd.service packaging/systemd/minime-asrd.socket
	install -Dm644 packaging/minime-settings.desktop $(DESTDIR)$(DATADIR)/applications/minime-settings.desktop
	install -Dm644 LICENSE                         $(DESTDIR)$(DATADIR)/licenses/minime/LICENSE

clean:
	rm -rf build/native $(ADDON_BUILD)
	cd netd && $(CARGO) clean
	cd settings && $(CARGO) clean
	cd asrd && $(CARGO) clean
