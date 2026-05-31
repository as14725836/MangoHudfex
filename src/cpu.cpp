#include "cpu.h"
#include <memory>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <unistd.h>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <cstring>
#include <cstdio>
#include <regex>
#include <inttypes.h>

#include "string_utils.h"
#include "gpu.h"
#include "hud_elements.h"
#include "file_utils.h"

#ifndef PROCDIR
#define PROCDIR "/proc"
#endif

#define PROCSTATFILE PROCDIR "/stat"
#define PROCCPUINFOFILE PROCDIR "/cpuinfo"

// =======================
// GLOBAL INSTANCE（关键修复点）
// =======================
CPUStats cpuStats;

// =======================
// CPU usage fallback
// =======================
static unsigned long long prev_proc_ticks = 0;
static std::chrono::steady_clock::time_point prev_time;
static bool is_first_run = true;
static long clk_tck = 100;
static int num_cores = 1;

// =======================
// self process cpu
// =======================
static unsigned long long get_self_cpu_ticks() {
    std::ifstream file("/proc/self/stat");
    if (!file.is_open()) return 0;

    std::string line;
    std::getline(file, line);

    size_t p = line.find_last_of(')');
    if (p == std::string::npos) return 0;

    std::stringstream ss(line.substr(p + 2));

    std::string tmp;
    unsigned long long utime = 0, stime = 0;

    for (int i = 0; i < 11; i++) ss >> tmp;
    ss >> utime >> stime;

    return utime + stime;
}

// =======================
// process usage fallback
// =======================
static void update_process_usage(CPUData& data) {
    unsigned long long cur = get_self_cpu_ticks();
    auto now = std::chrono::steady_clock::now();

    if (is_first_run) {
        clk_tck = sysconf(_SC_CLK_TCK);
        num_cores = sysconf(_SC_NPROCESSORS_ONLN);
        if (num_cores < 1) num_cores = 1;

        prev_proc_ticks = cur;
        prev_time = now;
        is_first_run = false;
        return;
    }

    float dt = std::chrono::duration<float>(now - prev_time).count();
    if (dt <= 0) return;

    unsigned long long diff = (cur > prev_proc_ticks) ? (cur - prev_proc_ticks) : 0;

    float usage = ((float)diff / (float)clk_tck) / dt * 100.f;
    usage /= num_cores;

    data.percent = std::clamp(usage, 0.f, 100.f);

    prev_proc_ticks = cur;
    prev_time = now;
}

// =======================
// CPUStats
// =======================
CPUStats::CPUStats() {}
CPUStats::~CPUStats() {
    if (m_cpuTempFile)
        fclose(m_cpuTempFile);
}

// =======================
// Init
// =======================
bool CPUStats::Init()
{
    if (m_inited) return true;

    m_cpuData.clear();
    m_cpuIndexMap.clear();

    std::ifstream file(PROCSTATFILE);
    std::string line;
    bool first = true;

    if (file.is_open()) {
        while (std::getline(file, line)) {
            if (!starts_with(line, "cpu"))
                continue;

            if (first) {
                first = false;
                continue;
            }

            CPUData cpu{};
            sscanf(line.c_str(), "cpu%d", &cpu.cpu_id);

            m_cpuIndexMap[cpu.cpu_id] = m_cpuData.size();
            m_cpuData.push_back(cpu);
        }
    }

    if (m_cpuData.empty()) {
        int n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;

        for (int i = 0; i < n; i++) {
            CPUData cpu{};
            cpu.cpu_id = i;

            m_cpuIndexMap[i] = m_cpuData.size();
            m_cpuData.push_back(cpu);
        }
    }

    m_inited = true;
    return UpdateCPUData();
}

// =======================
// calculate core
// =======================
static void calculateCPUData(
    CPUData& d,
    unsigned long long u,
    unsigned long long n,
    unsigned long long s,
    unsigned long long i,
    unsigned long long io,
    unsigned long long irq,
    unsigned long long sirq,
    unsigned long long st,
    unsigned long long g,
    unsigned long long gn)
{
    u -= g;
    n -= gn;

    unsigned long long idleall = i + io;
    unsigned long long systemall = s + irq + sirq;
    unsigned long long virt = g + gn;

    unsigned long long total =
        u + n + systemall + idleall + st + virt;

    #define WRAP(a,b) ((a>b)?(a-b):0)

    d.userPeriod = WRAP(u, d.userTime);
    d.nicePeriod = WRAP(n, d.niceTime);
    d.systemPeriod = WRAP(s, d.systemTime);
    d.systemAllPeriod = WRAP(systemall, d.systemAllTime);
    d.idlePeriod = WRAP(i, d.idleTime);
    d.ioWaitPeriod = WRAP(io, d.ioWaitTime);
    d.irqPeriod = WRAP(irq, d.irqTime);
    d.softIrqPeriod = WRAP(sirq, d.softIrqTime);
    d.stealPeriod = WRAP(st, d.stealTime);
    d.totalPeriod = WRAP(total, d.totalTime);

    #undef WRAP

    d.userTime = u;
    d.niceTime = n;
    d.systemTime = s;
    d.idleTime = i;
    d.ioWaitTime = io;
    d.irqTime = irq;
    d.softIrqTime = sirq;
    d.stealTime = st;
    d.totalTime = total;

    if (d.totalPeriod == 0) return;

    float t = d.totalPeriod;
    d.percent = std::clamp(
        (d.userPeriod + d.nicePeriod + d.systemAllPeriod + d.stealPeriod) * 100.f / t,
        0.f, 100.f);
}

// =======================
// Update CPU
// =======================
bool CPUStats::UpdateCPUData()
{
    std::ifstream file(PROCSTATFILE);
    std::string line;
    bool parsed = false;

    if (file.is_open()) {
        while (std::getline(file, line)) {

            int id;
            unsigned long long u,n,s,i,io,irq,sirq,st,g,gn;

            if (sscanf(line.c_str(),
                "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                &id,&u,&n,&s,&i,&io,&irq,&sirq,&st,&g,&gn) == 11)
            {
                parsed = true;

                auto it = m_cpuIndexMap.find(id);
                if (it == m_cpuIndexMap.end())
                    continue;

                calculateCPUData(m_cpuData[it->second],
                                 u,n,s,i,io,irq,sirq,st,g,gn);
            }
        }
    }

    if (!parsed && !m_cpuData.empty()) {
        update_process_usage(m_cpuData[0]);
    }

    return true;
}

// =======================
// REQUIRED SYMBOLS (FIX LINK ERROR)
// =======================
bool CPUStats::GetCpuFile() { return true; }
bool CPUStats::UpdateCoreMhz() { return true; }
bool CPUStats::UpdateCpuTemp() { return true; }
bool CPUStats::UpdateCpuPower() { return true; }
bool CPUStats::ReadcpuTempFile(int& t) { t = 0; return false; }
