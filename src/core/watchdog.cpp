// TW3SE core - the watchdog: finds out what is going on when the game hangs.
//
//  - Crumbs: a lock-free ring of the last 8192 steps TW3SE took (the pose and mimic hooks, calls into the engine,
//    Lua chunks, hooks, plugin callbacks, the phases of a tick), each with time, thread and up to three values.
//    Crumb() costs a few nanoseconds and works on any thread.
//  - Doing: what each thread is doing inside TW3SE right now (DoingScope), readable from another thread.
//  - The watch thread looks at the main thread's tick every 250 ms. When it stands for `hang_seconds` (tw3se.config,
//    default 8), it writes tw3se\out\hang_<time>.txt: the stall, every thread's stack (module+offset, TW3SE's
//    functions by name from tw3se.pdb when it is there, engine addresses by their name in addresses.inc), what each
//    thread does in TW3SE, the suspend log and the crumbs. A minidump (hang_<time>.dmp) only with hang_dump = on.
//    A second report follows after 30 s of the same stall, so a changing hang shows as such. While a save loads the
//    limit is `start_hang_seconds`: loading stands the main thread longer than 8 s by design.
//    Before the first tick (a game that dies while it starts leaves no other trace) one report comes when the
//    game has not ticked `start_hang_seconds` (default 180) after TW3SE's start: a start that hangs.
//  - The suspend log: every NtSuspendThread / NtResumeThread in the process with the caller's stack (config
//    suspend_log = off switches it off).
//  - `!status`, `!dump`, `!crumbs [n]`, `!suspends [n]` on the pipe run on the pipe's own thread (console.cpp): they
//    answer while the main thread hangs.
#include "core.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#pragma comment(lib, "dbghelp.lib")

