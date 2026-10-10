#!/data/data/com.termux/files/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MangoHudfex 诊断工具（Termux glibc + FEX + Wine）

逐项检查“为什么 HUD / FEX 面板没显示”，并直接指出缺什么：

  1. glibc 根与安装文件
  2. Vulkan 层清单（library_path 是否指向真实存在的 .so）
  3. 层所需的运行期依赖（libwayland-client / libxkbcommon / glibc 版本）
  4. Vulkan loader 是否存在
  5. /dev/shm 与 FEX 统计共享内存（解析 fex-<pid>-stats 头部）

用法：
    python3 scripts/mangohudfex-diagnose.py
    GLIBC_ROOT=/path/to/glibc python3 scripts/mangohudfex-diagnose.py
"""
import os
import re
import struct
import sys
import glob

GLIBC_ROOT = os.environ.get("GLIBC_ROOT", "/data/data/com.termux/files/usr/glibc")
def _pick_dir(*cands):
    """返回第一个存在的目录；都不存在则返回第一个候选（便于报错时给出预期路径）。"""
    for c in cands:
        if os.path.isdir(c):
            return c
    return cands[0]


# 扁平布局（<glibc>/lib/...）优先，回落 usr 布局（<glibc>/usr/lib/...）
LIB_DIR = os.environ.get("MANGO_LIB_DIR") or _pick_dir(
    os.path.join(GLIBC_ROOT, "lib/mangohud"),
    os.path.join(GLIBC_ROOT, "usr/lib/mangohud"),
)
LAYER_DIR = os.environ.get("MANGO_LAYER_DIR") or _pick_dir(
    os.path.join(GLIBC_ROOT, "share/vulkan/implicit_layer.d"),
    os.path.join(GLIBC_ROOT, "usr/share/vulkan/implicit_layer.d"),
)

OK = "\033[1;92m[OK]\033[0m"
BAD = "\033[1;91m[!!]\033[0m"
WARN = "\033[1;93m[! ]\033[0m"

problems = []
notes = []


def section(title):
    print("\n\033[1m== %s ==\033[0m" % title)


# ---------------------------------------------------------------- 1. 安装文件
section("1. glibc 根与安装文件")
print("GLIBC_ROOT = %s" % GLIBC_ROOT)
if not os.path.isdir(GLIBC_ROOT):
    print("%s glibc 根不存在" % BAD)
    problems.append("GLIBC_ROOT 不存在：%s" % GLIBC_ROOT)
else:
    print("%s glibc 根存在" % OK)

for name in ("libFHUD.so", "libFHUD_shim.so", "libFHUD_opengl.so"):
    p = os.path.join(LIB_DIR, name)
    if os.path.isfile(p):
        print("%s %s (%.1f MB)" % (OK, p, os.path.getsize(p) / 1048576.0))
    else:
        print("%s 缺失 %s" % (BAD, p))
        problems.append("缺少 %s —— 先运行 scripts/build-termux-glibc.sh --install" % name)

# ---------------------------------------------------------------- 2. 层清单
section("2. Vulkan 层清单")
manifests = sorted(glob.glob(os.path.join(LAYER_DIR, "*.json")))
if not manifests:
    print("%s %s 下没有层清单" % (BAD, LAYER_DIR))
    problems.append("没有 Vulkan 层清单，MangoHud 不会被自动加载")
for m in manifests:
    print("清单: %s" % m)
    try:
        txt = open(m, "r", encoding="utf-8").read()
    except Exception as e:
        print("%s 读取失败: %s" % (BAD, e))
        continue
    mt = re.search(r'"library_path"\s*:\s*"([^"]+)"', txt)
    if not mt:
        print("%s 清单里没有 library_path" % BAD)
        continue
    lib = mt.group(1)
    print("  library_path = %s" % lib)
    if os.path.isfile(lib):
        print("  %s 指向的文件存在" % OK)
    else:
        print("  %s 指向的文件不存在（层会加载失败）" % BAD)
        problems.append("层清单 library_path 指向不存在的文件：%s" % lib)
    if '"MANGOHUD"' in txt:
        print("  %s enable_environment 含 MANGOHUD" % OK)
    else:
        print("  %s 未声明 MANGOHUD 触发条件" % WARN)

# ---------------------------------------------------------------- 3. 依赖
section("3. 层的运行期依赖")


def find_lib(soname):
    pats = [
        os.path.join(GLIBC_ROOT, "lib", soname + "*"),
        os.path.join(GLIBC_ROOT, "usr/lib", soname + "*"),
        os.path.join(GLIBC_ROOT, "lib/aarch64-linux-gnu", soname + "*"),
        os.path.join(GLIBC_ROOT, "usr/lib/aarch64-linux-gnu", soname + "*"),
    ]
    for p in pats:
        hits = glob.glob(p)
        if hits:
            return hits[0]
    return None


for soname in ("libwayland-client.so.0", "libxkbcommon.so.0"):
    hit = find_lib(soname)
    if hit:
        print("%s %s -> %s" % (OK, soname, hit))
    else:
        print("%s %s 未找到（libFHUD.so 的硬依赖，会导致整层加载失败）" % (BAD, soname))
        problems.append("缺少 %s —— 在 glibc 源里安装 wayland / libxkbcommon" % soname)

# glibc 版本（产物要求 >= 2.38）
libc = None
for c in (os.path.join(GLIBC_ROOT, "lib/libc.so.6"),
          os.path.join(GLIBC_ROOT, "usr/lib/libc.so.6")):
    if os.path.exists(c):
        libc = c
        break
if libc:
    try:
        data = open(libc, "rb").read()
        vers = [tuple(int(x) for x in v.split(b".")) for v in re.findall(rb"GLIBC_2\.(\d+)", data)]
        if vers:
            mx = max(v[0] for v in vers)
            print("%s libc %s 最高 GLIBC_2.%d（产物要求 >= 2.38）" % (OK, libc, mx))
            if mx < 38:
                print("%s glibc 版本偏低，libFHUD.so 可能报 GLIBC_2.38 not found" % WARN)
                problems.append("glibc 最高只到 2.3%d，低于产物要求 2.38" % mx)
    except Exception:
        pass

# ---------------------------------------------------------------- 4. loader
section("4. Vulkan loader")
loader = None
for c in (os.path.join(GLIBC_ROOT, "lib/libvulkan.so.1"),
          os.path.join(GLIBC_ROOT, "usr/lib/libvulkan.so.1")):
    if os.path.exists(c):
        loader = c
        break
if loader:
    print("%s %s" % (OK, loader))
else:
    print("%s 未在 glibc 根找到 libvulkan.so.1（需要 Vulkan loader，非驱动）" % BAD)
    problems.append("缺少 libvulkan.so.1（Vulkan loader）")

vkp = os.environ.get("VK_LAYER_PATH", "")
if LAYER_DIR in vkp:
    print("%s VK_LAYER_PATH 已包含 %s" % (OK, LAYER_DIR))
else:
    print("%s VK_LAYER_PATH 未包含层目录（当前='%s'）" % (BAD, vkp))
    notes.append("启动前沿用：export VK_LAYER_PATH=%s" % LAYER_DIR)

if os.environ.get("MANGOHUD") == "1":
    print("%s MANGOHUD=1 已设置" % OK)
else:
    print("%s MANGOHUD 未设为 1，层不会启用" % BAD)
    notes.append("启动前：export MANGOHUD=1")

# ---------------------------------------------------------------- 5. FEX SHM
section("5. /dev/shm 与 FEX 统计共享内存")
HEADER_FMT = "<BBH48sIII"          # Version, app_type, ThreadStatsSize, fex_version[48], Head, Size, Pad
HEADER_SIZE = struct.calcsize(HEADER_FMT)   # 64
STAT_FMT = "<IIQQQQQ"              # Next, TID, JIT, Signal, SIGBUS, SMC, FloatFallback
STAT_SIZE = struct.calcsize(STAT_FMT)       # 48
APP_TYPES = {0: "Linux32", 1: "Linux64", 2: "arm64ec", 3: "wow64"}

if not os.path.isdir("/dev/shm"):
    print("%s /dev/shm 不存在 —— FEX 统计无法创建/读取" % BAD)
    problems.append("/dev/shm 不存在")
else:
    print("%s /dev/shm 存在" % OK)
    cands = sorted(glob.glob("/dev/shm/fex-*-stats"))
    if not cands:
        print("%s 当前没有 fex-*-stats" % WARN)
        notes.append("若游戏已在运行仍无此文件，说明你的 FEX 未创建统计（Wine 需支持 "
                     "libarm64ecfex / libwow64fex unixlib，且 FEX 版本含 SHMStats）")
    for path in cands:
        try:
            fd = os.open(path, os.O_RDONLY)
            head = os.read(fd, HEADER_SIZE)
            if len(head) < HEADER_SIZE:
                print("%s %s 太小，跳过" % (WARN, path))
                os.close(fd)
                continue
            ver, app_type, tssize, fexver, hoff, size, _pad = struct.unpack(HEADER_FMT, head)
            fexver = fexver.split(b"\x00", 1)[0].decode("utf-8", "replace")
            print("%s %s" % (OK, path))
            print("    header: version=%d  app_type=%s  thread_stats_size=%d  fex_version=%s"
                  % (ver, APP_TYPES.get(app_type, "?"), tssize, fexver or "(空)"))
            print("    shm:    head_offset=%d  size=%d" % (hoff, size))
            if ver != 2:
                if ver == 0 or ver > 2:
                    print("    %s 版本号异常，MangoHudfex 会拒绝读取" % BAD)
                else:
                    print("    %s 版本 %d（旧版），MangoHudfex 已兼容读取" % (WARN, ver))
            # 遍历线程链表（用 pread 从 0 偏移读，避免受前面 read 的文件偏移影响）
            base = os.pread(fd, max(size, HEADER_SIZE + 112 * 4), 0)
            off, n, loads = hoff, 0, []
            while off and off + STAT_SIZE <= len(base) and n < 4096:
                nxt, tid, jit, sig, sigbus, smc, ff = struct.unpack(
                    STAT_FMT, base[off:off + STAT_SIZE])
                if tid:
                    loads.append((tid, jit, sigbus, smc, ff))
                off = nxt
                n += 1
            print("    线程槽位: %d 个活跃" % len(loads))
            for tid, jit, sigbus, smc, ff in loads[:8]:
                print("      tid=%-6d jit=%-12d sigbus=%-6d smc=%-6d softfloat=%d"
                      % (tid, jit, sigbus, smc, ff))
            os.close(fd)
        except Exception as e:
            print("%s 解析 %s 失败: %s" % (WARN, path, e))

# ---------------------------------------------------------------- 6. CPU 频率
section("6. CPU 频率可读性")
import glob as _glob

cpu_nodes = sorted(_glob.glob("/sys/devices/system/cpu/cpu[0-9]*"))
cpufreq_ok, cpufreq_bad = [], []
for n in cpu_nodes:
    cur = os.path.join(n, "cpufreq/scaling_cur_freq")
    try:
        with open(cur) as f:
            khz = int(f.read().strip())
        cpufreq_ok.append((os.path.basename(n), khz // 1000))
    except Exception:
        cpufreq_bad.append(os.path.basename(n))

print("%s 发现 %d 个 CPU 节点；可读 scaling_cur_freq 的 %d 个"
      % (OK if cpu_nodes else WARN, len(cpu_nodes), len(cpufreq_ok)))
for name, mhz in cpufreq_ok[:8]:
    print("    %-6s %d MHz" % (name, mhz))
if cpufreq_bad:
    print("%s 以下核的 cpufreq 读不到（这些核的频率会显示为 0）：%s"
          % (WARN, ", ".join(cpufreq_bad[:8])))

# policyN 回落
policy = sorted(_glob.glob("/sys/devices/system/cpu/cpufreq/policy*"))
if policy:
    print("%s 存在 %d 个 policy 节点（核级节点缺失时可作为回落）" % (OK, len(policy)))
else:
    print("%s 没有 policy 节点" % WARN)

if not cpu_nodes:
    problems.append("/sys/devices/system/cpu 下没有 CPU 节点 —— /sys 可能未挂载进该环境")
elif not cpufreq_ok:
    problems.append("所有核的 cpufreq 都读不到（SELinux 限制或 /sys 未挂载），CPU 频率将显示 0")
    notes.append("可设置 MANGOHUD_CPUFREQ_PATH=<某个可读的 cpufreq 节点> 作为兜底")

if os.path.exists("/proc/stat"):
    try:
        open("/proc/stat").read(1)
        print("%s /proc/stat 可读" % OK)
    except Exception:
        print("%s /proc/stat 不可读（CPU 占用率会退化为“进程占用”而不是整机占用）" % WARN)
else:
    print("%s /proc/stat 不存在" % WARN)

cpuinfo_mhz = 0
try:
    with open("/proc/cpuinfo") as f:
        cpuinfo_mhz = f.read().count("MHz")
except Exception:
    pass
print("    /proc/cpuinfo 里 'MHz' 字段出现 %d 次（ARM 通常为 0，x86/Wine 才有）" % cpuinfo_mhz)

# ---------------------------------------------------------------- 结论
section("结论")
if problems:
    for p in problems:
        print("%s %s" % (BAD, p))
else:
    print("%s 未发现阻断性问题" % OK)
if notes:
    print()
    for n in notes:
        print("%s %s" % (WARN, n))
print()
print("提示：FEX 面板只在真正跑 FEX 的游戏进程里有数据；")
print("      Box64 / 原生 aarch64 下会显示 'Not Found!'，属正常。")
sys.exit(1 if problems else 0)
