#include "gpu_fdinfo.h"
#include "string_utils.h"

#ifndef TEST_ONLY
#include "hud_elements.h"
#endif

namespace fs = ghc::filesystem;

void GPU_fdinfo::find_fd()
{
    fdinfo.clear();
    fdinfo_data.clear();

    auto dir = std::string("/proc/") + std::to_string(pid) + "/fdinfo";
    auto path = fs::path(dir);

    SPDLOG_TRACE("fdinfo_dir = {}", dir);

    if (!fs::exists(path)) {
        SPDLOG_DEBUG("{} does not exist", path.string());
        return;
    }

    // Here we store client-ids, if ids match, we dont open this file,
    // because it will have same readings and it becomes a duplicate
    std::set<std::string> client_ids;
    int total = 0;

    for (const auto& entry : fs::directory_iterator(path)) {
        auto fd_path = entry.path().string();
        auto file = std::ifstream(fd_path);

        if (!file.is_open())
            continue;

        std::string driver, pdev, client_id;

        for (std::string line; std::getline(file, line);) {
            size_t colon = line.find(":");

            if (line[0] == ' ' || line[0] == '\t')
                continue;

            if (colon == std::string::npos || colon + 2 >= line.length())
                continue;

            auto key = line.substr(0, colon);
            auto val = line.substr(key.length() + 2);

            if (key == "drm-driver")
                driver = val;
            else if (key == "drm-pdev")
                pdev = val;
            else if (key == "drm-client-id")
                client_id = val;
        }

        if (!driver.empty() && driver == module) {
            total++;
            SPDLOG_TRACE(
                "driver = \"{}\", pdev = \"{}\", "
                "client_id = \"{}\", client_id_exists = \"{}\"",
                driver, pdev,
                client_id, client_ids.find(client_id) != client_ids.end()
            );
        }

        if (
            driver.empty() || client_id.empty() ||
            driver != module || pdev != pci_dev ||
            client_ids.find(client_id) != client_ids.end()
        )
            continue;

        client_ids.insert(client_id);
        open_fdinfo_fd(fd_path);
    }

    SPDLOG_TRACE(
        "Found {} total fds. Opened {} unique fds.",
        total,
        fdinfo.size()
    );
}

void GPU_fdinfo::open_fdinfo_fd(std::string path) {
    fdinfo.push_back(std::ifstream(path));
    fdinfo_data.push_back({});
}

void GPU_fdinfo::gather_fdinfo_data() {
    for (size_t i = 0; i < fdinfo.size(); i++) {
        fdinfo[i].clear();
        fdinfo[i].seekg(0);

        for (std::string line; std::getline(fdinfo[i], line);) {
            size_t colon = line.find(":");

            if (line[0] == ' ' || line[0] == '\t')
                continue;

            if (colon == std::string::npos || colon + 2 >= line.length())
                continue;

            auto key = line.substr(0, line.find(":"));
            auto val = line.substr(key.length() + 2);
            fdinfo_data[i][key] = val;
        }
    }
}

uint64_t GPU_fdinfo::get_gpu_time()
{
    uint64_t total = 0;

    if (module == "panfrost")
        return get_gpu_time_panfrost();

    for (auto& fd : fdinfo_data) {
        auto time = fd[drm_engine_type];

        if (time.empty())
            continue;

        total += std::stoull(time);
    }

    return total;
}

uint64_t GPU_fdinfo::get_gpu_time_panfrost() {
    uint64_t total = 0;

    for (auto& fd : fdinfo_data) {
        auto frag = fd["drm-engine-fragment"];
        auto vert = fd["drm-engine-vertex-tiler"];

        if (!frag.empty())
            total += std::stoull(frag);

        if (!vert.empty())
            total += std::stoull(vert);
    }

    return total;
}

