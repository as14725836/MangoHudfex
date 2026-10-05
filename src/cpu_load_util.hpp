/**
 * cpu_load_util.hpp — 在 /proc/stat 不可用时，用 cpuidle 空闲计数反推逐核负载
 *
 * 背景：MangoHud 的逐核负载（频率左边那个百分比）完全依赖 /proc/stat。
 * 但很多 Android/SELinux 设备上 /proc/stat 是 **Permission denied**，于是：
 *   - 逐核负载恒为 0
 *   - 总负载退化成"本进程的使用率"（update_process_usage），不是系统负载
 *
 * 本头提供一个可靠的旁路：
 *   /sys/devices/system/cpu/cpuN/cpuidle/stateM/time   ← 每核每个 idle 状态的累计驻留时间（微秒，单调递增）
 *   逐核负载 ≈ 1 - Δ(该核所有 idle 状态时间之和) / Δ(挂钟时间)
 *
 * 实测（aarch64 / Android 内核 5.15）：该节点可读、单调递增，能给出稳定的负载读数。
 */
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <time.h>

namespace cpuload_util {

inline long long monotonic_us() {
    struct timespec ts {};
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return static_cast<long long>(ts.tv_sec) * 1000000LL + static_cast<long long>(ts.tv_nsec) / 1000LL;
}

/** 某个核的累计空闲时间（微秒）：该核所有 cpuidle 状态 time 之和 */
inline bool core_idle_us(int core, unsigned long long& out) {
    char dir[256];
    std::snprintf(dir, sizeof(dir), "/sys/devices/system/cpu/cpu%d/cpuidle", core);
    DIR* d = opendir(dir);
    if (!d)
        return false;

    unsigned long long total = 0;
    bool any = false;
    while (struct dirent* e = readdir(d)) {
        if (std::strncmp(e->d_name, "state", 5) != 0)
            continue;
        // 用 std::string 拼接，避免 -Wformat-truncation（gcc 会按 d_name 最大长度推算）
        const std::string file = std::string(dir) + "/" + e->d_name + "/time";
        FILE* fp = std::fopen(file.c_str(), "r");
        if (!fp)
            continue;
        unsigned long long v = 0;
        if (std::fscanf(fp, "%llu", &v) == 1) {
            total += v;
            any = true;
        }
        std::fclose(fp);
    }
    closedir(d);
    if (!any)
        return false;
    out = total;
    return true;
}

/**
 * 逐核负载采样器。第一次调用只建立基线（返回 false），之后每次返回
 * 自上次采样以来的逐核负载百分比与全体平均负载。
 */
class IdleSampler {
public:
    bool sample(const std::vector<int>& cores, std::vector<float>& per_core_pct, float& total_pct) {
        const size_t n = cores.size();
        if (n == 0)
            return false;

        std::vector<unsigned long long> cur(n, 0);
        bool any = false;
        for (size_t i = 0; i < n; ++i) {
            if (core_idle_us(cores[i], cur[i]))
                any = true;
        }
        if (!any)
            return false;

        const long long now = monotonic_us();
        if (!m_have || m_prev.size() != n || now <= m_prev_us) {
            m_prev = cur;
            m_prev_us = now;
            m_have = true;
            return false;   // 需要下一帧才能算出增量
        }

        const double dt = static_cast<double>(now - m_prev_us);
        if (dt <= 0.0) {
            m_prev = cur;
            m_prev_us = now;
            return false;
        }

        per_core_pct.assign(n, 0.0f);
        double sum_busy = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double idle_delta = static_cast<double>(cur[i] - m_prev[i]);
            double busy = 1.0 - (idle_delta / dt);
            busy = std::min(std::max(busy, 0.0), 1.0);
            per_core_pct[i] = static_cast<float>(busy * 100.0);
            sum_busy += busy;
        }
        total_pct = static_cast<float>(sum_busy / static_cast<double>(n) * 100.0);

        m_prev = cur;
        m_prev_us = now;
        return true;
    }

private:
    std::vector<unsigned long long> m_prev;
    long long m_prev_us = 0;
    bool m_have = false;
};

} // namespace cpuload_util