#!/bin/bash
# build-deb.sh — 在 Debian / Ubuntu 上把已经 make 好的 MinIME 打成 .deb（只含本项目的程序）
#
#   make && packaging/deb/build-deb.sh        产物在 dist/minime_<版本>-1_amd64.deb
#
# 库依赖用 dpkg-shlibdeps 从实际的二进制算出来（各发行版、各版本的包名不同）；
# 运行时还要的工具（python3、7z、curl）和 fcitx5 手写在下面。
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
cd "$R"
VERSION=$(cat VERSION)
REL=${DEB_RELEASE:-1}
ARCH=$(dpkg --print-architecture)
[ "$ARCH" = amd64 ] || { echo "MinIME 只支持 amd64（引擎是 x86-64 机器码）" >&2; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
make install DESTDIR="$STAGE/root" PREFIX=/usr >/dev/null
# Debian 习惯：许可证放 /usr/share/doc/<包>/copyright
mkdir -p "$STAGE/root/usr/share/doc/minime"
mv "$STAGE/root/usr/share/licenses/minime/LICENSE" "$STAGE/root/usr/share/doc/minime/copyright"
rm -rf "$STAGE/root/usr/share/licenses"

# dpkg-shlibdeps 要在一个有 debian/control 的目录里跑
mkdir -p "$STAGE/debian"
printf 'Source: minime\n\nPackage: minime\nArchitecture: any\n' > "$STAGE/debian/control"
BINS=$(find "$STAGE/root/usr/bin" "$STAGE/root/usr/lib" -type f \( -perm -u+x -o -name '*.so' \) -exec sh -c 'file -b "$1" | grep -q ELF' _ {} \; -print)
SHLIBS=$(cd "$STAGE" && dpkg-shlibdeps -O $BINS 2>/dev/null | sed -n 's/^shlibs:Depends=//p')

SIZE=$(du -sk "$STAGE/root" | cut -f1)
mkdir -p "$STAGE/root/DEBIAN"
cat > "$STAGE/root/DEBIAN/control" <<CTL
Package: minime
Version: $VERSION-$REL
Architecture: $ARCH
Maintainer: MinIME contributors
Installed-Size: $SIZE
Depends: $SHLIBS, libvulkan1, libwayland-client0, libfontconfig1, fcitx5, python3, 7zip | p7zip-full, curl
Recommends: wl-clipboard, pipewire-bin | pulseaudio-utils, mesa-vulkan-drivers
Section: utils
Priority: optional
Homepage: https://github.com/viewv/minime
Description: native Linux fcitx5 input method running the WeType engine
 MinIME loads the official WeType (微信输入法) x86-64 engine natively with its
 own PE loader, without Wine. The package contains only MinIME's own programs;
 after installing, each user runs minime-fetch to download the official engine,
 dictionaries and models from Tencent's servers.
CTL
cat > "$STAGE/root/DEBIAN/postinst" <<'POST'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
  cat <<'MSG'
MinIME 已安装。以普通用户身份继续（不要用 sudo）：
  1. minime-fetch                                   从腾讯官方下载引擎和整句模型
  2. systemctl --user enable --now minime-engined    启动引擎服务
  3. 重启 fcitx5，在 fcitx5 配置里添加「MinIME」
MSG
fi
POST
chmod 755 "$STAGE/root/DEBIAN/postinst"

mkdir -p dist
OUT="dist/minime_${VERSION}-${REL}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$STAGE/root" "$OUT" >/dev/null
echo "$OUT"
