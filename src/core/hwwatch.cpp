// TW3SE core - hardware watchpoints (diagnostics for the analysis side, off until a `hwwatch` command or config).
//
// Finds the code behind a live moment that a static search cannot pin down (REDkit: the foliage senders,
// the content packs, Lua's array grow, the quest block activation): the four debug registers of the CPU, set on
// every thread of the process, raise a single-step exception when a function starts (x) or a field is written (w)
// or read/written (rw). A vectored handler that runs before the editor's crash handler (editorprobe.cpp) counts
// each hit by its stack (RIP and 8 return addresses) and keeps the registers of the first hit. `hwwatch stop`
// writes the stacks, most frequent first, to tw3se\out\hwwatch_<slot>.txt as module+RVA.
//
//   hwwatch x <where>                 who calls a function, with which rcx/rdx/r8/r9
//   hwwatch w|rw <where> <1|2|4|8>    who writes / reads a field
//   hwwatch stop [slot|all]           disarm and write the file;  hwwatch dump: write without disarming
//   <where>: a name from the address table, a hex address, a hex number below 0x10000000 (an RVA of the exe),
//            [expr] (the pointer stored there), and sums: [GContentManager]+0x50, [GGame]+CCommonGame.dlcManager
// Config hwwatch1 .. hwwatch4 = "<x|w|rw> <where> [len]": armed at start-up as soon as <where> resolves (a global
// pointer that is still null is retried every 100 ms). hwwatch_max = hits per slot before it disarms (20000).
//
// The handler runs on the faulting thread, possibly with engine locks held: it calls nothing that allocates or
// locks, reads the stack only inside the thread's own stack bounds, and inserts into a fixed table.
#include "core.h"
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace tw3se {
namespace {

constexpr int kSlots = 4;
constexpr int kFrames = 9;     // RIP + 8 return addresses
constexpr int kTable = 2048;   // distinct stacks kept per process (all slots together)

struct Slot {
    std::atomic<int> state{0};  // 0 free, 1 pending (where not resolved yet), 2 armed, 3 full (sweeper disarms)
    int kind = 0;               // 0 execute, 1 write, 3 read/write (the DR7 R/W bits)
    int len = 1;
    uintptr_t addr = 0;
    std::string where;
    std::atomic<long> hits{0};
    long max = 20000;
    ULONGLONG since = 0;
};

struct Hit {
    volatile long used;  // 0 free, 1 being written, 2 complete
    int slot;
    DWORD tid;
    long count;
    uint64_t frames[kFrames];
    uint64_t regs[4];   // rcx rdx r8 r9 of the first hit
    uint64_t value;     // the watched field after the first hit (write watches)
};

Slot g_slot[kSlots];
Hit g_hits[kTable];
std::atomic<int> g_dropped{0};
std::atomic<bool> g_sweeper{false};
std::atomic<int> g_sweepsAfterStop{0};
std::atomic<ULONGLONG> g_quietUntil{0};
std::atomic<int> g_passes{0};  // sweeper passes done: a command waits for two, then every thread has the registers  // after a stop: stale registers on threads the sweeper has not reached
PVOID g_veh = nullptr;
// set while this thread is inside the handler: a hit there (the handler's own stack walk touching a watched field)
// must not record again (reading a read-watched field in Record raises a hit inside Record, and the thread would
// wait forever for the table entry it is itself writing)
thread_local bool t_inHandler = false;

// --- the address expression

uintptr_t ParseExpr(const char*& s, bool first, bool* pending, std::string* err);

uintptr_t ParseTerm(const char*& s, bool first, bool* pending, std::string* err) {
    while (*s == ' ') ++s;
    if (*s == '[') {
        ++s;
        uintptr_t p = ParseExpr(s, true, pending, err);
        while (*s == ' ') ++s;
        if (*s != ']') {
            *err = "missing ]";
            return 0;
        }
        ++s;
        if (!p || !MemReadable(reinterpret_cast<void*>(p), 8)) {
            *pending = true;
            return 0;
        }
        uintptr_t v = *reinterpret_cast<uintptr_t*>(p);
        if (!v) *pending = true;
        return v;
    }
    if (*s >= '0' && *s <= '9') {
        char* end = nullptr;
        uintptr_t v = std::strtoull(s, &end, 16);
        s = end;
        return first && v < 0x10000000 ? ImageBase() + v : v;  // a small leading number is an RVA of the exe
    }
    std::string name;
    while (*s && (isalnum(static_cast<unsigned char>(*s)) || *s == '_' || *s == ':' || *s == '.')) name += *s++;
    if (name.empty()) {
        *err = std::string("unexpected '") + (*s ? *s : '?') + "'";
        return 0;
    }
    uintptr_t v = Address(name.c_str());
    if (!v) *err = name + " is not in the address table";
    return v;
}

uintptr_t ParseExpr(const char*& s, bool first, bool* pending, std::string* err) {
    uintptr_t v = ParseTerm(s, first, pending, err);
    for (;;) {
        while (*s == ' ') ++s;
        if (*s != '+') return v;
        ++s;
        v += ParseTerm(s, false, pending, err);
    }
}

// --- the debug registers of every thread

uint64_t WantedDr7() {
    uint64_t dr7 = 0;
    for (int i = 0; i < kSlots; ++i) {
        if (g_slot[i].state.load() != 2) continue;
        const int len = g_slot[i].len;
        const uint64_t lenBits = g_slot[i].kind == 0 ? 0 : len == 1 ? 0 : len == 2 ? 1 : len == 8 ? 2 : 3;
        dr7 |= 1ull << (2 * i);
        dr7 |= static_cast<uint64_t>(g_slot[i].kind) << (16 + 4 * i);
        dr7 |= lenBits << (18 + 4 * i);
    }
    return dr7;
}

// Sets DR0-3/DR7 on every other thread of the process. Allocates only before the first thread is suspended.
void ApplyAll() {
    std::vector<DWORD> tids;
    tids.reserve(512);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) tids.push_back(te.th32ThreadID);
    CloseHandle(snap);
    const uint64_t dr7 = WantedDr7();
    uintptr_t addr[kSlots];
    for (int i = 0; i < kSlots; ++i) addr[i] = g_slot[i].state.load() == 2 ? g_slot[i].addr : 0;
    for (DWORD tid : tids) {
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
        if (!t) continue;
        if (SuspendThread(t) != static_cast<DWORD>(-1)) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c) &&
                (c.Dr7 != dr7 || c.Dr0 != addr[0] || c.Dr1 != addr[1] || c.Dr2 != addr[2] || c.Dr3 != addr[3])) {
                c.Dr0 = addr[0];
                c.Dr1 = addr[1];
                c.Dr2 = addr[2];
                c.Dr3 = addr[3];
                c.Dr6 = 0;
                c.Dr7 = dr7;
                SetThreadContext(t, &c);
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
}

bool TryResolve(Slot& s) {
    bool pending = false;
    std::string err;
    const char* p = s.where.c_str();
    uintptr_t a = ParseExpr(p, true, &pending, &err);
    if (pending || !err.empty() || !a) return false;
    if (s.kind != 0 && (a & (s.len - 1))) a &= ~static_cast<uintptr_t>(s.len - 1);  // the CPU needs it aligned
    s.addr = a;
    s.since = GetTickCount64();
    s.hits = 0;
    s.state = 2;
    Log("hwwatch %d: %s %s -> %p (%d bytes)", static_cast<int>(&s - g_slot), s.kind == 0 ? "x" : s.kind == 1 ? "w" : "rw",
        s.where.c_str(), reinterpret_cast<void*>(a), s.len);
    return true;
}

void WriteFile(int i);

// Keeps new threads covered, resolves pending slots, disarms full ones. Runs while any slot is in use.
DWORD WINAPI Sweeper(void*) {
    for (;;) {
        bool any = false, changed = false;
        for (int i = 0; i < kSlots; ++i) {
            int st = g_slot[i].state.load();
            if (st == 1 && TryResolve(g_slot[i])) changed = true;
            if (st == 3) {
                WriteFile(i);
                Log("hwwatch %d: %ld hits, disarmed (hwwatch_max)", i, g_slot[i].hits.load());
                g_slot[i].state = 0;
                g_sweepsAfterStop = 10;
                g_quietUntil = GetTickCount64() + 5000;
                changed = true;
            }
            if (g_slot[i].state.load() != 0) any = true;
        }
        (void)changed;
        ApplyAll();
        ++g_passes;
        if (!any && g_sweepsAfterStop.fetch_sub(1) <= 0) break;  // a few passes more: threads that missed the stop
        Sleep(any ? 250 : 100);
    }
    g_sweeper = false;
    return 0;
}

void EnsureSweeper() {
    bool expected = false;
    if (g_sweeper.compare_exchange_strong(expected, true)) {
        HANDLE t = CreateThread(nullptr, 0, &Sweeper, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        else g_sweeper = false;
    }
}

// The registers are set by the sweeper (a thread cannot set its own, and commands run on the main thread, the one
// that matters most). A command returns once two passes have run: armed or disarmed on every thread by then.
void WaitForSweeps() {
    const int p0 = g_passes.load();
    EnsureSweeper();
    for (int n = 0; n < 400 && g_passes.load() < p0 + 2; ++n) Sleep(5);
}

// --- the handler

void Record(int slot, const CONTEXT* fault) {
    NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    const uintptr_t lo = reinterpret_cast<uintptr_t>(tib->StackLimit), hi = reinterpret_cast<uintptr_t>(tib->StackBase);
    uint64_t frames[kFrames] = {};
    CONTEXT ctx = *fault;
    for (int d = 0; d < kFrames && ctx.Rip; ++d) {
        frames[d] = ctx.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!f) {
            if (ctx.Rsp < lo || ctx.Rsp + 8 > hi) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        } else {
            void* hd = nullptr;
            DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, f, &ctx, &hd, &est, nullptr);
        }
        if (ctx.Rsp < lo || ctx.Rsp >= hi) break;
    }
    uint64_t h = 1469598103934665603ull ^ slot;
    for (uint64_t f : frames) h = (h ^ f) * 1099511628211ull;
    for (int probe = 0; probe < kTable; ++probe) {
        Hit& e = g_hits[(h + probe) % kTable];
        long u = e.used;
        if (u == 0 && InterlockedCompareExchange(&e.used, 1, 0) == 0) {
            e.slot = slot;
            e.tid = GetCurrentThreadId();
            e.count = 1;
            for (int k = 0; k < kFrames; ++k) e.frames[k] = frames[k];
            e.regs[0] = fault->Rcx;
            e.regs[1] = fault->Rdx;
            e.regs[2] = fault->R8;
            e.regs[3] = fault->R9;
            e.value = 0;
            if (g_slot[slot].kind == 1) {  // a written field (reading it is no hit; a read watch would hit again)
                const uintptr_t a = g_slot[slot].addr;
                const int n = g_slot[slot].len;
                e.value = n == 1 ? *reinterpret_cast<volatile uint8_t*>(a)
                        : n == 2 ? *reinterpret_cast<volatile uint16_t*>(a)
                        : n == 4 ? *reinterpret_cast<volatile uint32_t*>(a)
                                 : *reinterpret_cast<volatile uint64_t*>(a);
            }
            InterlockedExchange(&e.used, 2);
            return;
        }
        for (int spin = 0; e.used == 1; ++spin) {
            if (spin > 100000) {  // another thread is stuck halfway (never expected): give the hit up
                ++g_dropped;
                return;
            }
            YieldProcessor();
        }
        if (e.slot == slot && std::equal(frames, frames + kFrames, e.frames)) {
            InterlockedIncrement(&e.count);
            return;
        }
    }
    ++g_dropped;
}

}  // namespace