namespace tw3se {
namespace {

struct CrumbRec {
    uint64_t qpc;
    uint32_t tid;
    const char* what;
    uintptr_t a, b, c;
};
constexpr uint32_t kCrumbs = 8192;
CrumbRec g_crumbs[kCrumbs];
std::atomic<uint32_t> g_crumbIdx{0};
double g_qpcPerMs = 1;

struct DoingSlot {
    std::atomic<uint32_t> tid{0};
    std::atomic<const char*> what{nullptr};
    std::atomic<uintptr_t> arg{0};
    std::atomic<uint64_t> since{0};
};
constexpr int kSlots = 256;
DoingSlot g_doing[kSlots];

DoingSlot* SlotFor(uint32_t tid, bool create) {
    uint32_t h = (tid * 2654435761u) % kSlots;
    for (int i = 0; i < kSlots; ++i) {
        DoingSlot& s = g_doing[(h + i) % kSlots];
        uint32_t t = s.tid.load(std::memory_order_relaxed);
        if (t == tid) return &s;
        if (t == 0) {
            if (!create) return nullptr;
            uint32_t zero = 0;
            if (s.tid.compare_exchange_strong(zero, tid)) return &s;
            if (zero == tid) return &s;
        }
    }
    return nullptr;
}

uint64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

// Every NtSuspendThread / NtResumeThread in the process, with the caller's stack (in a hang threads can stand suspended
// and the main thread wait for them; this says who suspended them). A ring of its own,
// so the tick crumbs cannot push it out; tools/watch.py reads it from outside.
struct SuspendRec {
    uint64_t qpc;
    uint32_t tid, target;  // the calling thread, the thread it suspends or resumes
    uint32_t prev, kind;   // the suspend count before the call (0xffffffff: failed); kind 1 suspend, 2 resume, 0 half-written
    uintptr_t frames[8];   // the callers, innermost first
};
constexpr uint32_t kSuspends = 2048;
SuspendRec g_suspends[kSuspends];
std::atomic<uint32_t> g_suspendIdx{0};
uintptr_t g_sysLo[3] = {}, g_sysHi[3] = {};  // ntdll, KernelBase, kernel32: their frames are not "the caller"

using NtThreadCall_t = LONG(NTAPI*)(HANDLE, PULONG);
NtThreadCall_t g_ntSuspend = nullptr, g_ntResume = nullptr;

// The stack is taken before the call: the unwinder's lookup must not meet a lock of the thread just suspended.
struct Callers {
    void* fr[10] = {};
    USHORT n = 0;
};

void NoteSuspend(uint32_t kind, HANDLE t, LONG st, ULONG prev, const Callers& c) {
    uint32_t i = g_suspendIdx.fetch_add(1, std::memory_order_relaxed) % kSuspends;
    SuspendRec& r = g_suspends[i];
    r.kind = 0;
    r.qpc = Qpc();
    r.tid = GetCurrentThreadId();
    r.target = GetThreadId(t);
    r.prev = st >= 0 ? prev : 0xffffffffu;
    uintptr_t caller = 0;
    for (int k = 0; k < 8; ++k) r.frames[k] = k < c.n ? reinterpret_cast<uintptr_t>(c.fr[k]) : 0;
    for (USHORT k = 0; k < c.n && !caller; ++k) {
        uintptr_t a = reinterpret_cast<uintptr_t>(c.fr[k]);
        bool sys = false;
        for (int m = 0; m < 3; ++m) sys |= a >= g_sysLo[m] && a < g_sysHi[m];
        if (!sys) caller = a;
    }
    std::atomic_thread_fence(std::memory_order_release);
    r.kind = kind;
    Crumb(kind == 1 ? "thread suspended" : "thread resumed", r.target, caller, r.prev);
}

LONG NTAPI NtSuspendHook(HANDLE t, PULONG prevOut) {
    Callers c;
    c.n = RtlCaptureStackBackTrace(1, 10, c.fr, nullptr);  // past the hook
    ULONG prev = 0;
    LONG st = g_ntSuspend(t, &prev);
    if (prevOut) *prevOut = prev;
    NoteSuspend(1, t, st, prev, c);
    return st;
}

LONG NTAPI NtResumeHook(HANDLE t, PULONG prevOut) {
    Callers c;
    c.n = RtlCaptureStackBackTrace(1, 10, c.fr, nullptr);
    ULONG prev = 0;
    LONG st = g_ntResume(t, &prev);
    if (prevOut) *prevOut = prev;
    NoteSuspend(2, t, st, prev, c);
    return st;
}

}  // namespace

void Crumb(const char* what, uintptr_t a, uintptr_t b, uintptr_t c) {
    uint32_t i = g_crumbIdx.fetch_add(1, std::memory_order_relaxed) % kCrumbs;
    CrumbRec& r = g_crumbs[i];
    r.what = nullptr;  // a reader skips a half-written entry
    r.qpc = Qpc();
    r.tid = GetCurrentThreadId();
    r.a = a, r.b = b, r.c = c;
    std::atomic_thread_fence(std::memory_order_release);
    r.what = what;
}

DoingScope::DoingScope(const char* what, uintptr_t arg) {
    DoingSlot* s = SlotFor(GetCurrentThreadId(), true);
    slot_ = s;
    if (!s) return;
    prevWhat_ = s->what.load(std::memory_order_relaxed);
    prevArg_ = s->arg.load(std::memory_order_relaxed);
    s->arg.store(arg, std::memory_order_relaxed);
    s->since.store(GetTickCount64(), std::memory_order_relaxed);
    s->what.store(what, std::memory_order_release);
}
DoingScope::~DoingScope() {
    if (!slot_) return;
    auto s = static_cast<DoingSlot*>(slot_);
    s->arg.store(prevArg_, std::memory_order_relaxed);
    s->what.store(prevWhat_, std::memory_order_release);
}

namespace {

HANDLE g_proc = nullptr;
bool g_symOk = false;
uint32_t g_hangMs = 8000;
bool g_dump = false;  // config hang_dump = on: a minidump next to the report (in-process, it can stand itself)
uintptr_t g_selfLo = 0, g_selfHi = 0;

struct Mod {
    uintptr_t lo, hi;
    std::string name;
};
std::vector<Mod> Modules() {
    std::vector<Mod> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return out;
    MODULEENTRY32W me{};
    me.dwSize = sizeof me;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        char n[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, n, sizeof n, nullptr, nullptr);
        out.push_back({reinterpret_cast<uintptr_t>(me.modBaseAddr), reinterpret_cast<uintptr_t>(me.modBaseAddr) + me.modBaseSize, n});
    }
    CloseHandle(snap);
    return out;
}

std::string Where(const std::vector<Mod>& mods, uintptr_t a) {
    char b[400];
    for (const Mod& m : mods) {
        if (a < m.lo || a >= m.hi) continue;
        snprintf(b, sizeof b, "%s+0x%llx", m.name.c_str(), static_cast<unsigned long long>(a - m.lo));
        std::string s = b;
        if (a >= g_selfLo && a < g_selfHi && g_symOk) {  // our own function names from tw3se.pdb
            alignas(SYMBOL_INFO) char sb[sizeof(SYMBOL_INFO) + 256] = {};
            auto si = reinterpret_cast<SYMBOL_INFO*>(sb);
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 disp = 0;
            if (SymFromAddr(g_proc, a, &disp, si)) {
                snprintf(b, sizeof b, "  (%s+0x%llx)", si->Name, static_cast<unsigned long long>(disp));
                s += b;
            }
        } else if (m.lo == ImageBase()) {
            std::string n = NearestAddressName(a);
            if (!n.empty()) s += "  (" + n + ")";
        }
        return s;
    }
    snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(a));
    return b;
}

