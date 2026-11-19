#include "hw_monitor.h"
#include "logger.h"
#include <Windows.h>
#include <Psapi.h>
#include <powrprof.h>
#include <dxgi1_4.h>
#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "dxgi.lib")
#include <cstdint>
#include <cstring>
#include <vector>
#include <atomic>
#include <thread>

// ══════════════════════════════════════════════════════════════════
// AMD Display Library (ADL / ADL2) Telemetry
// ══════════════════════════════════════════════════════════════════
static bool s_adl_available = false;
static HMODULE s_adl_module = nullptr;
static void* s_adl_context = nullptr;
static int s_adl_adapter_index = -1;

typedef void* (__cdecl *PFN_ADL_Main_Malloc)(int);
typedef int (*PFN_ADL2_Main_Control_Create)(PFN_ADL_Main_Malloc, int, void**);
typedef int (*PFN_ADL2_Main_Control_Destroy)(void*);
typedef int (*PFN_ADL2_Adapter_NumberOfAdapters_Get)(void*, int*);
typedef int (*PFN_ADL2_Adapter_AdapterInfo_Get)(void*, void*, int);
typedef int (*PFN_ADL2_Adapter_Active_Get)(void*, int, int*);
typedef int (*PFN_ADL2_OverdriveN_Temperature_Get)(void*, int, int, int*);
typedef int (*PFN_ADL2_Overdrive6_Temperature_Get)(void*, int, int*);

struct ADLSingleSensorData {
    int supported;
    int value;
};
struct ADLPMLogDataOutput {
    int size;
    ADLSingleSensorData sensors[256];
};
typedef int (*PFN_ADL2_New_QueryPMLogData_Get)(void*, int, ADLPMLogDataOutput*);

struct AdapterInfo {
    int iSize;
    int iAdapterIndex;
    char strUDID[256];
    int iBusNumber;
    int iDeviceNumber;
    int iFunctionNumber;
    int iVendorID;
    char strAdapterName[256];
    char strDisplayName[256];
    int iPresent;
    int iExist;
    char strDriverPath[256];
    char strDriverPathExt[256];
    char strPNPString[256];
    int iOSDisplayIndex;
};

static PFN_ADL2_Main_Control_Create s_ADL2_Main_Control_Create = nullptr;
static PFN_ADL2_Main_Control_Destroy s_ADL2_Main_Control_Destroy = nullptr;
static PFN_ADL2_Adapter_NumberOfAdapters_Get s_ADL2_Adapter_NumberOfAdapters_Get = nullptr;
static PFN_ADL2_Adapter_AdapterInfo_Get s_ADL2_Adapter_AdapterInfo_Get = nullptr;
static PFN_ADL2_Adapter_Active_Get s_ADL2_Adapter_Active_Get = nullptr;
static PFN_ADL2_OverdriveN_Temperature_Get s_ADL2_OverdriveN_Temperature_Get = nullptr;
static PFN_ADL2_Overdrive6_Temperature_Get s_ADL2_Overdrive6_Temperature_Get = nullptr;
static PFN_ADL2_New_QueryPMLogData_Get s_ADL2_New_QueryPMLogData_Get = nullptr;

// Double-buffered data: poll thread writes to s_write_data, then atomically
// swaps the pointer. OSD reads from s_read_data. No locks, no stalls.
static HWMonitorData s_buf_a = {};
static HWMonitorData s_buf_b = {};
static std::atomic<HWMonitorData*> s_read_data{&s_buf_a};
static HWMonitorData* s_write_data = &s_buf_b;

// Background poll thread
static std::atomic<bool> s_running{false};
static std::thread s_poll_thread;

// CPU usage tracking (thread-local to poll thread)
static ULARGE_INTEGER s_prev_idle = {};
static ULARGE_INTEGER s_prev_kernel = {};
static ULARGE_INTEGER s_prev_user = {};
static bool s_cpu_first_sample = true;

