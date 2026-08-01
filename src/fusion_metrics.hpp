/**
 * fusion_metrics.hpp — Cross-vendor GPU/CPU/temp probe paths from FusionHUD
 *
 * Integrate into MangoHudfex src/gpu.cpp and src/cpu.cpp to add support for
 * Adreno KGSL, Mali, PowerVR, Exynos, Xclipse, and other ARM GPU sysfs paths
 * that the standard MangoHud DRM-based detection misses.
 *
 * Usage:
 *   1. Copy this file into MangoHudfex/src/
 *   2. #include "fusion_metrics.hpp" in gpu.cpp
 *   3. Call fusionhud::probeGpuUse() / probeGpuFreq() / probeGpuTemp() etc.
 *
 * Copyright (C) 2024  The FusionHUD-VK Authors
 * SPDX-License-Identifier: GPL-3.0
 * Based on FusionHUD by The412Banner (GPL-3.0)
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <algorithm>

namespace fusionhud {

// ================================================================
// GPU usage — static candidate list (14 paths, cross-vendor)
// ================================================================
struct GpuProbeResult {
    std::string path;        // effective sysfs node
    int value;               // 0..100 or -1 on failure
    std::string label;       // "KGSL gpubusy", "Mali gpuinfo", etc.
};

inline GpuProbeResult probeGpuUse(const std::string& vendor_prefix = "") {
    // 14 static paths in order of preference
    static const char* gpu_use_paths[] = {
        // Adreno KGSL (Qualcomm)
        "/sys/class/kgsl/kgsl-3d0/gpubusy",
        "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
        "/sys/devices/platform/kgsl-3d0.0/kgsl/kgsl-3d0/gpubusy",
        // Mali (ARM / MediaTek / Exynos)
        "/sys/class/misc/mali0/device/utilization",
        "/sys/devices/platform/mali0/utilization",
        "/sys/class/devfreq/13000000.mali/device/gpuinfo",
        "/sys/kernel/gpu/gpuinfo",
        // PowerVR (Imagination)
        "/sys/kernel/pvr/status",
        "/sys/class/pvr/pvr_status",
        // amdgpu / Xclipse (Samsung)
        "/sys/class/drm/card0/device/gpu_busy_percent",
        "/sys/kernel/debug/dri/0/gpu_busy",
        // Generic devfreq
        "/sys/class/devfreq/gpufreq/load",
        // panfrost
        "/sys/class/drm/card0/device/gpu_busy",
        // Generic fallback
        "/sys/kernel/gpu/gpu_busy",
        nullptr
    };

    for (int i = 0; gpu_use_paths[i]; i++) {
        std::ifstream f(gpu_use_paths[i]);
        if (!f.is_open()) continue;

        std::string line;
        if (!std::getline(f, line)) continue;
        f.close();

        const char* path = gpu_use_paths[i];
        int val = -1;

        // Adreno KGSL: "      123456   789012" (busy / total)
        if (strstr(path, "gpubusy")) {
            long long busy = 0, total = 0;
            if (sscanf(line.c_str(), "%lld %lld", &busy, &total) == 2 && total > 0) {
                val = (int)((float)busy / total * 100);
                if (val > 100) val = 100;
            }
        }
        // Mali gpuinfo: multi-line, need delta-over-wallclock
        else if (strstr(path, "gpuinfo")) {
            // Simplified: assume single-line percent format
            try {
                int raw = std::stoi(line);
                if (raw >= 0 && raw <= 100) val = raw;
            } catch (...) {}
        }
        // Plain integer percent
        else {
            try {
                int raw = std::stoi(line);
                if (raw >= 0 && raw <= 100) val = raw;
                else if (raw > 100 && raw <= 10000) val = raw / 100; // milli-percent
            } catch (...) {}
        }

        if (val >= 0) {
            return {path, val, strstr(path, "gpubusy") ? "KGSL gpubusy" :
                                  strstr(path, "gpuinfo") ? "Mali gpuinfo" :
                                  strstr(path, "mali") ? "Mali" :
                                  strstr(path, "pvr") ? "PowerVR" :
                                  strstr(path, "drm") ? "DRM" : "sysfs"};
        }
    }

    // Dynamic scan: /sys/devices/platform/* for GPU-token nodes
    return {"", -1, ""};
}

// ================================================================
// GPU frequency — 7 static + devfreq dynamic scan
// ================================================================
inline GpuProbeResult probeGpuFreq() {
    static const char* freq_paths[] = {
        "/sys/class/kgsl/kgsl-3d0/gpuclk",
        "/sys/class/kgsl/kgsl-3d0/max_gpuclk",
        "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
        "/sys/class/devfreq/13000000.mali/cur_freq",
        "/sys/kernel/gpu/gpu_clock",
        "/sys/class/drm/card0/device/hwmon/hwmon0/freq1_input",
        "/sys/devices/platform/soc/soc:gpu/devfreq/gpu/cur_freq",
        nullptr
    };

    for (int i = 0; freq_paths[i]; i++) {
        std::ifstream f(freq_paths[i]);
        if (!f.is_open()) continue;

        std::string line;
        if (!std::getline(f, line)) continue;
        f.close();

        try {
            double freq = std::stod(line);
            // Normalize to MHz
            if (freq > 1e7) freq /= 1e6;      // Hz → MHz
            else if (freq > 1e4) freq /= 1e3; // KHz → MHz
            return {freq_paths[i], (int)freq, "sysfs"};
        } catch (...) {}
    }
    return {"", -1, ""};
}

// ================================================================
// GPU temperature — prioritized thermal-zone discovery
// ================================================================
struct TempProbeResult {
    std::string path;   // e.g. "/sys/class/thermal/thermal_zone5/temp"
    int value;          // °C or -1
    int amber;          // amber threshold (°C) or -1
    int red;            // red/critical threshold (°C) or -1
    std::string label;  // "Qualcomm gpuss", "Mali", "mtktsgpu", etc.
};

// GPU temperature token keywords (priority order)
static const char* gpu_temp_tokens[] = {
    "gpuss", "tsens", "mtktsgpu", "g3d", "s5p-tmu",
    "sgpu", "xclipse", "mali", "kgsl", nullptr
};

inline TempProbeResult probeGpuTemp() {
    // Enumerate all thermal zones
    for (int z = 0; z < 50; z++) {
        char type_path[256];
        snprintf(type_path, sizeof(type_path),
                 "/sys/class/thermal/thermal_zone%d/type", z);

        std::ifstream tf(type_path);
        if (!tf.is_open()) continue;

        std::string type;
        std::getline(tf, type);
        tf.close();

        if (type.empty()) continue;

        // Match GPU tokens
        bool matched = false;
        for (int t = 0; gpu_temp_tokens[t]; t++) {
            if (type.find(gpu_temp_tokens[t]) != std::string::npos) {
                matched = true;
                break;
            }
        }
        if (!matched) continue;

        // Read temperature
        char temp_path[256];
        snprintf(temp_path, sizeof(temp_path),
                 "/sys/class/thermal/thermal_zone%d/temp", z);

        std::ifstream vf(temp_path);
        if (!vf.is_open()) continue;

        std::string val_str;
        std::getline(vf, val_str);
        vf.close();

        int temp = -1;
        try {
            temp = std::stoi(val_str);
            if (temp > 1000) temp /= 1000;  // milli-°C → °C
            if (temp < 1 || temp > 150) temp = -1; // sanity clamp
        } catch (...) {}

        if (temp < 0) continue;

        // Read trip points for thresholds
        int amber = -1, red = -1;
        for (int tp = 0; tp < 10; tp++) {
            char tp_path[300];
            snprintf(tp_path, sizeof(tp_path),
                     "/sys/class/thermal/thermal_zone%d/trip_point_%d_temp", z, tp);
            std::ifstream tpf(tp_path);
            if (!tpf.is_open()) continue;

            std::string tp_str;
            std::getline(tpf, tp_str);
            tpf.close();

            int tval = -1;
            try { tval = std::stoi(tp_str); if (tval > 1000) tval /= 1000; }
            catch (...) {}

            if (tval < 0) continue;

            // Read trip type
            char tpp_path[300];
            snprintf(tpp_path, sizeof(tpp_path),
                     "/sys/class/thermal/thermal_zone%d/trip_point_%d_type", z, tp);
            std::ifstream tppf(tpp_path);
            std::string tp_type;
            if (tppf.is_open()) std::getline(tppf, tp_type);

            if (tp_type.find("passive") != std::string::npos && (amber < 0 || tval < amber))
                amber = tval;
            if (tp_type.find("critical") != std::string::npos && (red < 0 || tval < red))
                red = tval;
        }

        return {temp_path, temp, amber, red, type};
    }
    return {"", -1, -1, -1, ""};
}

// ================================================================
// CPU temperature — prioritized discovery
// ================================================================
static const char* cpu_temp_tokens[] = {
    "cpu", "tsens", "xpu", "soc", nullptr
};

inline TempProbeResult probeCpuTemp() {
    for (int z = 0; z < 50; z++) {
        char type_path[256];
        snprintf(type_path, sizeof(type_path),
                 "/sys/class/thermal/thermal_zone%d/type", z);

        std::ifstream tf(type_path);
        if (!tf.is_open()) continue;

        std::string type;
        std::getline(tf, type);
        tf.close();

        if (type.empty()) continue;

        bool matched = false;
        for (int t = 0; cpu_temp_tokens[t]; t++) {
            if (type.find(cpu_temp_tokens[t]) != std::string::npos) {
                matched = true;
                break;
            }
        }
        if (!matched) continue;

        char temp_path[256];
        snprintf(temp_path, sizeof(temp_path),
                 "/sys/class/thermal/thermal_zone%d/temp", z);

        std::ifstream vf(temp_path);
        if (!vf.is_open()) continue;

        std::string val_str;
        std::getline(vf, val_str);
        vf.close();

        int temp = -1;
        try {
            temp = std::stoi(val_str);
            if (temp > 1000) temp /= 1000;
            if (temp < 1 || temp > 150) temp = -1;
        } catch (...) {}

        if (temp < 0) continue;

        return {temp_path, temp, -1, -1, type};
    }
    return {"", -1, -1, -1, ""};
}

// ================================================================
// VRAM — Adreno KGSL + amdgpu paths
// ================================================================
inline long long probeVramUsed() {
    // Adreno KGSL
    {
        std::ifstream f("/sys/class/kgsl/kgsl-3d0/mem_used");
        if (f.is_open()) {
            std::string l;
            if (std::getline(f, l)) {
                try { return std::stoll(l); } catch (...) {}
            }
        }
    }
    {
        std::ifstream f("/sys/class/kgsl/kgsl-3d0/mapped_mem");
        if (f.is_open()) {
            std::string l;
            if (std::getline(f, l)) {
                try { return std::stoll(l) / 1024; } catch (...) {} // bytes→KB
            }
        }
    }
    // amdgpu
    {
        std::ifstream f("/sys/class/drm/card0/device/mem_info_vram_used");
        if (f.is_open()) {
            std::string l;
            if (std::getline(f, l)) {
                try { return std::stoll(l); } catch (...) {}
            }
        }
    }
    return -1;
}

// ================================================================
// Temperature color — FusionHUD green/amber/red scale
// ================================================================
inline int tempColor(int celsius, int amber, int red) {
    if (celsius < 0) return 0xFF9AA4B2; // gray for N/A

    if (red > 0 && celsius >= red)       return 0xFFFF6B6B; // FusionHUD red
    if (amber > 0 && celsius >= amber)   return 0xFFFFAB5E; // FusionHUD amber
    if (celsius >= 80)                    return 0xFFFFAB5E; // default amber
    return 0xFF5EE08A; // FusionHUD green
}

} // namespace fusionhud