std::string ThreadName(HANDLE t) {
    using GetDesc_t = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static auto getDesc = reinterpret_cast<GetDesc_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    if (!getDesc) return "";
    PWSTR d = nullptr;
    std::string out;
    if (SUCCEEDED(getDesc(t, &d)) && d) {
        char n[200];
        WideCharToMultiByte(CP_UTF8, 0, d, -1, n, sizeof n, nullptr, nullptr);
        out = n;
        LocalFree(d);
    }
    return out;
}

// Every thread's stack. A thread is suspended only for GetThreadContext (nothing is allocated while it stands, so
// a thread holding the heap lock cannot block us); the walk reads its stack afterwards, which a hung thread leaves
// as it is.
std::string Stacks(const std::vector<Mod>& mods) {
    std::string out;
    DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return "no thread list\r\n";
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    std::vector<DWORD> tids;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self) tids.push_back(te.th32ThreadID);
    CloseHandle(snap);
    DWORD mainTid = MainThreadId();
    std::sort(tids.begin(), tids.end(), [&](DWORD a, DWORD b) { return (a == mainTid) > (b == mainTid); });
    for (DWORD tid : tids) {
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (!t) continue;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        bool got = false;
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            got = GetThreadContext(t, &ctx) != 0;
            ResumeThread(t);
        }
        std::vector<uintptr_t> pcs;
        if (got) {
            STACKFRAME64 sf{};
            sf.AddrPC.Offset = ctx.Rip;
            sf.AddrPC.Mode = AddrModeFlat;
            sf.AddrFrame.Offset = ctx.Rbp;
            sf.AddrFrame.Mode = AddrModeFlat;
            sf.AddrStack.Offset = ctx.Rsp;
            sf.AddrStack.Mode = AddrModeFlat;
            for (int i = 0; i < 48; ++i) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, g_proc, t, &sf, &ctx, nullptr, SymFunctionTableAccess64,
                                 SymGetModuleBase64, nullptr) || !sf.AddrPC.Offset)
                    break;
                pcs.push_back(static_cast<uintptr_t>(sf.AddrPC.Offset));
            }
        }
        bool ours = false;
        for (uintptr_t pc : pcs)
            if (pc >= g_selfLo && pc < g_selfHi) ours = true;
        char head[200];
        std::string nm = ThreadName(t);
        snprintf(head, sizeof head, "thread %lu%s%s%s%s\r\n", tid, tid == mainTid ? "  MAIN" : "", ours ? "  (in TW3SE)" : "",
                 nm.empty() ? "" : "  ", nm.c_str());
        out += head;
        for (uintptr_t pc : pcs) out += "    " + Where(mods, pc) + "\r\n";
        CloseHandle(t);
    }
    return out;
}

std::string DoingText() {
    std::string out;
    uint64_t now = GetTickCount64();
    for (DoingSlot& s : g_doing) {
        uint32_t tid = s.tid.load();
        const char* w = s.what.load();
        if (!tid || !w) continue;
        char b[300];
        snprintf(b, sizeof b, "thread %lu%s: %s (0x%llx) for %llu ms\r\n", tid, tid == MainThreadId() ? " MAIN" : "", w,
                 static_cast<unsigned long long>(s.arg.load()), static_cast<unsigned long long>(now - s.since.load()));
        out += b;
    }
    return out.empty() ? "no thread is inside TW3SE\r\n" : out;
}

