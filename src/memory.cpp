#include <array>
#include <fstream>
#include <map>
#include <cctype>
#include <spdlog/spdlog.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "memory.h"
#include "file_utils.h"
#include "hud_elements.h"

float memused, memmax, swapused;
int mem_temp;
uint64_t proc_mem_resident, proc_mem_shared, proc_mem_virt;

void update_meminfo() {
    std::ifstream file("/proc/meminfo");
    std::map<std::string, float> meminfo;

    if (!file.is_open()) {
        SPDLOG_ERROR("can't open /proc/meminfo");
        return;
    }

    for (std::string line; std::getline(file, line);) {
        auto key = line.substr(0, line.find(":"));
        auto val = line.substr(key.length() + 2);
        meminfo[key] = std::stoull(val) / 1024.f / 1024.f;
    }

    memmax = meminfo["MemTotal"];
    memused = meminfo["MemTotal"] - meminfo["MemAvailable"];
    swapused = meminfo["SwapTotal"] - meminfo["SwapFree"];
}

void update_mem_temp() {
    static bool inited = false;
    static std::vector<std::ifstream> mem_temp_files;

    if (!inited) {
        inited = true;
        std::string path = "/sys/class/hwmon/";
        auto dirs = ls(path.c_str(), "hwmon", LS_DIRS);
        for (auto &dir : dirs) {
            if (read_line(path + dir + "/name") == "spd5118")
                mem_temp_files.emplace_back(path + dir + "/temp1_input");
        }
        // Android 没有 hwmon spd5118（DDR5 SPD 温度）；
        // 退而求其次：找 thermal_zone 里名字含 ddr / mem 的。
        if (mem_temp_files.empty()) {
            const std::string tz = "/sys/class/thermal/";
            auto zones = ls(tz.c_str(), "thermal_zone", LS_DIRS);
            for (auto &z : zones) {
                std::string type = read_line(tz + z + "/type");
                std::string lower = type;
                for (auto &c : lower)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (lower.find("ddr") != std::string::npos ||
                    lower.find("mem") != std::string::npos)
                    mem_temp_files.emplace_back(tz + z + "/temp");
            }
        }
        if (mem_temp_files.empty())
            SPDLOG_DEBUG("no RAM temperature sensor found on this device");
    }

    int temp = 0;
    for (auto &file : mem_temp_files) {
        int _temp;
        file.clear();
        file.seekg(0);
        if ((file >> _temp) && _temp > temp)
            temp = _temp;
    }
    mem_temp = temp / 1000;
}

void update_procmem()
{
    auto page_size = sysconf(_SC_PAGESIZE);
    if (page_size < 0) page_size = 4096;

    std::string f = "/proc/";

    {
        auto gs_pid = HUDElements.g_gamescopePid;
        f += gs_pid < 1 ? "self" : std::to_string(gs_pid);
        f += "/statm";
    }

    std::ifstream file(f);

    if (!file.is_open()) {
        SPDLOG_ERROR("can't open {}", f);
        return;
    }

    size_t last_idx = 0;
    std::string line;
    std::getline(file, line);

    if (line.empty())
        return;

    std::array<uint64_t, 3> meminfo;

    for (auto i = 0; i < 3; i++) {
        auto idx = line.find(" ", last_idx);
        auto val = line.substr(last_idx, idx);

        meminfo[i] = std::stoull(val) * page_size;
        last_idx = idx + 1;
    }

    proc_mem_virt = meminfo[0];
    proc_mem_resident = meminfo[1];
    proc_mem_shared = meminfo[2];
}
