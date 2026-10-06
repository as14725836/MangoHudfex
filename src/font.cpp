#include <cstdint>
#include <cstdlib>
#include "overlay.h"
#include "file_utils.h"
#include "font_default.h"
#include "IconsForkAwesome.h"
#include "forkawesome.h"
#include "fusion_theme.hpp"

/**
 * 解析 FusionHUD 专用字体文件。
 * 顺序：用户显式设置的 font_file → MANGOHUD_FUSION_FONT → 随包安装的
 * DejaVu Sans Mono Bold → 桌面发行版 DejaVu → Android 系统等宽字体。
 * 全都找不到时返回空串，由调用方回退到内嵌字体。
 */
#include "hud_i18n.hpp"
#include <cstdio>   // is_loadable_ttf 用 FILE/fread
#include <cstring>   // strchr/strstr
#include <cctype>    // tolower
#include <vector>
#include <dirent.h>  // opendir/readdir

// stb_truetype 只支持 TrueType 轮廓：文件头 0x00010000（或 'true'）。
// OTF/CFF（'OTTO'）、字体集合 TTC（'ttcf'）、WOFF 等喂给 ImGui 会让
// stbtt_InitFont 解析失败并直接断言 —— 表现就是 HUD 黑屏。这里先按文件头筛掉。
static bool is_loadable_ttf(const std::string& path) {
   if (path.empty())
      return false;

   FILE* f = fopen(path.c_str(), "rb");
   if (!f)
      return false;

   unsigned char h[4] = { 0, 0, 0, 0 };
   const size_t got = fread(h, 1, 4, f);
   fclose(f);
   if (got != 4)
      return false;

   const unsigned int tag = ((unsigned int)h[0] << 24) | ((unsigned int)h[1] << 16) |
                            ((unsigned int)h[2] << 8) | (unsigned int)h[3];
   return tag == 0x00010000u || tag == 0x74727565u;
}

// 从 fontconfig 配置里取出 <dir>...</dir> 指定的字体目录。
// Termux glibc 的配置在 /data/data/com.termux/files/usr/glibc/etc/fonts/fonts.conf。
static void fontconfig_dirs(const std::string& conf, std::vector<std::string>& out) {
   FILE* f = fopen(conf.c_str(), "r");
   if (!f)
      return;

   std::string text;
   {
      char buf[4096];
      size_t n = 0;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
         text.append(buf, n);
   }
   fclose(f);

   size_t pos = 0;
   while ((pos = text.find("<dir>", pos)) != std::string::npos) {
      pos += 5;
      const size_t end = text.find("</dir>", pos);
      if (end == std::string::npos)
         break;

      std::string d = text.substr(pos, end - pos);
      while (!d.empty() && (d.front() == ' ' || d.front() == '\t' || d.front() == '\n'))
         d.erase(d.begin());
      while (!d.empty() && (d.back() == ' ' || d.back() == '\t' || d.back() == '\n' || d.back() == '\r'))
         d.pop_back();

      if (!d.empty() && d[0] == '/')
         out.push_back(d);
      pos = end + 6;
   }
}

// 扫描目录里第一个可用的 TrueType 字体。
// 优先带 CJK / SC / Han / Noto / WQY / Micro 字样的（多半含汉字）；
// .ttc / .otf 一律跳过 —— stb_truetype 解析不了 CFF/字体集合。
static std::string first_usable_font(const std::string& dir) {
   DIR* d = opendir(dir.c_str());
   if (!d)
      return {};

   std::string first, preferred;
   while (auto* e = readdir(d)) {
      std::string name = e->d_name;
      if (name.size() < 5 || name[0] == '.')
         continue;

      std::string low = name;
      for (char& c : low)
         c = (char)tolower((unsigned char)c);
      if (low.size() < 4 || low.compare(low.size() - 4, 4, ".ttf") != 0)
         continue;

      const std::string path = dir + "/" + name;
      if (!is_loadable_ttf(path))
         continue;

      if (first.empty())
         first = path;

      // 注意：不要用 "sc" —— CarroisGothicSC 之类的 SC 是 Small Caps，没有汉字。
      if (low.find("cjk") != std::string::npos || low.find("han") != std::string::npos ||
          low.find("wqy") != std::string::npos || low.find("micro") != std::string::npos ||
          low.find("hei") != std::string::npos || low.find("ming") != std::string::npos ||
          low.find("fallback") != std::string::npos) {
         preferred = path;
         break;
      }
   }
   closedir(d);
   return preferred.empty() ? first : preferred;
}