static bool InitADL() {
#ifdef _WIN64
    s_adl_module = LoadLibraryW(L"atiadlxx.dll");
#else
    s_adl_module = LoadLibraryW(L"atiadlxy.dll");
#endif
    if (!s_adl_module) {
        LOG_INFO("HWMonitor: atiadlxx.dll not found — AMD GPU hardware monitoring unavailable");
        return false;
    }

    s_ADL2_Main_Control_Create = reinterpret_cast<PFN_ADL2_Main_Control_Create>(
        GetProcAddress(s_adl_module, "ADL2_Main_Control_Create"));
    s_ADL2_Main_Control_Destroy = reinterpret_cast<PFN_ADL2_Main_Control_Destroy>(
        GetProcAddress(s_adl_module, "ADL2_Main_Control_Destroy"));
    s_ADL2_Adapter_NumberOfAdapters_Get = reinterpret_cast<PFN_ADL2_Adapter_NumberOfAdapters_Get>(
        GetProcAddress(s_adl_module, "ADL2_Adapter_NumberOfAdapters_Get"));
    s_ADL2_Adapter_AdapterInfo_Get = reinterpret_cast<PFN_ADL2_Adapter_AdapterInfo_Get>(
        GetProcAddress(s_adl_module, "ADL2_Adapter_AdapterInfo_Get"));
    s_ADL2_Adapter_Active_Get = reinterpret_cast<PFN_ADL2_Adapter_Active_Get>(
        GetProcAddress(s_adl_module, "ADL2_Adapter_Active_Get"));
    s_ADL2_OverdriveN_Temperature_Get = reinterpret_cast<PFN_ADL2_OverdriveN_Temperature_Get>(
        GetProcAddress(s_adl_module, "ADL2_OverdriveN_Temperature_Get"));
    s_ADL2_Overdrive6_Temperature_Get = reinterpret_cast<PFN_ADL2_Overdrive6_Temperature_Get>(
        GetProcAddress(s_adl_module, "ADL2_Overdrive6_Temperature_Get"));
    s_ADL2_New_QueryPMLogData_Get = reinterpret_cast<PFN_ADL2_New_QueryPMLogData_Get>(
        GetProcAddress(s_adl_module, "ADL2_New_QueryPMLogData_Get"));

    if (!s_ADL2_Main_Control_Create ||
        s_ADL2_Main_Control_Create([](int sz) -> void* { return malloc(sz); }, 1, &s_adl_context) != 0 ||
        !s_adl_context) {
        LOG_WARN("HWMonitor: ADL2_Main_Control_Create failed");
        return false;
    }

    int num = 0;
    if (s_ADL2_Adapter_NumberOfAdapters_Get && s_ADL2_Adapter_NumberOfAdapters_Get(s_adl_context, &num) == 0 && num > 0) {
        int chosen_idx = 0;
        if (s_ADL2_Adapter_AdapterInfo_Get) {
            std::vector<AdapterInfo> infos(num);
            memset(infos.data(), 0, sizeof(AdapterInfo) * num);
            infos[0].iSize = sizeof(AdapterInfo);
            if (s_ADL2_Adapter_AdapterInfo_Get(s_adl_context, infos.data(), sizeof(AdapterInfo) * num) == 0) {
                for (int i = 0; i < num; i++) {
                    int active = 0;
                    if (s_ADL2_Adapter_Active_Get)
                        s_ADL2_Adapter_Active_Get(s_adl_context, infos[i].iAdapterIndex, &active);

                    bool is_amd = (strstr(infos[i].strAdapterName, "Radeon") != nullptr ||
                                   strstr(infos[i].strAdapterName, "AMD") != nullptr ||
                                   infos[i].iVendorID == 0x1002 || infos[i].iVendorID == 0x3EA);

                    if (is_amd && infos[i].iPresent && infos[i].iExist) {
                        chosen_idx = infos[i].iAdapterIndex;
                        if (active) break; // Perfect match: active AMD adapter
                    }
                }
            }
        }

        s_adl_adapter_index = chosen_idx;
        s_adl_available = true;
        LOG_INFO("HWMonitor: AMD ADL2 initialized successfully, adapter_index=%d (total=%d, PMLog=%s)",
                 s_adl_adapter_index, num, s_ADL2_New_QueryPMLogData_Get ? "yes" : "no");
        return true;
    }

    LOG_WARN("HWMonitor: ADL2_Adapter_NumberOfAdapters_Get found 0 adapters");
    return false;
}

