#!/bin/bash
# install_settings.sh — 编译 MinIME 设置程序（GPUI）并加到应用菜单（不需要 root）
#
#   ~/.local/bin/minime-settings                       命令行启动（--page devices 直接打开跨设备页）
#   ~/.local/share/applications/minime-settings.desktop  应用菜单「MinIME 设置」
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
BIN=$R/settings/target/release/minime-settings
DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

(cd "$R/settings" && cargo build --release)
mkdir -p "$HOME/.local/bin" "$DATA_HOME/applications"
ln -sf "$BIN" "$HOME/.local/bin/minime-settings"
rm -f "$DATA_HOME/applications/wetype-settings.desktop" "$HOME/.local/bin/wetype-settings"  # 改名前的旧版
python3 -I - "$DATA_HOME/applications/minime-settings.desktop" "$BIN" <<'PY'
import sys

dst, bin_path = sys.argv[1:3]
if any(ch.isspace() or ch in '"\\$`\'><~|&;()*?#' for ch in bin_path):
    exec_val = '"' + bin_path.replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$') + '"'
else:
    exec_val = bin_path

content = f"""[Desktop Entry]
Type=Application
Name=MinIME 设置
Comment=MinIME 输入法的输入方案、模糊拼音与跨设备设置
Exec={exec_val}
Icon=preferences-desktop-keyboard
Categories=Settings;Utility;
StartupWMClass=minime-settings
Keywords=minime;wetype;input;pinyin;clipboard;
"""
open(dst, "w", encoding="utf-8").write(content)
PY
update-desktop-database "$DATA_HOME/applications" 2>/dev/null || true
echo "已安装：~/.local/bin/minime-settings，应用菜单「MinIME 设置」"
