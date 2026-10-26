// 验证 GPU 采样三处改动：
//   1) GpuMonitor 持久流读值正确（含 "0 %" / "<busy> <total>" / 0..1 小数 /
//      milli-percent 四种格式）
//   2) 死节点检测：值恒定时经过 kStaleSamples 次后返回 -1（不再显示假 0%），
//      值一旦变化立即解除 stale
//   3) 瞬态读失败保留上一个有效值，不跳 N/A
//   4) kMaxThermalZone 覆盖到高编号 zone（kalama 的 gpuss 在 zone63+）
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

#include "fusion_metrics.hpp"

static int g_fail = 0;
static void chk(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_fail;
    } else {
        std::printf("ok  : %s\n", what);
    }
}

// 用临时文件模拟 sysfs 节点
static std::string makeTmp(const char* content) {
    char tpl[] = "/tmp/fhgpu_XXXXXX";
    int fd = mkstemp(tpl);
    if (fd < 0) return std::string();
    if (content) {
        ssize_t n = write(fd, content, std::char_traits<char>::length(content));
        (void)n;
    }
    close(fd);
    return std::string(tpl);
}
static void writeTmp(const std::string& p, const char* content) {
    std::ofstream f(p, std::ios::trunc);
    f << content;
}

int main() {
    using namespace fusionhud;

    // ---- 1) 各种格式解析 ----
    {
        struct Case { const char* content; int expect; const char* what; };
        const Case cases[] = {
            { "0 %\n",           0,   "\"0 %\" -> 0" },
            { "37 %\n",          37,  "\"37 %\" -> 37" },
            { "100 %\n",         100, "\"100 %\" -> 100" },
            { "0.55\n",          55,  "0..1 小数 0.55 -> 55" },
            { "1234\n",          12,  "milli-percent 1234 -> 12" },
        };
        for (const Case& c : cases) {
            std::string p = makeTmp(c.content);
            GpuPaths gp;
            gp.use = p;
            GpuMonitor m(gp);
            int v = m.load();
            chk(v == c.expect, c.what);
            unlink(p.c_str());
        }
    }

    // ---- 2) gpubusy 双字段格式 ----
    {
        std::string p = makeTmp("      0 25396927\n");
        GpuPaths gp; gp.use = p;
        GpuMonitor m(gp);
        chk(m.load() == 0, "gpubusy \"0 25396927\" -> 0");

        std::string p2 = makeTmp("      50 100\n");
        GpuPaths gp2; gp2.use = p2;
        GpuMonitor m2(gp2);
        chk(m2.load() == 50, "gpubusy \"50 100\" -> 50");
        unlink(p.c_str()); unlink(p2.c_str());
    }

    // ---- 3) 死节点检测：恒定值必须最终报 -1 ----
    {
        std::string p = makeTmp("0 %\n");
        GpuPaths gp; gp.use = p;
        GpuMonitor m(gp);

        // 第 1 次：值 0，尚未达到 stale 阈值，应如实返回 0
        chk(m.load() == 0, "首次采样返回 0（尚未判定停更）");
        chk(!m.stale(), "首次采样未标记 stale");

        // 继续读到超过阈值
        int last = 0;
        for (int i = 0; i < 15; ++i)
            last = m.load();

        chk(last == -1, "值恒定 15 次后返回 -1（不再显示假 0%）");
        chk(m.stale(), "已被标记为 stale 节点");
        unlink(p.c_str());
    }

    // ---- 4) 值一旦变化，立即解除 stale ----
    {
        std::string p = makeTmp("0 %\n");
        GpuPaths gp; gp.use = p;
        GpuMonitor m(gp);
        for (int i = 0; i < 15; ++i) m.load();
        chk(m.stale(), "先进入 stale 状态");

        // 值变化 → 节点是活的
        writeTmp(p, "42 %\n");
        int v = m.load();
        chk(v == 42, "值变化后返回真实值 42");
        chk(!m.stale(), "值变化后解除 stale");
        unlink(p.c_str());
    }

    // ---- 5) 真实负载模拟：值持续波动，绝不能误判 stale ----
    {
        std::string p = makeTmp("10 %\n");
        GpuPaths gp; gp.use = p;
        GpuMonitor m(gp);
        bool ever_stale = false;
        const int seq[] = {10, 12, 12, 12, 35, 35, 80, 80, 80, 80, 80, 79, 79, 79, 79, 79, 79, 5};
        for (int v : seq) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%d %%\n", v);
            writeTmp(p, buf);
            m.load();
            if (m.stale()) ever_stale = true;
        }
        chk(!ever_stale, "波动的真实负载不会被误判为 stale");

        // 但若之后长期恒定在同一值，仍应被判定
        writeTmp(p, "5 %\n");
        for (int i = 0; i < 15; ++i) m.load();
        chk(m.stale(), "随后长期恒定 → 仍能判定 stale");
        unlink(p.c_str());
    }

    // ---- 6) 瞬态读失败保留上一个有效值 ----
    {
        std::string p = makeTmp("73 %\n");
        GpuPaths gp; gp.use = p;
        GpuMonitor m(gp);
        chk(m.load() == 73, "先读到有效值 73");

        // 清空文件 → getline 失败（模拟 sysfs 瞬态返回空）
        writeTmp(p, "");
        int v = m.load();
        chk(v == 73, "瞬态读空时保留上一个有效值 73（不跳 N/A）");
        chk(!m.stale(), "瞬态失败不应触发 stale");
        unlink(p.c_str());
    }

    // ---- 7) 文件不存在 → -1 ----
    {
        GpuPaths gp;
        gp.use = "/tmp/definitely_not_exist_fh_gpu";
        GpuMonitor m(gp);
        chk(m.load() == -1, "节点不存在返回 -1");
        chk(m.freq() == -1.0, "频率节点不存在返回 -1.0");
    }

    // ---- 8) 频率解析 ----
    {
        std::string p = makeTmp("295000000\n");
        GpuPaths gp; gp.freq = p;
        GpuMonitor m(gp);
        chk(m.freq() == 295.0, "295000000 Hz -> 295 MHz");

        writeTmp(p, "295000\n");
        chk(m.freq() == 295.0, "295000 KHz -> 295 MHz");
        unlink(p.c_str());
    }

    // ---- 9) kMaxThermalZone 必须覆盖 kalama 的 gpuss 编号 ----
    {
        chk(kMaxThermalZone > 70, "kMaxThermalZone 覆盖 zone70（kalama 的 gpuss-7）");
        chk(kMaxThermalZone >= 96, "kMaxThermalZone 有足够余量");
    }

    // ---- 10) 本机实际节点：确认 stale 判定能命中真实冻结节点 ----
    {
        GpuPaths gp;
        gp.use = "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage";
        std::ifstream probe(gp.use);
        if (probe.is_open()) {
            probe.close();
            GpuMonitor m(gp);
            int first = m.load();
            std::printf("      真实节点首读=%d\n", first);
            int last = first;
            for (int i = 0; i < 15; ++i) {
                usleep(1000);
                last = m.load();
            }
            std::printf("      恒定 15 次后=%d  stale=%d\n", last, (int)m.stale());
            chk(last == -1 && m.stale(),
                "本机冻结的 gpu_busy_percentage 被正确判定为 stale（返回 -1）");
        } else {
            std::printf("      (跳过：本机无 kgsl 节点)\n");
        }
    }

    std::printf("\n%s (%d failure)\n", g_fail == 0 ? "ALL PASS" : "SOME FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}