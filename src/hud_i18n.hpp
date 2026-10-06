/**
 * hud_i18n.hpp — HUD 界面标签的中文化（标准 HUD + FusionHUD 共用）
 *
 * 用法：
 *   hud_i18n::tr("GPU")  →  "显卡"      （表里没有的原样返回）
 *
 * 默认就是中文；想回英文设 MANGOHUD_LANG=en（或任意以 en 开头）。
 *
 * 字体：中文字形必须存在于字体图集里，zh_glyph_text() 把用到的汉字集中返回，
 * 由 font.cpp 精确烘焙（只加这几十个字，图集不会变大）。
 */
#pragma once

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace hud_i18n {

/** 界面上会用到的全部汉字（字体图集按需烘焙，别漏） */
inline const char* zh_glyph_text() {
    return
        "显卡处理器内存电池帧率时间平均温度风扇功耗延迟分辨率类型刷新程序名长剩余降频未知开关全屏垂直同步游戏模式呈现锐利图表应用核心读写帧数接口引擎同步方式"
        "占用无连接网络显存时钟上限当前扇转速续航";
}

/** 翻译表里出现过的所有汉字（自动汇总，避免加词忘了补字形 → 显示成 "?"） */
inline std::string zh_table_glyphs() {
    std::string out;
    for (const auto& kv : table())
        out += kv.second;
    return out;
}

/** 需要烘焙到字体图集里的全部字符 = 手写清单 + 翻译表 */
inline std::string zh_all_glyphs() {
    return std::string(zh_glyph_text()) + zh_table_glyphs();
}

inline bool chinese_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("MANGOHUD_LANG");
        if (!v || !*v)
            return true;                       // 默认中文
        return std::strncmp(v, "en", 2) != 0;  // MANGOHUD_LANG=en 回英文
    }();
    return enabled;
}

inline const std::unordered_map<std::string, const char*>& table() {
    static const std::unordered_map<std::string, const char*> t = {
        // ---- 核心指标 ----
        {"GPU", "显卡"},
        {"CPU", "处理器"},
        {"VRAM", "显存"},
        {"RAM", "内存"},
        {"BAT", "电池"},
        {"Battery", "电池"},
        {"FPS", "帧率"},
        {"Frametime", "帧时间"},
        {"AVG", "平均"},
        {"Frame Count", "帧数"},
        {"GPU Load", "显卡占用"},
        {"GPU Temp", "显卡温度"},
        {"GPU Core Clock", "显卡频率"},
        {"GPU Mem Clock", "显存频率"},
        {"CPU Load", "处理器占用"},
        {"CPU Temp", "处理器温度"},
        {"CPU Core Clock", "处理器频率"},
        {"RAM Temp", "内存温度"},
        {"Mem Clock", "内存频率"},
        {"Temp", "温度"},
        {"FAN", "风扇"},
        {"Fan", "风扇"},
        {"Fan Speed", "风扇转速"},
        {"RPM", "转速"},
        {"Power", "功耗"},
        {"Latency", "延迟"},
        // ---- 信息行 ----
        {"Resolution", "分辨率"},
        {"Display Hz", "刷新率"},
        {"Type", "类型"},
        {"Exe name", "程序名"},
        {"Duration", "时长"},
        {"Remaining Time", "剩余时间"},
        {"Battery Time", "电池续航"},
        {"Time", "时间"},
        {"App", "应用"},
        {"Present Mode", "呈现模式"},
        {"Engine", "引擎"},
        {"API", "接口"},
        {"Network", "网络"},
        {"Connect", "连接"},
        // ---- 状态词 ----
        {"Unknown", "未知"},
        {"ON", "开"},
        {"OFF", "关"},
        {"Full", "全屏"},
        {"Sharp", "锐利"},
        {"VSYNC", "垂直同步"},
        {"GAMEMODE", "游戏模式"},
        {"Throttling", "降频"},
        {"Power throttling", "功耗降频"},
        {"Thermal throttling", "温度降频"},
        {"Read", "读取"},
        {"Write", "写入"},
        {"Read/Write", "读写"},
        {"IO RD", "读取"},
        {"IO WR", "写入"},
        {"IO RW", "读写"},
        {"My Plot", "图表"},
        {"FEX JIT Load", "FEX JIT 占用"},
        {"FEX JIT top loaded threads", "FEX JIT 热点线程"},
        {"Winesync", "同步方式"},
    };
    return t;
}

/** 英文标签 → 中文；不在表里 / 关闭中文时原样返回 */
inline const char* tr(const char* s) {
    if (!s || !*s || !chinese_enabled())
        return s;
    const auto& t = table();
    auto it = t.find(s);
    return it == t.end() ? s : it->second;
}

inline std::string tr(const std::string& s) {
    const char* r = tr(s.c_str());
    return r == s.c_str() ? s : std::string(r);
}

} // namespace hud_i18n