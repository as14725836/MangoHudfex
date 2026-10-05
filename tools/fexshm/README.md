# fexshm —— 让 FEX 的统计文件在原生 Termux 下也能生成

## 问题

FEX-Emu 通过 POSIX 共享内存发布统计信息：

```c
shm_open("/fex-<pid>-stats", O_CREAT | O_TRUNC | O_RDWR, 0777);
```

`shm_open()` **只能**落在 glibc 的 SHMDIR（Linux 上就是 `/dev/shm`）。
原生 Termux（没进 proot）根本没有 `/dev/shm`，于是：

- `shm_open()` 直接失败 → **统计文件永远不会被创建**
- MangoHud 扫多少个目录都找不到 → 显示 `FEX Not Found!`

日志里能直接看到这一点：

```
FEX stats: N/A (tried 12 paths). dirs: /dev/shm(no) /data/data/com.termux/files/usr/tmp(ok) ...
```

`dirs` 全 `(no)` 或关键目录 `(no)`，就说明 FEX 侧压根没能创建文件。

## 解决

预加载这个小库，把 `shm_open()` 重定向到一个**可写的普通目录**：

```bash
export LD_PRELOAD=/data/data/com.termux/files/usr/glibc/opt/fexshm/libfexshm.so
```

默认写到 `/data/data/com.termux/files/usr/tmp/fex-<pid>-stats`，
正好是 MangoHud 扫描的第一顺位目录。可用 `FEXSHM_DIR` 覆盖：

```bash
export FEXSHM_DIR=/some/writable/dir
export LD_PRELOAD=/path/to/libfexshm.so
```

MangoHud 侧读取用的是普通 `open()`，所以**只有 FEX 需要预加载**，
HUD 本身不需要。

## 编译

```bash
gcc -shared -fPIC -O2 -o libfexshm.so fexshm.c
```

只依赖 `GLIBC_2.17`，老的 Termux glibc 也能加载（刻意不用 `dlsym`）。

## 限制

- 只接管平铺名字（`/name`）；带子路径的名字回落到真正的 `/dev/shm` 逻辑
- 只影响统计文件可见性，不改变 FEX 的翻译/执行行为
- 若以后有了 root，直接 `mkdir /dev/shm && chmod 1777 /dev/shm` 才是根治