// EXCEPTION_CONTINUE_EXECUTION for a hit of an armed slot, EXCEPTION_CONTINUE_SEARCH for anything else. Called
// first by the editor's crash handler hook (editorprobe.cpp) and registered as a first vectored handler.
LONG CALLBACK HwWatchVectored(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord ||
        ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    const bool haveDr = (c->ContextFlags & CONTEXT_DEBUG_REGISTERS) == CONTEXT_DEBUG_REGISTERS;
    int slot = -1;
    for (int i = 0; i < kSlots && slot < 0; ++i) {
        const int st = g_slot[i].state.load(std::memory_order_relaxed);
        if (st != 2 && st != 3) continue;
        if (haveDr ? (c->Dr6 & (1ull << i)) != 0 : g_slot[i].kind == 0 ? c->Rip == g_slot[i].addr : true) slot = i;
    }
    if (slot < 0) {
        // a stale register on a thread the sweeper has not reached yet: an execute hit of a freed slot repeats
        // forever without the resume flag, a data hit is harmless
        if ((haveDr && (c->Dr6 & 0xf)) || GetTickCount64() < g_quietUntil.load(std::memory_order_relaxed)) {
            c->Dr6 = 0;
            c->EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    Slot& s = g_slot[slot];
    if (s.state.load(std::memory_order_relaxed) == 2 && !t_inHandler) {
        t_inHandler = true;
        Record(slot, c);
        t_inHandler = false;
        if (s.hits.fetch_add(1, std::memory_order_relaxed) + 1 >= s.max) s.state = 3;
    }
    if (haveDr) c->Dr6 = 0;
    if (s.kind == 0) c->EFlags |= 0x10000;  // RF: run the instruction at the breakpoint without hitting it again
    return EXCEPTION_CONTINUE_EXECUTION;
}

namespace {

std::string ModRva(uint64_t a) {
    if (!a) return "-";
    HMODULE m = nullptr;
    char buf[64];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(a), &m) && m) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(m, path, MAX_PATH);
        std::wstring w = path;
        w = w.substr(w.find_last_of(L"\\/") + 1);
        std::string n(w.begin(), w.end());
        _snprintf_s(buf, sizeof buf, _TRUNCATE, "%s+%llx", n.c_str(),
                    static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(m)));
        return buf;
    }
    _snprintf_s(buf, sizeof buf, _TRUNCATE, "%llx", static_cast<unsigned long long>(a));
    return buf;
}

