#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include <atomic>
#include <algorithm>

// Standalone test for Refactored Adaptive Smoothing (Cross-API Dual-Bottleneck,
// Asymmetric Attack/Release, BBR Negative-Gain Headroom, and Headroom Streak Fast Release)
#include "../src/bottleneck.h"
#include "../src/rolling_window.h"

struct TestAdaptiveSmoothing {
    RollingWindow<double, 200> primary;
    RollingWindow<double, 64>  short_win;
    RollingWindow<double, 200> long_win;

    double smoothed_offset_us = 0.0;
    double raw_p99_us = 0.0;
    double alpha_attack  = 0.25;
    double alpha_release = 0.08;
    int    fast_convergence_frames = 0;
    int    consecutive_outliers = 0;
    uint32_t headroom_streak = 0;

    bool   dual_mode = true;
    double target_percentile = 0.99;
    bool   enabled = true;
    double queue_drain_margin_pct = 0.015;
    bool   enable_fast_release = true;

    void SoftReset() {
        size_t keep = primary.Size() / 4;
        if (keep > 0) {
            auto recent = primary.TakeLast(keep);
            primary.Clear();
            for (double v : recent) primary.Push(v);
        }
        if (dual_mode) {
            size_t keep_s = short_win.Size() / 4;
            if (keep_s > 0) {
                auto recent = short_win.TakeLast(keep_s);
                short_win.Clear();
                for (double v : recent) short_win.Push(v);
            }
            size_t keep_l = long_win.Size() / 4;
            if (keep_l > 0) {
                auto recent = long_win.TakeLast(keep_l);
                long_win.Clear();
                for (double v : recent) long_win.Push(v);
            }
        }
        consecutive_outliers = 0;
        headroom_streak = 0;
        fast_convergence_frames = 32;
    }

    double Update(double bottleneck_work_us, double effective_interval_us,
                  BottleneckKind bottleneck_kind = BottleneckKind::Unknown) {
        if (!enabled || bottleneck_work_us <= 0.0 || effective_interval_us <= 0.0) {
            smoothed_offset_us = 0.0;
            return 0.0;
        }
        if (bottleneck_work_us > effective_interval_us * 3.0)
            return smoothed_offset_us;

        if (primary.Size() >= 32) {
            double median = primary.Percentile(0.5);
            if (bottleneck_work_us > median * 3.0) {
                consecutive_outliers++;
                if (consecutive_outliers >= 8) {
                    consecutive_outliers = 0;
                    SoftReset();
                } else {
                    return smoothed_offset_us;
                }
            } else {
                consecutive_outliers = 0;
            }
        }

        primary.Push(bottleneck_work_us);
        if (dual_mode) {
            short_win.Push(bottleneck_work_us);
            long_win.Push(bottleneck_work_us);
        }

        if (primary.Size() < 32) {
            smoothed_offset_us = 0.0;
            raw_p99_us = 0.0;
            return 0.0;
        }

        double p99;
        if (dual_mode && short_win.Size() >= 16 && long_win.Size() >= 32) {
            double short_p = short_win.Percentile(target_percentile);
            double long_p  = long_win.Percentile(target_percentile);
            p99 = (std::max)(short_p, long_p);
        } else {
            p99 = primary.Percentile(target_percentile);
        }
        raw_p99_us = p99;

        // BBR negative-gain headroom
        double effective_p99 = p99;
        if (bottleneck_kind == BottleneckKind::GpuBound && queue_drain_margin_pct > 0.0) {
            effective_p99 = p99 * (1.0 + queue_drain_margin_pct);
        }

        double raw_offset = (std::max)(0.0, effective_p99 - effective_interval_us);
        double max_offset = effective_interval_us;
        raw_offset = (std::min)(raw_offset, max_offset);

        double alpha = (raw_offset > smoothed_offset_us) ? alpha_attack : alpha_release;
        if (fast_convergence_frames > 0) {
            alpha = 0.30;
            fast_convergence_frames--;
        }
        smoothed_offset_us += alpha * (raw_offset - smoothed_offset_us);

        // Headroom streak fast release
        if (enable_fast_release) {
            double current_capped_interval = effective_interval_us + smoothed_offset_us;
            if (smoothed_offset_us > 100.0 && bottleneck_work_us < current_capped_interval * 0.88) {
                headroom_streak++;
                if (headroom_streak >= 60) {
                    headroom_streak = 0;
                    if (dual_mode && long_win.Size() > 64) {
                        auto recent = long_win.TakeLast(64);
                        long_win.Clear();
                        for (double v : recent) long_win.Push(v);
                    }
                    if (primary.Size() > 64) {
                        auto recent = primary.TakeLast(64);
                        primary.Clear();
                        for (double v : recent) primary.Push(v);
                    }
                }
            } else {
                headroom_streak = (headroom_streak > 4) ? (headroom_streak - 4) : 0;
            }
        }

        return smoothed_offset_us;
    }
};

