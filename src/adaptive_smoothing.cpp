#include "adaptive_smoothing.h"
#include "logger.h"
#include <algorithm>
#include <cmath>

AdaptiveSmoothing g_adaptive_smoothing;
std::atomic<double> g_smoothing_offset_us{0.0};
std::atomic<double> g_smoothing_p99_us{0.0};

double AdaptiveSmoothing::Update(double bottleneck_work_us, double effective_interval_us,
                                 BottleneckKind bottleneck_kind) {
    if (!enabled || bottleneck_work_us <= 0.0 || effective_interval_us <= 0.0) {
        smoothed_offset_us = 0.0;
        return 0.0;
    }

    // Reject outliers: extreme stalls, scene transitions, shader compiles (>50ms or >3x current adapted interval)
    double current_adapted_us = effective_interval_us + smoothed_offset_us;
    double max_plausible_us = (std::max)(current_adapted_us * 3.0, 50000.0);
    if (bottleneck_work_us > max_plausible_us)
        return smoothed_offset_us;

    // Median-based outlier rejection: reject samples beyond 3x the median render time,
    // but only for frames taking > 33.3ms (below 30 FPS). Heavy frames taking 15-30ms
    // (47-66 FPS) are legitimate gameplay workload and must never be rejected.
    if (primary.Size() >= 32) {
        double median = primary.Percentile(0.5);
        if (bottleneck_work_us > 33333.0 && bottleneck_work_us > median * 3.0) {
            consecutive_outliers++;
            if (consecutive_outliers >= 4) {
                // Sustained workload shift / scene transition:
                consecutive_outliers = 0;
                SoftReset();
            } else {
                return smoothed_offset_us;
            }
        } else {
            consecutive_outliers = (consecutive_outliers > 0) ? (consecutive_outliers - 1) : 0;
        }
    }

    // Push to windows
    primary.Push(bottleneck_work_us);
    if (dual_mode) {
        short_win.Push(bottleneck_work_us);
        long_win.Push(bottleneck_work_us);
    }

    // Warmup guard: need >=32 samples for meaningful P99
    if (primary.Size() < 32) {
        smoothed_offset_us = 0.0;
        raw_p99_us = 0.0;
        return 0.0;
    }

    // Compute P99 (or configured percentile)
    double p99;
    if (dual_mode && short_win.Size() >= 16 && long_win.Size() >= 32) {
        double short_p = short_win.Percentile(target_percentile);
        double long_p  = long_win.Percentile(target_percentile);
        if (short_p > long_p) {
            // Fast attack: rising workload immediately tracked by short window
            p99 = short_p;
        } else {
            // Release / steady: if headroom streak confirms lower workload, track short window
            // to prevent lingering low FPS for 200 frames.
            p99 = (headroom_streak >= 20) ? short_p : long_p;
        }
    } else {
        p99 = primary.Percentile(target_percentile);
    }
    raw_p99_us = p99;

    // BBR negative-gain headroom: when GPU-bound, intentionally target slightly below
    // maximum GPU throughput (+1.5% interval margin) to continuously drain the GPU queue.
    double effective_p99 = p99;
    if (bottleneck_kind == BottleneckKind::GpuBound && queue_drain_margin_pct > 0.0) {
        effective_p99 = p99 * (1.0 + queue_drain_margin_pct);
    }

    // Raw offset: how much the effective P99 exceeds the target interval
    double raw_offset = (std::max)(0.0, effective_p99 - effective_interval_us);

    // Allow offset to extend interval up to 50ms (20 FPS) so adaptive smoothing
    // can gracefully track 47-50 FPS scenes even when VRR CAP is set to 144/165+ FPS.
    // Also support at least 6x base interval.
    double max_interval_us = (std::max)(effective_interval_us * 6.0, 50000.0);
    double max_offset = (std::max)(0.0, max_interval_us - effective_interval_us);
    raw_offset = (std::min)(raw_offset, max_offset);

    // Asymmetric EMA smoothing: fast attack (0.25) to prevent tear/stutter,
    // smooth release (0.08) to avoid oscillation when load drops.
    double alpha = (raw_offset > smoothed_offset_us) ? alpha_attack : alpha_release;
    if (fast_convergence_frames > 0) {
        alpha = 0.30;
        fast_convergence_frames--;
    }
    smoothed_offset_us += alpha * (raw_offset - smoothed_offset_us);

    // ── Headroom Streak Fast Release ──
    // If we have >=12% confirmed hardware headroom for 20 consecutive frames,
    // clear stale heavy history from windows to eliminate lingering low FPS.
    if (enable_fast_release) {
        double current_capped_interval = effective_interval_us + smoothed_offset_us;
        if (smoothed_offset_us > 100.0 && bottleneck_work_us < current_capped_interval * 0.88) {
            headroom_streak++;
            if (headroom_streak >= 20) {
                headroom_streak = 0;
                size_t keep = (std::min)(static_cast<size_t>(20), primary.Size());
                if (keep > 0) {
                    auto recent_light = primary.TakeLast(keep);
                    double median_light = recent_light[recent_light.size() / 2];
                    primary.Clear();
                    short_win.Clear();
                    long_win.Clear();
                    for (double v : recent_light) {
                        primary.Push(v);
                        short_win.Push(v);
                        long_win.Push(v);
                    }
                    // Pad to 32 so window stays warm and doesn't trigger cold start reset
                    while (primary.Size() < 32) {
                        primary.Push(median_light);
                        short_win.Push(median_light);
                        long_win.Push(median_light);
                    }
                    fast_convergence_frames = 20;
                    LOG_DEBUG("AdaptiveSmoothing: fast release triggered by 20 frames of >=12%% headroom");
                }
            }
        } else {
            headroom_streak = (headroom_streak > 2) ? (headroom_streak - 2) : 0;
        }
    }

    // Publish for OSD/CSV
    g_smoothing_offset_us.store(smoothed_offset_us, std::memory_order_relaxed);
    g_smoothing_p99_us.store(raw_p99_us, std::memory_order_relaxed);

    return smoothed_offset_us;
}

