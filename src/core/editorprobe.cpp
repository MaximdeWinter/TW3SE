// TW3SE core - REDkit editor probes (diagnostics, each off unless its config key is on).
//
// attachprobe = on: hooks eb's candidate for CEntity::AttachToWorld in editor.exe and logs what its parameters
// are (C++ type name read from the vtable, when there is one), so one spawn in Play shows whether it is the entity
// attach. The function takes three parameters in the editor (retail: entity, world); a fourth is passed through
// unread, as the lifecycle hooks do.
#include "core.h"
#include <array>
#include <atomic>
#include <utility>

extern "C" EXCEPTION_DISPOSITION __cdecl __C_specific_handler(struct _EXCEPTION_RECORD*, void*, struct _CONTEXT*,
                                                           struct _DISPATCHER_CONTEXT*);

namespace tw3se {

// The faults a TW3SE guard takes past the game's crash handler: a bad read, write or jump, a bad instruction, a
// division by zero. A stack overflow is not among them: the guard would have no stack left to run on.
bool GuardableFault(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return true;
    default:
        return false;
    }
}

namespace {

using Attach_t = void* (*)(void*, void*, void*, void*);
Attach_t oAttach = nullptr;
std::atomic<int> g_calls{0};

// The C++ type name of an object with a vtable (MSVC RTTI: vtable[-1] -> complete object locator -> type descriptor,
// name at +0x10), or null. Reads only, calls nothing: the parameter may be any pointer. No C++ objects here (SEH).
const char* ClassNameGuarded(void* p) {
    // every read asks MemReadable first: in the editor a fault ends the process before the __except runs
    __try {
        if (!p || !MemReadable(p, 8)) return nullptr;
        void** vtbl = *reinterpret_cast<void***>(p);
        if (!MemReadable(vtbl - 1, 8)) return nullptr;
        const uint32_t* col = reinterpret_cast<const uint32_t*>(vtbl[-1]);
        if (!MemReadable(col, 24)) return nullptr;
        uintptr_t base = reinterpret_cast<uintptr_t>(col) - col[5];  // pSelf: the locator's own RVA
        if (base != ImageBase()) return nullptr;                      // not a type of editor.exe
        const char* name = reinterpret_cast<const char*>(base + col[3] + 0x10);
        return MemReadable(name, 4) ? name : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* AttachProbe(void* a, void* b, void* c, void* d) {
    int n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 40 || n % 500 == 0) {
        const char* ca = ClassNameGuarded(a);
        const char* cb = ClassNameGuarded(b);
        const char* cc = ClassNameGuarded(c);
        Log("attachprobe #%d: p1=%p (%s) p2=%p (%s) p3=%p (%s)", n, a, ca ? ca : "-", b, cb ? cb : "-", c,
            cc ? cc : "-");
    }
    return oAttach(a, b, c, d);
}

// --- the editor's crash handler and TW3SE's guards
//
// The REDkit editor registers a first vectored exception handler (errorWindows.cpp, eb: EditorVectoredHandler_ed).
// It takes every access violation before any __except in the process runs: a fault that TW3SE's guard turns into
// `false` in the game writes a crash dump and ends the editor (the first enginedump). TW3SE sits in front of
// that handler: when a frame of tw3se.dll that has an exception handler is on the faulting stack, the violation goes
// past the editor's handler to the normal SEH dispatch, where TW3SE's __except takes it. Everything else goes to
// the editor's handler as before; a violation no TW3SE guard takes still reaches the editor's unhandled filter.
using Vectored_t = LONG(CALLBACK*)(EXCEPTION_POINTERS*);
Vectored_t oVectored = nullptr;
uintptr_t g_selfBase = 0, g_selfEnd = 0;

// The walk stays inside this thread's stack and never runs twice at once on a thread: a fault inside it (an unwind
// through a broken frame) came back here, walked again and faulted again until the stack was gone (the
// editor's own crash in its Wwise streaming thread would become a 32 MB stack overflow instead of its crash report).
thread_local bool t_walking = false;

bool Tw3seGuardOnStack(const CONTEXT* fault) {
    if (t_walking) return false;  // a fault in the walk itself: the editor's handler takes it
    struct Flag { Flag() { t_walking = true; } ~Flag() { t_walking = false; } } flag;
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const DWORD64 lo = reinterpret_cast<DWORD64>(tib->StackLimit), hi = reinterpret_cast<DWORD64>(tib->StackBase);
    CONTEXT ctx = *fault;
    for (int depth = 0; depth < 48 && ctx.Rip; ++depth) {
        if (ctx.Rsp < lo || ctx.Rsp + 8 > hi) return false;  // left the stack: no TW3SE frame found on it
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!f) {  // a leaf: return address on top of the stack
            if (!MemReadable(reinterpret_cast<void*>(ctx.Rsp), 8)) return false;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        const bool ours = ctx.Rip >= g_selfBase && ctx.Rip < g_selfEnd;
        PEXCEPTION_ROUTINE handler = RtlVirtualUnwind(UNW_FLAG_EHANDLER, imageBase, ctx.Rip, f, &ctx, &handlerData,
                                                      &establisher, nullptr);
        // a TW3SE function with a __try/__except on the stack (only SEH counts: a C++ frame handler is on
        // nearly every TW3SE function, also on TickHook, and a real crash of the game below it belongs to the game)
        if (ours && handler == reinterpret_cast<PEXCEPTION_ROUTINE>(&__C_specific_handler)) return true;
    }
    return false;
}

bool GuardTakes(EXCEPTION_POINTERS* ep) {
    return ep && ep->ExceptionRecord && ep->ContextRecord &&
           GuardableFault(ep->ExceptionRecord->ExceptionCode) && Tw3seGuardOnStack(ep->ContextRecord);
}

// --- the same in the game. The game's vectored handler (witcher3.exe, its crash report) runs before any __except too
// and suspends every other thread first. When TW3SE's guard then took the fault, nobody resumed them, and the next
// tick waited for the frozen workers forever (the 1b hang: IsScriptable read a garbage owner; the suspend log
// showed the main thread suspending 31 threads from witcher3.exe+27fc68a, called by ntdll's exception dispatch).
// Every vectored handler registered after TW3SE's start gets a thunk in front: a fault inside a TW3SE guard goes past
// it to the SEH dispatch, everything else reaches the handler as before.
// A handler that is removed frees its slot: AMD's driver (atcuf64.dll) adds and removes the same handler hundreds of
// times a minute, and with slots that stayed taken every handler after the 32nd went uncovered (and each one was a
// line in the log, 400 000 in an evening). A freed slot is taken again last (the next free one after the slot
// handed out before), so a thunk still running on another thread for the removed handler hardly ever meets a new one.
constexpr int kVehSlots = 64;
std::atomic<PVECTORED_EXCEPTION_HANDLER> g_vehOrig[kVehSlots];
std::atomic<PVOID> g_vehHandle[kVehSlots];  // what RtlAddVectoredExceptionHandler gave back for the slot's thunk
std::atomic<bool> g_vehUsed[kVehSlots];
std::atomic<int> g_vehNext{0};
std::atomic<int> g_vehUncovered{0};

// A thread inside TW3SE's notes of a fault (below): a fault in them goes to the handler without them again. 12.11.:
// dred's note faulted, came back here, faulted again, until the stack was gone.
thread_local bool t_inNotes = false;

template <int I>
LONG CALLBACK VehThunk(EXCEPTION_POINTERS* ep) {
    if (GuardTakes(ep)) return EXCEPTION_CONTINUE_SEARCH;
    PVECTORED_EXCEPTION_HANDLER h = g_vehOrig[I].load(std::memory_order_acquire);
    if (!h) return EXCEPTION_CONTINUE_SEARCH;  // removed while this exception ran
    if (!t_inNotes) {
        struct Flag { Flag() { t_inNotes = true; } ~Flag() { t_inNotes = false; } } flag;  // (/EHa: also when unwound)
        ScriptStackOnFault(ep);  // a fault the game's crash handler gets: the script stack first, while it still stands
        QuarantineOnFault(ep);   // the freed block it touched, with its free's stack (quarantine on)
        DredOnFault();           // a crash after the GPU dropped the device: what the GPU did (dred on)
        SessionFault(ep);        // and where it was into tw3se\out\session.txt, for the crash report at the next start
    }
    return h(ep);
}
template <int... I>
constexpr std::array<PVECTORED_EXCEPTION_HANDLER, sizeof...(I)> MakeVehThunks(std::integer_sequence<int, I...>) {
    return {&VehThunk<I>...};
}
const std::array<PVECTORED_EXCEPTION_HANDLER, kVehSlots> g_vehThunks = MakeVehThunks(std::make_integer_sequence<int, kVehSlots>{});

using AddVeh_t = PVOID(NTAPI*)(ULONG, PVECTORED_EXCEPTION_HANDLER);
using RemoveVeh_t = ULONG(NTAPI*)(PVOID);
AddVeh_t oAddVeh = nullptr;
RemoveVeh_t oRemoveVeh = nullptr;

// guardtest veh: a handler of TW3SE that is covered like a foreign one
LONG CALLBACK NoVeh(EXCEPTION_POINTERS*) { return EXCEPTION_CONTINUE_SEARCH; }

int TakeVehSlot() {
    int start = g_vehNext.load(std::memory_order_relaxed);
    for (int k = 0; k < kVehSlots; ++k) {
        int i = (start + k) % kVehSlots;
        bool no = false;
        if (g_vehUsed[i].compare_exchange_strong(no, true)) {
            g_vehNext.store((i + 1) % kVehSlots, std::memory_order_relaxed);
            return i;
        }
    }
    return -1;
}

PVOID NTAPI AddVehHook(ULONG first, PVECTORED_EXCEPTION_HANDLER h) {
    auto a = reinterpret_cast<uintptr_t>(h);
    if (!h || (a >= g_selfBase && a < g_selfEnd && h != &NoVeh)) return oAddVeh(first, h);  // TW3SE's own (hwwatch) stay as they are
    // the same handler again (a driver adding and removing it all the time) is logged once
    static std::atomic<PVECTORED_EXCEPTION_HANDLER> lastLogged{nullptr};
    bool tell = lastLogged.exchange(h) != h;
    HMODULE m = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    if (tell && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(h), &m))
        GetModuleFileNameW(m, name, MAX_PATH);
    const wchar_t* base = wcsrchr(name, L'\\');
    int i = TakeVehSlot();
    if (i < 0) {
        int n = g_vehUncovered.fetch_add(1) + 1;
        if (n == 1 || n % 1000 == 0)
            Log("guard: a vectored handler NOT covered (all %d slots taken, %d times so far)", kVehSlots, n);
        return oAddVeh(first, h);
    }
    g_vehOrig[i].store(h, std::memory_order_release);
    PVOID r = oAddVeh(first, g_vehThunks[i]);
    if (!r) {
        g_vehOrig[i].store(nullptr, std::memory_order_release);
        g_vehUsed[i].store(false);
        return r;
    }
    g_vehHandle[i].store(r, std::memory_order_release);
    if (tell)
        Log("guard: vectored handler %ls+0x%llx (first=%lu) gets TW3SE's guard in front (the same one again is not "
            "logged)", base ? base + 1 : name, static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(m)), first);
    return r;
}