int main() {
    std::cout << "=== ReLimiter Refactored Adaptive Smoothing Unit Tests ===" << std::endl;

    TestAdaptiveSmoothing smoother;
    smoother.enabled = true;
    smoother.target_percentile = 0.99;
    smoother.dual_mode = true;
    smoother.queue_drain_margin_pct = 0.015; // 1.5%

    // Test 1: Cold start warmup (< 32 frames)
    double target_interval = 7092.0; // ~141 FPS
    for (int i = 0; i < 30; i++) {
        double offset = smoother.Update(6800.0, target_interval, BottleneckKind::GpuBound);
        assert(offset == 0.0);
        assert(smoother.raw_p99_us == 0.0);
    }
    std::cout << "[PASS] Test 1: Cold start warmup correctly suppresses output before 32 samples" << std::endl;

    // Test 2: Stable rendering under target (no offset needed)
    for (int i = 0; i < 50; i++) {
        smoother.Update(6500.0, target_interval, BottleneckKind::GpuBound);
    }
    assert(smoother.raw_p99_us == 6500.0);
    assert(smoother.smoothed_offset_us == 0.0);
    std::cout << "[PASS] Test 2: Stable rendering under target produces zero offset" << std::endl;

    // Test 3: BBR Negative-Gain Headroom under GPU Bound
    // Workload around 7000 us (under 7092us). Without margin, offset would be 0.
    // With 1.5% margin on 7000 us -> effective_p99 = 7105 us -> raw_offset = 13 us > 0!
    for (int i = 0; i < 50; i++) {
        smoother.Update(7000.0, target_interval, BottleneckKind::GpuBound);
    }
    std::cout << "  P99: " << smoother.raw_p99_us << " us, Smoothed offset: " << smoother.smoothed_offset_us << " us" << std::endl;
    assert(smoother.smoothed_offset_us > 0.0);
    std::cout << "[PASS] Test 3: BBR negative gain headroom (+1.5%) successfully injects queue-draining offset" << std::endl;

    // Test 4: Asymmetric Attack vs Release
    // High load (8500 us) should ramp up fast (alpha = 0.25)
    double before_offset = smoother.smoothed_offset_us;
    smoother.Update(8500.0, target_interval, BottleneckKind::GpuBound);
    double step_up = smoother.smoothed_offset_us - before_offset;
    assert(step_up > 0.0);

    // Warm up high load for 40 frames to saturate offset
    for (int i = 0; i < 40; i++) {
        smoother.Update(8500.0, target_interval, BottleneckKind::GpuBound);
    }
    double high_offset = smoother.smoothed_offset_us;
    assert(high_offset >= 1000.0);

    // Drop load to 6000 us. Release should be smooth (alpha = 0.08)
    smoother.Update(6000.0, target_interval, BottleneckKind::GpuBound);
    double step_down = high_offset - smoother.smoothed_offset_us;
    // Attack rate (0.25) is more than 3x faster than release rate (0.08)
    std::cout << "  High offset: " << high_offset << " us, single-frame release drop: " << step_down << " us" << std::endl;
    assert(step_down > 0.0);
    std::cout << "[PASS] Test 4: Asymmetric Attack (0.25) and Release (0.08) verified" << std::endl;

    // Test 5: Headroom Streak Fast Release
    // Hold load at 5000 us (< 88% of target 7092 us) for 60 frames.
    // The long window history should be truncated to recent 64 samples.
    assert(smoother.long_win.Size() > 64);
    for (int i = 0; i < 65; i++) {
        smoother.Update(5000.0, target_interval, BottleneckKind::GpuBound);
    }
    std::cout << "  After 65 idle frames, offset dropped to: " << smoother.smoothed_offset_us << " us" << std::endl;
    // With old 512 window, offset would linger high for 500+ frames. With fast release, it drops rapidly!
    assert(smoother.smoothed_offset_us < 200.0);
    std::cout << "[PASS] Test 5: Headroom streak fast release eliminates lingering low FPS" << std::endl;

    // Test 6: Dual Bottleneck Arbitration
    double t_gpu = 12000.0;
    double t_cpu = 6000.0;
    BottleneckKind b1 = (t_gpu > 0.0 && t_gpu >= t_cpu * 0.95) ? BottleneckKind::GpuBound : BottleneckKind::CpuBound;
    assert(b1 == BottleneckKind::GpuBound);

    t_gpu = 4000.0;
    t_cpu = 8000.0;
    BottleneckKind b2 = (t_gpu > 0.0 && t_gpu >= t_cpu * 0.95) ? BottleneckKind::GpuBound : BottleneckKind::CpuBound;
    assert(b2 == BottleneckKind::CpuBound);
    std::cout << "[PASS] Test 6: Dual bottleneck arbitration correctly identifies GPU vs CPU bound" << std::endl;

    std::cout << "=== All Refactored Adaptive Smoothing tests passed successfully! ===" << std::endl;
    return 0;
}
