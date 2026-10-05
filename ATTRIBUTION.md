# Attribution / 致谢

本仓库 **MangoHudfex** 包含并移植了第三方作品。以下署名依各自许可条款保留。

---

## 1. FusionHUD —— 外观规范来源

本仓库的 **FusionHUD 外观移植**（调色板、面板几何、五档尺寸）源自：

> **FusionHUD** — https://github.com/The412Banner/FusionHUD
> Copyright (C) **The412Banner**
> 许可：**GNU GPL v3.0**，并附带 **GPL-3.0 §7(b) 附加署名条款**

FusionHUD 的 §7(b) 附加条款要求：任何使用、fork 或分发，都必须在**项目文档**与
**应用内致谢/关于界面**中保留对 `The412Banner` 的署名，以及指向
https://github.com/The412Banner/FusionHUD 的链接。

### 本仓库如何满足

| 要求 | 落点 |
|---|---|
| 项目文档中的署名与链接 | 本文件，以及 `README.md` 的「致谢」章节 |
| 应用内致谢/关于界面 | `mangohud --credits`、`mangohud --version`（命令行关于界面）；运行时 HUD 上的 `FusionHUD by The412Banner` 署名行 |
| 源码级出处标注 | 下列文件头部均带有 GPL-3.0 与出处声明 |

### 派生文件清单

以下文件在其头部标注了 FusionHUD 出处，其中的数值逐行对照
`fusionhud/src/main/java/com/winlator/star/widget/fusionhud/FusionHudView.kt` 与
`FusionHudModels.kt` 得出：

| 文件 | 说明 |
|---|---|
| `src/fusion_theme.hpp` | **FusionHUD 视觉规范的唯一事实源**：调色板、几何常量、五档尺寸、配置项映射 |
| `src/fusion_layout.hpp` | 五档布局预设（Full / Tiles / Pill / Minimal / Mega） |
| `src/fusion_appearance.hpp` | ImGui 样式与绘制辅助（HSV 混合、面板背景、描边、磁贴、帧时间曲线） |
| `src/fusion_metrics.hpp` | 跨厂商（Adreno KGSL / Mali / PowerVR / Xclipse）GPU 指标探测 |

### 与上游的差异说明

本移植是**视觉规范层面的对齐**，而非逐像素重写：

- 色彩体系、面板几何（纯黑底 × alpha 0.8、圆角 `sp(8f)`、accent 描边 `intensity*sp(3.5)`）、
  五档字号间距 —— **精确对齐上游**。
- 五档的逐像素排版未在 ImGui 中重写（MangoHud 是表格渲染器），改用布局与间距选项近似。
- 上游的**点击切档 / 拖动 / 长按锁定**手势未移植：MangoHud 是 Vulkan 层，没有对应的输入通道；
  切档改用配置项 `preset=10..14`。

---

## 2. MangoHud —— 本项目基于其上

> **MangoHud** — https://github.com/flightlessmango/MangoHud
> Copyright (C) flightlessmango
> 许可：**MIT**

本仓库是 MangoHud 的 fork（经 `moze30/MangoHud`），FEX-Emu 统计集成
（`src/fex.cpp`）与 Termux glibc 构建脚本由本仓库维护。

---

## 3. FEX-Emu —— 统计共享内存接口

`src/fex.cpp` 读取 FEX-Emu 暴露的运行时统计数据，结构定义与 shm 命名约定对照：

> **FEX-Emu** — https://github.com/FEX-Emu/FEX
> 相关文件：`FEXCore/include/FEXCore/Utils/SHMStats.h`、
> `Source/Windows/UnixLib/FEXUnixLib.cpp`
> 许可：**MIT**

---

## 4. 许可证兼容性

| 组件 | 许可 | 与本仓库 |
|---|---|---|
| FusionHUD | GPL-3.0 (+§7(b)) | 本仓库整体以 **GPL-3.0** 分发 |
| MangoHud | MIT | 兼容 |
| FEX-Emu | MIT | 兼容 |
| imgui / spdlog / Vulkan-Headers 等 | MIT / Apache-2.0 / 见各自 | 兼容 |

> ⚠️ 注意：FusionHUD 是 GPL-3.0，因此**整个 MangoHudfex 仓库需以 GPL-3.0 分发**。
> 上游 MangoHud 的 MIT 代码并入 GPL 作品是允许的（MIT 与 GPL 兼容）。

---

*如发现署名有遗漏，请提 issue 或直接补 PR。*