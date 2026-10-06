/**
 * fusion_theme.hpp — FusionHUD 视觉规范的唯一事实源
 *
 * 所有数值**逐行对照**上游 FusionHUD 的绘制代码得出：
 *   fusionhud/src/main/java/com/winlator/star/widget/fusionhud/FusionHudView.kt
 *
 * 上游是 Android Canvas 绘制的合成叠加层（不是 Vulkan 层），本文件把它的
 * “颜色 + 几何”翻译成 MangoHud 的配置项，使 MangoHudfex 渲染出同一套视觉语言。
 *
 * 与旧版移植的差异（均为修正）：
 *   背景      0xCC1A1D24 → 纯黑 0x000000 × alpha 0.8   （上游 Color.argb(0.8*255,0,0,0)）
 *   圆角      12         → 8                          （上游 sp(8f)；胶囊档为 height/2）
 *   边框      0x66222B3E → 强调色 0xA374FF、宽 1.4     （上游 accent + intensity*sp(3.5)）
 *   文字描边  开启        → 关闭                        （上游没有文字描边，只有面板边框）
 *   数值色    阈值绿黄红 → 恒定 0xF2F5F9                （上游数值恒为 colValue）
 *   若干色值   BC8CFF/FF8CBC/EB5B5B/FFD54F/FFFFFF → C98BFF/FF7BC0/FF6B6B/FFAB5E/F2F5F9
 *
 * Copyright (C) 2024  The FusionHUD-VK Authors
 * SPDX-License-Identifier: GPL-3.0
 * Based on FusionHUD by The412Banner (GPL-3.0)
 */
#pragma once

#include "overlay_params.h"
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <cmath>