float GPU_fdinfo::get_memory_used()
{
    uint64_t total = 0;

    for (auto& fd : fdinfo_data) {
        auto mem = fd[drm_memory_type];

        if (mem.empty())
            continue;

        std::string unit = mem.substr(mem.rfind(" ") + 1);
        uint64_t val = std::stoull(mem);

        if (unit == "KiB")
            val *= 1024;
        else if (unit == "MiB")
            val *= 1024 * 1024;
        else if (unit == "GiB")
            val *= 1024 * 1024 * 1024;

        total += val;
    }

    return static_cast<float>(total) / 1024 / 1024 / 1024;
}

void GPU_fdinfo::find_hwmon_sensors()
{
    std::string hwmon;

    if (module == "msm")
        hwmon = find_hwmon_sensor_dir("gpu");
    else if (module == "panfrost" || module == "panthor")
        hwmon = find_hwmon_sensor_dir("gpu_thermal");
    else
        hwmon = find_hwmon_dir();

    if (hwmon.empty()) {
        SPDLOG_DEBUG("hwmon: failed to find hwmon directory");
        return;
    }

    SPDLOG_DEBUG("hwmon: checking \"{}\" directory", hwmon);

    for (const auto &entry : fs::directory_iterator(hwmon)) {
        auto filename = entry.path().filename().string();

        for (auto& hs : hwmon_sensors) {
            auto key = hs.first;
            auto sensor = &hs.second;
            std::smatch matches;

            if (
                !std::regex_match(filename, matches, sensor->rx) ||
                matches.size() != 2
            )
                continue;

            auto cur_id = std::stoull(matches[1].str());

            if (sensor->filename.empty() || cur_id < sensor->id) {
                sensor->filename = entry.path().string();
                sensor->id = cur_id;
            }
        }
    }

    for (auto& hs : hwmon_sensors) {
        auto key = hs.first;
        auto sensor = &hs.second;

        if (sensor->filename.empty()) {
            SPDLOG_DEBUG("hwmon: {} reading not found at {}", key, hwmon);
            continue;
        }

        SPDLOG_DEBUG("hwmon: {} reading found at {}", key, sensor->filename);

        sensor->stream.open(sensor->filename);

        if (!sensor->stream.good()) {
            SPDLOG_DEBUG(
                "hwmon: failed to open {} reading {}",
                key, sensor->filename
            );
            continue;
        }
    }
}

std::string GPU_fdinfo::find_hwmon_dir() {
    std::string d = "/sys/class/drm/" + drm_node + "/device/hwmon";

    if (!fs::exists(d)) {
        SPDLOG_DEBUG("hwmon: hwmon directory \"{}\" doesn't exist", d);
        return "";
    }

    auto dir_iterator = fs::directory_iterator(d);
    auto hwmon = dir_iterator->path().string();

    if (hwmon.empty()) {
        SPDLOG_DEBUG("hwmon: hwmon directory \"{}\" is empty.", d);
        return "";
    }

    return hwmon;
}

std::string GPU_fdinfo::find_hwmon_sensor_dir(std::string name) {
    std::string d = "/sys/class/hwmon/";

    if (!fs::exists(d))
        return "";

    for (const auto &entry : fs::directory_iterator(d)) {
        auto hwmon_dir = entry.path().string();
        auto hwmon_name = hwmon_dir + "/name";

        std::ifstream name_stream(hwmon_name);
        std::string name_content;

        if (!name_stream.is_open())
            continue;

        std::getline(name_stream, name_content);

        if (name_content.find(name) == std::string::npos)
            continue;

        // return the first gpu sensor
        return hwmon_dir;
    }

    return "";
}

void GPU_fdinfo::get_current_hwmon_readings()
{
    for (auto& hs : hwmon_sensors) {
        auto key = hs.first;
        auto sensor = &hs.second;

        if (!sensor->stream.is_open())
            continue;

        sensor->stream.seekg(0);

        std::stringstream ss;
        ss << sensor->stream.rdbuf();

        if (ss.str().empty())
            continue;

        sensor->val = std::stoull(ss.str());
    }
}

