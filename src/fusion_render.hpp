/**
 * fusion_render.hpp — FusionHUD 五档布局的真实自绘实现
 *
 * FusionHUD 是 Winlator 系的 Android 库（android.view.View 子类，Canvas 自绘），
 * 其观感来自「排版」而不只是配色：大号 FPS、彩色 label + 白色 value 的 chip、
 * 圆角磁贴网格、胶囊面板、帧时间折线。
 *
 * 本文件把 FusionHudView.kt 的 rebuild()/onDraw() 逐条翻译到 ImGui 的 ImDrawList：
 *   字体度量走 ImFont::CalcTextSizeA / Ascent（与 ImGui 内部 AddText 同源，基线精确对齐），
 *   每个 Span 自带字号与颜色（Monospace 观感由 MangoHud 加载的字体决定）。
 *
 * 设计约定：
 *   - header-only + 模板参数（对 swapchain_stats 只做鸭子类型），因此不需要改 meson.build，
 *     也不会和 overlay.h 形成包含环。
 *   - 所有控制项复用 MangoHud 既有开关（fps / gpu_stats / ram / frame_timing / ...），
 *     这样用户用同一套配置就能控制 FusionHUD 各档显示什么。
 *
 * 坐标系：FusionHUD 的 sp(v) = v * density * scale。这里 sp(v) = v * kSpToPx * scale，
 * 与 fusion_theme.hpp 的 fusionFontSize() 使用同一换算基准。
 *
 * 衍生自 FusionHUD（https://github.com/The412Banner/FusionHUD, GPL-3.0 + §7(b) 附加署名条款）
 * 详见 ATTRIBUTION.md。
 */
#pragma once

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "overlay_params.h"
#include "fusion_theme.hpp"

namespace fusionhud {
namespace fr {

// ============================================================================
// 基础类型（对应 FusionHudView.kt 的 Span / Glyph / HudRow / Tile / RectF）
// ============================================================================

inline ImU32 to_imcol(uint32_t argb) {
    return IM_COL32(static_cast<int>((argb >> 16) & 0xFFu),
                    static_cast<int>((argb >> 8) & 0xFFu),
                    static_cast<int>(argb & 0xFFu),
                    static_cast<int>((argb >> 24) & 0xFFu));
}

inline std::string fmt1(float v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.1f", static_cast<double>(v));
    return std::string(b);
}

/**
 * 固定宽度数字格式化。
 *
 * 为什么必须固定宽度：面板尺寸由文本宽度算出，而 HUD 的数值每帧都在变
 * （59.9 -> 9.8 少一位；3.2GiB -> 12.4GiB 多一位）。宽度一变窗口尺寸就跟着变，
 * 表现就是面板边缘/内容"抖动"。用 %*.*f 补前导空格把字段占位固定下来，
 * 面板尺寸随即恒定 —— 这也是 FusionHUD 每秒重建布局却不抖的原因。
 */
inline std::string fmt_i(int v, int width) {
    char b[64];
    std::snprintf(b, sizeof(b), "%*d", width, v);
    return std::string(b);
}

inline std::string fmt_f(float v, int width, int prec = 1) {
    char b[64];
    std::snprintf(b, sizeof(b), "%*.*f", width, prec, static_cast<double>(v));
    return std::string(b);
}

/** 名称类文本（GPU 型号 / 驱动名等）的最大宽度（sp）。
 *  超过就换行 —— 否则一个长型号会把整块面板撑得很宽，甚至溢出磁贴。 */
inline constexpr float kNameMaxWidthSp = 170.0f;

/** 无数据时的占位：补齐到同样宽度，避免占位符比数字窄而再次抖动 */
inline std::string pad_dash(int width) {
    if (width <= 1)
        return std::string("—");
    return std::string(static_cast<size_t>(width - 1), ' ') + "—";
}

inline int iround(float v) { return static_cast<int>(v + (v >= 0.0f ? 0.5f : -0.5f)); }

struct Rect {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    float w() const { return x1 - x0; }
    float h() const { return y1 - y0; }
};

/** 一段文本：文字 + 颜色 + 字号（FusionHUD 的 Span） */
struct Span {
    std::string text;
    uint32_t col = kColValue;
    float px = 12.0f;
};

/** 已定好位置的字形（top = 行顶；基线 = top + Ascent*scale） */
struct Glyph {
    float x = 0.0f;
    float top = 0.0f;
    float px = 12.0f;
    std::string text;
    uint32_t col = kColValue;
};

/** 一个 "label + value-run" 行 */
struct Row {
    Span label;
    std::vector<Span> vals;
    bool inline_row = false;
};

/** Tiles 档的磁贴 */
struct Tile {
    std::string key;
    uint32_t key_col = kColDim;
    std::vector<Span> value;
    std::string sub;
    bool has_sub = false;
    bool wide = false;
    // 宽磁贴上的长名称：按可用宽度换行（磁贴高度随之增长）
    bool wrap = false;
    std::string model;
    std::vector<std::string> lines;
};

// ============================================================================
// 文本度量 —— 直接用 ImFont 的真实度量，保证与 ImGui 的 AddText 基线一致
// ============================================================================

/** 间距紧凑系数：只作用于 pad/gap，不改变字号。
 *  FusionHUD 的 sp 会乘手机密度，直接照搬到 HUD 上显得过于空；0.75 收紧后更贴身。 */
inline constexpr float kGapScale = 0.75f;

struct Metrics {
    ImFont* font = nullptr;
    float spk = kSpToPx;   // sp → px 基准
    float scale = 1.0f;    // hudScale（本实现复用 params->font_scale）

    float sp(float v) const { return v * spk * scale; }

    /** 间距专用：在 sp 基础上再乘紧凑系数（只用于 pad/gap，不影响字形尺寸） */
    float gsp(float v) const { return v * spk * scale * kGapScale; }

    float measure(const std::string& t, float px) const {
        if (!font || t.empty())
            return 0.0f;
        return font->CalcTextSizeA(px, FLT_MAX, 0.0f, t.c_str(), t.c_str() + t.size()).x;
    }

    float run_w(const std::vector<Span>& spans) const {
        float w = 0.0f;
        for (const Span& s : spans)
            w += measure(s.text, s.px);
        return w;
    }

    /** 行顶 → 基线的偏移（ImGui AddText 内部用的就是 Ascent*scale） */
    float ascent(float px) const {
        if (!font || font->FontSize <= 0.0f)
            return px * 0.8f;
        return font->Ascent * (px / font->FontSize);
    }

