# 开发

## 代码结构

| 目录 | 内容 |
|---|---|
| `loader/` | PE 装载器和 Win32 兼容层（C） |
| `src/` | 引擎服务 `engined`、引擎适配层、网络兼容层、整句模型推理（`e2e*.c`、`shaders/`）和调试工具 |
| `fcitx5-minime/` | fcitx5 插件（C++） |
| `netd/` | 网络服务（Rust），协议实现在 `src/lib.rs` 下的各模块，守护进程在 `src/main.rs` |
| `asrd/` | 离线语音服务（Rust） |
| `settings/` | 设置程序（Rust，GPUI） |
| `scripts/` | `minime-fetch`、开发用的安装脚本、测试和分析工具 |
| `packaging/` | systemd 单元、桌面文件、Arch / deb / rpm 打包文件 |
| `tests/` | 基准输入和评测集 |
| `docs/` | 本文档 |

## 构建

```bash
make                     # 全部组件
make engined             # 只构建引擎服务（build/native/engined）
make addon               # fcitx5 插件（fcitx5-minime/build/libminime.so）
make netd settings asrd  # Rust 组件
```

依赖见 [packaging](packaging.md)。

## 开发时直接运行

不打包时，可以把仓库里的构建产物装成用户服务：

```bash
python3 scripts/minime-fetch                          # 下载官方文件（只需一次）
scripts/install_engined.sh                            # 引擎服务（单元文件和软件包的一样，只换程序路径）
scripts/install_fcitx5_addon.sh                       # fcitx5 插件（需要 sudo）
scripts/install_netd.sh && scripts/install_asrd.sh && scripts/install_settings.sh
```

前台运行引擎服务，方便看日志：

```bash
systemctl --user stop minime-engined
MINIME_DEBUG=1 build/native/engined                   # 不带参数时使用 minime-fetch 下载的引擎
```

改了插件之后要真正重启 fcitx5 才会重新加载。在 KDE Plasma（Wayland）上用 `scripts/restart_fcitx5.sh`，
不要手动启动 fcitx5。

## 测试

```bash
python3 scripts/engined_test.py "nihao <SEL:0>"                 # 不经过 fcitx5，直接驱动引擎服务
python3 scripts/engined_test.py "<CFG:0,0,1,0,2,0,1> wqvbwq"    # 带配置（这里是五笔）
python3 scripts/engined_test.py --bench tests/baseline_3.0.1.35.tsv
(cd netd && cargo test)                                         # 协议实现与参考向量对照
build/native/e2e_test                                           # 整句模型 CPU / Vulkan 数值对照
```

`engined_test.py` 的脚本语法（`<BS>` 退格、`<ESC>`、`<SEL:n>` 选词、`<DEL:n>` 删词、`<CTX:文字>` 上下文、
`<CFG:…>` 配置等）见文件开头的说明。

要在不影响正在使用的输入法的情况下测试，可以另起一个引擎实例：把 `XDG_RUNTIME_DIR`、`XDG_DATA_HOME`、
`XDG_CONFIG_HOME` 都指向临时目录。

## 调试开关

| 环境变量 | 作用 |
|---|---|
| `MINIME_DEBUG=1` | 打印按键、事件和学习过程 |
| `MINIME_ENGINE_STDERR=1` | 输出引擎自己的日志 |
| `MINIME_ENGINE_DIR` | 引擎目录（缺省为 `minime-fetch` 下载的位置） |
| `MINIME_E2E=0` | 关闭整句模型 |
| `MINIME_E2E_DIR` | 整句模型目录 |
| `MINIME_E2E_BACKEND=cpu\|vulkan` | 整句模型的计算后端 |
| `MINIME_E2E_LEARN=0` | 选中整句后不让引擎学习 |
| `MINIME_MAX_CANDS` | 每次按键先取的候选数（默认 50） |
| `MINIME_CLOUD=0\|1` | 强制关闭或打开云输入（测试用） |
| `MINIME_SESSCFG="偏移=值,…"` | 逐字节修改引擎会话配置（试验引擎开关用） |
| `MINIME_CELL_DICTS` / `MINIME_EXTRA_DICTS` | 额外加载专题词库或其他词库（实验用） |
| `MINIME_TRACE_FILES=路径片段` | 打印引擎对匹配路径的文件操作 |
| `MINIME_LOADER_VERBOSE=1` | 打印装载器的详细过程 |
| `MINIME_ASRD_IDLE_SECS` | 语音服务空闲多久后退出（默认 600 秒） |

插件的调试日志：创建 `$XDG_RUNTIME_DIR/minime-plugin-debug` 文件后，插件把周围文字和自动加空格的判断写进
fcitx5 的日志（`journalctl --user`）。

## 提交问题

提交 Issue 时请附上：

- 发行版和版本、桌面环境（KDE / GNOME 等，Wayland 还是 X11）、fcitx5 版本；
- `minime-fetch status` 的输出；
- 相关日志：`journalctl --user -u minime-engined -b`（引擎）、`journalctl --user -u minime-netd -b`（网络）。

日志里可能包含你输入的内容，提交前请检查。
