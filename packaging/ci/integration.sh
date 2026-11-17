#!/bin/bash
# integration.sh — 真机式集成测试：下载官方文件，启动引擎服务打字；可选联网测试（netd 连接、云输入）
#
#   packaging/ci/integration.sh [--network]
#
# 先 make engined netd（或 make）。官方文件：
#   - 默认用 minime-fetch 从腾讯官方下载（引擎约 218 MB、整句模型约 78 MB），不缓存；
#   - 已有时设 MINIME_ENGINE_DIR（含 libwxhld.dll 和 BuildIn/）和 MINIME_E2E_DIR（含 decgnr.bin），跳过下载。
# --network：用匿名设备身份连接微信输入法服务器（设备名 MinIME-CI），测 netd 握手（协议常量是否从
#   ime_net.dll 正确读出）和云输入。会在服务器上注册一个新的匿名设备，不关联任何手机。
# 所有数据放在临时目录，不碰本机的输入法配置和词库。
set -uo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
cd "$R"
NETWORK=0
[ "${1:-}" = --network ] && NETWORK=1

T=$(mktemp -d)
RT=$(mktemp -d /tmp/minime-it.XXXXXX)          # socket 路径不能太长
chmod 700 "$RT"
export XDG_DATA_HOME=$T/data XDG_CONFIG_HOME=$T/config XDG_RUNTIME_DIR=$RT
mkdir -p "$XDG_CONFIG_HOME/fcitx5/conf" "$XDG_DATA_HOME"
PIDS=()
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; rm -rf "$T" "$RT"; }
trap cleanup EXIT

PASS=0 FAIL=0
ok()   { echo "  ✓ $1"; PASS=$((PASS + 1)); }
bad()  { echo "  ✗ $1"; FAIL=$((FAIL + 1)); }

# ---------------------------------------------------------------- 官方文件
if [ -z "${MINIME_ENGINE_DIR:-}" ]; then
  echo "== 下载官方文件（minime-fetch）"
  python3 scripts/minime-fetch engine e2e --no-setup || { echo "下载失败"; exit 1; }
  export MINIME_ENGINE_DIR=$XDG_DATA_HOME/minime/official/win/3.0.1.35
fi
E2E_DIR=${MINIME_E2E_DIR:-$XDG_DATA_HOME/minime/official/mac/imeData}

start_engined() {
  [ -n "${ENGINED_PID:-}" ] && { kill "$ENGINED_PID" 2>/dev/null; wait "$ENGINED_PID" 2>/dev/null; }
  rm -f "$RT/minime/engine.sock"
  env MINIME_E2E_DIR="$E2E_DIR" MINIME_E2E_BACKEND=cpu "$@" build/native/engined > "$T/engined.log" 2>&1 &
  ENGINED_PID=$!
  PIDS+=("$ENGINED_PID")
  for _ in $(seq 100); do [ -S "$RT/minime/engine.sock" ] && return 0; sleep 0.1; done
  echo "引擎服务没有启动："; tail -20 "$T/engined.log"; return 1
}

# typed <说明> <期望出现的文字> <engined_test 脚本>
typed() {
  local out
  out=$(CANDS_COMMENT=1 CANDS_N=12 timeout 30 python3 scripts/engined_test.py "$3" --unix "$RT/minime/engine.sock" 2>&1)
  if grep -qF -- "$2" <<<"$out"; then ok "$1"; else bad "$1（期望「$2」）"; tail -4 <<<"$out" | sed 's/^/      /'; fi
}

# ---------------------------------------------------------------- 打字
echo "== 引擎服务"
start_engined MINIME_CLOUD=0 || exit 1
grep -q '就绪' "$T/engined.log" && ok "引擎装载并初始化（$(grep -o '引擎 [^ ]* 就绪（[0-9]* ms）' "$T/engined.log")）" || bad "引擎装载"
for _ in $(seq 50); do grep -q 'e2e 整句模型就绪' "$T/engined.log" && break; sleep 0.2; done
grep -q 'e2e 整句模型就绪' "$T/engined.log" && ok "整句模型加载" || bad "整句模型加载"

echo "== 打字"
typed "全拼 nihao → 你好"               "上屏 你好"  "nihao <SEL:0>"
typed "简拼 zgrm → 中国人民"            "中国人民"   "zgrm <WAIT:3000>"
typed "整句候选：他把手机忘在出租车上了" "他把手机忘在出租车上了" "tabashoujiwangzaichuzucheshangle <WAIT:8000>"
typed "小鹤双拼 womf → 我们"            "上屏 我们"  "<CFG:0,0,1,0,1,3> womf <SEL:0>"
typed "五笔 wqvbwq 顶字上屏 你好"       "上屏 你好"  "<CFG:0,0,1,0,2,0,1> wqvbwq"
typed "繁体 taiwan → 台灣"              "台灣"       "<CFG:0,1,0> taiwan <WAIT:500>"
typed "日期候选 riqi"                   "年"         "<CFG:0,0,1> riqi <WAIT:500>"

# ---------------------------------------------------------------- 联网
if [ "$NETWORK" = 1 ]; then
  echo "== 联网（匿名设备，设备名 MinIME-CI）"
  printf '[Network]\nEnabled=True\nCloudInput=True\nClipboardSync=False\nDeviceName=MinIME-CI\n' \
    > "$XDG_CONFIG_HOME/fcitx5/conf/minime.conf"
  netd/target/release/minime-netd > "$T/netd.log" 2>&1 &
  PIDS+=("$!")
  st=""
  for _ in $(seq 60); do
    st=$(netd/target/release/minime-netd status 2>&1 || true)
    grep -q 'connected: yes' <<<"$st" && break
    sleep 1
  done
  if grep -q 'connected: yes' <<<"$st"; then
    ok "netd 连接服务器（协议常量从 ime_net.dll 读出，握手成功）"
  else
    bad "netd 连接服务器"; sed 's/^/      /' <<<"$st"; tail -10 "$T/netd.log" | sed 's/^/      /'
  fi

  echo "== 云输入"
  start_engined MINIME_CLOUD=1 || exit 1
  sleep 3                                     # 引擎登录云端
  # 云端结果可能在等待上限内直接并进普通候选（这时没有 ☁ 标记），所以看两件事：
  # netd 确实转发并收到了云输入请求（cmd 4329）的回复；候选里有只有云端才给的结果
  typed "云候选 yuanshenqidong → 源申启冻（只有云端给）" "源申启冻" "yuanshenqidong <WAIT:3000>"
  if grep -q 'engine task 4329/' "$T/netd.log"; then ok "云输入请求经 netd 发出并收到回复"; else bad "云输入请求（netd 日志里没有 4329）"; fi
fi

echo
echo "通过 $PASS 项，失败 $FAIL 项"
[ "$FAIL" = 0 ]