float GPU_fdinfo::get_power_usage()
{
    if (!hwmon_sensors["power"].filename.empty())
        return static_cast<float>(hwmon_sensors["power"].val) / 1'000'000;

    float now = hwmon_sensors["energy"].val;

    // Initialize value for the first time, otherwise delta will be very large
    // and your gpu power usage will be like 1 million watts for a second.
    if (this->last_power == 0.f)
        this->last_power = now;

    float delta = now - this->last_power;
    delta /= METRICS_UPDATE_PERIOD_MS / 1000.f;

    this->last_power = now;

    return delta / 1'000'000;
}

int GPU_fdinfo::get_xe_load()
{
    double load = 0;

    for (auto& fd : fdinfo_data) {
        std::string client_id = fd["drm-client-id"];
        std::string cur_cycles_str = fd["drm-cycles-rcs"];
        std::string cur_total_cycles_str = fd["drm-total-cycles-rcs"];

        if (
            client_id.empty() || cur_cycles_str.empty() ||
            cur_total_cycles_str.empty()
        )
            continue;

        auto cur_cycles = std::stoull(cur_cycles_str);
        auto cur_total_cycles = std::stoull(cur_total_cycles_str);

        if (prev_xe_cycles.find(client_id) == prev_xe_cycles.end()) {
            prev_xe_cycles[client_id] = { cur_cycles, cur_total_cycles };
            continue;
        }

        auto prev_cycles = prev_xe_cycles[client_id].first;
        auto prev_total_cycles = prev_xe_cycles[client_id].second;

        auto delta_cycles = cur_cycles - prev_cycles;
        auto delta_total_cycles = cur_total_cycles - prev_total_cycles;

        prev_xe_cycles[client_id] = { cur_cycles, cur_total_cycles };

        if (delta_cycles <= 0 || delta_total_cycles <= 0)
            continue;

        auto fd_load = static_cast<double>(delta_cycles) / delta_total_cycles * 100;
        load += fd_load;
    }

    if (load > 100.f)
        load = 100.f;

    return std::lround(load);
}

int GPU_fdinfo::get_gpu_load()
{
    if (module == "xe")
        return get_xe_load();
    else if (module == "msm_drm")
        return get_kgsl_load();

    uint64_t now = os_time_get_nano();
    uint64_t gpu_time_now = get_gpu_time();

    if (previous_time == 0) {
        previous_gpu_time = gpu_time_now;
        previous_time = now;

        return 0;
    }

    float delta_time = now - previous_time;
    float delta_gpu_time = gpu_time_now - previous_gpu_time;

    int result = delta_gpu_time / delta_time * 100;

    if (result > 100)
        result = 100;

    previous_gpu_time = gpu_time_now;
    previous_time = now;

    return std::round(result);
}

void GPU_fdinfo::find_i915_gt_dir()
{
    std::string device = "/sys/bus/pci/devices/" + pci_dev + "/drm";

    // Find first dir which starts with name "card"
    for (const auto& entry : fs::directory_iterator(device)) {
        auto path = entry.path().string();

        if (path.substr(device.size() + 1, 4) == "card") {
            device = path;
            break;
        }
    }

    auto gpu_clock_path = device + "/gt_act_freq_mhz";
    gpu_clock_stream.open(gpu_clock_path);

    if (!gpu_clock_stream.good())
        SPDLOG_WARN("Intel i915 gt dir: failed to open {}", device);

    // Assuming gt0 since all recent GPUs have the RCS engine on gt0,
    // and latest GPUs need Xe anyway
    auto throttle_folder = device + "/gt/gt0/throttle_";
    auto throttle_status_path = throttle_folder + "reason_status";

    throttle_status_stream.open(throttle_status_path);
    if (!throttle_status_stream.good()) {
       SPDLOG_WARN("Intel i915 gt dir: failed to open {}", throttle_status_path);
    } else {
        load_xe_i915_throttle_reasons(throttle_folder,
                                      intel_throttle_power,
                                      throttle_power_streams);

        load_xe_i915_throttle_reasons(throttle_folder,
                                      intel_throttle_current,
                                      throttle_current_streams);

        load_xe_i915_throttle_reasons(throttle_folder,
                                      intel_throttle_temp,
                                      throttle_temp_streams);
    }
}

