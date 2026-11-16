// The crash resistance test (tools/faulttest.py), not part of a release. TW3SE_FaultTest(a : int, b : string) : int
// reads its first argument and then faults, halfway through its arguments: TW3SE puts the script's argument cursor
// back, reads the arguments past and turns the native off. Built a second time with CRASH_IN_LOAD as
// faulttest_load.dll, which faults in TW3SE_PluginLoad: the plugin stays off, the game starts.
#include "tw3se/tw3se.h"

static const TW3SE_Interface* g_api = nullptr;

static void FaultTest(void*, void* frame, void*) {
    int a = 0;
    g_api->ReadParam(frame, &a);
    volatile int v = *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x10 + a));
    (void)v;
}

TW3SE_EXPORT int TW3SE_PluginQuery(TW3SE_PluginInfo* info) {
#ifdef CRASH_IN_LOAD
    info->name = "FaultTestLoad";
#else
    info->name = "FaultTest";
#endif
    info->author = "TW3SE tests";
    info->version = 0x00010000;
    info->minInterface = 1;
    return 1;
}

TW3SE_EXPORT int TW3SE_PluginLoad(const TW3SE_Interface* api) {
    g_api = api;
#ifdef CRASH_IN_LOAD
    volatile int v = *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x18));
    (void)v;
#endif
    api->RegisterNative("TW3SE_FaultTest", &FaultTest, 2);
    return 1;
}
