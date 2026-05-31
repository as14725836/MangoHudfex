#include "cpu.h"
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>

#define PROCSTATFILE "/proc/stat"

// =====================
// global instance（必须有）
// =====================
CPUStats cpuStats;

// =====================
// self usage fallback
// =====================
static unsigned long long prev_ticks = 0;
static std::chrono::steady_clock::time_point prev_time;
static bool first = true;
static long clk_tck = 100;
static int cores = 1;

static unsigned long long get_self_ticks() {
    std::ifstream f("/proc/self/stat");
    if (!f.is_open()) return 0;

    std::string line;
    std::getline(f, line);

    size_t p = line.find_last_of(')');
    if (p == std::string::npos) return 0;

    std::stringstream ss(line.substr(p + 2));

    std::string tmp;
    unsigned long long utime = 0, stime = 0;

    for (int i = 0; i < 11; i++) ss >> tmp;
    ss >> utime >> stime;

    return utime + stime;
}

static void update_self(CPUData& d) {
    auto now = std::chrono::steady_clock::now();
    unsigned long long cur = get_self_ticks();

    if (first) {
        clk_tck = sysconf(_SC_CLK_TCK);
        cores = sysconf(_SC_NPROCESSORS_ONLN);
        if (cores < 1) cores = 1;

        prev_ticks = cur;
        prev_time = now;
        first = false;
        return;
    }

    float dt = std::chrono::duration<float>(now - prev_time).count();
    if (dt <= 0) return;

    unsigned long long diff = (cur > prev_ticks) ? (cur - prev_ticks) : 0;

    float usage = ((float)diff / clk_tck) / dt * 100.f;
    d.percent = std::clamp(usage / cores, 0.f, 100.f);

    prev_ticks = cur;
    prev_time = now;
}

// =====================
// calculate CPU
// =====================
static void calc(CPUData& d,
    unsigned long long u,unsigned long long n,unsigned long long s,
    unsigned long long i,unsigned long long io,unsigned long long irq,
    unsigned long long sirq,unsigned long long st,
    unsigned long long g,unsigned long long gn)
{
    u -= g;
    n -= gn;

    unsigned long long idleall = i + io;
    unsigned long long systemall = s + irq + sirq;
    unsigned long long virt = g + gn;

    unsigned long long total =
        u+n+systemall+idleall+st+virt;

    #define WRAP(a,b) ((a>b)?(a-b):0)

    d.userPeriod = WRAP(u,d.userTime);
    d.nicePeriod = WRAP(n,d.niceTime);
    d.systemPeriod = WRAP(s,d.systemTime);
    d.systemAllPeriod = WRAP(systemall,d.systemAllTime);
    d.idlePeriod = WRAP(i,d.idleTime);
    d.ioWaitPeriod = WRAP(io,d.ioWaitTime);
    d.irqPeriod = WRAP(irq,d.irqTime);
    d.softIrqPeriod = WRAP(sirq,d.softIrqTime);
    d.stealPeriod = WRAP(st,d.stealTime);
    d.totalPeriod = WRAP(total,d.totalTime);

    #undef WRAP

    d.userTime=u;
    d.niceTime=n;
    d.systemTime=s;
    d.idleTime=i;
    d.ioWaitTime=io;
    d.irqTime=irq;
    d.softIrqTime=sirq;
    d.stealTime=st;
    d.totalTime=total;

    if (d.totalPeriod == 0) return;

    float t = d.totalPeriod;

    d.percent = std::clamp(
        (d.userPeriod + d.nicePeriod + d.systemAllPeriod + d.stealPeriod) * 100.f / t,
        0.f, 100.f);
}

// =====================
// Init
// =====================
bool CPUStats::Init()
{
    if (m_inited) return true;

    m_cpuData.clear();
    m_cpuIndexMap.clear();

    std::ifstream f(PROCSTATFILE);
    std::string line;
    bool firstLine = true;

    while (std::getline(f, line)) {

        if (!starts_with(line, "cpu"))
            continue;

        if (firstLine) {
            firstLine = false;
            continue;
        }

        CPUData d{};
        sscanf(line.c_str(), "cpu%d", &d.cpu_id);

        m_cpuIndexMap[d.cpu_id] = m_cpuData.size();
        m_cpuData.push_back(d);
    }

    if (m_cpuData.empty()) {
        int n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;

        for (int i = 0; i < n; i++) {
            CPUData d{};
            d.cpu_id = i;
            m_cpuIndexMap[i] = m_cpuData.size();
            m_cpuData.push_back(d);
        }
    }

    m_inited = true;
    return UpdateCPUData();
}

// =====================
// Update
// =====================
bool CPUStats::UpdateCPUData()
{
    std::ifstream f(PROCSTATFILE);
    std::string line;

    bool ok = false;

    while (std::getline(f, line)) {

        int id;
        unsigned long long u,n,s,i,io,irq,sirq,st,g,gn;

        if (sscanf(line.c_str(),
            "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
            &id,&u,&n,&s,&i,&io,&irq,&sirq,&st,&g,&gn) == 11)
        {
            ok = true;

            auto it = m_cpuIndexMap.find(id);
            if (it == m_cpuIndexMap.end())
                continue;

            calc(m_cpuData[it->second],
                u,n,s,i,io,irq,sirq,st,g,gn);
        }
    }

    if (!ok && !m_cpuData.empty()) {
        update_self(m_cpuData[0]);
    }

    return true;
}

// =====================
// REQUIRED STUBS (解决 linker)
// =====================
bool CPUStats::GetCpuFile() { return true; }
bool CPUStats::UpdateCoreMhz() { return true; }
bool CPUStats::UpdateCpuTemp() { return true; }
bool CPUStats::UpdateCpuPower() { return true; }
bool CPUStats::ReadcpuTempFile(int&) { return false; }
