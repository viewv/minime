> [!WARNING]
> **测试版**：MinIME 还处在早期测试阶段，可能存在大量问题，不建议作为唯一的输入法使用。
> **Beta**: MinIME is at an early testing stage and may have many problems.

## 本版更新（0.1.1）

- **隐私输入框**：应用标为敏感数据的输入框里，引擎不再学习选词（之前只是不学整句、不发周围文字、不用云输入）。之前学到的词照常出现。

## 下载

| 发行版 | 文件 | 安装 |
|---|---|---|
| Arch Linux | `minime-git-*.pkg.tar.zst` | `sudo pacman -U minime-git-*.pkg.tar.zst` |
| Debian 13 | `minime_*~debian13_amd64.deb` | `sudo apt install ./minime_*~debian13_amd64.deb` |
| Ubuntu 26.10 | `minime_*~ubuntu26.10_amd64.deb` | `sudo apt install ./minime_*~ubuntu26.10_amd64.deb` |
| Fedora 44 | `minime-*.fc44.x86_64.rpm` | `sudo dnf install ./minime-*.fc44.x86_64.rpm` |

软件包只包含 MinIME 自己的程序，只支持 x86-64。`SHA256SUMS` 是各文件的校验值。

## 安装之后

以普通用户身份运行（不要加 sudo）：

```bash
minime-fetch                                       # 从腾讯官方下载引擎、词库和整句模型
systemctl --user enable --now minime-engined       # 启动引擎服务
```

然后重启 fcitx5，在 fcitx5 配置里添加「MinIME」。离线语音、跨设备等可选功能和 KDE Plasma（Wayland）上的注意事项见
[README](https://github.com/viewv/minime#安装)。

遇到问题欢迎提交 Issue。
