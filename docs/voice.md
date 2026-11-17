# 离线语音输入

按住一个键说话，松开后识别结果上屏。识别完全在本机进行，录音不会离开这台电脑。

## 安装

```bash
minime-fetch voice                                   # 下载官方语音模型（约 103 MB），校验后解包
systemctl --user enable --now minime-asrd.socket     # 按需启动的语音服务
```

模型是微信输入法按需下载的离线语音资源包，从腾讯官方 CDN 获取，解包到 `~/.local/share/minime/asr/model/`。
录音需要 PipeWire 的 `pw-record`，没有时使用 PulseAudio 的 `parec`。

## 使用

- **长按右 Alt**（默认）：按住超过 0.4 秒开始说话，说话时预编辑里实时显示识别中的文字，松开后定稿上屏。
  短按 Alt 和 Alt 组合键照常交给应用，不受影响。可以改成左 Alt 或关闭（「MinIME 设置 → 输入 → 语音输入」）。
  默认不用左 Alt，因为按住 Alt 切换窗口时容易误触发。
- **快捷键**（默认 Ctrl+Alt+空格）：按住说话；或者按一下开始，再按一下或按回车结束。
- **Esc** 取消这次说话。切换输入框或切走输入法时也会取消。
- 密码框里不响应语音键。

## 工作方式

```
fcitx5 插件 ──start / stop──▶ minime-asrd ──录音 16 kHz 单声道──▶ 识别
     ▲                              │  说话时每 300 ms 给一次中间结果
     └───── 中间结果 / 最终结果 ◀────┘  结束后整段重新解码，给出最终结果
```

- `minime-asrd` 由 systemd 的 socket 按需启动：第一次使用语音时才启动（到麦克风就绪约 20–40 ms），
  空闲 10 分钟后自动退出。不用语音时没有任何进程在运行。
- 识别使用开源实现 [wechat-asr-gpu](https://github.com/Enter-tainer/wechat-asr-gpu)（MIT 许可）在 CPU 上推理。
  模型是卷积前端加 40 层 Transformer、CTC 输出，int8 量化。
- 资源：识别时用 4 个线程，3 秒的语音约 60 ms 识别完；运行中常驻内存约 20 MB，识别时峰值约 150 MB
  （大部分是模型文件的映射，可被系统回收）。
- 单次说话最长 120 秒。

## 调试

```bash
minime-asrd say        # 在终端里说一句话，回车结束，打印识别结果
minime-asrd file a.wav # 识别一个音频文件
journalctl --user -u minime-asrd
```

## 以后

识别目前只用 CPU，已经远快于实时。计划把计算迁到 GPU（Vulkan）或 NPU 上，进一步减少 CPU 占用。
