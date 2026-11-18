#!/bin/bash
# install_engined.sh — 开发时把仓库里构建的 engined 装成 systemd 用户服务（不需要 root）
#
# 用法: scripts/install_engined.sh              和软件包一样：不带参数，用 minime-fetch 下载的官方文件
#                                                （~/.local/share/minime/official/），只是程序用仓库里的构建
#       scripts/install_engined.sh <引擎目录>    实验用：指定别的引擎目录（例如 vendor/win/3.0.1.35），
#                                                同时用 E2E_DIR 或 <引擎目录>/BuildIn、vendor/mac/imeData 里的整句模型
#
# 单元文件由 packaging/systemd/minime-engined.service 生成（只换 ExecStart 的路径），
# 这样本机看到的就是用户装完看到的。官方文件没下载时先运行 scripts/minime-fetch（或在设置程序里下载）。
#
# 装完后：
#   systemctl --user enable --now minime-engined
#   journalctl --user -u minime-engined -f
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
BIN=$R/build/native/engined
UNIT=$HOME/.config/systemd/user/minime-engined.service

[ -x "$BIN" ] || make -C "$R/src" native
# 改名前（wetype）的服务先全停掉，免得它们在迁移后又往旧目录里写
for u in wetype-engined.service wetype-netd.service wetype-asrd.socket wetype-asrd.service; do
  systemctl --user stop "$u" 2>/dev/null || true
done
mkdir -p "$(dirname "$UNIT")"

ARGS=""
E2E_ENV=""
if [ -n "${1:-}" ]; then
  ENGINE=$(realpath "$1")
  [ -f "$ENGINE/libwxhld.dll" ] || { echo "找不到 $ENGINE/libwxhld.dll" >&2; exit 1; }
  ARGS=" $ENGINE ${XDG_DATA_HOME:-$HOME/.local/share}/minime/userdata"
  E2E_DIR=${E2E_DIR:-}
  if [ -z "$E2E_DIR" ]; then
    for d in "$ENGINE/BuildIn" "$R/vendor/mac/imeData"; do
      [ -f "$d/decgnr.bin" ] && [ -f "$d/mpd_e2e.bin" ] && { E2E_DIR=$(realpath "$d"); break; }
    done
  fi
  [ -n "$E2E_DIR" ] && E2E_ENV="Environment=MINIME_E2E_DIR=$E2E_DIR"
  echo "实验配置：引擎 $ENGINE${E2E_DIR:+，整句模型 $E2E_DIR}"
else
  OFF=${XDG_DATA_HOME:-$HOME/.local/share}/minime/official
  [ -f "$OFF/win/3.0.1.35/libwxhld.dll" ] || echo "提示：还没有下载官方引擎（$OFF），先运行 scripts/minime-fetch 或在设置程序里下载"
fi

# 单元文件直接用软件包里的那份，只换程序路径（实验配置再加参数和整句模型目录）
python3 -c '
import sys
s = open(sys.argv[1]).read()
s = s.replace("ExecStart=/usr/bin/minime-engined", "ExecStart=" + sys.argv[2] + (("\n" + sys.argv[3]) if sys.argv[3] else ""))
open(sys.argv[4], "w").write(s)
' "$R/packaging/systemd/minime-engined.service" "$BIN$ARGS" "$E2E_ENV" "$UNIT"

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
