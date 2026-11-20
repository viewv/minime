#!/bin/sh
# build-pkg.sh — 在 FreeBSD amd64 上把已经 gmake 好的 MinIME 打成 .pkg 和 .tar.xz（只含本项目的程序）
#
#   gmake && sh packaging/freebsd/build-pkg.sh
#   产物：
#     dist/minime-<版本>_freebsd<主版本>_amd64.pkg
#     dist/minime-<版本>_freebsd<主版本>_amd64.tar.xz
set -eu

R=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$R"

ARCH=$(uname -m)
if [ "$ARCH" != "amd64" ]; then
  echo "MinIME 只支持 amd64（引擎是 x86-64 机器码），当前架构：$ARCH" >&2
  exit 1
fi

VERSION=$(tr -d ' \t\r\n' < VERSION)
OSMAJOR=$(freebsd-version | cut -d. -f1)
ABI=$(pkg config ABI)

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

gmake install DESTDIR="$STAGE/root" PREFIX=/usr/local >/dev/null

# 生成相对 /usr/local 的 plist
(cd "$STAGE/root/usr/local" && find . -type f | sed 's|^\./||' | sort) > "$STAGE/plist"
if [ ! -s "$STAGE/plist" ]; then
  echo "staging 目录为空，请先运行 gmake" >&2
  exit 1
fi

# 用 python3 生成合法的 UCL/JSON +MANIFEST（含精确的 flatsize 与已安装核心运行时依赖）
python3 - "$STAGE/root/usr/local" "$STAGE/plist" "$STAGE/+MANIFEST" "$VERSION" "$ABI" <<'PY'
import json, os, subprocess, sys

root, plist_path, out_path, version, abi = sys.argv[1:6]

flatsize = 0
with open(plist_path, encoding="utf-8") as f:
    for line in f:
        rel = line.strip()
        if rel:
            flatsize += os.lstat(os.path.join(root, rel)).st_size

deps = {}
for pkg_name in ("fcitx5", "python3", "7-zip", "curl"):
    r = subprocess.run(
        ["pkg", "query", "%n\t%v\t%o", pkg_name],
        capture_output=True, text=True, check=False,
    )
    if r.returncode == 0 and r.stdout.strip():
        parts = r.stdout.strip().splitlines()[0].split("\t")
        if len(parts) == 3:
            deps[parts[0]] = {"version": parts[1], "origin": parts[2]}

manifest = {
    "name": "minime",
    "version": version,
    "origin": "chinese/minime",
    "comment": "Native fcitx5 input method running the WeType engine (experimental)",
    "desc": (
        "MinIME loads the official WeType x86-64 engine natively with its own PE loader, "
        "without Wine. This experimental FreeBSD package contains only MinIME's own programs "
        "(without minime-settings); after installing, run minime-fetch --no-setup as a normal "
        "user to download the official engine, dictionaries and models."
    ),
    "maintainer": "MinIME contributors",
    "www": "https://github.com/viewv/minime",
    "prefix": "/usr/local",
    "abi": abi,
    "licenselogic": "single",
    "licenses": ["MPL20"],
    "categories": ["chinese", "misc"],
    "flatsize": flatsize,
    "deps": deps,
    "messages": [
        {
            "type": "install",
            "message": (
                "MinIME (experimental FreeBSD build) has been installed.\n\n"
                "Recommended extra packages:\n"
                "  sudo pkg install fcitx5-configtool fcitx5-gtk3 noto-sans-sc noto-emoji\n\n"
                "Next steps (run as your normal user, without sudo):\n"
                "  1. Download official engine and model:\n"
                "       minime-fetch --no-setup\n"
                "  2. Set IM env vars in ~/.xprofile (GTK_IM_MODULE=fcitx, QT_IM_MODULE=fcitx, XMODIFIERS=@im=fcitx)\n"
                "     and autostart fcitx5 and minime-engined (see docs/bsd.md).\n"
                "  3. Run fcitx5-configtool to add MinIME and configure options.\n"
            ),
        }
    ],
}

with open(out_path, "w", encoding="utf-8") as f:
    json.dump(manifest, f, indent=2, ensure_ascii=False)
    f.write("\n")
PY

mkdir -p "$STAGE/out" dist
pkg create -M "$STAGE/+MANIFEST" -r "$STAGE/root" -p "$STAGE/plist" -o "$STAGE/out"

PKG_RAW=$(ls "$STAGE"/out/minime-*.pkg | head -n1)
PKG_OUT="dist/minime-${VERSION}_freebsd${OSMAJOR}_amd64.pkg"
TAR_OUT="dist/minime-${VERSION}_freebsd${OSMAJOR}_amd64.tar.xz"

cp "$PKG_RAW" "$PKG_OUT"
tar -C "$STAGE/root" -cJf "$TAR_OUT" usr/local

# 校验生成的 .pkg 元数据与文件清单
pkg info -F "$PKG_OUT"
pkg info -lF "$PKG_OUT"
ls -lh "$PKG_OUT" "$TAR_OUT"
