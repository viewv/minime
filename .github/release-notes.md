> [!WARNING]
> **测试版**：MinIME 还处在早期测试阶段，可能存在大量问题，不建议作为唯一的输入法使用。
> **Beta**: MinIME is at an early testing stage and may have many problems.

## 本版更新（0.1.2）

- **安装更简单**：`minime-fetch` 下载完后会自动启用并启动引擎服务，并把 MinIME 加进 fcitx5 的输入法列表；下载时显示进度。
  已经装过的用户升级后运行一次 `minime-fetch setup`。
- **设置程序**：「模型」页能看到下载的整句模型。
- **内存更少**：引擎服务的常驻内存降低约四分之一（DLL 按文件映射，整句模型权重上传显存后不再保留一份）。
- **网络服务**：用到的协议常量改为从用户下载的官方文件里读取。需要先运行过 `minime-fetch`，跨设备功能才能连接。
- **安装说明**：按实际安装流程重写，加了常见问题（找不到 MinIME、没有候选框、没有整句候选等）。

## 下载

| 发行版 | 文件 | 安装 |
|---|---|---|
| Arch Linux | `minime-git-*.pkg.tar.zst` | `sudo pacman -U minime-git-*.pkg.tar.zst` |
| Debian 13 | `minime_*debian13_amd64.deb` | `sudo apt install ./minime_*debian13_amd64.deb` |
| Ubuntu 26.10 | `minime_*ubuntu26.10_amd64.deb` | `sudo apt install ./minime_*ubuntu26.10_amd64.deb` |
| Fedora 44 | `minime-*.fc44.x86_64.rpm` | `sudo dnf install ./minime-*.fc44.x86_64.rpm` |

软件包只包含 MinIME 自己的程序，只支持 x86-64。`SHA256SUMS` 是各文件的校验值。

## 安装之后

1. **把 fcitx5 设为输入法**：KDE Plasma 在「系统设置 → 键盘 → 虚拟键盘」选 Fcitx 5；
   其他桌面：Debian / Ubuntu 运行 `im-config -n fcitx5`，Fedora 安装 `fcitx5-autostart`。然后注销重新登录。
2. **以普通用户身份运行**（不要加 sudo）：

   ```bash
   minime-fetch
   ```

   它从腾讯官方下载引擎、词库和整句模型（整句模型的服务器较慢，可能要几分钟），启动引擎服务，并把 MinIME 加进 fcitx5 的输入法列表。
3. 按 **Ctrl+Space** 切换到 MinIME 开始打字。

离线语音、跨设备、常见问题见 [README](https://github.com/viewv/minime#安装)。遇到问题欢迎提交 Issue。
