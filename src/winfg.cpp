// win-fg frame-generation statistics reader.
//
// Mirrors the FEX stats reader in fex.cpp: locate the "win-fg-<pid>-stats"
// POSIX shared-memory segment across the candidate shm directories, mmap it,
// and derive per-second real/generated frame rates from the cumulative
// counters. The reader treats the counters as weakly-consistent snapshots
// (win-fg writes them under its own mutex; a torn read resolves next frame).

#include "winfg.h"
#include "overlay.h"
#include "hud_elements.h"

#include <cstdlib>
#include <cstring>
#include <chrono>
#include <atomic>
#include <vector>
#include <string>

#include "spdlog/spdlog.h"

#ifdef _WIN32
// Windows build: win-fg is a Linux/Android/Termux layer and does not run here,
// so the stats reader is a no-op (always unavailable). The HUD element simply
// never renders.
namespace winfg {
bool is_available() { return false; }
bool is_enabled() { return false; }
double real_fps()  { return 0.0; }
double gen_fps()   { return 0.0; }
double total_fps() { return 0.0; }
void update() {}
}
#else

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <cerrno>

namespace winfg {

namespace {

struct Global {
    int             fd = -1;
    Stats*          base = nullptr;
    size_t          size = 0;
    int             pid = -1;
    bool            available = false;

    // FPS estimation state: accumulate counters over a fixed sampling
    // window (default 500ms, mirroring MangoHud's fps_sampling_period) so the
    // displayed rate is stable instead of jittering every frame.
    uint64_t        last_real = 0;
    uint64_t        last_gen  = 0;
    uint64_t        last_ns   = 0;
    uint64_t        win_start_real = 0;
    uint64_t        win_start_gen  = 0;
    uint64_t        win_start_ns   = 0;
    double          real_rate = 0.0;
    double          gen_rate  = 0.0;
    bool            enabled_flag = false;
    uint32_t        multiplier = 0;
};

Global g;

std::string shm_name_for_pid(int pid) {
    return "win-fg-" + std::to_string(pid) + "-stats";
}

bool is_shm_pid_alive(const std::string& name) {
    // "win-fg-<pid>-stats"
    constexpr const char* prefix = "win-fg-";
    constexpr size_t prefix_len = 7;
    constexpr const char* suffix = "-stats";
    constexpr size_t suffix_len = 6;
    if (name.size() <= prefix_len + suffix_len) return false;
    if (name.rfind(prefix, 0) != 0) return false;
    if (name.compare(name.size() - suffix_len, suffix_len, suffix) != 0) return false;
    const int p = atoi(name.c_str() + prefix_len);
    if (p <= 0) return false;
    return kill(p, 0) == 0 || errno == EPERM;
}

// Candidate shm directories, in priority order (Termux/glibc aware).
std::vector<std::string> shm_dirs() {
    std::vector<std::string> dirs;
    auto add = [&dirs](const std::string& d) {
        if (d.empty() || d[0] != '/') return;
        for (const auto& x : dirs)
            if (x == d) return;
        dirs.push_back(d);
    };
    if (const char* d = ::getenv("MANGOHUD_WIN_FG_SHM_DIR"))
        add(d);
    if (const char* d = ::getenv("MANGOHUD_WIN_FG_DIR"))
        add(d);
    if (!dirs.empty())
        return dirs;
    add("/data/data/com.termux/files/usr/tmp");
    if (const char* t = ::getenv("TMPDIR"))
        add(t);
    if (const char* p = ::getenv("PREFIX"))
        add(std::string(p) + "/tmp");
    add("/dev/shm");
    add("/data/data/com.termux/files/usr/glibc/tmp");
    add("/data/data/com.termux/files/usr/tmp/shm");
    add("/data/data/com.termux/files/usr/var/run/shm");
    add("/data/data/com.termux/files/usr/var/run");
    add("/data/data/com.termux/files/usr/glibc/tmp/shm");
    add("/data/data/com.termux/files/usr/glibc/var/run/shm");
    add("/data/local/tmp");
    add("/tmp");
    add("/tmp/shm");
    add("/run/shm");
    return dirs;
}

void scan_one_dir(const std::string& dir, const std::string& self,
                  std::vector<std::string>& out, int depth) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> subdirs;
    while (auto* entry = readdir(d)) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (name.rfind("win-fg-", 0) == 0) {
            if (name == self) continue;
            if (!is_shm_pid_alive(name)) continue;
            out.push_back(dir + "/" + name);
            continue;
        }
        if (depth > 0 &&
            (name.rfind("proot-", 0) == 0 || name.rfind("shm", 0) == 0))
            subdirs.push_back(dir + "/" + name);
    }
    closedir(d);
    for (const std::string& sub : subdirs)
        scan_one_dir(sub, self, out, depth - 1);
}

std::vector<std::string> scan_stats_shms(int self_pid) {
    std::vector<std::string> result;
    const std::string self = shm_name_for_pid(self_pid);
    for (const std::string& dir : shm_dirs())
        scan_one_dir(dir, self, result, 1);
    return result;
}

void close_shm() {
    if (g.base) { munmap(g.base, g.size); g.base = nullptr; }
    if (g.fd >= 0) { close(g.fd); g.fd = -1; }
    g.size = 0;
    g.pid = -1;
    g.available = false;
}

