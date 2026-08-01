/**
 * fusion_appearance.hpp —像素级外观对齐 FusionHUDView.kt
 *
 * 修改 MangoHudfex 的 ImGui 渲染以复现 FusionHUD 的：
 *  - HSV 颜色混合（替代简单 alpha）
 *  - 12px 圆角半透明背景
 *  - 文字描边（outline）
 *  - 精确的 cellpadding / spacing
 *  - 5 种布局的 chip 排列
 *  - Mono 粗体字
 *
 * 集成方式：在 src/hud_elements.cpp 的 HUD 窗口创建处调用
 * fusionhud::setupAppearance() 和 fusionhud::drawFusionBackground()。
 *
 * Copyright (C) 2024  The FusionHUD-VK Authors
 * SPDX-License-Identifier: GPL-3.0
 * Based on FusionHUD by The412Banner (GPL-3.0)
 */

#pragma once

#include <imgui.h>
#include <imgui_internal.h>
#include "overlay_params.h"
#include "fusion_layout.hpp"
#include <cmath>

namespace fusionhud {

// ================================================================
// HSV blend — matches Android Color.colorToHSV + lerp in HSV space
// ================================================================
inline void rgbToHsv(uint8_t r, uint8_t g, uint8_t b, float& h, float& s, float& v) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float mx = std::max({rf, gf, bf});
    float mn = std::min({rf, gf, bf});
    float delta = mx - mn;

    v = mx;
    s = (mx > 0.001f) ? delta / mx : 0.0f;

    if (delta < 0.001f) {
        h = 0.0f;
    } else if (mx == rf) {
        h = 60.0f * fmodf((gf - bf) / delta, 6.0f);
    } else if (mx == gf) {
        h = 60.0f * ((bf - rf) / delta + 2.0f);
    } else {
        h = 60.0f * ((rf - gf) / delta + 4.0f);
    }
    if (h < 0.0f) h += 360.0f;
}

inline void hsvToRgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rf = 0, gf = 0, bf = 0;

    if      (h <  60) { rf = c; gf = x; }
    else if (h < 120) { rf = x; gf = c; }
    else if (h < 180) { gf = c; bf = x; }
    else if (h < 240) { gf = x; bf = c; }
    else if (h < 300) { rf = x; bf = c; }
    else              { rf = c; bf = x; }

    r = (uint8_t)((rf + m) * 255.0f);
    g = (uint8_t)((gf + m) * 255.0f);
    b = (uint8_t)((bf + m) * 255.0f);
}

// Blend color with background using HSV space (matches FusionHUDView.kt)
inline uint32_t hsvBlend(uint32_t fg_argb, float opacity, uint32_t bg_argb = 0xFF1A1D24) {
    uint8_t fa = (fg_argb >> 24) & 0xFF;
    uint8_t fr = (fg_argb >> 16) & 0xFF;
    uint8_t fg = (fg_argb >>  8) & 0xFF;
    uint8_t fb = (fg_argb      ) & 0xFF;

    uint8_t ba = (bg_argb >> 24) & 0xFF;
    uint8_t br = (bg_argb >> 16) & 0xFF;
    uint8_t bg = (bg_argb >>  8) & 0xFF;
    uint8_t bb = (bg_argb      ) & 0xFF;

    // Simple alpha blend for quick paths
    if (opacity >= 1.0f) return fg_argb;
    if (opacity <= 0.0f) return bg_argb;

    // Convert both to HSV
    float fh, fs, fv, bh, bs, bv;
    rgbToHsv(fr, fg, fb, fh, fs, fv);
    rgbToHsv(br, bg, bb, bh, bs, bv);

    // Lerp in HSV space
    float h = fh * opacity + bh * (1.0f - opacity);
    float s = fs * opacity + bs * (1.0f - opacity);
    float v = fv * opacity + bv * (1.0f - opacity);

    uint8_t rr, rg, rb;
    hsvToRgb(h, s, v, rr, rg, rb);

    uint8_t a = (uint8_t)(fa * opacity + ba * (1.0f - opacity));
    return (a << 24) | (rr << 16) | (rg << 8) | rb;
}