ULONG NTAPI RemoveVehHook(PVOID handle) {
    ULONG r = oRemoveVeh(handle);
    if (handle) {
        for (int i = 0; i < kVehSlots; ++i) {
            PVOID want = handle;
            if (g_vehHandle[i].compare_exchange_strong(want, nullptr)) {
                g_vehOrig[i].store(nullptr, std::memory_order_release);
                g_vehUsed[i].store(false, std::memory_order_release);
                break;
            }
        }
    }
    return r;
}

bool SetSelfRange() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GuardTakes), &self);
    if (!self) return false;
    g_selfBase = reinterpret_cast<uintptr_t>(self);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_selfBase +
                                                          reinterpret_cast<const IMAGE_DOS_HEADER*>(g_selfBase)->e_lfanew);
    g_selfEnd = g_selfBase + nt->OptionalHeader.SizeOfImage;
    return true;
}

LONG CALLBACK VectoredFront(EXCEPTION_POINTERS* ep) {
    if (HwWatchVectored(ep) == EXCEPTION_CONTINUE_EXECUTION) return EXCEPTION_CONTINUE_EXECUTION;  // a hwwatch hit
    if (GuardTakes(ep)) return EXCEPTION_CONTINUE_SEARCH;  // to the SEH dispatch: TW3SE's guard takes it
    return oVectored(ep);
}

