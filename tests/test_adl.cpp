#include <windows.h>
#include <stdio.h>

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
typedef int (*PFN_ADL2_Device_PMLog_Device_Create)(void*, int, void**);
typedef int (*PFN_ADL2_Device_PMLog_Device_Destroy)(void*, void*);
typedef int (*PFN_ADL2_Adapter_PMLog_Support_Get)(void*, int, void*);

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

int main() {
    HMODULE adl = LoadLibraryW(L"atiadlxx.dll");
    if (!adl) {
        printf("atiadlxx.dll not found!\n");
        return 1;
    }
    printf("Loaded atiadlxx.dll\n");

    auto pCreate = (PFN_ADL2_Main_Control_Create)GetProcAddress(adl, "ADL2_Main_Control_Create");
    auto pDestroy = (PFN_ADL2_Main_Control_Destroy)GetProcAddress(adl, "ADL2_Main_Control_Destroy");
    auto pNum = (PFN_ADL2_Adapter_NumberOfAdapters_Get)GetProcAddress(adl, "ADL2_Adapter_NumberOfAdapters_Get");
    auto pInfo = (PFN_ADL2_Adapter_AdapterInfo_Get)GetProcAddress(adl, "ADL2_Adapter_AdapterInfo_Get");
    auto pActive = (PFN_ADL2_Adapter_Active_Get)GetProcAddress(adl, "ADL2_Adapter_Active_Get");
    auto pPMLog = (PFN_ADL2_New_QueryPMLogData_Get)GetProcAddress(adl, "ADL2_New_QueryPMLogData_Get");
    auto pOD_N = (PFN_ADL2_OverdriveN_Temperature_Get)GetProcAddress(adl, "ADL2_OverdriveN_Temperature_Get");
    auto pOD_6 = (PFN_ADL2_Overdrive6_Temperature_Get)GetProcAddress(adl, "ADL2_Overdrive6_Temperature_Get");

    void* ctx = nullptr;
    int res = pCreate([](int sz) -> void* { return malloc(sz); }, 1, &ctx);
    printf("ADL2_Main_Control_Create: %d, ctx=%p\n", res, ctx);
    if (res != 0 || !ctx) return 1;

    int numAdapters = 0;
    pNum(ctx, &numAdapters);
    printf("Number of adapters: %d\n", numAdapters);

    if (numAdapters > 0 && pInfo) {
        AdapterInfo* infos = (AdapterInfo*)malloc(sizeof(AdapterInfo) * numAdapters);
        memset(infos, 0, sizeof(AdapterInfo) * numAdapters);
        infos[0].iSize = sizeof(AdapterInfo);
        int infoRes = pInfo(ctx, infos, sizeof(AdapterInfo) * numAdapters);
        printf("AdapterInfo_Get: %d\n", infoRes);
        for (int i = 0; i < numAdapters; i++) {
            int active = 0;
            if (pActive) pActive(ctx, infos[i].iAdapterIndex, &active);
            printf("[%d] idx=%d name='%s' vendor=0x%X present=%d exist=%d active=%d\n",
                   i, infos[i].iAdapterIndex, infos[i].strAdapterName, infos[i].iVendorID, infos[i].iPresent, infos[i].iExist, active);
            
            if (pPMLog) {
                ADLPMLogDataOutput out = {};
                out.size = sizeof(out);
                int pmRes = pPMLog(ctx, infos[i].iAdapterIndex, &out);
                printf("  PMLog idx %d -> ret %d\n", infos[i].iAdapterIndex, pmRes);
                if (pmRes == 0) {
                    for (int s = 0; s < 100; s++) {
                        if (out.sensors[s].supported) {
                            printf("    sensor[%d] = %d\n", s, out.sensors[s].value);
                        }
                    }
                }
            }

            if (pOD_N) {
                int temp = 0;
                int nRes = pOD_N(ctx, infos[i].iAdapterIndex, 1, &temp);
                printf("  OD_N idx %d -> ret %d, temp=%d\n", infos[i].iAdapterIndex, nRes, temp);
            }
        }
        free(infos);
    }

    pDestroy(ctx);
    FreeLibrary(adl);
    return 0;
}
