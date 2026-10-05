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
#include <cstring>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <dirent.h>
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
    (void)vendor_prefix; // 预留参数，当前实现按静态优先级表探测
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

// ================================================================
// gpu.cpp 实际调用的 API
// 说明：这些函数原先由 CI workflow 用 heredoc 在构建时注入，属于“影子代码”；
//       现内置到源码，CI 注入逻辑可删除，构建结果可复现。
//   gpuProbePaths / readGpuUtilization / readGpuTemperature /
//   readGpuFrequency / detectVram
// ================================================================
struct GpuPaths {
    std::string use;   // GPU 占用率节点（sysfs）
    std::string freq;  // GPU 频率节点（sysfs）
};

// 返回第一个存在的 sysfs 节点；不存在返回空串
inline std::string fhFirstExisting(const char* const* paths) {
    for (int i = 0; paths[i]; ++i) {
        std::ifstream f(paths[i]);
        if (f.good()) return std::string(paths[i]);
    }
    return std::string();
}

// 探测一次即可，调用方（gpu.cpp 后台线程）应在循环外调用本函数
inline GpuPaths gpuProbePaths() {
    static const char* use_paths[] = {
        // 顺序很重要：gpu_busy_percentage 是"瞬时"占用率；
        // gpubusy 是累计(忙/总)计数比值 = 开机以来的平均值，数值几乎不动，
        // 所以瞬时值优先，避免 HUD 上显示一个恒定不变的数字。
        "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",    // Adreno: "37 %"
        "/sys/class/kgsl/kgsl-3d0/gpubusy",                // Adreno KGSL: "<busy> <total>"
        "/sys/class/kgsl/kgsl-3d0/devfreq/gpu_load",
        "/sys/class/misc/mali0/device/utilization",        // Mali
        "/sys/class/devfreq/13000000.mali/device/gpuinfo",
        "/sys/class/drm/card0/device/gpu_busy_percent",    // Xclipse / amdgpu
        "/sys/class/devfreq/gpufreq/load",                 // 通用 devfreq
        nullptr
    };
    static const char* freq_paths[] = {
        "/sys/class/kgsl/kgsl-3d0/gpuclk",                 // Hz
        "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
        "/sys/class/devfreq/13000000.mali/cur_freq",
        "/sys/class/drm/card0/device/hwmon/hwmon0/freq1_input",
        nullptr
    };
    GpuPaths p;
    p.use  = fhFirstExisting(use_paths);
    p.freq = fhFirstExisting(freq_paths);
    return p;
}

// 占用率：0..100，失败返回 -1
inline int readGpuUtilization(const GpuPaths& p) {
    if (p.use.empty()) return -1;
    std::ifstream f(p.use);
    if (!f.is_open()) return -1;
    std::string line;
    if (!std::getline(f, line)) return -1;

    // 格式 1： "<busy> <total>"（KGSL gpubusy / Mali gpuinfo）
    {
        std::istringstream is(line);
        long long busy = 0, total = 0;
        std::string rest;
        if ((is >> busy >> total) && total > 0 && !(is >> rest)) {
            long long v = busy * 100 / total;
            return (int)std::clamp<long long>(v, 0, 100);
        }
    }
    // 格式 2： 单值 —— 可能带 '%'、可能是 0..1 小数、可能是 milli-percent
    {
        std::string s = line;
        while (!s.empty() &&
               (std::isspace(static_cast<unsigned char>(s.back())) || s.back() == '%'))
            s.pop_back();
        if (s.empty()) return -1;
        try {
            double d = std::stod(s);
            if (line.find('%') == std::string::npos && d > 0.0 && d <= 1.0)
                d *= 100.0;                       // 0..1 → 百分比
            else if (d > 100.0 && d <= 10000.0)
                d /= 100.0;                       // milli-percent → 百分比
            return std::clamp(static_cast<int>(d), 0, 100);
        } catch (...) { return -1; }
    }
}

// 频率：返回 MHz，失败返回 -1.0
inline double readGpuFrequency(const GpuPaths& p) {
    if (p.freq.empty()) return -1.0;
    std::ifstream f(p.freq);
    if (!f.is_open()) return -1.0;
    std::string line;
    if (!std::getline(f, line)) return -1.0;
    try {
        double v = std::stod(line);
        if (v > 1e7)       v /= 1e6;   // Hz  → MHz
        else if (v > 1e4)  v /= 1e3;   // KHz → MHz
        return v;
    } catch (...) { return -1.0; }
}

// 温度：0..100 摄氏度，失败返回 -1
// 优化：缓存命中的 thermal_zone 编号，避免每帧扫 0..59 号 zone（每帧上百次 open）
inline int& fhTempZoneCache() { static int z = -1; return z; }

inline int fhReadZoneTemp(int z) {
    char vp[256];
    std::snprintf(vp, sizeof(vp), "/sys/class/thermal/thermal_zone%d/temp", z);
    std::ifstream vf(vp);
    if (!vf.is_open()) return -1;
    std::string line;
    if (!std::getline(vf, line)) return -1;
    try {
        double t = std::stod(line);
        if (t > 1000.0) t /= 1000.0;              // 毫摄氏度 → 摄氏度
        if (t > 0.0 && t < 200.0) return static_cast<int>(t);
    } catch (...) {}
    return -1;
}

