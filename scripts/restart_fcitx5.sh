#!/bin/bash
# restart_fcitx5.sh — 在 KDE Plasma（Wayland）下从命令行重启 fcitx5（改了插件后用）
#
# 为什么不能直接重启：KWin 启动的是 /usr/lib/fcitx5-wayland-launcher，它把 KWin 给的输入法专用连接
# （WAYLAND_SOCKET）经 D-Bus 交给 fcitx5，只交一次；fcitx5 自己也报告不能原地重启（CanRestart = false）。
# fcitx5 一旦退出，只有 KWin 重新启动 launcher 才能恢复——系统设置里切换"虚拟键盘"做的就是这件事。
#
# 这里的办法：先让 fcitx5 退出，再强杀 launcher；KWin 看到输入法进程"崩溃"会自动重启它，
# launcher 再经 D-Bus 拉起新的 fcitx5（KWin 要过约 15 秒才重启）。KWin 对短时间内的连续崩溃有次数限制，别连着跑很多次。
# 不成功时（打不了字）：系统设置 → 键盘 → 虚拟键盘，切换一次。
#
# 只改了配置、没改插件代码时，用 fcitx5-remote -r 就够了（它不会重新加载插件）。
set -u

if [ "${XDG_SESSION_TYPE:-}" != wayland ] || ! pgrep -f fcitx5-wayland-launcher >/dev/null; then
    echo "没找到 fcitx5-wayland-launcher（不是 KDE Wayland 会话，或输入法不是由 KWin 启动的）；不处理" >&2
    exit 1
fi

old=$(pgrep -x fcitx5 | head -1)
fcitx5-remote -e 2>/dev/null || true
for _ in $(seq 20); do                       # 等旧进程退出（最多 2 秒）
    [ -n "$old" ] && kill -0 "$old" 2>/dev/null || break
    sleep 0.1
done
pkill -KILL -f fcitx5-wayland-launcher

# KWin 不是立即重启崩溃的输入法：实测（Plasma 6.8）约 15 秒后才重新拉起 launcher
echo "等 KWin 重新拉起输入法（实测约 15 秒）…"
start=$(date +%s)
for _ in $(seq 300); do                      # 最多 30 秒
    new=$(pgrep -x fcitx5 | grep -vx "${old:-0}" | head -1)
    if [ -n "$new" ]; then
        echo "fcitx5 已重启（进程 $new，等了 $(( $(date +%s) - start )) 秒）"
        exit 0
    fi
    sleep 0.1
done
echo "30 秒内没看到新的 fcitx5。请在系统设置 → 键盘 → 虚拟键盘里切换一次。" >&2
exit 1