    /** 行高：Android mono 的 descent-ascent ≈ 1.17em；收到 1.10 更紧凑 */
    float line_h(float px) const { return px * 1.10f; }
};

// ============================================================================
// Span 构造小工具（对应 Kotlin 的 numUnit / valueUnit / tempSpans / gap）
// ============================================================================

/**
 * 按像素宽度把文本切成多行。
 * 优先在空格处断行；单个词本身超宽时按字符硬断。返回至少 1 行。
 */
inline std::vector<std::string> wrap_text(const Metrics& m, const std::string& t, float px, float max_w) {
    std::vector<std::string> out;
    if (t.empty())
        return out;
    if (max_w <= 0.0f || m.measure(t, px) <= max_w) {
        out.push_back(t);
        return out;
    }
    std::string cur;
    size_t i = 0;
    while (i < t.size()) {
        // 取下一个"词 + 尾随空格"
        size_t j = i;
        while (j < t.size() && t[j] != ' ')
            ++j;
        if (j < t.size())
            ++j;
        std::string word = t.substr(i, j - i);

        if (!cur.empty() && m.measure(cur + word, px) > max_w) {
            out.push_back(cur);
            cur.clear();
        }
        // 单个词过长：按字符硬断
        if (cur.empty() && m.measure(word, px) > max_w) {
            std::string piece;
            for (char ch : word) {
                if (!piece.empty() && m.measure(piece + ch, px) > max_w) {
                    out.push_back(piece);
                    piece.clear();
                }
                piece += ch;
            }
            cur = piece;
            i = j;
            continue;
        }
        cur += word;
        i = j;
    }
    if (!cur.empty())
        out.push_back(cur);
    if (out.empty())
        out.push_back(t);
    return out;
}

/** 把长名称按"面板自然宽度"换行：只增加行数，不把面板撑宽。
 *  avail_px 传其余行形成的数值列宽度；再给一个下限，避免宽度退化到几个字符。 */
inline std::vector<std::string> wrap_name_to(const Metrics& m, const std::string& t,
                                            float px, float avail_px) {
    const float floor_w = m.sp(96.0f);
    return wrap_text(m, t, px, std::max(floor_w, avail_px));
}

inline Span gap(float unit_px) { return Span{"  ", kColDim, unit_px}; }

inline std::vector<Span> num_unit(const int* v, const char* unit, float px, float unit_px) {
    // MHz 是 4 位（2016），百分比是 3 位（87）—— 按用途给足固定占位宽度
    const int width = (unit && std::strcmp(unit, "MHz") == 0) ? 4 : 3;
    if (!v || *v < 0)
        return {Span{pad_dash(width), kColDim, px}, Span{unit, kColDim, unit_px}};
    return {Span{fmt_i(*v, width), kColValue, px}, Span{unit, kColDim, unit_px}};
}

/** 浮点版本：<= 0 视为无数据（FusionHUD 的 lowText 语义） */
inline std::vector<Span> num_unit_f(float v, const char* unit, float px, float unit_px) {
    if (!(v > 0.0f))
        return {Span{pad_dash(5), kColDim, px}, Span{unit, kColDim, unit_px}};
    return {Span{fmt_f(v, 5, 1), kColValue, px}, Span{unit, kColDim, unit_px}};
}

/** "3.2GiB" → 白色数字 + 灰色后缀 */
inline std::vector<Span> value_unit(const std::string& t, float px, float unit_px) {
    if (t.empty())
        return {};
    size_t lead = 0;
    while (lead < t.size() && t[lead] == ' ')
        ++lead;   // 前导空格是数字字段的固定占位，保留在白色段里
    size_t i = lead;
    while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '.' || t[i] == '-'))
        ++i;
    if (i == lead || i >= t.size())
        return {Span{t, kColValue, px}};
    return {Span{t.substr(0, i), kColValue, px}, Span{t.substr(i), kColDim, unit_px}};
}

inline std::vector<Span> temp_spans(int c, float px, float unit_px) {
    if (c < 0)
        return {};
    return {Span{fmt_i(c, 3), kColValue, px}, Span{"°C", kColDim, unit_px}};
}

inline std::string gib(float v) { return fmt_f(v, 4, 1) + "GiB"; }

// ============================================================================
// 帧率历史 —— FusionHUD 的 AVG / 1% / 0.1% / 0.01% 低帧
// MangoHud 本身不统计百分位，这里自己维护一个帧时间环形缓冲。
// ============================================================================

inline std::vector<float>& ft_history() {
    static std::vector<float> h;
    return h;
}

inline void note_frametime(float ms) {
    if (!(ms > 0.0f) || !std::isfinite(ms))
        return;
    std::vector<float>& h = ft_history();
    h.push_back(ms);
    if (h.size() > 600)
        h.erase(h.begin(), h.begin() + static_cast<long>(h.size() - 600));
}

/** 最差 frac 比例帧时间的平均帧率（frac=0.01 → 1% low） */
inline float low_fps(float frac) {
    std::vector<float> h = ft_history();
    if (h.size() < 30)
        return 0.0f;
    std::sort(h.begin(), h.end());   // 升序：越差（ms 越大）越靠后
    size_t n = static_cast<size_t>(static_cast<float>(h.size()) * frac);
    if (n < 1)
        n = 1;
    if (n > h.size())
        n = h.size();
    float sum = 0.0f;
    for (size_t i = h.size() - n; i < h.size(); ++i)
        sum += h[i];
    const float avg_ms = sum / static_cast<float>(n);
    return avg_ms > 0.0f ? 1000.0f / avg_ms : 0.0f;
}

inline float ft_min_ms() {
    const std::vector<float>& h = ft_history();
    if (h.empty())
        return 0.0f;
    return *std::min_element(h.begin(), h.end());
}

inline float ft_max_ms() {
    const std::vector<float>& h = ft_history();
    if (h.empty())
        return 0.0f;
    return *std::max_element(h.begin(), h.end());
}

/** 折线图采样：每秒 1 个点，容量 60（对应 FusionHUD 的 GRAPH_CAP=60） */
inline std::vector<float>& graph_history() {
    static std::vector<float> g;
    return g;
}

inline void note_graph_sample(float ms) {
    static double last = -1.0;
    const double now = ImGui::GetTime();
    if (last >= 0.0 && (now - last) < 1.0)
        return;
    last = now;
    std::vector<float>& g = graph_history();
    if (!(ms > 0.0f) || !std::isfinite(ms))
        return;
    g.push_back(ms);
    if (g.size() > 60)
        g.erase(g.begin(), g.begin() + static_cast<long>(g.size() - 60));
}

// ============================================================================
// 数据快照
// ============================================================================

struct Snapshot {
    float fps = 0.0f;
    float fps_avg = 0.0f;
    float ft_ms = 0.0f;
    float low1 = 0.0f, low01 = 0.0f, low001 = 0.0f;
    float ft_min = 0.0f, ft_max = 0.0f;

    int cpu_pct = -1, cpu_temp = -1, cpu_mhz = -1;
    int gpu_pct = -1, gpu_temp = -1, gpu_mhz = -1;
    float vram_used = -1.0f;

    int ram_pct = -1;
    float ram_used = -1.0f, ram_total = -1.0f;
    float swap_used = -1.0f, swap_total = -1.0f;

    int bat_pct = -1, bat_temp = -1;
    float bat_w = 0.0f;

    std::string gpu_model, engine, engine_ver, driver;
    std::string session;

    std::vector<int> core_pct, core_mhz;
};

/** 控制项：复用 MangoHud 的既有开关 */
struct Chips {
    bool fps = true, gpu = true, cpu = true;
    bool gpu_temp = false, cpu_temp = true;
    bool vram = true, ram = true, swap = false;
    bool bat = true, power = true;
    bool graph = false, low001 = true, clock = true;
    bool gpu_model = true, engine = true;
    bool per_core = true, res = true, wine = true, session = true;
};

template <class St>
inline Chips read_chips(const overlay_params& p) {
    (void)sizeof(St*);
    Chips c;
    c.fps       = p.enabled[OVERLAY_PARAM_ENABLED_fps] != 0;
    c.gpu       = p.enabled[OVERLAY_PARAM_ENABLED_gpu_stats] != 0;
    c.cpu       = p.enabled[OVERLAY_PARAM_ENABLED_cpu_stats] != 0;
    c.gpu_temp  = p.enabled[OVERLAY_PARAM_ENABLED_gpu_temp] != 0;
    c.cpu_temp  = p.enabled[OVERLAY_PARAM_ENABLED_cpu_temp] != 0;
    c.vram      = p.enabled[OVERLAY_PARAM_ENABLED_vram] != 0;
    c.ram       = p.enabled[OVERLAY_PARAM_ENABLED_ram] != 0;
    c.swap      = p.enabled[OVERLAY_PARAM_ENABLED_swap] != 0;
    c.bat       = p.enabled[OVERLAY_PARAM_ENABLED_battery] != 0;
    c.power     = p.enabled[OVERLAY_PARAM_ENABLED_battery_watt] != 0;
    c.graph     = p.enabled[OVERLAY_PARAM_ENABLED_frame_timing] != 0;
    c.clock     = p.enabled[OVERLAY_PARAM_ENABLED_time] != 0;
    c.gpu_model = p.enabled[OVERLAY_PARAM_ENABLED_gpu_name] != 0;
    c.engine    = p.enabled[OVERLAY_PARAM_ENABLED_engine_version] != 0;
    c.per_core  = p.enabled[OVERLAY_PARAM_ENABLED_core_load] != 0;
    c.res       = p.enabled[OVERLAY_PARAM_ENABLED_resolution] != 0;
    c.wine      = p.enabled[OVERLAY_PARAM_ENABLED_wine] != 0;
    c.session   = p.enabled[OVERLAY_PARAM_ENABLED_duration] != 0;
    c.low001    = true;
    return c;
}

