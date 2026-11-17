#!/bin/bash
# install_netd.sh — 编译 minime-netd 并装成 systemd 用户服务（不需要 root）
#
# 服务默认不联网：要在 fcitx5 配置面板「联网功能」里打开（或在
# ~/.config/fcitx5/conf/minime.conf 的 [Network] 段写 Enabled=True）才会连服务器。
# 身份存在 ~/.local/share/minime/net/identity.json（0600）。
#
# 装完后：
#   systemctl --user enable --now minime-netd
#   journalctl --user -u minime-netd -f
#   netd/target/release/minime-netd pair        # 出码，在手机上输入
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
BIN=$R/netd/target/release/minime-netd
UNIT=$HOME/.config/systemd/user/minime-netd.service

(cd "$R/netd" && cargo build --release)
mkdir -p "$(dirname "$UNIT")"
cat > "$UNIT" <<EOF
[Unit]
Description=MinIME 网络服务（跨设备剪贴板，默认关闭）
Documentation=file://$R/docs/cross-device.md
After=graphical-session.target

[Service]
ExecStart=$BIN
Restart=on-failure
RestartSec=5

[Install]
WantedBy=graphical-session.target
EOF
# 改名前（wetype）的服务：停掉、取消自启、删掉单元文件
for u in wetype-netd.service; do
  if [ -e "$HOME/.config/systemd/user/$u" ]; then
    systemctl --user disable --now "$u" 2>/dev/null || true
    rm -f "$HOME/.config/systemd/user/$u"
    echo "已移除旧服务 $u"
  fi
done
systemctl --user daemon-reload
mkdir -p "$HOME/.local/bin"
ln -sf "$BIN" "$HOME/.local/bin/minime-netd"   # 命令行：minime-netd status / pair
echo "已写入 $UNIT，命令行链接 ~/.local/bin/minime-netd"
echo "启动并设为登录自启: systemctl --user enable --now minime-netd"
