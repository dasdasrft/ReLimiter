#pragma once

#include <cstdint>
#include <atomic>

// Vulkan/Present-based enforcement point.
// Called from EnfDisp_OnPresent when enforcement path is PresentBased.
// Uses shared scheduler core (OnMarker). Works for DX9, DX11, Vulkan, OpenGL.

// Measured frame render time (us) for non-Reflex / AMD present-based path
extern std::atomic<double> g_measured_render_time_us;

void VkEnforce_Init();
void VkEnforce_Shutdown();

// Called before each present via ReShade present event.
void VkEnforce_OnPresent(int64_t now_qpc);