static void PollGPU(HWMonitorData& d) {
    if (s_adl_available && s_adl_context) {
        __try {
            if (s_ADL2_New_QueryPMLogData_Get) {
                ADLPMLogDataOutput out = {};
                out.size = sizeof(out);
                if (s_ADL2_New_QueryPMLogData_Get(s_adl_context, s_adl_adapter_index, &out) == 0) {
                    // Temperature: Sensor 8 (Edge/Core), Sensor 27 (Hotspot/Junction), Sensor 9 (Memory/VRAM)
                    if (out.sensors[8].supported && out.sensors[8].value > 0)
                        d.gpu_temp_c = out.sensors[8].value;
                    if (out.sensors[27].supported && out.sensors[27].value > 0)
                        d.gpu_temp_hotspot_c = out.sensors[27].value;
                    if (out.sensors[9].supported && out.sensors[9].value > 0)
                        d.gpu_temp_mem_c = out.sensors[9].value;

                    // Clocks: Sensor 1 (GFX Core Clock MHz), Sensor 2 (Memory Clock MHz)
                    if (out.sensors[1].supported && out.sensors[1].value > 0)
                        d.gpu_clock_mhz = out.sensors[1].value;
                    if (out.sensors[2].supported && out.sensors[2].value > 0)
                        d.gpu_mem_clock_mhz = out.sensors[2].value;

                    // GPU Activity / Utilization (%): Sensor 19
                    if (out.sensors[19].supported && out.sensors[19].value >= 0 && out.sensors[19].value <= 100)
                        d.gpu_usage_pct = out.sensors[19].value;

                    // Power: Sensor 73 (Total Board Power in Watts)
                    if (out.sensors[73].supported && out.sensors[73].value > 0)
                        d.gpu_power_w = out.sensors[73].value;
                }
            }

            // Fallback for pre-RDNA / legacy cards if PMLog did not populate temperature
            if (d.gpu_temp_c < 0) {
                int temp = 0;
                if (s_ADL2_OverdriveN_Temperature_Get && s_ADL2_OverdriveN_Temperature_Get(s_adl_context, s_adl_adapter_index, 1, &temp) == 0 && temp > 0) {
                    d.gpu_temp_c = temp / 1000;
                } else if (s_ADL2_Overdrive6_Temperature_Get && s_ADL2_Overdrive6_Temperature_Get(s_adl_context, s_adl_adapter_index, &temp) == 0 && temp > 0) {
                    d.gpu_temp_c = temp / 1000;
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Query VRAM via standard DXGI 1.4 for the active adapter
    if (d.vram_total_mb <= 0) {
        IDXGIFactory4* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            IDXGIAdapter1* adapter = nullptr;
            if (SUCCEEDED(factory->EnumAdapters1(0, &adapter))) {
                IDXGIAdapter3* adapter3 = nullptr;
                if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
                    DXGI_QUERY_VIDEO_MEMORY_INFO memInfo = {};
                    if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo))) {
                        d.vram_total_mb = static_cast<int64_t>(memInfo.Budget / (1024 * 1024));
                        d.vram_used_mb  = static_cast<int64_t>(memInfo.CurrentUsage / (1024 * 1024));
                    }
                    adapter3->Release();
                }
                adapter->Release();
            }
            factory->Release();
        }
    }
}

