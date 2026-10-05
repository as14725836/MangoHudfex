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
static std::string fusion_font_path(const overlay_params& params) {
   if (!params.font_file.empty() && file_exists(params.font_file))
      return params.font_file;   // 用户在配置里指定了字体，尊重其选择

   if (const char* env = std::getenv("MANGOHUD_FUSION_FONT")) {
      if (*env && file_exists(env))
         return env;
   }

   static const char* kCandidates[] = {
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
      if (file_exists(p))
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

   static const ImWchar default_range[] =
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
   static const ImWchar fusion_ranges[] = {
      0x0020, 0x00FF,   // Latin-1（含 ° · ² 等）
      0x2013, 0x2014,   // – —
      0x2190, 0x2193,   // ← ↑ → ↓
      0,
   };

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
   auto bake_fusion = [&](float px) -> ImFont* {
      if (!fusion_ttf.empty())
         return font_atlas->AddFontFromFileTTF(fusion_ttf.c_str(), px, &fusion_config, fusion_ranges);
      return font_atlas->AddFontFromMemoryCompressedBase85TTF(
         GetDefaultCompressedFontDataTTFBase85(), px, &fusion_config, fusion_ranges);
   };

   // ImGui takes ownership of the data, no need to free it
   if (!params.font_file.empty() && file_exists(params.font_file)) {
      font_atlas->AddFontFromFileTTF(params.font_file.c_str(), font_size, nullptr, same_font && text_same_size ? glyph_ranges.Data : default_range);
      font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size, &config, icon_ranges);
      fusion.small = bake_fusion(size_small);
      fusion.mid = bake_fusion(size_mid);
      fusion.big = bake_fusion(size_big);
      if (params.no_small_font)
         small_font = font_atlas->Fonts[0];
      else {
         small_font = font_atlas->AddFontFromFileTTF(params.font_file.c_str(), font_size * 0.55f, nullptr, default_range);
         font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size * 0.55f, &config, icon_ranges);
      }
      if (secondary_same_size) {
         secondary_font = font_atlas->Fonts[0];
      } else {
         secondary_font = font_atlas->AddFontFromFileTTF(params.font_file.c_str(), font_size_secondary, nullptr, default_range);
         font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size_secondary, &config, icon_ranges);
      }
   } else {
      const char* ttf_compressed_base85 = GetDefaultCompressedFontDataTTFBase85();
      font_atlas->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size, nullptr, default_range);
      font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size, &config, icon_ranges);
      fusion.small = bake_fusion(size_small);
      fusion.mid = bake_fusion(size_mid);
      fusion.big = bake_fusion(size_big);
      if (params.no_small_font)
         small_font = font_atlas->Fonts[0];
      else {
         small_font = font_atlas->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size * 0.55f, nullptr, default_range);
         font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size * 0.55f, &config, icon_ranges);
      }
      if (secondary_same_size) {
         secondary_font = font_atlas->Fonts[0];
      } else {
         secondary_font = font_atlas->AddFontFromMemoryCompressedBase85TTF(ttf_compressed_base85, font_size_secondary, nullptr, default_range);
         font_atlas->AddFontFromMemoryCompressedBase85TTF(forkawesome_compressed_data_base85, font_size_secondary, &config, icon_ranges);
      }
   }

   auto font_file_text = params.font_file_text;
   if (font_file_text.empty())
      font_file_text = params.font_file;

   if ((!same_font || !text_same_size) && file_exists(font_file_text))
      text_font = font_atlas->AddFontFromFileTTF(font_file_text.c_str(), font_size_text, nullptr, glyph_ranges.Data);
   else
      text_font = font_atlas->Fonts[0];

   font_atlas->Build();
}
