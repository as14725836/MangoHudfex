#include "cpu.h"
#include <memory>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <utility>
#include <cstdlib>
#include <numeric>
#include <algorithm>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <regex>
#include <inttypes.h>
#include <spdlog/spdlog.h>
#include "string_utils.h"
#include "cpu_freq_util.hpp"
#include "cpu_load_util.hpp"
#include "gpu.h"
#include "hud_elements.h"

#ifndef TEST_ONLY
#include "hud_elements.h"
#endif

#ifndef PROCDIR
#define PROCDIR "/proc"
#endif

#ifndef PROCSTATFILE
#define PROCSTATFILE PROCDIR "/stat"
#endif

#ifndef PROCMEMINFOFILE
#define PROCMEMINFOFILE PROCDIR "/meminfo"
#endif

#ifndef PROCCPUINFOFILE
#define PROCCPUINFOFILE PROCDIR "/cpuinfo"
#endif

#include "file_utils.h"

// 进程 CPU 使用率监控相关变量
static unsigned long long prev_proc_ticks = 0;
static std::chrono::steady_clock::time_point prev_time;
static bool is_first_run = true;
static long clk_tck = 100;
static int num_cores = 1;

// 读取当前进程的 CPU 时间 (utime + stime)
static unsigned long long get_self_cpu_ticks() {
    std::ifstream file("/proc/self/stat");
    if (!file.is_open()) return 0;

    std::string line;
    std::getline(file, line);
    
    // /proc/self/stat 格式复杂，第二项 (comm) 可能包含空格和括号
    size_t last_parenthesis = line.find_last_of(')');
    if (last_parenthesis == std::string::npos || last_parenthesis + 2 >= line.length()) return 0;

    std::stringstream ss(line.substr(last_parenthesis + 2));
    
    std::string val;
    unsigned long long utime = 0, stime = 0;
    
    // 跳过前 11 项
    for (int i = 0; i < 11; i++) ss >> val;
    
    ss >> utime >> stime;
    return utime + stime;
}

// 更新进程 CPU 使用率
static void update_process_usage(CPUData& cpuDataTotal) {
    unsigned long long cur_ticks = get_self_cpu_ticks();
    auto cur_time = std::chrono::steady_clock::now();

    if (is_first_run) {
        clk_tck = sysconf(_SC_CLK_TCK);
        num_cores = sysconf(_SC_NPROCESSORS_ONLN);
        if (num_cores < 1) num_cores = 1;
        if (clk_tck < 1) clk_tck = 100;

        prev_proc_ticks = cur_ticks;
        prev_time = cur_time;
        is_first_run = false;
        return;
    }

    std::chrono::duration<float> elapsed_seconds = cur_time - prev_time;
    float dt = elapsed_seconds.count();

    if (dt > 0.0f) {
        unsigned long long tick_diff = 0;
        if (cur_ticks > prev_proc_ticks) tick_diff = cur_ticks - prev_proc_ticks;

        float cpu_usage = ((float)tick_diff / (float)clk_tck) / dt * 100.0f;
        cpu_usage /= (float)num_cores;

        if (cpu_usage > 100.0f) cpu_usage = 100.0f;
        if (cpu_usage < 0.0f) cpu_usage = 0.0f;

        cpuDataTotal.percent = cpu_usage;

        prev_proc_ticks = cur_ticks;
        prev_time = cur_time;
    }
}

