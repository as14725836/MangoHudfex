#!/usr/bin/env bash
# ============================================================================
# cleanup-legacy.sh —— 清理 MangoHud 旧命名残留（升级“擦干净”用）
#
# 解决的问题：旧版本解包/安装留下的文件不会被覆盖安装删除，导致——
#   * Vulkan loader 扫到旧层清单 MangoHud*.json → 去找已不存在的旧库 → 报错
#   * shell 里旧的 LD_PRELOAD=.../lib/mangohud/... → ld.so 报“找不到旧库”
#
# 用法：
#   scripts/cleanup-legacy.sh                 # 默认清理 $GLIBC_ROOT
#   scripts/cleanup-legacy.sh /path/to/dir    # 清理指定目录（如旧解包目录）
# ============================================================================
set -u
ROOT="${1:-${GLIBC_ROOT:-/data/data/com.termux/files/usr/glibc}}"
if [ ! -d "$ROOT" ]; then
    echo "[!] 目录不存在: $ROOT" >&2
    exit 1
fi
echo "[*] 目标: $ROOT"
# 旧层清单（只删 MangoHud*，不动 FHUD*）
find "$ROOT" -maxdepth 5 -path '*implicit_layer.d/MangoHud*.json' -print -delete 2>/dev/null
# 旧库目录 / 旧库文件
rm -rfv "$ROOT/lib/mangohud" "$ROOT/usr/lib/mangohud" 2>/dev/null
rm -fv  "$ROOT/lib/"libMangoHud* "$ROOT/usr/lib/"libMangoHud* 2>/dev/null
# 旧 wrapper / 绘图工具
rm -fv  "$ROOT/bin/mangohud" "$ROOT/bin/mangoplot" 2>/dev/null
rm -fv  "$ROOT/usr/bin/mangohud" "$ROOT/usr/bin/mangoplot" 2>/dev/null
echo "[*] 完成。当前 implicit_layer.d："
ls -la "$ROOT/share/vulkan/implicit_layer.d/" "$ROOT/usr/share/vulkan/implicit_layer.d/" 2>/dev/null | head -24
echo "[*] 提示：shell 里若 export 过旧 LD_PRELOAD（含 libMangoHud），记得 unset。"
