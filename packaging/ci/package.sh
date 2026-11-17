#!/bin/bash
# package.sh <arch|debian|ubuntu|fedora> — 在对应发行版上打包，产物放进 dist/（需要先跑 deps.sh）
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
cd "$R"
VERSION=$(cat VERSION)
mkdir -p dist
git config --global --add safe.directory '*'
case "${1:?用法: package.sh <arch|debian|ubuntu|fedora>}" in
  arch)
    # makepkg 不能以 root 运行；源码用当前检出的仓库
    id builder >/dev/null 2>&1 || useradd -m builder
    git config --system --add safe.directory '*'
    W=$(mktemp -d) && cp packaging/arch/PKGBUILD packaging/arch/minime.install "$W/"
    sed -i "s#^source=.*#source=(\"\$_name::git+file://$R\")#" "$W/PKGBUILD"
    chown -R builder "$W"
    (cd "$W" && sudo -u builder makepkg -f --noconfirm)
    cp "$W"/minime-git-[0-9r]*.pkg.tar.zst dist/
    ;;
  debian|ubuntu)
    . /etc/os-release
    make
    DEB_RELEASE="1~${ID}${VERSION_ID}" packaging/deb/build-deb.sh
    ;;
  fedora)
    T=$(mktemp -d)
    mkdir -p "$T/SOURCES"
    git archive --prefix="minime-$VERSION/" -o "$T/SOURCES/minime-$VERSION.tar.gz" HEAD
    rpmbuild -bb --define "_topdir $T" packaging/rpm/minime.spec
    cp "$T"/RPMS/x86_64/minime-*.rpm dist/
    ;;
  *) echo "不认识的发行版：$1" >&2; exit 1 ;;
esac
ls -l dist/
