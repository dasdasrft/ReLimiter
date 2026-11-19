#include "osd.h"
#include "config.h"
#include "scheduler.h"
#include "predictor.h"
#include "fg_divisor.h"
#include "display_state.h"
#include "marker_log.h"
#include "correlator.h"
#include "stress_detector.h"
#include "wake_guard.h"
#include "pqi.h"
#include "csv_writer.h"
#include "baseline.h"
#include "swapchain_manager.h"
#include "enforcement_dispatcher.h"
#include "loadlib_hooks.h"
#include "vsync_control.h"
#include "health.h"
#include "presentation_gate.h"
#include "flip_model.h"
#include "adaptive_smoothing.h"
#include "gpu_timer.h"
#include "bottleneck.h"
#include "vk_enforce.h"
#include "hw_monitor.h"
#include "feedback.h"
#include "blackout.h"
#include "focus_lock.h"
#include "localization.h"
#include "logger.h"
#include <string>
#include <atomic>
#include <algorithm>

// ── OSD readout state ──
static double s_real_fps = 0.0;
static double s_display_fps = 0.0;
static double s_display_render_fps = 0.0;
static double s_display_output_fps = 0.0;
static ULONGLONG s_fps_display_tick = 0;

// ── Frametime history for graph ──
static constexpr int FT_HISTORY_SIZE = 200;
static float s_ft_history[FT_HISTORY_SIZE] = {};
static int   s_ft_history_idx = 0;

// ── Adaptive graph scale ──
static float s_graph_scale_min = 0.0f;
static float s_graph_scale_max = 33.0f;

