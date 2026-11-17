#!/bin/bash
# install_asrd.sh — 编译 minime-asrd（离线语音）并装成 systemd 用户服务（不需要 root）
#
# 识别完全在本机：录音（pw-record）→ 本地模型 → 上屏，不联网。
# 模型是官方语音包里的 .xnet（约 130 MB），不随仓库分发；本脚本会调用
# `minime-asrd fetch-model` 从官方 CDN 下载、校验 md5、解包到 ~/.local/share/minime/asr/model。
#
# 按需启动：装的是 minime-asrd.socket（systemd 持有监听 socket），第一次按语音键才启动服务，
# 空闲 10 分钟自动退出，平时不占内存。
#
# 装完后：
#   systemctl --user enable --now minime-asrd.socket
#   minime-asrd say          # 终端里说一句试试（回车结束）
#   在任意输入框按 Ctrl+Alt+空格（设置程序里可改）开始说话
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
BIN=$R/asrd/target/release/minime-asrd
UNIT=$HOME/.config/systemd/user/minime-asrd.service
SOCK=$HOME/.config/systemd/user/minime-asrd.socket

(cd "$R/asrd" && cargo build --release)
"$BIN" fetch-model
mkdir -p "$(dirname "$UNIT")"
cat > "$UNIT" <<EOF
[Unit]
Description=MinIME 离线语音（本地识别，不联网）
Documentation=file://$R/docs/voice.md
After=graphical-session.target pipewire.service

[Service]
ExecStart=$BIN
Restart=on-failure
RestartSec=5
EOF
cat > "$SOCK" <<EOF
[Unit]
Description=MinIME 离线语音（按需启动）
Documentation=file://$R/docs/voice.md

[Socket]
ListenStream=%t/minime-asrd.sock
SocketMode=0600

[Install]
WantedBy=sockets.target
EOF
# 早先的版本把服务设成了登录自启：改成按需启动
systemctl --user disable minime-asrd.service 2>/dev/null || true
# 改名前（wetype）的服务：停掉、取消自启、删掉单元文件
for u in wetype-asrd.socket wetype-asrd.service; do
  if [ -e "$HOME/.config/systemd/user/$u" ]; then
    systemctl --user disable --now "$u" 2>/dev/null || true
    rm -f "$HOME/.config/systemd/user/$u"
    echo "已移除旧服务 $u"
  fi
done
systemctl --user daemon-reload
mkdir -p "$HOME/.local/bin"
ln -sf "$BIN" "$HOME/.local/bin/minime-asrd"
echo "已写入 $UNIT 和 $SOCK，命令行链接 ~/.local/bin/minime-asrd"
echo "启用（按需启动）: systemctl --user enable --now minime-asrd.socket"
