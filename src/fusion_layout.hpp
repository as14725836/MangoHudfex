/**
 * fusion_layout.hpp — FusionHUD 5-mode layout system for MangoHudfex
 *
 * Adds FULL / TILES / PILL / MINIMAL / MEGA presets to MangoHud's config.
 *
 * Usage:
 *   1. Copy this file into MangoHudfex/src/
 *   2. Add these entries to overlay_params.h OVERLAY_PARAMS macro:
 *        OVERLAY_PARAM_BOOL(fusion_full)
 *        OVERLAY_PARAM_BOOL(fusion_tiles)
 *        OVERLAY_PARAM_BOOL(fusion_pill)
 *        OVERLAY_PARAM_BOOL(fusion_minimal)
 *        OVERLAY_PARAM_BOOL(fusion_mega)
 *   3. In overlay_params.cpp presets(), add preset cases that call
 *      fusionhud::applyPreset(params, preset_id)
 *   4. In hud_elements.cpp, use fusionhud::layouter to position elements
 *
 * Copyright (C) 2024  The FusionHUD-VK Authors
 * SPDX-License-Identifier: GPL-3.0
 * Based on FusionHUD by The412Banner (GPL-3.0)
 */

#pragma once
#include "overlay_params.h"
#include "fusion_theme.hpp"
#include <cmath>
#include <cstdint>
#include <string>

