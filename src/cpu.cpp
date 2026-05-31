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
#include <spdlog/spdlog.h>
#include "gpu.h"
#include "file_utils.h"
#include <dirent.h>
#include <regex>

// 记录上一帧的状态
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
    
    size_t last_parenthesis = line.find_last_of(')');
    if (last_parenthesis == std::string::npos || last_parenthesis + 2 >= line.length()) return 0;

    std::stringstream ss(line.substr(last_parenthesis + 2));
    
    std::string val;
    unsigned long long utime = 0, stime = 0;
    
    for (int i = 0; i < 11; i++) ss >> val;
    
    ss >> utime >> stime;
    return utime + stime;
}

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

// 读取CPU频率的函数
bool CPUStats::UpdateCoreMhz() {
    // 清空现有的频率数据
    for (auto& cpu : m_cpuData) {
        cpu.mhz = 0.0f;
    }
    
    // 正则表达式匹配 cpu0, cpu1, cpu2, ... 等目录
    std::regex cpu_regex("cpu([0-9]+)");
    
    // 尝试读取 /sys/devices/system/cpu/ 目录下的cpu核心
    std::string cpu_base_path = "/sys/devices/system/cpu/";
    DIR* dir = opendir(cpu_base_path.c_str());
    if (!dir) {
        spdlog::warn("无法打开CPU设备目录: {}", cpu_base_path);
        return false;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string dir_name = entry->d_name;
        std::smatch matches;
        
        // 检查是否是cpu核心目录（如cpu0, cpu1等）
        if (std::regex_match(dir_name, matches, cpu_regex) && matches.size() > 1) {
            int cpu_id = std::stoi(matches[1]);
            
            // 构建频率文件路径
            std::string freq_file_path = cpu_base_path + dir_name + "/cpufreq/scaling_cur_freq";
            
            std::ifstream freq_file(freq_file_path);
            if (freq_file.is_open()) {
                std::string line;
                if (std::getline(freq_file, line)) {
                    try {
                        // 读取的是KHz，转换为MHz
                        float freq_khz = std::stof(line);
                        float freq_mhz = freq_khz / 1000.0f;
                        
                        // 确保CPU ID在有效范围内
                        if (cpu_id >= 0 && cpu_id < static_cast<int>(m_cpuData.size())) {
                            m_cpuData[cpu_id].mhz = freq_mhz;
                        }
                        
                        // 同时更新总CPU数据的最大频率
                        if (freq_mhz > m_cpuDataTotal.mhz) {
                            m_cpuDataTotal.mhz = freq_mhz;
                        }
                        
                    } catch (const std::exception& e) {
                        spdlog::warn("解析CPU{}频率失败: {}", cpu_id, e.what());
                    }
                }
                freq_file.close();
            } else {
                // 尝试备选路径（有些设备可能用不同的位置）
                std::string alt_freq_file_path = cpu_base_path + dir_name + "/cpufreq/cpuinfo_cur_freq";
                std::ifstream alt_freq_file(alt_freq_file_path);
                if (alt_freq_file.is_open()) {
                    std::string line;
                    if (std::getline(alt_freq_file, line)) {
                        try {
                            float freq_khz = std::stof(line);
                            float freq_mhz = freq_khz / 1000.0f;
                            
                            if (cpu_id >= 0 && cpu_id < static_cast<int>(m_cpuData.size())) {
                                m_cpuData[cpu_id].mhz = freq_mhz;
                            }
                            
                            if (freq_mhz > m_cpuDataTotal.mhz) {
                                m_cpuDataTotal.mhz = freq_mhz;
                            }
                            
                        } catch (const std::exception& e) {
                            spdlog::warn("解析CPU{}备选频率失败: {}", cpu_id, e.what());
                        }
                    }
                    alt_freq_file.close();
                }
            }
        }
    }
    
    closedir(dir);
    
    // 计算平均频率（如果所有核心频率都可用）
    float total_freq = 0.0f;
    int valid_cores = 0;
    for (const auto& cpu : m_cpuData) {
        if (cpu.mhz > 0) {
            total_freq += cpu.mhz;
            valid_cores++;
        }
    }
    
    if (valid_cores > 0) {
        m_cpuDataTotal.avg_mhz = total_freq / valid_cores;
    } else {
        m_cpuDataTotal.avg_mhz = 0.0f;
    }
    
    return true;
}

