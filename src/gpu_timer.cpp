#include "gpu_timer.h"
#include "logger.h"
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <d3d12.h>
#endif

GpuTimer g_gpu_timer;

GpuTimer::~GpuTimer() {
    Reset();
}

void GpuTimer::Reset() {
    if (device_ != nullptr) {
        if (mapped_ptr_ != nullptr && readback_buffer_.handle != 0) {
            device_->unmap_buffer_region(readback_buffer_);
            mapped_ptr_ = nullptr;
        }
        if (readback_buffer_.handle != 0) {
            device_->destroy_resource(readback_buffer_);
            readback_buffer_ = {0};
        }
        if (query_heap_.handle != 0) {
            device_->destroy_query_heap(query_heap_);
            query_heap_ = {0};
        }
        device_ = nullptr;
    }
    timestamp_freq_ = 0;
    frame_index_ = 0;
    frame_started_ = false;
    has_readback_buffer_ = false;
    for (size_t i = 0; i < RING_SIZE; i++) {
        slot_recorded_[i] = false;
    }
    supported_.store(false, std::memory_order_relaxed);
    latest_gpu_time_us_.store(0.0, std::memory_order_relaxed);
}

void GpuTimer::OnInitDevice(reshade::api::device* device) {
    Reset();
    if (!device) return;

    device_ = device;
    LOG_INFO("GpuTimer: initializing for device API %d", static_cast<int>(device->get_api()));

    // Total query slots: RING_SIZE * 2 (start + end for each frame slot)
    constexpr uint32_t total_queries = RING_SIZE * 2;
    if (!device_->create_query_heap(reshade::api::query_type::timestamp, total_queries, &query_heap_)) {
        LOG_WARN("GpuTimer: device failed to create timestamp query heap, disabling");
        supported_.store(false, std::memory_order_relaxed);
        return;
    }

    // Check if the device supports copying query heap results directly on GPU (DX12 / Vulkan)
    bool can_copy = device_->check_capability(reshade::api::device_caps::copy_query_heap_results);
    if (can_copy) {
        reshade::api::resource_desc desc(
            total_queries * sizeof(uint64_t),
            reshade::api::memory_heap::readback,
            reshade::api::resource_usage::copy_dest
        );

        if (device_->create_resource(desc, nullptr, reshade::api::resource_usage::copy_dest, &readback_buffer_)) {
            if (device_->map_buffer_region(readback_buffer_, 0, total_queries * sizeof(uint64_t),
                                           reshade::api::map_access::read_only, &mapped_ptr_)) {
                has_readback_buffer_ = true;
                LOG_INFO("GpuTimer: readback buffer created and mapped successfully (%u bytes)",
                         static_cast<uint32_t>(total_queries * sizeof(uint64_t)));
            } else {
                LOG_WARN("GpuTimer: failed to map readback buffer, will try get_query_heap_results fallback");
                device_->destroy_resource(readback_buffer_);
                readback_buffer_ = {0};
            }
        } else {
            LOG_WARN("GpuTimer: failed to create readback buffer, will try get_query_heap_results fallback");
        }
    }

    supported_.store(true, std::memory_order_relaxed);
    LOG_INFO("GpuTimer: successfully initialized (heap=%llu, has_rb=%d)",
             query_heap_.handle, has_readback_buffer_);
}

void GpuTimer::OnDestroyDevice(reshade::api::device* device) {
    if (device_ == device) {
        LOG_INFO("GpuTimer: destroying device resources");
        Reset();
    }
}