std::string CrumbText(uint32_t n) {
    std::string out;
    uint32_t end = g_crumbIdx.load();
    if (n > kCrumbs) n = kCrumbs;
    if (n > end) n = end;
    uint64_t now = Qpc();
    for (uint32_t k = end - n; k != end; ++k) {
        const CrumbRec& r = g_crumbs[k % kCrumbs];
        const char* w = r.what;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (!w) continue;
        char b[300];
        snprintf(b, sizeof b, "%9.1f ms ago  thread %-6lu %-32s %llx %llx %llx\r\n",
                 static_cast<double>(now - r.qpc) / g_qpcPerMs, r.tid, w, static_cast<unsigned long long>(r.a),
                 static_cast<unsigned long long>(r.b), static_cast<unsigned long long>(r.c));
        out += b;
    }
    return out;
}

uintptr_t FirstCaller(const SuspendRec& r) {  // the first frame outside ntdll, KernelBase and kernel32
    for (uintptr_t a : r.frames) {
        bool sys = false;
        for (int m = 0; m < 3; ++m) sys |= a >= g_sysLo[m] && a < g_sysHi[m];
        if (a && !sys) return a;
    }
    return r.frames[0];
}

// The suspends and resumes, oldest first; at the end the threads someone suspended and nobody resumed since.
std::string SuspendText(const std::vector<Mod>& mods, uint32_t n) {
    std::string out;
    uint32_t end = g_suspendIdx.load();
    if (n > kSuspends) n = kSuspends;
    if (n > end) n = end;
    uint64_t now = Qpc();
    struct Open {
        uint32_t target, by;
        uintptr_t caller;
        int count;
    };
    std::vector<Open> open;
    for (uint32_t k = end - n; k != end; ++k) {
        const SuspendRec& r = g_suspends[k % kSuspends];
        uint32_t kind = r.kind;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (!kind) continue;
        char b[200];
        snprintf(b, sizeof b, "%9.1f ms ago  thread %-6lu %s thread %-6lu (count before %ld)\r\n",
                 static_cast<double>(now - r.qpc) / g_qpcPerMs, r.tid, kind == 1 ? "SUSPENDS" : "resumes ", r.target,
                 static_cast<long>(r.prev));
        out += b;
        for (uintptr_t f : r.frames)
            if (f) out += "        " + Where(mods, f) + "\r\n";
        if (r.prev == 0xffffffffu) continue;
        auto it = std::find_if(open.begin(), open.end(), [&](const Open& o) { return o.target == r.target; });
        if (it == open.end()) it = open.insert(open.end(), {r.target, r.tid, 0, 0});
        it->count += kind == 1 ? 1 : -1;
        if (kind == 1) it->by = r.tid, it->caller = FirstCaller(r);
    }
    out += "\r\n[suspended in the window above and not resumed]\r\n";
    bool any = false;
    for (const Open& o : open) {
        if (o.count <= 0) continue;
        any = true;
        char b[160];
        snprintf(b, sizeof b, "thread %-6lu +%d, last by thread %lu from ", o.target, o.count, o.by);
        out += b + Where(mods, o.caller) + "\r\n";
    }
    if (!any) out += "(none)\r\n";
    return out;
}

std::string Status() {
    char b[300];
    uint64_t last = LastTickMs();
    snprintf(b, sizeof b, "main thread %lu, last tick %llu ms ago, %u crumb(s), hang report after %u ms\r\n", MainThreadId(),
             last ? static_cast<unsigned long long>(GetTickCount64() - last) : 0ULL, g_crumbIdx.load(), g_hangMs);
    return b;
}

