#pragma once

#include <cstdint>
#include <atomic>
#include <reshade.hpp>

// ── Cross-API GPU Hardware Timestamp Timer ──
// Measures GPU execution duration using ReShade Add-on query heaps
// with non-blocking N-3 frame ring buffer readback across DX12, Vulkan, and DX11.

class GpuTimer {
public:
    static constexpr uint32_t RING_SIZE = 4; // 4 frames in flight (N-3 readback)

    GpuTimer() = default;
    ~GpuTimer();

    // Prevent copying
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;

    // ReShade lifecycle events
    void OnInitDevice(reshade::api::device* device);
    void OnDestroyDevice(reshade::api::device* device);
    void OnCommandListExecute(reshade::api::command_queue* queue, reshade::api::command_list* cmd_list);
    void OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* sc);
    void OnFinishPresent(reshade::api::command_queue* queue, reshade::api::swapchain* sc);

    // Queries
    double GetLatestGpuTimeUs() const noexcept {
        return latest_gpu_time_us_.load(std::memory_order_relaxed);
    }

    bool IsSupported() const noexcept {
        return supported_.load(std::memory_order_relaxed);
    }

    const char* GetSourceTag() const noexcept {
        if (!IsSupported()) return "N/A";
        return has_readback_buffer_ ? "HW Query (RB)" : "HW Query";
    }

    void Reset();

private:
    reshade::api::device*     device_ = nullptr;
    reshade::api::query_heap  query_heap_ = {0};
    reshade::api::resource    readback_buffer_ = {0};
    void*                     mapped_ptr_ = nullptr;

    uint64_t                  timestamp_freq_ = 0;
    uint32_t                  frame_index_ = 0;
    bool                      frame_started_ = false;
    bool                      has_readback_buffer_ = false;
    bool                      slot_recorded_[RING_SIZE] = {false, false, false, false};

    std::atomic<bool>         supported_{false};
    std::atomic<double>       latest_gpu_time_us_{0.0};
};

extern GpuTimer g_gpu_timer;
