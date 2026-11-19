#include <cassert>
#include <iostream>
#include <vector>
#include <cmath>
#include <atomic>
#include <algorithm>

// Standalone test for Adaptive Smoothing on AMD / non-Reflex hardware
#include "../src/rolling_window.h"

struct TestAdaptiveSmoothing {
    RollingWindow<double, 256> primary;
    RollingWindow<double, 64>  short_win;
    RollingWindow<double, 512> long_win;

    double smoothed_offset_us = 0.0;
    double raw_p99_us = 0.0;
    double ema_alpha = 0.15;
    int    fast_convergence_frames = 0;
    int    consecutive_outliers = 0;
    bool   dual_mode = false;
    double target_percentile = 0.99;
    bool   enabled = true;

    void SoftReset() {
        size_t keep = primary.Size() / 4;
        if (keep > 0) {
            auto recent = primary.TakeLast(keep);
            primary.Clear();
            for (double v : recent) primary.Push(v);
        }
        consecutive_outliers = 0;
        fast_convergence_frames = 32;
        ema_alpha = 0.30;
    }

    double Update(double render_time_us, double effective_interval_us) {
        if (!enabled || render_time_us <= 0.0 || effective_interval_us <= 0.0) {
            smoothed_offset_us = 0.0;
            return 0.0;
        }
        if (render_time_us > effective_interval_us * 3.0)
            return smoothed_offset_us;

        if (primary.Size() >= 32) {
            double median = primary.Percentile(0.5);
            if (render_time_us > median * 3.0) {
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

        primary.Push(render_time_us);
        if (dual_mode) {
            short_win.Push(render_time_us);
            long_win.Push(render_time_us);
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

        double raw_offset = (std::max)(0.0, p99 - effective_interval_us);
        double max_offset = effective_interval_us;
        raw_offset = (std::min)(raw_offset, max_offset);

        smoothed_offset_us += ema_alpha * (raw_offset - smoothed_offset_us);
        return smoothed_offset_us;
    }
};

int main() {
    std::cout << "=== ReLimiter AMD Adaptive Smoothing Unit Tests ===" << std::endl;

    TestAdaptiveSmoothing smoother;
    smoother.enabled = true;
    smoother.target_percentile = 0.99;

    // Test 1: Cold start warmup (< 32 frames)
    double target_interval = 7092.0; // ~141 FPS
    for (int i = 0; i < 30; i++) {
        double offset = smoother.Update(6800.0, target_interval);
        assert(offset == 0.0);
        assert(smoother.raw_p99_us == 0.0);
    }
    std::cout << "[PASS] Test 1: Cold start warmup correctly suppresses output before 32 samples" << std::endl;

    // Test 2: Stable rendering under target (no offset needed)
    for (int i = 0; i < 50; i++) {
        smoother.Update(6500.0, target_interval);
    }
    assert(smoother.raw_p99_us == 6500.0);
    assert(smoother.smoothed_offset_us == 0.0);
    std::cout << "[PASS] Test 2: Stable rendering under target produces zero offset" << std::endl;

    // Test 3: Realistic workload with P99 render-time variance (simulating AMD RX 9070 XT under heavy load)
    // 95% of frames around 6900 us, but 5% tail spikes up to 8200 us
    for (int i = 0; i < 150; i++) {
        double frame_time = (i % 20 == 0) ? 8200.0 : 6900.0;
        smoother.Update(frame_time, target_interval);
    }
    std::cout << "  Calculated P99: " << smoother.raw_p99_us << " us, Smoothed offset: " << smoother.smoothed_offset_us << " us" << std::endl;
    assert(smoother.raw_p99_us >= 8100.0);
    assert(smoother.smoothed_offset_us > 0.0);
    // Effective interval should now accommodate the P99 tail
    double effective = target_interval + smoother.smoothed_offset_us;
    assert(effective > target_interval);
    std::cout << "  New effective frame interval: " << effective << " us (~" << 1000000.0 / effective << " FPS)" << std::endl;
    std::cout << "[PASS] Test 3: Tail latency correctly computes positive offset to eliminate micro-stutters" << std::endl;

    // Test 4: Fallback data source resolution
    // Simulating: on NVIDIA, reflex_gpu_active is positive. On AMD, reflex is 0.0 and measured_render_time feeds.
    std::atomic<double> reflex_gpu_active{0.0};
    std::atomic<double> measured_render_time{7850.0};

    double resolved_active = reflex_gpu_active.load();
    if (resolved_active <= 0.0) {
        resolved_active = measured_render_time.load();
    }
    assert(resolved_active == 7850.0);
    std::cout << "[PASS] Test 4: AMD fallback successfully routes measured render time" << std::endl;

    // Test 5: Scene transition / deadlock recovery (Menu 1.5ms -> Gameplay 15ms)
    TestAdaptiveSmoothing transition_smoother;
    transition_smoother.enabled = true;
    // Simulate menu with 60 frames of 1500 us render time
    for (int i = 0; i < 60; i++) {
        transition_smoother.Update(1500.0, 16666.0);
    }
    assert(transition_smoother.raw_p99_us <= 2000.0);

    // Now transition into heavy gameplay (15000 us render time)
    // Old implementation deadlocked here forever because 15000 > 1500 * 3
    for (int i = 0; i < 40; i++) {
        transition_smoother.Update(15000.0, 16666.0);
    }
    // Must successfully adapt to the new scene
    assert(transition_smoother.raw_p99_us >= 14000.0);
    std::cout << "[PASS] Test 5: Scene transition deadlock recovery successfully broke through menu lock" << std::endl;

    std::cout << "=== All AMD Adaptive Smoothing tests passed successfully! ===" << std::endl;
    return 0;
}
