// Loaded as hid.dll: the game finds this file in its folder first. Every hid.dll export jumps straight to the
// real one in System32, so the game's HID calls behave exactly as without the mod.
// Only the standalone hid.dll build uses this file (with hid.def); the crloader build (crmods/GestureSwitch.dll)
// needs no proxy.
#include "mod.h"
#include <stdio.h>

#define HID_EXPORTS(X) \
    X(HidD_FlushQueue) X(HidD_FreePreparsedData) X(HidD_GetAttributes) X(HidD_GetConfiguration) \
    X(HidD_GetFeature) X(HidD_GetHidGuid) X(HidD_GetIndexedString) X(HidD_GetInputReport) \
    X(HidD_GetManufacturerString) X(HidD_GetMsGenreDescriptor) X(HidD_GetNumInputBuffers) \
    X(HidD_GetPhysicalDescriptor) X(HidD_GetPreparsedData) X(HidD_GetProductString) \
    X(HidD_GetSerialNumberString) X(HidD_Hello) X(HidD_SetConfiguration) X(HidD_SetFeature) \
    X(HidD_SetNumInputBuffers) X(HidD_SetOutputReport) X(HidP_GetButtonArray) X(HidP_GetButtonCaps) \
    X(HidP_GetCaps) X(HidP_GetData) X(HidP_GetExtendedAttributes) X(HidP_GetLinkCollectionNodes) \
    X(HidP_GetScaledUsageValue) X(HidP_GetSpecificButtonCaps) X(HidP_GetSpecificValueCaps) \
    X(HidP_GetUsageValue) X(HidP_GetUsageValueArray) X(HidP_GetUsages) X(HidP_GetUsagesEx) \
    X(HidP_GetValueCaps) X(HidP_GetVersionInternal) X(HidP_InitializeReportForID) \
    X(HidP_MaxDataListLength) X(HidP_MaxUsageListLength) X(HidP_SetButtonArray) X(HidP_SetData) \
    X(HidP_SetScaledUsageValue) X(HidP_SetUsageValue) X(HidP_SetUsageValueArray) X(HidP_SetUsages) \
    X(HidP_TranslateUsagesToI8042ScanCodes) X(HidP_UnsetUsages) X(HidP_UsageListDifference)

// One pointer per export, filled in by proxy_init, and a stub with the export's name that jumps through it.
// The stubs are plain jumps, so arguments and return values pass through untouched.
#define DECLARE_PTR(name) void *p_##name;
HID_EXPORTS(DECLARE_PTR)

#define DECLARE_STUB(name) \
    __asm__(".text\n.globl " #name "\n" #name ":\n\tjmp *p_" #name "(%rip)\n");
HID_EXPORTS(DECLARE_STUB)

int proxy_init(void)
{
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 10) return 0;
    lstrcatA(path, "\\hid.dll");
    HMODULE real = LoadLibraryA(path);
    if (!real) return 0;
#define RESOLVE(name) p_##name = (void *)GetProcAddress(real, #name);
    HID_EXPORTS(RESOLVE)
    return 1;
}