/**
 * 采集数值快照。
 * data_source_* 由调用方（overlay.cpp）注入，避免本头文件依赖 overlay.h / cpu.h / gpu.h。
 */
struct Sources {
    double fps = 0.0;
    double frametime_ms = 0.0;
    std::string gpu_name, engine_name_str, engine_version, driver_name;
    int gpu_load = -1, gpu_temp = -1, gpu_core_clock = -1;
    float vram_used_gib = -1.0f;
    float cpu_load = -1.0f;
    int cpu_temp = -1, cpu_mhz = -1;
    float ram_used_gib = -1.0f, ram_total_gib = -1.0f;
    float swap_used_gib = -1.0f, swap_total_gib = -1.0f;
    int bat_pct = -1, bat_temp = -1;
    float bat_watts = 0.0f;
    std::vector<int> core_pct, core_mhz;
};

inline Snapshot make_snapshot(const Sources& s) {
    Snapshot o;
    o.fps = static_cast<float>(s.fps);
    o.ft_ms = static_cast<float>(s.frametime_ms);
    if (!(o.fps > 0.0f) && o.ft_ms > 0.0f)
        o.fps = 1000.0f / o.ft_ms;
    o.fps_avg = o.fps;
    o.low1 = low_fps(0.01f);
    o.low01 = low_fps(0.001f);
    o.low001 = low_fps(0.0001f);
    o.ft_min = ft_min_ms();
    o.ft_max = ft_max_ms();

    o.gpu_pct = s.gpu_load;
    o.gpu_temp = s.gpu_temp;
    o.gpu_mhz = s.gpu_core_clock;
    o.vram_used = s.vram_used_gib;

    o.cpu_pct = s.cpu_load >= 0.0f ? iround(s.cpu_load) : -1;
    o.cpu_temp = s.cpu_temp;
    o.cpu_mhz = s.cpu_mhz;

    o.ram_used = s.ram_used_gib;
    o.ram_total = s.ram_total_gib;
    o.swap_used = s.swap_used_gib;
    o.swap_total = s.swap_total_gib;
    if (s.ram_total_gib > 0.0f && s.ram_used_gib >= 0.0f)
        o.ram_pct = iround(100.0f * s.ram_used_gib / s.ram_total_gib);

    o.bat_pct = s.bat_pct;
    o.bat_temp = s.bat_temp;
    o.bat_w = s.bat_watts;

    o.gpu_model = s.gpu_name;
    o.engine = s.engine_name_str;
    o.engine_ver = s.engine_version;
    o.driver = s.driver_name;
    o.core_pct = s.core_pct;
    o.core_mhz = s.core_mhz;
    return o;
}

// ============================================================================
// Frame —— 布局结果（glyphs / tiles / 面板 / 折线）与五档构建
// ============================================================================

struct Frame {
    Metrics M;
    std::vector<Glyph> glyphs;
    std::vector<Rect> tiles;
    bool has_pill = false;
    Rect pill;
    bool has_graph = false;
    Rect graph;
    float content_w = 0.0f;
    float content_h = 0.0f;
    std::string credit;   // 画面署名（自绘模式下由本渲染器负责显示）

    /** 放一段 run，返回结束 x */
    float place(float x, float baseline, const std::vector<Span>& spans) {
        for (const Span& s : spans) {
            if (s.text.empty())
                continue;
            Glyph g;
            g.x = x;
            g.top = baseline - M.ascent(s.px);
            g.px = s.px;
            g.text = s.text;
            g.col = s.col;
            glyphs.push_back(g);
            x += M.measure(s.text, s.px);
        }
        return x;
    }
};

/** 底部右下角的低调时钟（对应 addSubtleClock） */
inline void add_subtle_clock(Frame& f, bool enabled, float pad, float center_x = -1.0f) {
    if (!enabled)
        return;
    const float px = f.M.sp(9.5f);
    char buf[32];
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    std::strftime(buf, sizeof(buf), "%H:%M", &tmv);
    const std::string txt(buf);
    const float w = f.M.measure(txt, px);

    const float footer_top = f.content_h - pad;
    const float baseline = footer_top - f.M.ascent(px);
    float x;
    if (center_x >= 0.0f) {
        x = center_x - w * 0.5f;
        const float lo = pad;
        const float hi = std::max(pad, f.content_w - pad - w);
        x = std::min(std::max(x, lo), hi);
    } else {
        x = std::max(pad, f.content_w - pad - w);
    }
    Glyph g;
    g.x = x;
    g.top = baseline - f.M.ascent(px);
    g.px = px;
    g.text = txt;
    g.col = kColDim;
    f.glyphs.push_back(g);

    f.content_h = footer_top + f.M.line_h(px) + pad * 0.5f;
    f.content_w = std::max(f.content_w, pad + w + pad);
}

/**
 * 画面底部的低调署名行。
 *
 * 为什么必须由自绘路径负责：`custom_text_center` 是 MangoHud 表格路径里的项
 * （hud_elements.cpp 的 ordered_functions），而 FusionHUD 档位整块自绘、不再走表格，
 * 于是署名行会消失。GPL-3.0 §7(b) 的附加署名条款要求它在应用内可见，
 * 所以这里在面板底部统一补一行。
 */
inline void add_credit_line(Frame& f, float pad) {
    if (f.credit.empty() || f.content_w <= 0.0f || f.content_h <= 0.0f)
        return;
    const float px = f.M.sp(9.5f);
    const float w = f.M.measure(f.credit, px);
    // 放在底部时钟下方，单独占一行，避免和时钟重叠
    const float baseline = f.content_h + f.M.line_h(px) * 0.5f;
    const float x = std::max(pad, (f.content_w - w) * 0.5f);
    Glyph g;
    g.x = x;
    g.top = baseline - f.M.ascent(px);
    g.px = px;
    g.text = f.credit;
    g.col = kColDim;
    f.glyphs.push_back(g);
    f.content_h += f.M.line_h(px) + pad * 0.5f;
    f.content_w = std::max(f.content_w, pad + w + pad);
}

inline std::string api_label(const Snapshot& s, const Chips& c) {
    if (c.engine && !s.engine.empty())
        return s.engine;
    return "FPS";
}

inline std::string dx_version_labeled(const Snapshot& s) {
    if (s.engine_ver.empty())
        return "";
    if (s.engine.find("VKD3D") != std::string::npos || s.engine.find("vkd3d") != std::string::npos)
        return "VKD3D " + s.engine_ver;
    if (s.engine.find("DXVK") != std::string::npos || s.engine.find("dxvk") != std::string::npos)
        return "DXVK " + s.engine_ver;
    return s.engine_ver;
}

inline std::string session_string() {
    static const double t0 = ImGui::GetTime();
    const double sec_d = ImGui::GetTime() - t0;
    long sec = static_cast<long>(sec_d);
    if (sec < 0)
        sec = 0;
    const long h = sec / 3600;
    const long m = (sec % 3600) / 60;
    const long s = sec % 60;
    char b[48];
    if (h > 0)
        std::snprintf(b, sizeof(b), "%ld:%02ld:%02ld", h, m, s);
    else
        std::snprintf(b, sizeof(b), "%02ld:%02ld", m, s);
    return std::string(b);
}

// ---------------------------------------------------------------- FULL -----

/** 一列 "label + value" 行；返回 pair<bottomY, rightX> */
inline void layout_column(Frame& f, const std::vector<Row>& rows, float x, float y0,
                          float row_px, float line_gap, float lv_gap,
                          float& bottom, float& right) {
    bottom = y0;
    right = x;
    if (rows.empty())
        return;
    float label_col = 0.0f;
    for (const Row& r : rows)
        if (!r.inline_row)
            label_col = std::max(label_col, f.M.measure(r.label.text, row_px));
    const float h = f.M.line_h(row_px);
    const float asc = f.M.ascent(row_px);
    float y = y0;
    right = std::max(right, x + label_col + lv_gap);
    for (const Row& r : rows) {
        const float baseline = y - asc;
        f.place(x, baseline, std::vector<Span>{r.label});
        const float val_x = r.inline_row
            ? x + f.M.measure(r.label.text, row_px) + lv_gap
            : x + label_col + lv_gap;
        const float end = f.place(val_x, baseline, r.vals);
        right = std::max(right, end);
        y += h + line_gap;
    }
    bottom = y - line_gap;
}