inline bool fhZoneIsGpu(int z) {
    char tp[256];
    std::snprintf(tp, sizeof(tp), "/sys/class/thermal/thermal_zone%d/type", z);
    std::ifstream tf(tp);
    if (!tf.is_open()) return false;
    std::string type;
    if (!std::getline(tf, type)) return false;
    for (char& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* tokens[] = {
        "gpuss", "gpu", "kgsl", "g3d", "mtktsgpu", "xclipse", "mali", "tsens", nullptr
    };
    for (int i = 0; tokens[i]; ++i)
        if (type.find(tokens[i]) != std::string::npos) return true;
    return false;
}

inline int readGpuTemperature() {
    // 1) 先试缓存命中的 zone（热点路径）
    int cached = fhTempZoneCache();
    if (cached >= 0) {
        int t = fhReadZoneTemp(cached);
        if (t >= 0) return t;
        fhTempZoneCache() = -1;                   // 缓存失效，重新扫描
    }
    // 2) 扫描 thermal_zone，找 type 含 GPU 关键字的
    for (int z = 0; z < 60; ++z) {
        if (!fhZoneIsGpu(z)) continue;
        int t = fhReadZoneTemp(z);
        if (t >= 0) { fhTempZoneCache() = z; return t; }
    }
    // 3) 退路：KGSL 自带温度节点
    static const char* fallbacks[] = {
        "/sys/class/kgsl/kgsl-3d0/temp",
        "/sys/class/kgsl/kgsl-3d0/devfreq/temp",
        nullptr
    };
    for (int i = 0; fallbacks[i]; ++i) {
        std::ifstream f(fallbacks[i]);
        if (!f.is_open()) continue;
        std::string line;
        if (!std::getline(f, line)) continue;
        try {
            double t = std::stod(line);
            if (t > 1000.0) t /= 1000.0;
            if (t > 0.0 && t < 200.0) return static_cast<int>(t);
        } catch (...) {}
    }
    return -1;
}

// 是否是 Adreno / kgsl 设备（Android 上的高通 GPU）。
// 这类设备是统一内存(UMA)：GPU 没有独立显存，显存就是共享的系统内存。
inline bool fhIsAdrenoKgsl() {
    std::ifstream f("/sys/class/kgsl/kgsl-3d0/gpu_model");
    if (f.is_open()) return true;
    std::ifstream g("/sys/class/kgsl/kgsl-3d0/gpubusy");
    return g.is_open();
}

// 是否允许用"共享内存"来表示显存：
//   MANGOHUD_VRAM_SHARED=0      -> 关闭（显示 N/A）
//   MANGOHUD_VRAM_SHARED=1 等   -> 强制开启
//   未设置                      -> Adreno/kgsl 设备上自动开启，其它设备关闭
// 说明：统一内存设备的显存与系统内存是同一块 RAM，所以这个数值对它们有实际意义；
//       桌面独显有真正的显存计数，不会走到这里，也不会被这个开关影响。
inline bool fhVramSharedAllowed() {
    const char* v = getenv("MANGOHUD_VRAM_SHARED");
    if (v && v[0] == '0' && v[1] == '\0') return false;
    if (v && *v) return true;
    return fhIsAdrenoKgsl();
}

struct VramInfo {
    long long usedMB  = -1;
    long long totalMB = -1;
};

// 环境变量是否"打开"（未设置 / 空 / "0" 都算关闭）
inline bool fhEnvOn(const char* name) {
    const char* v = getenv(name);
    if (!v || !*v) return false;
    return !(v[0] == '0' && v[1] == '\0');
}

// 解析 fdinfo 里的内存值："1234 KiB" / "12 MiB" -> 字节
inline long long fhParseFdinfoBytes(const std::string& val) {
    std::istringstream is(val);
    long long v = 0;
    std::string unit;
    if (!(is >> v)) return 0;
    is >> unit;
    if (unit == "KiB") return v * 1024LL;
    if (unit == "MiB") return v * 1024LL * 1024;
    if (unit == "GiB") return v * 1024LL * 1024 * 1024;
    return v;  // 无单位按字节
}

// 从 /proc/self/fdinfo 读取 DRM 客户端的显存字段（Mesa/DRM 的官方途径）。
// 返回 MB；读不到任何 drm-memory-* 字段时返回 -1。
// 说明：kgsl 设备(/dev/kgsl-3d0)的 fdinfo 不输出这些字段，此时会返回 -1，
//       不会误报 0。amdgpu/panfrost/msm(freedreno) 等走 DRM 的驱动才有。
inline long long probeVramUsedViaFdinfoMB() {
    DIR* d = opendir("/proc/self/fdinfo");
    if (!d) return -1;

    long long total = 0;
    bool any = false;
    std::vector<std::string> seen_clients;  // drm-client-id 去重（同一客户端多个 fd 会重复读）

    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        std::ifstream f(std::string("/proc/self/fdinfo/") + e->d_name);
        if (!f.is_open())
            continue;

        std::string driver, client;
        long long bytes = 0;
        for (std::string line; std::getline(f, line);) {
            if (line.empty() || line[0] == ' ' || line[0] == '\t')
                continue;
            size_t colon = line.find(':');
            if (colon == std::string::npos || colon + 1 >= line.size())
                continue;
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            while (!val.empty() && (val[0] == ' ' || val[0] == '\t'))
                val.erase(val.begin());
            if (key == "drm-driver")            driver = val;
            else if (key == "drm-client-id")    client = val;
            else if (key == "drm-memory-vram" || key == "drm-memory-gtt")
                bytes += fhParseFdinfoBytes(val);
        }

        if (driver.empty()) continue;
        // 只认 Adreno/freedreno 相关的 DRM 客户端
        if (driver != "msm" && driver != "freedreno" && driver != "kgsl")
            continue;
        if (!client.empty()) {
            if (std::find(seen_clients.begin(), seen_clients.end(), client) != seen_clients.end())
                continue;
            seen_clients.push_back(client);
        }
        if (bytes > 0) {
            total += bytes;
            any = true;
        }
    }
    closedir(d);
    return any ? total / 1024 / 1024 : -1;
}

