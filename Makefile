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
# systemd 用户单元统一在 $(PREFIX)/lib/systemd/user，不跟随 Fedora /usr/lib64 或 Debian multiarch LIBDIR
USERUNITDIR ?= $(if $(filter /usr,$(PREFIX)),$(shell pkg-config --variable=systemduserunitdir systemd 2>/dev/null || echo $(PREFIX)/lib/systemd/user),$(PREFIX)/lib/systemd/user)
# fcitx5 插件目录跟着 fcitx5 自己的 libdir 走（Arch /usr/lib、Debian /usr/lib/x86_64-linux-gnu、Fedora /usr/lib64）
FCITX5LIBDIR ?= $(shell pkg-config --variable=libdir Fcitx5Core 2>/dev/null || echo $(LIBDIR))/fcitx5
VERSION    := $(shell cat VERSION)
CARGO      ?= cargo
CARGOFLAGS ?= --release --locked

ADDON_BUILD := fcitx5-minime/build

.PHONY: all engined addon netd settings asrd install uninstall clean

# 避免顶层并行同时启动多个 Cargo/CMake 构建耗尽内存；各子构建内部仍按 jobserver / --parallel / Cargo 并发
.NOTPARALLEL:
all: engined addon netd settings asrd

engined:
	$(MAKE) -C src ../build/native/engined

addon:
	cmake -S fcitx5-minime -B $(ADDON_BUILD) -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$(PREFIX)
	cmake --build $(ADDON_BUILD) --parallel

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
	install -Dm755 scripts/minime-uninstall        $(DESTDIR)$(BINDIR)/minime-uninstall
	install -Dm755 $(ADDON_BUILD)/libminime.so     $(DESTDIR)$(FCITX5LIBDIR)/libminime.so
	install -Dm644 fcitx5-minime/minime-addon.conf $(DESTDIR)$(DATADIR)/fcitx5/addon/minime.conf
	install -Dm644 fcitx5-minime/minime-inputmethod.conf $(DESTDIR)$(DATADIR)/fcitx5/inputmethod/minime.conf
	install -Dm644 -t $(DESTDIR)$(USERUNITDIR) packaging/systemd/minime-engined.service \
		packaging/systemd/minime-netd.service packaging/systemd/minime-asrd.service packaging/systemd/minime-asrd.socket
	install -Dm644 packaging/minime-settings.desktop $(DESTDIR)$(DATADIR)/applications/minime-settings.desktop
	install -Dm644 LICENSE                         $(DESTDIR)$(DATADIR)/licenses/minime/LICENSE

# 删掉 make install 装的文件（每个用户自己的服务和数据用 minime-uninstall 清理）
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/minime-engined $(DESTDIR)$(BINDIR)/minime-netd $(DESTDIR)$(BINDIR)/minime-settings \
		$(DESTDIR)$(BINDIR)/minime-asrd $(DESTDIR)$(BINDIR)/minime-fetch $(DESTDIR)$(BINDIR)/minime-uninstall
	rm -f $(DESTDIR)$(FCITX5LIBDIR)/libminime.so $(DESTDIR)$(DATADIR)/fcitx5/addon/minime.conf \
		$(DESTDIR)$(DATADIR)/fcitx5/inputmethod/minime.conf
	rm -f $(DESTDIR)$(USERUNITDIR)/minime-engined.service $(DESTDIR)$(USERUNITDIR)/minime-netd.service \
		$(DESTDIR)$(USERUNITDIR)/minime-asrd.service $(DESTDIR)$(USERUNITDIR)/minime-asrd.socket
	rm -f $(DESTDIR)$(DATADIR)/applications/minime-settings.desktop
	rm -rf $(DESTDIR)$(DATADIR)/licenses/minime

clean:
	rm -rf build/native $(ADDON_BUILD)
	cd netd && $(CARGO) clean
	cd settings && $(CARGO) clean
	cd asrd && $(CARGO) clean
