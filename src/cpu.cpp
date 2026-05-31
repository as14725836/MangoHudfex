#include "cpu.h"
#include <memory>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
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
#include <unordered_map>

#include "string_utils.h"
#include "gpu.h"
#include "hud_elements.h"
#include "file_utils.h"

#ifndef PROCDIR
#define PROCDIR "/proc"
#endif

#ifndef PROCSTATFILE
#define PROCSTATFILE PROCDIR "/stat"
#endif

#ifndef PROCCPUINFOFILE
#define PROCCPUINFOFILE PROCDIR "/cpuinfo"
#endif

// =======================
// CPU usage process stats
// =======================
static unsigned long long prev_proc_ticks = 0;
static std::chrono::steady_clock::time_point prev_time;
static bool is_first_run = true;
static long clk_tck = 100;
static int num_cores = 1;

// =======================
// CPU ID MAP（关键修复）
// =======================
std::unordered_map<int, size_t> m_cpuIndexMap;

// =======================
// self cpu usage
// =======================
static unsigned long long get_self_cpu_ticks() {
    std::ifstream file("/proc/self/stat");
    if (!file.is_open()) return 0;

    std::string line;
    std::getline(file, line);

    size_t last_parenthesis = line.find_last_of(')');
    if (last_parenthesis == std::string::npos) return 0;

    std::stringstream ss(line.substr(last_parenthesis + 2));

    std::string val;
    unsigned long long utime = 0, stime = 0;

    for (int i = 0; i < 11; i++) ss >> val;

    ss >> utime >> stime;
    return utime + stime;
}

// =======================
// process cpu usage
// =======================
static void update_process_usage(CPUData& cpuDataTotal) {
    unsigned long long cur_ticks = get_self_cpu_ticks();
    auto cur_time = std::chrono::steady_clock::now();

    if (is_first_run) {
        clk_tck = sysconf(_SC_CLK_TCK);
        num_cores = sysconf(_SC_NPROCESSORS_ONLN);
        if (num_cores < 1) num_cores = 1;

        prev_proc_ticks = cur_ticks;
        prev_time = cur_time;
        is_first_run = false;
        return;
    }

    std::chrono::duration<float> dt = cur_time - prev_time;

    if (dt.count() > 0.0f) {
        unsigned long long diff = (cur_ticks > prev_proc_ticks)
            ? (cur_ticks - prev_proc_ticks) : 0;

        float usage = ((float)diff / (float)clk_tck) / dt.count() * 100.0f;
        usage /= (float)num_cores;

        cpuDataTotal.percent = std::clamp(usage, 0.0f, 100.0f);

        prev_proc_ticks = cur_ticks;
        prev_time = cur_time;
    }
}

// =======================
// calculate per-core data
// =======================
static void calculateCPUData(
    CPUData& cpuData,
    unsigned long long usertime,
    unsigned long long nicetime,
    unsigned long long systemtime,
    unsigned long long idletime,
    unsigned long long ioWait,
    unsigned long long irq,
    unsigned long long softIrq,
    unsigned long long steal,
    unsigned long long guest,
    unsigned long long guestnice)
{
    usertime -= guest;
    nicetime -= guestnice;

    unsigned long long idlealltime = idletime + ioWait;
    unsigned long long systemalltime = systemtime + irq + softIrq;
    unsigned long long virtalltime = guest + guestnice;

    unsigned long long totaltime =
        usertime + nicetime + systemalltime +
        idlealltime + steal + virtalltime;

    #define WRAP(a,b) ((a > b) ? (a - b) : 0)

    cpuData.userPeriod = WRAP(usertime, cpuData.userTime);
    cpuData.nicePeriod = WRAP(nicetime, cpuData.niceTime);
    cpuData.systemPeriod = WRAP(systemtime, cpuData.systemTime);
    cpuData.systemAllPeriod = WRAP(systemalltime, cpuData.systemAllTime);
    cpuData.idleAllPeriod = WRAP(idlealltime, cpuData.idleAllTime);
    cpuData.idlePeriod = WRAP(idletime, cpuData.idleTime);
    cpuData.ioWaitPeriod = WRAP(ioWait, cpuData.ioWaitTime);
    cpuData.irqPeriod = WRAP(irq, cpuData.irqTime);
    cpuData.softIrqPeriod = WRAP(softIrq, cpuData.softIrqTime);
    cpuData.stealPeriod = WRAP(steal, cpuData.stealTime);
    cpuData.guestPeriod = WRAP(virtalltime, cpuData.guestTime);
    cpuData.totalPeriod = WRAP(totaltime, cpuData.totalTime);

    #undef WRAP

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

    float usage =
        cpuData.userPeriod +
        cpuData.nicePeriod +
        cpuData.systemAllPeriod +
        cpuData.stealPeriod +
        cpuData.guestPeriod;

    cpuData.percent = std::clamp(usage * 100.0f / total, 0.0f, 100.0f);
}

// =======================
// CPUStats
// =======================
CPUStats::CPUStats() {}
CPUStats::~CPUStats() {
    if (m_cpuTempFile) fclose(m_cpuTempFile);
}

// =======================
// INIT (关键修复 MAP)
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

            if (starts_with(line, "cpu")) {

                if (first) {
                    first = false;
                    continue;
                }

                CPUData cpu{};
                sscanf(line.c_str(), "cpu%4d", &cpu.cpu_id);

                m_cpuIndexMap[cpu.cpu_id] = m_cpuData.size();
                m_cpuData.push_back(cpu);
            }
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
// UPDATE CPU DATA (FIX CORE BUG)
// =======================
bool CPUStats::UpdateCPUData()
{
    if (!m_inited) return false;

    std::ifstream file(PROCSTATFILE);
    std::string line;

    bool parsed = false;

    if (file.is_open()) {
        while (std::getline(file, line)) {

            unsigned long long u, n, s, i, io, irq, sirq, st, g, gn;
            int id;

            if (sscanf(line.c_str(),
                "cpu%4d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                &id, &u,&n,&s,&i,&io,&irq,&sirq,&st,&g,&gn) == 11)
            {
                parsed = true;

                auto it = m_cpuIndexMap.find(id);
                if (it == m_cpuIndexMap.end())
                    continue;

                calculateCPUData(
                    m_cpuData[it->second],
                    u,n,s,i,io,irq,sirq,st,g,gn
                );
            }
        }
    }

    if (!parsed) {
        update_process_usage(m_cpuData[0]);
    }

    return true;
}