void WriteFile(int i) {
    const Slot& s = g_slot[i];
    std::vector<const Hit*> rows;
    for (const Hit& e : g_hits)
        if (e.used == 2 && e.slot == i) rows.push_back(&e);
    std::sort(rows.begin(), rows.end(), [](const Hit* a, const Hit* b) { return a->count > b->count; });
    CreateDirectoryW((Dir() + L"\\out").c_str(), nullptr);
    wchar_t name[64];
    swprintf_s(name, L"\\out\\hwwatch_%d.txt", i);
    FILE* f = nullptr;
    _wfopen_s(&f, (Dir() + name).c_str(), L"w");
    if (!f) return;
    fprintf(f, "hwwatch %d: %s %s -> %s (%llx), %d bytes, build %08x\n", i,
            s.kind == 0 ? "x" : s.kind == 1 ? "w" : "rw", s.where.c_str(), ModRva(s.addr).c_str(),
            static_cast<unsigned long long>(s.addr), s.len, GameBuild());
    fprintf(f, "%ld hits in %.1f s, %zu distinct stacks, %d not kept (table full)\n", s.hits.load(),
            (GetTickCount64() - s.since) / 1000.0, rows.size(), g_dropped.load());
    fprintf(f, "frames: RIP first (a data watch stops after the accessing instruction), then return addresses\n\n");
    for (const Hit* e : rows) {
        fprintf(f, "%ld x  thread %lu  rcx=%llx rdx=%llx r8=%llx r9=%llx", e->count, e->tid,
                static_cast<unsigned long long>(e->regs[0]), static_cast<unsigned long long>(e->regs[1]),
                static_cast<unsigned long long>(e->regs[2]), static_cast<unsigned long long>(e->regs[3]));
        if (s.kind == 1) fprintf(f, "  value=%llx", static_cast<unsigned long long>(e->value));
        fprintf(f, "\n");
        for (int k = 0; k < kFrames && e->frames[k]; ++k) fprintf(f, "    %s\n", ModRva(e->frames[k]).c_str());
    }
    fclose(f);
}

