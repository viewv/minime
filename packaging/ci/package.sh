#!/bin/bash
# package.sh <arch|debian|ubuntu|fedora> — 在对应发行版上打包，产物放进 dist/（需要先跑 deps.sh）
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
cd "$R"
VERSION=$(cat VERSION)

# 校验 VERSION 与各组件元数据版本号一致
python3 -I - "$R" <<'PY' || exit 1
import os, re, sys

root = sys.argv[1]
ver = open(os.path.join(root, "VERSION"), encoding="utf-8").read().strip()

def check(rel, pattern):
    text = open(os.path.join(root, rel), encoding="utf-8").read()
    m = re.search(pattern, text, re.M)
    got = m.group(1) if m else None
    if got != ver:
        print(f"版本号不一致：{rel} 为 {got!r}，VERSION 为 {ver!r}", file=sys.stderr)
        return False
    return True

def check_cargo(rel):
    in_pkg = False
    got = None
    for line in open(os.path.join(root, rel), encoding="utf-8"):
        s = line.strip()
        if s.startswith("["):
            in_pkg = (s == "[package]")
        elif in_pkg:
            m = re.match(r'^version\s*=\s*"([^"]+)"', s)
            if m:
                got = m.group(1)
                break
    if got != ver:
        print(f"版本号不一致：{rel} 为 {got!r}，VERSION 为 {ver!r}", file=sys.stderr)
        return False
    return True

ok = all([
    check("packaging/rpm/minime.spec", r'^Version:\s*(\S+)'),
    check("fcitx5-minime/minime-addon.conf", r'^Version=(\S+)'),
    check("fcitx5-minime/CMakeLists.txt", r'^project\(fcitx5-minime\s+VERSION\s+(\S+)'),
    check_cargo("netd/Cargo.toml"),
    check_cargo("settings/Cargo.toml"),
    check_cargo("asrd/Cargo.toml"),
])
sys.exit(0 if ok else 1)
PY

mkdir -p dist
TMP=""
cleanup() { [ -n "$TMP" ] && rm -rf "$TMP"; }
trap cleanup EXIT
git config --global --add safe.directory '*'
case "${1:?用法: package.sh <arch|debian|ubuntu|fedora>}" in
  arch)
    # makepkg 不能以 root 运行；源码用当前检出的仓库
    id builder >/dev/null 2>&1 || useradd -m builder
    git config --system --add safe.directory '*'
    TMP=$(mktemp -d)
    cp packaging/arch/PKGBUILD packaging/arch/minime.install "$TMP/"
    python3 -I - "$TMP/PKGBUILD" "$R" <<'PY'
import sys
p, repo = sys.argv[1], sys.argv[2]
lines = open(p, encoding="utf-8").readlines()
out = [f'source=("$_name::git+file://{repo}")\n' if l.startswith("source=") else l for l in lines]
open(p, "w", encoding="utf-8").writelines(out)
PY
    chown -R builder "$TMP"
    (cd "$TMP" && sudo -u builder makepkg -f --noconfirm)
    cp "$TMP"/minime-git-[0-9r]*.pkg.tar.zst dist/
    ;;
  debian|ubuntu)
    . /etc/os-release
    TMP=$(mktemp -d)
    git archive HEAD | tar -x -C "$TMP"
    make -C "$TMP" -j"$(nproc)"
    (cd "$TMP" && DEB_RELEASE="1~${ID}${VERSION_ID}" "$TMP/packaging/deb/build-deb.sh")
    cp "$TMP"/dist/*.deb dist/
    ;;
  fedora)
    TMP=$(mktemp -d)
    mkdir -p "$TMP/SOURCES"
    git archive --prefix="minime-$VERSION/" -o "$TMP/SOURCES/minime-$VERSION.tar.gz" HEAD
    rpmbuild -bb --define "_topdir $TMP" packaging/rpm/minime.spec
    cp "$TMP"/RPMS/x86_64/minime-*.rpm dist/
    ;;
  *) echo "不认识的发行版：$1" >&2; exit 1 ;;
esac
ls -l dist/
