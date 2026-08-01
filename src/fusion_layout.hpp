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
#include <cmath>
#include <cstdint>

namespace fusionhud {

// ================================================================
// FusionHUD color palette (packed ARGB, matching FusionHudView.kt)
// ================================================================
enum FusionColors : uint32_t {
    kColGpu         = 0xFF5EE08A,
    kColCpu         = 0xFF58A6FF,
    kColVram        = 0xFFC98BFF,
    kColRam         = 0xFFFF7BC0,
    kColBat         = 0xFFFFAB5E,
    kColFps         = 0xFFFF6B6B,
    kColGraph       = 0xFF5EE08A,
    kColValue       = 0xFFF2F5F9,
    kColDim         = 0xFF9AA4B2,
    kColLo          = 0xFFE4E8EE,
    kColBg          = 0xCC1A1D24,
    kColOutline     = 0x66222B3E,
};

// ================================================================
// 5 layout presets
// ================================================================
enum class FusionPreset {
    FULL     = 0,
    TILES    = 1,
    PILL     = 2,
    MINIMAL  = 3,
    MEGA     = 4,
};

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

    switch (static_cast<FusionPreset>(preset)) {
    case FusionPreset::FULL:
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

    // Apply FusionHUD color scheme
    params->gpu_color   = kColGpu;
    params->cpu_color   = kColCpu;
    params->vram_color  = kColVram;
    params->ram_color   = kColRam;
    params->battery_color = kColBat;
    params->text_color  = kColValue;
    params->engine_color = kColDim;
    params->wine_color  = kColDim;
    params->background_color = kColBg;
    params->background_alpha = 0.8f;
    params->round_corners    = 12.0f;
    params->text_outline_color = kColOutline;
    params->text_outline_thickness = 1.5f;
    params->font_size     = 14.0f;
    params->font_scale    = 1.0f;
    params->alpha         = 1.0f;
}

} // namespace fusionhud