// libMangoHud.so 自己所在目录：用来定位随包字体，不受安装前缀影响。
// 读 /proc/self/maps 而不是 dladdr()，避免额外链接依赖。
static std::string libmangohud_dir() {
   FILE* f = fopen("/proc/self/maps", "r");
   if (!f)
      return {};

   char line[1024];
   std::string hit;
   while (fgets(line, sizeof(line), f)) {
      char* p = strchr(line, '/');
      if (!p)
         continue;
      size_t len = strlen(p);
      while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' '))
         p[--len] = '\0';
      if (strstr(p, "libMangoHud.so")) {
         hit = p;
         break;
      }
   }
   fclose(f);

   const auto slash = hit.find_last_of('/');
   if (hit.empty() || slash == std::string::npos)
      return {};
   return hit.substr(0, slash);
}

static std::string fusion_font_path(const overlay_params& params) {
   if (is_loadable_ttf(params.font_file))
      return params.font_file;   // 用户在配置里指定了字体，尊重其选择

   if (const char* env = std::getenv("MANGOHUD_FUSION_FONT")) {
      if (*env && is_loadable_ttf(env))
         return env;
   }

   // 随包中文子集：固定位置先试一遍（不依赖 fontconfig / 安装前缀）
   static const char* kCjkFirst[] = {
      "/data/data/com.termux/files/usr/glibc/share/fonts/MangoHud-CJK.ttf",
      "/data/data/com.termux/files/usr/glibc/share/mangohud/fonts/MangoHud-CJK.ttf",
      "/data/data/com.termux/files/usr/share/fonts/MangoHud-CJK.ttf",
      "/usr/share/mangohud/fonts/MangoHud-CJK.ttf",
      "/usr/local/share/mangohud/fonts/MangoHud-CJK.ttf",
   };
   for (const char* p : kCjkFirst)
      if (is_loadable_ttf(p))
         return p;

   // 1) 再扫常用字体目录（Termux glibc 的 share/fonts 放最前）
   static const char* kFontDirs[] = {
      "/data/data/com.termux/files/usr/glibc/share/fonts",
      "/data/data/com.termux/files/usr/glibc/share/mangohud/fonts",
      "/data/data/com.termux/files/usr/share/fonts",
      "/data/data/com.termux/files/usr/share/mangohud/fonts",
      "/usr/share/mangohud/fonts",
      "/usr/local/share/mangohud/fonts",
      "/usr/share/fonts",
      "/usr/local/share/fonts",
   };
   for (const char* dir : kFontDirs)
      if (std::string p = first_usable_font(dir); !p.empty())
         return p;

   // fontconfig 的 fonts.conf（把 <dir> 列出的目录也扫一遍） 的 fonts.conf 走（它写明了系统字体到底在哪）
   static const char* kFontConf[] = {
      "/data/data/com.termux/files/usr/glibc/etc/fonts/fonts.conf",
      "/data/data/com.termux/files/usr/etc/fonts/fonts.conf",
      "/etc/fonts/fonts.conf",
   };
   std::vector<std::string> fc_dirs;
   for (const char* conf : kFontConf)
      fontconfig_dirs(conf, fc_dirs);
   for (const std::string& dir : fc_dirs)
      if (std::string p = first_usable_font(dir); !p.empty())
         return p;


   // 2) 再按 libMangoHud.so 的位置找随包字体（<prefix>/lib/mangohud -> <prefix>/share/mangohud/fonts）
   if (const std::string lib_dir = libmangohud_dir(); !lib_dir.empty()) {
      static const char* kRel[] = {
         "../../share/mangohud/fonts/",   // lib/mangohud/ 安装（本包/多数发行版）
         "../share/mangohud/fonts/",      // lib/ 直接安装
         "../lib/mangohud/fonts/",        // 字体与库放一起
         "/",                             // 字体就在库旁边
      };
      static const char* kNames[] = {
         "MangoHud-CJK.ttf",              // 随包中文子集（优先）
         "DejaVuSansMono-Bold.ttf",
      };
      for (const char* rel : kRel)
         for (const char* name : kNames) {
            std::string p = lib_dir + "/" + rel + name;
            if (is_loadable_ttf(p))
               return p;
         }
   }

   static const char* kCandidates[] = {
      // 随包的中文子集：界面标签是中文，DejaVu / Roboto 都没有汉字
      "/data/data/com.termux/files/usr/glibc/share/mangohud/fonts/MangoHud-CJK.ttf",
      "/data/data/com.termux/files/usr/glibc/usr/share/mangohud/fonts/MangoHud-CJK.ttf",
      "/data/data/com.termux/files/usr/share/mangohud/fonts/MangoHud-CJK.ttf",
      "/usr/share/mangohud/fonts/MangoHud-CJK.ttf",
      "/usr/local/share/mangohud/fonts/MangoHud-CJK.ttf",
      // 安卓自带中文字体优先：界面标签是中文，需要汉字覆盖。
      // 注意：只能放 TrueType（.ttf）——Noto 的 .otf/.ttc 是 CFF/集合，
      // stb_truetype 解析会失败并让 HUD 黑屏（已被 is_loadable_ttf 兜住）。
      "/system/fonts/NotoSansCJK-Regular.ttf",
      "/system/fonts/NotoSansSC-Regular.ttf",
      "/system/fonts/DroidSansFallbackFull.ttf",
      "/system/fonts/DroidSansFallback.ttf",
      // 随包安装（termux-glibc 扁平布局 / usr 布局 / 通用前缀）
      "/data/data/com.termux/files/usr/glibc/share/mangohud/fonts/DejaVuSansMono-Bold.ttf",
      "/data/data/com.termux/files/usr/glibc/usr/share/mangohud/fonts/DejaVuSansMono-Bold.ttf",
      "/data/data/com.termux/files/usr/share/mangohud/fonts/DejaVuSansMono-Bold.ttf",
      "/usr/share/mangohud/fonts/DejaVuSansMono-Bold.ttf",
      "/usr/local/share/mangohud/fonts/DejaVuSansMono-Bold.ttf",
      // 桌面发行版常见的 DejaVu
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
      // Android 系统等宽字体
      "/system/fonts/DroidSansMono.ttf",
      "/system/fonts/RobotoMono-Regular.ttf",
      "/system/fonts/NotoSansMono-Regular.ttf",
   };
   for (const char* p : kCandidates) {
      if (is_loadable_ttf(p))
         return p;
   }
   return std::string();
}