// 统一内存设备（Adreno 等 iGPU）用"系统内存占用"代理显存。
// 这些设备的显存就是共享的 RAM，所以数字有参考意义；桌面独显上会误导，
// 因此默认关闭，须显式设置 MANGOHUD_VRAM_SHARED=1。
inline bool probeSharedMemoryAsVramMB(long long& usedMB, long long& totalMB) {
    std::ifstream mi("/proc/meminfo");
    if (!mi.is_open()) return false;
    std::string line;
    long long total = -1, avail = -1;
    while (std::getline(mi, line)) {
        try {
            if (line.rfind("MemTotal:", 0) == 0)           total = std::stoll(line.substr(9));
            else if (line.rfind("MemAvailable:", 0) == 0)  avail = std::stoll(line.substr(13));
        } catch (...) {}
        if (total > 0 && avail >= 0) break;
    }
    if (total <= 0) return false;
    totalMB = total / 1024;
    if (avail >= 0) usedMB = (total - avail) / 1024;
    return true;
}

inline VramInfo detectVram() {
    VramInfo info;
    // 1) KGSL mem_used（单位 KB）→ MB
    {
        std::ifstream f("/sys/class/kgsl/kgsl-3d0/mem_used");
        if (f.is_open()) {
            std::string l;
            if (std::getline(f, l)) {
                try { info.usedMB = std::stoll(l) / 1024; return info; } catch (...) {}
            }
        }
    }
    // 2) 其它桌面/独立显卡路径
    {
        static const char* paths[] = {
            "/sys/class/drm/card0/device/mem_info_vram_used",
            "/sys/class/drm/card1/device/mem_info_vram_used",
            nullptr
        };
        for (int i = 0; paths[i]; ++i) {
            std::ifstream f(paths[i]);
            if (!f.is_open()) continue;
            std::string l;
            if (!std::getline(f, l)) continue;
            try { info.usedMB = std::stoll(l) / 1024; return info; } catch (...) {}
        }
    }
    // 3) fdinfo：Mesa/DRM 的正式途径（MangoHud 的 amdgpu / panfrost 也是这么读的）。
    //    扫本进程已打开的 DRM 客户端 fd，累加 drm-memory-vram / drm-memory-gtt。
    //    kgsl 设备的 fdinfo 不输出这些字段 -> 返回 -1，继续往下走。
    {
        long long mb = probeVramUsedViaFdinfoMB();
        if (mb >= 0) {
            info.usedMB = mb;
            return info;
        }
    }
    // 4) 统一内存设备（Adreno/kgsl）：显示共享内存占用作为显存。
    //    默认在检测到 kgsl 时自动开启，可用 MANGOHUD_VRAM_SHARED=0 关闭。
    if (fhVramSharedAllowed()) {
        long long used = -1, total = -1;
        if (probeSharedMemoryAsVramMB(used, total)) {
            info.usedMB  = used;
            info.totalMB = total;
            return info;
        }
    }
    // 5) 实在拿不到 -> 返回 -1（不可用）。
    //    绝不"假装有值"：那会变成 HUD 上假的 0。调用方按 -1 处理：
    //    标准 HUD 显示 N/A，FusionHUD 直接不画这一行。
    info.usedMB  = -1;
    info.totalMB = -1;
    return info;
}

// 带缓存的显存探测：渲染路径每帧都会取值，不能每帧去 open 一堆文件。
inline VramInfo detectVramCached(unsigned refresh_ms = 200) {
    static VramInfo cached;
    static long long last_ms = 0;
    long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    if (now_ms - last_ms >= (long long)refresh_ms) {
        cached = detectVram();
        last_ms = now_ms;
    }
    return cached;
}

} // namespace fusionhud