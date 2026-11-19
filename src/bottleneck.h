#pragma once

#include <atomic>
#include <cstdint>

enum class BottleneckKind : uint8_t {
    Unknown,
    GpuBound,
    CpuBound
};

inline const char* BottleneckToString(BottleneckKind kind) {
    switch (kind) {
        case BottleneckKind::GpuBound: return "GPU Bound";
        case BottleneckKind::CpuBound: return "CPU Bound";
        default:                       return "Balanced";
    }
}

extern std::atomic<BottleneckKind> g_bottleneck_kind;
extern std::atomic<double> g_telemetry_gpu_work_us;
extern std::atomic<double> g_telemetry_cpu_work_us;