void GPU_fdinfo::find_xe_gt_dir()
{
    std::string device = "/sys/bus/pci/devices/" + pci_dev + "/tile0";

    if (!fs::exists(device)) {
        SPDLOG_WARN(
            "\"{}\" doesn't exist. GPU clock will be unavailable.",
            device
        );
        return;
    }

    bool has_rcs = true;

    // Check every "gt" dir if it has "engines/rcs" inside
    for (const auto& entry : fs::directory_iterator(device)) {
        auto path = entry.path().string();

        if (path.substr(device.size() + 1, 2) != "gt")
            continue;

        SPDLOG_DEBUG("Checking \"{}\" for rcs.", path);

        if (!fs::exists(path + "/engines/rcs")) {
            SPDLOG_DEBUG("Skipping \"{}\" because rcs doesn't exist.", path);
            continue;
        }

        SPDLOG_DEBUG("Found rcs in \"{}\"", path);
        has_rcs = true;
        device = path;
        break;

    }

    if (!has_rcs) {
        SPDLOG_WARN(
            "rcs not found inside \"{}\". GPU clock will not be available.",
            device
        );
        return;
    }

    auto gpu_clock_path = device + "/freq0/act_freq";
    gpu_clock_stream.open(gpu_clock_path);

    if (!gpu_clock_stream.good())
        SPDLOG_WARN("Intel xe gt dir: failed to open {}", gpu_clock_path);

    auto throttle_folder = device + "/freq0/throttle/";
    auto throttle_status_path = throttle_folder + "status";

    throttle_status_stream.open(throttle_status_path);
    if (!throttle_status_stream.good()) {
       SPDLOG_WARN("Intel xe gt dir: failed to open {}", throttle_status_path);
    } else {
        load_xe_i915_throttle_reasons(throttle_folder,
                                      intel_throttle_power,
                                      throttle_power_streams);

        load_xe_i915_throttle_reasons(throttle_folder,
                                      intel_throttle_current,
                                      throttle_current_streams);

        load_xe_i915_throttle_reasons(throttle_folder,
                                      intel_throttle_temp,
                                      throttle_temp_streams);
    }
}

void GPU_fdinfo::load_xe_i915_throttle_reasons(
    std::string throttle_folder,
    std::vector<std::string> throttle_reasons,
    std::vector<std::ifstream>& throttle_reason_streams
) {
    for (const auto& throttle_reason : throttle_reasons) {
        std::string throttle_path = throttle_folder + throttle_reason;
        if (!fs::exists(throttle_path)) {
            SPDLOG_WARN(
                "Intel xe/i915 gt dir: Throttle file {} not found",
                throttle_path
            );
            continue;
        }
        auto throttle_stream = std::ifstream(throttle_path);
        if (!throttle_stream.good()) {
            SPDLOG_WARN("Intel xe/i915 gt dir: failed to open {}", throttle_path);
            continue;
        }
        throttle_reason_streams.push_back(std::move(throttle_stream));
    }
}

int GPU_fdinfo::get_gpu_clock()
{
    if (module == "panfrost" || module == "panthor")
        return get_gpu_clock_mali();

    if (!gpu_clock_stream.is_open())
        return 0;

    std::string clock_str;

    gpu_clock_stream.seekg(0);

    std::getline(gpu_clock_stream, clock_str);

    if (clock_str.empty())
        return 0;

    try {
        double freq = std::stod(clock_str);
        
        // 处理不同单位的频率值
        // 如果值大于 1e7，认为是 Hz 单位，转换为 MHz
        if (freq > 1e7) {
            freq = freq / 1e6;
        }
        // 如果值大于 1e4，认为是 KHz 单位，转换为 MHz
        else if (freq > 1e4) {
            freq = freq / 1e3;
        }
        // 否则认为已经是 MHz 单位
        
        return std::round(freq);
    } catch (...) {
        return 0;
    }
}

