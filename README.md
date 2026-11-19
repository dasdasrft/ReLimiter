# ReLimiter (AMD Radeon Edition / 中文增强版)

[![Build and Release](https://github.com/dasdasrft/ReLimiter/actions/workflows/release.yml/badge.svg)](https://github.com/dasdasrft/ReLimiter/actions/workflows/release.yml)
[![GitHub release](https://img.shields.io/github/v/release/dasdasrft/ReLimiter)](https://github.com/dasdasrft/ReLimiter/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

针对 **AMD Radeon 显卡**（深度适配 RX 9000、RX 7000 及各代 Radeon GPU）与 **FreeSync / VRR 可变刷新率显示器**深度优化的低延迟帧率限制器（ReShade 专用 Addon 插件）。

本项目 Fork 自 [RankFTW/ReLimiter](https://github.com/RankFTW/ReLimiter)，在原版优秀帧同步管线的基础上，移除了对 NVIDIA 专有库（NVAPI / NVML）的依赖，重构为 **AMD 原生专用版本**，并完成了**游戏内 OSD 浮层与设置菜单的全面汉化**，修复了多项帧平滑度与低帧统计缺陷。

---

## 🌟 核心特性与改进

### 1. AMD Radeon 驱动级原生硬件遥测 (ADL2 PMLog)
* **多传感器完整监控**：通过 AMD Display Library 2 (ADL2) 的 `PMLog` 高频低开销接口，实时读取并展示：
  * **GPU 核心温度 (Edge)**、**热点结温 (Hotspot)** 以及 **显存温度 (VRAM)**。
  * **显卡核心运行频率** 与 **显存频率** (MHz)。
  * **GPU 图形引擎实时使用率** (%)。
  * **显卡整卡功耗 (TBP)** (W)。
* **自愈与设备索引锁定**：修复了多虚拟输出端环境下适配器索引覆写导致监控数据丢失的问题，并在后台线程加入自愈重试，进游戏即刻稳定读数。

### 2. 重构平滑度抖动算法 (Smoothness)
* **公认帧间抖动模型**：将平滑度计算从原版粗暴的 `|实际帧时间 - 目标帧时间|` 改为真正的相邻帧生成时间绝对差值 $|FT_t - FT_{t-1}|$。
* **拒绝误报爆红**：在显卡满载（GPU Bound）或稳定输出 60/120 FPS 的平稳画面下，平滑度精准反映为 **0.02ms ～ 0.15ms（绿色极度平滑）**，不再因为显卡跑不满所设上限而错误爆红（如 5.42ms / 10.53ms）。

### 3. 1% Low 与 0.1% Low 离群值强效过滤
* **离群暂停帧剔除**：自动过滤 >100ms（<10 FPS）的非游戏战斗帧（如按下 `Home` 呼出 ReShade 菜单、过场加载切场景、Alt-Tab 切屏等停顿帧）。
* **告别 0 FPS 假象**：彻底消除加载停顿帧污染滑动统计窗口的问题，75 FPS 正常游戏时 1% 低帧稳定反映为 ~70 FPS，58 FPS 时反映为 ~50 FPS，**绝不会再莫名跌落至 0 FPS、1 FPS 或 3 FPS**。

### 4. PQI (帧节奏质量指数) 智能基准适配
* 当显卡性能满载（GPU Bound，实际帧率低于目标限制）时，评分基准自动适配为实际帧率中位数，不再把显卡算力瓶颈粗暴惩罚为卡顿，客观真实反映 VRR 步调平稳度。

### 5. 自适应平滑 (Adaptive Smoothing)
* 实时追踪显卡长尾渲染耗时，根据 P99 / P50 百分位数自动动态微调帧率上限，以极小的平均帧率代价主动消除微小卡顿（Micro-Stutters）。
* 支持双窗口保守阻尼模式（64 帧短窗口快速应对突发掉帧，512 帧长窗口平缓恢复，避免镜头切换时帧率频繁抖动跳变）。

### 6. FreeSync / VRR 智能防撕裂限帧
* 根据显示器实时刷新率，通过公式 $\lfloor Hz - \frac{Hz^2}{3600} \rfloor$ 自动计算防撕裂最佳帧率上限（如 144Hz 设为 138 FPS，240Hz 设为 224 FPS），确保游戏全程落在 G-Sync / FreeSync 硬件刷新窗口内，杜绝画面撕裂并消除 VSync 输入延迟。

### 7. 游戏内 OSD 与设置菜单全中文汉化
* 完整支持中英文无缝切换，实时监控浮层所有参数（FPS、1% 低帧、延迟、PQI 细项、显卡温度/功耗等）全部根据语言设定自动本土化显示。

### 8. 窗口管理与屏幕保护套件
* **防切后台锁定 (Focus Lock)**：阻止游戏在 Alt-Tab 切到其他窗口时检测到焦点丢失，游戏音频与逻辑在后台平稳运行。
* **后台限帧 (Background FPS)**：切到后台时自动限制帧率，降低发热与功耗。
* **副屏纯黑遮罩 (Blackout)**：暗室多显示器环境下，一键将副屏纯黑化，彻底杜绝侧方杂光与干扰。
* **OLED 防烧屏保护 (OLED Care)**：挂机暂离自动黑化屏幕并降频，防止 OLED 面板长时间静态灼伤。

---

## 📥 安装指南

### 方法一：直接下载预编译版本（推荐）

1. 从本项目 [Releases](https://github.com/dasdasrft/ReLimiter/releases) 页面下载最新发布的 `relimiter.addon64`（绝大多数游戏使用 64 位版本）。
2. 将 `relimiter.addon64` 复制到你的游戏主程序（`.exe`）同级目录下（即 ReShade 所在目录）。
3. 启动游戏，按快捷键呼出 ReShade 界面即可看到 **ReLimiter** 设置选项卡。

> [!TIP]
> **关于中文字体显示**：
> 若在游戏内 ReShade 界面中看到中文字符显示为问号 `?`，请在 ReShade 的 **[Settings]** 选项卡中，将 **[Font]** 路径设置为系统自带中文字体：
> ```
> C:\Windows\Fonts\msyh.ttc
> ```
> （微软雅黑）或任何支持中文的 TTF/OTF 字体文件。

---

## 📊 OSD 监控指标对照

| 指标项目 | 英文标识 | 中文标识 | 说明 |
| :--- | :--- | :--- | :--- |
| **实时帧率** | `fps` | **`FPS`** | 实时输出帧率（开启插帧时显示渲染与呈现帧率） |
| **1% 最低帧** | `1% Low` | **`1% 低帧`** | 剔除过场加载后的真实 1% 稳态最低帧 |
| **0.1% 极限低帧** | `0.1% Low` | **`0.1% 低帧`** | 极限低帧统计，反映极端偶发掉帧 |
| **帧生成时间** | `ms` | **`ms`** | 实时帧生成毫秒耗时 |
| **平滑度** | `Smoothness` | **`平滑度`** | 相邻两帧间隔抖动差（绿 < 0.5ms，黄 < 1.5ms，红 ≥ 1.5ms） |
| **节奏质量** | `PQI` | **`PQI 评分`** | 0-100% 综合得分（包含节奏稳态、卡顿抑制、准时达成率） |
| **显卡温度** | `GPU Temp` | **`显卡温度`** | 核心温度、热点结温 (Hotspot) 与显存温度 (VRAM) |
| **显卡频率** | `GPU Clock` | **`显卡频率`** | 核心频率与显存频率 (MHz) |
| **显卡占用** | `GPU Load` | **`显卡占用`** | 图形核心实时负载百分比 |
| **整卡功耗** | `GPU Power` | **`显卡功耗`** | 整卡实时功耗 (W) |
| **显存占用** | `VRAM` | **`显存占用`** | 当前显存使用量与总量 (MB) |
| **CPU / 内存** | `CPU Load` / `RAM` | **`CPU 占用`** / **`内存占用`** | 整体处理器与系统内存使用情况 |

---

## 🛠️ 支持的图形 API

| 图形 API | 同步生效方式 | 备注 |
| :--- | :--- | :--- |
| **DirectX 12** | Presentation Gate / Reflex 标记 | 完美支持全部特性与自适应平滑 |
| **DirectX 11** | Present 钩子 / 伪装独占全屏 | 支持 Flip Model 改写，适配高刷 FreeSync |
| **Vulkan** | Presentation Gate (Vulkan) | 深度适配 Vulkan 帧步调 |
| **OpenGL** | Present 钩子 | 支持强制垂直同步改写 |

---

## 🔨 从源码编译

要求环境：
* **Visual Studio 2022** (含 C++ 桌面开发工作负载及 Windows 10/11 SDK)
* **CMake 3.20+**

```powershell
# 克隆仓库（包含子模块）
git clone --recursive https://github.com/dasdasrft/ReLimiter.git
cd ReLimiter

# 构建 64 位版本
cmake -B build -A x64
cmake --build build --config Release

# 构建 32 位版本 (可选)
cmake -B build32 -A Win32
cmake --build build32 --config Release
```

编译输出产物位于：
* 64 位：`build/bin/Release/relimiter.addon64`
* 32 位：`build32/bin/Release/relimiter.addon32`

---

## 📜 鸣谢与开源许可

* 本项目基于 [RankFTW/ReLimiter](https://github.com/RankFTW/ReLimiter) 进行重构与二次开发，感谢原作者 [RankFTW](https://github.com/RankFTW) 卓越的帧步调设计。
* 基于 [MIT License](LICENSE) 许可协议开源。