static void calculateCPUData(CPUData& cpuData,
    unsigned long long int usertime,
    unsigned long long int nicetime,
    unsigned long long int systemtime,
    unsigned long long int idletime,
    unsigned long long int ioWait,
    unsigned long long int irq,
    unsigned long long int softIrq,
    unsigned long long int steal,
    unsigned long long int guest,
    unsigned long long int guestnice)
{
    // Guest time is already accounted in usertime
    usertime = usertime - guest;
    nicetime = nicetime - guestnice;
    // Fields existing on kernels >= 2.6
    // (and RHEL's patched kernel 2.4...)
    unsigned long long int idlealltime = idletime + ioWait;
    unsigned long long int systemalltime = systemtime + irq + softIrq;
    unsigned long long int virtalltime = guest + guestnice;
    unsigned long long int totaltime = usertime + nicetime + systemalltime + idlealltime + steal + virtalltime;

    // Since we do a subtraction (usertime - guest) and cputime64_to_clock_t()
    // used in /proc/stat rounds down numbers, it can lead to a case where the
    // integer overflow.
    #define WRAP_SUBTRACT(a,b) (a > b) ? a - b : 0
    cpuData.userPeriod = WRAP_SUBTRACT(usertime, cpuData.userTime);
    cpuData.nicePeriod = WRAP_SUBTRACT(nicetime, cpuData.niceTime);
    cpuData.systemPeriod = WRAP_SUBTRACT(systemtime, cpuData.systemTime);
    cpuData.systemAllPeriod = WRAP_SUBTRACT(systemalltime, cpuData.systemAllTime);
    cpuData.idleAllPeriod = WRAP_SUBTRACT(idlealltime, cpuData.idleAllTime);
    cpuData.idlePeriod = WRAP_SUBTRACT(idletime, cpuData.idleTime);
    cpuData.ioWaitPeriod = WRAP_SUBTRACT(ioWait, cpuData.ioWaitTime);
    cpuData.irqPeriod = WRAP_SUBTRACT(irq, cpuData.irqTime);
    cpuData.softIrqPeriod = WRAP_SUBTRACT(softIrq, cpuData.softIrqTime);
    cpuData.stealPeriod = WRAP_SUBTRACT(steal, cpuData.stealTime);
    cpuData.guestPeriod = WRAP_SUBTRACT(virtalltime, cpuData.guestTime);
    cpuData.totalPeriod = WRAP_SUBTRACT(totaltime, cpuData.totalTime);
    #undef WRAP_SUBTRACT
    cpuData.userTime = usertime;
    cpuData.niceTime = nicetime;
    cpuData.systemTime = systemtime;
    cpuData.systemAllTime = systemalltime;
    cpuData.idleAllTime = idlealltime;
    cpuData.idleTime = idletime;
    cpuData.ioWaitTime = ioWait;
    cpuData.irqTime = irq;
    cpuData.softIrqTime = softIrq;
    cpuData.stealTime = steal;
    cpuData.guestTime = virtalltime;
    cpuData.totalTime = totaltime;

    if (cpuData.totalPeriod == 0)
        return;
    float total = (float)cpuData.totalPeriod;
    float v[4];
    v[0] = cpuData.nicePeriod * 100.0f / total;
    v[1] = cpuData.userPeriod * 100.0f / total;

    /* if not detailed */
    v[2] = cpuData.systemAllPeriod * 100.0f / total;
    v[3] = (cpuData.stealPeriod + cpuData.guestPeriod) * 100.0f / total;
    //cpuData.percent = std::clamp(v[0]+v[1]+v[2]+v[3], 0.0f, 100.0f);
    cpuData.percent = std::min(std::max(v[0]+v[1]+v[2]+v[3], 0.0f), 100.0f);
}

CPUStats::CPUStats()
{
}

CPUStats::~CPUStats()
{
    if (m_cpuTempFile) {
        fclose(m_cpuTempFile);
        m_cpuTempFile = nullptr;
    }
}

bool CPUStats::Init()
{
    if (m_inited)
        return true;

    // 尝试从 /proc/stat 读取 CPU 核心信息
    std::string line;
    std::ifstream file (PROCSTATFILE);
    bool first = true;
    m_cpuData.clear();

    if (file.is_open()) {
        do {
            if (!std::getline(file, line)) {
                break;
            } else if (starts_with(line, "cpu")) {
                if (first) {
                    first = false;
                    continue;
                }

                CPUData cpu = {};
                cpu.totalTime = 1;
                cpu.totalPeriod = 1;
                sscanf(line.c_str(), "cpu%4d ", &cpu.cpu_id);
                m_cpuData.push_back(cpu);

            } else if (starts_with(line, "btime ")) {
                sscanf(line.c_str(), "btime %lld\n", &m_boottime);
                break;
            }
        } while(true);
    }

    // 如果 /proc/stat 不可用，创建默认的核心列表
    if (m_cpuData.empty()) {
        int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (num_cpus < 1) num_cpus = 1;
        
        for (int i = 0; i < num_cpus; i++) {
            CPUData cpu = {};
            cpu.cpu_id = i;
            cpu.totalTime = 1;
            cpu.totalPeriod = 1;
            m_cpuData.push_back(cpu);
        }
        // Android 上 /proc/stat 常被 SELinux 拒读，这是预期情况，不当警告刷屏
        SPDLOG_DEBUG("Could not read /proc/stat, created {} default CPU entries", num_cpus);
    }

#ifndef TEST_ONLY
    if (get_params()->enabled[OVERLAY_PARAM_ENABLED_core_type])
        get_cpu_cores_types();
#endif

    m_inited = true;
    return UpdateCPUData();
}

bool CPUStats::Reinit()
{
    m_inited = false;
    return Init();
}

