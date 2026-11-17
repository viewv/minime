#!/bin/bash
# deps.sh <arch|debian|ubuntu|fedora> — 安装构建依赖（CI 容器里以 root 运行；也可以在本机参考）
# Debian / Ubuntu / Fedora 自带的 Rust 可能太旧，用 rustup 装 stable。
set -euo pipefail
case "${1:?用法: deps.sh <arch|debian|ubuntu|fedora>}" in
  arch)
    pacman -Syu --noconfirm --needed base-devel git cmake rust shaderc vulkan-headers vulkan-icd-loader \
      fcitx5 jemalloc python p7zip curl pkgconf libxkbcommon libxkbcommon-x11 libxcb wayland fontconfig sudo
    ;;
  debian|ubuntu)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq build-essential cmake pkg-config git curl ca-certificates file dpkg-dev python3 \
      fcitx5 libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev glslc libvulkan-dev libjemalloc-dev \
      libxkbcommon-dev libxkbcommon-x11-dev libxcb1-dev libwayland-dev libfontconfig-dev
    ;;
  fedora)
    dnf install -y -q gcc gcc-c++ make cmake pkgconf git curl file rpm-build python3 fcitx5-devel \
      systemd-rpm-macros glslc vulkan-headers jemalloc-devel libxkbcommon-devel libxkbcommon-x11-devel \
      libxcb-devel wayland-devel fontconfig-devel
    ;;
  *) echo "不认识的发行版：$1" >&2; exit 1 ;;
esac
if [ "$1" != arch ] && ! command -v cargo >/dev/null; then
  curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
  # GitHub Actions：让后面的步骤能找到 cargo
  if [ -n "${GITHUB_PATH:-}" ]; then echo "$HOME/.cargo/bin" >> "$GITHUB_PATH"; fi
fi
exit 0