int GPU_fdinfo::get_gpu_clock_mali() {
    if (fdinfo_data.empty())
        return 0;

    std::string key;

    if (module == "panfrost")
        key = "drm-curfreq-fragment";
    else if (module == "panthor")
        key = "drm-curfreq-panthor";

    std::string freq_str = fdinfo_data[0][key];

    if (freq_str.empty())
        return 0;

    float freq = std::stoull(freq_str) / 1'000'000;

    return std::round(freq);
}

bool GPU_fdinfo::check_throttle_reasons(
    std::vector<std::ifstream>& throttle_reason_streams)
{
    for (auto& throttle_reason_stream : throttle_reason_streams) {
        std::string throttle_reason_str;
        throttle_reason_stream.seekg(0);
        std::getline(throttle_reason_stream, throttle_reason_str);

        if (throttle_reason_str == "1")
            return true;
    }

    return false;
}

int GPU_fdinfo::get_throttling_status()
{
    if (!throttle_status_stream.is_open())
        return 0;

    std::string throttle_status_str;
    throttle_status_stream.seekg(0);
    std::getline(throttle_status_stream, throttle_status_str);

    if (throttle_status_str != "1")
        return 0;

    int reasons =
        check_throttle_reasons(throttle_power_streams) * GPU_throttle_status::POWER +
        check_throttle_reasons(throttle_current_streams) * GPU_throttle_status::CURRENT +
        check_throttle_reasons(throttle_temp_streams) * GPU_throttle_status::TEMP;
    // No throttle reasons for OTHER currently
    if (reasons == 0)
        reasons |= GPU_throttle_status::OTHER;

    return reasons;
}

float GPU_fdinfo::amdgpu_helper_get_proc_vram() {
#ifndef TEST_ONLY
    if (HUDElements.g_gamescopePid > 0 && HUDElements.g_gamescopePid != pid)
    {
        pid = HUDElements.g_gamescopePid;
        find_fd();
    }
#endif

    // Recheck fds every 10secs, fixes Mass Effect 1, maybe some others too
    {
        auto t = os_time_get_nano() / 1'000'000;
        if (t - fdinfo_last_update_ms >= 10'000) {
            find_fd();
            fdinfo_last_update_ms = t;
        }
    }

    gather_fdinfo_data();

    return get_memory_used();
}