//TODO take sampling interval into account?
bool CPUStats::UpdateCPUData()
{
    if (!m_inited)
        return false;

    // 首先尝试从 /proc/stat 读取
    unsigned long long int usertime, nicetime, systemtime, idletime;
    unsigned long long int ioWait, irq, softIrq, steal, guest, guestnice;
    int cpuid = -1;
    size_t cpu_count = 0;

    std::string line;
    std::ifstream file (PROCSTATFILE);
    bool ret = false;

    if (file.is_open()) {
        do {
            if (!std::getline(file, line)) {
                break;
            } else if (!ret && sscanf(line.c_str(), "cpu  %16llu %16llu %16llu %16llu %16llu %16llu %16llu %16llu %16llu %16llu",
                &usertime, &nicetime, &systemtime, &idletime, &ioWait, &irq, &softIrq, &steal, &guest, &guestnice) == 10) {
                ret = true;
                calculateCPUData(m_cpuDataTotal, usertime, nicetime, systemtime, idletime, ioWait, irq, softIrq, steal, guest, guestnice);
            } else if (sscanf(line.c_str(), "cpu%4d %16llu %16llu %16llu %16llu %16llu %16llu %16llu %16llu %16llu %16llu",
                &cpuid, &usertime, &nicetime, &systemtime, &idletime, &ioWait, &irq, &softIrq, &steal, &guest, &guestnice) == 11) {

                if (!ret) {
                    SPDLOG_DEBUG("Failed to parse 'cpu' line:{}", line);
                    break;
                }

                if (cpuid < 0) {
                    SPDLOG_DEBUG("Cpu id '{}' is out of bounds", cpuid);
                    break;
                }

                if (cpu_count + 1 > m_cpuData.size() || m_cpuData[cpu_count].cpu_id != cpuid) {
                    SPDLOG_DEBUG("Cpu id '{}' is out of bounds or wrong index, reiniting", cpuid);
                    return Reinit();
                }

                CPUData& cpuData = m_cpuData[cpu_count];
                calculateCPUData(cpuData, usertime, nicetime, systemtime, idletime, ioWait, irq, softIrq, steal, guest, guestnice);
                cpuid = -1;
                cpu_count++;

            } else {
                break;
            }
        } while(true);

        if (ret && cpu_count < m_cpuData.size())
            m_cpuData.resize(cpu_count);
    }

    // 如果 /proc/stat 不可用（Android/SELinux 上常见 Permission denied），
    // 先用 cpuidle 的每核空闲计数反推逐核负载 —— 否则频率左边那个百分比恒为 0，
    // 且总负载会退化成"本进程使用率"而不是系统负载。
    if (!ret) {
        std::vector<int> cores;
        cores.reserve(m_cpuData.size());
        for (const auto& c : m_cpuData)
            cores.push_back(c.cpu_id);

        std::vector<float> per_core;
        float total_pct = -1.0f;
        bool idle_ok = false;
        {
            static cpuload_util::IdleSampler sampler;
            idle_ok = sampler.sample(cores, per_core, total_pct);
        }

        if (idle_ok) {
            for (size_t i = 0; i < m_cpuData.size() && i < per_core.size(); ++i)
                m_cpuData[i].percent = per_core[i];
            if (total_pct >= 0.0f)
                m_cpuDataTotal.percent = total_pct;
            SPDLOG_DEBUG("Using cpuidle-based per-core load: {:.1f}%", m_cpuDataTotal.percent);
        } else {
            update_process_usage(m_cpuDataTotal);
            SPDLOG_DEBUG("Using process CPU monitoring: {:.1f}%", m_cpuDataTotal.percent);
        }
    } else {
        m_cpuPeriod = (double)m_cpuData[0].totalPeriod / m_cpuData.size();
    }

    m_updatedCPUs = true;
    return true; // 总是返回 true，因为我们有备用方案
}