// Tooltip helper
static void HelpTip(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(300.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ── Monitor enumeration for display selector ──
struct MonitorEntry {
    HMONITOR hmon;
    RECT     rc;
    char     label[64];
};
static constexpr int MAX_MONITORS = 8;
static MonitorEntry s_monitors[MAX_MONITORS];
static int s_monitor_count = 0;
static int s_selected_monitor = 0;

static BOOL CALLBACK MonitorEnumProc(HMONITOR hmon, HDC, LPRECT, LPARAM) {
    if (s_monitor_count >= MAX_MONITORS) return FALSE;
    MONITORINFOEXA mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoA(hmon, &mi);
    auto& e = s_monitors[s_monitor_count];
    e.hmon = hmon;
    e.rc = mi.rcMonitor;
    int w = mi.rcMonitor.right - mi.rcMonitor.left;
    int h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    snprintf(e.label, sizeof(e.label), "%d: %s (%dx%d)",
             s_monitor_count + 1, mi.szDevice, w, h);
    s_monitor_count++;
    return TRUE;
}

static void RefreshMonitorList() {
    s_monitor_count = 0;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
}

static void MoveWindowToMonitor(int idx) {
    HWND hwnd = SwapMgr_GetHWND();
    if (!hwnd || idx < 0 || idx >= s_monitor_count) return;
    struct MoveData { HWND hwnd; RECT rc; };
    auto* data = new MoveData{ hwnd, s_monitors[idx].rc };
    CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
        auto* d = static_cast<MoveData*>(param);
        RECT& rc = d->rc;
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        SetWindowPos(d->hwnd, nullptr, rc.left, rc.top, w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(100);
        QueryVRRCeiling();
        delete d;
        return 0;
    }, data, 0, nullptr);
}

// ── Sticky monitor: re-apply after alt-tab if window drifted ──
static void EnforceStickyMonitor() {
    if (s_selected_monitor == 0) return;
    int idx = s_selected_monitor - 1;
    if (idx < 0 || idx >= s_monitor_count) return;

    HWND hwnd = SwapMgr_GetHWND();
    if (!hwnd) return;

    // Throttle to once per second
    static ULONGLONG s_last_check = 0;
    ULONGLONG now = GetTickCount64();
    if (now - s_last_check < 1000) return;
    s_last_check = now;

    HMONITOR current = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (current == s_monitors[idx].hmon) return;

    LOG_INFO("Sticky monitor: window drifted, re-applying monitor %d", idx + 1);
    MoveWindowToMonitor(idx);
}

void DrawSettings(reshade::api::effect_runtime* /*rt*/) {
    // Dirty flag — set by any config change, triggers SaveConfig at end of frame
    bool config_dirty = false;

    // Lazy install focus lock if enabled in config but not yet installed,
    // or re-install if the swapchain HWND has changed (DX11 launcher → game window).
    if (g_config.focus_lock) {
        HWND hwnd = SwapMgr_GetHWND();
        static HWND s_focus_lock_hwnd = nullptr;
        if (hwnd && hwnd != s_focus_lock_hwnd) {
            if (FocusLock_IsActive()) FocusLock_Remove();
            FocusLock_Install(hwnd);
            s_focus_lock_hwnd = hwnd;
        }
    }

    // ════════════════════════════════════════════
    // LANGUAGE SWITCHER / 语言切换
    // ════════════════════════════════════════════
    {
        ImGui::Text("%s", TR("Language / 语言:", "界面语言 (Language):"));
        ImGui::SameLine();
        bool is_zh = IsChinese();
        if (ImGui::RadioButton("简体中文", is_zh)) {
            if (!is_zh) {
                g_config.language = "zh-CN";
                config_dirty = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("English", !is_zh)) {
            if (is_zh) {
                g_config.language = "en-US";
                config_dirty = true;
            }
        }
        if (is_zh) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                "提示: 若中文字符显示为问号，请在 ReShade 的 [Settings] 选项卡中将 [Font] 设置为 C:\\Windows\\Fonts\\msyh.ttc (微软雅黑)");
        }
        ImGui::Separator();
    }

    // ════════════════════════════════════════════
    // SECTION 1: FPS (AMD Radeon Edition)
    // ════════════════════════════════════════════
    ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "ReLimiter (AMD Radeon Edition / AMD 专用版)");
    ImGui::Spacing();

    static int s_target_edit = g_user_target_fps.load(std::memory_order_relaxed);
    static bool s_target_active = false;
    if (!s_target_active)
        s_target_edit = g_user_target_fps.load(std::memory_order_relaxed);
    int target = s_target_edit;

    // Compute FreeSync / VRR cap: fps = hz - hz^2/3600, floored
    double ceiling_hz = g_ceiling_hz.load(std::memory_order_relaxed);
    int freesync_cap = 0;
    if (ceiling_hz > 1.0)
        freesync_cap = static_cast<int>(ceiling_hz - (ceiling_hz * ceiling_hz / 3600.0));

    if (ImGui::RadioButton(TR("FreeSync Cap", "FreeSync/VRR 自动限帧"), freesync_cap > 0 && target == freesync_cap)) {
        if (freesync_cap > 0) {
            g_user_target_fps.store(freesync_cap, std::memory_order_relaxed);
            g_config.target_fps = freesync_cap;
            s_target_edit = freesync_cap;
            config_dirty = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(TR("Custom", "自定义"), target > 0 && target != freesync_cap)) {
        if (target == 0 || target == freesync_cap) {
            target = 60;
            g_user_target_fps.store(target, std::memory_order_relaxed);
            g_config.target_fps = target;
            s_target_edit = target;
            config_dirty = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(TR("Off", "关闭"), target == 0)) {
        g_user_target_fps.store(0, std::memory_order_relaxed);
        g_config.target_fps = 0;
        s_target_edit = 0;
        config_dirty = true;
    }
    if (freesync_cap > 0) {
        char tip[256];
        snprintf(tip, sizeof(tip),
                 TR("FreeSync Cap sets your FPS to the safe maximum for your display (hz - hz^2/3600). For your monitor that's %d fps.",
                    "FreeSync/VRR 自动限帧：根据显示器刷新率自动计算防撕裂上限 (刷新率 - 刷新率^2/3600)。当前显示器推荐值为 %d fps。"), freesync_cap);
        HelpTip(tip);
    } else {
        HelpTip(TR("FreeSync Cap sets your FPS to the safe maximum for your display. Display not detected yet.",
                    "FreeSync/VRR 自动限帧：根据显示器刷新率自动计算防撕裂上限。尚未检测到显示器。"));
    }

    char slider_fmt[32];
    if (s_target_edit == 0)
        snprintf(slider_fmt, sizeof(slider_fmt), "%s", TR("Off", "关闭"));
    else if (s_target_edit == freesync_cap && freesync_cap > 0)
        snprintf(slider_fmt, sizeof(slider_fmt), "%s", TR("%d (FreeSync)", "%d (FreeSync 限帧)"));
    else
        snprintf(slider_fmt, sizeof(slider_fmt), "%%d");

    ImGui::SliderInt(TR("Target FPS", "目标帧率"), &s_target_edit, 0, 360, slider_fmt);
    s_target_active = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (s_target_edit > 0 && s_target_edit < 30) s_target_edit = 30;
        g_user_target_fps.store(s_target_edit, std::memory_order_relaxed);
        g_config.target_fps = s_target_edit;
        config_dirty = true;
    }
    HelpTip(TR("The FPS the limiter will target. 0 = no limiting. Minimum is 30.",
               "帧率限制器的目标 FPS。0 为不限帧，最低限制为 30。"));

    for (int p : {30, 60, 120, 240}) {
        ImGui::SameLine();
        char label[8];
        snprintf(label, sizeof(label), "%d", p);
        if (ImGui::Button(label)) {
            g_user_target_fps.store(p, std::memory_order_relaxed);
            g_config.target_fps = p;
            s_target_edit = p;
            config_dirty = true;
        }
    }

    ImGui::Spacing();
    static int s_bg_edit = g_background_fps.load(std::memory_order_relaxed);
    static bool s_bg_active = false;
    if (!s_bg_active)
        s_bg_edit = g_background_fps.load(std::memory_order_relaxed);
    char bg_fmt[32];
    if (s_bg_edit == 0)
        snprintf(bg_fmt, sizeof(bg_fmt), "%s", TR("Uncapped", "不限"));
    else
        snprintf(bg_fmt, sizeof(bg_fmt), "%%d");
    ImGui::SliderInt(TR("Background FPS", "后台帧率限制"), &s_bg_edit, 0, 60, bg_fmt);
    s_bg_active = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (s_bg_edit > 0 && s_bg_edit < 20) s_bg_edit = 20;
        g_background_fps.store(s_bg_edit, std::memory_order_relaxed);
        g_config.background_fps = s_bg_edit;
        config_dirty = true;
    }
    HelpTip(TR("FPS cap when the game window loses focus. 0 = uncapped. Minimum is 20.",
               "当游戏窗口失去焦点（切到后台）时的帧率限制。0 为不限帧，最低为 20。"));

    ImGui::Spacing();
    {
        const char* vsync_labels_en[] = {"Game", "Off", "On"};
        const char* vsync_labels_zh[] = {"跟随游戏", "强制关闭", "强制开启"};
        const char** vsync_labels = IsChinese() ? vsync_labels_zh : vsync_labels_en;
        int current_vsync = 0;
        if (g_config.vsync_mode == "off") current_vsync = 1;
        else if (g_config.vsync_mode == "on") current_vsync = 2;

        if (ImGui::BeginCombo(TR("VSync", "垂直同步"), vsync_labels[current_vsync])) {
            for (int i = 0; i < 3; i++) {
                bool selected = (current_vsync == i);
                if (ImGui::Selectable(vsync_labels[i], selected)) {
                    if (i != current_vsync) {
                        const char* mode_str[] = {"game", "off", "on"};
                        g_config.vsync_mode = mode_str[i];
                        config_dirty = true;
                        // Apply immediately for OpenGL
                        VSync_ApplyOpenGL();
                    }
                }
            }
            ImGui::EndCombo();
        }
        HelpTip(TR("Game = use the game's VSync setting. Off = force VSync off (recommended with frame limiter). On = force VSync on.",
                   "跟随游戏 = 使用游戏内设置；强制关闭 = 强行关闭垂直同步（推荐搭配限帧器使用）；强制开启 = 强行开启垂直同步。"));
    }

    // ════════════════════════════════════════════
    // SECTION 2: OSD (collapsible)
    // ════════════════════════════════════════════
    ImGui::Separator();
    if (ImGui::CollapsingHeader(TR("OSD", "OSD 监控面板"))) {
        if (ImGui::Checkbox(TR("Show OSD", "启用 OSD 显示"), &g_config.osd_enabled)) config_dirty = true;
        HelpTip(TR("Toggle the in-game overlay on or off.", "开启或关闭游戏内 OSD 监控面板。"));
        float osd_x_pct = g_config.osd_x * 100.0f;
        float osd_y_pct = g_config.osd_y * 100.0f;
        if (ImGui::SliderFloat(TR("OSD X", "水平位置 X"), &osd_x_pct, 0.0f, 100.0f, "%.1f%%"))
            g_config.osd_x = osd_x_pct / 100.0f;
        if (ImGui::IsItemDeactivatedAfterEdit()) config_dirty = true;
        HelpTip(TR("Horizontal position of the overlay as a percentage of screen width.", "OSD 面板在屏幕上的横向位置百分比。"));
        if (ImGui::SliderFloat(TR("OSD Y", "垂直位置 Y"), &osd_y_pct, 0.0f, 100.0f, "%.1f%%"))
            g_config.osd_y = osd_y_pct / 100.0f;
        if (ImGui::IsItemDeactivatedAfterEdit()) config_dirty = true;
        HelpTip(TR("Vertical position of the overlay as a percentage of screen height.", "OSD 面板在屏幕上的纵向位置百分比。"));
        ImGui::SliderFloat(TR("OSD Opacity", "背景不透明度"), &g_config.osd_opacity, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) config_dirty = true;
        HelpTip(TR("Background transparency of the overlay. 0 = fully transparent, 1 = fully opaque.", "OSD 监控面板的背景透明度。0 为完全透明，1 为纯色背景。"));

        // ── Appearance ──
        ImGui::Spacing();
        ImGui::Text("%s", TR("Appearance", "外观设置"));

        // OSD Style selector
        const char* style_labels_en[] = { "Modern Card", "Horizontal Bar (Steam Deck)", "Classic List" };
        const char* style_labels_zh[] = { "现代卡片式 (Modern Card)", "极简顶栏式 (Steam Deck)", "经典列表式 (Classic List)" };
        const char** style_labels = IsChinese() ? style_labels_zh : style_labels_en;
        int current_style = 0;
        if (g_config.osd_style == "bar") current_style = 1;
        else if (g_config.osd_style == "classic") current_style = 2;

        if (ImGui::BeginCombo(TR("OSD Style", "OSD 风格"), style_labels[current_style])) {
            for (int i = 0; i < 3; i++) {
                bool selected = (current_style == i);
                if (ImGui::Selectable(style_labels[i], selected)) {
                    const char* styles[] = { "card", "bar", "classic" };
                    g_config.osd_style = styles[i];
                    config_dirty = true;
                }
            }
            ImGui::EndCombo();
        }
        HelpTip(TR("Modern Card: sleek grouped badges and cards. Horizontal Bar: compact Steam Deck style top bar. Classic List: traditional vertical text.",
                   "现代卡片式：圆角悬浮徽标卡片，按硬件模块紧凑聚合；极简顶栏式：类似 Steam Deck 的单行/双行横向悬浮条；经典列表式：传统垂直逐行文本。"));

        if (g_config.osd_style != "classic") {
            if (ImGui::SliderFloat(TR("Card Rounding", "圆角弧度"), &g_config.osd_rounding, 0.0f, 20.0f, "%.0f px"))
                config_dirty = true;
            HelpTip(TR("Corner rounding for modern cards and pill badges.", "现代卡片与徽标的圆角平滑度。"));
        }

        float scale_pct = g_config.osd_scale * 100.0f;
        if (ImGui::SliderFloat(TR("OSD Scale", "缩放比例"), &scale_pct, 50.0f, 200.0f, "%.0f%%"))
            g_config.osd_scale = scale_pct / 100.0f;
        if (ImGui::IsItemDeactivatedAfterEdit()) config_dirty = true;
        HelpTip(TR("Scale the entire OSD overlay. 100% is default size.", "整体缩放 OSD 监控面板。100% 为默认标准大小。"));
        if (ImGui::Checkbox(TR("Drop Shadow", "文字阴影"), &g_config.osd_drop_shadow)) config_dirty = true;
        HelpTip(TR("Draw a shadow behind OSD text for better readability.", "在 OSD 文字后方绘制阴影轮廓，增强各种游戏背景下的可读性。"));
        ImGui::SliderFloat(TR("Text Brightness", "文字亮度"), &g_config.osd_text_brightness, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) config_dirty = true;
        HelpTip(TR("Brightness of the OSD text. 1.0 = full white, 0.0 = black.", "OSD 文字亮度。1.0 为纯白，0.0 为纯黑。"));

        if (g_config.osd_show_frametime_graph) {
            if (ImGui::Checkbox(TR("Target Guideline", "帧时间目标参考线"), &g_config.osd_show_target_line))
                config_dirty = true;
            HelpTip(TR("Draw a cyan reference line at the target frame time interval in the graph.", "在帧时间曲线图中绘制当前目标帧率对应的参考基准线，方便快速识别卡顿尖峰。"));
        }

        // ── Presets ──
        ImGui::Spacing();
        ImGui::Text("%s", TR("Presets", "快捷预设"));

        // Built-in presets
        if (ImGui::Button(TR("Min", "极简"))) {
            // Minimal: FPS + Frametime + GPU Temp
            OSDPreset p = {};
            p.show_fps = true;
            p.show_frametime = true;
            p.show_gpu_temp = true;
            OSDPreset_ApplyTogglesOnly(p);
            config_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Med", "均衡"))) {
            // Medium: core diagnostics
            OSDPreset p = {};
            p.show_fps = true;
            p.show_1pct_low = true;
            p.show_frametime = true;
            p.show_frametime_graph = true;
            p.show_gpu_render_time = true;
            p.show_pqi = true;
            p.show_smoothness = true;
            p.show_fg = true;
            p.show_gpu_temp = true;
            p.show_gpu_usage = true;
            p.show_vram = true;
            OSDPreset_ApplyTogglesOnly(p);
            config_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Full", "全量"))) {
            // Full: everything on (AMD Edition)
            OSDPreset p = {};
            p.show_fps = true;
            p.show_1pct_low = true;
            p.show_0_1pct_low = true;
            p.show_frametime = true;
            p.show_frametime_graph = true;
            p.show_cpu_latency = true;
            p.show_gpu_render_time = true;
            p.show_total_frame_cost = true;
            p.show_pqi = true;
            p.show_pqi_breakdown = true;
            p.show_smoothness = true;
            p.show_fg = true;
            p.show_limiter = true;
            p.show_adaptive_smoothing = true;
            p.show_gpu_temp = true;
            p.show_gpu_clock = true;
            p.show_gpu_usage = true;
            p.show_gpu_power = true;
            p.show_vram = true;
            p.show_cpu_usage = true;
            p.show_ram = true;
            p.show_api = true;
            p.show_resolution = true;
            p.show_flip_model = true;
            OSDPreset_ApplyTogglesOnly(p);
            config_dirty = true;
        }

        // User preset slots (left-click = load, right-click = delete)
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        int preset_count = OSDPreset_GetCount();
        for (int i = 0; i < preset_count; i++) {
            ImGui::SameLine();
            OSDPreset& slot = OSDPreset_GetSlot(i);
            char label[40];
            if (slot.occupied)
                snprintf(label, sizeof(label), "%s##upreset%d", slot.name, i);
            else
                snprintf(label, sizeof(label), "---##upreset%d", i);
            if (ImGui::Button(label)) {
                if (slot.occupied) {
                    OSDPreset_ApplyToConfig(slot);
                    config_dirty = true;
                }
            }
            // Right-click context menu for delete
            if (slot.occupied) {
                char ctx_id[32];
                snprintf(ctx_id, sizeof(ctx_id), "##preset_ctx_%d", i);
                if (ImGui::BeginPopupContextItem(ctx_id)) {
                    char del_label[48];
                    snprintf(del_label, sizeof(del_label), TR("Delete \"%s\"", "删除 \"%s\""), slot.name);
                    if (ImGui::Selectable(del_label)) {
                        OSDPreset_DeleteSlot(i);
                        config_dirty = true;
                    }
                    ImGui::EndPopup();
                }
            }
        }

        // Show [+] button only when all initial slots are occupied
        {
            bool all_initial_used = true;
            int count = OSDPreset_GetCount();
            int check_up_to = (count < OSD_INITIAL_PRESET_SLOTS) ? count : OSD_INITIAL_PRESET_SLOTS;
            for (int i = 0; i < check_up_to; i++) {
                if (!OSDPreset_GetSlot(i).occupied) {
                    all_initial_used = false;
                    break;
                }
            }
            if (all_initial_used && count < OSD_MAX_PRESET_SLOTS) {
                ImGui::SameLine();
                if (ImGui::Button("+##add_preset")) {
                    OSDPreset_AddSlot();
                }
            }
        }

        // Save current config to a user slot
        {
            static int s_save_slot = 0;
            static char s_save_name[32] = {};
            static bool s_save_popup = false;

            ImGui::SameLine();
            if (ImGui::Button(TR("Save", "保存"))) {
                s_save_popup = true;
                s_save_slot = 0;
                snprintf(s_save_name, sizeof(s_save_name), "Preset %d",
                         s_save_slot + 1);
            }
            HelpTip(TR("Save the current OSD element selection to a user preset slot.", "将当前选中的 OSD 监控项目保存到自定义预设槽位。"));

            if (s_save_popup) {
                ImGui::OpenPopup("Save OSD Preset");
                s_save_popup = false;
            }
            if (ImGui::BeginPopup("Save OSD Preset")) {
                ImGui::Text("%s", TR("Save current OSD layout:", "保存当前 OSD 布局配置:"));
                ImGui::InputText(TR("Name", "预设名称"), s_save_name, sizeof(s_save_name));
                // Slot selector — dynamic count
                int count = OSDPreset_GetCount();
                if (s_save_slot >= count) s_save_slot = count - 1;
                if (s_save_slot < 0) s_save_slot = 0;
                if (ImGui::BeginCombo(TR("Slot", "槽位"), s_save_slot < count ?
                        (OSDPreset_GetSlot(s_save_slot).occupied ?
                         OSDPreset_GetSlot(s_save_slot).name : "---") : "---")) {
                    for (int i = 0; i < count; i++) {
                        char slot_label[48];
                        OSDPreset& s = OSDPreset_GetSlot(i);
                        if (s.occupied)
                            snprintf(slot_label, sizeof(slot_label), TR("Slot %d: %s", "槽位 %d: %s"), i + 1, s.name);
                        else
                            snprintf(slot_label, sizeof(slot_label), TR("Slot %d: ---", "槽位 %d: 空"), i + 1);
                        if (ImGui::Selectable(slot_label, s_save_slot == i))
                            s_save_slot = i;
                    }
                    ImGui::EndCombo();
                }
                ImGui::Spacing();
                if (ImGui::Button(TR("Save##confirm", "保存##confirm"), ImVec2(80, 0))) {
                    OSDPreset& slot = OSDPreset_GetSlot(s_save_slot);
                    slot = OSDPreset_FromConfig();
                    snprintf(slot.name, sizeof(slot.name), "%s", s_save_name);
                    slot.occupied = true;
                    OSDPreset_SaveSlot(s_save_slot);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button(TR("Cancel", "取消"), ImVec2(80, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // ── Default preset (shared mode only) ──
        // Applied automatically at game launch for every game.
        if (g_config.shared_presets) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            if (OSDPreset_HasDefault()) {
                char def_label[48];
                snprintf(def_label, sizeof(def_label), TR("Default: %s", "默认: %s"), OSDPreset_GetDefault().name);
                ImGui::TextDisabled("%s", def_label);
                ImGui::SameLine();
                if (ImGui::Button(TR("Update##default_preset", "更新##default_preset"))) {
                    OSDPreset p = OSDPreset_FromConfig();
                    snprintf(p.name, sizeof(p.name), "%s", OSDPreset_GetDefault().name);
                    OSDPreset_SaveDefault(p);
                }
                HelpTip(TR("Update the default preset with the current OSD layout. Applied automatically on launch for every game.", "将当前 OSD 布局更新为默认预设。所有游戏启动时将自动应用。"));
                ImGui::SameLine();
                if (ImGui::Button(TR("Clear##default_preset", "清除##default_preset"))) {
                    OSDPreset_ClearDefault();
                }
                HelpTip(TR("Remove the default preset. Games will use their own saved OSD layout.", "移除默认预设。各游戏将使用各自保存的独立 OSD 布局。"));
            } else {
                if (ImGui::Button(TR("Set as Default##default_preset", "设为默认##default_preset"))) {
                    OSDPreset p = OSDPreset_FromConfig();
                    snprintf(p.name, sizeof(p.name), "Default");
                    OSDPreset_SaveDefault(p);
                }
                HelpTip(TR("Save the current OSD layout as the default. It will be applied automatically on launch for every game using the shared presets file.", "将当前 OSD 布局设为全局默认预设。"));
            }
        }

        // ── Preset cycling keybinds ──
        {
            // Built-in preset definitions for cycling
            // Index: 0=Min, 1=Med, 2=Full, then 3..N = user presets (occupied only)
            auto ApplyPresetByIndex = [&](int idx) {
                if (idx == 0) {
                    OSDPreset p = {};
                    p.show_fps = true; p.show_frametime = true; p.show_gpu_temp = true;
                    OSDPreset_ApplyTogglesOnly(p);
                } else if (idx == 1) {
                    OSDPreset p = {};
                    p.show_fps = true; p.show_1pct_low = true; p.show_frametime = true;
                    p.show_frametime_graph = true; p.show_gpu_render_time = true;
                    p.show_pqi = true; p.show_smoothness = true; p.show_fg = true;
                    p.show_gpu_temp = true; p.show_gpu_usage = true; p.show_vram = true;
                    OSDPreset_ApplyTogglesOnly(p);
                } else if (idx == 2) {
                    OSDPreset p = {};
                    p.show_fps = true; p.show_1pct_low = true; p.show_0_1pct_low = true;
                    p.show_frametime = true; p.show_frametime_graph = true;
                    p.show_cpu_latency = true; p.show_gpu_render_time = true;
                    p.show_total_frame_cost = true;
                    p.show_pqi = true; p.show_pqi_breakdown = true;
                    p.show_smoothness = true; p.show_fg = true; p.show_limiter = true;
                    p.show_adaptive_smoothing = true; p.show_gpu_temp = true;
                    p.show_gpu_clock = true; p.show_gpu_usage = true; p.show_gpu_power = true; p.show_vram = true;
                    p.show_cpu_usage = true; p.show_cpu_clock = true; p.show_cpu_power = true; p.show_ram = true;
                    OSDPreset_ApplyTogglesOnly(p);
                } else {
                    // User preset (idx - 3)
                    int user_idx = idx - 3;
                    if (user_idx >= 0 && user_idx < OSDPreset_GetCount()) {
                        OSDPreset& slot = OSDPreset_GetSlot(user_idx);
                        if (slot.occupied)
                            OSDPreset_ApplyToConfig(slot);
                    }
                }
                config_dirty = true;
            };

            // Count total presets: 3 built-in + occupied user presets
            static int s_current_preset = 0;
            int total_presets = 3;
            for (int i = 0; i < OSDPreset_GetCount(); i++)
                if (OSDPreset_GetSlot(i).occupied) total_presets++;

            // Keybind UI
            ImGui::Spacing();

            // Prev Preset keybind
            {
                static bool s_capturing = false;
                ImGui::Text("%s", TR("Prev Preset:", "上一预设热键:"));
                ImGui::SameLine();
                if (s_capturing) {
                    ImGui::TextColored(ImVec4(1,1,0.3f,1), "%s", TR("Press key...", "请按键..."));
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Cancel##prev", "取消##prev"))) s_capturing = false;
                    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                        ImGuiKey key = static_cast<ImGuiKey>(k);
                        if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl) continue;
                        if (key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift) continue;
                        if (key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt) continue;
                        if (key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper) continue;
                        if (k == ImGuiMod_Ctrl || k == ImGuiMod_Shift || k == ImGuiMod_Alt || k == ImGuiMod_Super) continue;
                        if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) continue;
                        const char* kn = ImGui::GetKeyName(key);
                        if (kn && kn[0] == 'M' && kn[1] == 'o' && kn[2] == 'd') continue;
                        if (ImGui::IsKeyPressed(key, false)) {
                            std::string name;
                            if (ImGui::IsKeyDown(ImGuiMod_Ctrl)) name += "Ctrl+";
                            if (ImGui::IsKeyDown(ImGuiMod_Alt)) name += "Alt+";
                            if (ImGui::IsKeyDown(ImGuiMod_Shift)) name += "Shift+";
                            name += ImGui::GetKeyName(key);
                            g_config.osd_preset_prev_key = name;
                            s_capturing = false;
                            config_dirty = true;
                            break;
                        }
                    }
                } else {
                    ImGui::Text("%s", g_config.osd_preset_prev_key.empty() ? TR("None", "无") : g_config.osd_preset_prev_key.c_str());
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Bind##prev", "绑定##prev"))) s_capturing = true;
                    if (!g_config.osd_preset_prev_key.empty()) {
                        ImGui::SameLine();
                        if (ImGui::Button(TR("Clear##prev", "清除##prev"))) {
                            g_config.osd_preset_prev_key.clear();
                            config_dirty = true;
                        }
                    }
                }
            }

            // Next Preset keybind
            {
                static bool s_capturing = false;
                ImGui::Text("%s", TR("Next Preset:", "下一预设热键:"));
                ImGui::SameLine();
                if (s_capturing) {
                    ImGui::TextColored(ImVec4(1,1,0.3f,1), "%s", TR("Press key...", "请按键..."));
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Cancel##next", "取消##next"))) s_capturing = false;
                    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                        ImGuiKey key = static_cast<ImGuiKey>(k);
                        if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl) continue;
                        if (key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift) continue;
                        if (key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt) continue;
                        if (key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper) continue;
                        if (k == ImGuiMod_Ctrl || k == ImGuiMod_Shift || k == ImGuiMod_Alt || k == ImGuiMod_Super) continue;
                        if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) continue;
                        const char* kn = ImGui::GetKeyName(key);
                        if (kn && kn[0] == 'M' && kn[1] == 'o' && kn[2] == 'd') continue;
                        if (ImGui::IsKeyPressed(key, false)) {
                            std::string name;
                            if (ImGui::IsKeyDown(ImGuiMod_Ctrl)) name += "Ctrl+";
                            if (ImGui::IsKeyDown(ImGuiMod_Alt)) name += "Alt+";
                            if (ImGui::IsKeyDown(ImGuiMod_Shift)) name += "Shift+";
                            name += ImGui::GetKeyName(key);
                            g_config.osd_preset_next_key = name;
                            s_capturing = false;
                            config_dirty = true;
                            break;
                        }
                    }
                } else {
                    ImGui::Text("%s", g_config.osd_preset_next_key.empty() ? TR("None", "无") : g_config.osd_preset_next_key.c_str());
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Bind##next", "绑定##next"))) s_capturing = true;
                    if (!g_config.osd_preset_next_key.empty()) {
                        ImGui::SameLine();
                        if (ImGui::Button(TR("Clear##next", "清除##next"))) {
                            g_config.osd_preset_next_key.clear();
                            config_dirty = true;
                        }
                    }
                }
            }

            // Move OSD (corner cycling) keybind + button
            {
                static bool s_capturing = false;
                ImGui::Text("%s", TR("Move OSD:", "轮换位置:"));
                ImGui::SameLine();
                if (s_capturing) {
                    ImGui::TextColored(ImVec4(1,1,0.3f,1), "%s", TR("Press key...", "请按键..."));
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Cancel##pos", "取消##pos"))) s_capturing = false;
                    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                        ImGuiKey key = static_cast<ImGuiKey>(k);
                        if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl) continue;
                        if (key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift) continue;
                        if (key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt) continue;
                        if (key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper) continue;
                        if (k == ImGuiMod_Ctrl || k == ImGuiMod_Shift || k == ImGuiMod_Alt || k == ImGuiMod_Super) continue;
                        if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) continue;
                        const char* kn = ImGui::GetKeyName(key);
                        if (kn && kn[0] == 'M' && kn[1] == 'o' && kn[2] == 'd') continue;
                        if (ImGui::IsKeyPressed(key, false)) {
                            std::string name;
                            if (ImGui::IsKeyDown(ImGuiMod_Ctrl)) name += "Ctrl+";
                            if (ImGui::IsKeyDown(ImGuiMod_Alt)) name += "Alt+";
                            if (ImGui::IsKeyDown(ImGuiMod_Shift)) name += "Shift+";
                            name += ImGui::GetKeyName(key);
                            g_config.osd_position_cycle_key = name;
                            s_capturing = false;
                            config_dirty = true;
                            break;
                        }
                    }
                } else {
                    ImGui::Text("%s", g_config.osd_position_cycle_key.empty() ? TR("None", "无") : g_config.osd_position_cycle_key.c_str());
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Bind##pos", "绑定##pos"))) s_capturing = true;
                    if (!g_config.osd_position_cycle_key.empty()) {
                        ImGui::SameLine();
                        if (ImGui::Button(TR("Clear##pos", "清除##pos"))) {
                            g_config.osd_position_cycle_key.clear();
                            config_dirty = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Move##pos_now", "轮换##pos_now"))) {
                        // Cycle through corners: top-right → bottom-right → bottom-left → top-left → top-right
                        OSD_CyclePosition();
                        config_dirty = true;
                    }
                    HelpTip(TR("Cycle the OSD position through each corner of the screen (clockwise).", "顺时针将 OSD 面板轮换到屏幕的四个角落。"));
                }
            }
        }

        // ── Elements by category ──
        // Layout: checkboxes flow horizontally, separated by " - ", wrapping on overflow.
        // HelpTip is attached to each checkbox via its (?) marker.

        // Helper lambda: place next checkbox on same line with dash separator,
        // or wrap to next line if not enough room.
        auto FlowSeparator = [&]() {
            ImGui::SameLine();
            float avail = ImGui::GetContentRegionAvail().x;
            // Estimate: dash ~20px + next checkbox ~120px minimum
            float needed = 140.0f;
            if (avail > needed) {
                ImGui::TextDisabled("-");
                ImGui::SameLine();
            } else {
                ImGui::NewLine();
            }
        };

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.9f, 1.0f), "%s", TR("Performance", "性能指标"));
        if (ImGui::Checkbox(TR("FPS##osd_elem", "帧率 (FPS)##osd_elem"), &g_config.osd_show_fps)) config_dirty = true;
        HelpTip(TR("Show the current FPS. When Frame Generation is active, shows both output and render FPS.", "显示当前实时帧率。当开启插帧时，同时显示显示帧率与真实渲染帧率。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("1%% Low##osd_elem", "1% 低帧##osd_elem"), &g_config.osd_show_1pct_low)) config_dirty = true;
        HelpTip(TR("Show the 1%% low FPS over a rolling window. Uses display FPS when Frame Generation is active.", "显示滑动窗口内的 1% 最低帧率。开启插帧时对应实际呈现帧率。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("0.1%% Low##osd_elem", "0.1% 低帧##osd_elem"), &g_config.osd_show_0_1pct_low)) config_dirty = true;
        HelpTip(TR("Show the 0.1%% low FPS over a rolling window. Requires more samples than 1%% low to stabilize.", "显示滑动窗口内的 0.1% 极限低帧率，比 1% 低帧需要更多采样来稳定。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("Frametime##osd_elem", "帧生成时间##osd_elem"), &g_config.osd_show_frametime)) config_dirty = true;
        HelpTip(TR("Show the current frame time in milliseconds.", "以毫秒 (ms) 为单位显示当前帧生成时间。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("Frametime Graph##osd_elem", "帧时间走势图##osd_elem"), &g_config.osd_show_frametime_graph)) config_dirty = true;
        HelpTip(TR("Show a rolling graph of recent frame times.", "显示近期帧时间的平滑波动曲线走势图。"));

        ImGui::Spacing();
#ifdef _WIN64
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", TR("Latency", "延迟与耗时"));
        if (ImGui::Checkbox(TR("CPU Latency##osd_elem", "CPU 提交延迟##osd_elem"), &g_config.osd_show_cpu_latency)) config_dirty = true;
        HelpTip(TR("CPU pipeline time: SIM_START to RENDERSUBMIT_END. Measures how long the CPU spends on simulation + render submission.", "CPU 管线耗时：从模拟开始到渲染指令提交结束。测量 CPU 在逻辑与提交上的耗时。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("GPU Render##osd_elem", "GPU 渲染耗时##osd_elem"), &g_config.osd_show_gpu_render_time)) config_dirty = true;
        HelpTip(TR("GPU active render time in ms. Shows actual GPU work excluding idle bubbles (AMD Radeon).", "GPU 纯活跃渲染耗时 (毫秒)。展示 GPU 纯渲染负载（AMD Radeon 专用优化）。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("Frame Cost##osd_elem", "整帧总开销##osd_elem"), &g_config.osd_show_total_frame_cost)) config_dirty = true;
        HelpTip(TR("Total frame cost: sim + render + GPU. The real cost of a frame before limiter sleep.", "整帧总耗时：模拟 + 提交 + 渲染。限帧器休眠前的实际帧开销。"));
#endif

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "%s", TR("Quality", "画质与平滑度"));
        if (ImGui::Checkbox(TR("PQI##osd_elem", "节奏质量得分 (PQI)##osd_elem"), &g_config.osd_show_pqi)) config_dirty = true;
        HelpTip(TR("Pacing Quality Index: a 0-100% composite score. Green=great, yellow=ok, red=poor.", "帧节奏质量指数：0-100% 综合得分。绿色=优秀，黄色=良好，红色=较差。"));
        if (g_config.osd_show_pqi) {
            ImGui::SameLine();
            ImGui::TextDisabled("-");
            ImGui::SameLine();
            if (ImGui::Checkbox(TR("Breakdown##osd_elem", "细项得分##osd_elem"), &g_config.osd_show_pqi_breakdown)) config_dirty = true;
            HelpTip(TR("Show individual PQI sub-scores: cadence, stutter, and deadline.", "显示 PQI 细项：节奏 (Cadence)、卡顿 (Stutter)、截止时间达成率 (Deadline)。"));
        }
        FlowSeparator();
        if (ImGui::Checkbox(TR("Smoothness##osd_elem", "抖动偏差 (平滑度)##osd_elem"), &g_config.osd_show_smoothness)) config_dirty = true;
        HelpTip(TR("Frame interval deviation from target in milliseconds. Lower = smoother. Green < 0.5ms, yellow < 1.5ms, red above. EMA-smoothed, skips loading screen outliers.", "帧间隔偏离目标值的抖动绝对差（毫秒）。数值越小越平滑。绿色 < 0.5ms，黄色 < 1.5ms，红色超出。"));

#ifdef _WIN64
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "%s", TR("Pipeline", "渲染管线"));
        if (ImGui::Checkbox(TR("Frame Generation##osd_elem", "插帧状态 (FG)##osd_elem"), &g_config.osd_show_fg)) config_dirty = true;
        HelpTip(TR("Show FG mode (AFMF, Smooth Motion, etc.).", "显示插帧模式（如 AFMF、Smooth Motion 等）。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("Limiter / Tier##osd_elem", "限帧器/休眠等级##osd_elem"), &g_config.osd_show_limiter)) config_dirty = true;
        HelpTip(TR("Show how much time the limiter added and the current degradation tier (T0=full, T4=suspended).", "显示限帧器注入的等待时间以及当前的降级保护等级 (T0=全量, T4=暂停)。"));
#endif

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", TR("System (AMD Radeon & PC)", "硬件监控 (AMD Radeon & 系统)"));
        if (ImGui::Checkbox(TR("GPU Temp##osd_elem", "显卡温度##osd_elem"), &g_config.osd_show_gpu_temp)) config_dirty = true;
        HelpTip(TR("GPU core, hotspot, and VRAM temperatures in Celsius (AMD ADL2 PMLog). Color-coded: green < 75C, yellow < 90C, red above.",
                   "显卡核心、热点（结温）及显存温度（摄氏度，基于 AMD ADL2 PMLog）。绿色 < 75°C，黄色 < 90°C，红色超过 90°C。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("GPU Clock##osd_elem", "显卡频率##osd_elem"), &g_config.osd_show_gpu_clock)) config_dirty = true;
        HelpTip(TR("GPU core and memory clock speeds in MHz (AMD ADL2).", "GPU 核心与显存实时运行频率 (MHz，基于 AMD ADL2)。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("GPU Usage##osd_elem", "显卡占用##osd_elem"), &g_config.osd_show_gpu_usage)) config_dirty = true;
        HelpTip(TR("GPU graphics engine utilization percentage (AMD ADL2).", "GPU 图形引擎实时使用率百分比 (基于 AMD ADL2)。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("GPU Power##osd_elem", "显卡功耗##osd_elem"), &g_config.osd_show_gpu_power)) config_dirty = true;
        HelpTip(TR("GPU board power draw in watts (AMD ADL2 PMLog).", "GPU 整卡实时功耗 (W，基于 AMD ADL2 PMLog)。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("VRAM##osd_elem", "显存占用##osd_elem"), &g_config.osd_show_vram)) config_dirty = true;
        HelpTip(TR("Video memory usage (used / total).", "显存 (VRAM) 占用量与显存总量 (MB)。支持 AMD Radeon。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("CPU Usage##osd_elem", "CPU 占用##osd_elem"), &g_config.osd_show_cpu_usage)) config_dirty = true;
        HelpTip(TR("System-wide CPU utilization percentage.", "系统级整体 CPU 负载使用率百分比。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("CPU Clock##osd_elem", "CPU 频率##osd_elem"), &g_config.osd_show_cpu_clock)) config_dirty = true;
        HelpTip(TR("Real-time CPU effective operating frequency in MHz.", "处理器实时运行主频 (MHz)。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("CPU Power##osd_elem", "CPU 功耗##osd_elem"), &g_config.osd_show_cpu_power)) config_dirty = true;
        HelpTip(TR("CPU package power draw in watts (Windows RAPL / AMD ADL).", "处理器封装整颗功耗 (W，基于 Windows RAPL / AMD ADL)。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("RAM##osd_elem", "系统内存##osd_elem"), &g_config.osd_show_ram)) config_dirty = true;
        HelpTip(TR("System RAM usage (used / total) in GB.", "系统物理内存 (RAM) 占用量与总量 (GB)。"));

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f), "%s", TR("Display & API", "显示与图形接口"));
        if (ImGui::Checkbox(TR("API##osd_elem", "图形接口 (API)##osd_elem"), &g_config.osd_show_api)) config_dirty = true;
        HelpTip(TR("Show graphics API and binary architecture (e.g. DX12 64-bit, Vulkan).", "显示当前游戏运行的图形接口及架构位数（如 DX12 64位、Vulkan 等）。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("Resolution##osd_elem", "渲染分辨率##osd_elem"), &g_config.osd_show_resolution)) config_dirty = true;
        HelpTip(TR("Show current swapchain / backbuffer rendering resolution.", "显示当前游戏交换链/后台缓冲区的渲染分辨率。"));
        FlowSeparator();
        if (ImGui::Checkbox(TR("Flip Model##osd_elem", "翻转呈现模式##osd_elem"), &g_config.osd_show_flip_model)) config_dirty = true;
        HelpTip(TR("Show presentation model (FLIP_DISCARD, BitBlt, Mailbox, FIFO, etc.) and VRR tearing status.", "显示当前呈现翻转模式（如 FLIP_DISCARD、位块传输 BitBlt、Mailbox、FIFO 等）以及 VRR 撕裂支持状态。"));
    }

    // ════════════════════════════════════════════
    // SECTION 3: Screen (collapsible)
    // ════════════════════════════════════════════
    ImGui::Separator();

    // Initialize monitor list and restore selection (runs once regardless of section state)
    {
        static bool s_monitors_init = false;
        if (!s_monitors_init) {
            RefreshMonitorList();
            s_selected_monitor = g_config.selected_monitor;
            if (s_selected_monitor > s_monitor_count)
                s_selected_monitor = 0;
            s_monitors_init = true;
        }
    }

    // Sticky monitor enforcement — runs every frame, moves window back if it drifted
    EnforceStickyMonitor();

    if (ImGui::CollapsingHeader(TR("Screen", "显示与窗口"))) {
#ifndef _WIN64
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "Note: Screen options are experimental on 32-bit and may cause");
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "adverse effects (minimize loops, DWM throttling) in some games.");
        ImGui::Spacing();
#endif
        if (ImGui::Button(TR("Refresh##monitors", "刷新显示器列表##monitors"))) RefreshMonitorList();
        ImGui::SameLine();

        if (ImGui::BeginCombo(TR("Display", "目标显示器"), s_selected_monitor == 0 ? TR("Default", "默认")
                : s_monitors[s_selected_monitor - 1].label)) {
            if (ImGui::Selectable(TR("Default", "默认"), s_selected_monitor == 0)) {
                s_selected_monitor = 0;
                g_config.selected_monitor = 0;
                config_dirty = true;
            }
            for (int i = 0; i < s_monitor_count; i++) {
                bool selected = (s_selected_monitor == i + 1);
                if (ImGui::Selectable(s_monitors[i].label, selected)) {
                    s_selected_monitor = i + 1;
                    g_config.selected_monitor = i + 1;
                    config_dirty = true;
                    MoveWindowToMonitor(i);
                }
            }
            ImGui::EndCombo();
        }
        HelpTip(TR("Move the game window to a different monitor. Selection is saved and re-applied after alt-tab.", "将游戏窗口移动到指定显示器。该设置会自动保存，并在 Alt-Tab 切回时自动保持。"));

        ImGui::Spacing();

        // Window mode combo: Default / Borderless / Fullscreen
        const char* mode_labels_en[] = {"Default", "Borderless", "Fullscreen"};
        const char* mode_labels_zh[] = {"默认 (Default)", "无边框窗口 (Borderless)", "置顶全屏 (Fullscreen)"};
        const char** mode_labels = IsChinese() ? mode_labels_zh : mode_labels_en;
        int current_mode = 0;
        if (g_config.window_mode == "borderless") current_mode = 1;
        else if (g_config.window_mode == "fullscreen") current_mode = 2;

        if (ImGui::BeginCombo(TR("Window Mode", "窗口模式"), mode_labels[current_mode])) {
            for (int i = 0; i < 3; i++) {
                bool selected = (current_mode == i);
                if (ImGui::Selectable(mode_labels[i], selected)) {
                    if (i != current_mode) {
                        HWND hwnd = SwapMgr_GetHWND();
                        if (hwnd) {
                            const char* mode_str[] = {"default", "borderless", "fullscreen"};
                            g_config.window_mode = mode_str[i];
                            config_dirty = true;

                            struct WMData { HWND hwnd; int mode; };
                            auto* data = new WMData{ hwnd, i };
                            CreateThread(nullptr, 0, [](LPVOID param) -> DWORD {
                                auto* d = static_cast<WMData*>(param);
                                HMONITOR hmon = MonitorFromWindow(d->hwnd, MONITOR_DEFAULTTONEAREST);
                                MONITORINFO mi = {};
                                mi.cbSize = sizeof(mi);
                                GetMonitorInfo(hmon, &mi);
                                RECT& rc = mi.rcMonitor;
                                int w = rc.right - rc.left;
                                int h = rc.bottom - rc.top;

                                if (d->mode == 0) {
                                    LONG style = GetWindowLong(d->hwnd, GWL_STYLE);
                                    style |= (WS_CAPTION | WS_THICKFRAME | WS_BORDER);
                                    SetWindowLong(d->hwnd, GWL_STYLE, style);
                                    SetWindowPos(d->hwnd, nullptr,
                                                 rc.left + 20, rc.top + 20,
                                                 w - 40, h - 40,
                                                 SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
                                } else if (d->mode == 1) {
                                    LONG style = GetWindowLong(d->hwnd, GWL_STYLE);
                                    SetWindowLong(d->hwnd, GWL_STYLE,
                                                  style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER));
                                    SetWindowPos(d->hwnd, HWND_TOP,
                                                 rc.left, rc.top, w, h,
                                                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
                                } else if (d->mode == 2) {
                                    LONG style = GetWindowLong(d->hwnd, GWL_STYLE);
                                    SetWindowLong(d->hwnd, GWL_STYLE,
                                                  style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER));
                                    SetWindowPos(d->hwnd, HWND_TOPMOST,
                                                 rc.left, rc.top, w, h,
                                                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
                                }
                                delete d;
                                return 0;
                            }, data, 0, nullptr);
                        }
                    }
                }
            }
            ImGui::EndCombo();
        }
        HelpTip(TR("Default restores the game's normal window. Borderless removes borders and fills the screen. Fullscreen does the same but keeps the window topmost.",
                   "默认：恢复游戏原本窗口样式；无边框：消除边框并全屏铺满；置顶全屏：消除边框并将窗口始终置顶。"));

        ImGui::Spacing();
        if (ImGui::Checkbox(TR("Fake Fullscreen", "伪装独占全屏 (Fake Fullscreen)"), &g_config.fake_fullscreen)) config_dirty = true;
        HelpTip(TR("Intercept exclusive fullscreen and run as borderless window instead. The game still thinks it's in exclusive fullscreen. Takes effect on next fullscreen transition or game restart.",
                   "拦截独占全屏请求并以无边框窗口运行，但欺骗游戏使其认为仍处于独占全屏模式。下一次全屏切换或重启游戏生效。"));

        // ── Focus Lock ──
        ImGui::Spacing();
        if (ImGui::Checkbox(TR("Keep Game Focused", "防切后台锁定 (Focus Lock)"), &g_config.focus_lock)) {
            config_dirty = true;
            if (g_config.focus_lock) {
                HWND hwnd = SwapMgr_GetHWND();
                if (hwnd) FocusLock_Install(hwnd);
            } else {
                FocusLock_Remove();
            }
        }
        if (FocusLock_IsActive()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "%s", TR("(Active)", "(生效中)"));
        }
        HelpTip(TR("Prevent the game from detecting focus loss when you alt-tab. Audio and game logic continue running in the background. The background FPS cap still applies independently. May not work with all games (UWP/Game Pass titles use system-level notifications).",
                   "阻止游戏在 Alt-Tab 切到其他窗口时检测到焦点丢失。游戏音频和逻辑在后台持续运行。后台限帧设置依然独立生效。"));

        // ── Monitor Blackout ──
        ImGui::Spacing();
        {
            bool blackout_on = Blackout_IsActive();
            if (ImGui::Checkbox(TR("Blackout Other Monitors", "遮罩副屏 (纯黑防打扰)"), &blackout_on)) {
                Blackout_SetActive(blackout_on);
            }
            if (Blackout_IsActive()) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "%s", TR("(Active)", "(生效中)"));
            }
            HelpTip(TR("Cover all non-game monitors with a black screen. Useful for reducing distractions and light bleed in dark rooms. Toggle with the keybind below or this checkbox.",
                       "使用全黑遮罩覆盖非游戏副屏，减少暗室中的多屏光污染和干扰。可使用下方热键或复选框切换。"));

            // Blackout keybind
            static bool s_blackout_capturing = false;
            ImGui::Text("%s", TR("Blackout Key:", "副屏遮罩热键:"));
            ImGui::SameLine();
            if (s_blackout_capturing) {
                ImGui::TextColored(ImVec4(1,1,0.3f,1), "%s", TR("Press key...", "请按键..."));
                ImGui::SameLine();
                if (ImGui::Button(TR("Cancel##blackout", "取消##blackout"))) s_blackout_capturing = false;
                for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                    ImGuiKey key = static_cast<ImGuiKey>(k);
                    if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl) continue;
                    if (key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift) continue;
                    if (key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt) continue;
                    if (key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper) continue;
                    if (k == ImGuiMod_Ctrl || k == ImGuiMod_Shift || k == ImGuiMod_Alt || k == ImGuiMod_Super) continue;
                    if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) continue;
                    const char* kn = ImGui::GetKeyName(key);
                    if (kn && kn[0] == 'M' && kn[1] == 'o' && kn[2] == 'd') continue;
                    if (ImGui::IsKeyPressed(key, false)) {
                        std::string name;
                        if (ImGui::IsKeyDown(ImGuiMod_Ctrl)) name += "Ctrl+";
                        if (ImGui::IsKeyDown(ImGuiMod_Alt)) name += "Alt+";
                        if (ImGui::IsKeyDown(ImGuiMod_Shift)) name += "Shift+";
                        name += ImGui::GetKeyName(key);
                        g_config.blackout_key = name;
                        s_blackout_capturing = false;
                        config_dirty = true;
                        break;
                    }
                }
            } else {
                ImGui::Text("%s", g_config.blackout_key.empty() ? TR("None", "无") : g_config.blackout_key.c_str());
                ImGui::SameLine();
                if (ImGui::Button(TR("Bind##blackout", "绑定##blackout"))) s_blackout_capturing = true;
                if (!g_config.blackout_key.empty()) {
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Clear##blackout", "清除##blackout"))) {
                        g_config.blackout_key.clear();
                        config_dirty = true;
                    }
                }
            }
        }

        // ── OLED Care (sectioned off) ──
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        {
            ImGui::Text("%s", TR("OLED Care", "OLED 防烧屏保护"));
            HelpTip(TR("Blacks out monitors and reduces FPS to background cap for screen protection while AFK. Toggle with the keybind below.",
                       "挂机暂离时将屏幕全黑化并降至后台帧率限制，防止 OLED 面板长时间静态烧屏。使用下方热键或挂机计时器激活。"));

            // OLED Care keybind + All Monitors on same line
            static bool s_oled_care_capturing = false;
            ImGui::Text("%s", TR("OLED Care Key:", "防烧屏热键:"));
            ImGui::SameLine();
            if (s_oled_care_capturing) {
                ImGui::TextColored(ImVec4(1,1,0.3f,1), "%s", TR("Press key...", "请按键..."));
                ImGui::SameLine();
                if (ImGui::Button(TR("Cancel##oled_care", "取消##oled_care"))) s_oled_care_capturing = false;
                for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                    ImGuiKey key = static_cast<ImGuiKey>(k);
                    if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl) continue;
                    if (key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift) continue;
                    if (key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt) continue;
                    if (key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper) continue;
                    if (k == ImGuiMod_Ctrl || k == ImGuiMod_Shift || k == ImGuiMod_Alt || k == ImGuiMod_Super) continue;
                    if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) continue;
                    const char* kn = ImGui::GetKeyName(key);
                    if (kn && kn[0] == 'M' && kn[1] == 'o' && kn[2] == 'd') continue;
                    if (ImGui::IsKeyPressed(key, false)) {
                        std::string name;
                        if (ImGui::IsKeyDown(ImGuiMod_Ctrl)) name += "Ctrl+";
                        if (ImGui::IsKeyDown(ImGuiMod_Alt)) name += "Alt+";
                        if (ImGui::IsKeyDown(ImGuiMod_Shift)) name += "Shift+";
                        name += ImGui::GetKeyName(key);
                        g_config.oled_care_key = name;
                        s_oled_care_capturing = false;
                        config_dirty = true;
                        break;
                    }
                }
            } else {
                ImGui::Text("%s", g_config.oled_care_key.empty() ? TR("None", "无") : g_config.oled_care_key.c_str());
                ImGui::SameLine();
                if (ImGui::Button(TR("Bind##oled_care", "绑定##oled_care"))) s_oled_care_capturing = true;
                if (!g_config.oled_care_key.empty()) {
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Clear##oled_care", "清除##oled_care"))) {
                        g_config.oled_care_key.clear();
                        config_dirty = true;
                    }
                }
                ImGui::SameLine();
                ImGui::TextDisabled("|");
                ImGui::SameLine();
                if (ImGui::Checkbox(TR("All Monitors##oled_care", "遮罩所有屏幕##oled_care"), &g_config.oled_care_all_monitors))
                    config_dirty = true;
                HelpTip(TR("When ticked, all monitors are blacked out. When unticked, only the game's display is blacked out.", "勾选时遮罩所有显示器；未勾选时仅黑化游戏所在的主显示器。"));
            }

            // ── Idle timer ──
            {
                static int s_idle_edit = 0;
                static bool s_idle_active = false;
                if (!s_idle_active)
                    s_idle_edit = g_config.oled_care_idle_minutes;

                ImGui::Text("%s", TR("Auto after idle:", "空闲自动激活:"));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(45.0f);
                if (ImGui::InputInt(TR("min##oled_care_idle_input", "分钟##oled_care_idle_input"), &s_idle_edit, 0, 0)) {
                    if (s_idle_edit < 0) s_idle_edit = 0;
                    if (s_idle_edit > 999) s_idle_edit = 999;
                    g_config.oled_care_idle_minutes = s_idle_edit;
                    config_dirty = true;
                }
                s_idle_active = ImGui::IsItemActive();
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    g_config.oled_care_idle_minutes = s_idle_edit;
                    config_dirty = true;
                }
                HelpTip(TR("Automatically activate OLED Care after this many minutes without any mouse, keyboard, or controller input. 0 = disabled. Any input (moving the mouse, pressing a key, clicking, or using a controller) will turn it back off.",
                           "无鼠标、键盘或手柄输入达到该分钟数后自动激活 OLED Care 防烧屏。0 = 禁用。任何输入操作均可立即恢复。"));
            }
        }
    }

    // ════════════════════════════════════════════
    // SECTION: Adaptive Smoothing (collapsible)
    // ════════════════════════════════════════════
    ActiveAPI active_api = SwapMgr_GetActiveAPI();
    if (active_api == ActiveAPI::DX12 || active_api == ActiveAPI::DX11 || active_api == ActiveAPI::Vulkan) {
    ImGui::Separator();
    if (ImGui::CollapsingHeader(TR("Adaptive Smoothing", "自适应平滑 (Adaptive Smoothing)"))) {
        bool adaptive = g_config.adaptive_smoothing;
        if (ImGui::Checkbox(TR("Enable##adaptive", "启用自适应平滑##adaptive"), &adaptive)) {
            g_config.adaptive_smoothing = adaptive;
            g_adaptive_smoothing.SetConfig(
                g_config.smoothing_window == "dual",
                g_config.smoothing_percentile,
                adaptive);
            config_dirty = true;
        }
        if (g_adaptive_smoothing.IsWarm() && g_config.adaptive_smoothing) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "(+%.1f us)",
                               g_smoothing_offset_us.load(std::memory_order_relaxed));
        }
        HelpTip(TR("P99-based adaptive interval extension. Extends the target interval so 99% of frames complete within it, reducing micro-stutters from render time variance. Supports AMD, Intel, and NVIDIA.",
                   "基于 P99 渲染耗时的自适应间隔动态补偿。动态扩展目标帧间隔，确保 99% 的帧在预算内完成，有效消除显卡渲染耗时波动引起的微卡顿。支持 AMD、Intel 与 NVIDIA 显卡。"));

        if (g_config.adaptive_smoothing) {
            if (ImGui::Checkbox(TR("Show on OSD##adaptive", "在 OSD 上显示自适应平滑##adaptive"), &g_config.osd_show_adaptive_smoothing))
                config_dirty = true;
            ImGui::SameLine();
            if (ImGui::Checkbox(TR("Show Bottleneck##adaptive", "在 OSD 显示瓶颈##adaptive"), &g_config.osd_show_bottleneck))
                config_dirty = true;
            ImGui::SameLine();
            if (ImGui::Checkbox(TR("Show CPU Work##adaptive", "在 OSD 显示 CPU 耗时##adaptive"), &g_config.osd_show_cpu_work))
                config_dirty = true;
            HelpTip(TR("Show adaptive smoothing offset, bottleneck state (GPU/CPU), and CPU work on the in-game overlay.",
                       "在游戏内 OSD 监控面板上显示自适应平滑偏置、硬件瓶颈状态 (显卡/处理器) 及 CPU 真实耗时。"));

            // Percentile slider
            float pct = static_cast<float>(g_config.smoothing_percentile * 100.0);
            if (ImGui::SliderFloat(TR("Percentile##adaptive", "目标分位数 (Percentile)##adaptive"), &pct, 50.0f, 99.9f, "P%.1f")) {
                g_config.smoothing_percentile = static_cast<double>(pct) / 100.0;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                g_adaptive_smoothing.SetConfig(
                    g_config.smoothing_window == "dual",
                    g_config.smoothing_percentile,
                    g_config.adaptive_smoothing,
                    g_config.queue_drain_margin_pct,
                    g_config.enable_fast_release);
                config_dirty = true;
            }
            HelpTip(TR("Target percentile for the threshold. P99 = 99% of frames fit within the interval. Lower = more headroom, higher = tighter.",
                       "平滑的目标耗时分位数。P99 代表保证 99% 的帧落在该区间内。较低值容差更大，较高值抗偶发卡顿更严苛。"));

            // Window mode combo
            const char* win_labels_en[] = {"Medium (200)", "Dual (64+200)"};
            const char* win_labels_zh[] = {"中窗口 Medium (200帧)", "双窗口 Dual (64+200帧)"};
            const char** win_labels = IsChinese() ? win_labels_zh : win_labels_en;
            int current_win = (g_config.smoothing_window == "dual") ? 1 : 0;
            if (ImGui::BeginCombo(TR("Window##adaptive", "采样窗口模式 (Window)##adaptive"), win_labels[current_win])) {
                for (int i = 0; i < 2; i++) {
                    bool selected = (current_win == i);
                    if (ImGui::Selectable(win_labels[i], selected)) {
                        g_config.smoothing_window = (i == 1) ? "dual" : "medium";
                        g_adaptive_smoothing.SetConfig(
                            i == 1,
                            g_config.smoothing_percentile,
                            g_config.adaptive_smoothing,
                            g_config.queue_drain_margin_pct,
                            g_config.enable_fast_release);
                        config_dirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            HelpTip(TR("Medium: single 200-frame window (~3s). Dual: short 64 + long 200 window, uses max of both P99s for robustness against scene changes.",
                       "Medium: 单一 200 帧滑动窗口 (约 3 秒)；Dual: 结合短窗口 (64帧) 与长窗口 (200帧) 取最大值，对场景剧烈切换更鲁棒抗卡。"));

            // BBR Queue Drain Margin slider
            float drain_pct = static_cast<float>(g_config.queue_drain_margin_pct * 100.0);
            if (ImGui::SliderFloat(TR("Queue Drain Margin (%)##adaptive", "BBR 队列排空余量 (%)##adaptive"), &drain_pct, 0.0f, 5.0f, "%.1f %%")) {
                g_config.queue_drain_margin_pct = static_cast<double>(drain_pct) / 100.0;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                g_adaptive_smoothing.SetConfig(
                    g_config.smoothing_window == "dual",
                    g_config.smoothing_percentile,
                    g_config.adaptive_smoothing,
                    g_config.queue_drain_margin_pct,
                    g_config.enable_fast_release);
                config_dirty = true;
            }
            HelpTip(TR("BBR-inspired negative gain headroom when GPU bound. Targets slightly below GPU saturation to continuously drain driver queues and eliminate bufferbloat latency. Default: 1.5%.",
                       "借鉴 BBR 拥塞控制思想的负增益排空余量。当显卡满载时主动将目标帧率压在极限吞吐之下约 1.5%，持续排空驱动命令队列，降低输入延迟。"));

            // Fast release checkbox
            if (ImGui::Checkbox(TR("Fast Release (Headroom Streak)##adaptive", "空闲快速释放 (Headroom Streak)##adaptive"), &g_config.enable_fast_release)) {
                g_adaptive_smoothing.SetConfig(
                    g_config.smoothing_window == "dual",
                    g_config.smoothing_percentile,
                    g_config.adaptive_smoothing,
                    g_config.queue_drain_margin_pct,
                    g_config.enable_fast_release);
                config_dirty = true;
            }
            HelpTip(TR("Detects sustained hardware headroom (>=12% for 60 frames) and accelerates release back to the target FPS cap, preventing lingering low FPS when moving to lighter scenes.",
                       "余量连续确认快速释放：当检测到持续拥有 >=12% 硬件余量达 60 帧时，主动裁剪历史高值，让帧率在 1-2 秒内平滑回弹至上限，彻底消除低帧率粘滞。"));

            // Bias slider
            float bias = static_cast<float>(g_config.smoothing_bias_us);
            if (ImGui::SliderFloat(TR("Bias##adaptive", "额外安全偏置 (Bias)##adaptive"), &bias, 0.0f, 1000.0f, "%.0f us")) {
                g_config.smoothing_bias_us = static_cast<double>(bias);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                config_dirty = true;
            }
            HelpTip(TR("Constant offset added on top of the computed P99 smoothing. Use this to add extra headroom if the automatic offset isn't enough. 0 = no extra bias, 1000 = +1ms added to every frame interval.",
                       "在计算出的平滑耗时基础上额外增加的固定安全余量。若自动计算仍偶有微卡顿可适当调大。0 = 不加额外偏置，1000 = 额外增加 1 毫秒余量。"));
        }
    }
    } // DX12 only (Adaptive Smoothing)

    // ════════════════════════════════════════════
    // SECTION 4: Advanced (collapsible)
    // ════════════════════════════════════════════
    ImGui::Separator();
    if (ImGui::CollapsingHeader(TR("Advanced", "高级选项"))) {
#ifdef _WIN64
        // Flip Model Override toggle (DX11 only)
        static bool s_flip_model_at_launch = g_config.flip_model_override;
        bool flip_override = g_config.flip_model_override;
        if (ImGui::Checkbox(TR("Flip Model Override", "DX11 翻转模型覆盖 (Flip Model)"), &flip_override)) {
            g_config.flip_model_override = flip_override;
            config_dirty = true;
        }
        HelpTip(TR("Force DX11 games from bitblt to flip model presentation. Enables true FreeSync/VRR operation and eliminates DWM composition stutter. Requires game restart to take effect.",
                   "强制将 DX11 传统位块传输升级为现代翻转模型呈现。彻底消除桌面窗口管理器 (DWM) 合成卡顿并解锁完整 FreeSync/VRR。需重启游戏生效。"));
        if (FlipModel_WasApplied()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "%s", TR("(Active)", "(生效中)"));
        } else if (FlipModel_IsNativeFlip()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "%s", TR("(Native)", "(原生翻转)"));
        } else if (g_config.flip_model_override && SwapMgr_GetActiveAPI() == ActiveAPI::DX11) {
            ImGui::SameLine();
            if (s_flip_model_at_launch && SwapMgr_IsValid())
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", TR("(Not supported)", "(不受支持)"));
            else
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", TR("(Restart required)", "(需重启游戏)"));
        }

        ImGui::Spacing();
#endif // _WIN64

        bool csv = g_config.csv_enabled;
        if (ImGui::Checkbox(TR("Telemetry Logging", "启用 CSV 遥测日志"), &csv)) {
            g_config.csv_enabled = csv;
            CSV_SetEnabled(csv);
            config_dirty = true;
        }
        HelpTip(TR("Enable per-frame CSV telemetry recording. Toggle in-game with the CSV hotkey (default F11).", "开启逐帧 CSV 遥测数据记录。游戏内可按热键（默认 F11）快捷启闭。"));

        // Advanced Logging toggle (log_level: warn <-> info)
        bool info_logging = (g_config.log_level == "info");
        if (ImGui::Checkbox(TR("Advanced Logging", "详细诊断日志 (Debug Info)"), &info_logging)) {
            g_config.log_level = info_logging ? "info" : "warn";
            Log_SetLevel(Log_ParseLevel(g_config.log_level.c_str()));
            config_dirty = true;
        }
        HelpTip(TR("When enabled, the log file records detailed info-level messages. When disabled, only warnings and errors are logged. Enable this before reporting issues.",
                   "启用后日志记录详细排查信息；禁用时仅记录警告与错误。反馈排查问题时建议开启。"));
    }

    // ════════════════════════════════════════════
    // OSD Toggle Keybind (always visible, not in a dropdown)
    // ════════════════════════════════════════════
    ImGui::Separator();
    {
        static bool s_capturing = false;

        ImGui::Text("%s", TR("OSD Toggle Key:", "OSD 开关快捷键:"));
        ImGui::SameLine();

        if (s_capturing) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1.0f), "%s", TR("Press key combo...", "请按下组合键..."));
            ImGui::SameLine();
            if (ImGui::Button(TR("Cancel", "取消"))) {
                s_capturing = false;
            }

            // Use ImGui key state — works inside the ReShade overlay
            bool ctrl  = ImGui::IsKeyDown(ImGuiMod_Ctrl);
            bool alt   = ImGui::IsKeyDown(ImGuiMod_Alt);
            bool shift = ImGui::IsKeyDown(ImGuiMod_Shift);

            // Scan ImGuiKey range for a non-modifier key press
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++) {
                ImGuiKey key = static_cast<ImGuiKey>(k);
                // Skip modifiers (both physical keys and mod flags)
                if (key == ImGuiKey_LeftCtrl  || key == ImGuiKey_RightCtrl)  continue;
                if (key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift) continue;
                if (key == ImGuiKey_LeftAlt   || key == ImGuiKey_RightAlt)   continue;
                if (key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper) continue;
                if (k == ImGuiMod_Ctrl || k == ImGuiMod_Shift || k == ImGuiMod_Alt || k == ImGuiMod_Super) continue;
                // Skip mouse buttons
                if (key == ImGuiKey_MouseLeft || key == ImGuiKey_MouseRight ||
                    key == ImGuiKey_MouseMiddle || key == ImGuiKey_MouseX1 ||
                    key == ImGuiKey_MouseX2 || key == ImGuiKey_MouseWheelX ||
                    key == ImGuiKey_MouseWheelY) continue;
                // Skip any key whose name starts with "Mod" (catches ModCtrl, ModAlt, etc.)
                const char* kname = ImGui::GetKeyName(key);
                if (kname && kname[0] == 'M' && kname[1] == 'o' && kname[2] == 'd') continue;

                if (ImGui::IsKeyPressed(key, false)) {
                    std::string name;
                    if (ctrl)  name += "Ctrl+";
                    if (alt)   name += "Alt+";
                    if (shift) name += "Shift+";
                    name += ImGui::GetKeyName(key);

                    g_config.osd_toggle_key = name;
                    s_capturing = false;
                    config_dirty = true;
                    break;
                }
            }
        } else {
            ImGui::Text("%s", g_config.osd_toggle_key.c_str());
            ImGui::SameLine();
            if (ImGui::Button(TR("Bind", "绑定快捷键"))) {
                s_capturing = true;
            }
        }
        HelpTip(TR("Keybind to toggle the OSD overlay on/off. Click Bind, then press a key combo (e.g. Ctrl+F12, Alt+P). Works in-game and with the ReShade UI open.",
                   "快捷启闭 OSD 面板的热键。点击绑定后直接在键盘上按下组合键（如 Ctrl+F12）。"));
    }

    // ════════════════════════════════════════════
    // STATUS INFO (always visible at bottom, not on OSD)
    // ════════════════════════════════════════════
    ImGui::Separator();
    {
        ActiveAPI api = SwapMgr_GetActiveAPI();
        const char* api_name = "None";
        if (api == ActiveAPI::DX12) api_name = "DX12";
        else if (api == ActiveAPI::DX11) api_name = "DX11";
        else if (api == ActiveAPI::DX9) api_name = "DX9";
        else if (api == ActiveAPI::Vulkan) api_name = "Vulkan";
        else if (api == ActiveAPI::OpenGL) api_name = "OpenGL";

#ifdef _WIN64
        const char* enforce_mode = "Present";
        if (api == ActiveAPI::DX12) enforce_mode = "Presentation Gate (DX12)";
        else if (api == ActiveAPI::DX11) enforce_mode = "Presentation Gate (DX11)";
        else if (api == ActiveAPI::Vulkan) enforce_mode = "Presentation Gate (Vulkan)";
        else if (api == ActiveAPI::OpenGL) enforce_mode = "Presentation Gate (OpenGL)";

        PacingMode mode = g_pacing_mode.load(std::memory_order_relaxed);
        ImGui::Text("API: %s (64-bit)  |  Telemetry: AMD ADL2 PMLog  |  FreeSync/VRR: %s",
                    api_name, (mode == PacingMode::VRR) ? "VRR" : "Fixed");
        ImGui::Text("Enforce: %s%s", enforce_mode,
                    FlipModel_WasApplied() ? "  |  Flip Model: Active" : "");

        double ceil = g_ceiling_hz.load(std::memory_order_relaxed);
        double floor = g_floor_hz.load(std::memory_order_relaxed);
        if (mode == PacingMode::VRR && ceil > 1.0 && floor > 0.0)
            ImGui::Text("FreeSync / VRR Range: %.0f - %.0f Hz", floor, ceil);
#else
        ImGui::Text("API: %s (32-bit)  |  Enforce: Present", api_name);
#endif

        // Pipeline health
        bool present_ok = IsSwapchainValid();
        ImVec4 col_ok = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);
        ImVec4 col_bad = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);

        ImGui::Text("Pipeline:");
        ImGui::SameLine();
        ImGui::TextColored(present_ok ? col_ok : col_bad, "PRESENT %s", present_ok ? "ok" : "X");
    }

    // Save immediately when any setting was changed this frame
    if (config_dirty)
        SaveConfig();
}

// ── 1% low tracking ──
static constexpr int LOW_HISTORY_SIZE = 1000;
static double s_low_history[LOW_HISTORY_SIZE] = {};
static int    s_low_history_idx = 0;
static int    s_low_history_count = 0;

// Cached results — recomputed every 30 frames instead of every frame
static double s_cached_1pct_low = 0.0;
static double s_cached_01pct_low = 0.0;
static int    s_low_recompute_counter = 0;

void OSD_ResetLowHistory() {
    s_low_history_count = 0;
    s_low_history_idx = 0;
    s_cached_1pct_low = 0.0;
    s_cached_01pct_low = 0.0;
}

void OSD_CyclePosition() {
    // Corner positions (clockwise): top-left, top-right, bottom-right, bottom-left
    // Determine current corner based on osd_x and osd_y
    // x < 0.5 = left, x >= 0.5 = right
    // y < 0.5 = top, y >= 0.5 = bottom
    
    bool is_right = g_config.osd_x >= 0.5f;
    bool is_bottom = g_config.osd_y >= 0.5f;
    
    // Current corner: 0=top-left, 1=top-right, 2=bottom-right, 3=bottom-left
    int corner = 0;
    if (!is_right && !is_bottom) corner = 0;      // top-left
    else if (is_right && !is_bottom) corner = 1;  // top-right
    else if (is_right && is_bottom) corner = 2;   // bottom-right
    else corner = 3;                               // bottom-left
    
    // Cycle clockwise: top-left → top-right → bottom-right → bottom-left → top-left
    corner = (corner + 1) % 4;
    
    // Set new position - exact corners, no margin
    switch (corner) {
        case 0:  // top-left
            g_config.osd_x = 0.0f;
            g_config.osd_y = 0.0f;
            break;
        case 1:  // top-right
            g_config.osd_x = 1.0f;
            g_config.osd_y = 0.0f;
            break;
        case 2:  // bottom-right
            g_config.osd_x = 1.0f;
            g_config.osd_y = 1.0f;
            break;
        case 3:  // bottom-left
            g_config.osd_x = 0.0f;
            g_config.osd_y = 1.0f;
            break;
    }
}

static void RecomputeLowFPS() {
    int total = s_low_history_count < LOW_HISTORY_SIZE ? s_low_history_count : LOW_HISTORY_SIZE;

    // Filter out pause/loading outliers (>100ms / <10 FPS)
    static double sorted[LOW_HISTORY_SIZE];
    int n = 0;
    for (int i = 0; i < total; i++) {
        double val = s_low_history[i];
        if (val > 0.0 && val <= 100000.0) { // under 100ms
            sorted[n++] = val;
        }
    }

    if (n >= 30) {
        std::sort(sorted, sorted + n);

        // 1% low: 99th percentile frame time (or average of top 1% slowest frames)
        int tail_start = static_cast<int>(0.99 * (n - 1));
        int tail_count = n - tail_start;
        if (tail_count < 1) tail_count = 1;
        double sum = 0.0;
        for (int i = tail_start; i < n; i++)
            sum += sorted[i];
        double avg_worst = sum / tail_count;
        s_cached_1pct_low = (avg_worst > 0.0) ? 1000000.0 / avg_worst : 0.0;

        // 0.1% low: 99.9th percentile frame time
        if (n >= 100) {
            int tail_01 = static_cast<int>(0.999 * (n - 1));
            int count_01 = n - tail_01;
            if (count_01 < 1) count_01 = 1;
            double sum_01 = 0.0;
            for (int i = tail_01; i < n; i++)
                sum_01 += sorted[i];
            double avg_01 = sum_01 / count_01;
            s_cached_01pct_low = (avg_01 > 0.0) ? 1000000.0 / avg_01 : 0.0;
        }
    }
}

// ── Category colors (Muted Nord / MangoHud Aesthetic Palette) ──
static ImVec4 ColPerf()     { float b = g_config.osd_text_brightness; return ImVec4(0.35f*b, 0.82f*b, 0.55f*b, 1.0f); } // Sage Green
static ImVec4 ColLatency()  { float b = g_config.osd_text_brightness; return ImVec4(0.95f*b, 0.72f*b, 0.32f*b, 1.0f); } // Warm Gold
static ImVec4 ColPipeline() { float b = g_config.osd_text_brightness; return ImVec4(0.60f*b, 0.75f*b, 0.95f*b, 1.0f); } // Slate Blue
static ImVec4 ColSystem()   { float b = g_config.osd_text_brightness; return ImVec4(0.88f*b, 0.90f*b, 0.92f*b, 1.0f); } // Crisp Off-White
static ImVec4 ColStatus()   { float b = g_config.osd_text_brightness; return ImVec4(0.95f*b, 0.35f*b, 0.35f*b, 1.0f); } // Crimson

static ImVec4 PQIColor(double score) {
    float b = g_config.osd_text_brightness;
    if (score >= 0.9) return ImVec4(0.35f*b, 0.82f*b, 0.55f*b, 1.0f);
    if (score >= 0.7) return ImVec4(0.95f*b, 0.75f*b, 0.25f*b, 1.0f);
    return ImVec4(0.95f*b, 0.35f*b, 0.35f*b, 1.0f);
}

static void OSDTextColored(ImVec4 col, const char* text) {
    if (g_config.osd_drop_shadow) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        float offset = (std::max)(1.0f, g_config.osd_scale);
        ImU32 shadow = ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.8f));
        ImGui::GetWindowDrawList()->AddText(ImVec2(pos.x + offset, pos.y + offset), shadow, text);
    }
    ImGui::TextColored(col, "%s", text);
}

// ── Parse keybind string into VK code + modifier flags ──
// Key names come from ImGui::GetKeyName() (e.g. "F12", "P", "Escape", "Tab")
struct ParsedKeybind {
    int vk = 0;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
};

static int KeyNameToVK(const std::string& name) {
    // F-keys
    if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f') && isdigit(name[1])) {
        int fnum = atoi(name.c_str() + 1);
        if (fnum >= 1 && fnum <= 24) return VK_F1 + fnum - 1;
    }
    // Single letter
    if (name.size() == 1 && isalpha(name[0])) return toupper(name[0]);
    // Single digit
    if (name.size() == 1 && isdigit(name[0])) return name[0]; // VK 0-9 match ASCII
    // Named keys
    if (_stricmp(name.c_str(), "Space") == 0)     return VK_SPACE;
    if (_stricmp(name.c_str(), "Tab") == 0)       return VK_TAB;
    if (_stricmp(name.c_str(), "Enter") == 0)     return VK_RETURN;
    if (_stricmp(name.c_str(), "Escape") == 0)    return VK_ESCAPE;
    if (_stricmp(name.c_str(), "Backspace") == 0) return VK_BACK;
    if (_stricmp(name.c_str(), "Delete") == 0)    return VK_DELETE;
    if (_stricmp(name.c_str(), "Insert") == 0)    return VK_INSERT;
    if (_stricmp(name.c_str(), "Home") == 0)      return VK_HOME;
    if (_stricmp(name.c_str(), "End") == 0)       return VK_END;
    if (_stricmp(name.c_str(), "PageUp") == 0)    return VK_PRIOR;
    if (_stricmp(name.c_str(), "PageDown") == 0)  return VK_NEXT;
    if (_stricmp(name.c_str(), "Pause") == 0)     return VK_PAUSE;
    if (_stricmp(name.c_str(), "ScrollLock") == 0) return VK_SCROLL;
    if (_stricmp(name.c_str(), "PrintScreen") == 0) return VK_SNAPSHOT;
    if (_stricmp(name.c_str(), "LeftBracket") == 0)  return VK_OEM_4;    // [
    if (_stricmp(name.c_str(), "RightBracket") == 0) return VK_OEM_6;    // ]
    if (_stricmp(name.c_str(), "Backslash") == 0)    return VK_OEM_5;    // '\'
    if (_stricmp(name.c_str(), "Semicolon") == 0)    return VK_OEM_1;    // ;
    if (_stricmp(name.c_str(), "Apostrophe") == 0)   return VK_OEM_7;    // '
    if (_stricmp(name.c_str(), "Comma") == 0)        return VK_OEM_COMMA;
    if (_stricmp(name.c_str(), "Period") == 0)       return VK_OEM_PERIOD;
    if (_stricmp(name.c_str(), "Slash") == 0)        return VK_OEM_2;    // /
    if (_stricmp(name.c_str(), "GraveAccent") == 0)  return VK_OEM_3;   // `
    if (_stricmp(name.c_str(), "Minus") == 0)        return VK_OEM_MINUS;
    if (_stricmp(name.c_str(), "Equal") == 0)        return VK_OEM_PLUS; // = key (VK_OEM_PLUS is the =/+ key)
    if (_stricmp(name.c_str(), "UpArrow") == 0)      return VK_UP;
    if (_stricmp(name.c_str(), "DownArrow") == 0)    return VK_DOWN;
    if (_stricmp(name.c_str(), "LeftArrow") == 0)    return VK_LEFT;
    if (_stricmp(name.c_str(), "RightArrow") == 0)   return VK_RIGHT;
    if (_stricmp(name.c_str(), "NumLock") == 0)      return VK_NUMLOCK;
    if (_stricmp(name.c_str(), "CapsLock") == 0)     return VK_CAPITAL;
    // Numpad
    if (_stricmp(name.c_str(), "Keypad0") == 0)      return VK_NUMPAD0;
    if (_stricmp(name.c_str(), "Keypad1") == 0)      return VK_NUMPAD1;
    if (_stricmp(name.c_str(), "Keypad2") == 0)      return VK_NUMPAD2;
    if (_stricmp(name.c_str(), "Keypad3") == 0)      return VK_NUMPAD3;
    if (_stricmp(name.c_str(), "Keypad4") == 0)      return VK_NUMPAD4;
    if (_stricmp(name.c_str(), "Keypad5") == 0)      return VK_NUMPAD5;
    if (_stricmp(name.c_str(), "Keypad6") == 0)      return VK_NUMPAD6;
    if (_stricmp(name.c_str(), "Keypad7") == 0)      return VK_NUMPAD7;
    if (_stricmp(name.c_str(), "Keypad8") == 0)      return VK_NUMPAD8;
    if (_stricmp(name.c_str(), "Keypad9") == 0)      return VK_NUMPAD9;
    // Hex fallback
    if (name.size() >= 3 && name[0] == '0' && (name[1] == 'x' || name[1] == 'X'))
        return static_cast<int>(strtol(name.c_str(), nullptr, 16));
    return 0;
}

static ParsedKeybind ParseKeybind(const std::string& str) {
    ParsedKeybind kb;
    if (str.empty()) return kb;

    std::string s = str;

    auto consume = [&](const char* prefix) -> bool {
        size_t len = strlen(prefix);
        if (s.size() > len && _strnicmp(s.c_str(), prefix, len) == 0) {
            s = s.substr(len);
            return true;
        }
        return false;
    };

    while (true) {
        if (consume("Ctrl+"))  { kb.ctrl = true; continue; }
        if (consume("Alt+"))   { kb.alt = true; continue; }
        if (consume("Shift+")) { kb.shift = true; continue; }
        break;
    }

    kb.vk = KeyNameToVK(s);
    return kb;
}

static inline ParsedKeybind GetCachedKeybind(const std::string& str, std::string& cached_str, ParsedKeybind& cached_kb) {
    if (str != cached_str) {
        cached_str = str;
        cached_kb = ParseKeybind(str);
    }
    return cached_kb;
}

// ── Modern OSD badge & layout helpers (MangoHud / Nord Matte Palette) ──
static constexpr ImU32 BADGE_FPS  = IM_COL32(82, 185, 124, 255);  // Sage Green
static constexpr ImU32 BADGE_GPU  = IM_COL32(235, 90, 100, 255);  // AMD Radeon Crimson
static constexpr ImU32 BADGE_VRAM = IM_COL32(175, 140, 220, 255); // Lavender / Mauve
static constexpr ImU32 BADGE_CPU  = IM_COL32(80, 155, 235, 255);  // Electric Blue
static constexpr ImU32 BADGE_RAM  = IM_COL32(215, 130, 155, 255); // Dusty Rose
static constexpr ImU32 BADGE_LAT  = IM_COL32(235, 170, 70, 255);  // Warm Gold / Amber
static constexpr ImU32 BADGE_PIPE = IM_COL32(135, 155, 225, 255); // Slate Indigo
static constexpr ImU32 BADGE_API  = IM_COL32(130, 150, 170, 255); // Steel Grey
static constexpr ImU32 BADGE_REC  = IM_COL32(220, 50, 50, 255);   // Red
static constexpr ImU32 BADGE_BASE = IM_COL32(255, 130, 0, 255);   // Amber

static void DrawBadge(const char* tag, ImU32 accent_col, float scale, float min_width = 0.0f) {
    ImVec2 txt_sz = ImGui::CalcTextSize(tag);
    float pad_x = 5.0f * scale;
    float pad_y = 1.5f * scale;
    float pill_w = txt_sz.x + pad_x * 2.0f;
    if (min_width > 0.0f && pill_w < min_width * scale) {
        pill_w = min_width * scale;
        pad_x = (pill_w - txt_sz.x) * 0.5f;
    }
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(p0.x + pill_w, p0.y + txt_sz.y + pad_y * 2.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float round_r = (std::min)(4.0f * scale, (p1.y - p0.y) * 0.5f);

    ImU32 r = (accent_col) & 0xFF;
    ImU32 g = (accent_col >> 8) & 0xFF;
    ImU32 b = (accent_col >> 16) & 0xFF;

    ImU32 bg_col = IM_COL32(r, g, b, 32);       // 12.5% soft tinted frosted glass
    ImU32 border_col = IM_COL32(r, g, b, 115);  // 45% delicate hairline border
    ImU32 text_col = IM_COL32(r, g, b, 245);    // crisp readable label

    dl->AddRectFilled(p0, p1, bg_col, round_r);
    dl->AddRect(p0, p1, border_col, round_r, 0, 1.0f);
    dl->AddText(ImVec2(p0.x + pad_x, p0.y + pad_y), text_col, tag);
    ImGui::SetCursorScreenPos(ImVec2(p1.x + 6.0f * scale, p0.y));
}

static void DrawDot(float scale) {
    ImGui::SameLine(0.0f, 4.0f * scale);
    float b = g_config.osd_text_brightness;
    ImGui::TextColored(ImVec4(0.45f * b, 0.50f * b, 0.58f * b, 0.70f), "·");
    ImGui::SameLine(0.0f, 4.0f * scale);
}

static void DrawBarDivider(float scale) {
    ImGui::SameLine(0.0f, 7.0f * scale);
    float b = g_config.osd_text_brightness;
    ImGui::TextColored(ImVec4(0.35f * b, 0.42f * b, 0.50f * b, 0.55f), "|");
    ImGui::SameLine(0.0f, 7.0f * scale);
}

static void DrawModernCardOSD(float scale, double ft_ms) {
    const HWMonitorData& hw = HWMonitor_GetData();
    float b = g_config.osd_text_brightness;
    constexpr float COL_W = 38.0f; // Aligned badge column width for perfect tabular layout

    // ── Module 1: FPS & Frametime ──
    if (g_config.osd_show_fps || g_config.osd_show_frametime || g_config.osd_show_1pct_low || g_config.osd_show_0_1pct_low) {
        DrawBadge("FPS", BADGE_FPS, scale, COL_W);

        bool has_first = false;
        if (g_config.osd_show_fps) {
            double output = s_display_output_fps;
            int fg_div_raw = ComputeFGDivisorRaw();
            char buf[64];
            if (output > 0.0 && fg_div_raw > 1) {
                snprintf(buf, sizeof(buf), TR("%.0f fps (%.1f)", "%.0f FPS (%.1f 渲染)"), output, s_display_fps);
            } else {
                double show_fps = s_display_fps;
                if (g_user_target_fps.load(std::memory_order_relaxed) <= 0 && output > 0.0 && std::abs(show_fps - output) > 10.0) {
                    show_fps = output;
                }
                snprintf(buf, sizeof(buf), TR("%.1f fps", "%.1f FPS"), show_fps);
            }
            OSDTextColored(ColPerf(), buf);
            has_first = true;
        }

        if (g_config.osd_show_frametime) {
            if (has_first) DrawDot(scale);
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f ms", ft_ms);
            OSDTextColored(ColPerf(), buf);
            has_first = true;
        }

        if (g_config.osd_show_1pct_low && s_cached_1pct_low > 0.0) {
            if (has_first) DrawDot(scale);
            char buf[32];
            snprintf(buf, sizeof(buf), TR("1%%: %.0f", "1%%: %.0f"), s_cached_1pct_low);
            OSDTextColored(ColPerf(), buf);
            has_first = true;
        }

        if (g_config.osd_show_0_1pct_low && s_cached_01pct_low > 0.0) {
            if (has_first) DrawDot(scale);
            char buf[32];
            snprintf(buf, sizeof(buf), TR("0.1%%: %.0f", "0.1%%: %.0f"), s_cached_01pct_low);
            OSDTextColored(ColPerf(), buf);
        }
    }

    // ── Module 2: Quality & Smoothness (PQI) ──
    if (g_config.osd_show_pqi || g_config.osd_show_smoothness) {
        PQIScores pqi = {};
        if (g_config.osd_show_pqi || g_config.osd_show_pqi_breakdown)
            pqi = PQI_GetRolling();

        ImU32 pqi_accent = (pqi.pqi >= 90.0) ? BADGE_FPS : (pqi.pqi >= 70.0 ? BADGE_LAT : BADGE_GPU);
        DrawBadge("PQI", pqi_accent, scale, COL_W);

        bool has_first = false;
        if (g_config.osd_show_pqi) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0f%%", pqi.pqi);
            OSDTextColored(PQIColor(pqi.pqi / 100.0), buf);
            has_first = true;

            if (g_config.osd_show_pqi_breakdown) {
                char bbuf[64];
                snprintf(bbuf, sizeof(bbuf), TR(" (Cad: %.0f%% · Stut: %.0f%% · Ddl: %.0f%%)",
                                               " (节奏: %.0f%% · 卡顿: %.0f%% · 准时: %.0f%%)"),
                         pqi.cadence * 100.0, pqi.stutter * 100.0, pqi.deadline * 100.0);
                ImGui::SameLine(0.0f, 2.0f * scale);
                OSDTextColored(ImVec4(0.60f*b, 0.65f*b, 0.72f*b, 1.0f), bbuf);
            }
        }

        if (g_config.osd_show_smoothness) {
            double smooth = g_smoothness_us.load(std::memory_order_relaxed);
            if (smooth > 0.0) {
                if (has_first) DrawDot(scale);
                char buf[32];
                snprintf(buf, sizeof(buf), TR("Smooth: %.2f ms", "平滑: %.2f ms"), smooth / 1000.0);
                ImVec4 col = (smooth < 500.0) ? ImVec4(0.35f*b, 0.82f*b, 0.55f*b, 1.0f)
                           : (smooth < 1500.0 ? ImVec4(0.95f*b, 0.75f*b, 0.25f*b, 1.0f) : ImVec4(0.95f*b, 0.35f*b, 0.35f*b, 1.0f));
                OSDTextColored(col, buf);
            }
        }
    }

    // ── Module 3: GPU (AMD Radeon Crimson) ──
    bool has_gpu = (g_config.osd_show_gpu_usage && hw.gpu_usage_pct >= 0) ||
                   (g_config.osd_show_gpu_clock && hw.gpu_clock_mhz >= 0) ||
                   (g_config.osd_show_gpu_temp && hw.gpu_temp_c >= 0) ||
                   (g_config.osd_show_gpu_power && hw.gpu_power_w >= 0);

    if (has_gpu) {
        DrawBadge("GPU", BADGE_GPU, scale, COL_W);

        bool has_first = false;
        if (g_config.osd_show_gpu_usage && hw.gpu_usage_pct >= 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d%%", hw.gpu_usage_pct);
            OSDTextColored(ColSystem(), buf);
            has_first = true;
        }

        if (g_config.osd_show_gpu_clock && hw.gpu_clock_mhz >= 0) {
            if (has_first) DrawDot(scale);
            char buf[48];
            if (hw.gpu_mem_clock_mhz >= 0)
                snprintf(buf, sizeof(buf), "%d / %d MHz", hw.gpu_clock_mhz, hw.gpu_mem_clock_mhz);
            else
                snprintf(buf, sizeof(buf), "%d MHz", hw.gpu_clock_mhz);
            OSDTextColored(ColSystem(), buf);
            has_first = true;
        }

        if (g_config.osd_show_gpu_temp && hw.gpu_temp_c >= 0) {
            if (has_first) DrawDot(scale);
            char tbuf[96];
            if (hw.gpu_temp_hotspot_c >= 0 && hw.gpu_temp_mem_c >= 0) {
                snprintf(tbuf, sizeof(tbuf), "%d°C (Hot: %d°C, VRAM: %d°C)",
                         hw.gpu_temp_c, hw.gpu_temp_hotspot_c, hw.gpu_temp_mem_c);
            } else if (hw.gpu_temp_hotspot_c >= 0) {
                snprintf(tbuf, sizeof(tbuf), "%d°C (Hot: %d°C)",
                         hw.gpu_temp_c, hw.gpu_temp_hotspot_c);
            } else if (hw.gpu_temp_mem_c >= 0) {
                snprintf(tbuf, sizeof(tbuf), "%d°C (VRAM: %d°C)",
                         hw.gpu_temp_c, hw.gpu_temp_mem_c);
            } else {
                snprintf(tbuf, sizeof(tbuf), "%d°C", hw.gpu_temp_c);
            }

            int max_temp = hw.gpu_temp_c;
            if (hw.gpu_temp_hotspot_c > max_temp) max_temp = hw.gpu_temp_hotspot_c;
            if (hw.gpu_temp_mem_c > max_temp) max_temp = hw.gpu_temp_mem_c;

            ImVec4 col;
            if (max_temp < 75)       col = ImVec4(0.88f * b, 0.90f * b, 0.92f * b, 1.0f);
            else if (max_temp < 90)  col = ImVec4(0.95f * b, 0.75f * b, 0.25f * b, 1.0f);
            else                     col = ImVec4(0.95f * b, 0.35f * b, 0.35f * b, 1.0f);
            OSDTextColored(col, tbuf);
            has_first = true;
        }

        if (g_config.osd_show_gpu_power && hw.gpu_power_w >= 0) {
            if (has_first) DrawDot(scale);
            char buf[32];
            snprintf(buf, sizeof(buf), "%d W", hw.gpu_power_w);
            OSDTextColored(ColSystem(), buf);
            has_first = true;
        }
    }

    // ── Module 3b: VRAM (Lavender Mauve) ──
    if (g_config.osd_show_vram && hw.vram_used_mb >= 0 && hw.vram_total_mb > 0) {
        DrawBadge("VRAM", BADGE_VRAM, scale, COL_W);
        float usage = static_cast<float>(hw.vram_used_mb) / static_cast<float>(hw.vram_total_mb);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.1f / %.1f GB (%.0f%%)",
                 hw.vram_used_mb / 1024.0, hw.vram_total_mb / 1024.0, usage * 100.0f);
        ImVec4 col;
        if (usage > 0.90f)
            col = ImVec4(0.95f*b, 0.35f*b, 0.35f*b, 1.0f);
        else if (usage > 0.75f)
            col = ImVec4(0.95f*b, 0.75f*b, 0.25f*b, 1.0f);
        else
            col = ImVec4(0.88f*b, 0.90f*b, 0.92f*b, 1.0f);
        OSDTextColored(col, buf);
    }

    // ── Module 4: CPU (Nord Electric Blue) ──
    bool has_cpu = (g_config.osd_show_cpu_usage && hw.cpu_usage_pct >= 0) ||
                   (g_config.osd_show_cpu_clock && (hw.cpu_clock_mhz > 0 || hw.cpu_clock_p_mhz > 0)) ||
                   (g_config.osd_show_cpu_power && hw.cpu_power_w > 0);

    if (has_cpu) {
        DrawBadge("CPU", BADGE_CPU, scale, COL_W);

        bool has_first = false;
        if (g_config.osd_show_cpu_usage && hw.cpu_usage_pct >= 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%d%%", hw.cpu_usage_pct);
            OSDTextColored(ColSystem(), buf);
            has_first = true;
        }

        if (g_config.osd_show_cpu_clock) {
            char buf[64] = "";
            if (hw.cpu_is_hybrid && hw.cpu_clock_p_mhz > 0 && hw.cpu_clock_e_mhz > 0) {
                snprintf(buf, sizeof(buf), TR("P: %.1fG · E: %.1fG", "大核: %.1fG · 小核: %.1fG"),
                         hw.cpu_clock_p_mhz / 1000.0, hw.cpu_clock_e_mhz / 1000.0);
            } else if (hw.cpu_clock_mhz > 0) {
                snprintf(buf, sizeof(buf), "%.1f GHz", hw.cpu_clock_mhz / 1000.0);
            }
            if (buf[0] != '\0') {
                if (has_first) DrawDot(scale);
                OSDTextColored(ColSystem(), buf);
                has_first = true;
            }
        }

        if (g_config.osd_show_cpu_power && hw.cpu_power_w > 0) {
            if (has_first) DrawDot(scale);
            char buf[32];
            snprintf(buf, sizeof(buf), "%d W", hw.cpu_power_w);
            OSDTextColored(ColSystem(), buf);
            has_first = true;
        }
    }

    // ── Module 4b: RAM (Dusty Rose) ──
    if (g_config.osd_show_ram && hw.ram_used_mb >= 0 && hw.ram_total_mb > 0) {
        DrawBadge("RAM", BADGE_RAM, scale, COL_W);
        float usage = static_cast<float>(hw.ram_used_mb) / static_cast<float>(hw.ram_total_mb);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.1f / %.1f GB (%.0f%%)",
                 hw.ram_used_mb / 1024.0, hw.ram_total_mb / 1024.0, usage * 100.0f);
        OSDTextColored(ColSystem(), buf);
    }

    // ── Module 5: Latency & Bottleneck (Warm Amber) ──
#ifdef _WIN64
    double cpu_lat_us = g_config.osd_show_cpu_latency ? g_cpu_latency_us.load(std::memory_order_relaxed) : 0.0;
    double gpu_lat_us = 0.0;
    if (g_config.osd_show_gpu_render_time) {
        gpu_lat_us = g_telemetry_gpu_work_us.load(std::memory_order_relaxed);
        if (gpu_lat_us <= 0.0) gpu_lat_us = g_reflex_gpu_active_us.load(std::memory_order_relaxed);
        if (gpu_lat_us <= 0.0) gpu_lat_us = g_gpu_timer.GetLatestGpuTimeUs();
        if (gpu_lat_us <= 0.0) gpu_lat_us = g_measured_render_time_us.load(std::memory_order_relaxed);
    }
    double total_cost_us = g_config.osd_show_total_frame_cost ? g_reflex_total_frame_cost_us.load(std::memory_order_relaxed) : 0.0;

    bool has_lat = (cpu_lat_us > 0.0) || (gpu_lat_us > 0.0) || (total_cost_us > 0.0) || g_config.osd_show_bottleneck;

    if (has_lat) {
        DrawBadge("LAT", BADGE_LAT, scale, COL_W);

        bool has_first = false;
        if (gpu_lat_us > 0.0) {
            char buf[32];
            snprintf(buf, sizeof(buf), TR("GPU: %.1f ms", "显卡: %.1f ms"), gpu_lat_us / 1000.0);
            OSDTextColored(ColLatency(), buf);
            has_first = true;
        }

        if (cpu_lat_us > 0.0) {
            if (has_first) DrawDot(scale);
            char buf[32];
            snprintf(buf, sizeof(buf), TR("CPU: %.1f ms", "CPU: %.1f ms"), cpu_lat_us / 1000.0);
            OSDTextColored(ColLatency(), buf);
            has_first = true;
        }

        if (total_cost_us > 0.0) {
            if (has_first) DrawDot(scale);
            char buf[32];
            snprintf(buf, sizeof(buf), TR("Frame: %.1f ms", "整帧: %.1f ms"), total_cost_us / 1000.0);
            OSDTextColored(ColLatency(), buf);
            has_first = true;
        }

        if (g_config.osd_show_bottleneck) {
            if (has_first) DrawDot(scale);
            BottleneckKind bkind = g_bottleneck_kind.load(std::memory_order_relaxed);
            const char* bstr = (bkind == BottleneckKind::GpuBound) ? TR("GPU Bound", "显卡受限")
                             : ((bkind == BottleneckKind::CpuBound) ? TR("CPU Bound", "处理器受限") : TR("Balanced", "负载平衡"));
            ImU32 bcol = (bkind == BottleneckKind::GpuBound) ? BADGE_GPU
                       : ((bkind == BottleneckKind::CpuBound) ? BADGE_CPU : BADGE_API);
            DrawBadge(bstr, bcol, scale);
        }
    }

    // ── Module 6: Pipeline (Frame Gen / Limiter / Adaptive) ──
    int fg_div_raw = ComputeFGDivisorRaw();
    bool show_fg_active = g_config.osd_show_fg && fg_div_raw > 1;
    bool has_pipe = show_fg_active || g_config.osd_show_limiter || (g_config.osd_show_adaptive_smoothing && g_config.adaptive_smoothing);

    if (has_pipe) {
        DrawBadge("PIPE", BADGE_PIPE, scale, COL_W);

        bool has_first = false;
        if (show_fg_active) {
            char buf[32];
            snprintf(buf, sizeof(buf), "FG %dx", fg_div_raw);
            OSDTextColored(ColPipeline(), buf);
            has_first = true;
        }

        if (g_config.osd_show_limiter) {
            if (has_first) DrawDot(scale);
            int tier = static_cast<int>(g_current_tier);
            double added_ms = g_last_gate_sleep_us.load(std::memory_order_relaxed) / 1000.0;
            char buf[48];
            snprintf(buf, sizeof(buf), TR("Limiter: +%.1f ms (T%d)", "限帧: +%.1f ms (T%d)"), added_ms, tier);
            OSDTextColored(ColPipeline(), buf);
            has_first = true;
        }

        if (g_config.osd_show_adaptive_smoothing && g_config.adaptive_smoothing && g_adaptive_smoothing.IsWarm()) {
            if (has_first) DrawDot(scale);
            double offset = g_smoothing_offset_us.load(std::memory_order_relaxed);
            char buf[48];
            snprintf(buf, sizeof(buf), TR("Adaptive: +%.0f us", "平滑: +%.0f us"), offset);
            OSDTextColored(ColPipeline(), buf);
        }
    }
#endif

    // ── Module 7: Display & API ──
    if (g_config.osd_show_api || g_config.osd_show_resolution || g_config.osd_show_flip_model) {
        DrawBadge("API", BADGE_API, scale, COL_W);

        bool has_first = false;
        if (g_config.osd_show_api) {
            ActiveAPI api = SwapMgr_GetActiveAPI();
            const char* api_name = (api == ActiveAPI::DX12) ? "DX12" : (api == ActiveAPI::DX11 ? "DX11" : (api == ActiveAPI::Vulkan ? "Vulkan" : (api == ActiveAPI::OpenGL ? "OpenGL" : "None")));
            const char* arch = (sizeof(void*) == 8) ? "64-bit" : "32-bit";
            char buf[48];
            snprintf(buf, sizeof(buf), "%s %s", api_name, arch);
            OSDTextColored(ColPipeline(), buf);
            has_first = true;
        }

        if (g_config.osd_show_resolution) {
            uint32_t rw = 0, rh = 0;
            SwapMgr_GetResolution(rw, rh);
            if (rw > 0 && rh > 0) {
                if (has_first) DrawDot(scale);
                char buf[32];
                snprintf(buf, sizeof(buf), "%ux%u", rw, rh);
                OSDTextColored(ColPipeline(), buf);
                has_first = true;
            }
        }

        if (g_config.osd_show_flip_model) {
            if (has_first) DrawDot(scale);
            OSDTextColored(ColPipeline(), SwapMgr_GetPresentationModelString());
        }
    }

    // ── Module 8: Status (REC / Baseline) ──
    if (CSV_IsEnabled()) {
        DrawBadge("REC", BADGE_REC, scale, COL_W);
        OSDTextColored(ColStatus(), TR("Recording", "录制中"));
    }
    if (Baseline_IsCapturing() || Baseline_IsComparison()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f%%", Baseline_GetProgress() * 100.0);
        DrawBadge("BASE", BADGE_BASE, scale, COL_W);
        OSDTextColored(ColStatus(), buf);
    }
}

