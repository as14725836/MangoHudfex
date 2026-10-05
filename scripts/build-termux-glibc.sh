#!/usr/bin/env bash
# ============================================================================
# MangoHudfex —— Termux glibc (aarch64) 一键构建 / 打包 / 安装
#
# 用法：
#   ./scripts/build-termux-glibc.sh                 # 构建 + 打包到 dist/
#   ./scripts/build-termux-glibc.sh --install       # 构建 + 直接安装到 GLIBC_ROOT
#   ./scripts/build-termux-glibc.sh --no-strip      # 保留符号（调试用）
#
# 可覆盖的环境变量：
#   GLIBC_ROOT   Termux glibc 根（默认 /data/data/com.termux/files/usr/glibc）
#   PREFIX       meson --prefix（默认按 GLIBC_ROOT 布局自动判断；详见下方注释）
#   LIBDIR       meson --libdir（默认 lib/mangohud）
#   BUILDTYPE    release/debug（默认 release）
#   NPROC        并行度（默认 nproc）
#   DIST_DIR     产物目录（默认 dist）
# ============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# ---------------- 可配置项 ----------------
GLIBC_ROOT="${GLIBC_ROOT:-/data/data/com.termux/files/usr/glibc}"
LIBDIR="${LIBDIR:-lib/mangohud}"
BUILDTYPE="${BUILDTYPE:-release}"
BUILD_DIR="${BUILD_DIR:-build}"
DIST_DIR="${DIST_DIR:-dist}"
NPROC="${NPROC:-$(nproc 2>/dev/null || echo 4)}"

DO_INSTALL=0
DO_STRIP=1
for arg in "$@"; do
    case "$arg" in
        --install)  DO_INSTALL=1 ;;
        --no-strip) DO_STRIP=0 ;;
        -h|--help)  sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "未知参数: $arg" >&2; exit 2 ;;
    esac
done

DESTDIR_ABS="${REPO_ROOT}/${BUILD_DIR}/release"

# ---- 安装前缀 ----------------------------------------------------------------
# termux-glibc 是**扁平布局**：glibc 根下直接就是 lib/ bin/ share/，
# 没有 usr/ 这一层。因此默认前缀为空串，安装树为 lib/mangohud、share/vulkan/… 。
# 若显式设置了 PREFIX（含空串）则尊重之；否则按目标 GLIBC_ROOT 的既有布局判断：
#   存在 <glibc>/usr/lib 且不存在 <glibc>/lib  → 用 /usr
#   其余情况                                  → 用 ""（扁平）
if [ -z "${PREFIX+x}" ]; then
    if [ -d "$GLIBC_ROOT/usr/lib" ] && [ ! -d "$GLIBC_ROOT/lib" ]; then
        PREFIX="/usr"
    else
        PREFIX=""
    fi
fi

LIBDIR_ABS="${PREFIX%/}/${LIBDIR}"          # 例如 /lib/mangohud（扁平）或 /usr/lib/mangohud
LAYER_DIR="${PREFIX%/}/share/vulkan/implicit_layer.d"

log()  { printf '\033[1;92m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;93m[!]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;91m[x]\033[0m %s\n' "$*" >&2; exit 1; }

# ---------------- 0. 前置检查 ----------------
command -v meson >/dev/null || die "缺少 meson"
command -v ninja >/dev/null || die "缺少 ninja"
command -v g++   >/dev/null || die "缺少 g++"
if command -v python3 >/dev/null; then
    python3 -c 'import mako' 2>/dev/null || warn "缺少 python3-mako，可能影响代码生成"
fi

# 子模块（modules/minhook 是必需依赖）
if [ -d .git ] && git submodule status --recursive 2>/dev/null | grep -q '^-'; then
    log "初始化 git 子模块"
    git submodule update --init --recursive
fi

# ---------------- 1. 配置 ----------------
MESON_OPTS=(
    --prefix="$PREFIX"
    --libdir="$LIBDIR"
    --buildtype="$BUILDTYPE"
    -Dappend_libdir_mangohud=false
    -Dwith_fex=true
    -Dwith_nvml=disabled
    -Dwith_xnvctrl=disabled
    -Dwith_x11=enabled
    -Dwith_wayland=enabled
    -Dwith_dbus=enabled
    -Dmangoapp=false
    -Dmangohudctl=false
    -Dinclude_doc=false
    -Dtests=disabled
)

log "meson setup ($BUILD_DIR)"
if [ -f "$BUILD_DIR/build.ninja" ]; then
    meson setup --reconfigure "$BUILD_DIR" "${MESON_OPTS[@]}"
else
    meson setup "$BUILD_DIR" "${MESON_OPTS[@]}"
fi

