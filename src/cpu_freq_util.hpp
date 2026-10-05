/**
 * cpu_freq_util.hpp — 跨平台/跨内核的 CPU 频率读取兜底链
 *
 * 背景：MangoHud 原实现只认两条路：
 *   1) /sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq
 *   2) /proc/cpuinfo 的 "cpu MHz"（x86 专有）
 * 在 ARM/Android（glibc + FEX + Wine）下经常两条都走不通，导致频率恒为 0。
 *
 * 本头按“由准到糙”的顺序尝试多个真实存在的节点：
 *   1) cpuN/cpufreq/scaling_cur_freq      —— 最准，本机实测可读（kHz）
 *   2) cpuN/cpufreq/cpuinfo_cur_freq      —— 硬件当前频率（kHz）
 *   3) policyN/scaling_cur_freq           —— 部分内核只在 policy 下暴露
 *   4) cpuN/cpufreq/stats/time_in_state   —— Android 常用：取累计时间最小的频点
 *   5) MANGOHUD_CPUFREQ_PATH              —— 环境变量兜底（支持 %d 模板）
 *   6) /proc/cpuinfo 的 "MHz"             —— 最后的 x86 兼容路径
 *
 * 该读取很轻（单次 fscanf），但仍加 1 秒缓存，避免在 HUD 每帧渲染时反复打 sysfs。
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <time.h>

namespace cpufreq_util {

/** 读文件里的第一个整数（fscanf 会跳过前导空白/换行） */
inline bool read_first_ll(const std::string& path, long long& out) {
    FILE* fp = std::fopen(path.c_str(), "r");
    if (!fp)
        return false;
    long long v = 0;
    const bool ok = (std::fscanf(fp, "%lld", &v) == 1);
    std::fclose(fp);
    if (ok)
        out = v;
    return ok;
}

/** "核心号 -> /sys/devices/system/cpu/cpufreq/policyN" 映射（只建一次） */
inline const std::vector<std::pair<int, std::string>>& policy_map() {
    static std::vector<std::pair<int, std::string>> map;
    static bool built = false;
    if (!built) {
        built = true;
        DIR* d = opendir("/sys/devices/system/cpu/cpufreq");
        if (d) {
            while (struct dirent* e = readdir(d)) {
                if (std::strncmp(e->d_name, "policy", 6) != 0)
                    continue;
                const std::string base = std::string("/sys/devices/system/cpu/cpufreq/") + e->d_name;
                FILE* fp = std::fopen((base + "/related_cpus").c_str(), "r");
                if (!fp)
                    continue;
                char buf[256] = {};
                std::string rel;
                if (std::fgets(buf, sizeof(buf), fp))
                    rel = buf;
                std::fclose(fp);
                std::istringstream ss(rel);
                int id;
                while (ss >> id)
                    map.emplace_back(id, base);
            }
            closedir(d);
        }
    }
    return map;
}

/**
 * time_in_state：每行 "频率(kHz) 累计时间"。Android 上当前频率 ≈ 累计时间最小的那一档。
 * 若全部为 0，则退回最大频点（至少能给出一个合理数字）。
 */
inline bool time_in_state_mhz(int core, int& mhz_out) {
    char p[256];
    std::snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/stats/time_in_state", core);
    std::ifstream f(p);
    if (!f.is_open())
        return false;
    long long best_freq = 0, best_time = -1, max_freq = 0;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        long long freq = 0, t = 0;
        if (!(ss >> freq >> t))
            continue;
        if (freq > max_freq)
            max_freq = freq;
        if (t > 0 && (best_time < 0 || t < best_time)) {
            best_time = t;
            best_freq = freq;
        }
    }
    const long long pick = best_freq > 0 ? best_freq : max_freq;
    if (pick <= 0)
        return false;
    mhz_out = static_cast<int>(pick / 1000);
    return true;
}

/** 取单个核心的 MHz；失败返回 -1 */
inline int core_mhz_uncached(int core) {
    long long khz = 0;
    char buf[512];

    // 0) 环境变量兜底（支持 "%d" 模板，也支持直接一个文件）
    if (const char* env = std::getenv("MANGOHUD_CPUFREQ_PATH")) {
        if (*env) {
            if (std::strchr(env, '%')) {
                std::snprintf(buf, sizeof(buf), env, core);
                if (read_first_ll(buf, khz) && khz > 0)
                    return static_cast<int>(khz / 1000);
            } else if (read_first_ll(env, khz) && khz > 0) {
                return static_cast<int>(khz / 1000);
            }
        }
    }

    // 1) / 2) 核级节点
    static const char* kPerCore[] = {
        "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",
        "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_cur_freq",
    };
    for (const char* fmt : kPerCore) {
        std::snprintf(buf, sizeof(buf), fmt, core);
        if (read_first_ll(buf, khz) && khz > 0)
            return static_cast<int>(khz / 1000);
    }

    // 3) policy 级
    for (const auto& p : policy_map()) {
        if (p.first != core)
            continue;
        const std::string path = p.second + "/scaling_cur_freq";
        if (read_first_ll(path, khz) && khz > 0)
            return static_cast<int>(khz / 1000);
    }

    // 4) time_in_state
    int mhz = 0;
    if (time_in_state_mhz(core, mhz) && mhz > 0)
        return mhz;

    return -1;
}

/** 带 1 秒缓存 */
inline int core_mhz(int core) {
    struct Cache {
        time_t t = 0;
        int mhz = -1;
    };
    static std::vector<Cache> cache;
    static time_t last = 0;

    const time_t now = time(nullptr);
    if (static_cast<int>(cache.size()) <= core)
        cache.resize(static_cast<size_t>(core) + 1);

    if (now != last) {
        last = now;
        for (auto& c : cache)
            c.t = 0;
    }
    Cache& c = cache[static_cast<size_t>(core)];
    if (c.t == now && now != 0)
        return c.mhz;
    c.t = now;
    c.mhz = core_mhz_uncached(core);
    return c.mhz;
}

/** 全核最大 MHz（聚合值）；失败返回 -1 */
inline int max_mhz(int cores) {
    int best = -1;
    for (int i = 0; i < cores; ++i) {
        const int v = core_mhz(i);
        if (v > best)
            best = v;
    }
    return best;
}

/** /proc/cpuinfo 的 "cpu MHz"（x86/Wine 有，ARM 通常没有）。失败返回 -1。 */
inline int cpuinfo_mhz() {
    std::ifstream f("/proc/cpuinfo");
    if (!f.is_open())
        return -1;
    std::string line;
    while (std::getline(f, line)) {
        const size_t pos = line.find("MHz");
        if (pos == std::string::npos)
            continue;
        const size_t colon = line.find(':');
        const std::string num = colon != std::string::npos ? line.substr(colon + 1) : line;
        try {
            const double v = std::stod(num);
            if (v > 0.0)
                return static_cast<int>(v);
        } catch (...) {
        }
    }
    return -1;
}

} // namespace cpufreq_util