static void DrawModernBarOSD(float scale, double ft_ms) {
    const HWMonitorData& hw = HWMonitor_GetData();
    bool has_prev = false;

    // Module 1: FPS
    if (g_config.osd_show_fps) {
        DrawBadge("FPS", BADGE_FPS, scale);
        double output = s_display_output_fps;
        int fg_div_raw = ComputeFGDivisorRaw();
        char buf[64];
        if (output > 0.0 && fg_div_raw > 1) {
            snprintf(buf, sizeof(buf), "%.0f (%.1f)", output, s_display_fps);
        } else {
            snprintf(buf, sizeof(buf), "%.1f", (output > 0.0 && g_user_target_fps.load(std::memory_order_relaxed) <= 0) ? output : s_display_fps);
        }
        OSDTextColored(ColPerf(), buf);

        if (g_config.osd_show_frametime) {
            DrawDot(scale);
            char ft_buf[24];
            snprintf(ft_buf, sizeof(ft_buf), "%.1fms", ft_ms);
            OSDTextColored(ColPerf(), ft_buf);
        }
        if (g_config.osd_show_1pct_low && s_cached_1pct_low > 0.0) {
            DrawDot(scale);
            char l_buf[24];
            snprintf(l_buf, sizeof(l_buf), "1%% %.0f", s_cached_1pct_low);
            OSDTextColored(ColPerf(), l_buf);
        }
        has_prev = true;
    }

    // Module 2: GPU
    if ((g_config.osd_show_gpu_usage && hw.gpu_usage_pct >= 0) || (g_config.osd_show_gpu_temp && hw.gpu_temp_c >= 0)) {
        if (has_prev) DrawBarDivider(scale);
        DrawBadge("GPU", BADGE_GPU, scale);
        char gbuf[96];
        char tbuf[48] = "";
        if (g_config.osd_show_gpu_temp && hw.gpu_temp_c >= 0) {
            if (hw.gpu_temp_mem_c >= 0) {
                snprintf(tbuf, sizeof(tbuf), " · %d°C (Mem %d°C)", hw.gpu_temp_c, hw.gpu_temp_mem_c);
            } else if (hw.gpu_temp_hotspot_c >= 0) {
                snprintf(tbuf, sizeof(tbuf), " · %d°C (Hot %d°C)", hw.gpu_temp_c, hw.gpu_temp_hotspot_c);
            } else {
                snprintf(tbuf, sizeof(tbuf), " · %d°C", hw.gpu_temp_c);
            }
        }
        if (hw.gpu_usage_pct >= 0) {
            snprintf(gbuf, sizeof(gbuf), "%d%%%s", hw.gpu_usage_pct, tbuf);
        } else if (tbuf[0] != '\0') {
            snprintf(gbuf, sizeof(gbuf), "%s", tbuf + 3);
        }
        OSDTextColored(ColSystem(), gbuf);
        if (g_config.osd_show_gpu_power && hw.gpu_power_w > 0) {
            DrawDot(scale);
            char p_buf[16];
            snprintf(p_buf, sizeof(p_buf), "%dW", hw.gpu_power_w);
            OSDTextColored(ColSystem(), p_buf);
        }
        has_prev = true;
    }

    // Module 3: VRAM
    if (g_config.osd_show_vram && hw.vram_used_mb >= 0) {
        if (has_prev) DrawBarDivider(scale);
        char vbuf[32];
        snprintf(vbuf, sizeof(vbuf), "VRAM %.1fG", hw.vram_used_mb / 1024.0);
        OSDTextColored(ColSystem(), vbuf);
        has_prev = true;
    }

    // Module 4: CPU
    if ((g_config.osd_show_cpu_usage && hw.cpu_usage_pct >= 0) || (g_config.osd_show_cpu_clock && (hw.cpu_clock_mhz > 0 || hw.cpu_clock_p_mhz > 0))) {
        if (has_prev) DrawBarDivider(scale);
        DrawBadge("CPU", BADGE_CPU, scale);
        char cbuf[64];
        if (hw.cpu_is_hybrid && hw.cpu_clock_p_mhz > 0 && hw.cpu_clock_e_mhz > 0) {
            snprintf(cbuf, sizeof(cbuf), "%d%% · P%.1fG/E%.1fG", hw.cpu_usage_pct, hw.cpu_clock_p_mhz / 1000.0, hw.cpu_clock_e_mhz / 1000.0);
        } else if (hw.cpu_usage_pct >= 0 && hw.cpu_clock_mhz > 0) {
            snprintf(cbuf, sizeof(cbuf), "%d%% · %.1fG", hw.cpu_usage_pct, hw.cpu_clock_mhz / 1000.0);
        } else if (hw.cpu_usage_pct >= 0) {
            snprintf(cbuf, sizeof(cbuf), "%d%%", hw.cpu_usage_pct);
        } else {
            snprintf(cbuf, sizeof(cbuf), "%.1f GHz", hw.cpu_clock_mhz / 1000.0);
        }
        OSDTextColored(ColSystem(), cbuf);
        if (g_config.osd_show_cpu_power && hw.cpu_power_w > 0) {
            DrawDot(scale);
            char cp_buf[16];
            snprintf(cp_buf, sizeof(cp_buf), "%dW", hw.cpu_power_w);
            OSDTextColored(ColSystem(), cp_buf);
        }
        has_prev = true;
    }

    // Module 5: RAM
    if (g_config.osd_show_ram && hw.ram_used_mb >= 0) {
        if (has_prev) DrawBarDivider(scale);
        char rbuf[32];
        snprintf(rbuf, sizeof(rbuf), "RAM %.1fG", hw.ram_used_mb / 1024.0);
        OSDTextColored(ColSystem(), rbuf);
        has_prev = true;
    }

    // Module 6: PQI
    if (g_config.osd_show_pqi) {
        if (has_prev) DrawBarDivider(scale);
        PQIScores pqi = PQI_GetRolling();
        char pbuf[24];
        snprintf(pbuf, sizeof(pbuf), "PQI %.0f%%", pqi.pqi);
        OSDTextColored(PQIColor(pqi.pqi / 100.0), pbuf);
        has_prev = true;
    }

    // Module 7: Bottleneck
    if (g_config.osd_show_bottleneck) {
        if (has_prev) DrawBarDivider(scale);
        BottleneckKind bkind = g_bottleneck_kind.load(std::memory_order_relaxed);
        const char* bstr = (bkind == BottleneckKind::GpuBound) ? "GPU"
                         : ((bkind == BottleneckKind::CpuBound) ? "CPU" : "OK");
        ImU32 bcol = (bkind == BottleneckKind::GpuBound) ? BADGE_GPU
                   : ((bkind == BottleneckKind::CpuBound) ? BADGE_CPU : BADGE_API);
        DrawBadge(bstr, bcol, scale);
    }
}

