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
 *   若干色值   BC8CFF/FF8CBC/EB5B5B/FFD54F/FFFFFF → B08CFF/45D6C8/FF6B6B/FFD166/F2F5F9
 *              （v2 起再次重排：按色相分离选色，暖色只留 FPS 一处红）
 *
 * Copyright (C) 2024  The FusionHUD-VK Authors
 * SPDX-License-Identifier: GPL-3.0
 * Based on FusionHUD by The412Banner (GPL-3.0)
 */
#pragma once

#include "overlay_params.h"
#include <cstdint>
#include <string>
#include <random>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <utility>
#include <cmath>

namespace fusionhud {

// ============================================================================
// 调色板 —— FusionHudView.kt 第 131~140 行，逐字对应
// ============================================================================
// 色相分离：0(红) / 42(黄) / 150(绿) / 175(青) / 213(蓝) / 265(紫)
// 旧版的 RAM 粉(330) 与 BAT 橙(30) 都压在这次暖色区里，六个标签里有三个偏红，
// 远看就糊成一片。现在暖色只保留 FPS 一处红（低帧警示），其余全走冷色。
// ↑ 上面是"出厂默认"色（也是下面这些变量的初始值）。
// 随机配色功能会覆盖它们：见文件末尾的 randomizePalette()。
// 之所以能直接改名字不变：这些色一律只在运行期使用（无 constexpr 上下文）。
inline uint32_t kColGpu    = 0xFF6EE7A0;  // colGpu   默认绿   h150
inline uint32_t kColCpu    = 0xFF5AA9FF;  // colCpu   默认蓝   h213
inline uint32_t kColVram   = 0xFFB08CFF;  // colVram  默认紫   h265
inline uint32_t kColRam    = 0xFF45D6C8;  // colRam   默认青   h175
inline uint32_t kColBat    = 0xFFFFD166;  // colBat   默认黄   h 42
inline uint32_t kColFps    = 0xFFFF6B6B;  // colFps   默认红   h  0
inline uint32_t kColGraph  = 0xFF6EE7A0;  // colGraph 默认与 GPU 同色
constexpr uint32_t kColValue  = 0xFFFFFFFF;  // colValue 数值白
constexpr uint32_t kColDim    = 0xFFC2CEDA;  // colDim   标签灰（略收，避免抢数值的视线）
constexpr uint32_t kColUnit   = 0xFF9AA7B8;  // 单位后缀（比标签再淡一档，形成层级）
constexpr uint32_t kColLo     = 0xFFF7FAFF;  // colLo    AVG / 1% / 0.1% / 0.01%
/** 面板描边色：AppThemeState.getCurrentAccentArgb() 的默认值 */
inline uint32_t kColAccent = 0xFFA374FF;
/** 面板底色（深蓝黑）：与紫色强调色同色系，比纯黑更有"材质"感 */
inline uint32_t kColPanelRgb  = 0x000B0F16u;
inline std::string kPanelHex  = "0B0F16";
/** 面板内高光描边（1px，白 9%）：玻璃质感 */
constexpr uint32_t kColPanelEdge = 0x18FFFFFFu;
/** 面板外投影（两层：近处深、远处淡） */
constexpr uint32_t kColShadowNear = 0x1C000000u;
constexpr uint32_t kColShadowFar  = 0x10000000u;
/** 字距（em 比例，烘焙期写入 ImFontConfig::GlyphExtraSpacing） */
constexpr float kTrackingEm = 0.015f;

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

// ============================================================================
// 内置随机配色（默认开启，零配置）
//
// 每次启动进程时随机选一套配色：6 个指标色在色环上等距 60° 排布，只随机
// 「起始色相」与「槽位旋转」，饱和度/明度固定 —— 所以每次都不一样，但都好看、
// 都清晰（不会出现两个指标撞色或某个色看不清）。
//
// 关掉：MANGOHUD_FUSION_RANDOM_COLORS=0
// ============================================================================
namespace detail {

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/** HSL -> 0xFFRRGGBB */
inline uint32_t hslToRgb(float h, float s, float l)
{
    h = std::fmod(h, 360.0f);
    if (h < 0.0f)
        h += 360.0f;
    s = clampf(s, 0.0f, 1.0f);
    l = clampf(l, 0.0f, 1.0f);
    const float c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
    const float hp = h / 60.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (hp < 1.0f)      { r = c; g = x; }
    else if (hp < 2.0f) { r = x; g = c; }
    else if (hp < 3.0f) { g = c; b = x; }
    else if (hp < 4.0f) { g = x; b = c; }
    else if (hp < 5.0f) { r = x; b = c; }
    else                { r = c; b = x; }
    const float m = l - c * 0.5f;
    const uint32_t R = static_cast<uint32_t>(std::lround(clampf(r + m, 0.0f, 1.0f) * 255.0f));
    const uint32_t G = static_cast<uint32_t>(std::lround(clampf(g + m, 0.0f, 1.0f) * 255.0f));
    const uint32_t B = static_cast<uint32_t>(std::lround(clampf(b + m, 0.0f, 1.0f) * 255.0f));
    return 0xFF000000u | (R << 16) | (G << 8) | B;
}

} // namespace detail

/** 随机配色是否启用（默认启用；MANGOHUD_FUSION_RANDOM_COLORS=0 关闭） */
inline bool randomColorsEnabled()
{
    if (const char* e = std::getenv("MANGOHUD_FUSION_RANDOM_COLORS")) {
        if (*e && std::string(e) == "0")
            return false;
    }
    return true;
}

/**
 * 生成并应用一套随机配色。进程内只做一次（除非 force）。
 * 首次调用点：fusionThemeOptions() —— 也就是任何档位初始化的最前面，
 * 保证在颜色被消费之前就位。
 */
inline void randomizePalette(bool force = false)
{
    static bool done = false;
    if (done && !force)
        return;
    done = true;

    // 显式关闭：MANGOHUD_FUSION_RANDOM_COLORS=0（保持出厂默认色）
    if (!randomColorsEnabled())
        return;

    // 种子：时间 + 地址随机化 + 高精度时钟，保证每次启动都不同
    static int entropy = 0;
    uint64_t seed = static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    seed ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&entropy)) * 0x9E3779B97F4A7C15ull;
    std::mt19937 rng(static_cast<uint32_t>(seed ^ (seed >> 32)));

    std::uniform_real_distribution<float> base_hue(0.0f, 360.0f);
    std::uniform_real_distribution<float> jitter(-0.06f, 0.06f);
    std::uniform_int_distribution<int> slot_rot(0, 5);

    const float h0 = base_hue(rng);            // 起始色相
    const int rot = slot_rot(rng);             // 槽位旋转：同一色相不会永远落在同一个指标上
    const float S = 0.80f;                     // 饱和度：浓一些
    const float L = 0.68f;                     // 明度：比纯亮版深一档，又保证对比清楚

    auto hue_at = [&](int slot) { return h0 + 60.0f * static_cast<float>((slot + rot) % 6); };
    auto shade = [&](int slot) {
        return detail::hslToRgb(hue_at(slot), S, detail::clampf(L + jitter(rng), 0.60f, 0.76f));
    };

    kColGpu  = shade(0);
    kColCpu  = shade(1);
    kColVram = shade(2);
    kColRam  = shade(3);
    kColBat  = shade(4);
    kColFps  = shade(5);
    kColGraph = kColGpu;                       // 折线图与 GPU 同色（沿用原设计）

    kColAccent = detail::hslToRgb(h0 + 30.0f, 0.60f, 0.66f);

    // 面板底色：同色系极暗调（L≈0.07），整体协调但不会偏色到影响读字
    kColPanelRgb = detail::hslToRgb(h0, 0.28f, 0.07f) & 0x00FFFFFFu;
    char hexbuf[16];
    std::snprintf(hexbuf, sizeof(hexbuf), "%06X", kColPanelRgb);
    kPanelHex = hexbuf;
}

