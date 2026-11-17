#!/bin/bash
# install_settings.sh — 编译 MinIME 设置程序（GPUI）并加到应用菜单（不需要 root）
#
#   ~/.local/bin/minime-settings                       命令行启动（--page devices 直接打开跨设备页）
#   ~/.local/share/applications/minime-settings.desktop  应用菜单「MinIME 设置」
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
BIN=$R/settings/target/release/minime-settings

(cd "$R/settings" && cargo build --release)
mkdir -p "$HOME/.local/bin" "$HOME/.local/share/applications"
ln -sf "$BIN" "$HOME/.local/bin/minime-settings"
rm -f "$HOME/.local/share/applications/wetype-settings.desktop" "$HOME/.local/bin/wetype-settings"  # 改名前的旧版
cat > "$HOME/.local/share/applications/minime-settings.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=MinIME 设置
Comment=MinIME 输入法的输入方案、模糊拼音与跨设备设置
Exec=$BIN
Icon=preferences-desktop-keyboard
Categories=Settings;Utility;
StartupWMClass=minime-settings
Keywords=minime;wetype;input;pinyin;clipboard;
EOF
update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
echo "已安装：~/.local/bin/minime-settings，应用菜单「MinIME 设置」"