static void DrawClassicOSD(float scale, double ft_ms) {
    // ═══════════════════════════════════
    // PERFORMANCE (cyan)
    // ═══════════════════════════════════
    if (g_config.osd_show_fps) {
        double output = s_display_output_fps;
        int fg_div_raw = ComputeFGDivisorRaw();
        char buf[64];
        if (output > 0.0 && fg_div_raw > 1) {
            snprintf(buf, sizeof(buf), TR("%.0f fps (%.1f render)", "%.0f FPS (%.1f 渲染)"), output, s_display_fps);
        } else {
            double show_fps = s_display_fps;
            if (g_user_target_fps.load(std::memory_order_relaxed) <= 0 && output > 0.0 && std::abs(show_fps - output) > 10.0) {
                show_fps = output;
            }
            snprintf(buf, sizeof(buf), TR("%.1f fps", "%.1f FPS"), show_fps);
        }
        OSDTextColored(ColPerf(), buf);
    }

    if (g_config.osd_show_1pct_low) {
        if (s_cached_1pct_low > 0.0) {
            char buf[32];
            snprintf(buf, sizeof(buf), TR("1%% Low: %.0f", "1%% 低帧: %.0f"), s_cached_1pct_low);
            OSDTextColored(ColPerf(), buf);
        }
    }

    if (g_config.osd_show_0_1pct_low) {
        if (s_cached_01pct_low > 0.0) {
            char buf[32];
            snprintf(buf, sizeof(buf), TR("0.1%% Low: %.0f", "0.1%% 低帧: %.0f"), s_cached_01pct_low);
            OSDTextColored(ColPerf(), buf);
        }
    }

    if (g_config.osd_show_frametime) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f ms", ft_ms);
        OSDTextColored(ColPerf(), buf);
    }

    // ═══════════════════════════════════
    // LATENCY (orange)
    // ═══════════════════════════════════
    if (g_config.osd_show_cpu_latency) {
        double cpu_us = g_cpu_latency_us.load(std::memory_order_relaxed);
        if (cpu_us > 0.0) {
            char buf[32];
            snprintf(buf, sizeof(buf), TR("CPU: %.1f ms", "CPU 提交: %.1f ms"), cpu_us / 1000.0);
            OSDTextColored(ColLatency(), buf);
        }
    }

    if (g_config.osd_show_gpu_render_time) {
        double gpu_us = g_telemetry_gpu_work_us.load(std::memory_order_relaxed);
        if (gpu_us <= 0.0) {
            gpu_us = g_reflex_gpu_active_us.load(std::memory_order_relaxed);
            if (gpu_us <= 0.0)
                gpu_us = g_gpu_timer.GetLatestGpuTimeUs();
            if (gpu_us <= 0.0)
                gpu_us = g_measured_render_time_us.load(std::memory_order_relaxed);
        }
        if (gpu_us > 0.0) {
            const char* tag = "HW";
            if (g_reflex_gpu_active_us.load(std::memory_order_relaxed) > 0.0)
                tag = "Reflex";
            else if (g_gpu_timer.IsSupported())
                tag = g_gpu_timer.GetSourceTag();

            char buf[64];
            snprintf(buf, sizeof(buf), TR("GPU (HW): %.1f ms [%s]", "显卡耗时 (HW): %.1f ms [%s]"),
                     gpu_us / 1000.0, tag);
            OSDTextColored(ColLatency(), buf);
        }
    }

    if (g_config.osd_show_total_frame_cost) {
        double cost_us = g_reflex_total_frame_cost_us.load(std::memory_order_relaxed);
        if (cost_us > 0.0) {
            char buf[32];
            snprintf(buf, sizeof(buf), TR("Frame: %.1f ms", "整帧耗时: %.1f ms"), cost_us / 1000.0);
            OSDTextColored(ColLatency(), buf);
        }
    }

    // ═══════════════════════════════════
    // QUALITY (green/yellow/red)
    // ═══════════════════════════════════
    PQIScores pqi = {};
    if (g_config.osd_show_pqi || g_config.osd_show_pqi_breakdown)
        pqi = PQI_GetRolling();

    if (g_config.osd_show_pqi) {
        ImVec4 pqi_color = PQIColor(pqi.pqi / 100.0);
        char buf[32];
        snprintf(buf, sizeof(buf), TR("PQI: %.0f%%", "PQI 评分: %.0f%%"), pqi.pqi);
        OSDTextColored(pqi_color, buf);
    }

    if (g_config.osd_show_pqi && g_config.osd_show_pqi_breakdown) {
        char buf[64];
        snprintf(buf, sizeof(buf), TR(" Cadence: %.0f%%", " 节奏稳态: %.0f%%"), pqi.cadence * 100.0);
        OSDTextColored(PQIColor(pqi.cadence), buf);
        snprintf(buf, sizeof(buf), TR(" Stutter: %.0f%%", " 卡顿抑制: %.0f%%"), pqi.stutter * 100.0);
        OSDTextColored(PQIColor(pqi.stutter), buf);
        snprintf(buf, sizeof(buf), TR(" Deadline: %.0f%%", " 准时达成: %.0f%%"), pqi.deadline * 100.0);
        OSDTextColored(PQIColor(pqi.deadline), buf);
    }

    if (g_config.osd_show_smoothness) {
        double smooth = g_smoothness_us.load(std::memory_order_relaxed);
        if (smooth > 0.0) {
            char buf[32];
            snprintf(buf, sizeof(buf), TR("Smoothness: %.2f ms", "平滑度: %.2f ms"), smooth / 1000.0);
            ImVec4 col;
            float b = g_config.osd_text_brightness;
            if (smooth < 500.0)       col = ImVec4(0.35f*b, 0.82f*b, 0.55f*b, 1.0f);
            else if (smooth < 1500.0) col = ImVec4(0.95f*b, 0.75f*b, 0.25f*b, 1.0f);
            else                      col = ImVec4(0.95f*b, 0.35f*b, 0.35f*b, 1.0f);
            OSDTextColored(col, buf);
        }
    }

    // ═══════════════════════════════════
    // PIPELINE (light blue)
    // ═══════════════════════════════════
