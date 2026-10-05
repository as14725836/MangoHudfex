#!/data/data/com.termux/files/usr/bin/bash
# ============================================================================
# MangoHudfex 运行环境（Termux glibc + FEX + Wine）
#
# 用法一（推荐，包一层启动）：
#     ./scripts/mangohudfex-env.sh <你的游戏启动命令...>
#   例：
#     ./scripts/mangohudfex-env.sh startonwine game.exe
#
# 用法二（注入当前 shell）：
#     source ./scripts/mangohudfex-env.sh
#     startonwine game.exe
# ============================================================================

GLIBC_ROOT="${GLIBC_ROOT:-/data/data/com.termux/files/usr/glibc}"
# 自动适配两种布局：
#   扁平： <glibc>/lib/mangohud          （termux-glibc 实际用的就是这种）
#   usr ： <glibc>/usr/lib/mangohud
if [ -z "${MANGO_LIB_DIR:-}" ]; then
    for _c in "$GLIBC_ROOT/lib/mangohud" "$GLIBC_ROOT/usr/lib/mangohud"; do
        [ -d "$_c" ] && { MANGO_LIB_DIR="$_c"; break; }
    done
    MANGO_LIB_DIR="${MANGO_LIB_DIR:-$GLIBC_ROOT/lib/mangohud}"
fi

if [ -z "${MANGO_LAYER_DIR:-}" ]; then
    for _c in "$GLIBC_ROOT/share/vulkan/implicit_layer.d" \
              "$GLIBC_ROOT/usr/share/vulkan/implicit_layer.d"; do
        [ -d "$_c" ] && { MANGO_LAYER_DIR="$_c"; break; }
    done
    MANGO_LAYER_DIR="${MANGO_LAYER_DIR:-$GLIBC_ROOT/share/vulkan/implicit_layer.d}"
fi

# ---- 1. Vulkan 隐式层发现 ----------------------------------------------------
# glibc 环境下 /usr/share 并不存在（真实路径在 GLIBC_ROOT 下），
# 所以必须显式告诉 Vulkan loader 去哪找 MangoHud 层清单。
export VK_LAYER_PATH="$MANGO_LAYER_DIR${VK_LAYER_PATH:+:$VK_LAYER_PATH}"

# ---- 2. 启用 MangoHud --------------------------------------------------------
export MANGOHUD=1

# ---- 3. FEX 面板字段（不设时 aarch64 上默认全开；此处显式声明便于排查） ----
# 可用字段：status, apptype, hotthreads, jitload, sigbus, smc, softfloat
if [ -z "${MANGOHUD_CONFIG:-}" ]; then
    export MANGOHUD_CONFIG="fex_stats=status,apptype,sigbus,smc,softfloat,jitload"
fi

# ---- 4. FEX 统计共享内存 -----------------------------------------------------
# 默认读 /dev/shm/fex-<本进程pid>-stats。若你的 Wine 启动链路中
# 出图进程与 FEX 进程不是同一个 pid，可显式指定其一：
#     export MANGOHUD_FEX_PID=<游戏进程pid>
#     export MANGOHUD_FEX_SHM=fex-<pid>-stats

# ---- 5. 环境自检 -------------------------------------------------------------
_warn=0
if [ ! -d /dev/shm ]; then
    echo "[!] /dev/shm 不存在：FEX 统计与层通信都会失败" >&2
    _warn=1
fi
if [ ! -f "$MANGO_LAYER_DIR/MangoHud.aarch64.json" ]; then
    echo "[!] 未找到层清单：$MANGO_LAYER_DIR/MangoHud.aarch64.json" >&2
    echo "    请先运行 scripts/build-termux-glibc.sh --install" >&2
    _warn=1
fi
if [ ! -f "$MANGO_LIB_DIR/libMangoHud.so" ]; then
    echo "[!] 未找到 $MANGO_LIB_DIR/libMangoHud.so" >&2
    _warn=1
fi
if [ "$_warn" -eq 1 ]; then
    echo "    可运行 scripts/mangohudfex-diagnose.py 查看详细诊断" >&2
fi

# ---- 6. 执行 -----------------------------------------------------------------
if [ "$#" -gt 0 ]; then
    exec "$@"
fi