void ClearHits(int i) {
    for (Hit& e : g_hits)
        if (e.used == 2 && e.slot == i) e.used = 0;
}

// "<x|w|rw> <where> [len]" into a free slot. Returns the slot or -1 (err filled).
int Arm(const std::string& spec, bool waitForPointer, std::string* err) {
    char kind[8] = {};
    int consumed = 0;
    if (sscanf_s(spec.c_str(), " %7s %n", kind, static_cast<unsigned>(sizeof kind), &consumed) != 1) {
        *err = "usage: hwwatch <x|w|rw> <where> [1|2|4|8]";
        return -1;
    }
    const int k = !strcmp(kind, "x") ? 0 : !strcmp(kind, "w") ? 1 : !strcmp(kind, "rw") ? 3 : -1;
    if (k < 0) {
        *err = "kind must be x, w or rw";
        return -1;
    }
    std::string rest = spec.substr(consumed);
    int len = k == 0 ? 1 : 8;
    size_t sp = rest.find_last_of(' ');
    if (sp != std::string::npos) {
        std::string tail = rest.substr(sp + 1);
        if (tail == "1" || tail == "2" || tail == "4" || tail == "8") {
            len = k == 0 ? 1 : std::atoi(tail.c_str());
            rest = rest.substr(0, sp);
        }
    }
    while (!rest.empty() && rest.back() == ' ') rest.pop_back();
    int i = 0;
    for (; i < kSlots; ++i) {
        int expected = 0;
        if (g_slot[i].state.compare_exchange_strong(expected, 1)) break;
    }
    if (i == kSlots) {
        *err = "all four debug registers are in use (hwwatch stop <slot>)";
        return -1;
    }
    Slot& s = g_slot[i];
    s.kind = k;
    s.len = len;
    s.where = rest;
    s.max = std::atol(ConfigGet("hwwatch_max", "20000").c_str());
    if (s.max <= 0) s.max = 20000;
    ClearHits(i);
    if (!TryResolve(s)) {
        bool pending = false;
        const char* p = s.where.c_str();
        ParseExpr(p, true, &pending, err);
        if (!waitForPointer || !err->empty()) {
            if (err->empty()) *err = s.where + " does not resolve yet (a null pointer)";
            s.state = 0;
            return -1;
        }
        Log("hwwatch %d: %s waits for its pointer", i, s.where.c_str());
        EnsureSweeper();
        return i;
    }
    if (waitForPointer) EnsureSweeper();  // start-up: nothing waits for it
    else WaitForSweeps();
    return i;
}