inline void build_full(Frame& f, const Snapshot& s, const Chips& c) {
    const float row_px = f.M.sp(12.0f);
    const float unit_px = row_px * 0.62f;
    const float pad = f.M.gsp(10.0f);
    const float line_gap = f.M.gsp(4.0f);
    const float lv_gap = f.M.gsp(8.0f);

    std::vector<Row> rows;
    auto add = [&](const char* label, uint32_t lcol, std::vector<Span> vals) {
        Row r;
        r.label = Span{label, lcol, row_px};
        r.vals = std::move(vals);
        rows.push_back(std::move(r));
    };

    // 型号先不落行：等其它行算完"自然宽度"后再定换行点（两遍布局）
    const std::string model_txt =
        (c.gpu_model && !s.gpu_model.empty()) ? s.gpu_model : std::string();
    if (c.gpu) {
        std::vector<Span> v;
        for (const Span& x : num_unit(s.gpu_pct >= 0 ? &s.gpu_pct : nullptr, "%", row_px, unit_px))
            v.push_back(x);
        if (c.gpu_temp) {
            std::vector<Span> t = temp_spans(s.gpu_temp, row_px, unit_px);
            if (!t.empty()) {
                v.push_back(gap(unit_px));
                for (const Span& x : t)
                    v.push_back(x);
            }
        }
        v.push_back(gap(unit_px));
        for (const Span& x : num_unit(s.gpu_mhz > 0 ? &s.gpu_mhz : nullptr, "MHz", row_px, unit_px))
            v.push_back(x);
        add("GPU", kColGpu, std::move(v));
    }
    if (c.cpu) {
        std::vector<Span> v;
        for (const Span& x : num_unit(s.cpu_pct >= 0 ? &s.cpu_pct : nullptr, "%", row_px, unit_px))
            v.push_back(x);
        if (c.cpu_temp) {
            std::vector<Span> t = temp_spans(s.cpu_temp, row_px, unit_px);
            if (!t.empty()) {
                v.push_back(gap(unit_px));
                for (const Span& x : t)
                    v.push_back(x);
            }
        }
        v.push_back(gap(unit_px));
        for (const Span& x : num_unit(s.cpu_mhz > 0 ? &s.cpu_mhz : nullptr, "MHz", row_px, unit_px))
            v.push_back(x);
        add("CPU", kColCpu, std::move(v));
    }
    if (c.vram && s.vram_used >= 0.0f)
        add("VRAM", kColVram, value_unit(gib(s.vram_used), row_px, unit_px));
    if (c.ram) {
        std::vector<Span> v;
        if (s.ram_used >= 0.0f)
            for (const Span& x : value_unit(gib(s.ram_used), row_px, unit_px))
                v.push_back(x);
        v.push_back(gap(unit_px));
        for (const Span& x : num_unit(s.ram_pct >= 0 ? &s.ram_pct : nullptr, "%", row_px, unit_px))
            v.push_back(x);
        add("RAM", kColRam, std::move(v));
    }
    if (c.bat || c.power) {
        std::vector<Span> v;
        bool any = false;
        if (c.bat && s.bat_pct >= 0) {
            for (const Span& x : num_unit(&s.bat_pct, "%", row_px, unit_px))
                v.push_back(x);
            any = true;
        }
        if (c.power && s.bat_w > 0.0f) {
            if (any)
                v.push_back(gap(unit_px));
            for (const Span& x : num_unit_f(s.bat_w, "W", row_px, unit_px))
                v.push_back(x);
            any = true;
        }
        if (any)
            add("BAT", kColBat, std::move(v));
    }
    if (c.fps) {
        std::vector<Span> v;
        for (const Span& x : num_unit_f(s.fps, "FPS", row_px, unit_px))
            v.push_back(x);
        v.push_back(gap(unit_px));
        for (const Span& x : num_unit_f(1000.0f / std::max(s.fps, 1.0f), "ms", row_px, unit_px))
            v.push_back(x);
        add(api_label(s, c).c_str(), kColFps, std::move(v));

        add("AVG", kColLo, num_unit_f(s.fps_avg, "FPS", row_px, unit_px));
        add("1%", kColLo, num_unit_f(s.low1, "FPS", row_px, unit_px));
        add("0.1%", kColLo, num_unit_f(s.low01, "FPS", row_px, unit_px));
        if (c.low001)
            add("0.01%", kColLo, num_unit_f(s.low001, "FPS", row_px, unit_px));
    }

    if (!model_txt.empty()) {
        float lw = f.M.measure("GPU", row_px);
        float vw = 0.0f;
        for (const Row& r : rows) {
            lw = std::max(lw, f.M.measure(r.label.text, row_px));
            vw = std::max(vw, f.M.run_w(r.vals));
        }
        const std::vector<std::string> ml = wrap_name_to(f.M, model_txt, row_px, vw + pad);
        std::vector<Row> mrows;
        mrows.push_back(Row{Span{"GPU", kColGpu, row_px}, {Span{ml[0], kColValue, row_px}}, false});
        for (size_t i = 1; i < ml.size(); ++i)
            mrows.push_back(Row{Span{"", kColValue, row_px}, {Span{ml[i], kColValue, row_px}}, false});
        rows.insert(rows.begin(), mrows.begin(), mrows.end());
    }

    float label_col = 0.0f;
    for (const Row& r : rows)
        if (!r.inline_row)
            label_col = std::max(label_col, f.M.measure(r.label.text, row_px));
    const float h = f.M.line_h(row_px);
    const float asc = f.M.ascent(row_px);
    float y = pad;
    float max_right = pad + label_col + lv_gap;
    for (const Row& r : rows) {
        const float baseline = y - asc;
        f.place(pad, baseline, std::vector<Span>{r.label});
        // 数值紧跟标签列左对齐（FusionHUD 原设计），不留多余空隙
        const float val_x = r.inline_row
            ? pad + f.M.measure(r.label.text, row_px) + lv_gap
            : pad + label_col + lv_gap;
        const float end = f.place(val_x, baseline, r.vals);
        max_right = std::max(max_right, end);
        y += h + line_gap;
    }

    if (c.fps && c.graph) {
        const float ft_px = unit_px * 1.15f;
        const float baseline = y - asc;
        f.place(pad, baseline, std::vector<Span>{Span{"Frametime", kColFps, ft_px}});
        const std::string stat = "min:" + fmt_f(s.ft_min, 5, 1) + " max:" + fmt_f(s.ft_max, 5, 1);
        const float stat_x = pad + std::max(label_col, f.M.measure("Frametime", ft_px)) + lv_gap;
        const float end = f.place(stat_x, baseline, std::vector<Span>{Span{stat, kColDim, unit_px}});
        max_right = std::max(max_right, end);
        y += h + line_gap;
        const float gh = f.M.sp(kGraphHeightSp);
        const float right = std::max(max_right, pad + f.M.sp(kFullGraphMinSp));
        f.graph = Rect{pad, y, right, y + gh};
        f.has_graph = true;
        max_right = std::max(max_right, right);
        y += gh;
    }

    f.content_w = max_right + pad;
    f.content_h = y + pad;
    add_subtle_clock(f, c.clock, pad);
}

// --------------------------------------------------------------- TILES -----