namespace fusionhud {

// ================================================================
// 5 layout presets —— 直接复用 fusion_theme.hpp 的 FusionSize，
// 避免两处各定义一份枚举/调色板而漂移。
// ================================================================
using FusionPreset = FusionSize;

/**
 * Apply a FusionHUD layout preset to MangoHud config.
 * Call this from overlay_params.cpp presets().
 *
 * @param params    MangoHud overlay_params to modify
 * @param preset    FusionHUD preset ID (0-4)
 * @param inherit   Whether to inherit existing params (MangoHud convention)
 */
inline void applyPreset(overlay_params* params, int preset, bool inherit = false) {
    if (!params) return;

    // Reset all FusionHUD flags
    params->enabled[OVERLAY_PARAM_ENABLED_fusion_full]    = false;
    params->enabled[OVERLAY_PARAM_ENABLED_fusion_tiles]   = false;
    params->enabled[OVERLAY_PARAM_ENABLED_fusion_pill]    = false;
    params->enabled[OVERLAY_PARAM_ENABLED_fusion_minimal] = false;
    params->enabled[OVERLAY_PARAM_ENABLED_fusion_mega]    = false;

    switch (static_cast<FusionSize>(preset - kPresetBase)) {
    case FusionSize::FULL:
        // Standard chip set: FPS+graph+0.01%low, GPU, GPU-temp, CPU,
        // VRAM, RAM, Power, Temp, Battery, GPU model, Engine, Clock
        params->enabled[OVERLAY_PARAM_ENABLED_fusion_full] = true;
        params->enabled[OVERLAY_PARAM_ENABLED_fps]         = true;
        params->enabled[OVERLAY_PARAM_ENABLED_frame_timing] = true;
        params->enabled[OVERLAY_PARAM_ENABLED_gpu_stats]   = true;
        params->enabled[OVERLAY_PARAM_ENABLED_gpu_temp]    = true;
        params->enabled[OVERLAY_PARAM_ENABLED_cpu_stats]   = true;
        params->enabled[OVERLAY_PARAM_ENABLED_vram]        = true;
        params->enabled[OVERLAY_PARAM_ENABLED_ram]         = true;
        params->enabled[OVERLAY_PARAM_ENABLED_battery]     = true;
        params->enabled[OVERLAY_PARAM_ENABLED_gpu_name]    = true;
        params->enabled[OVERLAY_PARAM_ENABLED_time]        = true;
        params->enabled[OVERLAY_PARAM_ENABLED_engine_version] = true;
        params->enabled[OVERLAY_PARAM_ENABLED_legacy_layout]  = false;
        params->enabled[OVERLAY_PARAM_ENABLED_hud_compact]    = false;
        // Spacing: standard
        params->cellpadding_y = 4.0f;
        break;

    case FusionPreset::TILES:
        // Same chip set as Full, tiled layout
        params->enabled[OVERLAY_PARAM_ENABLED_fusion_tiles]  = true;
        params->enabled[OVERLAY_PARAM_ENABLED_fps]           = true;
        params->enabled[OVERLAY_PARAM_ENABLED_frame_timing]  = true;
        params->enabled[OVERLAY_PARAM_ENABLED_gpu_stats]     = true;
        params->enabled[OVERLAY_PARAM_ENABLED_gpu_temp]      = true;
        params->enabled[OVERLAY_PARAM_ENABLED_cpu_stats]     = true;
        params->enabled[OVERLAY_PARAM_ENABLED_vram]          = true;
        params->enabled[OVERLAY_PARAM_ENABLED_ram]           = true;
        params->enabled[OVERLAY_PARAM_ENABLED_battery]       = true;
        params->enabled[OVERLAY_PARAM_ENABLED_gpu_name]      = true;
        params->enabled[OVERLAY_PARAM_ENABLED_time]          = true;
        params->enabled[OVERLAY_PARAM_ENABLED_engine_version] = true;
        params->enabled[OVERLAY_PARAM_ENABLED_legacy_layout] = false;
        params->enabled[OVERLAY_PARAM_ENABLED_hud_compact]   = false;
        params->cellpadding_y = 6.0f;
        break;

    case FusionPreset::PILL:
        // Compact pill: FPS+graph, GPU, CPU, VRAM, Battery, GPU model, Clock
        params->enabled[OVERLAY_PARAM_ENABLED_fusion_pill]   = true;
        params->enabled[OVERLAY_PARAM_ENABLED_fps]           = true;
        params->enabled[OVERLAY_PARAM_ENABLED_gpu_stats]     = true;
        params->enabled[OVERLAY_PARAM_ENABLED_gpu_temp]      = true;
        params->enabled[OVERLAY_PARAM_ENABLED_cpu_stats]     = true;
        params->enabled[OVERLAY_PARAM_ENABLED_vram]          = true;
        params->enabled[OVERLAY_PARAM_ENABLED_battery]       = true;
        params->enabled[OVERLAY_PARAM_ENABLED_gpu_name]      = true;
        params->enabled[OVERLAY_PARAM_ENABLED_time]          = true;
        params->enabled[OVERLAY_PARAM_ENABLED_engine_version] = false;
        params->enabled[OVERLAY_PARAM_ENABLED_ram]            = false;
        params->enabled[OVERLAY_PARAM_ENABLED_frame_timing]   = true; // graph only
        params->enabled[OVERLAY_PARAM_ENABLED_hud_compact]    = true;
        params->enabled[OVERLAY_PARAM_ENABLED_legacy_layout]  = false;
        params->cellpadding_y = 2.0f;
        break;

    case FusionPreset::MINIMAL:
        // Just FPS + graph + 0.01% low + decimal + subtle Clock
        params->enabled[OVERLAY_PARAM_ENABLED_fusion_minimal] = true;
        params->enabled[OVERLAY_PARAM_ENABLED_fps]            = true;
        params->enabled[OVERLAY_PARAM_ENABLED_frame_timing]   = true;
        params->enabled[OVERLAY_PARAM_ENABLED_time]           = true;
        params->enabled[OVERLAY_PARAM_ENABLED_gpu_stats]      = false;
        params->enabled[OVERLAY_PARAM_ENABLED_cpu_stats]      = false;
        params->enabled[OVERLAY_PARAM_ENABLED_vram]           = false;
        params->enabled[OVERLAY_PARAM_ENABLED_ram]            = false;
        params->enabled[OVERLAY_PARAM_ENABLED_battery]        = false;
        params->enabled[OVERLAY_PARAM_ENABLED_gpu_name]       = false;
        params->enabled[OVERLAY_PARAM_ENABLED_engine_version] = false;
        params->enabled[OVERLAY_PARAM_ENABLED_hud_compact]    = true;
        params->enabled[OVERLAY_PARAM_ENABLED_legacy_layout]  = false;
        params->cellpadding_y = 0.0f;
        break;

    case FusionPreset::MEGA:
        // Everything: Full + per-core CPU, Swap, Network, Resolution,
        // Proton, Wrapper, DX version, Session length
        params->enabled[OVERLAY_PARAM_ENABLED_fusion_mega]    = true;
        params->enabled[OVERLAY_PARAM_ENABLED_fps]            = true;
        params->enabled[OVERLAY_PARAM_ENABLED_frame_timing]   = true;
        params->enabled[OVERLAY_PARAM_ENABLED_gpu_stats]      = true;
        params->enabled[OVERLAY_PARAM_ENABLED_gpu_temp]       = true;
        params->enabled[OVERLAY_PARAM_ENABLED_cpu_stats]      = true;
        params->enabled[OVERLAY_PARAM_ENABLED_core_load]      = true;
        params->enabled[OVERLAY_PARAM_ENABLED_core_bars]      = true;
        params->enabled[OVERLAY_PARAM_ENABLED_vram]           = true;
        params->enabled[OVERLAY_PARAM_ENABLED_ram]            = true;
        params->enabled[OVERLAY_PARAM_ENABLED_swap]           = true;
        params->enabled[OVERLAY_PARAM_ENABLED_battery]        = true;
        params->enabled[OVERLAY_PARAM_ENABLED_battery_watt]   = true;
        params->enabled[OVERLAY_PARAM_ENABLED_gpu_name]       = true;
        params->enabled[OVERLAY_PARAM_ENABLED_time]           = true;
        params->enabled[OVERLAY_PARAM_ENABLED_engine_version] = true;
        params->enabled[OVERLAY_PARAM_ENABLED_resolution]     = true;
        params->enabled[OVERLAY_PARAM_ENABLED_wine]           = true;
        params->enabled[OVERLAY_PARAM_ENABLED_duration]       = true;
        params->enabled[OVERLAY_PARAM_ENABLED_legacy_layout]  = false;
        params->enabled[OVERLAY_PARAM_ENABLED_hud_compact]    = false;
        params->cellpadding_y = 3.0f;
        break;
    }

    // ---- FusionHUD 主题（唯一事实源 fusion_theme.hpp）----
    // 这里直接写 params：本函数用于“强制套用”，可在配置解析之后再次调用。
    params->gpu_color            = kColGpu;
    params->cpu_color            = kColCpu;
    params->vram_color           = kColVram;
    params->ram_color            = kColRam;
    params->battery_color        = kColBat;
    params->engine_color         = kColFps;   // FPS / 引擎行
    params->network_color        = kColFps;   // Mega 的 NET 行
    params->frametime_color      = kColGraph; // 帧时间图
    params->wine_color           = kColDim;
    params->io_color             = kColDim;
    params->text_color           = kColValue;
    params->horizontal_separator_color = kColLo;

    // 数值恒为白：三段阈值色统一
    const unsigned white = kColValue;
    params->gpu_load_color = { white, white, white };
    params->cpu_load_color = { white, white, white };
    params->fps_color      = { white, white, white };

    // 面板：纯黑底 + 圆角 8（上游 sp(8f)），描边用强调色
    params->background_color = 0x000000;
    params->background_alpha = kBgOpacityDefault;
    params->alpha            = 1.0f;
    params->round_corners    = kBgRadiusSp;

    // 上游没有文字描边，只有面板边框
    params->enabled[OVERLAY_PARAM_ENABLED_text_outline] = false;

    const FusionSize sz = static_cast<FusionSize>(preset - kPresetBase);
    params->font_size  = fusionFontSize(sz);
    params->font_scale = 1.0f;
    params->cellpadding_y = fusionCellPaddingY(sz);
}

} // namespace fusionhud