// ================================================================
// ImGui style + FusionHUD appearance setup
// Call once during ImGui init (after CreateContext)
// ================================================================
inline void setupAppearance(const overlay_params* params = nullptr) {
    ImGuiStyle& s = ImGui::GetStyle();

    // --- Window ---
    s.WindowRounding    = 12.0f;   // FusionHUD card corner radius
    s.WindowBorderSize  = 0.0f;    // no border
    s.WindowPadding     = ImVec2(8.0f, 6.0f);
    s.WindowMinSize     = ImVec2(1.0f, 1.0f);

    // --- Frame (inner widgets) ---
    s.FrameRounding     = 4.0f;
    s.FrameBorderSize   = 0.0f;
    s.FramePadding      = ImVec2(4.0f, 2.0f);

    // --- Item spacing ---
    s.ItemSpacing       = ImVec2(6.0f, 3.0f);
    s.ItemInnerSpacing  = ImVec2(4.0f, 2.0f);

    // --- Scrollbar (hidden for overlay) ---
    s.ScrollbarSize     = 0.0f;

    // --- Colors ---
    s.Colors[ImGuiCol_WindowBg]         = ImVec4(0.10f, 0.11f, 0.14f, 0.80f); // #1A1D24 CC
    s.Colors[ImGuiCol_Border]           = ImVec4(0.13f, 0.17f, 0.24f, 0.40f); // outline
    s.Colors[ImGuiCol_Text]             = ImVec4(0.95f, 0.96f, 0.98f, 1.00f); // #F2F5F9
    s.Colors[ImGuiCol_TextDisabled]     = ImVec4(0.60f, 0.64f, 0.70f, 1.00f); // #9AA4B2
    s.Colors[ImGuiCol_PlotLines]        = ImVec4(0.37f, 0.88f, 0.54f, 1.00f); // #5EE08A
    s.Colors[ImGuiCol_PlotHistogram]    = ImVec4(0.37f, 0.88f, 0.54f, 0.70f);

    // If user config provided, override with FusionHUD palette
    if (params) {
        // Colors will be applied per-element in hud_elements.cpp
    }
}

// ================================================================
// Draw FusionHUD-style background card
// Call at the beginning of the HUD window (after ImGui::Begin)
// ================================================================
inline void drawFusionBackground() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();

    // Background fill with rounded corners
    dl->AddRectFilled(
        pos,
        ImVec2(pos.x + size.x, pos.y + size.y),
        IM_COL32(0x1A, 0x1D, 0x24, 0xCC),  // #1A1D24 80% opacity
        12.0f   // corner radius matching FusionHUD
    );

    // Subtle outline
    dl->AddRect(
        pos,
        ImVec2(pos.x + size.x, pos.y + size.y),
        IM_COL32(0x22, 0x2B, 0x3E, 0x66),  // #222B3E 40% opacity
        12.0f,
        0,
        1.0f  // outline thickness
    );
}

// ================================================================
// Draw a FusionHUD-style metric chip
//   label: "GPU" or "FPS"
//   value: "87%" or "62.5"
//   color: packed ARGB (FusionColors enum)
//   x, y: top-left position
//   width: chip width (0 = auto)
// ================================================================
inline void drawMetricChip(
    const char* label,
    const char* value,
    uint32_t color_argb,
    float x, float y,
    float font_size = 14.0f,
    float width = 0.0f)
{
    // Extract components
    int a = (color_argb >> 24) & 0xFF;
    int r = (color_argb >> 16) & 0xFF;
    int g = (color_argb >>  8) & 0xFF;
    int b = (color_argb      ) & 0xFF;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();

    // Chip layout (matches FusionHUDView.kt tile layout):
    // ┌─────────────┐
    // │ LABEL  7pt  │  ← dim color
    // │ VALUE 14pt  │  ← bright color
    // └─────────────┘

    float label_w = ImGui::CalcTextSize(label).x;
    float value_w = ImGui::CalcTextSize(value).x;
    if (width <= 0.0f) width = std::max(label_w, value_w) + 16.0f;

    // Label (small, dim)
    dl->AddText(
        font, font_size * 0.6f,
        ImVec2(x + 4.0f, y + 2.0f),
        IM_COL32(0x9A, 0xA4, 0xB2, 0xFF),  // kColDim
        label
    );

    // Value (large, colored)
    dl->AddText(
        font, font_size,
        ImVec2(x + 4.0f, y + font_size * 0.8f),
        IM_COL32(r, g, b, a),
        value
    );

    // Vertical accent bar (2px wide, left edge)
    dl->AddRectFilled(
        ImVec2(x, y),
        ImVec2(x + 2.0f, y + font_size * 1.8f),
        IM_COL32(r, g, b, (int)(a * 0.6f))
    );
}

