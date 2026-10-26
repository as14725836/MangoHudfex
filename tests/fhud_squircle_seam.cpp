// 验证「BAR 档左边半圆上方黑线」的两处修复：
//   1) squircle_path 生成的顶点序列不得出现相邻重合点（零长度边）
//      —— 零长度边的法线算出 (0,0)，该顶点 fringe 缺失，边缘出现硬切暗点；
//   2) panel_margin 必须覆盖外投影的外扩量，否则纯黑半透明阴影被窗口
//      裁剪区截断成一条硬边黑线。
#include <cmath>
#include <cstdio>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "fusion_render.hpp"

static int g_fail = 0;
static void chk(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_fail;
    } else {
        std::printf("ok  : %s\n", what);
    }
}

// 统计相邻重合顶点数（含首尾闭合边）
static int coincident_pairs(const std::vector<ImVec2>& p) {
    int n = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        const ImVec2& a = p[i];
        const ImVec2& b = p[(i + 1) % p.size()];
        if (std::fabs(a.x - b.x) < 0.01f && std::fabs(a.y - b.y) < 0.01f)
            ++n;
    }
    return n;
}

int main() {
    std::vector<ImVec2> p;

    using namespace fusionhud::fr;
    using fusionhud::kPillRadiusRatio;
    using fusionhud::kOutlineMaxSp;

    // --- 1) BAR 档几何：w=600, h=30, r=h/2=15, n=2（真半圆）---
    {
        const float x0 = 10.0f, y0 = 10.0f, x1 = 610.0f, y1 = 40.0f;
        const float r = (y1 - y0) * fusionhud::kPillRadiusRatio;
        squircle_path(p, x0, y0, x1, y1, r, 2.0f, kSquircleSeg);
        chk(coincident_pairs(p) == 0, "BAR 面板路径无相邻重合顶点");
        chk(p.size() >= 40, "BAR 面板路径顶点数合理");

        // 顶点都不得越出包围盒（越界说明弧采样算错，会被裁剪）
        bool inside = true;
        for (const ImVec2& v : p) {
            if (v.x < x0 - 0.01f || v.x > x1 + 0.01f || v.y < y0 - 0.01f || v.y > y1 + 0.01f)
                inside = false;
        }
        chk(inside, "BAR 面板路径全部落在包围盒内");

        // 左端半圆必须有顶点真正抵达最左的 x0（否则端头是平的）
        float minx = 1e9f;
        for (const ImVec2& v : p)
            minx = std::min(minx, v.x);
        chk(std::fabs(minx - x0) < 0.01f, "左端半圆抵达最左边界 x0");

        // 半圆顶部必须触及 y0（否则弧顶比直边低，上面留缝）
        float miny = 1e9f;
        for (const ImVec2& v : p)
            miny = std::min(miny, v.y);
        chk(std::fabs(miny - y0) < 0.01f, "半圆弧顶到达顶边界 y0");
    }

    // --- 2) 外投影路径（同样跑一遍，阴影若是零长边也会有黑点）---
    {
        const float x0 = 10.0f, y0 = 10.0f, x1 = 610.0f, y1 = 40.0f;
        const float r = (y1 - y0) * fusionhud::kPillRadiusRatio;
        for (int i = 2; i >= 1; --i) {
            const float e = 1.4f * 2.1f * 1.0f * static_cast<float>(i);   // sp(1.4)*i
            squircle_path(p, x0 - e, y0 - e * 0.5f, x1 + e, y1 + e, r + e, 2.0f, kSquircleSeg);
            chk(coincident_pairs(p) == 0, i == 1 ? "近层阴影路径无重合顶点"
                                                 : "远层阴影路径无重合顶点");
        }
    }

    // --- 3) 普通面板（squircle n=4）也不能有重合顶点 ---
    {
        squircle_path(p, 0.0f, 0.0f, 400.0f, 260.0f, 20.0f, kSquircleN, kSquircleSeg);
        chk(coincident_pairs(p) == 0, "普通面板路径无重合顶点");
    }

    // --- 4) 退化情形：r 超过短边一半时被 clamp 到 rmax=w/2，仍不能重合 ---
    {
        squircle_path(p, 0.0f, 0.0f, 100.0f, 20.0f, 999.0f, 2.0f, kSquircleSeg);
        chk(coincident_pairs(p) == 0, "r 被 clamp 后仍无重合顶点");
        // r=w/2 时上下边长度为 0，此时允许上下边端点合并
        float minx = 1e9f, maxx = -1e9f;
        for (const ImVec2& v : p) {
            minx = std::min(minx, v.x);
            maxx = std::max(maxx, v.x);
        }
        chk(std::fabs(minx) < 0.01f && std::fabs(maxx - 100.0f) < 0.01f,
            "r=w/2 时仍完整覆盖宽度");
    }

    // --- 5) panel_margin 必须覆盖阴影外扩（核心：黑线的直接成因）---
    {
        Metrics M;
        M.spk = 2.1f;
        M.scale = 0.64f;                    // 用户当前 MANGOHUD_FUSION_SCALE
        Frame f;
        f.M = M;
        Options opt;                        // outline = kOutlineDefault = 0
        const float mg = panel_margin(f, opt);
        const float e = M.sp(1.4f) * 2.0f;  // 远层阴影外扩
        std::printf("      panel_margin=%.3f  阴影外扩 e=%.3f\n", mg, e);
        chk(mg >= e + 1.0f, "panel_margin >= 阴影外扩 + 1px 抗锯齿 fringe");
        chk(mg < 20.0f, "panel_margin 未过度膨胀（不至于吃掉大量画面）");
    }

    // --- 6) 放大后（scale=1.0）依然成立 ---
    {
        Metrics M;
        M.spk = 2.1f;
        M.scale = 1.0f;
        Frame f;
        f.M = M;
        Options opt;
        const float mg = panel_margin(f, opt);
        const float e = M.sp(1.4f) * 2.0f;
        std::printf("      scale=1.0: panel_margin=%.3f  e=%.3f\n", mg, e);
        chk(mg >= e + 1.0f, "scale=1.0 时 panel_margin 仍覆盖阴影");
    }

    // --- 7) 有描边时留白不小于描边宽度 ---
    {
        Metrics M;
        M.spk = 2.1f;
        M.scale = 0.64f;
        Frame f;
        f.M = M;
        Options opt;
        opt.outline = 1.0f;
        const float mg = panel_margin(f, opt);
        const float sw = std::max(1.0f, std::round(M.sp(fusionhud::kOutlineMaxSp)));
        chk(mg >= sw + 4.0f, "有描边时留白 >= 线宽+4px");
    }

    std::printf("\n%s (%d failure)\n", g_fail == 0 ? "ALL PASS" : "SOME FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