static void PollCPU(HWMonitorData& d) {
    FILETIME idle_ft, kernel_ft, user_ft;
    if (GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
        ULARGE_INTEGER idle, kernel, user;
        idle.LowPart = idle_ft.dwLowDateTime;     idle.HighPart = idle_ft.dwHighDateTime;
        kernel.LowPart = kernel_ft.dwLowDateTime; kernel.HighPart = kernel_ft.dwHighDateTime;
        user.LowPart = user_ft.dwLowDateTime;     user.HighPart = user_ft.dwHighDateTime;

        if (!s_cpu_first_sample) {
            uint64_t d_idle   = idle.QuadPart   - s_prev_idle.QuadPart;
            uint64_t d_kernel = kernel.QuadPart - s_prev_kernel.QuadPart;
            uint64_t d_user   = user.QuadPart   - s_prev_user.QuadPart;
            uint64_t d_total  = d_kernel + d_user;
            if (d_total > 0) {
                uint64_t d_busy = d_total - d_idle;
                d.cpu_usage_pct = static_cast<int>((d_busy * 100) / d_total);
            }
        }
        s_prev_idle = idle;
        s_prev_kernel = kernel;
        s_prev_user = user;
        s_cpu_first_sample = false;
    }

    {
        typedef struct {
            ULONG Number;
            ULONG MaxMhz;
            ULONG CurrentMhz;
            ULONG MhzLimit;
            ULONG MaxIdleState;
            ULONG CurrentIdleState;
        } PROCESSOR_POWER_INFORMATION;

        SYSTEM_INFO si;
        GetSystemInfo(&si);
        int nproc = static_cast<int>(si.dwNumberOfProcessors);
        if (nproc > 256) nproc = 256;

        PROCESSOR_POWER_INFORMATION ppi[256] = {};
        ULONG buf_size = static_cast<ULONG>(nproc * sizeof(PROCESSOR_POWER_INFORMATION));

        LONG status = CallNtPowerInformation(
            static_cast<POWER_INFORMATION_LEVEL>(11),
            nullptr, 0, ppi, buf_size);
        if (status == 0) {
            ULONG max_mhz = 0;
            for (int i = 0; i < nproc; i++) {
                if (ppi[i].CurrentMhz > max_mhz)
                    max_mhz = ppi[i].CurrentMhz;
            }
            if (max_mhz > 0)
                d.cpu_clock_mhz = static_cast<int>(max_mhz);
        }
    }
}

static void PollRAM(HWMonitorData& d) {
    MEMORYSTATUSEX mem = {};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem)) {
        d.ram_total_mb = static_cast<int64_t>(mem.ullTotalPhys / (1024 * 1024));
        d.ram_used_mb  = static_cast<int64_t>((mem.ullTotalPhys - mem.ullAvailPhys) / (1024 * 1024));
    }
}

// ── Background poll thread ──
// All sensor queries run here, never on the render/present thread.
// Results are written to s_write_data, then atomically published via
// pointer swap. The OSD reads s_read_data which is always a complete,
// consistent snapshot — no partial updates, no locks, no stalls.
static void PollThreadProc() {
    while (s_running.load(std::memory_order_relaxed)) {
        HWMonitorData fresh = {};
        PollGPU(fresh);
        PollCPU(fresh);
        PollRAM(fresh);

        *s_write_data = fresh;
        HWMonitorData* old_read = s_read_data.exchange(s_write_data, std::memory_order_release);
        s_write_data = old_read;

        for (int i = 0; i < 100 && s_running.load(std::memory_order_relaxed); i++)
            Sleep(10);
    }
}

// ── Public API ──

void HWMonitor_Init() {
    s_adl_available = InitADL();

    // Do one synchronous poll so data is available immediately
    HWMonitorData initial = {};
    PollGPU(initial);
    PollCPU(initial);
    PollRAM(initial);
    s_buf_a = initial;
    s_buf_b = initial;
    s_read_data.store(&s_buf_a, std::memory_order_relaxed);
    s_write_data = &s_buf_b;

    // Start background poll thread
    s_running.store(true, std::memory_order_relaxed);
    s_poll_thread = std::thread(PollThreadProc);

    LOG_INFO("HWMonitor: initialized (AMD ADL2=%s), poll thread started",
             s_adl_available ? "yes" : "no");
}

void HWMonitor_Shutdown() {
    s_running.store(false, std::memory_order_relaxed);
    if (s_poll_thread.joinable())
        s_poll_thread.join();

    if (s_adl_available && s_adl_context && s_ADL2_Main_Control_Destroy) {
        s_ADL2_Main_Control_Destroy(s_adl_context);
        s_adl_context = nullptr;
    }
    if (s_adl_module) {
        FreeLibrary(s_adl_module);
        s_adl_module = nullptr;
    }
    s_adl_available = false;

    LOG_INFO("HWMonitor: shutdown");
}

void HWMonitor_Update() {
    // Kept for API compatibility — polling runs on background thread.
}

const HWMonitorData& HWMonitor_GetData() {
    return *s_read_data.load(std::memory_order_acquire);
}
