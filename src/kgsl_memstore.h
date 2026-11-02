// kgsl memstore 读取：在 sysfs 被冻结/遮蔽时，直接从 kgsl 驱动的 memstore 取 GPU 忙闲。
//
// 为什么需要这条路径：部分 Adreno 设备上 /sys/class/kgsl/kgsl-3d0 下的
// gpu_busy_percentage、gpubusy、devfreq/gpu_load 全为常量 0（内核不再更新这些
// 计数器，或 SELinux 把节点遮蔽），此时任何基于 sysfs 的占用率都只能是假 0%。
//
// kgsl 允许任意客户端只读映射自己的 memstore（KGSL_PROP_DEVICE_SHADOW）。命令
// 处理器在每批提交的命令缓冲区之前写入 start-of-pipe 时间戳，之后写入
// end-of-pipe 时间戳，所以 sop != eop 就表示该 ringbuffer / context 在 GPU 上有
// 活。每秒采样约 1000 次，统计 busy 占比即得占用率。
//
// 两种模式：
//   1) ringbuffer：内核把时间戳写进 ringbuffer 条目（较老的、软件调度的内核）
//   2) contexts  ：硬件调度（hwsched）内核把 ringbuffer 条目留成 0，改由 GMU 固件
//                  写每个 context 自己的条目。Adreno 8xx 全部是这种，部分 7xx
//                  也是。打开后先试 ringbuffer，全 0 就自动退到 contexts。
//
// 这套布局与解析逻辑参照 DroidDeck 的公开实现（MIT），并入本项目的读取链。
#pragma once

#include <cstddef>
#include <cstdint>

namespace fhud_kgsl {

// 每个 memstore 条目的字数。sop 在第 0 字，eop 在第 2 字。
constexpr int kEntryWords = 10;
constexpr int kSop = 0;
constexpr int kEop = 2;
// ringbuffer 条目位于 memstore 尾部，取 5 个槽位覆盖有/无 LPAC 两种内核布局。
constexpr int kSlots = 5;
// contexts 模式最多同时盯这么多个 context，够覆盖正常游戏 + 系统合成。
constexpr int kMaxLive = 64;

// memstore 字节数 -> 第一个被监视的 ringbuffer 槽位下标；太小返回 -1。
inline int first_slot(std::size_t bytes)
{
    std::size_t entries = bytes / (kEntryWords * sizeof(uint32_t));
    return entries < 8 ? -1 : static_cast<int>(entries - 6);
}

// 读 ringbuffer 槽位。返回 1 表示有活在跑；*moved 在 sop 发生变化时置 1，
// 并把新值存进 last[]。
inline int sample_ringbuffer(const volatile uint32_t *memstore, int first,
                             uint32_t last[kSlots], int *moved)
{
    int busy = 0;
    for (int i = 0; i < kSlots; i++) {
        const volatile uint32_t *entry =
            memstore + static_cast<std::size_t>(first + i) * kEntryWords;
        uint32_t sop = entry[kSop], eop = entry[kEop];
        if (sop != eop)
            busy = 1;
        if (sop != last[i]) {
            *moved = 1;
            last[i] = sop;
        }
    }
    return busy;
}

// 收集要采样的 context：first 之下所有 sop 非 0 的条目（kgsl 在 context 创建/
// 销毁时把条目清零）。相对上次扫描 sop 变了的、以及当前就 busy 的排在前面，
// 这样 kMaxLive 的截断只会丢掉空闲的。返回写进 live[] 的个数。
inline int scan_contexts(const volatile uint32_t *memstore, int first,
                         uint32_t *seen, int *live, int max_live)
{
    int n = 0, idle = 0;
    int idle_ids[kMaxLive];
    for (int id = 1; id < first; id++) {
        const volatile uint32_t *entry =
            memstore + static_cast<std::size_t>(id) * kEntryWords;
        uint32_t sop = entry[kSop], eop = entry[kEop];
        int changed = sop != seen[id];
        seen[id] = sop;
        if ((changed || sop != eop) && n < max_live)
            live[n++] = id;
        else if (sop != 0 && idle < kMaxLive)
            idle_ids[idle++] = id;
    }
    for (int i = 0; i < idle && n < max_live; i++)
        live[n++] = idle_ids[i];
    return n;
}

// 读 context 条目。语义同 sample_ringbuffer，last[] 按 context id 索引。
inline int sample_contexts(const volatile uint32_t *memstore, const int *live,
                           int count, uint32_t *last, int *moved)
{
    int busy = 0;
    for (int i = 0; i < count; i++) {
        const volatile uint32_t *entry =
            memstore + static_cast<std::size_t>(live[i]) * kEntryWords;
        uint32_t sop = entry[kSop], eop = entry[kEop];
        if (sop != eop)
            busy = 1;
        if (sop != last[live[i]]) {
            *moved = 1;
            last[live[i]] = sop;
        }
    }
    return busy;
}

// ── 公开接口（kgsl_memstore.cpp 实现）──────────────────────────────────
// 首次调用会打开 /dev/kgsl-3d0 并启动 1ms 采样线程；失败后不再重试。
bool ensure_open();
// 当前占用率 0..100；未打开或样本不足返回 -1。
int read_percent();
// 当前使用的来源名，供诊断输出。
const char *source_name();
// 停止采样线程并释放资源。
void shutdown();

} // namespace fhud_kgsl
