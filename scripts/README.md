# Termux glibc (aarch64) 使用说明

面向 **Termux glibc + FEX + Wine** 场景。三个脚本：

| 脚本 | 作用 |
|---|---|
| `build-termux-glibc.sh` | 一键构建 / 打包 / 安装，并自动修正层清单与 wrapper 的路径 |
| `mangohudfex-env.sh` | 运行环境包装（`VK_LAYER_PATH` / `MANGOHUD` / FEX 面板字段） |
| `mangohudfex-diagnose.py` | 诊断“为什么不显示”：逐项检查层、依赖、`/dev/shm`、FEX 统计 |

---

## 1. 构建与安装

```bash
./scripts/build-termux-glibc.sh            # 产物在 dist/
./scripts/build-termux-glibc.sh --install  # 直接装进 GLIBC_ROOT
```

默认 `GLIBC_ROOT=/data/data/com.termux/files/usr/glibc`，可用环境变量覆盖。

**安装前缀会自动判断**：termux-glibc 是**扁平布局**（glibc 根下直接是 `lib/`、`bin/`、`share/`，
没有 `usr/` 这一层），所以默认用空前缀，安装树为：

```
<glibc>/lib/fhud/libFHUD.so
<glibc>/share/vulkan/implicit_layer.d/FHUD.aarch64.json
<glibc>/bin/fhud
```

（meson 要求 `--prefix` 必须是绝对路径，所以构建时固定 `--prefix=/usr`，
打包阶段再把 `usr/` 这一层展开掉。）

若目标是 `usr/` 布局（`<glibc>/usr/lib/...`）：
`LAYOUT=usr ./scripts/build-termux-glibc.sh`。

`bin/fhud` 里的 shim 路径在**运行时按脚本自身位置自定位**（依次尝试 `../lib/fhud`、
`../usr/lib/fhud` 等），因此两种布局都不会出现 `LD_PRELOAD` 指向不存在的文件。

## 2. 运行

推荐用包装脚本，它会自动设置必需的环境变量：

```bash
./scripts/mangohudfex-env.sh startonwine game.exe
```

或者注入当前 shell：

```bash
source ./scripts/mangohudfex-env.sh
startonwine game.exe
```

它做了三件事：

1. `VK_LAYER_PATH=<glibc>/usr/share/vulkan/implicit_layer.d`
   —— glibc 环境下 `/usr/share` 不存在，不设这个 Vulkan loader 找不到层。
2. `MANGOHUD=1`
3. `MANGOHUD_CONFIG=fex_stats=status,apptype,sigbus,smc,softfloat,jitload`

## 3. FEX 面板

FEX 面板的数据来自 FEX 自己创建的共享内存：

```
/dev/shm/fex-<pid>-stats
```

- pid 是 **运行 FEX 的本机 aarch64 进程** 的 pid（`Source/Windows/UnixLib/FEXUnixLib.cpp` 里 `shm_open`）。
- 结构：`ThreadStatsHeader`（64B）+ 每线程一个 `ThreadStats`（112B，仅前 48B 被读取）。
- `STATS_VERSION`：当前上游为 `2`，本项目兼容读取 `1..2`。

正常情况下本进程 pid 就能对上；若你的启动链路对不上，可显式指定：

```bash
export MANGOHUD_FEX_PID=<游戏进程pid>
# 或
export MANGOHUD_FEX_SHM=fex-<pid>-stats
```

MangoHudfex 还会自动扫描 `/dev/shm/fex-*-stats`，只接受 **该 pid 仍然存活** 的项，
以避开崩溃残留的过期文件。找不到时每 2 秒重试一次，不会每帧重扫目录。

### FEX 面板字段（`fex_stats=`）

`status` `apptype` `hotthreads` `jitload` `sigbus` `smc` `softfloat`

不写 `fex_stats` 时，aarch64 上默认全部开启。写了任一 token 则只开启列出的项。

## 4. 排查

```bash
python3 ./scripts/mangohudfex-diagnose.py
```

会逐项报告并给出结论，常见结论：

| 现象 | 原因 |
|---|---|
| `FEX: Not Found!` | `/dev/shm` 里没有 `fex-<pid>-stats` —— 你的 FEX/Wine 组合未创建统计（Wine 需支持 `libarm64ecfex`/`libwow64fex` unixlib，且 FEX 版本含 SHMStats） |
| `FEX: version mismatch` | FEX 统计版本 > 2（本项目支持的上限） |
| 整个 HUD 都不显示 | `VK_LAYER_PATH` / `MANGOHUD` 未设，或层清单 `library_path` 指向的文件不存在 |
| 层加载失败、报 `GLIBC_2.38 not found` | glibc 版本低于 2.38 |
| 层加载失败、报符号缺失 | 缺 `libwayland-client.so.0` 或 `libxkbcommon.so.0` |

用 **Box64** 或原生 aarch64 跑时，显示 `FEX: Not Found!` 是正常现象 —— 没有 FEX 就没有统计。