# ---------------- 2. 编译 ----------------
log "编译中（-j$NPROC）"
ninja -C "$BUILD_DIR" -j "$NPROC"

# 校验 -Dwith_fex 真正生效
if grep -q 'HAVE_FEX' "$BUILD_DIR/build.ninja" "$BUILD_DIR/compile_commands.json" 2>/dev/null; then
    log "FEX 支持已编入 (-DHAVE_FEX)"
else
    die "未检测到 HAVE_FEX，with_fex 未生效"
fi

# ---------------- 3. 安装到 DESTDIR ----------------
log "安装到 DESTDIR"
rm -rf "$DESTDIR_ABS"
DESTDIR="$DESTDIR_ABS" ninja -C "$BUILD_DIR" install

SHIM="$DESTDIR_ABS${LIBDIR_ABS}/libMangoHud_shim.so"
[ -f "$SHIM" ] || die "未找到 $SHIM，安装布局与预期不符"

# ---------------- 4. 瘦身 ----------------
if [ "$DO_STRIP" -eq 1 ]; then
    log "strip 二进制"
    find "$DESTDIR_ABS" -type f -name '*.so' -exec strip --strip-unneeded {} + 2>/dev/null || true
fi

# ---------------- 5. 修正 Vulkan 层清单的库路径 ----------------
# 原产物写死 /usr/lib/mangohud/...，装到 glibc 根后必须改成绝对路径
log "修正 Vulkan 层清单路径 -> ${GLIBC_ROOT}${LIBDIR_ABS}/"
find "$DESTDIR_ABS${LAYER_DIR}" -name '*.json' -print0 2>/dev/null |
    xargs -0 -r sed -i "s|\"library_path\"[[:space:]]*:[[:space:]]*\"[^\"]*\"|\"library_path\" : \"${GLIBC_ROOT}${LIBDIR_ABS}/libMangoHud.so\"|"

# ---------------- 6. wrapper 检查 ----------------
# wrapper 里的 shim 路径由脚本**运行时自定位**（bin/mangohud.in），
# 因此这里不再改写它 —— 早先的 sed 会误伤自定位语句本身。
WRAPPER="$DESTDIR_ABS${PREFIX%/}/bin/mangohud"
if [ -f "$WRAPPER" ]; then
    if grep -q 'MANGOHUD_LIB_NAME' "$WRAPPER"; then
        log "wrapper 就绪（shim 路径运行时自定位）"
    else
        warn "wrapper 中未找到 MANGOHUD_LIB_NAME，LD_PRELOAD 可能失效"
    fi
else
    warn "未生成 wrapper：$WRAPPER"
fi

# ---------------- 7. 打包 ----------------
mkdir -p "$DIST_DIR"
VER="$(git describe --tags --always --dirty 2>/dev/null || echo dev)"
ARCH="$(uname -m)"
TARBALL="${DIST_DIR}/termux-glibc-mangohudfex-${VER}-${ARCH}.tar.gz"
log "打包 -> $TARBALL"
tar -czf "$TARBALL" -C "$DESTDIR_ABS" .

# ---------------- 8. 可选安装 ----------------
if [ "$DO_INSTALL" -eq 1 ]; then
    [ -d "$GLIBC_ROOT" ] || die "GLIBC_ROOT 不存在: $GLIBC_ROOT"
    log "安装到 $GLIBC_ROOT"
    tar -xzf "$TARBALL" -C "$GLIBC_ROOT"
    log "安装完成"
fi

# ---------------- 9. 运行期依赖自检 ----------------
log "运行期依赖自检"
ok=1

# glibc 版本（需 >= 2.38）
for cand in "${GLIBC_ROOT}/lib/libc.so.6" "${GLIBC_ROOT}/usr/lib/libc.so.6" /lib/aarch64-linux-gnu/libc.so.6; do
    if [ -e "$cand" ]; then
        v="$("$cand" 2>/dev/null | head -1 || true)"
        [ -n "$v" ] && { log "libc: $v"; }
        break
    fi
done

# 硬依赖：libwayland-client.so.0 / libxkbcommon.so.0
for lib in libwayland-client.so.0 libxkbcommon.so.0; do
    if find "${GLIBC_ROOT}" -name "$lib" -print -quit 2>/dev/null | grep -q .; then
        log "依赖 $lib: 已找到"
    else
        warn "依赖 $lib: 未在 GLIBC_ROOT 中找到 —— LD_PRELOAD/层加载可能失败，请装 wayland / libxkbcommon"
        ok=0
    fi
done

echo
log "完成"
echo "  产物: $TARBALL"
echo "  未打包前的安装树: $DESTDIR_ABS"
if [ "$ok" -eq 0 ]; then
    echo
    warn "存在缺失的运行时依赖，见上方 [!] 提示"
fi