#ifdef _WIN64
    int tier = static_cast<int>(g_current_tier);
    double limiter_added_ms = g_last_gate_sleep_us.load(std::memory_order_relaxed) / 1000.0;
    int fg_div_raw = ComputeFGDivisorRaw();
    if (g_config.osd_show_fg && fg_div_raw > 1) {
        char buf[48];
        snprintf(buf, sizeof(buf), TR("FG: %dx", "插帧: %dx"), fg_div_raw);
        OSDTextColored(ColPipeline(), buf);
    }

    if (g_config.osd_show_limiter) {
        char buf[48];
        snprintf(buf, sizeof(buf), TR("Limiter: +%.1f ms  T%d", "限帧延迟: +%.1f ms  T%d"), limiter_added_ms, tier);
        OSDTextColored(ColPipeline(), buf);
    }

    if (g_config.osd_show_adaptive_smoothing && g_config.adaptive_smoothing) {
        double offset = g_smoothing_offset_us.load(std::memory_order_relaxed);
        double p99 = g_smoothing_p99_us.load(std::memory_order_relaxed);
        if (g_adaptive_smoothing.IsWarm()) {
            const char* mode = g_adaptive_smoothing.dual_mode ? TR("Dual", "双重") : TR("Med", "中位");
            double pct = g_adaptive_smoothing.target_percentile * 100.0;
            char buf[96];
            snprintf(buf, sizeof(buf), TR("Adaptive: +%.0f us (P%.0f: %.1f ms) [%s]",
                                         "自适应平滑: +%.0f us (P%.0f: %.1f ms) [%s]"),
                     offset, pct, p99 / 1000.0, mode);
            float b = g_config.osd_text_brightness;
            ImVec4 col = (offset < 100.0)
                ? ImVec4(0.35f*b, 0.82f*b, 0.55f*b, 1.0f)
                : ImVec4(0.95f*b, 0.75f*b, 0.25f*b, 1.0f);
            OSDTextColored(col, buf);
        }
    }

    // Bottleneck state (GPU Bound / CPU Bound)
    if (g_config.osd_show_bottleneck) {
        BottleneckKind bkind = g_bottleneck_kind.load(std::memory_order_relaxed);
        const char* bstr = (bkind == BottleneckKind::GpuBound)
            ? TR("GPU Bound", "显卡受限 (GPU)")
            : ((bkind == BottleneckKind::CpuBound)
                ? TR("CPU Bound", "处理器受限 (CPU)")
                : TR("Balanced", "负载平衡"));
        char buf[64];
        snprintf(buf, sizeof(buf), TR("Bottleneck: %s", "硬件瓶颈: %s"), bstr);
        float b = g_config.osd_text_brightness;
        ImVec4 bcol = (bkind == BottleneckKind::GpuBound)
            ? ImVec4(0.95f*b, 0.35f*b, 0.35f*b, 1.0f)
            : ((bkind == BottleneckKind::CpuBound)
                ? ImVec4(0.35f*b, 0.65f*b, 0.95f*b, 1.0f)
                : ImVec4(0.70f*b, 0.75f*b, 0.80f*b, 1.0f));
        OSDTextColored(bcol, buf);
    }

    if (g_config.osd_show_cpu_work) {
        double cpu_us = g_telemetry_cpu_work_us.load(std::memory_order_relaxed);
        char buf[48];
        snprintf(buf, sizeof(buf), TR("CPU Work: %.1f ms", "CPU 真实耗时: %.1f ms"), cpu_us / 1000.0);
        OSDTextColored(ColLatency(), buf);
    }
