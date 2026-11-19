#include "enforcement_dispatcher.h"
#include "scheduler.h"
#include "vk_enforce.h"
#include "pcl_hooks.h"
#include "streamline_hooks.h"
#include "health.h"
#include "flush.h"
#include "wake_guard.h"
#include "logger.h"
#include <atomic>
#include <Windows.h>

// ── File-static state ──

static std::atomic<EnforcementPath> s_active_path{EnforcementPath::None};
static std::atomic<ActiveAPI>       s_prev_api{ActiveAPI::None};

// ── Output FPS (all presents including FG) ──
// Rolling 1-second window of present timestamps. FPS = count of presents
// in the window. Supports high-refresh and uncapped games up to 2000+ FPS.
std::atomic<double> g_output_fps{0.0};
static constexpr int MAX_PRESENT_TIMESTAMPS = 2048;
static int64_t s_present_ts[MAX_PRESENT_TIMESTAMPS] = {};
static int     s_present_head = 0;
static int     s_present_count = 0;

// ── Path selection logic ──

static EnforcementPath SelectPath(ActiveAPI api) {
    switch (api) {
        case ActiveAPI::DX12:
            // DX12 prefers NvAPI marker hooks for enforcement (Reflex games).
            // Once we've seen NvAPI markers, this is a Reflex game — stay on
            // the marker path even during temporary marker gaps (alt-tab,
            // loading screens). Falling back to present-based during gaps
            // causes double enforcement and FG coalescing issues on refocus.
            {
                static bool s_ever_seen_nvapi = false;
                if (AreNvAPIMarkersFlowing())
                    s_ever_seen_nvapi = true;
                if (s_ever_seen_nvapi)
                    return EnforcementPath::NvAPIMarkers;
                return EnforcementPath::PresentBased;
            }

        case ActiveAPI::Vulkan:
            if (PCL_MarkersFlowing())
                return EnforcementPath::PCLMarkers;
            return EnforcementPath::PresentBased;

        case ActiveAPI::DX9:
        case ActiveAPI::DX11:
            return EnforcementPath::PresentBased;

        case ActiveAPI::OpenGL:
            return EnforcementPath::PresentBased;

        case ActiveAPI::None:
        default:
            return EnforcementPath::None;
    }
}

// ── Lifecycle ──

void EnfDisp_OnSwapchainInit(ActiveAPI api) {
    ActiveAPI prev = s_prev_api.load(std::memory_order_relaxed);

    // On API change: flush all scheduler state before re-evaluating path (Req 6.5)
    if (prev != ActiveAPI::None && prev != api) {
        LOG_INFO("EnfDisp: API changed %d -> %d, flushing scheduler state",
                 static_cast<int>(prev), static_cast<int>(api));
        Flush(FLUSH_ALL);
    }

    s_prev_api.store(api, std::memory_order_relaxed);

    EnforcementPath path = SelectPath(api);
    s_active_path.store(path, std::memory_order_relaxed);

    LOG_INFO("EnfDisp: init api=%d path=%d", static_cast<int>(api), static_cast<int>(path));
}

void EnfDisp_OnSwapchainDestroy(bool full_teardown) {
    if (full_teardown) {
        s_active_path.store(EnforcementPath::None, std::memory_order_relaxed);
        // Don't clear s_prev_api — we need it to detect API changes on next init
        LOG_INFO("EnfDisp: full teardown, path=None");
    } else {
        LOG_INFO("EnfDisp: resize teardown, path preserved=%d",
                 static_cast<int>(s_active_path.load(std::memory_order_relaxed)));
    }
}

// ── Present event routing ──

void EnfDisp_OnPresent(int64_t now_qpc) {
    // ── Output FPS + 1% low: rolling 1-second window of present timestamps ──
    {
        // Record this present
        s_present_ts[s_present_head] = now_qpc;
        s_present_head = (s_present_head + 1) % MAX_PRESENT_TIMESTAMPS;
        if (s_present_count < MAX_PRESENT_TIMESTAMPS)
            s_present_count++;

        // Collect timestamps within the last 1 second
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        int64_t one_sec = freq.QuadPart;
        int64_t cutoff = now_qpc - one_sec;

        int wcount = 0;
        for (int i = 0; i < s_present_count; i++) {
            if (s_present_ts[i] >= cutoff)
                wcount++;
        }

        // Output FPS: count of presents in the 1-second window
        g_output_fps.store(static_cast<double>(wcount), std::memory_order_relaxed);

        // Track frame time from real presents as ground truth when limiter is off
        static int64_t s_last_present_qpc_ft = 0;
        if (s_last_present_qpc_ft > 0) {
            double ft_us = static_cast<double>(now_qpc - s_last_present_qpc_ft) * 1000000.0 / static_cast<double>(freq.QuadPart);
            if (ft_us > 0.0 && g_user_target_fps.load(std::memory_order_relaxed) <= 0) {
                g_actual_frame_time_us.store(ft_us, std::memory_order_relaxed);
            }
        }
        s_last_present_qpc_ft = now_qpc;
    }

    // DMFG passthrough: skip enforcement dispatch, FPS counter already updated
    if (IsDmfgActive())
        return;

    // If path was set to None by a full teardown, don't re-evaluate.
    // A late present callback after destroy_swapchain must not resurrect
    // enforcement — the swapchain and all DXGI pointers are already freed.
    EnforcementPath current_path = s_active_path.load(std::memory_order_relaxed);
    if (current_path == EnforcementPath::None)
        return;

    // Re-evaluate path lazily on each present to pick up marker state changes.
    // This handles the case where PCL hooks install after the first present,
    // or NvAPI markers start flowing after swapchain init.
    ActiveAPI api = s_prev_api.load(std::memory_order_relaxed);
    EnforcementPath path = SelectPath(api);
    s_active_path.store(path, std::memory_order_relaxed);

    switch (path) {
        case EnforcementPath::PresentBased:
            // Delegate to VkEnforce_OnPresent which handles FG coalescing
            // and predictor feeding for present-based enforcement.
            VkEnforce_OnPresent(now_qpc);
            break;

        case EnforcementPath::NvAPIMarkers:
            // Marker-based: enforcement normally happens in Hook_SetLatencyMarker.
            // Fallback to present-based when:
            // 1. NvAPI markers stop flowing entirely (menus, loading, cutscenes)
            // 2. NvAPI markers are flowing but the enforcement marker type never
            //    fires (e.g., game sends RENDERSUBMIT_END but config expects
            //    SIMULATION_START). Without this, the scheduler never runs,
            //    deadlines go stale, and the presentation gate misbehaves.
            if (!AreNvAPIMarkersFlowing() || !AreMarkersFlowing())
                VkEnforce_OnPresent(now_qpc);
            break;

        case EnforcementPath::PCLMarkers:
            // Same fallback logic for Vulkan+Streamline PCL marker path.
            if (!PCL_MarkersFlowing())
                VkEnforce_OnPresent(now_qpc);
            break;

        case EnforcementPath::None:
        default:
            break;
    }
}

// ── Query ──

EnforcementPath EnfDisp_GetActivePath() {
    return s_active_path.load(std::memory_order_relaxed);
}