/**
 * 把当前这套（随机）配色强行写进 HUD 的颜色参数 —— 标准 HUD 也一起吃。
 *
 * 调用点：set_parameters_from_options() 的**最后**。
 * 因为它在所有显式颜色解析之后运行，所以连配置串里写死的
 * gpu_color=... / fps_color=... 也会被本次启动的随机色覆盖。
 * MANGOHUD_FUSION_RANDOM_COLORS=0 时不覆盖（完全尊重配置）。
 */
inline void applyRandomColorsToParams(overlay_params *p)
{
    if (!p || !randomColorsEnabled())
        return;

    randomizePalette();   // 幂等：进程内只随机一次

    const uint32_t white = kColValue;
    p->gpu_color       = kColGpu;
    p->cpu_color       = kColCpu;
    p->vram_color      = kColVram;
    p->ram_color       = kColRam;
    p->battery_color   = kColBat;
    p->engine_color    = kColFps;
    p->network_color   = kColCpu;
    p->frametime_color = kColGraph;
    p->io_color        = kColDim;
    p->wine_color      = kColDim;
    p->text_color      = white;

    // 数值恒为白：三段阈值色拉平（否则高负载会把数字染成红/黄）
    p->gpu_load_color = {white, white, white};
    p->cpu_load_color = {white, white, white};
    p->fps_color      = {white, white, white};

    // 面板底色跟着随机色系走（0xRRGGBB）
    p->background_color = kColPanelRgb & 0x00FFFFFFu;
}

/** 面板背景不透明度：默认 0（全透明）。用 MANGOHUD_FUSION_BG_ALPHA 可调回 0~1 */
inline float fusionBgAlpha()
{
    if (const char* e = std::getenv("MANGOHUD_FUSION_BG_ALPHA")) {
        if (*e) {
            const float v = static_cast<float>(std::atof(e));
            return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
    }
    return 0.0f;
}

/**
 * FusionHUD 主题 → MangoHud 配置键值表。
 * 交给 overlay_params.cpp 的 add_to_options() 走**正常解析路径**写入，
 * 保证不会被后续的配置解析覆盖。
 */
inline std::vector<std::pair<std::string, std::string>>
fusionThemeOptions(FusionSize size, float bg_opacity = kBgOpacityDefault,
                   float outline_intensity = kOutlineDefault) {
    // 内置随机配色：进程内第一次走到这里就定色（MANGOHUD_FUSION_RANDOM_COLORS=0 可关）
    randomizePalette();

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
    o.emplace_back("background_color", kPanelHex);
    // 默认全透明（详见 fusionBgAlpha()）；bg_opacity 参数保留仅为兼容旧调用
    (void)bg_opacity;
    o.emplace_back("background_alpha", std::to_string(fusionBgAlpha()));
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

/** FHUD-only：原版渲染已移除，外观/布局恒按 Fusion 处理 */
inline bool isFusionActive(const overlay_params&) {
    return true;
}

/**
 * 当前档位 —— 现在**恒定返回 FULL**。
 *
 * FusionHUD 只保留一种面板（即 preset 10 的 Full 布局）。保留这个函数是为了
 * 兼容旧的 fusion_tiles / fusion_pill / ... 配置：即使有人手写这些开关，
 * 渲染出来的仍然是同一套布局，不会出现"另一套面板没人维护"的情况。
 * 想恢复多布局：把下面改成按 enabled[] 判断即可（原实现见 git 历史）。
 */
inline FusionSize currentFusionSize(const overlay_params& p) {
    (void)p;
    return FusionSize::FULL;
}

} // namespace fusionhud