#endif

    // ═══════════════════════════════════
    // DISPLAY & API (light blue)
    // ═══════════════════════════════════
    if (g_config.osd_show_api) {
        ActiveAPI api = SwapMgr_GetActiveAPI();
        const char* api_name = "None";
        if (api == ActiveAPI::DX12) api_name = "DX12";
        else if (api == ActiveAPI::DX11) api_name = "DX11";
        else if (api == ActiveAPI::DX9) api_name = "DX9";
        else if (api == ActiveAPI::Vulkan) api_name = "Vulkan";
        else if (api == ActiveAPI::OpenGL) api_name = "OpenGL";

        const char* arch = (sizeof(void*) == 8) ? "64-bit" : "32-bit";
        char buf[64];
        snprintf(buf, sizeof(buf), TR("API: %s (%s)", "接口: %s (%s)"), api_name, arch);
        OSDTextColored(ColPipeline(), buf);
    }

    if (g_config.osd_show_resolution) {
        uint32_t rw = 0, rh = 0;
        SwapMgr_GetResolution(rw, rh);
        char buf[64];
        if (rw > 0 && rh > 0) {
            snprintf(buf, sizeof(buf), TR("Res: %ux%u", "分辨率: %ux%u"), rw, rh);
        } else {
            snprintf(buf, sizeof(buf), TR("Res: N/A", "分辨率: 未知"));
        }
        OSDTextColored(ColPipeline(), buf);
    }

    if (g_config.osd_show_flip_model) {
        const char* flip_str = SwapMgr_GetPresentationModelString();
        char buf[128];
        snprintf(buf, sizeof(buf), TR("Flip Model: %s", "翻转模式: %s"), flip_str);
        OSDTextColored(ColPipeline(), buf);
    }

    // ═══════════════════════════════════
    // SYSTEM (grey — hardware sensors)
    // ═══════════════════════════════════
    {
        const HWMonitorData& hw = HWMonitor_GetData();

        if (g_config.osd_show_gpu_temp && hw.gpu_temp_c >= 0) {
            char buf[96];
            if (hw.gpu_temp_hotspot_c >= 0 && hw.gpu_temp_mem_c >= 0) {
                snprintf(buf, sizeof(buf), TR("GPU: %d C (Hot: %d C, VRAM: %d C)",
                                             "显卡温度: %d C (热点: %d C, 显存: %d C)"),
                         hw.gpu_temp_c, hw.gpu_temp_hotspot_c, hw.gpu_temp_mem_c);
            } else if (hw.gpu_temp_hotspot_c >= 0) {
                snprintf(buf, sizeof(buf), TR("GPU: %d C (Hot: %d C)",
                                             "显卡温度: %d C (热点: %d C)"),
                         hw.gpu_temp_c, hw.gpu_temp_hotspot_c);
            } else if (hw.gpu_temp_mem_c >= 0) {
                snprintf(buf, sizeof(buf), TR("GPU: %d C (VRAM: %d C)",
                                             "显卡温度: %d C (显存: %d C)"),
                         hw.gpu_temp_c, hw.gpu_temp_mem_c);
            } else {
                snprintf(buf, sizeof(buf), TR("GPU Temp: %d C", "显卡温度: %d C"), hw.gpu_temp_c);
            }
            int max_temp = hw.gpu_temp_c;
            if (hw.gpu_temp_hotspot_c > max_temp) max_temp = hw.gpu_temp_hotspot_c;
            if (hw.gpu_temp_mem_c > max_temp) max_temp = hw.gpu_temp_mem_c;

            ImVec4 col;
            float b = g_config.osd_text_brightness;
            if (max_temp < 75)       col = ImVec4(0.88f*b, 0.90f*b, 0.92f*b, 1.0f);
            else if (max_temp < 90)  col = ImVec4(0.95f*b, 0.75f*b, 0.25f*b, 1.0f);
            else                     col = ImVec4(0.95f*b, 0.35f*b, 0.35f*b, 1.0f);
            OSDTextColored(col, buf);
        }

        if (g_config.osd_show_gpu_clock && hw.gpu_clock_mhz >= 0) {
            char buf[48];
            if (hw.gpu_mem_clock_mhz >= 0)
                snprintf(buf, sizeof(buf), TR("GPU Clock: %d / %d MHz", "显卡频率: %d / %d MHz"), hw.gpu_clock_mhz, hw.gpu_mem_clock_mhz);
            else
                snprintf(buf, sizeof(buf), TR("GPU Clock: %d MHz", "显卡频率: %d MHz"), hw.gpu_clock_mhz);
            OSDTextColored(ColSystem(), buf);
        }

        if (g_config.osd_show_gpu_usage && hw.gpu_usage_pct >= 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), TR("GPU Load: %d%%", "显卡占用: %d%%"), hw.gpu_usage_pct);
            OSDTextColored(ColSystem(), buf);
        }

        if (g_config.osd_show_gpu_power && hw.gpu_power_w >= 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), TR("GPU Power: %dW", "显卡功耗: %dW"), hw.gpu_power_w);
            OSDTextColored(ColSystem(), buf);
        }

        if (g_config.osd_show_vram && hw.vram_used_mb >= 0 && hw.vram_total_mb > 0) {
            char buf[48];
            snprintf(buf, sizeof(buf), TR("VRAM: %lld / %lld MB", "显存占用: %lld / %lld MB"),
                     (long long)hw.vram_used_mb, (long long)hw.vram_total_mb);
            float usage = static_cast<float>(hw.vram_used_mb) / static_cast<float>(hw.vram_total_mb);
            float b = g_config.osd_text_brightness;
            ImVec4 col;
            if (usage > 0.90f)
                col = ImVec4(0.95f*b, 0.35f*b, 0.35f*b, 1.0f);
            else if (usage > 0.75f)
                col = ImVec4(0.95f*b, 0.75f*b, 0.25f*b, 1.0f);
            else
                col = ImVec4(0.88f*b, 0.90f*b, 0.92f*b, 1.0f);
            OSDTextColored(col, buf);
        }

        if (g_config.osd_show_cpu_usage && hw.cpu_usage_pct >= 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), TR("CPU Load: %d%%", "CPU 占用: %d%%"), hw.cpu_usage_pct);
            OSDTextColored(ColSystem(), buf);
        }

        if (g_config.osd_show_cpu_clock) {
            char buf[64] = "";
            if (hw.cpu_is_hybrid && hw.cpu_clock_p_mhz > 0 && hw.cpu_clock_e_mhz > 0) {
                snprintf(buf, sizeof(buf), TR("CPU Clock: P: %d MHz  E: %d MHz", "CPU 频率: 大核 %d MHz  小核 %d MHz"),
                         hw.cpu_clock_p_mhz, hw.cpu_clock_e_mhz);
            } else if (hw.cpu_clock_mhz > 0) {
                snprintf(buf, sizeof(buf), TR("CPU Clock: %d MHz", "CPU 频率: %d MHz"), hw.cpu_clock_mhz);
            }
            if (buf[0] != '\0') {
                OSDTextColored(ColSystem(), buf);
            }
        }

        if (g_config.osd_show_cpu_power && hw.cpu_power_w > 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), TR("CPU Power: %d W", "CPU 功耗: %d W"), hw.cpu_power_w);
            OSDTextColored(ColSystem(), buf);
        }

        if (g_config.osd_show_ram && hw.ram_used_mb >= 0 && hw.ram_total_mb > 0) {
            char buf[48];
            snprintf(buf, sizeof(buf), TR("RAM: %.1f / %.1f GB", "内存占用: %.1f / %.1f GB"),
                     hw.ram_used_mb / 1024.0, hw.ram_total_mb / 1024.0);
            OSDTextColored(ColSystem(), buf);
        }
    }

    if (CSV_IsEnabled())
        OSDTextColored(ColStatus(), TR("REC", "录制中"));

    if (Baseline_IsCapturing()) {
        char buf[32];
        snprintf(buf, sizeof(buf), TR("Baseline: %.0f%%", "基准记录: %.0f%%"), Baseline_GetProgress() * 100.0);
        OSDTextColored(ColStatus(), buf);
    } else if (Baseline_IsComparison()) {
        char buf[32];
        snprintf(buf, sizeof(buf), TR("Compare: %.0f%%", "基准对比: %.0f%%"), Baseline_GetProgress() * 100.0);
        OSDTextColored(ColStatus(), buf);
    }
}