inline void build_tiles(Frame& f, const Snapshot& s, const Chips& c) {
    const float key_px = f.M.sp(10.0f);
    const float val_px = f.M.sp(18.0f);
    const float sub_px = f.M.sp(10.0f);
    const float unit_px = f.M.sp(11.0f);
    const float pad = f.M.gsp(9.0f);
    const float inner_pad = f.M.gsp(8.0f);
    const float tile_gap = f.M.gsp(6.0f);
    const float line_gap = f.M.gsp(4.0f);

    std::vector<Tile> tiles;
    auto push = [&](Tile t) { tiles.push_back(std::move(t)); };

    if (c.fps) {
        Tile t;
        t.key = "FPS";
        t.key_col = kColFps;
        t.value = {Span{fmt_f(s.fps, 5, 1), kColValue, val_px}};
        t.sub = fmt_f(s.fps_avg, 5, 1) + " avg · " +
                (s.low1 > 0.0f ? fmt_f(s.low1, 5, 1) : pad_dash(5)) + " 1%";
        t.has_sub = true;
        push(std::move(t));

        Tile fr;
        fr.key = "FRAME";
        fr.key_col = kColDim;
        fr.value = num_unit_f(1000.0f / std::max(s.fps, 1.0f), "ms", val_px, unit_px);
        fr.sub = fmt_f(s.ft_min, 5, 1) + " – " + fmt_f(s.ft_max, 5, 1);
        fr.has_sub = true;
        push(std::move(fr));
    }
    if (c.gpu) {
        Tile t;
        t.key = "GPU";
        t.key_col = kColGpu;
        t.value = num_unit(s.gpu_pct >= 0 ? &s.gpu_pct : nullptr, "%", val_px, unit_px);
        if (c.gpu_temp && s.gpu_temp >= 0)
            t.sub = fmt_i(s.gpu_temp, 3) + "°C";
        else if (s.gpu_mhz > 0)
            t.sub = fmt_i(s.gpu_mhz, 4) + "MHz";
        t.has_sub = !t.sub.empty();
        push(std::move(t));
    }
    if (c.cpu) {
        Tile t;
        t.key = "CPU";
        t.key_col = kColCpu;
        t.value = num_unit(s.cpu_pct >= 0 ? &s.cpu_pct : nullptr, "%", val_px, unit_px);
        std::string sub;
        if (c.cpu_temp && s.cpu_temp >= 0)
            sub = fmt_i(s.cpu_temp, 3) + "°C";
        if (s.cpu_mhz > 0) {
            if (!sub.empty())
                sub += " · ";
            sub += fmt_i(s.cpu_mhz, 4);
        }
        t.sub = sub;
        t.has_sub = !sub.empty();
        push(std::move(t));
    }
    if (c.vram && s.vram_used >= 0.0f) {
        Tile t;
        t.key = "VRAM";
        t.key_col = kColVram;
        t.value = value_unit(gib(s.vram_used), val_px, unit_px);
        push(std::move(t));
    }
    if (c.ram) {
        Tile t;
        t.key = "RAM";
        t.key_col = kColRam;
        t.value = num_unit(s.ram_pct >= 0 ? &s.ram_pct : nullptr, "%", val_px, unit_px);
        if (s.ram_used >= 0.0f)
            t.sub = gib(s.ram_used) + (s.ram_total > 0.0f ? (" / " + gib(s.ram_total)) : "");
        t.has_sub = !t.sub.empty();
        push(std::move(t));
    }
    if (c.engine && !s.engine.empty()) {
        Tile t;
        t.key = "API";
        t.key_col = kColFps;
        t.value = {Span{s.engine, kColValue, val_px}};
        t.sub = s.engine_ver;
        t.has_sub = !t.sub.empty();
        push(std::move(t));
    }
    if (c.gpu_model && !s.gpu_model.empty()) {
        Tile t;
        t.key = "GPU";
        t.key_col = kColGpu;
        t.value = {Span{s.gpu_model, kColValue, val_px}};
        t.wide = true;
        t.wrap = true;              // 型号按磁贴可用宽度换行
        t.model = s.gpu_model;
        push(std::move(t));
    }
    if (c.bat || c.power) {
        std::vector<Span> parts;
        bool any = false;
        if (c.bat && s.bat_pct >= 0) {
            for (const Span& x : num_unit(&s.bat_pct, "%", val_px, unit_px))
                parts.push_back(x);
            any = true;
        }
        if (c.power && s.bat_w > 0.0f) {
            if (any)
                parts.push_back(Span{" · ", kColDim, sub_px});
            for (const Span& x : num_unit_f(s.bat_w, "W", val_px, unit_px))
                parts.push_back(x);
            any = true;
        }
        if (any) {
            Tile t;
            t.key = "BAT";
            t.key_col = kColBat;
            t.value = std::move(parts);
            t.wide = true;
            push(std::move(t));
        }
    }
    if (tiles.empty()) {
        f.content_w = 0.0f;
        f.content_h = 0.0f;
        return;
    }

    const float key_h = f.M.line_h(key_px);
    const float val_h = f.M.line_h(val_px);
    const float sub_h = f.M.line_h(sub_px);

    auto tile_w = [&](const Tile& t) {
        float w = std::max(f.M.measure(t.key, key_px), f.M.run_w(t.value));
        if (t.has_sub)
            w = std::max(w, f.M.measure(t.sub, sub_px));
        return w + inner_pad * 2.0f;
    };

    float normal_w = 0.0f;
    for (const Tile& t : tiles)
        if (!t.wide)
            normal_w = std::max(normal_w, tile_w(t));
    if (normal_w == 0.0f)
        for (const Tile& t : tiles)
            normal_w = std::max(normal_w, tile_w(t) / 2.0f);

    const float tile_h = inner_pad * 2.0f + key_h + line_gap + val_h + line_gap + sub_h;
    const float full_w = normal_w * 2.0f + tile_gap;

    // 宽磁贴若带长名称，先按可用宽度切行，再算它自己的高度
    for (Tile& t : tiles) {
        if (t.wrap && !t.model.empty())
            t.lines = wrap_text(f.M, t.model, val_px, full_w - inner_pad * 2.0f);
    }
    auto tile_height = [&](const Tile& t) {
        const float nl = (t.wrap && !t.lines.empty()) ? static_cast<float>(t.lines.size()) : 1.0f;
        float h = inner_pad * 2.0f + key_h + line_gap + nl * val_h + (nl - 1.0f) * line_gap;
        if (t.has_sub)
            h += line_gap + sub_h;
        if (h < tile_h)
            h = tile_h;   // 不足标准高度就补齐，保证观感一致
        return h;
    };
    auto place_tile = [&](const Tile& t, float tx, float ty, float tw) {
        const float th = tile_height(t);
        f.tiles.push_back(Rect{tx, ty, tx + tw, ty + th});
        float by = ty + inner_pad + f.M.ascent(key_px);
        f.place(tx + inner_pad, by, std::vector<Span>{Span{t.key, t.key_col, key_px}});
        by = ty + inner_pad + key_h + line_gap;
        if (t.wrap && !t.lines.empty()) {
            for (const std::string& ln : t.lines) {
                f.place(tx + inner_pad, by + f.M.ascent(val_px),
                        std::vector<Span>{Span{ln, kColValue, val_px}});
                by += val_h + line_gap;
            }
        } else {
            f.place(tx + inner_pad, by + f.M.ascent(val_px), t.value);
            by += val_h + line_gap;
        }
        if (t.has_sub)
            f.place(tx + inner_pad, by + f.M.ascent(sub_px),
                    std::vector<Span>{Span{t.sub, kColDim, sub_px}});
        return th;
    };

    float x = pad, y = pad;
    int col = 0;
    for (const Tile& t : tiles) {
        if (t.wide) {
            if (col != 0) {
                y += tile_h + tile_gap;
                col = 0;
                x = pad;
            }
            const float th = place_tile(t, pad, y, full_w);
            y += th + tile_gap;
            col = 0;
            x = pad;
        } else {
            place_tile(t, x, y, normal_w);
            col++;
            x += normal_w + tile_gap;
            if (col == 2) {
                col = 0;
                x = pad;
                y += tile_h + tile_gap;
            }
        }
    }
    if (col != 0)
        y += tile_h + tile_gap;

    f.content_w = pad + full_w + pad;
    f.content_h = y + (pad - tile_gap);
    add_subtle_clock(f, c.clock, pad);
}

// ---------------------------------------------------------------- PILL -----

