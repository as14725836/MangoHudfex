#pragma once
#ifndef MANGOHUD_CPU_H
#define MANGOHUD_CPU_H

#include <vector>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <map>
#include <unordered_map>

#include "timing.hpp"
#include "gpu.h"

typedef struct CPUData_ {
    unsigned long long totalTime;
    unsigned long long userTime;
    unsigned long long systemTime;
    unsigned long long systemAllTime;
    unsigned long long idleAllTime;
    unsigned long long idleTime;
    unsigned long long niceTime;
    unsigned long long ioWaitTime;
    unsigned long long irqTime;
    unsigned long long softIrqTime;
    unsigned long long stealTime;
    unsigned long long guestTime;

    unsigned long long totalPeriod;
    unsigned long long userPeriod;
    unsigned long long systemPeriod;
    unsigned long long systemAllPeriod;
    unsigned long long idleAllPeriod;
    unsigned long long idlePeriod;
    unsigned long long nicePeriod;
    unsigned long long ioWaitPeriod;
    unsigned long long irqPeriod;
    unsigned long long softIrqPeriod;
    unsigned long long stealPeriod;
    unsigned long long guestPeriod;

    int cpu_id;
    float percent;
    int mhz;
    int temp;
    int cpu_mhz;
    float power;

    std::string label = "unknown";
} CPUData;

struct CPUPowerData {
    virtual ~CPUPowerData() = default;
    int source;
};

class CPUStats {
public:
    CPUStats();
    ~CPUStats();

    bool Init();
    bool Reinit();
    bool Updated() { return m_updatedCPUs; }

    bool UpdateCPUData();
    bool UpdateCoreMhz();
    bool UpdateCpuTemp();
    bool UpdateCpuPower();
    bool ReadcpuTempFile(int& temp);
    bool GetCpuFile();
    bool InitCpuPowerData();

    const std::vector<CPUData>& GetCPUData() const { return m_cpuData; }
    const CPUData& GetCPUDataTotal() const { return m_cpuDataTotal; }

private:
    unsigned long long m_boottime = 0;
    std::vector<CPUData> m_cpuData;
    CPUData m_cpuDataTotal{};
    std::vector<int> m_coreMhz;
    double m_cpuPeriod = 0;
    bool m_updatedCPUs = false;
    bool m_inited = false;
    FILE* m_cpuTempFile = nullptr;

    std::unique_ptr<CPUPowerData> m_cpuPowerData;

    // ✅ 关键修复：CPU index map 必须在 class 里
    std::unordered_map<int, size_t> m_cpuIndexMap;

    const std::map<std::string, std::string> intel_cores = {
        {"P", "/sys/devices/cpu_core/cpus"},
        {"E", "/sys/devices/cpu_atom/cpus"}
    };

    const std::map<std::string, std::string> arm_cores = {
        {"0xd07", "A57"}, {"0xd08", "A72"}, {"0xd09", "A73"},
        {"0xd0a", "A75"}, {"0xd0b", "A76"}, {"0xd0c", "A77"},
        {"0xd41", "A78"}, {"0xd44", "X1"},  {"0xd4d", "X2"},
        {"0xd4e", "X3"},  {"0xd47", "A710"},{"0xd4f", "A720"},
        {"0xd4b", "X4"},  {"0xd03", "A53"}, {"0xd05", "A55"},
        {"0xd46", "A510"},{"0xd4a", "A520"},{"0xd04", "A35"},
        {"0xd06", "A65"}
    };
};

extern CPUStats cpuStats;

#endif