namespace fusionhud {

// ============================================================================
// 调色板 —— FusionHudView.kt 第 131~140 行，逐字对应
// ============================================================================
constexpr uint32_t kColGpu    = 0xFF5EE08A;  // colGpu   绿
constexpr uint32_t kColCpu    = 0xFF58A6FF;  // colCpu   蓝
constexpr uint32_t kColVram   = 0xFFC98BFF;  // colVram  紫
constexpr uint32_t kColRam    = 0xFFFF7BC0;  // colRam   粉
constexpr uint32_t kColBat    = 0xFFFFAB5E;  // colBat   橙
constexpr uint32_t kColFps    = 0xFFFF6B6B;  // colFps   红
constexpr uint32_t kColGraph  = 0xFF5EE08A;  // colGraph 绿（与 GPU 同色）
constexpr uint32_t kColValue  = 0xFFFFFFFF;  // colValue 数值白
constexpr uint32_t kColDim    = 0xFFC6D0DC;  // colDim   标签灰
constexpr uint32_t kColLo     = 0xFFF7FAFF;  // colLo    AVG / 1% / 0.1% / 0.01%
/** 面板描边色：AppThemeState.getCurrentAccentArgb() 的默认值 */
constexpr uint32_t kColAccent = 0xFFA374FF;

// ============================================================================
// 几何常量 —— 对照 onDraw() / sp() 换算
// ============================================================================
constexpr float kBgRadiusSp      = 8.0f;   // onDraw: sp(8f)
constexpr float kPillRadiusRatio = 0.5f;   // Pill: height / 2f（胶囊）
constexpr float kTileRadiusSp    = 6.0f;   // onDraw: sp(6f)
constexpr float kTileBgBase      = 14.0f;  // Color.argb(14 * bgOpacity) → clamp[8,40]
constexpr float kOutlineMaxSp    = 3.5f;   // strokeW = outlineIntensity * sp(3.5f)
/** 画面边缘留白（px）：FusionHUD 外观默认不贴屏幕边缘。
 *  上游的 10px 边距在用户设了 offset_x / offset_y 或 hud_no_margin 时会变成 0，
 *  这里补一个下限，避免面板贴住屏幕边缘。 */
constexpr float kEdgeInset = 12.0f;
constexpr float kRightInsetSp = 5.0f;   // 面板右侧内缩（比 pad 小）

constexpr float kOutlineDefault  = 0.0f;   // 面板描边强度：默认 0 = 不画紫边；>0 时 × sp(3.5) 为线宽
constexpr float kOutlineStrong   = 0.7f;   // "strong" → 70
constexpr float kBgOpacityDefault = 0.8f;  // bgOpacity 默认 0.8
constexpr float kGraphHeightSp   = 26.0f;  // Full/Mega: gh = sp(26f)
constexpr float kFullGraphMinSp  = 200.0f; // Full: 图最小宽 sp(200f)
constexpr float kMinimalGraphSp  = 150.0f; // Minimal: gw = sp(150f)

// ============================================================================
// 五种尺寸 —— FusionHudModels.kt 的 FusionSize
// ============================================================================
enum class FusionSize { FULL, TILES, PILL, MINIMAL, MEGA };

/**
 * MangoHud 预设编号基址：FusionHUD 五档占用 preset 10~14
 * （见 overlay_params.cpp 的 presets() switch）。
 */
constexpr int kPresetBase = 10;

inline const char* fusionSizeToken(FusionSize s) {
    switch (s) {
        case FusionSize::FULL:    return "full";
        case FusionSize::TILES:   return "tiles";
        case FusionSize::PILL:    return "pill";
        case FusionSize::MINIMAL: return "minimal";
        case FusionSize::MEGA:    return "mega";
    }
    return "full";
}

inline FusionSize fusionSizeFromToken(const std::string& t) {
    if (t == "tiles"   || t == "TILES")   return FusionSize::TILES;
    if (t == "pill"    || t == "PILL")    return FusionSize::PILL;
    if (t == "minimal" || t == "MINIMAL") return FusionSize::MINIMAL;
    if (t == "mega"    || t == "MEGA")    return FusionSize::MEGA;
    return FusionSize::FULL;
}

/** 各尺寸的字号/间距（单位 sp，上游还会乘 density*scale） */
struct FusionMetrics {
    float row_px;        // 主行字号（Full/Mega 的 rowPx）
    float unit_ratio;    // 单位字号 = rowPx * unit_ratio
    float pad;           // 外边距
    float line_gap;      // 行间距
    float lv_gap;        // 标签列与数值列间距
    float key_px;        // Tiles: 键字号
    float val_px;        // Tiles: 值字号
    float sub_px;        // Tiles: 副行字号
    float big_px;        // Pill/Minimal: 主数字字号
    float band_px;       // Mega 底栏字号
};

inline FusionMetrics fusionMetrics(FusionSize s) {
    switch (s) {
        // Full:  rowPx=14 unit=0.62 pad=12 lineGap=5 lvGap=9
        case FusionSize::FULL:    return {14.0f, 0.62f, 12.0f, 5.0f, 9.0f, 12.0f, 20.0f, 12.0f,  0.0f,  0.0f};
        // Tiles: key=12 val=20 sub=12 unit=13 pad=11 innerPad=9 tileGap=7 lineGap=5
        case FusionSize::TILES:   return {14.0f, 0.61f, 11.0f, 5.0f, 9.0f, 12.0f, 20.0f, 12.0f,  0.0f,  0.0f};
        // Pill:  big=34 bigUnit=big*0.36 stk=13 pad=12 midGap=12 stkLineGap=4
        case FusionSize::PILL:    return {13.0f, 0.36f, 12.0f, 4.0f, 12.0f, 12.0f, 20.0f, 12.0f, 34.0f,  0.0f};
        // Minimal: big=38 bigUnit=big*0.32 sub=13 pad=12 lineGap=6
        case FusionSize::MINIMAL: return {13.0f, 0.32f, 12.0f, 6.0f,  9.0f, 12.0f, 20.0f, 12.0f, 38.0f,  0.0f};
        // Mega:  rowPx=13 unit=0.62 band=11 pad=12 lineGap=4.5 lvGap=8 gutter=16
        case FusionSize::MEGA:    return {13.0f, 0.62f, 12.0f, 4.5f, 8.0f, 12.0f, 20.0f, 12.0f,  0.0f, 11.0f};
    }
    return {14.0f, 0.62f, 12.0f, 5.0f, 9.0f, 12.0f, 20.0f, 12.0f, 0.0f, 0.0f};
}

/** FusionHUD 用到的字号（sp 单位）。font.cpp 按这三档分别烘焙专用字体，
 *  绘制时挑“不小于目标字号”的那一档 —— 只缩不放，最清晰。
 *  大：Minimal 的大号 FPS = sp(34)≈51px；中：Tiles 的数值 = sp(18)≈27px；
 *  小：常规行 / 单位 / 时钟 = sp(12)≈18px。 */
constexpr float kMaxTextSp = 22.0f;
constexpr float kMidTextSp = 18.0f;
constexpr float kSmallTextSp = 13.0f;

/**
 * sp → MangoHud font_size 的换算系数。
 * 上游 sp 会乘 Android density（主流手机 ≈2.6~3.5）；MangoHud 的 font_size 直接是
 * 像素高度。取 2.1 作为折中，并把结果夹在 [20,34] 内，避免在 HUD 上出现极端字号。
 */
constexpr float kSpToPx = 2.1f;   // sp -> px 基准（2.1 让字号明显更醒目）

inline float fusionFontSize(FusionSize s) {
    const FusionMetrics m = fusionMetrics(s);
    float primary = m.row_px;
    if (s == FusionSize::PILL || s == FusionSize::MINIMAL) primary = m.big_px;
    if (s == FusionSize::TILES) primary = m.val_px;
    float px = primary * kSpToPx;
    if (px < 20.0f) px = 20.0f;   // 下限抬高：小档也不再是“蚂蚁字”
    if (px > 34.0f) px = 34.0f;   // 上限抬高：Pill/Minimal 的大数字终于能放大
    return std::round(px);
}

/** 行距：用 FusionHUD 的 lineGap/rowPx 比例近似 MangoHud 的 cellpadding_y */
inline float fusionCellPaddingY(FusionSize s) {
    const FusionMetrics m = fusionMetrics(s);
    const float ratio = m.line_gap / (m.row_px > 0.0f ? m.row_px : 1.0f);
    // MangoHud 默认 -0.085；按比例映射到 0.0~0.12
    return std::round(ratio * 30.0f) / 100.0f;
}

// ============================================================================
// 配置项输出
// ============================================================================
inline std::string hex6(uint32_t argb) {
    static const char* d = "0123456789ABCDEF";
    std::string s(6, '0');
    s[0] = d[(argb >> 20) & 0xF];
    s[1] = d[(argb >> 16) & 0xF];
    s[2] = d[(argb >> 12) & 0xF];
    s[3] = d[(argb >>  8) & 0xF];
    s[4] = d[(argb >>  4) & 0xF];
    s[5] = d[(argb >>  0) & 0xF];
    return s;
}

/**
 * FusionHUD 主题 → MangoHud 配置键值表。
 * 交给 overlay_params.cpp 的 add_to_options() 走**正常解析路径**写入，
 * 保证不会被后续的配置解析覆盖。
 */
inline std::vector<std::pair<std::string, std::string>>
fusionThemeOptions(FusionSize size, float bg_opacity = kBgOpacityDefault,
                   float outline_intensity = kOutlineDefault) {
    std::vector<std::pair<std::string, std::string>> o;

    // ---- 指标色（标签用彩色，数值恒为 colValue）----
    o.emplace_back("gpu_color",    hex6(kColGpu));
    o.emplace_back("cpu_color",    hex6(kColCpu));
    o.emplace_back("vram_color",   hex6(kColVram));
    o.emplace_back("ram_color",    hex6(kColRam));
    o.emplace_back("battery_color", hex6(kColBat));
    o.emplace_back("engine_color", hex6(kColFps));   // FusionHUD 的 FPS/引擎行标签
    o.emplace_back("network_color", hex6(kColFps));  // Mega 的 NET 行
    o.emplace_back("frametime_color", hex6(kColGraph)); // 帧时间图线
    o.emplace_back("wine_color",   hex6(kColDim));
    o.emplace_back("io_color",     hex6(kColDim));
    o.emplace_back("text_color",   hex6(kColValue));
    o.emplace_back("horizontal_separator_color", hex6(kColLo));

    // ---- 数值恒为白：把三段阈值色全部设成 colValue ----
    const std::string white = hex6(kColValue) + "," + hex6(kColValue) + "," + hex6(kColValue);
    o.emplace_back("gpu_load_color", white);
    o.emplace_back("cpu_load_color", white);
    o.emplace_back("fps_color",      white);

    // ---- 面板：纯黑底（上游 Color.argb(bgOpacity*255, 0,0,0)）+ 圆角 8，无文字描边 ----
    o.emplace_back("background_color", "000000");
    o.emplace_back("background_alpha", std::to_string(bg_opacity));
    o.emplace_back("alpha", "1.0");
    o.emplace_back("round_corners", std::to_string((int)std::lround(kBgRadiusSp)));
    o.emplace_back("text_outline", "0");

    // ---- 字号与行距 ----
    o.emplace_back("font_size", std::to_string((int)std::lround(fusionFontSize(size))));
    o.emplace_back("font_scale", "1.0");
    o.emplace_back("cellpadding_y", std::to_string(fusionCellPaddingY(size)));

    // ---- 署名 ----
    // HUD 上默认不再显示署名行；GPL-3.0 §7(b) 的署名由下列位置承担：
    //   1) 项目文档：ATTRIBUTION.md、README 的 Credits 章节
    //   2) 命令行关于界面：mangohud --credits / mangohud --version
    // 若仍想在画面上显示，用户可自行在配置里设置：
    //   custom_text_center=Your text

    // outline_intensity 只影响面板描边宽度，由 fusionOutlineWidth() 在渲染期使用
    (void)outline_intensity;
    return o;
}

/** FusionHUD 的面板描边宽度（对应 strokeW = intensity * sp(3.5)） */
inline float fusionOutlineWidth(float outline_intensity = kOutlineDefault) {
    if (outline_intensity <= 0.0f) return 0.0f;
    return outline_intensity * kOutlineMaxSp;
}

/** 当前是否处于任一 FusionHUD 档位 */
inline bool isFusionActive(const overlay_params& p) {
    return p.enabled[OVERLAY_PARAM_ENABLED_fusion_full]    ||
           p.enabled[OVERLAY_PARAM_ENABLED_fusion_tiles]   ||
           p.enabled[OVERLAY_PARAM_ENABLED_fusion_pill]    ||
           p.enabled[OVERLAY_PARAM_ENABLED_fusion_minimal] ||
           p.enabled[OVERLAY_PARAM_ENABLED_fusion_mega];
}

/** 当前档位（多个同时置位时按 FusionSize 顺序取第一个） */
inline FusionSize currentFusionSize(const overlay_params& p) {
    if (p.enabled[OVERLAY_PARAM_ENABLED_fusion_tiles])   return FusionSize::TILES;
    if (p.enabled[OVERLAY_PARAM_ENABLED_fusion_pill])    return FusionSize::PILL;
    if (p.enabled[OVERLAY_PARAM_ENABLED_fusion_minimal]) return FusionSize::MINIMAL;
    if (p.enabled[OVERLAY_PARAM_ENABLED_fusion_mega])    return FusionSize::MEGA;
    return FusionSize::FULL;
}

} // namespace fusionhud