inline void build_pill(Frame& f, const Snapshot& s, const Chips& c) {
    const float big_px = f.M.sp(30.0f);
    const float big_unit_px = big_px * 0.36f;
    const float stk_px = f.M.sp(11.5f);
    const float pad = f.M.gsp(10.0f);
    const float mid_gap = f.M.gsp(12.0f);
    const float stk_line_gap = f.M.gsp(3.0f);

    std::vector<Span> left = {Span{fmt_f(s.fps, 5, 1), kColValue, big_px},
                              Span{"fps", kColDim, big_unit_px}};

    std::vector<std::vector<Span>> stack;
    const std::string model_txt =
        (c.gpu_model && !s.gpu_model.empty()) ? s.gpu_model : std::string();
    {
        std::vector<Span> l;
        if (c.gpu) {
            l.push_back(Span{"GPU ", kColGpu, stk_px});
            l.push_back(Span{s.gpu_pct >= 0 ? fmt_i(s.gpu_pct, 3) : pad_dash(3), kColGpu, stk_px});
            l.push_back(Span{"%", kColGpu, stk_px});
        }
        if (c.cpu) {
            if (!l.empty())
                l.push_back(Span{" · ", kColDim, stk_px});
            l.push_back(Span{"CPU ", kColCpu, stk_px});
            l.push_back(Span{s.cpu_pct >= 0 ? fmt_i(s.cpu_pct, 3) : pad_dash(3), kColCpu, stk_px});
            l.push_back(Span{"%", kColCpu, stk_px});
        }
        if (!l.empty())
            stack.push_back(std::move(l));
    }
    if (c.ram) {
        stack.push_back({Span{"RAM ", kColRam, stk_px},
                         Span{s.ram_pct >= 0 ? fmt_i(s.ram_pct, 3) : pad_dash(3), kColRam, stk_px},
                         Span{"%", kColRam, stk_px}});
    }
    if (c.bat || c.power) {
        std::vector<Span> l;
        bool any = false;
        if (c.bat && s.bat_pct >= 0) {
            l.push_back(Span{"BAT ", kColBat, stk_px});
            l.push_back(Span{fmt_i(s.bat_pct, 3), kColBat, stk_px});
            l.push_back(Span{"%", kColBat, stk_px});
            any = true;
        }
        if (c.power && s.bat_w > 0.0f) {
            if (any)
                l.push_back(Span{" · ", kColDim, stk_px});
            l.push_back(Span{fmt_f(s.bat_w, 4, 1) + "W", kColDim, stk_px});
            any = true;
        }
        if (any)
            stack.push_back(std::move(l));
    }
    {
        std::vector<Span> l;
        l.push_back(Span{fmt_f(1000.0f / std::max(s.fps, 1.0f), 5, 1) + "ms", kColDim, stk_px});
        if (c.vram && s.vram_used >= 0.0f) {
            l.push_back(Span{" · ", kColDim, stk_px});
            l.push_back(Span{gib(s.vram_used) + " vram", kColDim, stk_px});
        }
        stack.push_back(std::move(l));
    }

    const std::string api_str = api_label(s, c);
    const bool has_api = api_str != "FPS";
    const float api_h = has_api ? f.M.line_h(stk_px) : 0.0f;
    const float api_gap = has_api ? stk_line_gap : 0.0f;

    const float left_w = f.M.run_w(left);
    const float left_h = f.M.line_h(big_px);
    const float api_w = has_api ? f.M.measure(api_str, stk_px) : 0.0f;
    const float left_block_w = std::max(left_w, api_w);
    const float left_col_h = api_h + api_gap + left_h;

    const float stk_h = f.M.line_h(stk_px);
    float stack_w = 0.0f;
    for (const std::vector<Span>& l : stack)
        stack_w = std::max(stack_w, f.M.run_w(l));
    if (!model_txt.empty()) {
        // 型号放到栈顶，并按栈的自然宽度换行（不撑宽胶囊）
        std::vector<std::vector<Span>> mlines;
        for (const std::string& ln : wrap_name_to(f.M, model_txt, stk_px, stack_w))
            mlines.push_back({Span{ln, kColDim, stk_px}});
        stack.insert(stack.begin(), mlines.begin(), mlines.end());
        for (const std::vector<Span>& l : stack)
            stack_w = std::max(stack_w, f.M.run_w(l));
    }
    const float n = static_cast<float>(stack.size());
    const float stack_total_h = n * stk_h + std::max(0.0f, n - 1.0f) * stk_line_gap;
    const float inner_h = std::max(left_col_h, stack_total_h);

    f.content_w = pad + left_block_w + mid_gap + stack_w + pad;
    f.content_h = pad + inner_h + pad;

    float ly = pad + (inner_h - left_col_h) * 0.5f;
    if (has_api) {
        f.place(pad + (left_block_w - api_w) * 0.5f, ly + f.M.ascent(stk_px),
                std::vector<Span>{Span{api_str, kColFps, stk_px}});
        ly += api_h + api_gap;
    }
    f.place(pad + (left_block_w - left_w) * 0.5f, ly + f.M.ascent(big_px), left);

    float sy = pad + (inner_h - stack_total_h) * 0.5f;
    for (const std::vector<Span>& l : stack) {
        f.place(pad + left_block_w + mid_gap, sy + f.M.ascent(stk_px), l);
        sy += stk_h + stk_line_gap;
    }

    add_subtle_clock(f, c.clock, pad, pad + left_block_w * 0.5f);
    f.has_pill = true;
    f.pill = Rect{0.0f, 0.0f, f.content_w, f.content_h};
}

// ------------------------------------------------------------- MINIMAL -----

inline void build_minimal(Frame& f, const Snapshot& s, const Chips& c) {
    const float big_px = f.M.sp(34.0f);
    const float big_unit_px = big_px * 0.32f;
    const float sub_px = f.M.sp(11.5f);
    const float pad = f.M.gsp(10.0f);
    const float line_gap = f.M.gsp(6.0f);

    const std::vector<Span> big = {Span{fmt_f(s.fps, 5, 1), kColValue, big_px},
                                   Span{"fps", kColDim, big_unit_px}};
    std::vector<Span> sub;
    sub.push_back(Span{"1% ", kColDim, sub_px});
    sub.push_back(Span{s.low1 > 0.0f ? fmt_f(s.low1, 5, 1) : pad_dash(5), kColFps, sub_px});
    sub.push_back(Span{"  ·  0.1% ", kColDim, sub_px});
    sub.push_back(Span{s.low01 > 0.0f ? fmt_f(s.low01, 5, 1) : pad_dash(5), kColFps, sub_px});
    if (c.low001) {
        sub.push_back(Span{"  ·  0.01% ", kColDim, sub_px});
        sub.push_back(Span{s.low001 > 0.0f ? fmt_f(s.low001, 5, 1) : pad_dash(5), kColFps, sub_px});
    }

    const float big_w = f.M.run_w(big);
    const float big_h = f.M.line_h(big_px);
    const float sub_w = f.M.run_w(sub);
    const float sub_h = f.M.line_h(sub_px);
    const float gw = f.M.sp(kMinimalGraphSp);
    const float gh = f.M.sp(kGraphHeightSp);
    const float inner = std::max(std::max(big_w, sub_w), c.graph ? gw : 0.0f);

    f.content_w = inner + pad * 2.0f;
    float y = pad;
    f.place(pad + (inner - big_w) * 0.5f, y + f.M.ascent(big_px), big);
    y += big_h + line_gap;
    f.place(pad + (inner - sub_w) * 0.5f, y + f.M.ascent(sub_px), sub);
    y += sub_h + line_gap;
    if (c.graph) {
        f.graph = Rect{pad + (inner - gw) * 0.5f, y, pad + (inner - gw) * 0.5f + gw, y + gh};
        f.has_graph = true;
        y += gh;
    }
    f.content_h = y + pad;

    if (c.engine && !s.engine.empty()) {
        const float api_px = f.M.sp(9.5f);
        const float footer_top = f.content_h - pad;
        Glyph g;
        g.x = pad;
        g.top = footer_top - f.M.ascent(api_px);
        g.px = api_px;
        g.text = s.engine;
        g.col = kColDim;
        f.glyphs.push_back(g);
        const float api_w = f.M.measure(s.engine, api_px);
        const float clock_w = c.clock ? f.M.measure("00:00", api_px) : 0.0f;
        f.content_w = std::max(f.content_w, pad + api_w + f.M.sp(12.0f) + clock_w + pad);
        if (!c.clock)
            f.content_h = footer_top + f.M.line_h(api_px) + pad * 0.5f;
    }
    add_subtle_clock(f, c.clock, pad);
}