void CmdHwWatch(const char* args, char* reply, uint32_t size) {
    std::string a = args ? args : "";
    while (!a.empty() && a.front() == ' ') a.erase(0, 1);
    if (a.empty() || a == "status") {
        std::string out;
        for (int i = 0; i < kSlots; ++i) {
            const Slot& s = g_slot[i];
            char line[256];
            static const char* const kState[] = {"free", "waiting", "armed", "full"};
            _snprintf_s(line, sizeof line, _TRUNCATE, "%d %s %s %s %ld hits; ", i, kState[s.state.load()],
                        s.state.load() ? (s.kind == 0 ? "x" : s.kind == 1 ? "w" : "rw") : "",
                        s.state.load() ? s.where.c_str() : "", s.hits.load());
            out += line;
        }
        strncpy_s(reply, size, out.c_str(), _TRUNCATE);
        return;
    }
    if (a.rfind("stop", 0) == 0 || a.rfind("dump", 0) == 0) {
        const bool stop = a[0] == 's';
        std::string which = a.size() > 4 ? a.substr(5) : "all";
        std::string out;
        for (int i = 0; i < kSlots; ++i) {
            if (which != "all" && std::atoi(which.c_str()) != i) continue;
            if (g_slot[i].state.load() == 0) continue;
            if (stop) g_slot[i].state = 0;
            WriteFile(i);
            char line[96];
            _snprintf_s(line, sizeof line, _TRUNCATE, "slot %d: %ld hits -> out\\hwwatch_%d.txt; ", i,
                        g_slot[i].hits.load(), i);
            out += line;
        }
        if (stop) {
            g_sweepsAfterStop = 10;
            g_quietUntil = GetTickCount64() + 5000;
            WaitForSweeps();
        }
        strncpy_s(reply, size, out.empty() ? "hwwatch: no slot in use" : out.c_str(), _TRUNCATE);
        return;
    }
    std::string err;
    int i = Arm(a, false, &err);
    if (i < 0) {
        _snprintf_s(reply, size, _TRUNCATE, "hwwatch: %s", err.c_str());
        return;
    }
    _snprintf_s(reply, size, _TRUNCATE, "hwwatch %d: %s -> %s, armed on all threads", i, a.c_str(),
                ModRva(g_slot[i].addr).c_str());
}

}  // namespace

std::string HwWatchCommand(const char* args) {
    char reply[1024] = {};
    CmdHwWatch(args, reply, sizeof reply);
    return reply;
}

void InstallHwWatch() {
    RegisterCommand("hwwatch", &CmdHwWatch,
                    "hwwatch <x|w|rw> <where> [len] | stop [slot] | dump [slot]: hardware watchpoint -> out\\hwwatch_<slot>.txt");
    g_veh = AddVectoredExceptionHandler(1, &HwWatchVectored);
    for (int n = 1; n <= kSlots; ++n) {
        char key[16];
        _snprintf_s(key, sizeof key, _TRUNCATE, "hwwatch%d", n);
        std::string spec = ConfigGet(key, "");
        if (spec.empty()) continue;
        std::string err;
        if (Arm(spec, true, &err) < 0) Log("hwwatch: config %s = %s: %s", key, spec.c_str(), err.c_str());
    }
}

}  // namespace tw3se
