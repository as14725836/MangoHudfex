// kgsl memstore 占用率读取实现。见 kgsl_memstore.h 的说明。
//
// 只在 Linux/Android 上编译；其它平台整份文件编成空实现，调用方无需分支。
#include "kgsl_memstore.h"

#include <spdlog/spdlog.h>

#if defined(__linux__)

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fhud_kgsl {

namespace {

// ── kgsl uapi ────────────────────────────────────────────────────────────
// 抄一份最小定义，避免依赖设备上不一定存在的内核头文件。
constexpr unsigned kIocType = 0x09;
constexpr unsigned kPropDeviceShadow = 0x2;

struct KgslShadowprop {
    unsigned long gpuaddr;
    std::size_t size;
    unsigned int flags;
};

struct KgslDeviceGetproperty {
    unsigned int type;
    void *value;
    std::size_t sizebytes;
};

// _IOWR(KGSL_IOC_TYPE, 0x2, struct kgsl_device_getproperty)
constexpr unsigned long kgsl_getproperty_ioctl()
{
    constexpr std::size_t sz = sizeof(KgslDeviceGetproperty);
    return (3UL << 30) | (static_cast<unsigned long>(sz) << 16) |
           (static_cast<unsigned long>(kIocType) << 8) | 0x2UL;
}

// 采样间隔与 context 重扫周期。
constexpr auto kSampleGap = std::chrono::milliseconds(1);
constexpr int kScanEvery = 200;

enum Mode { kModeNone = 0, kModeRingbuffer = 1, kModeContexts = 2 };

} // namespace

class Memstore {
public:
    Memstore() = default;
    ~Memstore() { close(); }

    Memstore(const Memstore &) = delete;
    Memstore &operator=(const Memstore &) = delete;

    // 打开并映射 memstore；成功后启动采样线程。失败返回 false，reason 给出原因。
    bool open()
    {
        if (fd_ >= 0)
            return true;

        fd_ = ::open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            reason_ = std::string("open /dev/kgsl-3d0: ") + std::strerror(errno);
            return false;
        }

        KgslShadowprop shadow{};
        KgslDeviceGetproperty prop{};
        prop.type = kPropDeviceShadow;
        prop.value = &shadow;
        prop.sizebytes = sizeof(shadow);
        if (::ioctl(fd_, kgsl_getproperty_ioctl(), &prop) != 0) {
            reason_ = std::string("KGSL_PROP_DEVICE_SHADOW: ") + std::strerror(errno);
            close();
            return false;
        }
        if (shadow.size == 0) {
            reason_ = "memstore size is 0";
            close();
            return false;
        }

        bytes_ = shadow.size;
        first_ = first_slot(bytes_);
        if (first_ < 0) {
            reason_ = "memstore too small";
            close();
            return false;
        }

        map_ = ::mmap(nullptr, bytes_, PROT_READ, MAP_SHARED, fd_,
                      static_cast<off_t>(shadow.gpuaddr));
        if (map_ == MAP_FAILED) {
            map_ = nullptr;
            reason_ = std::string("mmap memstore: ") + std::strerror(errno);
            close();
            return false;
        }
        mem_ = static_cast<const volatile uint32_t *>(map_);

        // 先按 ringbuffer 试一次：全 0 说明是 hwsched 内核，改用 context 条目。
        int moved = 0;
        sample_ringbuffer(mem_, first_, last_rb_, &moved);
        bool any = false;
        for (int i = 0; i < kSlots; i++)
            any = any || last_rb_[i] != 0;

        if (any) {
            mode_ = kModeRingbuffer;
            reason_ = "ringbuffer timestamps";
        } else {
            seen_.assign(static_cast<std::size_t>(first_), 0);
            ctx_last_.assign(static_cast<std::size_t>(first_), 0);
            live_.assign(kMaxLive, 0);
            live_count_ = scan_contexts(mem_, first_, seen_.data(), live_.data(), kMaxLive);
            sample_contexts(mem_, live_.data(), live_count_, ctx_last_.data(), &moved);
            mode_ = kModeContexts;
            reason_ = "per-context timestamps (hardware-scheduled kernel)";
        }

        run_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    // 停止采样线程并释放资源。可重复调用。
    void close()
    {
        run_ = false;
        if (thread_.joinable())
            thread_.join();
        if (map_)
            ::munmap(map_, bytes_);
        if (fd_ >= 0)
            ::close(fd_);
        map_ = nullptr;
        mem_ = nullptr;
        fd_ = -1;
        bytes_ = 0;
        first_ = 0;
        mode_ = kModeNone;
    }

    // 当前占用率百分比；样本不足时返回 -1，让调用方回落到其它来源。
    int percent() const
    {
        uint64_t samples = samples_.load(std::memory_order_relaxed);
        if (samples < 8)
            return -1;
        uint64_t busy = busy_.load(std::memory_order_relaxed);
        int pct = static_cast<int>((busy * 100 + samples / 2) / samples);
        return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
    }

    // 已观察到的最大时间戳推进次数。0 表示时间戳从未动过（读到了但没数据）。
    uint64_t moves() const { return moves_.load(std::memory_order_relaxed); }

    int mode() const { return mode_; }
    const std::string &reason() const { return reason_; }

private:
    const volatile uint32_t *mem_ = nullptr;

    void loop()
    {
        int until_scan = 0;
        while (run_.load(std::memory_order_relaxed)) {
            int moved = 0, busy = 0;
            if (mode_ == kModeContexts) {
                if (until_scan-- <= 0) {
                    live_count_ = scan_contexts(mem_, first_, seen_.data(),
                                                live_.data(), kMaxLive);
                    until_scan = kScanEvery;
                }
                busy = sample_contexts(mem_, live_.data(), live_count_,
                                       ctx_last_.data(), &moved);
            } else if (mode_ == kModeRingbuffer) {
                busy = sample_ringbuffer(mem_, first_, last_rb_, &moved);
            }
            if (busy)
                busy_.fetch_add(1, std::memory_order_relaxed);
            if (moved)
                moves_.fetch_add(1, std::memory_order_relaxed);
            samples_.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(kSampleGap);
        }
    }

    int fd_ = -1;
    void *map_ = nullptr;
    std::size_t bytes_ = 0;
    int first_ = 0;
    int mode_ = kModeNone;
    int live_count_ = 0;

    uint32_t last_rb_[kSlots] = {};
    std::vector<uint32_t> seen_;
    std::vector<uint32_t> ctx_last_;
    std::vector<int> live_;

    std::thread thread_;
    std::atomic<bool> run_{false};
    std::atomic<uint64_t> busy_{0};
    std::atomic<uint64_t> samples_{0};
    std::atomic<uint64_t> moves_{0};
    std::string reason_;
};

// ── 进程级单例 ──────────────────────────────────────────────────────────
// 采样线程只应存在一份：HUD 的多个读点共享同一个 reader。
namespace {

Memstore &instance()
{
    static Memstore m;
    return m;
}

bool g_tried = false;
bool g_ok = false;

} // namespace

bool ensure_open()
{
    if (!g_tried) {
        g_tried = true;
        g_ok = instance().open();
        if (g_ok)
            SPDLOG_INFO("kgsl memstore: {}", instance().reason());
        else
            SPDLOG_DEBUG("kgsl memstore unavailable: {}", instance().reason());
    }
    return g_ok;
}

int read_percent()
{
    if (!ensure_open())
        return -1;
    return instance().percent();
}

const char *source_name()
{
    if (!g_ok)
        return "none";
    return instance().mode() == kModeRingbuffer ? "memstore ringbuffer"
                                                 : "memstore contexts";
}

void shutdown()
{
    instance().close();
}

} // namespace fhud_kgsl

#else // 非 Linux：编成空实现

namespace fhud_kgsl {
bool ensure_open() { return false; }
int read_percent() { return -1; }
const char *source_name() { return "none"; }
void shutdown() {}
} // namespace fhud_kgsl

#endif