// ----------------------------------------------------------------- MEGA -----

inline void build_mega(Frame& f, const Snapshot& s, const Chips& c) {
    const float row_px = f.M.sp(11.5f);
    const float unit_px = row_px * 0.62f;
    const float band_px = f.M.sp(9.5f);
    const float pad = f.M.gsp(10.0f);
    const float line_gap = f.M.gsp(3.5f);
    const float lv_gap = f.M.gsp(7.0f);
    const float gutter = f.M.gsp(16.0f);

    std::vector<Row> left;
    const std::string model_txt =
        (c.gpu_model && !s.gpu_model.empty()) ? s.gpu_model : std::string();
    if (c.gpu) {
        std::vector<Span> v;
        for (const Span& x : num_unit(s.gpu_pct >= 0 ? &s.gpu_pct : nullptr, "%", row_px, unit_px))
            v.push_back(x);
        if (c.gpu_temp) {
            std::vector<Span> t = temp_spans(s.gpu_temp, row_px, unit_px);
            if (!t.empty()) {
                v.push_back(gap(unit_px));
                for (const Span& x : t)
                    v.push_back(x);
            }
        }
        v.push_back(gap(unit_px));
        for (const Span& x : num_unit(s.gpu_mhz > 0 ? &s.gpu_mhz : nullptr, "MHz", row_px, unit_px))
            v.push_back(x);
        left.push_back(Row{Span{"GPU", kColGpu, row_px}, std::move(v), false});
    }
    if (c.cpu) {
        std::vector<Span> v;
        for (const Span& x : num_unit(s.cpu_pct >= 0 ? &s.cpu_pct : nullptr, "%", row_px, unit_px))
            v.push_back(x);
        if (c.cpu_temp) {
            std::vector<Span> t = temp_spans(s.cpu_temp, row_px, unit_px);
            if (!t.empty()) {
                v.push_back(gap(unit_px));
                for (const Span& x : t)
                    v.push_back(x);
            }
        }
        v.push_back(gap(unit_px));
        for (const Span& x : num_unit(s.cpu_mhz > 0 ? &s.cpu_mhz : nullptr, "MHz", row_px, unit_px))
            v.push_back(x);
        left.push_back(Row{Span{"CPU", kColCpu, row_px}, std::move(v), false});
    }
    if (c.per_core) {
        const size_t n = std::max(s.core_pct.size(), s.core_mhz.size());
        for (size_t i = 0; i < n; ++i) {
            std::vector<Span> v;
            const int pct = (i < s.core_pct.size() && s.core_pct[i] >= 0) ? s.core_pct[i] : -1;
            for (const Span& x : num_unit(pct >= 0 ? &pct : nullptr, "%", row_px, unit_px))
                v.push_back(x);
            v.push_back(gap(unit_px));
            const int clk = (i < s.core_mhz.size() && s.core_mhz[i] > 0) ? s.core_mhz[i] : -1;
            for (const Span& x : num_unit(clk > 0 ? &clk : nullptr, "MHz", row_px, unit_px))
                v.push_back(x);
            left.push_back(Row{Span{"C" + std::to_string(i), kColCpu, row_px}, std::move(v), false});
        }
    }

    std::vector<Row> right;
    if (c.vram && s.vram_used >= 0.0f)
        right.push_back(Row{Span{"VRAM", kColVram, row_px}, value_unit(gib(s.vram_used), row_px, unit_px), false});
    if (c.ram) {
        std::vector<Span> v;
        if (s.ram_used >= 0.0f)
            for (const Span& x : value_unit(gib(s.ram_used), row_px, unit_px))
                v.push_back(x);
        if (s.ram_total > 0.0f) {
            v.push_back(Span{"/", kColDim, unit_px});
            for (const Span& x : value_unit(gib(s.ram_total), row_px, unit_px))
                v.push_back(x);
        }
        v.push_back(gap(unit_px));
        for (const Span& x : num_unit(s.ram_pct >= 0 ? &s.ram_pct : nullptr, "%", row_px, unit_px))
            v.push_back(x);
        right.push_back(Row{Span{"RAM", kColRam, row_px}, std::move(v), false});
    }
    if (c.swap && s.swap_used >= 0.0f) {
        std::vector<Span> v;
        for (const Span& x : value_unit(gib(s.swap_used), row_px, unit_px))
            v.push_back(x);
        if (s.swap_total > 0.0f) {
            v.push_back(Span{"/", kColDim, unit_px});
            for (const Span& x : value_unit(gib(s.swap_total), row_px, unit_px))
                v.push_back(x);
        }
        right.push_back(Row{Span{"SWP", kColRam, row_px}, std::move(v), false});
    }
    if (c.bat || c.power) {
        std::vector<Span> v;
        bool any = false;
        if (c.bat && s.bat_pct >= 0) {
            for (const Span& x : num_unit(&s.bat_pct, "%", row_px, unit_px))
                v.push_back(x);
            any = true;
        }
        if (c.power && s.bat_w > 0.0f) {
            if (any)
                v.push_back(gap(unit_px));
            for (const Span& x : num_unit_f(s.bat_w, "W", row_px, unit_px))
                v.push_back(x);
            any = true;
        }
        if (any)
            right.push_back(Row{Span{"BAT", kColBat, row_px}, std::move(v), false});
    }
    if (c.fps) {
        std::vector<Span> v;
        for (const Span& x : num_unit_f(s.fps, "FPS", row_px, unit_px))
            v.push_back(x);
        v.push_back(gap(unit_px));
        for (const Span& x : num_unit_f(1000.0f / std::max(s.fps, 1.0f), "ms", row_px, unit_px))
            v.push_back(x);
        right.push_back(Row{Span{api_label(s, c), kColFps, row_px}, std::move(v), false});
        right.push_back(Row{Span{"AVG", kColLo, row_px}, num_unit_f(s.fps_avg, "FPS", row_px, unit_px), false});
        right.push_back(Row{Span{"1%", kColLo, row_px}, num_unit_f(s.low1, "FPS", row_px, unit_px), false});
        right.push_back(Row{Span{"0.1%", kColLo, row_px}, num_unit_f(s.low01, "FPS", row_px, unit_px), false});
        if (c.low001)
            right.push_back(Row{Span{"0.01%", kColLo, row_px}, num_unit_f(s.low001, "FPS", row_px, unit_px), false});
    }

    if (!model_txt.empty()) {
        float lw = f.M.measure("GPU", row_px);
        float vw = 0.0f;
        for (const Row& r : left) {
            lw = std::max(lw, f.M.measure(r.label.text, row_px));
            vw = std::max(vw, f.M.run_w(r.vals));
        }
        const std::vector<std::string> ml = wrap_name_to(f.M, model_txt, row_px, vw + pad);
        std::vector<Row> mrows;
        mrows.push_back(Row{Span{"GPU", kColGpu, row_px}, {Span{ml[0], kColValue, row_px}}, false});
        for (size_t i = 1; i < ml.size(); ++i)
            mrows.push_back(Row{Span{"", kColValue, row_px}, {Span{ml[i], kColValue, row_px}}, false});
        left.insert(left.begin(), mrows.begin(), mrows.end());
    }

    float left_bottom = pad, left_right = pad;
    layout_column(f, left, pad, pad, row_px, line_gap, lv_gap, left_bottom, left_right);
    float right_bottom = pad, right_right = pad;
    const float right_x = left.empty() ? pad : left_right + gutter;
    layout_column(f, right, right_x, pad, row_px, line_gap, lv_gap, right_bottom, right_right);

    float y = std::max(left_bottom, right_bottom);
    float max_right = std::max(left_right, right_right);

    // 底栏：分辨率 · Proton · elapsed
    std::vector<std::vector<Span>> band;
    if (c.res) {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        char rb[64];
        std::snprintf(rb, sizeof(rb), "%dx%d", static_cast<int>(ds.x), static_cast<int>(ds.y));
        band.push_back({Span{"RES ", kColDim, band_px}, Span{rb, kColValue, band_px}});
    }
    if (c.wine && !s.engine_ver.empty())
        band.push_back({Span{s.engine_ver, kColVram, band_px}});
    if (c.session)
        band.push_back({Span{"elapsed ", kColDim, band_px}, Span{session_string(), kColValue, band_px}});

    if (!band.empty()) {
        y += f.M.gsp(4.0f);
        const float max_w = std::max(max_right - pad, f.M.sp(180.0f));
        const float h = f.M.line_h(band_px);
        const float asc = f.M.ascent(band_px);
        const float sep_w = f.M.measure(" · ", band_px);
        float cx = pad;
        bool first = true;
        for (const std::vector<Span>& frag : band) {
            const float w = f.M.run_w(frag);
            if (!first && (cx + sep_w + w - pad) > max_w) {
                y += h + line_gap;
                cx = pad;
                first = true;
            }
            if (!first) {
                f.place(cx, y + asc, std::vector<Span>{Span{" · ", kColDim, band_px}});
                cx += sep_w;
            }
            const float end = f.place(cx, y + asc, frag);
            cx = end;
            max_right = std::max(max_right, end);
            first = false;
        }
        y += h;
    }

    if (c.graph) {
        y += f.M.gsp(3.0f);
        const float gh = f.M.sp(kGraphHeightSp);
        const float gr = std::max(max_right, pad + f.M.sp(220.0f));
        f.graph = Rect{pad, y, gr, y + gh};
        f.has_graph = true;
        max_right = std::max(max_right, gr);
        y += gh;
    }

    const std::string dx_line = dx_version_labeled(s);
    const bool has_dx = c.engine && !dx_line.empty();
    if (has_dx) {
        y += f.M.gsp(4.0f);
        const float end = f.place(pad, y + f.M.ascent(band_px),
                                  std::vector<Span>{Span{dx_line, kColDim, band_px}});
        max_right = std::max(max_right, end);
        y += f.M.line_h(band_px);
    }
    if (c.wine && !s.driver.empty()) {
        y += has_dx ? f.M.gsp(1.0f) : f.M.gsp(4.0f);
        const float end = f.place(pad, y + f.M.ascent(band_px),
                                  std::vector<Span>{Span{s.driver, kColDim, band_px}});
        max_right = std::max(max_right, end);
        y += f.M.line_h(band_px);
    }

    f.content_w = max_right + pad;
    f.content_h = y + pad;
    add_subtle_clock(f, c.clock, pad);
}