void create_fonts(ImFontAtlas* font_atlas, const overlay_params& params, ImFont*& small_font, ImFont*& text_font, ImFont*& secondary_font, FusionFonts& fusion)
{
   auto& io = ImGui::GetIO();
   if (!font_atlas)
        font_atlas = io.Fonts;
   font_atlas->Clear();

   ImGui::GetIO().FontGlobalScale = params.font_scale; // set here too so ImGui::CalcTextSize is correct
   float font_size = params.font_size;
   if (font_size < FLT_EPSILON)
      font_size = 24;

   float font_size_text = params.font_size_text;
   if (font_size_text < FLT_EPSILON)
      font_size_text = font_size;

   float font_size_secondary = params.font_size_secondary;
   if (font_size_secondary > font_size || font_size_secondary < FLT_EPSILON)
      font_size_secondary = font_size;

   [[maybe_unused]] static const ImWchar default_range[] =
   {
      0x0020, 0x00FF, // Basic Latin + Latin Supplement
      0x2018, 0x201F, // Bunch of quotation marks
      //0x0100, 0x017F, // Latin Extended-A
      //0x2103, 0x2103, // Degree Celsius
      //0x2109, 0x2109, // Degree Fahrenheit
      0,
   };
   // Load Icon file and merge to exisitng font
    ImFontConfig config;
    config.MergeMode = true;
    // ImGui changed OversampleH default to 2, but it appears to sometimes cause
    // crashing issues in 32bit applications.
    config.OversampleH = 1;
    config.OversampleV = 1;
    config.PixelSnapH = true;
    static const ImWchar icon_ranges[] = { ICON_MIN_FK, ICON_MAX_FK, 0 };

   ImVector<ImWchar> glyph_ranges;
   ImFontGlyphRangesBuilder builder;
   builder.AddRanges(font_atlas->GetGlyphRangesDefault());
   // 中文化标签用到的汉字：按需烘焙，图集只多几十个字形
   {   // 中文化：手写清单 + 翻译表里的所有汉字（自动汇总，防漏字）
      const std::string zh = hud_i18n::zh_all_glyphs();
      builder.AddText(zh.c_str());
   }
   if (params.font_glyph_ranges & FG_KOREAN)
      builder.AddRanges(font_atlas->GetGlyphRangesKorean());
   if (params.font_glyph_ranges & FG_CHINESE_FULL)
      builder.AddRanges(font_atlas->GetGlyphRangesChineseFull());
   if (params.font_glyph_ranges & FG_CHINESE_SIMPLIFIED)
      builder.AddRanges(font_atlas->GetGlyphRangesChineseSimplifiedCommon());
   if (params.font_glyph_ranges & FG_JAPANESE)
      builder.AddRanges(font_atlas->GetGlyphRangesJapanese()); // Not exactly Shift JIS compatible?
   if (params.font_glyph_ranges & FG_CYRILLIC)
      builder.AddRanges(font_atlas->GetGlyphRangesCyrillic());
   if (params.font_glyph_ranges & FG_THAI)
      builder.AddRanges(font_atlas->GetGlyphRangesThai());
   if (params.font_glyph_ranges & FG_VIETNAMESE)
      builder.AddRanges(font_atlas->GetGlyphRangesVietnamese());
   if (params.font_glyph_ranges & FG_LATIN_EXT_A) {
      constexpr ImWchar latin_ext_a[] { 0x0100, 0x017F, 0 };
      builder.AddRanges(latin_ext_a);
   }
   if (params.font_glyph_ranges & FG_LATIN_EXT_B) {
      constexpr ImWchar latin_ext_b[] { 0x0180, 0x024F, 0 };
      builder.AddRanges(latin_ext_b);
   }
   builder.BuildRanges(&glyph_ranges);

   bool same_font = (params.font_file == params.font_file_text || params.font_file_text.empty());
   bool text_same_size = (font_size == font_size_text);
   bool secondary_same_size = (font_size == font_size_secondary);

   // ---- FusionHUD 专用字体 ----
   // ImGui 把字体按固定尺寸烘焙进图集：放大绘制 = 位图放大（发虚），
   // 缩小绘制 = 无 mipmap 的降采样（变软）。FusionHUD 的字号跨度很大
   // （约 14~51px），所以按三档分别烘焙，绘制时挑"不小于目标字号"的那一档，
   // 做到全程只缩不放 —— 既清晰又保留大号 FPS 的冲击力。
   //
   // 字体优先用随包安装的 DejaVu Sans Mono Bold（现代等宽粗体，观感接近
   // FusionHUD 原版的 Monospace Bold）；找不到再退回内嵌字体。
   // 刻意**不**跟随 font_glyph_ranges：在 51px 下加 CJK 会让图集体积爆炸，
   // 而 FusionHUD 只会画 ASCII + ° · — ↓。
   // FHUD 的 glyph ranges：Latin-1 + 符号 + 中文化标签用到的汉字。
   // 汉字按需添加（只有几十个字形），所以 51px 下也不会把图集撑爆。
   ImVector<ImWchar> fusion_ranges_vec;
   {
      static const ImWchar fusion_base[] = {
         0x0020, 0x00FF,   // Latin-1（含 ° · ² 等）
         0x2013, 0x2014,   // – —
         0x2190, 0x2193,   // ← ↑ → ↓
         0,
      };
      ImFontGlyphRangesBuilder fusion_builder;
      fusion_builder.AddRanges(fusion_base);
      const std::string zh_f = hud_i18n::zh_all_glyphs();
      fusion_builder.AddText(zh_f.c_str());
      fusion_builder.BuildRanges(&fusion_ranges_vec);
   }
   const ImWchar* fusion_ranges = fusion_ranges_vec.Data;

   ImFontConfig fusion_config;
   fusion_config.OversampleH = 2;
   fusion_config.OversampleV = 1;
   fusion_config.PixelSnapH = true;   // 字形前进取整，位置更锐利

   const float fusion_scale = params.font_scale > 0.0f ? params.font_scale : 1.0f;
   auto bake_size = [&](float sp_value) {
      float px = sp_value * fusionhud::kSpToPx * fusion_scale;
      if (px < font_size)
         px = font_size;
      if (px > 96.0f)
         px = 96.0f;
      return px;
   };
   const float size_big = bake_size(fusionhud::kMaxTextSp);
   const float size_mid = bake_size(fusionhud::kMidTextSp);
   const float size_small = bake_size(fusionhud::kSmallTextSp);

   const std::string fusion_ttf = fusion_font_path(params);
   if (!fusion_ttf.empty()) {
      // 默认静默；排查字体时用 MANGOHUD_FONT_DEBUG=1 打开
      const char* fdbg = std::getenv("MANGOHUD_FONT_DEBUG");
      if (fdbg && *fdbg && std::string(fdbg) != "0")
         fprintf(stderr, "[MangoHud] HUD font: %s\n", fusion_ttf.c_str());
   }
   auto bake_fusion = [&](float px) -> ImFont* {
      // 每档单独一份配置：字距按 em 比例给（排版更透气），横向过采样提到 3（更锐利）
      ImFontConfig cfg = fusion_config;
      cfg.OversampleH = 3;
      cfg.GlyphExtraSpacing.x = px * fusionhud::kTrackingEm;
      if (!fusion_ttf.empty())
         return font_atlas->AddFontFromFileTTF(fusion_ttf.c_str(), px, &cfg, fusion_ranges);
      return font_atlas->AddFontFromMemoryCompressedBase85TTF(
         GetDefaultCompressedFontDataTTFBase85(), px, &cfg, fusion_ranges);
   };

   // ImGui takes ownership of the data, no need to free it
   if (is_loadable_ttf(params.font_file)) {
      font_atlas->AddFontFromFileTTF(params.font_file.c_str(), font_size, nullptr, glyph_ranges.Data);
      font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size, &config, icon_ranges);
      fusion.small = bake_fusion(size_small);
      fusion.mid = bake_fusion(size_mid);
      fusion.big = bake_fusion(size_big);
      if (params.no_small_font)
         small_font = font_atlas->Fonts[0];
      else {
         small_font = font_atlas->AddFontFromFileTTF(params.font_file.c_str(), font_size * 0.55f, nullptr, glyph_ranges.Data);
         font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size * 0.55f, &config, icon_ranges);
      }
      if (secondary_same_size) {
         secondary_font = font_atlas->Fonts[0];
      } else {
         secondary_font = font_atlas->AddFontFromFileTTF(params.font_file.c_str(), font_size_secondary, nullptr, glyph_ranges.Data);
         font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size_secondary, &config, icon_ranges);
      }
   } else if (!fusion_ttf.empty()) {
      // 没设 font_file：用 fusion_font_path() 选到的字体（随包中文子集 / 系统字体）。
      // 以前这里直接退回内嵌英文字体，中文标签就全变 "?" 了。
      font_atlas->AddFontFromFileTTF(fusion_ttf.c_str(), font_size, nullptr,
                                     glyph_ranges.Data);
      font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size, &config, icon_ranges);
      fusion.small = bake_fusion(size_small);
      fusion.mid = bake_fusion(size_mid);
      fusion.big = bake_fusion(size_big);
      if (params.no_small_font)
         small_font = font_atlas->Fonts[0];
      else {
         small_font = font_atlas->AddFontFromFileTTF(fusion_ttf.c_str(), font_size * 0.55f, nullptr, glyph_ranges.Data);
         font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size * 0.55f, &config, icon_ranges);
      }
      if (secondary_same_size) {
         secondary_font = font_atlas->Fonts[0];
      } else {
         secondary_font = font_atlas->AddFontFromFileTTF(fusion_ttf.c_str(), font_size_secondary, nullptr, glyph_ranges.Data);
         font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size_secondary, &config, icon_ranges);
      }
   } else {
      const char* ttf_compressed_base85 = GetDefaultCompressedFontDataTTFBase85();
      font_atlas->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size, nullptr, glyph_ranges.Data);
      font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size, &config, icon_ranges);
      fusion.small = bake_fusion(size_small);
      fusion.mid = bake_fusion(size_mid);
      fusion.big = bake_fusion(size_big);
      if (params.no_small_font)
         small_font = font_atlas->Fonts[0];
      else {
         small_font = font_atlas->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size * 0.55f, nullptr, glyph_ranges.Data);
         font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size * 0.55f, &config, icon_ranges);
      }
      if (secondary_same_size) {
         secondary_font = font_atlas->Fonts[0];
      } else {
         secondary_font = font_atlas->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size_secondary, nullptr, glyph_ranges.Data);
         font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size_secondary, &config, icon_ranges);
      }
   }

   auto font_file_text = params.font_file_text;
   if (font_file_text.empty())
      font_file_text = params.font_file;

   if ((!same_font || !text_same_size) && is_loadable_ttf(font_file_text))
      text_font = font_atlas->AddFontFromFileTTF(font_file_text.c_str(), font_size_text, nullptr, glyph_ranges.Data);
   else
      text_font = font_atlas->Fonts[0];

   font_atlas->Build();
}
