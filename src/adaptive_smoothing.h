#pragma once

#include <atomic>
#include <cstdint>
#include "bottleneck.h"

#ifdef _WIN64

#include "rolling_window.h"

// Adaptive smoothing: tracks bottleneck execution distribution and computes a P99-based
// offset to extend effective_interval, keeping frames within budget for optimal cadence consistency.
// Incorporates asymmetric attack/release, headroom streak fast release, and BBR negative gain headroom.

struct AdaptiveSmoothing {
    // Primary window (medium mode, 200 samples)
    RollingWindow<double, 200> primary;

    // Dual-window mode: short (64 frames) for fast tracking, long (200 frames) for stability
    RollingWindow<double, 64>  short_win;
    RollingWindow<double, 200> long_win;

    // Output state
    double smoothed_offset_us = 0.0;
    double raw_p99_us = 0.0;

    // Asymmetric EMA parameters
    double alpha_attack  = 0.25;
    double alpha_release = 0.08;
    int    fast_convergence_frames = 0;
    int    consecutive_outliers = 0;

    // Headroom streak tracking
    uint32_t headroom_streak = 0;

    // Configuration
    bool   dual_mode = true;
    double target_percentile = 0.99;
    bool   enabled = true;
    double queue_drain_margin_pct = 0.015; // 1.5% BBR negative-gain headroom
    bool   enable_fast_release = true;

    // Feed a bottleneck time sample (work_us) and compute the smoothing offset.
    // bottleneck_work_us: max(T_gpu, T_cpu) excluding self-sleep.
    // effective_interval_us: target interval before smoothing.
    // bottleneck_kind: whether GpuBound, CpuBound, or Unknown.
    // Returns the smoothed offset to add to effective_interval.
    double Update(double bottleneck_work_us, double effective_interval_us,
                  BottleneckKind bottleneck_kind = BottleneckKind::Unknown);

    // Soft reset: retain last 25% of samples, accelerate EMA for 32 frames.
    // Called on regime breaks and external state flushes.
    void SoftReset();

    // Full reset: clear all state. Called on FG/FPS change or full flush.
    void Reset();

    // Accessors for telemetry
    double GetOffset() const { return smoothed_offset_us; }
    double GetP99()    const { return raw_p99_us; }
    bool   IsWarm()    const { return primary.Size() >= 32; }

    // Apply config values
    void SetConfig(bool dual, double percentile, bool enable,
                   double drain_margin = 0.015, bool fast_rel = true);
};

#else // 32-bit stub

struct AdaptiveSmoothing {
    double smoothed_offset_us = 0.0;
    double raw_p99_us = 0.0;
    bool   dual_mode = false;
    double target_percentile = 0.99;
    bool   enabled = false;
    double queue_drain_margin_pct = 0.015;
    bool   enable_fast_release = true;

    double Update(double, double, BottleneckKind = BottleneckKind::Unknown) { return 0.0; }
    void SoftReset() {}
    void Reset() {}
    double GetOffset() const { return 0.0; }
    double GetP99()    const { return 0.0; }
    bool   IsWarm()    const { return false; }
    void SetConfig(bool, double, bool, double = 0.015, bool = true) {}
};

#endif // _WIN64

// Published for OSD consumption (relaxed atomics, written from render thread)
#ifdef _WIN64
extern std::atomic<double> g_smoothing_offset_us;
extern std::atomic<double> g_smoothing_p99_us;
extern AdaptiveSmoothing g_adaptive_smoothing;
#else
inline std::atomic<double> g_smoothing_offset_us{0.0};
inline std::atomic<double> g_smoothing_p99_us{0.0};
inline AdaptiveSmoothing g_adaptive_smoothing{};
#endif
