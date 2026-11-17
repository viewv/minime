#!/bin/bash
# install_engined.sh — 把原生服务装成 systemd 用户服务（不需要 root）
#
# 用法: scripts/install_engined.sh [引擎安装目录]
#   引擎安装目录缺省为本仓库的 vendor/win/3.0.1.35（含 libwxhld.dll、XNet.dll、
#   tensorflowlite_c.dll 和 BuildIn/）。
# 用户数据（用户词库等）放在 ~/.local/share/minime/userdata，和引擎版本无关。
# e2e 整句模型：引擎目录的 BuildIn/ 或本仓库 vendor/mac/imeData/ 里有 decgnr.bin 和 mpd_e2e.bin 时启用
# （取模型见 docs/whole-sentence.md "模型从哪来"），也可以用环境变量 E2E_DIR 指定。
#
# 装完后：
#   systemctl --user enable --now minime-engined
#   journalctl --user -u minime-engined -f
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
ENGINE=$(realpath "${1:-$R/vendor/win/3.0.1.35}")
BIN=$R/build/native/engined
DATA=$HOME/.local/share/minime/userdata
UNIT=$HOME/.config/systemd/user/minime-engined.service

[ -x "$BIN" ] || make -C "$R/src" native
[ -f "$ENGINE/libwxhld.dll" ] || { echo "找不到 $ENGINE/libwxhld.dll" >&2; exit 1; }
# 改名前（wetype）的服务先全停掉，免得它们在迁移后又往旧目录里写
for u in wetype-engined.service wetype-netd.service wetype-asrd.socket wetype-asrd.service; do
  systemctl --user stop "$u" 2>/dev/null || true
done
# 旧数据目录还在时不要先建新目录：engined 第一次启动时会把 ~/.local/share/wetype 整个迁过来
if [ -d "$HOME/.local/share/wetype" ] && [ ! -e "$HOME/.local/share/minime" ]; then
  echo "发现旧数据目录 ~/.local/share/wetype，引擎第一次启动时会迁到 ~/.local/share/minime"
  mkdir -p "$(dirname "$UNIT")"
else
  mkdir -p "$DATA" "$(dirname "$UNIT")"
fi

E2E_DIR=${E2E_DIR:-}
if [ -z "$E2E_DIR" ]; then
  for d in "$ENGINE/BuildIn" "$R/vendor/mac/imeData"; do
    [ -f "$d/decgnr.bin" ] && [ -f "$d/mpd_e2e.bin" ] && { E2E_DIR=$(realpath "$d"); break; }
  done
fi
E2E_ENV=""
if [ -n "$E2E_DIR" ]; then
  E2E_ENV="Environment=MINIME_E2E_DIR=$E2E_DIR"
  echo "e2e 整句模型: $E2E_DIR"
else
  echo "没找到 e2e 整句模型（decgnr.bin + mpd_e2e.bin），不出整句候选"
fi

cat > "$UNIT" <<EOF
[Unit]
Description=MinIME 输入法引擎（原生装载）
Documentation=file://$R/docs/engine.md

[Service]
ExecStart=$BIN $ENGINE $DATA
$E2E_ENV
Restart=on-failure
RestartSec=1

[Install]
WantedBy=default.target
EOF

# 改名前（wetype）的服务：停掉、取消自启、删掉单元文件
for u in wetype-engined.service; do
  if [ -e "$HOME/.config/systemd/user/$u" ]; then
    systemctl --user disable --now "$u" 2>/dev/null || true
    rm -f "$HOME/.config/systemd/user/$u"
    echo "已移除旧服务 $u"
  fi
done
systemctl --user daemon-reload
echo "已写入 $UNIT"
echo "启动并设为开机自启: systemctl --user enable --now minime-engined"
echo "已在运行时重启生效:   systemctl --user restart minime-engined"