bool CPUStats::UpdateCoreMhz() {
    m_coreMhz.clear();

    // 逐核按候选路径依次尝试。
    // 原实现的问题：遇到**第一个** fopen 失败就 break 并把 scaling_freq 置为 false，
    // 此后永久走 /proc/cpuinfo 分支 —— 而该分支只认 x86 的 "MHz" 字段，
    // 在 ARM/Android（以及容器里 /sys 不可见）下就永远不会再恢复，频率恒为 0。
    auto read_khz = [](const std::string& path, int64_t& out) -> bool {
        FILE* fp = fopen(path.c_str(), "r");
        if (!fp)
            return false;
        const bool ok = (fscanf(fp, "%" PRId64, &out) == 1);
        fclose(fp);
        return ok;
    };

    // 有些内核只在 policyN 下暴露 cpufreq（cpuN/cpufreq 不存在）。
    // 先建立 "核心号 -> policy 目录" 的映射用于回落。
    std::vector<std::pair<int, std::string>> policy_dirs;
    if (DIR* d = opendir("/sys/devices/system/cpu/cpufreq")) {
        while (auto* e = readdir(d)) {
            if (strncmp(e->d_name, "policy", 6) != 0)
                continue;
            const std::string base = std::string("/sys/devices/system/cpu/cpufreq/") + e->d_name;
            FILE* fp = fopen((base + "/related_cpus").c_str(), "r");
            if (!fp)
                continue;
            char buf[256] = {};
            std::string rel;
            if (fgets(buf, sizeof(buf), fp))
                rel = buf;
            fclose(fp);
            std::stringstream ss(rel);
            int id;
            while (ss >> id)
                policy_dirs.emplace_back(id, base);
        }
        closedir(d);
    }

    // 允许环境变量指定一个代表路径（容器/裁剪环境下 /sys 路径不同时可以救急）
    const char* env_path = getenv("MANGOHUD_CPUFREQ_PATH");

    size_t ok_cores = 0;
    for (auto& cpu : m_cpuData) {
        const std::string core = std::to_string(cpu.cpu_id);

        std::vector<std::string> candidates;
        if (env_path && *env_path)
            candidates.emplace_back(env_path);
        candidates.emplace_back("/sys/devices/system/cpu/cpu" + core + "/cpufreq/scaling_cur_freq");
        candidates.emplace_back("/sys/devices/system/cpu/cpu" + core + "/cpufreq/cpuinfo_cur_freq");
        for (const auto& p : policy_dirs)
            if (p.first == cpu.cpu_id)
                candidates.emplace_back(p.second + "/scaling_cur_freq");

        int64_t khz = 0;
        for (const auto& path : candidates) {
            if (read_khz(path, khz) && khz > 0) {
                cpu.mhz = khz / 1000;   // kHz -> MHz
                ++ok_cores;
                break;
            }
        }
    }

    // 全部核都读不到时才回退 /proc/cpuinfo（x86 / Wine 有 "cpu MHz"；
    // 多数 ARM 内核没有该字段，此时只能保持 0）
    if (ok_cores == 0) {
        SPDLOG_WARN("CPUStats: 所有核的 cpufreq 都不可读，回退 /proc/cpuinfo");
        std::ifstream cpuInfo(PROCCPUINFOFILE);
        std::string row;
        size_t i = 0;
        while (std::getline(cpuInfo, row) && i < m_cpuData.size()) {
            if (row.find("MHz") != std::string::npos){
                row = std::regex_replace(row, std::regex(R"([^0-9.])"), "");
                // 解析失败时**不要**把已有的频率抹成 0（原实现是破坏性的，
                // 会让兜底链已经读到的值白读）
                int parsed = 0;
                if (try_stoi(parsed, row) && parsed > 0)
                    m_cpuData[i].mhz = parsed;
                i++;
            }
        }
    }

    // 最后的安全网：如果上面这些路径全都没读到（ARM/Android 常见），
    // 用多路兜底链再试一次 —— 覆盖 cpuinfo_cur_freq / policyN / time_in_state /
    // MANGOHUD_CPUFREQ_PATH，避免频率恒为 0。
    if (ok_cores == 0) {
        for (auto& cpu : m_cpuData) {
            const int mhz_util = cpufreq_util::core_mhz(cpu.cpu_id);
            if (mhz_util > 0) {
                cpu.mhz = mhz_util;
                ++ok_cores;
            }
        }
    }

    m_cpuDataTotal.cpu_mhz = 0;
    for (auto& data : m_cpuData)
        if (data.mhz > m_cpuDataTotal.cpu_mhz)
            m_cpuDataTotal.cpu_mhz = data.mhz;

    // 让 m_coreMhz 只作为逐核 MHz 的既有镜像保持有效（此前全仓库只在 clear，从未填充）
    m_coreMhz.clear();
    m_coreMhz.reserve(m_cpuData.size());
    for (const auto& data : m_cpuData)
        m_coreMhz.push_back(data.mhz);

    return true;
}

bool CPUStats::ReadcpuTempFile(int& temp) {
	if (!m_cpuTempFile)
		return false;

	rewind(m_cpuTempFile);
	fflush(m_cpuTempFile);
	bool ret = (fscanf(m_cpuTempFile, "%d", &temp) == 1);
	temp = temp / 1000;

	return ret;
}

bool CPUStats::UpdateCpuTemp() {
    if (gpus) {
        for (auto gpu : gpus->available_gpus) {
            if (gpu->is_apu()) {
                m_cpuDataTotal.temp = gpu->metrics.apu_cpu_temp;
                return true;
            }
        }
    }

    int temp = 0;
    bool ret = ReadcpuTempFile(temp);
    m_cpuDataTotal.temp = temp;

    return ret;
}

static bool get_cpu_power_k10temp(CPUPowerData* cpuPowerData, float& power) {
    CPUPowerData_k10temp* powerData_k10temp = (CPUPowerData_k10temp*)cpuPowerData;

    if(powerData_k10temp->corePowerFile || powerData_k10temp->socPowerFile)
    {
        rewind(powerData_k10temp->corePowerFile);
        rewind(powerData_k10temp->socPowerFile);
        fflush(powerData_k10temp->corePowerFile);
        fflush(powerData_k10temp->socPowerFile);
        int corePower, socPower;
        if (fscanf(powerData_k10temp->corePowerFile, "%d", &corePower) != 1)
            goto voltagebased;
        if (fscanf(powerData_k10temp->socPowerFile, "%d", &socPower) != 1)
            goto voltagebased;
        power = (corePower + socPower) / 1000000;
        return true;
    }
    voltagebased:
    if (!powerData_k10temp->coreVoltageFile || !powerData_k10temp->coreCurrentFile || !powerData_k10temp->socVoltageFile || !powerData_k10temp->socCurrentFile)
        return false;
    rewind(powerData_k10temp->coreVoltageFile);
    rewind(powerData_k10temp->coreCurrentFile);
    rewind(powerData_k10temp->socVoltageFile);
    rewind(powerData_k10temp->socCurrentFile);

    fflush(powerData_k10temp->coreVoltageFile);
    fflush(powerData_k10temp->coreCurrentFile);
    fflush(powerData_k10temp->socVoltageFile);
    fflush(powerData_k10temp->socCurrentFile);

    int coreVoltage, coreCurrent;
    int socVoltage, socCurrent;

    if (fscanf(powerData_k10temp->coreVoltageFile, "%d", &coreVoltage) != 1)
        return false;
    if (fscanf(powerData_k10temp->coreCurrentFile, "%d", &coreCurrent) != 1)
        return false;
    if (fscanf(powerData_k10temp->socVoltageFile, "%d", &socVoltage) != 1)
        return false;
    if (fscanf(powerData_k10temp->socCurrentFile, "%d", &socCurrent) != 1)
        return false;

    power = (coreVoltage * coreCurrent + socVoltage * socCurrent) / 1000000;

    return true;
}