// `guardtest`: one deliberate read fault inside a TW3SE guard. "caught" = the editor guard works.
bool FaultGuarded() {
    __try {
        volatile int v = *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x10));
        (void)v;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

void CmdGuardTest(const char* args, char* reply, uint32_t size) {
    if (args && !strcmp(args, "veh")) {  // a handler added and removed 500 times, as AMD's driver does: no slot stays
        auto taken = [] {
            int n = 0;
            for (auto& u : g_vehUsed) n += u.load() ? 1 : 0;
            return n;
        };
        int before = taken(), uncovered = g_vehUncovered.load();
        for (int k = 0; k < 500; ++k) RemoveVectoredExceptionHandler(AddVectoredExceptionHandler(1, &NoVeh));
        snprintf(reply, size, "guardtest veh: slots taken %d before, %d after 500 adds and removes; %d not covered",
                 before, taken(), g_vehUncovered.load() - uncovered);
        return;
    }
    strncpy_s(reply, size, FaultGuarded() ? "guardtest: caught" : "guardtest: no fault?", _TRUNCATE);
}

}  // namespace

void InstallEditorGuard() {
    RegisterCommand("guardtest", &CmdGuardTest, "guardtest: one deliberate read fault inside a TW3SE guard");
    if (!SetSelfRange()) return;
    HookNamed("EditorVectoredHandler_ed", reinterpret_cast<void*>(&VectoredFront), reinterpret_cast<void**>(&oVectored));
    Log("editor guard: %s", oVectored ? "TW3SE's guards take their own faults before the editor's crash handler"
                                      : "the editor's crash handler was not found - a fault in a guard ends the editor");
}