CPUStats::CPUStats() {}
CPUStats::~CPUStats() { if (m_cpuTempFile) fclose(m_cpuTempFile); }

bool CPUStats::Init() {
    m_inited = true;
    
    // 获取CPU核心数
    num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores < 1) num_cores = 1;
    
    // 根据实际核心数创建CPU数据
    m_cpuData.clear();
    for(int i = 0; i < num_cores; i++) {
        CPUData cpu = {};
        cpu.cpu_id = i;
        cpu.mhz = 0.0f;
        m_cpuData.push_back(cpu);
    }
    
    // 初始化总CPU数据
    m_cpuDataTotal.cpu_id = -1; // 总CPU的ID设为-1
    m_cpuDataTotal.mhz = 0.0f;
    m_cpuDataTotal.avg_mhz = 0.0f;
    m_cpuDataTotal.percent = 0.0f;
    
    return true;
}

bool CPUStats::Reinit() { 
    return Init(); 
}

bool CPUStats::UpdateCPUData() {
    // 更新进程CPU使用率
    update_process_usage(m_cpuDataTotal);
    
    // 更新CPU频率
    UpdateCoreMhz();
    
    m_updatedCPUs = true;
    return true;
}

// 简单版本（不使用目录遍历）
bool CPUStats::UpdateCoreMhzSimple() {
    // 重置频率数据
    m_cpuDataTotal.mhz = 0.0f;
    m_cpuDataTotal.avg_mhz = 0.0f;
    
    float total_freq = 0.0f;
    int valid_cores = 0;
    
    // 尝试读取所有可能的CPU核心（假设最多32个）
    for (int i = 0; i < num_cores; i++) {
        std::string freq_file_path = "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/cpufreq/scaling_cur_freq";
        std::ifstream freq_file(freq_file_path);
        
        if (freq_file.is_open()) {
            std::string line;
            if (std::getline(freq_file, line)) {
                try {
                    float freq_khz = std::stof(line);
                    float freq_mhz = freq_khz / 1000.0f;
                    
                    // 更新单个核心频率
                    if (i < static_cast<int>(m_cpuData.size())) {
                        m_cpuData[i].mhz = freq_mhz;
                    }
                    
                    // 更新最高频率
                    if (freq_mhz > m_cpuDataTotal.mhz) {
                        m_cpuDataTotal.mhz = freq_mhz;
                    }
                    
                    total_freq += freq_mhz;
                    valid_cores++;
                    
                } catch (const std::exception& e) {
                    spdlog::debug("解析CPU{}频率失败: {}", i, e.what());
                }
            }
            freq_file.close();
        }
    }
    
    // 计算平均频率
    if (valid_cores > 0) {
        m_cpuDataTotal.avg_mhz = total_freq / valid_cores;
    }
    
    return (valid_cores > 0);
}

bool CPUStats::ReadcpuTempFile(int& temp) {
    if (!m_cpuTempFile) return false;
    rewind(m_cpuTempFile);
    bool ret = (fscanf(m_cpuTempFile, "%d", &temp) == 1);
    temp = temp / 1000;
    return ret;
}

bool CPUStats::UpdateCpuTemp() {
    if (gpus && !gpus->available_gpus.empty()) {
        m_cpuDataTotal.temp = gpus->available_gpus[0]->metrics.temp;
    } else {
        FILE* fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
        if (fp) {
            int t;
            if (fscanf(fp, "%d", &t) == 1) m_cpuDataTotal.temp = t / 1000;
            fclose(fp);
        }
    }
    return true;
}

bool CPUStats::UpdateCpuPower() { return true; }
bool CPUStats::GetCpuFile() { return true; }
bool CPUStats::InitCpuPowerData() { return true; }
void CPUStats::get_cpu_cores_types() {}
void CPUStats::get_cpu_cores_types_intel() {}
void CPUStats::get_cpu_cores_types_arm() {}

CPUStats cpuStats;