static bool get_cpu_power_zenpower(CPUPowerData* cpuPowerData, float& power) {
    CPUPowerData_zenpower* powerData_zenpower = (CPUPowerData_zenpower*)cpuPowerData;

    if (!powerData_zenpower->corePowerFile || !powerData_zenpower->socPowerFile)
        return false;

    rewind(powerData_zenpower->corePowerFile);
    rewind(powerData_zenpower->socPowerFile);

    fflush(powerData_zenpower->corePowerFile);
    fflush(powerData_zenpower->socPowerFile);

    int corePower, socPower;

    if (fscanf(powerData_zenpower->corePowerFile, "%d", &corePower) != 1)
        return false;
    if (fscanf(powerData_zenpower->socPowerFile, "%d", &socPower) != 1)
        return false;

    power = (corePower + socPower) / 1000000;

    return true;
}

static bool get_cpu_power_zenergy(CPUPowerData* cpuPowerData, float& power) {
    CPUPowerData_zenergy* powerData_zenergy = (CPUPowerData_zenergy*)cpuPowerData;
    if (!powerData_zenergy->energyCounterFile)
        return false;

    rewind(powerData_zenergy->energyCounterFile);
    fflush(powerData_zenergy->energyCounterFile);

    uint64_t energyCounterValue = 0;
    if (fscanf(powerData_zenergy->energyCounterFile, "%" SCNu64, &energyCounterValue) != 1)
        return false;

    Clock::time_point now = Clock::now();
    Clock::duration timeDiff = now - powerData_zenergy->lastCounterValueTime;
    int64_t timeDiffMicro = std::chrono::duration_cast<std::chrono::microseconds>(timeDiff).count();
    uint64_t energyCounterDiff = energyCounterValue - powerData_zenergy->lastCounterValue;


    if (powerData_zenergy->lastCounterValue > 0 && energyCounterValue > powerData_zenergy->lastCounterValue)
        power = (float) energyCounterDiff / (float) timeDiffMicro;

    powerData_zenergy->lastCounterValue = energyCounterValue;
    powerData_zenergy->lastCounterValueTime = now;

    return true;
}

static bool get_cpu_power_rapl(CPUPowerData* cpuPowerData, float& power) {
    CPUPowerData_rapl* powerData_rapl = (CPUPowerData_rapl*)cpuPowerData;

    if (!powerData_rapl->energyCounterFile)
        return false;

    rewind(powerData_rapl->energyCounterFile);
    fflush(powerData_rapl->energyCounterFile);

    uint64_t energyCounterValue = 0;
    if (fscanf(powerData_rapl->energyCounterFile, "%" SCNu64, &energyCounterValue) != 1)
        return false;

    Clock::time_point now = Clock::now();
    Clock::duration timeDiff = now - powerData_rapl->lastCounterValueTime;
    int64_t timeDiffMicro = std::chrono::duration_cast<std::chrono::microseconds>(timeDiff).count();
    uint64_t energyCounterDiff = energyCounterValue - powerData_rapl->lastCounterValue;

    if (powerData_rapl->lastCounterValue > 0 && energyCounterValue > powerData_rapl->lastCounterValue)
        power = energyCounterDiff / timeDiffMicro;

    powerData_rapl->lastCounterValue = energyCounterValue;
    powerData_rapl->lastCounterValueTime = now;

    return true;
}

static bool get_cpu_power_amdgpu(float& power) {
    if (gpus)
        for (auto gpu : gpus->available_gpus)
            if (gpu->is_apu()) {
                power = gpu->metrics.apu_cpu_power;
                return true;
            }

    return false;
}

static bool get_cpu_power_xgene(CPUPowerData* cpuPowerData, float& power) {
    CPUPowerData_xgene* powerData_xgene = (CPUPowerData_xgene*)cpuPowerData;
    if (!powerData_xgene->powerFile)
        return false;

    rewind(powerData_xgene->powerFile);
    fflush(powerData_xgene->powerFile);

    uint64_t powerValue = 0;
    if (fscanf(powerData_xgene->powerFile, "%" SCNu64, &powerValue) != 1)
        return false;

    power = (float) powerValue / 1000000.0f;

    return true;
}