std::wstring Stamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t b[64];
    swprintf_s(b, L"%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

// The newest `keep` reports stay in out\, the older ones go (.txt and .dmp). Loading a save stands the main thread
// past 8 s on many PCs: 1 to 3 reports per start, about 300 KB each (hundreds of reports
// after a few weeks). The names begin with the time, so sorted by name is sorted by age.
void KeepNewestReports(const std::wstring& dir, size_t keep) {
    std::vector<std::wstring> bases;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\hang_*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        size_t dot = n.rfind(L'.');
        if (dot != std::wstring::npos) bases.push_back(n.substr(0, dot));
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(bases.begin(), bases.end());
    bases.erase(std::unique(bases.begin(), bases.end()), bases.end());
    for (size_t i = 0; i + keep < bases.size(); ++i) {
        DeleteFileW((dir + L"\\" + bases[i] + L".txt").c_str());
        DeleteFileW((dir + L"\\" + bases[i] + L".dmp").c_str());
    }
}

// Writes the report (and a minidump); returns the report's path.
std::wstring Report(const char* why) {
    std::wstring dir = Dir() + L"\\out";
    CreateDirectoryW(dir.c_str(), nullptr);
    KeepNewestReports(dir, 19);  // with this one 20
    std::wstring base = dir + L"\\hang_" + Stamp();
    std::vector<Mod> mods = Modules();
    // First what needs no other thread (the watch thread can stand in the stacks or the minidump of a hung game), then the stacks, and the minidump only on request (config hang_dump).
    std::string r = std::string("TW3SE watchdog report: ") + why + "\r\n" + Status() + "\r\n[what each thread does in TW3SE]\r\n" +
                    DoingText() + "\r\n[the script functions the main thread runs, innermost first]\r\n" +
                    ScriptStackText(true) + "\r\n[thread suspends and resumes, the last 300, oldest first]\r\n" + SuspendText(mods, 300) +
                    "\r\n[the last 2000 crumbs, oldest first]\r\n" + CrumbText(2000);
    HANDLE h = CreateFileW((base + L".txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, r.data(), static_cast<DWORD>(r.size()), &w, nullptr);
        FlushFileBuffers(h);
        std::string s = "\r\n[stacks]\r\n" + Stacks(mods);
        WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &w, nullptr);
        CloseHandle(h);
    }
    if (!g_dump) return base + L".txt";
    HANDLE d = CreateFileW((base + L".dmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (d != INVALID_HANDLE_VALUE) {
        MiniDumpWriteDump(g_proc, GetCurrentProcessId(), d,
                          static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                                                     MiniDumpWithIndirectlyReferencedMemory),
                          nullptr, nullptr, nullptr);
        CloseHandle(d);
    }
    return base + L".txt";
}

uint64_t g_startMs = 0;      // when the watch thread began (the end of TW3SE's start-up)
uint32_t g_startHangMs = 180000;

DWORD WINAPI WatchThread(void*) {
    uint64_t reportedFor = 0, secondDone = 0;
    bool startDone = false;
    for (;;) {
        Sleep(250);
        uint64_t last = LastTickMs();
        uint64_t now = GetTickCount64();
        if (!last) {  // no tick yet (the game is still starting): one report when the start takes far too long
            if (!startDone && g_startHangMs && now - g_startMs >= g_startHangMs) {
                startDone = true;
                char why[120];
                snprintf(why, sizeof why, "no tick %llu s after TW3SE's start (the game's start hangs, or is very slow)",
                         static_cast<unsigned long long>((now - g_startMs) / 1000));
                std::wstring p = Report(why);
                Log("WATCHDOG: %s - report %ls", why, p.c_str());
            }
            continue;
        }
        // While a save loads (inside OnGameStart or RestoreSession, lifecycle.cpp) the main thread stands for many
        // seconds by design: every load wrote a report, and one of them was read as the reason a game ended.
        // There the limit is the start's.
        bool loading = GameStarting();
        uint32_t limit = loading && g_startHangMs > g_hangMs ? g_startHangMs : g_hangMs;
        if (now - last < limit) {
            reportedFor = secondDone = 0;
            continue;
        }
        if (reportedFor != last) {
            reportedFor = last;
            char why[120];
            snprintf(why, sizeof why, "the main thread stands for %llu ms%s", static_cast<unsigned long long>(now - last),
                     loading ? " while a save loads" : "");
            std::wstring p = Report(why);
            Log("WATCHDOG: the main thread stands for %llu ms - report %ls", static_cast<unsigned long long>(now - last), p.c_str());
        } else if (!secondDone && now - last >= limit + 30000) {
            secondDone = 1;
            std::wstring p = Report("the same stall, 30 s later");
            Log("WATCHDOG: still standing after %llu ms - second report %ls", static_cast<unsigned long long>(now - last),
                p.c_str());
        }
    }
}

}  // namespace

