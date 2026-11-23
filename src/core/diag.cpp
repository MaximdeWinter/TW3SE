// TW3SE core - diagnostics: who ends the process (call stack), and a trace of the DLC manager.
#include "core.h"

#include <intrin.h>

namespace tw3se {
namespace {

using RtlExitUserProcess_t = void(NTAPI*)(LONG);
using TerminateProcess_t = BOOL(WINAPI*)(HANDLE, UINT);
RtlExitUserProcess_t oRtlExitUserProcess;
TerminateProcess_t oTerminateProcess;
volatile LONG g_endSeen = 0;  // an end the exit and TerminateProcess hooks wrote down (NtTerminateProcess comes after)

void LogStack(const char* why) {
    void* frames[48];
    USHORT n = RtlCaptureStackBackTrace(1, 48, frames, nullptr);
    Log("%s - call stack (%u frames):", why, n);
    for (USHORT i = 0; i < n; ++i) {
        HMODULE m = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &m) && m) {
            GetModuleFileNameA(m, name, MAX_PATH);
            const char* base = strrchr(name, '\\');
            Log("  #%02u %s+0x%llx", i, base ? base + 1 : name,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) - reinterpret_cast<uintptr_t>(m)));
        } else {
            Log("  #%02u %p", i, frames[i]);
        }
    }
}

void NTAPI ExitHook(LONG code) {
    InterlockedExchange(&g_endSeen, 1);
    char why[64];
    snprintf(why, sizeof why, "process exit, code %ld", code);
    LogStack(why);
    SessionExit(code, false);
    SessionNotesToLog();  // nothing after a normal end (the marker is gone)
    oRtlExitUserProcess(code);
}

BOOL WINAPI TerminateHook(HANDLE p, UINT code) {
    if (p == GetCurrentProcess() || GetProcessId(p) == GetCurrentProcessId()) {
        InterlockedExchange(&g_endSeen, 1);
        char why[64];
        snprintf(why, sizeof why, "TerminateProcess(self), code %u", code);
        LogStack(why);
        SessionExit(static_cast<long>(code), true);
        SessionNotesToLog();
    }
    return oTerminateProcess(p, code);
}

// Two ends that leave no other trace: a fail-fast through the API, and NtTerminateProcess called past
// TerminateProcess. Each writes one line with the module that called into the session marker, the stack into the log.
// (The __fastfail instruction itself has no function to hook.)
void CallerOf(void* ret, char* out, size_t n) {
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(ret), &m) && m)
        GetModuleFileNameA(m, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    snprintf(out, n, "%s+0x%llx", base ? base + 1 : name,
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ret) - reinterpret_cast<uintptr_t>(m)));
}

using RaiseFailFast_t = void(WINAPI*)(PEXCEPTION_RECORD, PCONTEXT, DWORD);
RaiseFailFast_t oRaiseFailFast;
void WINAPI FailFastHook(PEXCEPTION_RECORD rec, PCONTEXT ctx, DWORD flags) {
    char from[MAX_PATH + 32], line[MAX_PATH + 128];
    CallerOf(_ReturnAddress(), from, sizeof from);
    snprintf(line, sizeof line, "ended: a fail-fast (RaiseFailFastException, code 0x%08lx), called from %s",
             rec ? static_cast<unsigned long>(rec->ExceptionCode) : 0ul, from);
    SessionNote(line);
    LogStack(line);
    SessionNotesToLog();
    oRaiseFailFast(rec, ctx, flags);
}

using NtTerminate_t = LONG(NTAPI*)(HANDLE, LONG);
NtTerminate_t oNtTerminate;
LONG NTAPI NtTerminateHook(HANDLE p, LONG code) {
    // NtTerminateProcess(nullptr) ends the other threads only (the normal exit does that first)
    bool self = p == GetCurrentProcess() || (p && GetProcessId(p) == GetCurrentProcessId());
    if (self && InterlockedExchange(&g_endSeen, 1) == 0) {
        char from[MAX_PATH + 32], line[MAX_PATH + 128];
        CallerOf(_ReturnAddress(), from, sizeof from);
        snprintf(line, sizeof line, "ended: NtTerminateProcess of the game itself, code 0x%08lx, called from %s",
                 static_cast<unsigned long>(code), from);
        SessionNote(line);
        LogStack(line);
        SessionNotesToLog();
    }
    return oNtTerminate(p, code);
}

// DLC manager trace: when does the engine scan for .reddlc files and mount the DLC mounters?
using Pass4_t = void* (*)(void*, void*, void*, void*);
Pass4_t oScan, oOnGameStarting;
void* ScanHook(void* self, void* type, void* flag, void* a4) {
    Log("trace: CDLCManager::Scan(type %llu, flag %llu)", reinterpret_cast<uint64_t>(type) & 0xffffffff,
        reinterpret_cast<uint64_t>(flag) & 0xff);
    return oScan(self, type, flag, a4);
}
void* OnGameStartingHook(void* self, void* a2, void* a3, void* a4) {
    Log("trace: CDLCManager::OnGameStarting");
    return oOnGameStarting(self, a2, a3, a4);
}

void HookExport(const wchar_t* dll, const char* fn, void* detour, void** orig) {
    void* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(dll), fn));
    if (!target || !Hook(target, detour, orig)) Log("diag: %s not hooked", fn);
}

}  // namespace

void InstallDiagnostics() {
    HookExport(L"ntdll.dll", "RtlExitUserProcess", reinterpret_cast<void*>(&ExitHook),
               reinterpret_cast<void**>(&oRtlExitUserProcess));
    if (oRtlExitUserProcess) StartSession();  // the normal end deletes the marker only through this hook
    HookExport(L"kernelbase.dll", "TerminateProcess", reinterpret_cast<void*>(&TerminateHook),
               reinterpret_cast<void**>(&oTerminateProcess));
    HookExport(L"kernelbase.dll", "RaiseFailFastException", reinterpret_cast<void*>(&FailFastHook),
               reinterpret_cast<void**>(&oRaiseFailFast));
    HookExport(L"ntdll.dll", "NtTerminateProcess", reinterpret_cast<void*>(&NtTerminateHook),
               reinterpret_cast<void**>(&oNtTerminate));
    HookNamed("CDLCManager::Scan", reinterpret_cast<void*>(&ScanHook), reinterpret_cast<void**>(&oScan));
    HookNamed("CDLCManager::OnGameStarting", reinterpret_cast<void*>(&OnGameStartingHook),
              reinterpret_cast<void**>(&oOnGameStarting));
}

}  // namespace tw3se