bool CPUStats::UpdateCpuPower() {
    InitCpuPowerData();

    if(!m_cpuPowerData)
        return false;

    float power = 0;

    switch(m_cpuPowerData->source) {
        case CPU_POWER_K10TEMP:
            if (!get_cpu_power_k10temp(m_cpuPowerData.get(), power)) return false;
            break;
        case CPU_POWER_ZENPOWER:
            if (!get_cpu_power_zenpower(m_cpuPowerData.get(), power)) return false;
            break;
        case CPU_POWER_ZENERGY:
            if (!get_cpu_power_zenergy(m_cpuPowerData.get(), power)) return false;
            break;
        case CPU_POWER_RAPL:
            if (!get_cpu_power_rapl(m_cpuPowerData.get(), power)) return false;
            break;
        case CPU_POWER_AMDGPU:
            if (!get_cpu_power_amdgpu(power)) return false;
            break;
        case CPU_POWER_XGENE:
            if (!get_cpu_power_xgene(m_cpuPowerData.get(), power)) return false;
            break;
        default:
            return false;
    }

    m_cpuDataTotal.power = power;

    return true;
}

static bool find_input(const std::string& path, const char* input_prefix, std::string& input, const std::string& name)
{
    auto files = ls(path.c_str(), input_prefix, LS_FILES);
    for (auto& file : files) {
        if (!ends_with(file, "_label"))
            continue;

        auto label = read_line(path + "/" + file);
        if (label != name)
            continue;

        auto uscore = file.find_first_of("_");
        if (uscore != std::string::npos) {
            file.erase(uscore, std::string::npos);
            input = path + "/" + file + "_input";
            //9 characters should not overflow the 32-bit int
            return safe_stoi(read_line(input).substr(0, 9)) > 0;
        }
    }
    return false;
}

static bool find_fallback_input(const std::string& path, const char* input_prefix, std::string& input)
{
    auto files = ls(path.c_str(), input_prefix, LS_FILES);
    if (!files.size())
        return false;

    std::sort(files.begin(), files.end());
    for (auto& file : files) {
        if (!ends_with(file, "_input"))
            continue;
        input = path + "/" + file;
		SPDLOG_DEBUG("fallback cpu {} input: {}", input_prefix, input);
        return true;
    }
    return false;
}

static void check_thermal_zones(std::string& path, std::string& input) {
    std::string sysfs_thermal = "/sys/class/thermal/";

    if (!fs::exists(sysfs_thermal))
        return;

    for (auto& d : fs::directory_iterator(sysfs_thermal)) {
        if (d.path().filename().string().substr(0, 12) != "thermal_zone")
            continue;

        std::string type = read_line(d / "type");
        if (type.substr(0, 6) != "cpuss-")
            continue;

        path = d.path();
        input = d / "temp";

        return;
    }
}

bool CPUStats::GetCpuFile() {
    if (m_cpuTempFile)
        return true;

    std::string name, path, input;
    std::string hwmon = "/sys/class/hwmon/";
    std::smatch match;

    auto dirs = ls(hwmon.c_str());
    for (auto& dir : dirs) {
        path = hwmon + dir;
        name = read_line(path + "/name");
        SPDLOG_DEBUG("hwmon: sensor name: {}", name);

        std::map<std::string, std::string> custom_sensor = get_params()->cpu_custom_temp_sensor;

        if (!custom_sensor["hwmon_name"].empty() && !custom_sensor["hwmon_input"].empty()) {
            if (name != custom_sensor["hwmon_name"])
                continue;

            find_fallback_input(path, custom_sensor["hwmon_input"].c_str(), input);
            break;
        } else if (name == "coretemp") {
            find_input(path, "temp", input, "Package id 0");
            break;
        } else if ((name == "zenpower" || name == "k10temp")) {
            if (!find_input(path, "temp", input, "Tdie"))
                find_input(path, "temp", input, "Tctl");
            break;
        } else if (name == "atk0110") {
            find_input(path, "temp", input, "CPU Temperature");
            break;
        } else if (name == "it8603") {
            find_input(path, "temp", input, "temp1");
            break;
        } else if (starts_with(name, "cpuss0_")) {
            find_fallback_input(path, "temp1", input);
            break;
        } else if (starts_with(name, "nct")) {
            // Only break if nct module has TSI0_TEMP node
            if (find_input(path, "temp", input, "TSI0_TEMP"))
                break;

        } else if (name == "asusec") {
            // Only break if module has CPU node
            if (find_input(path, "temp", input, "CPU"))
                break;
        } else if (name == "l_pcs") {
            // E2K (Elbrus 2000) CPU temperature module
            find_input(path, "temp", input, "Node 0 Max");
            break;
        } else if (std::regex_match(name, match, std::regex("cpu\\d*_thermal"))) {
            find_fallback_input(path, "temp1", input);
            break;
        } else if (name == "apm_xgene") {
            find_input(path, "temp", input, "SoC Temperature");
            break;
        } else {
            path.clear();
        }
    }

    if (path.empty()) {
        try {
            check_thermal_zones(path, input);
        } catch (fs::filesystem_error& ex) {
            SPDLOG_DEBUG("check_thermal_zones: {}", ex.what());
        }
    }

    if (input.empty() || !file_exists(input)) {
        SPDLOG_ERROR("Could not find cpu temp sensor location");
        return false;
    }

    SPDLOG_DEBUG("hwmon: using input: {}", input);
    m_cpuTempFile = fopen(input.c_str(), "r");

    return true;
}

