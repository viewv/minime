# 架构

MinIME 由几个独立的进程组成。打字只经过 fcitx5 插件和引擎服务；网络、语音、设置各自是单独的程序，
其中任何一个出问题都不会影响打字。

```
应用程序 ⇄ fcitx5 ⇄ fcitx5-minime（插件）
                        │  Unix socket
                        ├──────────────► minime-engined ── 官方引擎（PE 装载）、整句模型
                        │                     │  Unix socket（引擎的网络请求）
                        │                     ▼
                        │                minime-netd ── WSS / HTTPS ──► 微信输入法服务器
                        │                     ▲          （只在开启联网后）
                        │  Unix socket        │ 控制 socket
                        └──────────────► minime-asrd    MinIME 设置
```

## 组件

| 组件 | 语言 | 职责 |
|---|---|---|
| `fcitx5-minime` | C++ | fcitx5 插件。收按键、显示预编辑和候选、选词上屏；标点、中英文切换、自动加空格、长按说话等前端逻辑也在这里。拼音和词库的处理都交给引擎服务。 |
| `minime-engined` | C | 引擎服务，systemd 用户服务，常驻。用自己的 PE 装载器在 Linux 进程里运行官方引擎（见 [engine](engine.md)），负责会话、候选、整句模型推理（见 [whole-sentence](whole-sentence.md)），并把引擎发出的网络请求转交给 netd。 |
| `minime-netd` | Rust | 网络服务。从协议层重新实现了官方网络栈，提供跨设备功能（见 [cross-device](cross-device.md)）。不开联网时不连接任何服务器。 |
| `minime-asrd` | Rust | 离线语音识别服务，由 systemd socket 按需启动，空闲后自动退出（见 [voice](voice.md)）。 |
| `minime-settings` | Rust（GPUI） | 图形设置程序。 |
| `minime-fetch` | Python | 从腾讯官方地址下载引擎、词库和模型（见 [packaging](packaging.md)）。 |

所有服务都以当前用户身份运行，只监听本机 Unix socket（目录权限 0700、socket 0600）；
引擎服务和语音服务还会核对连接方是同一个用户。

## 一次按键

以输入 `n` 为例：

1. fcitx5 把按键交给插件，插件向 engined 发一条「输入字母」命令，不等回复，立即处理下一个按键。
2. engined 调用引擎的输入接口。
3. 引擎在自己的线程里回调预编辑和候选；engined 把它们编码后写回 socket。
4. 插件收到后刷新预编辑和候选框。
5. 如果开着整句候选，engined 同时把当前拼音交给后台的整句模型；结果出来后插进候选列表再发一次。

一次按键的往返（发出到收到候选）平均约 3.3 ms。引擎只在 engined 的主线程里调用，
引擎的回调只做编码和写 socket。

插件和 engined 之间是一条连接一个引擎会话。消息格式是 4 字节长度加 protobuf 负载，
沿用官方客户端和引擎服务之间的消息定义，另加了几条 MinIME 自己的命令（配置、取更多候选、删词等）。

## 配置

所有组件共用一份配置文件 `~/.config/fcitx5/conf/minime.conf`：

- fcitx5 自带的配置界面和「MinIME 设置」都读写它；
- 插件读到变化后把相关选项发给 engined（例如输入方案、模糊音），需要时 engined 重建引擎会话；
- netd 每两秒检查一次 `[Network]` 一节，联网开关改动即时生效。

## 文件和目录

| 位置 | 内容 |
|---|---|
| `~/.config/fcitx5/conf/minime.conf` | 配置 |
| `~/.local/share/minime/official/` | `minime-fetch` 下载的官方文件：`win/`（引擎、词库）、`mac/`（整句模型） |
| `~/.local/share/minime/userdata/` | 引擎的工作目录，用户词库在其中的 `DictDir2/` |
| `~/.local/share/minime/dicts/` | 词库配置（启用哪些内置词库、导入的专题词库） |
| `~/.local/share/minime/asr/model/` | 语音模型 |
| `~/.local/share/minime/net/` | 网络服务的设备身份、常用语、换机助手的备份（目录 0700，文件 0600） |
| `$XDG_RUNTIME_DIR/minime/engine.sock` | engined 的 socket（插件连接） |
| `$XDG_RUNTIME_DIR/minime/ctl.sock` | engined 的控制 socket（netd 用来导入导出用户词库） |
| `$XDG_RUNTIME_DIR/minime-netd.sock`、`minime-netd-net.sock` | netd 的控制 socket、引擎网络请求的 socket |
| `$XDG_RUNTIME_DIR/minime-asrd.sock` | 语音服务的 socket（由 systemd 持有） |

## 服务管理

```bash
systemctl --user status minime-engined          # 引擎
journalctl --user -u minime-engined -f          # 引擎日志
systemctl --user restart minime-engined         # 重启引擎（正在输入的内容会丢）
```

「MinIME 设置」的「关于」页能看到各个服务的状态，并提供重启按钮。