void InstallGameGuard() {
    RegisterCommand("guardtest", &CmdGuardTest, "guardtest: one deliberate read fault inside a TW3SE guard");
    if (!SetSelfRange()) return;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    void* add = nt ? reinterpret_cast<void*>(GetProcAddress(nt, "RtlAddVectoredExceptionHandler")) : nullptr;
    // the remove hook first: a handler added through the add hook must free its slot when it goes
    void* remove = nt ? reinterpret_cast<void*>(GetProcAddress(nt, "RtlRemoveVectoredExceptionHandler")) : nullptr;
    if (!remove || !Hook(remove, reinterpret_cast<void*>(&RemoveVehHook), reinterpret_cast<void**>(&oRemoveVeh)))
        Log("guard: RtlRemoveVectoredExceptionHandler not hooked - a removed handler keeps its slot");
    bool ok = add && Hook(add, reinterpret_cast<void*>(&AddVehHook), reinterpret_cast<void**>(&oAddVeh));
    Log("guard: %s", ok ? "vectored handlers registered from now on let TW3SE's guards take their own faults"
                       : "RtlAddVectoredExceptionHandler not hooked - a fault in a guard reaches the game's crash handler");
}

void InstallEditorProbes() {
    if (!ConfigBool("attachprobe", false)) return;
    HookNamed("CEntity::AttachToWorld_candidate_ed", reinterpret_cast<void*>(&AttachProbe),
              reinterpret_cast<void**>(&oAttach));
    Log("attachprobe: %s", oAttach ? "hooked" : "not hooked");
}

}  // namespace tw3se