static CPUPowerData_k10temp* init_cpu_power_data_k10temp(const std::string path) {
    auto powerData = std::make_unique<CPUPowerData_k10temp>();

    std::string coreVoltageInput, coreCurrentInput;
    std::string socVoltageInput, socCurrentInput;
    std::string socPowerInput, corePowerInput;

    if(find_input(path, "power", corePowerInput, "Pcore") && find_input(path, "power", socPowerInput, "Psoc")) {
        powerData->corePowerFile = fopen(corePowerInput.c_str(), "r");
        powerData->socPowerFile = fopen(socPowerInput.c_str(), "r");
        SPDLOG_DEBUG("hwmon: using input: {}", corePowerInput);
        SPDLOG_DEBUG("hwmon: using input: {}", socPowerInput);
        return powerData.release();
    }

    if(!find_input(path, "in", coreVoltageInput, "Vcore")) return nullptr;
    if(!find_input(path, "curr", coreCurrentInput, "Icore")) return nullptr;
    if(!find_input(path, "in", socVoltageInput, "Vsoc")) return nullptr;
    if(!find_input(path, "curr", socCurrentInput, "Isoc")) return nullptr;

    SPDLOG_DEBUG("hwmon: using input: {}", coreVoltageInput);
    SPDLOG_DEBUG("hwmon: using input: {}", coreCurrentInput);
    SPDLOG_DEBUG("hwmon: using input: {}", socVoltageInput);
    SPDLOG_DEBUG("hwmon: using input: {}", socCurrentInput);

    powerData->coreVoltageFile = fopen(coreVoltageInput.c_str(), "r");
    powerData->coreCurrentFile = fopen(coreCurrentInput.c_str(), "r");
    powerData->socVoltageFile = fopen(socVoltageInput.c_str(), "r");
    powerData->socCurrentFile = fopen(socCurrentInput.c_str(), "r");

    return powerData.release();
}

static CPUPowerData_zenpower* init_cpu_power_data_zenpower(const std::string path) {
    auto powerData = std::make_unique<CPUPowerData_zenpower>();

    std::string corePowerInput, socPowerInput;

    if(!find_input(path, "power", corePowerInput, "SVI2_P_Core")) return nullptr;
    if(!find_input(path, "power", socPowerInput, "SVI2_P_SoC")) return nullptr;

    SPDLOG_DEBUG("hwmon: using input: {}", corePowerInput);
    SPDLOG_DEBUG("hwmon: using input: {}", socPowerInput);

    powerData->corePowerFile = fopen(corePowerInput.c_str(), "r");
    powerData->socPowerFile = fopen(socPowerInput.c_str(), "r");

    return powerData.release();
}

static CPUPowerData_zenergy* init_cpu_power_data_zenergy(const std::string path) {
    auto powerData = std::make_unique<CPUPowerData_zenergy>();
    std::string energyCounterPath;

    if(!find_input(path, "energy", energyCounterPath, "Esocket0")) return nullptr;

    SPDLOG_DEBUG("hwmon: using input: {}", energyCounterPath);
    powerData->energyCounterFile = fopen(energyCounterPath.c_str(), "r");

    return powerData.release();
}

static CPUPowerData_rapl* init_cpu_power_data_rapl(const std::string path) {
    auto powerData = std::make_unique<CPUPowerData_rapl>();

    std::string energyCounterPath = path + "/energy_uj";
    if (!file_exists(energyCounterPath)) return nullptr;

    powerData->energyCounterFile = fopen(energyCounterPath.c_str(), "r");
    if (!powerData->energyCounterFile) {
        SPDLOG_DEBUG("Rapl: energy_uj is not accessible");
        powerData->energyCounterFile = nullptr;
        return nullptr;
    }

    return powerData.release();
}

static CPUPowerData_xgene* init_cpu_power_data_xgene(const std::string path) {
    auto powerData = std::make_unique<CPUPowerData_xgene>();
    std::string powerPath;

    if(!find_input(path, "power", powerPath, "CPU power")) return nullptr;

    SPDLOG_DEBUG("hwmon: using input: {}", powerPath);
    powerData->powerFile = fopen(powerPath.c_str(), "r");

    return powerData.release();
}