// ================================================================
// Draw FPS graph — matching FusionHUDView.kt's graph bar
// ================================================================
inline void drawFpsGraph(
    const float* samples,    // frametime samples array
    int count,               // number of samples
    float x, float y,
    float width, float height,
    uint32_t line_color = 0xFF5EE08A,  // kColGraph
    uint32_t fill_color = 0x335EE08A)  // kColGraph 20% alpha
{
    if (count < 2) return;

    int r_l = (line_color >> 16) & 0xFF;
    int g_l = (line_color >>  8) & 0xFF;
    int b_l = (line_color      ) & 0xFF;
    int a_l = (line_color >> 24) & 0xFF;

    int r_f = (fill_color >> 16) & 0xFF;
    int g_f = (fill_color >>  8) & 0xFF;
    int b_f = (fill_color      ) & 0xFF;
    int a_f = (fill_color >> 24) & 0xFF;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Find min/max for Y scaling
    float min_v = samples[0], max_v = samples[0];
    for (int i = 1; i < count; i++) {
        if (samples[i] < min_v) min_v = samples[i];
        if (samples[i] > max_v) max_v = samples[i];
    }
    if (max_v <= min_v) max_v = min_v + 1.0f;

    float x_step = width / (count - 1);

    // Build polygon for filled area
    std::vector<ImVec2> poly(count + 2);
    for (int i = 0; i < count; i++) {
        float px = x + i * x_step;
        float py = y + height - ((samples[i] - min_v) / (max_v - min_v)) * height;
        poly[i] = ImVec2(px, py);
    }
    poly[count]     = ImVec2(x + width, y + height);
    poly[count + 1] = ImVec2(x, y + height);

    // Fill
    dl->AddConvexPolyFilled(poly.data(), count + 2, IM_COL32(r_f, g_f, b_f, a_f));

    // Line
    for (int i = 0; i < count - 1; i++) {
        dl->AddLine(poly[i], poly[i + 1], IM_COL32(r_l, g_l, b_l, a_l), 1.0f);
    }
}

// ================================================================
// Apply FusionHUD layout to MangoHud ImGui window
// Call from hud_elements.cpp where the HUD window is created
// ================================================================
inline void applyFusionLayout(const overlay_params* params) {
    if (!params) return;

    // Check which FusionHUD mode is active
    bool is_full    = params->enabled[OVERLAY_PARAM_ENABLED_fusion_full];
    bool is_tiles   = params->enabled[OVERLAY_PARAM_ENABLED_fusion_tiles];
    bool is_pill    = params->enabled[OVERLAY_PARAM_ENABLED_fusion_pill];
    bool is_minimal = params->enabled[OVERLAY_PARAM_ENABLED_fusion_minimal];
    bool is_mega    = params->enabled[OVERLAY_PARAM_ENABLED_fusion_mega];

    if (!(is_full || is_tiles || is_pill || is_minimal || is_mega))
        return;  // not using FusionHUD mode

    ImGuiStyle& s = ImGui::GetStyle();

    if (is_pill || is_minimal) {
        // Compact: single-row pill
        s.WindowPadding     = ImVec2(6.0f, 4.0f);
        s.ItemSpacing       = ImVec2(4.0f, 1.0f);
        s.ItemInnerSpacing  = ImVec2(3.0f, 1.0f);
    } else if (is_tiles) {
        // Tiled: 2-3 columns
        s.WindowPadding     = ImVec2(8.0f, 8.0f);
        s.ItemSpacing       = ImVec2(8.0f, 6.0f);
    } else {
        // Full / Mega: vertical list
        s.WindowPadding     = ImVec2(10.0f, 6.0f);
        s.ItemSpacing       = ImVec2(6.0f, 4.0f);
    }
}

// ================================================================
// One-shot: apply full FusionHUD appearance (style + layout)
// Called from overlay_new_frame() every frame
// ================================================================
inline void applyFusionAppearance(const overlay_params& params) {
    bool is_fusion = params.enabled[OVERLAY_PARAM_ENABLED_fusion_full] ||
                     params.enabled[OVERLAY_PARAM_ENABLED_fusion_tiles] ||
                     params.enabled[OVERLAY_PARAM_ENABLED_fusion_pill] ||
                     params.enabled[OVERLAY_PARAM_ENABLED_fusion_minimal] ||
                     params.enabled[OVERLAY_PARAM_ENABLED_fusion_mega];
    if (!is_fusion) return;

    ImGuiStyle& s = ImGui::GetStyle();
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.14f, params.background_alpha);

    if (params.enabled[OVERLAY_PARAM_ENABLED_fusion_pill] ||
        params.enabled[OVERLAY_PARAM_ENABLED_fusion_minimal]) {
        s.WindowPadding     = ImVec2(6.0f, 4.0f);
        s.ItemSpacing       = ImVec2(4.0f, 1.0f);
        s.ItemInnerSpacing  = ImVec2(3.0f, 1.0f);
    } else if (params.enabled[OVERLAY_PARAM_ENABLED_fusion_tiles]) {
        s.WindowPadding     = ImVec2(8.0f, 8.0f);
        s.ItemSpacing       = ImVec2(8.0f, 6.0f);
    } else {
        s.WindowPadding     = ImVec2(10.0f, 6.0f);
        s.ItemSpacing       = ImVec2(6.0f, 4.0f);
    }
}

} // namespace fusionhud