bool init_shm(int pid) {
    close_shm();

    const std::vector<std::string> dirs = shm_dirs();
    std::vector<std::string> candidates;
    auto add_path = [&candidates](const std::string& p) {
        if (p.empty()) return;
        for (const auto& x : candidates)
            if (x == p) return;
        candidates.push_back(p);
    };

    if (const char* env = ::getenv("MANGOHUD_WIN_FG_SHM")) {
        if (*env && ::strchr(env, '/'))
            add_path(env);   // full path → sole candidate
    }
    if (const char* env = ::getenv("MANGOHUD_WIN_FG_PID")) {
        const int p = atoi(env);
        if (p > 0) {
            for (const std::string& d : dirs)
                add_path(d + "/" + shm_name_for_pid(p));
        }
    }
    // Default: self pid, then scan other live win-fg-*-stats segments.
    {
        const char* env = ::getenv("MANGOHUD_WIN_FG_SHM");
        if (!(env && *env && ::strchr(env, '/'))) {
            for (const std::string& d : dirs)
                add_path(d + "/" + shm_name_for_pid(pid));
            for (auto& p : scan_stats_shms(pid))
                add_path(p);
        }
    }

    int fd = -1;
    std::string used;
    for (const auto& path : candidates) {
        const int cf = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (cf == -1) continue;
        struct stat st{};
        const bool ok = fstat(cf, &st) == 0 &&
                        st.st_size >= static_cast<off_t>(sizeof(Stats));
        if (!ok) { close(cf); continue; }
        fd = cf;
        used = path;
        break;
    }
    if (fd == -1) {
        SPDLOG_DEBUG("win-fg stats: N/A (tried {} paths)", candidates.size());
        return false;
    }

    struct stat st{};
    if (fstat(fd, &st) == -1 || st.st_size < static_cast<off_t>(sizeof(Stats))) {
        close(fd);
        return false;
    }

    void* base = mmap(nullptr, sizeof(Stats), PROT_READ, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        SPDLOG_DEBUG("win-fg stats: mmap failed");
        return false;
    }

    g.fd = fd;
    g.base = static_cast<Stats*>(base);
    g.size = sizeof(Stats);
    g.pid = pid;

    // Validate magic/version.
    const Stats* s = g.base;
    if (s->magic != kMagic || s->version != kVersion) {
        SPDLOG_DEBUG("win-fg stats: bad magic/version in {}", used);
        close_shm();
        return false;
    }

    g.available = true;
    g.last_real = 0;
    g.last_gen  = 0;
    g.last_ns   = 0;
    g.real_rate = 0.0;
    g.gen_rate  = 0.0;
    SPDLOG_INFO("win-fg stats: using {}", used);
    return true;
}

} // namespace

bool is_available() {
    return g.available;
}

bool is_enabled() {
    return g.available && g.enabled_flag;
}

double real_fps()  { return g.real_rate; }
double gen_fps()   { return g.gen_rate; }
double total_fps() { return g.real_rate + g.gen_rate; }

void update() {
    // Locate on first call (or after pid change).
    const int gs_pid = HUDElements.g_gamescopePid > 0 ? HUDElements.g_gamescopePid : ::getpid();
    if (gs_pid < 1) return;

    if (g.pid != gs_pid) {
        // Throttle rescan: when we cannot find a segment, retry at most every 2s
        // so a later-started win-fg still gets picked up without burning cycles.
        static std::chrono::steady_clock::time_point last_attempt{};
        const auto now = std::chrono::steady_clock::now();
        if (g.pid == -1 && last_attempt.time_since_epoch().count() != 0 &&
            now - last_attempt < std::chrono::seconds(2)) {
            return;
        }
        last_attempt = now;
        if (!init_shm(gs_pid)) {
            g.pid = -1;
            return;
        }
    }

    if (!g.available || !g.base) return;

    const Stats* s = g.base;
    const uint64_t real = __atomic_load_n(&s->real_frames, __ATOMIC_RELAXED);
    const uint64_t gen  = __atomic_load_n(&s->gen_frames,  __ATOMIC_RELAXED);
    const uint64_t ns   = __atomic_load_n(&s->last_ns,      __ATOMIC_RELAXED);
    g.enabled_flag = (__atomic_load_n(&s->enabled, __ATOMIC_RELAXED) != 0);
    g.multiplier   = __atomic_load_n(&s->multiplier, __ATOMIC_RELAXED);

    // First sample: just establish the baseline.
    if (g.last_ns == 0) {
        g.last_real = real;
        g.last_gen  = gen;
        g.last_ns   = ns;
        g.win_start_real = real;
        g.win_start_gen  = gen;
        g.win_start_ns   = ns;
        return;
    }

    // Accumulate counters over a fixed sampling window. Only publish a new
    // rate once the window has elapsed, so the displayed value is stable
    // (default 500ms, matching MangoHud's fps_sampling_period).
    g.last_real = real;
    g.last_gen  = gen;
    g.last_ns   = ns;

    const uint64_t win_ns = ns - g.win_start_ns;
    static const uint64_t kSampleNs = 500000000ull;  // 500ms
    if (win_ns >= kSampleNs) {
        const double dt_s = (double)win_ns / 1e9;
        const uint64_t dreal = (real >= g.win_start_real) ? (real - g.win_start_real) : 0;
        const uint64_t dgen  = (gen  >= g.win_start_gen)  ? (gen  - g.win_start_gen)  : 0;
        if (dt_s > 0.0) {
            g.real_rate = (double)dreal / dt_s;
            g.gen_rate  = (double)dgen  / dt_s;
        }
        // Reset the window for the next sample.
        g.win_start_real = real;
        g.win_start_gen  = gen;
        g.win_start_ns   = ns;
    }
}

} // namespace winfg

#endif // _WIN32