bool CPUStats::InitCpuPowerData() {
    if(m_cpuPowerData != nullptr)
        return true;

    // 只尝试有限次数；失败后彻底放弃，避免每帧重复扫描 sysfs。
    static int retries = 0;
    static bool unavailable = false;
    if (unavailable)
        return false;
    if (retries >= 5) {
        unavailable = true;
        return false;
    }
    retries++;
    
    std::string name, path;
    std::string hwmon = "/sys/class/hwmon/";

    CPUPowerData* cpuPowerData = nullptr;

    auto dirs = ls(hwmon.c_str());
    for (auto& dir : dirs) {
        path = hwmon + dir;
        name = read_line(path + "/name");
        SPDLOG_DEBUG("hwmon: sensor name: {}", name);

        if (name == "k10temp") {
            cpuPowerData = (CPUPowerData*)init_cpu_power_data_k10temp(path);
        } else if (name == "zenpower") {
            cpuPowerData = (CPUPowerData*)init_cpu_power_data_zenpower(path);
            break;
        } else if (name == "zenergy") {
            cpuPowerData = (CPUPowerData*)init_cpu_power_data_zenergy(path);
            break;
        } else if (name == "apm_xgene") {
            cpuPowerData = (CPUPowerData*)init_cpu_power_data_xgene(path);
            break;
        }
    }

    if (!cpuPowerData) {
        if (gpus) {
            for (auto gpu : gpus->available_gpus) {
                if (gpu->vendor_id == 0x1002 && gpu->is_apu() && gpu->get_metrics().apu_cpu_power > 0) {
                    auto powerData = std::make_unique<CPUPowerData_amdgpu>();
                    cpuPowerData = (CPUPowerData*)powerData.release();
                }
            }
        }
    }

    if (!cpuPowerData) {
        std::string powercap = "/sys/class/powercap/";
        auto powercap_dirs = ls(powercap.c_str());
        for (auto& dir : powercap_dirs) {
            path = powercap + dir;
            name = read_line(path + "/name");
            SPDLOG_DEBUG("powercap: name: {}", name);
            if (name == "package-0") {
                cpuPowerData = (CPUPowerData*)init_cpu_power_data_rapl(path);
                break;
            }
        }
    }
    
    if(cpuPowerData == nullptr) {
        // 移动端/多数 ARM 设备没有 k10temp / zenpower / RAPL 功耗计数器，
        // 这是正常情况：降为 debug 且只提示一次（外层之后会放弃重试）。
        static bool warned = false;
        if (!warned) {
            warned = true;
            SPDLOG_DEBUG("CPU power data unavailable (no k10temp/zenpower/zenergy/xgene/RAPL on this device)");
        }
        return false;
    }

    m_cpuPowerData.reset(cpuPowerData);
    return true;
}

void CPUStats::get_cpu_cores_types() {
#if defined(__x86_64__) || defined(__i386__)
    std::ifstream cpuinfo(PROCCPUINFOFILE);

    if (!cpuinfo.is_open()) {
        SPDLOG_ERROR("failed to open {}", PROCCPUINFOFILE);
        return;
    }

    std::string vendor = "unknown";
    for (std::string line; std::getline(cpuinfo, line);) {
        if (line.empty() || line.find(":") + 1 == line.length())
            continue;

        std::string key = line.substr(0, line.find(":") - 1);
        std::string val = line.substr(key.length() + 3);

        if (key == "vendor_id") {
            vendor = val;
            break;
        }
    }

    SPDLOG_INFO("cpu vendor: {}", vendor);

    if (vendor == "GenuineIntel")
        get_cpu_cores_types_intel();
#endif

#if defined(__arm__) || defined(__aarch64__)
    get_cpu_cores_types_arm();
#endif
}

void CPUStats::get_cpu_cores_types_intel() {
    for (auto const& it : intel_cores) {
        auto key = it.first;
        auto file = it.second;

        std::ifstream core_file(file);

        if (!core_file.is_open()) {
            SPDLOG_ERROR("failed to open core info file");
            return;
        }

        std::string cpus;
        std::getline(core_file, cpus);

        std::regex rx("(\\d+)-(\\d+)");
        std::smatch matches;

        if (!std::regex_match(cpus, matches, rx) || matches.size() != 3)
            continue;

        int start = 0, end = 0;

        try {
            start = std::stoi(matches[1]);
            end = std::stoi(matches[2]) + 1;
        } catch (...) {
            SPDLOG_ERROR("error parsing cpus \"{}\"", cpus);
        }

        for (int i = start; i < end; i++) {
            for (size_t k = 0; k < m_cpuData.size(); k++) {
                if (m_cpuData[k].cpu_id != i)
                    continue;

                m_cpuData[k].label = key;
                break;
            }
        }
    }
}

void CPUStats::get_cpu_cores_types_arm() {
    std::ifstream cpuinfo(PROCCPUINFOFILE);

    if (!cpuinfo.is_open()) {
        SPDLOG_ERROR("failed to open {}", PROCCPUINFOFILE);
        return;
    }

    uint8_t cur_core = 0;
    bool detected_first_core = false;

    for (std::string line; std::getline(cpuinfo, line);) {
        if (line.empty() || line.find(":") + 1 == line.length())
            continue;

        auto key = line.substr(0, line.find(":") - 1);
        auto val = line.substr(key.length() + 3);

        if (key != "CPU part")
            continue;

        if (detected_first_core)
            cur_core += 1;
        else
            detected_first_core = true;

        std::string core_type;

        try {
            core_type = arm_cores.at(val);
            SPDLOG_INFO("found {} core", core_type);
        }
        catch(const std::out_of_range& ex) {
            SPDLOG_WARN("unknown cpu part {}", val);
            continue;
        }

        // just in case
        for (size_t i = 0; i < m_cpuData.size(); i++) {
            if (m_cpuData[i].cpu_id != cur_core)
                continue;

            m_cpuData[i].label = core_type;
        }
    }
}

CPUStats cpuStats;
