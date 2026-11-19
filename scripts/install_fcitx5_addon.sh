#!/bin/bash
# install_fcitx5_addon.sh — 把 fcitx5-minime 插件安装到系统目录（需要 sudo）
set -e
T=$(cd "$(dirname "$0")/.." && pwd)
SO="$T/fcitx5-minime/build/libminime.so"

if [ ! -f "$SO" ] || [ "$T/fcitx5-minime/src/minime.cpp" -nt "$SO" ] || [ "$T/fcitx5-minime/CMakeLists.txt" -nt "$SO" ]; then
  make -C "$T" addon
fi

LIBDIR=$(pkg-config --variable=libdir Fcitx5Core 2>/dev/null || true)
if [ -n "$LIBDIR" ]; then
  ADDON_DIR="$LIBDIR/fcitx5"
else
  ADDON_DIR=""
  for d in /usr/lib/fcitx5 /usr/lib/x86_64-linux-gnu/fcitx5 /usr/lib64/fcitx5; do
    if [ -e "$d/libkeyboard.so" ] || [ -e "$d/libunicode.so" ] || [ -e "$d/libclassicui.so" ]; then
      ADDON_DIR=$d
      break
    fi
  done
  if [ -z "$ADDON_DIR" ]; then
    for d in /usr/lib/fcitx5 /usr/lib/x86_64-linux-gnu/fcitx5 /usr/lib64/fcitx5; do
      [ -d "$d" ] && { ADDON_DIR=$d; break; }
    done
  fi
fi
ADDON_DIR=${ADDON_DIR:-/usr/lib/fcitx5}

sudo install -Dm644 "$T/fcitx5-minime/minime-addon.conf" /usr/share/fcitx5/addon/minime.conf
sudo install -Dm644 "$T/fcitx5-minime/minime-inputmethod.conf" /usr/share/fcitx5/inputmethod/minime.conf
sudo install -Dm755 "$T/fcitx5-minime/build/libminime.so" "$ADDON_DIR/libminime.so"

# 系统安装路径下 Library 用裸名
sudo sed -i 's|^Library=.*|Library=libminime|' /usr/share/fcitx5/addon/minime.conf

# 改名前（wetype）装的旧插件：删掉，免得 fcitx5 里同时出现两个
WETYPE_FILES=("$ADDON_DIR/libwetype.so" /usr/share/fcitx5/addon/wetype.conf /usr/share/fcitx5/inputmethod/wetype.conf)
[ "$ADDON_DIR" = "/usr/lib/fcitx5" ] || WETYPE_FILES+=(/usr/lib/fcitx5/libwetype.so)
for f in "${WETYPE_FILES[@]}"; do
  [ -e "$f" ] && sudo rm -f "$f" && echo "已删除旧插件文件 $f"
done

echo "已安装:"
echo "  /usr/share/fcitx5/addon/minime.conf"
echo "  /usr/share/fcitx5/inputmethod/minime.conf"
echo "  $ADDON_DIR/libminime.so"
echo
echo "重启 fcitx5（重新加载插件）: scripts/restart_fcitx5.sh（KDE Wayland）"
echo "启用输入法:  在 fcitx5 配置里添加「MinIME」（从旧版升级时要重新添加一次）"