void AdaptiveSmoothing::SoftReset() {
    size_t keep_primary = primary.Size() / 4;
    if (keep_primary > 0) {
        auto recent = primary.TakeLast(keep_primary);
        primary.Clear();
        for (double v : recent) primary.Push(v);
    }

    if (dual_mode) {
        size_t keep_short = short_win.Size() / 4;
        if (keep_short > 0) {
            auto recent = short_win.TakeLast(keep_short);
            short_win.Clear();
            for (double v : recent) short_win.Push(v);
        }

        size_t keep_long = long_win.Size() / 4;
        if (keep_long > 0) {
            auto recent = long_win.TakeLast(keep_long);
            long_win.Clear();
            for (double v : recent) long_win.Push(v);
        }
    }

    fast_convergence_frames = 32;
    consecutive_outliers = 0;
    headroom_streak = 0;

    LOG_DEBUG("AdaptiveSmoothing: soft reset, retained %zu samples, fast convergence for 32 frames",
              primary.Size());
}

void AdaptiveSmoothing::Reset() {
    primary.Clear();
    short_win.Clear();
    long_win.Clear();
    smoothed_offset_us = 0.0;
    raw_p99_us = 0.0;
    fast_convergence_frames = 0;
    consecutive_outliers = 0;
    headroom_streak = 0;

    g_smoothing_offset_us.store(0.0, std::memory_order_relaxed);
    g_smoothing_p99_us.store(0.0, std::memory_order_relaxed);
}

void AdaptiveSmoothing::SetConfig(bool dual, double percentile, bool enable,
                                 double drain_margin, bool fast_rel) {
    bool mode_changed = (dual != dual_mode);
    dual_mode = dual;
    target_percentile = std::clamp(percentile, 0.50, 0.999);
    enabled = enable;
    queue_drain_margin_pct = std::clamp(drain_margin, 0.0, 0.10);
    enable_fast_release = fast_rel;

    if (mode_changed && primary.Size() > 0) {
        SoftReset();
        LOG_INFO("AdaptiveSmoothing: config changed, dual=%d percentile=%.3f drain=%.3f fast_rel=%d",
                 dual, target_percentile, queue_drain_margin_pct, enable_fast_release);
    }
}