void GPU_fdinfo::init_kgsl() {
    const std::string sys_path = "/sys/class/kgsl/kgsl-3d0";

    try {
        if (!fs::exists(sys_path)) {
            SPDLOG_WARN("kgsl: {} is not found. kgsl stats will not work!", sys_path);
            return;
        }
    } catch (const fs::filesystem_error& ex) {
        SPDLOG_WARN("kgsl: {}", ex.what());
        return;
    } catch (const std::exception& ex) {
        SPDLOG_WARN("kgsl: Error checking sys_path: {}", ex.what());
        return;
    }

    try {
        // 尝试多个 GPU 频率路径
        const char* freq_paths[] = {
            "/sys/kernel/gpu/gpu_clock",
            "/sys/class/kgsl/kgsl-3d0/gpuclk",
            "/sys/class/kgsl/kgsl-3d0/cur_freq",
            "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
            nullptr
        };

        for (int i = 0; freq_paths[i]; i++) {
            try {
                if (fs::exists(freq_paths[i])) {
                    SPDLOG_DEBUG("kgsl: GPU freq path found: {}", freq_paths[i]);
                    gpu_clock_stream.open(freq_paths[i]);
                    if (gpu_clock_stream.is_open())
                        break;
                }
            } catch (...) {
                // 继续尝试下一个路径
            }
        }

        // 如果上述路径都失败，尝试 clock_mhz
        if (!gpu_clock_stream.is_open()) {
            std::string clock_path = sys_path + "/clock_mhz";
            try {
                if (fs::exists(clock_path)) {
                    SPDLOG_DEBUG("kgsl: {} found", clock_path);
                    gpu_clock_stream.open(clock_path);
                }
            } catch (...) {
                // 忽略错误
            }
        }

        // 尝试多个 GPU 使用率路径
        const char* load_paths[] = {
            "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
            "/sys/kernel/gpu/gpu_busy",
            "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
            "/sys/class/kgsl/kgsl-3d0/devfreq/gpu_load",
            nullptr
        };

        for (int i = 0; load_paths[i]; i++) {
            try {
                if (fs::exists(load_paths[i])) {
                    SPDLOG_DEBUG("kgsl: GPU load path found: {}", load_paths[i]);
                    kgsl_streams["gpu_busy_percentage"].open(load_paths[i]);
                    if (kgsl_streams["gpu_busy_percentage"].is_open())
                        break;
                }
            } catch (...) {
                // 继续尝试下一个路径
            }
        }

        // 尝试 gpubusy 文件（返回 "busy_time total_time"）
        std::string gpubusy_path = sys_path + "/gpubusy";
        try {
            if (fs::exists(gpubusy_path)) {
                SPDLOG_DEBUG("kgsl: gpubusy path found: {}", gpubusy_path);
                kgsl_streams["gpubusy"].open(gpubusy_path);
            }
        } catch (...) {
            // 忽略错误
        }

        // 温度路径
        std::string temp_path = sys_path + "/temp";
        try {
            if (fs::exists(temp_path)) {
                SPDLOG_DEBUG("kgsl: {} found", temp_path);
                kgsl_streams["temp"].open(temp_path);
            }
        } catch (...) {
            // 忽略错误
        }
    } catch (const std::exception& ex) {
        SPDLOG_WARN("kgsl: Error during initialization: {}", ex.what());
    }
}

int GPU_fdinfo::get_kgsl_load() {
    // 首先尝试 gpu_busy_percentage 文件
    std::ifstream* s = &kgsl_streams["gpu_busy_percentage"];

    if (s->is_open()) {
        std::string usage_str;
        s->seekg(0);
        std::getline(*s, usage_str);

        if (!usage_str.empty()) {
            try {
                return safe_stoi(usage_str);
            } catch (...) {
                // 解析失败，继续尝试其他方法
            }
        }
    }

    // 尝试 gpubusy 文件（返回 "busy_time total_time"）
    std::ifstream* busy_s = &kgsl_streams["gpubusy"];

    if (busy_s->is_open()) {
        std::string busy_str;
        busy_s->seekg(0);
        std::getline(*busy_s, busy_str);

        if (!busy_str.empty()) {
            std::istringstream iss(busy_str);
            double busy_time = 0, total_time = 0;
            if (iss >> busy_time >> total_time) {
                if (total_time > 0) {
                    return static_cast<int>(busy_time / total_time * 100.0);
                } else {
                    return 0; // GPU 空闲
                }
            }
        }
    }

    return 0;
}