void DrawOSD(reshade::api::effect_runtime* /*rt*/) {
    // ── Keybind polling (runs every frame via reshade_overlay event) ──
    // Uses GetAsyncKeyState for the main key (works regardless of UI state)
    // and for modifiers. Edge-triggered on press→release transition to avoid
    // repeat toggles while held.
    {
        static bool s_was_pressed = false;
        static std::string s_last_toggle_str;
        static ParsedKeybind s_cached_toggle_kb;
        ParsedKeybind kb = GetCachedKeybind(g_config.osd_toggle_key, s_last_toggle_str, s_cached_toggle_kb);
        if (kb.vk != 0) {
            bool key_down = (GetAsyncKeyState(kb.vk) & 0x8000) != 0;

            // For modifiers, check both left and right variants explicitly
            // GetAsyncKeyState(VK_CONTROL) can miss if only checked once per frame
            bool ctrl_held  = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) ||
                              (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
            bool alt_held   = (GetAsyncKeyState(VK_LMENU) & 0x8000) ||
                              (GetAsyncKeyState(VK_RMENU) & 0x8000);
            bool shift_held = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) ||
                              (GetAsyncKeyState(VK_RSHIFT) & 0x8000);

            bool mods_ok = true;
            if (kb.ctrl  && !ctrl_held)  mods_ok = false;
            if (kb.alt   && !alt_held)   mods_ok = false;
            if (kb.shift && !shift_held) mods_ok = false;

            // Also reject if extra modifiers are held that weren't in the bind
            // (prevents Ctrl+P from firing when just P is bound)
            if (!kb.ctrl  && ctrl_held)  mods_ok = false;
            if (!kb.alt   && alt_held)   mods_ok = false;
            if (!kb.shift && shift_held) mods_ok = false;

            bool pressed = key_down && mods_ok;
            if (pressed && !s_was_pressed) {
                g_config.osd_enabled = !g_config.osd_enabled;
            }
            s_was_pressed = pressed;
        }
    }

    // ── Preset cycling keybind polling ──
    {
        static bool s_prev_pressed = false;
        static bool s_next_pressed = false;
        static int s_active_preset = -1;  // -1 = no preset selected via keybind yet

        // Count total presets: 3 built-in + occupied user presets
        int total_presets = 3;
        for (int i = 0; i < OSDPreset_GetCount(); i++)
            if (OSDPreset_GetSlot(i).occupied) total_presets++;

        static std::string s_last_prev_str, s_last_next_str;
        static ParsedKeybind s_cached_prev_kb, s_cached_next_kb;

        auto PollPresetKey = [](const std::string& key_str, std::string& cached_str, ParsedKeybind& cached_kb, bool& was_pressed) -> bool {
            if (key_str.empty()) return false;
            ParsedKeybind kb = GetCachedKeybind(key_str, cached_str, cached_kb);
            if (kb.vk == 0) return false;

            bool key_down = (GetAsyncKeyState(kb.vk) & 0x8000) != 0;
            bool ctrl_held  = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
            bool alt_held   = (GetAsyncKeyState(VK_LMENU) & 0x8000) || (GetAsyncKeyState(VK_RMENU) & 0x8000);
            bool shift_held = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_RSHIFT) & 0x8000);

            bool mods_ok = true;
            if (kb.ctrl && !ctrl_held) mods_ok = false;
            if (kb.alt && !alt_held) mods_ok = false;
            if (kb.shift && !shift_held) mods_ok = false;
            if (!kb.ctrl && ctrl_held) mods_ok = false;
            if (!kb.alt && alt_held) mods_ok = false;
            if (!kb.shift && shift_held) mods_ok = false;

            bool pressed = key_down && mods_ok;
            bool triggered = pressed && !was_pressed;
            was_pressed = pressed;
            return triggered;
        };

        // Apply preset by index: 0=Min, 1=Med, 2=Full, 3+=user presets
        auto ApplyCyclePreset = [](int idx) {
            if (idx == 0) {
                OSDPreset p = {};
                p.show_fps = true; p.show_frametime = true; p.show_gpu_temp = true;
                OSDPreset_ApplyTogglesOnly(p);
            } else if (idx == 1) {
                OSDPreset p = {};
                p.show_fps = true; p.show_1pct_low = true; p.show_frametime = true;
                p.show_frametime_graph = true; p.show_gpu_render_time = true;
                p.show_pqi = true; p.show_smoothness = true; p.show_fg = true;
                p.show_gpu_temp = true; p.show_gpu_usage = true; p.show_vram = true;
                OSDPreset_ApplyTogglesOnly(p);
            } else if (idx == 2) {
                OSDPreset p = {};
                p.show_fps = true; p.show_1pct_low = true; p.show_0_1pct_low = true;
                p.show_frametime = true; p.show_frametime_graph = true;
                p.show_cpu_latency = true; p.show_gpu_render_time = true;
                p.show_total_frame_cost = true; p.show_fg_time = true;
                p.show_pqi = true; p.show_pqi_breakdown = true;
                p.show_smoothness = true; p.show_fg = true; p.show_limiter = true;
                p.show_adaptive_smoothing = true; p.show_gpu_temp = true;
                p.show_gpu_clock = true; p.show_gpu_usage = true; p.show_gpu_power = true; p.show_vram = true;
                p.show_cpu_usage = true; p.show_ram = true;
                p.show_api = true; p.show_resolution = true; p.show_flip_model = true;
                p.show_dlss_quality = true; p.show_dlss_features = true;
                p.show_dlss_resolution = true; p.show_dlss_presets = true;
                p.show_dlss_versions = true;
                OSDPreset_ApplyTogglesOnly(p);
            } else {
                int user_idx = idx - 3;
                if (user_idx >= 0 && user_idx < OSDPreset_GetCount()) {
                    OSDPreset& slot = OSDPreset_GetSlot(user_idx);
                    if (slot.occupied)
                        OSDPreset_ApplyToConfig(slot);
                }
            }
            SaveConfig();
        };

        if (total_presets > 0) {
            if (PollPresetKey(g_config.osd_preset_prev_key, s_last_prev_str, s_cached_prev_kb, s_prev_pressed)) {
                s_active_preset = (s_active_preset <= 0) ? total_presets - 1 : s_active_preset - 1;
                LOG_INFO("OSD: Preset cycle prev -> %d/%d", s_active_preset, total_presets);
                ApplyCyclePreset(s_active_preset);
            }
            if (PollPresetKey(g_config.osd_preset_next_key, s_last_next_str, s_cached_next_kb, s_next_pressed)) {
                s_active_preset = (s_active_preset >= total_presets - 1) ? 0 : s_active_preset + 1;
                LOG_INFO("OSD: Preset cycle next -> %d/%d", s_active_preset, total_presets);
                ApplyCyclePreset(s_active_preset);
            }
        }
    }

    // ── Position cycle keybind polling ──
    {
        static bool s_pos_pressed = false;
        static std::string s_last_pos_str;
        static ParsedKeybind s_cached_pos_kb;
        if (!g_config.osd_position_cycle_key.empty()) {
            ParsedKeybind kb = GetCachedKeybind(g_config.osd_position_cycle_key, s_last_pos_str, s_cached_pos_kb);
            if (kb.vk != 0) {
                bool key_down = (GetAsyncKeyState(kb.vk) & 0x8000) != 0;
                bool ctrl_held  = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
                bool alt_held   = (GetAsyncKeyState(VK_LMENU) & 0x8000) || (GetAsyncKeyState(VK_RMENU) & 0x8000);
                bool shift_held = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_RSHIFT) & 0x8000);

                bool mods_ok = true;
                if (kb.ctrl && !ctrl_held) mods_ok = false;
                if (kb.alt && !alt_held) mods_ok = false;
                if (kb.shift && !shift_held) mods_ok = false;
                if (!kb.ctrl && ctrl_held) mods_ok = false;
                if (!kb.alt && alt_held) mods_ok = false;
                if (!kb.shift && shift_held) mods_ok = false;

                bool pressed = key_down && mods_ok;
                if (pressed && !s_pos_pressed) {
                    OSD_CyclePosition();
                    SaveConfig();
                    LOG_INFO("OSD: Position cycled to (%.3f, %.3f)", g_config.osd_x, g_config.osd_y);
                }
                s_pos_pressed = pressed;
            }
        }
    }

    // ── Blackout keybind polling ──
    {
        static bool s_blackout_pressed = false;
        static std::string s_last_blackout_str;
        static ParsedKeybind s_cached_blackout_kb;
        if (!g_config.blackout_key.empty()) {
            ParsedKeybind kb = GetCachedKeybind(g_config.blackout_key, s_last_blackout_str, s_cached_blackout_kb);
            if (kb.vk != 0) {
                bool key_down = (GetAsyncKeyState(kb.vk) & 0x8000) != 0;
                bool ctrl_held  = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
                bool alt_held   = (GetAsyncKeyState(VK_LMENU) & 0x8000) || (GetAsyncKeyState(VK_RMENU) & 0x8000);
                bool shift_held = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_RSHIFT) & 0x8000);

                bool mods_ok = true;
                if (kb.ctrl && !ctrl_held) mods_ok = false;
                if (kb.alt && !alt_held) mods_ok = false;
                if (kb.shift && !shift_held) mods_ok = false;
                if (!kb.ctrl && ctrl_held) mods_ok = false;
                if (!kb.alt && alt_held) mods_ok = false;
                if (!kb.shift && shift_held) mods_ok = false;

                bool pressed = key_down && mods_ok;
                if (pressed && !s_blackout_pressed)
                    Blackout_Toggle();
                s_blackout_pressed = pressed;
            }
        }
    }

    // ── OLED Care keybind polling ──
    {
        static bool s_oled_care_pressed = false;
        static std::string s_last_oled_str;
        static ParsedKeybind s_cached_oled_kb;
        if (!g_config.oled_care_key.empty()) {
            ParsedKeybind kb = GetCachedKeybind(g_config.oled_care_key, s_last_oled_str, s_cached_oled_kb);
            if (kb.vk != 0) {
                bool key_down = (GetAsyncKeyState(kb.vk) & 0x8000) != 0;
                bool ctrl_held  = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000);
                bool alt_held   = (GetAsyncKeyState(VK_LMENU) & 0x8000) || (GetAsyncKeyState(VK_RMENU) & 0x8000);
                bool shift_held = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_RSHIFT) & 0x8000);

                bool mods_ok = true;
                if (kb.ctrl && !ctrl_held) mods_ok = false;
                if (kb.alt && !alt_held) mods_ok = false;
                if (kb.shift && !shift_held) mods_ok = false;
                if (!kb.ctrl && ctrl_held) mods_ok = false;
                if (!kb.alt && alt_held) mods_ok = false;
                if (!kb.shift && shift_held) mods_ok = false;

                bool pressed = key_down && mods_ok;
                if (pressed && !s_oled_care_pressed)
                    OLEDCare_Toggle();
                s_oled_care_pressed = pressed;
            }
        }
    }

    if (!g_config.osd_enabled) return;

    // Update hardware sensors (throttled internally to ~1Hz)
    HWMonitor_Update();

    // Real FPS from enforcement-to-enforcement interval (CPU frames only)
    // EMA-smoothed so the OSD number is readable instead of flickering.
    double ft = g_actual_frame_time_us.load(std::memory_order_relaxed);
    if (ft > 0.0) {
        double instant_fps = 1000000.0 / ft;
        if (s_real_fps > 0.0) {
            double delta = std::abs(instant_fps - s_real_fps);
            double alpha = (delta > 20.0) ? 0.35 : (delta > 5.0 ? 0.15 : 0.05);
            s_real_fps += alpha * (instant_fps - s_real_fps);
        } else {
            s_real_fps = instant_fps;
        }
    }

    // Throttle FPS display update to 2x per second for readability
    {
        ULONGLONG now = GetTickCount64();
        if (now - s_fps_display_tick >= 500) {
            s_fps_display_tick = now;
            s_display_fps = s_real_fps;
            s_display_render_fps = s_real_fps;
            s_display_output_fps = g_output_fps.load(std::memory_order_relaxed);
        }
    }

    // Compute pixel position from screen percentage
    ImVec2 display = ImGui::GetIO().DisplaySize;
    float px_x = g_config.osd_x * display.x;
    float px_y = g_config.osd_y * display.y;
    // Anchor point flips based on position quadrant so the OSD stays within
    // screen bounds and sits flush against whichever edge it's near.
    float pivot_x = (g_config.osd_x > 0.5f) ? 1.0f : 0.0f;
    float pivot_y = (g_config.osd_y > 0.5f) ? 1.0f : 0.0f;

    // Prevent OSD from overflowing the screen boundary when scaled up
    static ImVec2 s_last_osd_size = {0.0f, 0.0f};
    if (s_last_osd_size.y > 0.0f && display.y > 0.0f) {
        constexpr float MARGIN = 8.0f;
        if (pivot_y == 0.0f && px_y + s_last_osd_size.y > display.y - MARGIN) {
            px_y = (std::max)(MARGIN, display.y - s_last_osd_size.y - MARGIN);
        }
        if (pivot_x == 0.0f && px_x + s_last_osd_size.x > display.x - MARGIN) {
            px_x = (std::max)(MARGIN, display.x - s_last_osd_size.x - MARGIN);
        }
    }

    bool is_modern = (g_config.osd_style == "card" || g_config.osd_style == "bar");
    float rounding = is_modern ? (g_config.osd_rounding * g_config.osd_scale) : 0.0f;
    float border_sz = is_modern ? 1.0f : 0.0f;
    ImVec2 padding = is_modern ? ImVec2(10.0f * g_config.osd_scale, 8.0f * g_config.osd_scale) : ImVec2(8.0f * g_config.osd_scale, 6.0f * g_config.osd_scale);
    ImVec2 spacing = is_modern ? ImVec2(6.0f * g_config.osd_scale, 4.0f * g_config.osd_scale) : ImVec2(8.0f * g_config.osd_scale, 3.0f * g_config.osd_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, border_sz);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.32f, 0.40f, 0.55f));

    ImGui::SetNextWindowPos({px_x, px_y}, ImGuiCond_Always, {pivot_x, pivot_y});
    ImGui::SetNextWindowBgAlpha(g_config.osd_opacity);

    if (ImGui::Begin("##limiter_osd", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoFocusOnAppearing)) {

        float scale = g_config.osd_scale;
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);

        // Always update frame time history for graph and 1% low
        double ft_ms = ft / 1000.0;
        s_ft_history[s_ft_history_idx] = static_cast<float>(ft_ms);
        s_ft_history_idx = (s_ft_history_idx + 1) % FT_HISTORY_SIZE;
        if (ft > 0.0) {
            double display_ft = ft;
            int divisor = ComputeFGDivisorRaw();
            if (divisor >= 2) {
                display_ft = ft / static_cast<double>(divisor);
            }
            // Feed 1% low history for valid frames (excluding pause/loading freezes >100ms)
            if (display_ft > 0.0 && display_ft <= 100000.0) {
                s_low_history[s_low_history_idx] = display_ft;
                s_low_history_idx = (s_low_history_idx + 1) % LOW_HISTORY_SIZE;
                if (s_low_history_count < LOW_HISTORY_SIZE) s_low_history_count++;
            }
        }

        // Recompute 1%/0.1% low every 30 frames (~5Hz) instead of every frame
        if (++s_low_recompute_counter >= 30) {
            s_low_recompute_counter = 0;
            RecomputeLowFPS();
        }

        // ── Layout Style Dispatcher ──
        if (g_config.osd_style == "bar") {
            DrawModernBarOSD(scale, ft_ms);
        } else if (g_config.osd_style == "card") {
            DrawModernCardOSD(scale, ft_ms);
        } else {
            DrawClassicOSD(scale, ft_ms);
        }

        // ═══════════════════════════════════
        // FRAMETIME GRAPH
        // ═══════════════════════════════════
        if (g_config.osd_show_frametime_graph) {
            float ordered[FT_HISTORY_SIZE];
            float data_max = 0.0f;
            float data_min = 1e9f;
            for (int i = 0; i < FT_HISTORY_SIZE; i++) {
                float v = s_ft_history[(s_ft_history_idx + i) % FT_HISTORY_SIZE];
                ordered[i] = v;
                if (v > 0.0f) {
                    if (v > data_max) data_max = v;
                    if (v < data_min) data_min = v;
                }
            }

            float current = static_cast<float>(ft_ms);
            float ideal_min = (std::max)(0.0f, current - 4.0f);
            float ideal_max = current + 4.0f;

            float needed_min = (std::min)(ideal_min, data_min - 1.0f);
            float needed_max = (std::max)(ideal_max, data_max + 1.0f);
            needed_min = (std::max)(0.0f, needed_min);

            if (needed_min < s_graph_scale_min) s_graph_scale_min = needed_min;
            if (needed_max > s_graph_scale_max) s_graph_scale_max = needed_max;

            s_graph_scale_min += (ideal_min - s_graph_scale_min) * 0.02f;
            s_graph_scale_max += (ideal_max - s_graph_scale_max) * 0.02f;
            s_graph_scale_min = (std::max)(0.0f, s_graph_scale_min);
            if (s_graph_scale_max < s_graph_scale_min + 2.0f)
                s_graph_scale_max = s_graph_scale_min + 2.0f;

            float scale_min = s_graph_scale_min;
            float scale_max = s_graph_scale_max;
            float scale_mid = (scale_min + scale_max) * 0.5f;

            float font_size = ImGui::GetFontSize();
            float graph_w = std::clamp(280.0f * scale, 180.0f, 600.0f);
            float graph_h = std::clamp(65.0f * scale, 35.0f, 150.0f);
            float label_w = ImGui::CalcTextSize("999").x + 6.0f;
            if (label_w < 32.0f * scale) label_w = 32.0f * scale;

            char top_label[16], mid_label[16], bot_label[16];
            snprintf(top_label, sizeof(top_label), "%.0f", scale_max);
            snprintf(mid_label, sizeof(mid_label), "%.0f", scale_mid);
            snprintf(bot_label, sizeof(bot_label), "%.0f", scale_min);

            ImVec2 cursor = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();

            if (is_modern) {
                // Subtle dark backdrop container for modern card look
                dl->AddRectFilled(
                    ImVec2(cursor.x - 2.0f, cursor.y - 2.0f),
                    ImVec2(cursor.x + label_w + graph_w + 2.0f, cursor.y + graph_h + 2.0f),
                    IM_COL32(10, 15, 20, 130),
                    4.0f * scale
                );
            }

            dl->AddText({cursor.x, cursor.y}, 0xFFCCCCCC, top_label);
            dl->AddText({cursor.x, cursor.y + (graph_h - font_size) * 0.5f}, 0xFFCCCCCC, mid_label);
            dl->AddText({cursor.x, cursor.y + graph_h - font_size}, 0xFFCCCCCC, bot_label);

            ImGui::SetCursorScreenPos({cursor.x + label_w, cursor.y});
            ImGui::PlotLines("##ft_graph", ordered, FT_HISTORY_SIZE,
                             0, nullptr, scale_min, scale_max,
                             ImVec2(graph_w, graph_h));

            // Target frame time guideline
            double user_fps = g_user_target_fps.load(std::memory_order_relaxed);
            if (g_config.osd_show_target_line && user_fps > 0.0) {
                float target_ft_ms = static_cast<float>(1000.0 / user_fps);
                if (target_ft_ms >= scale_min && target_ft_ms <= scale_max && (scale_max > scale_min)) {
                    float norm_y = 1.0f - ((target_ft_ms - scale_min) / (scale_max - scale_min));
                    float line_y = cursor.y + norm_y * graph_h;
                    dl->AddLine(
                        ImVec2(cursor.x + label_w, line_y),
                        ImVec2(cursor.x + label_w + graph_w, line_y),
                        IM_COL32(0, 229, 255, 150),
                        1.0f * scale
                    );
                }
            }

            // Reserve space for the entire graph block so auto-resize includes it completely
            ImGui::SetCursorScreenPos({cursor.x, cursor.y + graph_h + 4.0f * scale});
            ImGui::Dummy(ImVec2(label_w + graph_w, 2.0f * scale));
        }

        s_last_osd_size = ImGui::GetWindowSize();
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

void RegisterOSD() {
    reshade::register_overlay("ReLimiter", DrawSettings);
    reshade::register_event<reshade::addon_event::reshade_overlay>(DrawOSD);
}