// ============================================================================
// build / draw —— 对外接口
// ============================================================================

struct Options {
    float scale = 1.0f;          // hudScale（复用 params->font_scale）
    float outline = kOutlineDefault;
    ImFont* font = nullptr;
    /** 画面署名文本；留空时用默认署名（见 build()）。
     *  取自 params.custom_text_center，便于用户自定义。 */
    std::string credit;
};

inline void build(Frame& f, const Snapshot& s, const Chips& c, FusionSize size, const Options& o) {
    f.glyphs.clear();
    f.tiles.clear();
    f.has_graph = false;
    f.has_pill = false;
    f.content_w = 0.0f;
    f.content_h = 0.0f;
    f.M.font = o.font;
    f.M.scale = o.scale;
    // 留空时用默认署名，保证 §7(b) 的署名在任何配置下都在画面上
    f.credit = o.credit.empty() ? std::string("FusionHUD by The412Banner") : o.credit;

    switch (size) {
        case FusionSize::FULL:    build_full(f, s, c); break;
        case FusionSize::TILES:   build_tiles(f, s, c); break;
        case FusionSize::PILL:    build_pill(f, s, c); break;
        case FusionSize::MINIMAL: build_minimal(f, s, c); break;
        case FusionSize::MEGA:    build_mega(f, s, c); break;
    }

    // 署名行统一追加（自绘模式不再走表格，custom_text_center 需由这里负责）
    add_credit_line(f, f.M.gsp(10.0f));
}

inline void draw(const Frame& f, const overlay_params& p, ImDrawList* dl, ImVec2 o, const Options& opt) {
    if (!dl || f.content_w <= 0.0f || f.content_h <= 0.0f)
        return;

    const ImVec2 p0(o.x, o.y);
    const ImVec2 p1(o.x + f.content_w, o.y + f.content_h);

    // 背景：纯黑 × bgOpacity（上游 onDraw 的 Color.argb(bgOpacity*255, 0, 0, 0)）
    float bg_a = p.background_alpha;
    bg_a = std::min(std::max(bg_a, 0.0f), 1.0f);
    const uint32_t bg = (static_cast<uint32_t>(std::lround(bg_a * 255.0f)) << 24) | 0x000000u;
    const float radius = f.has_pill ? (f.content_h * kPillRadiusRatio) : f.M.sp(kBgRadiusSp);
    dl->AddRectFilled(p0, p1, to_imcol(bg), radius);

    // 描边：accent，宽 = intensity * sp(3.5)，内缩半宽（与上游一致）
    const float sw = f.M.sp(kOutlineMaxSp * opt.outline);
    if (sw > 0.0f) {
        const float h = sw * 0.5f;
        dl->AddRect(ImVec2(p0.x + h, p0.y + h), ImVec2(p1.x - h, p1.y - h),
                    to_imcol(kColAccent), radius, 0, sw);
    }

    // 磁贴底：白 × clamp(14*bgOpacity, 8, 40)
    if (!f.tiles.empty()) {
        float a = 14.0f * bg_a;
        a = std::min(std::max(a, 8.0f), 40.0f);
        const uint32_t tc = (static_cast<uint32_t>(std::lround(a)) << 24) | 0x00FFFFFFu;
        const float tr = f.M.sp(kTileRadiusSp);
        // 磁贴加一圈极淡的白描边：网格边界更清晰，观感更精致
        const uint32_t tb = (static_cast<uint32_t>(
                                 std::lround(std::min(0.20f, bg_a * 0.22f) * 255.0f))
                             << 24) |
                            0x00FFFFFFu;
        for (const Rect& t : f.tiles) {
            const ImVec2 q0(o.x + t.x0, o.y + t.y0);
            const ImVec2 q1(o.x + t.x1, o.y + t.y1);
            dl->AddRectFilled(q0, q1, to_imcol(tc), tr);
            dl->AddRect(q0, q1, to_imcol(tb), tr, 0, 1.0f);
        }
    }

    // 文本
    for (const Glyph& g : f.glyphs) {
        if (g.text.empty())
            continue;
        dl->AddText(f.M.font, g.px, ImVec2(o.x + g.x, o.y + g.top), to_imcol(g.col),
                    g.text.c_str(), g.text.c_str() + g.text.size());
    }

    // 帧时间折线
    if (f.has_graph) {
        const std::vector<float>& vals = graph_history();
        if (vals.size() >= 2) {
            float peak = 1.0f;
            for (float v : vals)
                if (std::isfinite(v))
                    peak = std::max(peak, v);
            std::vector<ImVec2> pts;
            pts.reserve(vals.size());
            const float step = f.graph.w() / static_cast<float>(vals.size() - 1);
            for (size_t i = 0; i < vals.size(); ++i) {
                float t = std::isfinite(vals[i]) ? (vals[i] / peak) : 0.0f;
                t = std::min(std::max(t, 0.0f), 1.0f);
                pts.push_back(ImVec2(o.x + f.graph.x0 + static_cast<float>(i) * step,
                                     o.y + f.graph.y1 - t * f.graph.h()));
            }
            // 图区底衬：极淡的绿底 + 描边，比裸线更易读
            const ImVec2 g0(o.x + f.graph.x0, o.y + f.graph.y0);
            const ImVec2 g1(o.x + f.graph.x1, o.y + f.graph.y1);
            const float gr_r = f.M.sp(3.0f);
            dl->AddRectFilled(g0, g1, to_imcol(0x14000000u | (kColGraph & 0x00FFFFFFu)), gr_r);
            dl->AddRect(g0, g1, to_imcol(0x1EFFFFFFu), gr_r, 0, 1.0f);
            // 外发光 + 实线：先粗描一层低透明度，再叠细实线
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()),
                            to_imcol(0x46000000u | (kColGraph & 0x00FFFFFFu)), false, f.M.sp(3.4f));
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), to_imcol(kColGraph), false,
                            f.M.sp(1.6f));
        }
    }
}

} // namespace fr
} // namespace fusionhud