void GpuTimer::OnCommandListExecute(reshade::api::command_queue* queue, reshade::api::command_list* cmd_list) {
    if (!supported_.load(std::memory_order_relaxed) || !query_heap_.handle || !queue)
        return;

    // Fetch frequency on first queue interaction if not known
    if (timestamp_freq_ == 0) {
        timestamp_freq_ = queue->get_timestamp_frequency();
#ifdef _WIN32
        if (timestamp_freq_ == 0 && device_ && device_->get_api() == reshade::api::device_api::d3d12) {
            uint64_t native_queue = queue->get_native();
            if (native_queue) {
                auto* d3d12_q = reinterpret_cast<ID3D12CommandQueue*>(native_queue);
                UINT64 freq = 0;
                if (SUCCEEDED(d3d12_q->GetTimestampFrequency(&freq)) && freq > 0) {
                    timestamp_freq_ = freq;
                }
            }
        }
#endif
        if (timestamp_freq_ > 0) {
            LOG_INFO("GpuTimer: detected GPU timestamp frequency: %llu Hz", timestamp_freq_);
        }
    }

    // Only record on graphics queues
    auto q_type = queue->get_type();
    if ((static_cast<uint32_t>(q_type) & static_cast<uint32_t>(reshade::api::command_queue_type::graphics)) == 0)
        return;

    // Record T_start on the first graphics command list of this frame
    if (!frame_started_) {
        uint32_t slot = frame_index_ % RING_SIZE;
        uint32_t query_idx = slot * 2;

        auto* imm = queue->get_immediate_command_list();
        if (imm) {
            imm->end_query(query_heap_, reshade::api::query_type::timestamp, query_idx);
            frame_started_ = true;
            slot_recorded_[slot] = false; // Will become true after T_end
        } else if (cmd_list) {
            cmd_list->end_query(query_heap_, reshade::api::query_type::timestamp, query_idx);
            frame_started_ = true;
            slot_recorded_[slot] = false;
        }
    }
}

void GpuTimer::OnPresent(reshade::api::command_queue* queue, reshade::api::swapchain* /*sc*/) {
    if (!supported_.load(std::memory_order_relaxed) || !query_heap_.handle || !queue)
        return;

    uint32_t current_slot = frame_index_ % RING_SIZE;

    if (frame_started_) {
        uint32_t end_query_idx = current_slot * 2 + 1;
        auto* imm = queue->get_immediate_command_list();
        if (imm) {
            imm->end_query(query_heap_, reshade::api::query_type::timestamp, end_query_idx);

            if (has_readback_buffer_ && readback_buffer_.handle != 0) {
                imm->copy_query_heap_results(
                    query_heap_,
                    reshade::api::query_type::timestamp,
                    current_slot * 2,
                    2,
                    readback_buffer_,
                    current_slot * 2 * sizeof(uint64_t),
                    sizeof(uint64_t)
                );
            }
            queue->flush_immediate_command_list();
            slot_recorded_[current_slot] = true;
        }
    }

    // ── Non-blocking N-3 Frame Delayed Readback ──
    uint32_t read_slot = (frame_index_ + 1) % RING_SIZE; // 3 frames ago
    if (frame_index_ >= RING_SIZE && slot_recorded_[read_slot] && timestamp_freq_ > 0) {
        uint64_t t_start = 0;
        uint64_t t_end = 0;
        bool read_ok = false;

        if (has_readback_buffer_ && mapped_ptr_) {
            const auto* data = static_cast<const uint64_t*>(mapped_ptr_);
            t_start = data[read_slot * 2];
            t_end   = data[read_slot * 2 + 1];
            read_ok = true;
        } else if (device_) {
            uint64_t query_data[2] = {0, 0};
            if (device_->get_query_heap_results(query_heap_, reshade::api::query_type::timestamp,
                                                read_slot * 2, 2, query_data, sizeof(uint64_t))) {
                t_start = query_data[0];
                t_end   = query_data[1];
                read_ok = true;
            }
        }

        if (read_ok && t_end > t_start) {
            double gpu_us = static_cast<double>(t_end - t_start) * 1e6 / static_cast<double>(timestamp_freq_);
            // Outlier / validity guard: (100us, 200ms)
            if (gpu_us >= 100.0 && gpu_us <= 200000.0) {
                latest_gpu_time_us_.store(gpu_us, std::memory_order_relaxed);
            }
        }
    }
}

void GpuTimer::OnFinishPresent(reshade::api::command_queue* /*queue*/, reshade::api::swapchain* /*sc*/) {
    frame_index_++;
    frame_started_ = false;
}
