// 验证：win-fg 插帧工作时隐藏 FPS 那一组，只显示 "real→total"。
#include "../src/fusion_render.hpp"
#include <cstdio>
#include <cmath>
#include <string>

static int g_fail = 0;
static void ck(bool cond, const std::string& what) {
    printf("%s  %s\n", cond ? "[ OK ]" : "[FAIL]", what.c_str());
    if (!cond) ++g_fail;
}
static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

static fusionhud::fr::Snapshot fg_snap() {
    fusionhud::fr::Snapshot s;
    s.fps = 541.0f;
    s.fps_avg = 540.0f;
    s.low1 = 500.0f;
    s.ft_ms = 1000.0f / 541.0f;
    s.ft_min = 1.7f;
    s.ft_max = 2.5f;
    s.winfg_real = 541.0f;
    s.winfg_total = 1083.0f;
    s.winfg_on = true;
    return s;
}

int main() {
    using namespace fusionhud::fr;

    // --- fg_active 判定 ---
    {
        auto s = fg_snap();
        ck(fg_active(s), "插帧工作时 fg_active 为真");
        s.winfg_on = false;
        ck(!fg_active(s), "win_fg 关闭时 fg_active 为假");
        s = fg_snap(); s.winfg_total = 0.0f;
        ck(!fg_active(s), "无插帧产出时 fg_active 为假（避免闪 0）");
    }

    // --- 文本格式 ---
    {
        auto s = fg_snap();
        ck(fg_pair_text(s) == "541->1083",
           "fg_pair_text 生成 541->1083（实际：" + fg_pair_text(s) + "）");
    }

    // --- 关键：FPS 行的显示条件在各档位都不再出现原帧 ---
    {
        auto s = fg_snap();
        const bool c_fps = true;
        ck(!(c_fps && !fg_active(s)), "插帧时 FPS 行条件为假（FULL/BAR/MEGA/TILES 隐藏）");
        s.winfg_on = false;
        ck(c_fps && !fg_active(s), "未插帧时 FPS 行条件为真（正常显示）");
    }

    // --- 帧时间图条件 ---
    {
        auto s = fg_snap();
        const bool c_fps = true, c_graph = true;
        ck(!(c_fps && c_graph && !fg_active(s)), "插帧时帧时间图条件为假（隐藏原帧时间）");
    }

    printf("\n==== %s (%d 个失败) ====\n", g_fail == 0 ? "ALL PASS" : "HAS FAILURE", g_fail);
    return g_fail == 0 ? 0 : 1;
}