// `!status`, `!dump`, `!crumbs [n]`, `!doing`: on the pipe's thread (console.cpp), so they answer during a hang.
std::string WatchdogCommand(const std::string& line) {
    std::string a = line;
    if (a == "status") return Status() + DoingText();
    if (a == "doing") return DoingText();
    if (a == "dump") {
        std::wstring p = Report("asked for (!dump)");
        char b[MAX_PATH * 2];
        WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, b, sizeof b, nullptr, nullptr);
        return std::string("report written: ") + b;
    }
    if (a.rfind("crumbs", 0) == 0) {
        int n = a.size() > 7 ? atoi(a.c_str() + 7) : 40;
        return CrumbText(n > 0 ? static_cast<uint32_t>(n) : 40);
    }
    if (a.rfind("suspends", 0) == 0) {
        int n = a.size() > 9 ? atoi(a.c_str() + 9) : 60;
        return SuspendText(Modules(), n > 0 ? static_cast<uint32_t>(n) : 60);
    }
    return "usage: !status | !doing | !crumbs [n] | !suspends [n] | !dump";
}

void InstallWatchdog() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcPerMs = static_cast<double>(f.QuadPart) / 1000.0;
    g_proc = GetCurrentProcess();
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&InstallWatchdog), &self);
    g_selfLo = reinterpret_cast<uintptr_t>(self);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_selfLo + reinterpret_cast<const IMAGE_DOS_HEADER*>(g_selfLo)->e_lfanew);
    g_selfHi = g_selfLo + nt->OptionalHeader.SizeOfImage;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    g_symOk = SymInitializeW(g_proc, (Dir()).c_str(), TRUE) != 0;
    std::string s = ConfigGet("hang_seconds", "8");
    int sec = atoi(s.c_str());
    if (sec <= 0) {
        Log("watchdog: off (hang_seconds = %s)", s.c_str());
        return;
    }
    g_hangMs = static_cast<uint32_t>(sec) * 1000;
    g_dump = ConfigGet("hang_dump", "off") == "on";
    if (ConfigGet("suspend_log", "on") != "off") {  // who suspends threads: NtSuspendThread / NtResumeThread
        const wchar_t* sys[3] = {L"ntdll.dll", L"KernelBase.dll", L"kernel32.dll"};
        for (int m = 0; m < 3; ++m) {
            auto b = reinterpret_cast<uintptr_t>(GetModuleHandleW(sys[m]));
            if (!b) continue;
            auto h = reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
            g_sysLo[m] = b, g_sysHi[m] = b + h->OptionalHeader.SizeOfImage;
        }
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        void* s = nt ? reinterpret_cast<void*>(GetProcAddress(nt, "NtSuspendThread")) : nullptr;
        void* r = nt ? reinterpret_cast<void*>(GetProcAddress(nt, "NtResumeThread")) : nullptr;
        bool ok = s && r && Hook(s, reinterpret_cast<void*>(&NtSuspendHook), reinterpret_cast<void**>(&g_ntSuspend)) &&
                  Hook(r, reinterpret_cast<void*>(&NtResumeHook), reinterpret_cast<void**>(&g_ntResume));
        Log("watchdog: suspend log %s", ok ? "on" : "NOT hooked");
    }
    RegisterCommand("hangtest", [](const char* args, char* reply, uint32_t size) {
        int s = args && *args ? atoi(args) : 12;  // holds the main thread on purpose: the watchdog has to report it
        if (s < 1 || s > 60) s = 12;
        Crumb("hangtest: the main thread sleeps", static_cast<uintptr_t>(s));
        DoingScope d("hangtest (sleeping on purpose)", static_cast<uintptr_t>(s));
        Sleep(static_cast<DWORD>(s) * 1000);
        snprintf(reply, size, "slept %d s on the main thread", s);
    }, "hangtest [s]: hold the main thread s seconds (default 12) - the watchdog writes a report");
    int startSec = atoi(ConfigGet("start_hang_seconds", "180").c_str());
    g_startHangMs = startSec > 0 ? static_cast<uint32_t>(startSec) * 1000 : 0;
    g_startMs = GetTickCount64();
    HANDLE t = CreateThread(nullptr, 0, &WatchThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    Log("watchdog: on (a report after %d s without a tick; symbols %s)", sec, g_symOk ? "on" : "off");
}

}  // namespace tw3se