int GPU_fdinfo::get_kgsl_temp() {
    // 首先尝试 kgsl 温度文件
    std::ifstream* s = &kgsl_streams["temp"];

    if (s->is_open()) {
        std::string temp_str;
        s->seekg(0);
        std::getline(*s, temp_str);

        if (!temp_str.empty()) {
            try {
                return std::round(safe_stoi(temp_str) / 1'000.f);
            } catch (...) {
                // 解析失败，继续尝试其他方法
            }
        }
    }

    // 尝试从 thermal zones 查找 GPU 温度
    static int cached_gpu_thermal_zone = -1;
    static bool thermal_zone_searched = false;

    if (!thermal_zone_searched) {
        thermal_zone_searched = true;
        std::string sysfs_thermal = "/sys/class/thermal/";

        if (fs::exists(sysfs_thermal)) {
            for (const auto& entry : fs::directory_iterator(sysfs_thermal)) {
                std::string filename = entry.path().filename().string();
                if (filename.substr(0, 12) != "thermal_zone")
                    continue;

                std::string type_file = entry.path().string() + "/type";
                std::ifstream type_stream(type_file);
                std::string type;

                if (type_stream.is_open() && std::getline(type_stream, type)) {
                    // 查找包含 "gpuss" 的 thermal zone
                    if (type.find("gpuss") != std::string::npos) {
                        // 提取 thermal zone 编号
                        try {
                            cached_gpu_thermal_zone = safe_stoi(filename.substr(12));
                            SPDLOG_DEBUG("Found GPU thermal zone: {} with type: {}", filename, type);
                            break;
                        } catch (...) {
                            continue;
                        }
                    }
                }
            }
        }
    }

    if (cached_gpu_thermal_zone >= 0) {
        std::string temp_file = "/sys/class/thermal/thermal_zone" + 
                               std::to_string(cached_gpu_thermal_zone) + "/temp";
        std::ifstream temp_stream(temp_file);
        std::string temp_str;

        if (temp_stream.is_open() && std::getline(temp_stream, temp_str)) {
            try {
                double temp = std::stod(temp_str);
                // 温度通常是毫度，需要除以 1000
                if (temp > 1000)
                    temp /= 1000.0;
                return std::round(temp);
            } catch (...) {
                // 解析失败
            }
        }
    }

    return 0;
}

void GPU_fdinfo::main_thread()
{
    while (!stop_thread) {
        std::unique_lock<std::mutex> lock(metrics_mutex);
        cond_var.wait(lock, [this]() { return !paused || stop_thread; });

#ifndef TEST_ONLY
        if (HUDElements.g_gamescopePid > 0 && HUDElements.g_gamescopePid != pid)
        {
            pid = HUDElements.g_gamescopePid;
            find_fd();
        }
#endif

        // Recheck fds every 10secs, fixes Mass Effect 1, maybe some others too
        {
            auto t = os_time_get_nano() / 1'000'000;
            if (t - fdinfo_last_update_ms >= 10'000) {
                find_fd();
                fdinfo_last_update_ms = t;
            }
        }

        gather_fdinfo_data();
        get_current_hwmon_readings();

        metrics.load = get_gpu_load();
        metrics.proc_vram_used = get_memory_used();

        metrics.powerUsage = get_power_usage();
        metrics.powerLimit = static_cast<float>(hwmon_sensors["power_limit"].val) / 1'000'000;

        metrics.CoreClock = get_gpu_clock();
        metrics.voltage = hwmon_sensors["voltage"].val;

        if (module == "msm_drm" || module == "msm_dpu") {
            metrics.temp = get_kgsl_temp();
            // 如果 kgsl 温度不可用，尝试 hwmon
            if (metrics.temp == 0 && hwmon_sensors["temp"].val > 0)
                metrics.temp = hwmon_sensors["temp"].val / 1000.f;
        } else {
            metrics.temp = hwmon_sensors["temp"].val / 1000.f;
        }

        metrics.memory_temp = hwmon_sensors["vram_temp"].val / 1000.f;

        metrics.fan_speed = hwmon_sensors["fan_speed"].val;
        metrics.fan_rpm = true; // Fan data is pulled from hwmon

        int throttling = get_throttling_status();
        metrics.is_power_throttled = throttling & GPU_throttle_status::POWER;
        metrics.is_current_throttled = throttling & GPU_throttle_status::CURRENT;
        metrics.is_temp_throttled = throttling & GPU_throttle_status::TEMP;
        metrics.is_other_throttled = throttling & GPU_throttle_status::OTHER;

        SPDLOG_DEBUG(
            "pci_dev = {}, pid = {}, module = {}, "
            "load = {}, proc_vram = {}, power = {}, "
            "core = {}, temp = {}, fan = {}, "
            "voltage = {}",
            pci_dev, pid, module,
            metrics.load, metrics.proc_vram_used, metrics.powerUsage,
            metrics.CoreClock, metrics.temp, metrics.fan_speed,
            metrics.voltage
        );

        std::this_thread::sleep_for(
            std::chrono::milliseconds(METRICS_UPDATE_PERIOD_MS)
        );
    }
}
