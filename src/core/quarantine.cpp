// Quarantine of freed memory (config `quarantine = hold | poison`, off by default): a debugging aid for crashes that
// come from memory used after it was freed. Memory::Free of the game is hooked; a freed block is not given back at
// once but held for `quarantine_seconds` (5), with the time and the call stack of its free.
//   hold    the block keeps its contents: a crash that stops with it is a use after free
//   poison  the block's first 8 bytes become 0xdddddddddddddddd: the first use through its vtable or first pointer
//           crashes at once, at the place that uses it, and the report says which free it was
//   tagged  every size, held `quarantine_seconds` (2), poisoned with a tag that names its record: the free's stack is
//           found from the poison even after the block went back, and at any fault the last free of each register's
//           block (8M records a thread; with `quarantine_only`)
// Each thread holds its own blocks and gives them back itself (the allocator may keep per-thread caches; no block is
// freed by another thread than the one that freed it, no lock in the hot path). At a fault outside a guard the log
// names the held block the fault address or a register lies in, with its free's stack.
#include "core.h"

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <algorithm>
#include <map>
#include <vector>

namespace tw3se {
namespace {

using Free_t = void (*)(void*);
Free_t oFree = nullptr;
int g_mode = 0;               // 0 off, 1 hold, 2 poison, 3 rebuild (only around the graphics rebuild)
ULONGLONG g_holdMs = 5000;
constexpr int kFrames = 12;  // a free under a few wrappers (Release, a destructor) still shows its caller
constexpr uint32_t kRingSize = 8192;  // per thread

struct Held {
    void* p;
    ULONGLONG tick;
    void* frames[kFrames];
};

struct Ring {
    DWORD tid = 0;
    Held* slots = nullptr;
    std::atomic<uint32_t> head{0}, count{0};  // the oldest at head
    std::atomic<uint64_t> held{0};
};

std::mutex g_ringsMx;
std::vector<Ring*> g_rings;  // every thread's, never freed (read at a fault)
thread_local Ring* t_ring = nullptr;
thread_local bool t_inFree = false;
std::atomic<uint64_t> g_frees{0}, g_passed{0};

// quarantine_only = mods: the modules of TW3SE and its plugins, and the two pass-through hooks that do not count
bool g_onlyMods = false;
std::wstring g_onlyPart = L"\\tw3se\\";  // the modules counted: their path holds this
constexpr int kMaxMods = 64;
uintptr_t g_modLo[kMaxMods], g_modHi[kMaxMods];
std::atomic<int> g_modCount{0};
uintptr_t g_passLo[2], g_passHi[2];

void ReadModRanges() {
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    int n = 0;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && n < kMaxMods; ++i) {
        wchar_t path[MAX_PATH];
        if (!GetModuleFileNameW(mods[i], path, MAX_PATH)) continue;
        for (wchar_t* c = path; *c; ++c) *c = towlower(*c);
        if (!wcsstr(path, L"\\tw3se\\") || !wcsstr(path, g_onlyPart.c_str())) continue;  // TW3SE and its plugins: under bin\x64_dx12\tw3se
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi)) continue;
        g_modLo[n] = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        g_modHi[n] = g_modLo[n] + mi.SizeOfImage;
        ++n;
    }
    g_modCount.store(n, std::memory_order_release);
}

void FunctionRange(void* fn, uintptr_t* lo, uintptr_t* hi) {
    DWORD64 base = 0;
    if (auto* rf = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(fn), &base, nullptr)) {
        *lo = static_cast<uintptr_t>(base + rf->BeginAddress);
        *hi = static_cast<uintptr_t>(base + rf->EndAddress);
    }
}

// The free comes from TW3SE or a plugin: one of its nearest frames lies in their code, outside the pass-through hooks
bool FromMods(void* const* frames) {
    int n = g_modCount.load(std::memory_order_acquire);
    for (int f = 0; f < kFrames && frames[f]; ++f) {
        auto a = reinterpret_cast<uintptr_t>(frames[f]);
        if ((a >= g_passLo[0] && a < g_passHi[0]) || (a >= g_passLo[1] && a < g_passHi[1])) return false;  // under them: the game's
        for (int m = 0; m < n; ++m)
            if (a >= g_modLo[m] && a < g_modHi[m]) return true;
    }
    return false;
}

void ModRangesTick(float, void*) {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    ReadModRanges();
}

Ring* MyRing() {
    if (t_ring) return t_ring;
    auto* r = new Ring();
    r->tid = GetCurrentThreadId();
    r->slots = static_cast<Held*>(VirtualAlloc(nullptr, sizeof(Held) * kRingSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!r->slots) {
        delete r;
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(g_ringsMx);
    g_rings.push_back(r);
    t_ring = r;
    return r;
}

// quarantine = rebuild: the window is open from the rebuild's BEFORE (until ~0) to g_holdMs after its AFTER
std::atomic<ULONGLONG> g_windowUntil{0};
std::atomic<int64_t> g_heldNow{0}, g_heldPeak{0};
std::atomic<uint64_t> g_heldTotal{0};
bool g_rbPoison = false;  // quarantine_poison = on: stacks kept and blocks poisoned (finding the reader)

// One thread's blocks of the windows: a ring of records in address space reserved once, committed in 1 MB steps as
// it fills (never moved: the fault handler reads it while other threads go on). Counters only grow: [head, tail)
// are the records kept, [ret, tail) the blocks still held; a record stays after its block went back.
constexpr size_t kRbMax = size_t(1) << 23;  // 8M records a thread (committed as they fill)
struct RbBuf {
    DWORD tid = 0;
    uint32_t index = 0;  // in g_rbBufs: the poison tag names it
    Held* base = nullptr;
    std::atomic<uint64_t> head{0}, ret{0}, tail{0};
    size_t committed = 0;  // entries from the start of the ring
};
thread_local RbBuf* t_rb = nullptr;
std::vector<RbBuf*> g_rbBufs;  // under g_ringsMx, never freed

RbBuf* MyRb() {
    if (t_rb) return t_rb;
    auto* b = new RbBuf();
    b->tid = GetCurrentThreadId();
    b->base = static_cast<Held*>(VirtualAlloc(nullptr, sizeof(Held) * kRbMax, MEM_RESERVE, PAGE_READWRITE));
    if (!b->base) {
        delete b;
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(g_ringsMx);
    b->index = static_cast<uint32_t>(g_rbBufs.size());
    g_rbBufs.push_back(b);
    t_rb = b;
    return b;
}

// The game's small-block heap (Memory::Free: `mov rcx, [allocator]`, `and rcx, -8`; its range at +0xc900 / +0xc908):
// a block there lies in a 64 KB page whose header holds the block size at +0x20 (the small free reads it from there)
const uintptr_t* g_allocator = nullptr;
size_t SmallBlockSize(void* p) {
    if (!g_allocator) return 0;
    uintptr_t a = *g_allocator & ~uintptr_t(7);
    if (!a) return 0;
    uintptr_t lo = *reinterpret_cast<const uintptr_t*>(a + 0xc900), hi = *reinterpret_cast<const uintptr_t*>(a + 0xc908);
    uintptr_t x = reinterpret_cast<uintptr_t>(p);
    if (x < lo || x >= hi) return 0;
    uint32_t n = *reinterpret_cast<const uint32_t*>((x & ~uintptr_t(0xffff)) + 0x20);
    return n >= 8 && n <= 0x8000 ? n : 0;
}

bool WindowOpen() {
    ULONGLONG until = g_windowUntil.load(std::memory_order_relaxed);
    return until && (until == ~0ull || GetTickCount64() < until);
}

std::string Place(void* a);

// 20.11.: a held block's poison, checked before it goes back. A changed word: something wrote into it after its free
// (a stale pointer of the game or a mod) - the block, its free and what was written are logged (the first 40)
std::atomic<int> g_writesLogged{0};
std::atomic<uint64_t> g_writesFound{0};
void CheckPoison(RbBuf* b, uint64_t rec) {
    if (!g_rbPoison) return;
    const Held& h = b->base[rec % kRbMax];
    uint64_t pat = 0xdddd000000000000ull | (uint64_t(b->index & 0xff) << 40) | (rec & 0xffffffffffull);
    size_t n = SmallBlockSize(h.p), len = n ? n : 8;
    const char* p = static_cast<const char*>(h.p);
    int bad = 0;
    std::string what;
    for (size_t o = 0; o + 8 <= len; o += 8) {
        uint64_t v;
        memcpy(&v, p + o, 8);
        if (v == pat) continue;
        if (bad < 8) {
            char w[96];
            snprintf(w, sizeof w, " +0x%zx=%016llx", o, static_cast<unsigned long long>(v));
            what += w;
            std::string pl = v > 0x10000 && (v >> 48) == 0 ? Place(reinterpret_cast<void*>(v)) : "?";
            if (pl != "?") what += " (" + pl + ")";
        }
        ++bad;
    }
    if (!bad) return;
    ++g_writesFound;
    if (g_writesLogged.fetch_add(1) >= 40) return;
    std::string st;
    for (int i = 0; i < kFrames && h.frames[i]; ++i) st += "\n    " + Place(h.frames[i]);
    Log("quarantine: WRITTEN after its free: block %p (%zu bytes), %d word(s) changed:%s - freed %.2f s before on "
        "thread %lu, from:%s", h.p, len, bad, what.c_str(), (GetTickCount64() - h.tick) / 1000.0, b->tid, st.c_str());
}

// quarantine = small: one size class, every free of it, held by age (not by a window)
size_t g_smallSize = 16;
void SmallFree(void* p) {
    RbBuf* b = t_rb ? t_rb : MyRb();
    if (!b) return oFree(p);
    t_inFree = true;
    ULONGLONG now = GetTickCount64();
    uint64_t head = b->head.load(std::memory_order_relaxed), ret = b->ret.load(std::memory_order_relaxed),
             tail = b->tail.load(std::memory_order_relaxed);
    int k = 0;
    for (; k < 64 && ret < tail && now - b->base[ret % kRbMax].tick >= g_holdMs; ++k, ++ret) {  // the old ones back
        CheckPoison(b, ret);
        oFree(b->base[ret % kRbMax].p);
    }
    if (k) {
        b->ret.store(ret, std::memory_order_release);
        g_heldNow -= k;
    }
    if (tail - head == kRbMax) {  // full: the oldest record goes (its block first, if still held)
        if (ret == head) {
            CheckPoison(b, head);
            oFree(b->base[head % kRbMax].p);
            b->ret.store(++ret, std::memory_order_release);
            --g_heldNow;
        }
        b->head.store(++head, std::memory_order_release);
    }
    if (tail < kRbMax && tail >= b->committed) {
        size_t step = (1 << 20) / sizeof(Held);
        if (!VirtualAlloc(b->base + b->committed, step * sizeof(Held), MEM_COMMIT, PAGE_READWRITE)) {
            oFree(p);
            t_inFree = false;
            return;
        }
        b->committed += step;
    }
    Held& h = b->base[tail % kRbMax];
    h.p = p;
    h.tick = now;
    RtlCaptureStackBackTrace(1, kFrames, h.frames, nullptr);
    uint64_t pat = 0xdddd000000000000ull | (uint64_t(b->index & 0xff) << 40) | (tail & 0xffffffffffull);
    size_t len = g_smallSize;
    if (g_mode == 5) {  // tagged: a small block whole (as CheckPoison reads it), others their first 8 bytes
        size_t n = SmallBlockSize(p);
        len = n ? n : 8;
    }
    for (size_t o = 0; o + 8 <= len; o += 8) memcpy(static_cast<char*>(p) + o, &pat, 8);
    b->tail.store(tail + 1, std::memory_order_release);
    ++g_heldTotal;
    int64_t c = ++g_heldNow, peak = g_heldPeak.load(std::memory_order_relaxed);
    while (c > peak && !g_heldPeak.compare_exchange_weak(peak, c)) {
    }
    t_inFree = false;
}

void RebuildFree(void* p) {
    RbBuf* b = t_rb;
    t_inFree = true;  // the allocator's own bookkeeping goes through at once
    if (WindowOpen() && (b || (b = MyRb()))) {
        uint64_t head = b->head.load(std::memory_order_relaxed), ret = b->ret.load(std::memory_order_relaxed),
                 tail = b->tail.load(std::memory_order_relaxed);
        if (tail - head == kRbMax) {  // full: the oldest record goes (its block first, if still held)
            if (ret == head) {
                CheckPoison(b, head);
                oFree(b->base[head % kRbMax].p);
                b->ret.store(++ret, std::memory_order_release);
                --g_heldNow;
            }
            b->head.store(++head, std::memory_order_release);
        }
        if (tail < kRbMax && tail >= b->committed) {
            size_t step = (1 << 20) / sizeof(Held);
            if (!VirtualAlloc(b->base + b->committed, step * sizeof(Held), MEM_COMMIT, PAGE_READWRITE)) {
                oFree(p);  // no memory for the record: this one goes back at once
                t_inFree = false;
                return;
            }
            b->committed += step;
        }
        Held& h = b->base[tail % kRbMax];
        h.p = p;
        h.tick = GetTickCount64();
        if (g_rbPoison) {
            RtlCaptureStackBackTrace(1, kFrames, h.frames, nullptr);
            uint64_t pat = 0xdddd000000000000ull | (uint64_t(b->index & 0xff) << 40) | (tail & 0xffffffffffull);
            size_t n = SmallBlockSize(p);  // a small block whole (a reader at +0x18 crashes too), others their first 8 bytes
            for (size_t o = 0; o + 8 <= (n ? n : 8); o += 8) memcpy(static_cast<char*>(p) + o, &pat, 8);
        } else {
            h.frames[0] = nullptr;
        }
        b->tail.store(tail + 1, std::memory_order_release);
        ++g_heldTotal;
        int64_t c = ++g_heldNow, peak = g_heldPeak.load(std::memory_order_relaxed);
        while (c > peak && !g_heldPeak.compare_exchange_weak(peak, c)) {
        }
        t_inFree = false;
        return;
    }
    if (b) {  // the window is closed: this thread's held blocks go back, a few at each free; their records stay
        uint64_t ret = b->ret.load(std::memory_order_relaxed), tail = b->tail.load(std::memory_order_relaxed);
        int k = 0;
        for (; k < 256 && ret < tail; ++k, ++ret) {
            CheckPoison(b, ret);
            oFree(b->base[ret % kRbMax].p);
            b->ret.store(ret + 1, std::memory_order_release);
        }
        g_heldNow -= k;
    }
    oFree(p);
    t_inFree = false;
}

size_t PrivateMb() {
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
    return pm.PrivateUsage >> 20;
}

size_t g_mbAtOpen = 0, g_mbPeak = 0;
ULONGLONG g_openedAt = 0;

void RebuildTick(float, void*) {  // the log while the window is open or blocks are held
    static ULONGLONG last = 0;
    static bool active = false;
    ULONGLONG now = GetTickCount64();
    bool open = WindowOpen();
    int64_t held = g_heldNow.load();
    if (!open && held <= 0 && !active) return;
    if (now - last < 1000 && (open || held > 0)) return;
    last = now;
    size_t mb = PrivateMb();
    g_mbPeak = (std::max)(g_mbPeak, mb);
    if (open || held > 0) {
        active = true;
        Log("quarantine: rebuild window %s, %lld block(s) held, private memory %zu MB (%+lld MB since it opened)",
            open ? "open" : "closed, giving back", static_cast<long long>(held), mb,
            static_cast<long long>(mb) - static_cast<long long>(g_mbAtOpen));
        return;
    }
    active = false;
    Log("quarantine: rebuild window done after %.1f s: peak %lld block(s) held, private memory peak %zu MB (%+lld MB "
        "over the start), now %zu MB; %llu block(s) written after their free so far", (now - g_openedAt) / 1000.0,
        static_cast<long long>(g_heldPeak.load()), g_mbPeak,
        static_cast<long long>(g_mbPeak) - static_cast<long long>(g_mbAtOpen), mb,
        static_cast<unsigned long long>(g_writesFound.load()));
    g_heldPeak = 0;
}

// F28: the old buffer of a render-size change stays (a lens flare still points into it); its free's return address
uintptr_t g_keepRet = 0, g_keepCaller = 0;  // the free's return address, and that of the resize's call above it
std::atomic<uint32_t> g_kept{0};
std::atomic<ULONGLONG> g_fixUntil{0};  // ~0 from a rebuild's BEFORE, then 5 s after its AFTER
// rebuild_fix: the frees inside the rebuild call (BEFORE to AFTER, on its thread) stay
bool g_keepCall = false;
std::atomic<DWORD> g_callTid{0};
size_t g_callMbBefore = 0;
// The rebuild fix's blocks stay until `rebuild_fix_seconds` (60) after the last rebuild of a series, then go back to
// the game a part each tick, on the thread that freed them (the resize fix's few stay for good: a lens flare may point
// into its buffer as long as it plays). Kept for good, 32 rebuilds (an upscaler and its frame generation
// switched back and forth) held 5 million blocks, the game's heap ran out and PhysX got no memory (int 3 at
// witcher3.exe+0x1e88295, 22 s after the last rebuild). The readers found read 16 to 26 s after the rebuild.
struct KeptGen {
    DWORD tid = 0;
    std::vector<void*> blocks;
};
std::mutex g_keptMx;
std::deque<KeptGen> g_gens;         // one per rebuild, oldest first
std::vector<void*> g_curCall;       // the rebuild call's frees, only its thread touches it until its AFTER
ULONGLONG g_keepMs = 60000;
std::atomic<ULONGLONG> g_lastRebuild{0};  // the AFTER of the series' last rebuild (0: no series)
std::atomic<ULONGLONG> g_hcRebuildAt{0};  // the heap check's: the last rebuild's AFTER
uint64_t g_keptNow = 0, g_seriesRebuilds = 0, g_seriesKept = 0, g_seriesDropped = 0;  // under g_keptMx
constexpr uint64_t kKeptMax = 2500000;  // about 16 rebuilds: more, and the middle ones of the series go back at once
constexpr size_t kBackPerTick = 20000;
const int64_t* g_heapBytes = nullptr;   // the allocator's count of bytes in use (Memory::Free subtracts from it)

bool g_firstFreeLogged = false;
void* g_firstFree[32] = {};  // the stack of the first free inside a rebuild call, logged after it

long long HeapMb() { return g_heapBytes ? static_cast<long long>(*g_heapBytes >> 20) : -1; }

bool FixWindow() {
    ULONGLONG u = g_fixUntil.load(std::memory_order_relaxed);
    return u && (u == ~0ull || GetTickCount64() < u);
}
constexpr uint32_t kKeepMax = 4096;  // more than that: not the rare case it was found as, frees go through again

void FreeHook(void* p) {
    if (p && g_callTid.load(std::memory_order_relaxed) == GetCurrentThreadId()) {  // inside the rebuild call
        if (g_curCall.empty() && !g_firstFreeLogged) {  // the way from SetVarValue to the rebuild, once a session
            g_firstFreeLogged = true;
            RtlCaptureStackBackTrace(0, 32, g_firstFree, nullptr);
        }
        g_curCall.push_back(p);
        return;
    }
    void* up[4] = {};
    if (p && g_keepRet && reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_keepRet && FixWindow() &&
        RtlCaptureStackBackTrace(0, 4, up, nullptr) &&
        (reinterpret_cast<uintptr_t>(up[1]) == g_keepCaller || reinterpret_cast<uintptr_t>(up[2]) == g_keepCaller ||
         reinterpret_cast<uintptr_t>(up[3]) == g_keepCaller)) {
        uint32_t n = ++g_kept;
        if (n <= kKeepMax) {
            if (n <= 8 || (n & (n - 1)) == 0)
                Log("resize fix: the old buffer of a render-size change after a graphics rebuild is kept (%u so far)", n);
            return;
        }
        if (n == kKeepMax + 1) Log("resize fix: more than %u kept - given back again from now on", kKeepMax);
    }
    if (!p || t_inFree || !g_mode) return oFree(p);
    if (g_mode == 3) return RebuildFree(p);
    if (g_mode == 4) return SmallBlockSize(p) == g_smallSize ? SmallFree(p) : oFree(p);
    if (g_mode == 5) {
        void* f[kFrames] = {};
        if (g_onlyMods && (RtlCaptureStackBackTrace(1, kFrames, f, nullptr), !FromMods(f))) return oFree(p);
        return SmallFree(p);
    }
    ++g_frees;
    Ring* r = MyRing();
    if (!r) {
        ++g_passed;
        return oFree(p);
    }
    void* frames[kFrames] = {};
    if (g_onlyMods) {
        RtlCaptureStackBackTrace(1, kFrames, frames, nullptr);
        if (!FromMods(frames)) {  // the game's own free: back at once, as without the quarantine
            ++g_passed;
            return oFree(p);
        }
    }
    t_inFree = true;  // a free inside the original (the allocator's own bookkeeping) goes through at once
    ULONGLONG now = GetTickCount64();
    // the oldest go back: when the ring is full, or when they were held long enough
    while (r->count.load(std::memory_order_relaxed)) {
        Held& o = r->slots[r->head.load(std::memory_order_relaxed)];
        if (r->count.load(std::memory_order_relaxed) < kRingSize && now - o.tick < g_holdMs) break;
        void* old = o.p;
        o.p = nullptr;
        r->head.store((r->head.load(std::memory_order_relaxed) + 1) % kRingSize, std::memory_order_relaxed);
        r->count.fetch_sub(1, std::memory_order_relaxed);
        oFree(old);
    }
    uint32_t at = (r->head.load(std::memory_order_relaxed) + r->count.load(std::memory_order_relaxed)) % kRingSize;
    Held& h = r->slots[at];
    h.tick = now;
    if (g_onlyMods) memcpy(h.frames, frames, sizeof frames);
    else RtlCaptureStackBackTrace(1, kFrames, h.frames, nullptr);
    if (g_mode == 2) {
        uint64_t pat = 0xddddddddddddddddull;
        memcpy(p, &pat, 8);
    }
    h.p = p;
    r->count.fetch_add(1, std::memory_order_release);
    ++r->held;
    t_inFree = false;
}

std::string Place(void* a) {
    HMODULE m = nullptr;
    char path[MAX_PATH] = "?";
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>(a), &m))
        return "?";
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    char b[MAX_PATH + 32];
    snprintf(b, sizeof b, "%s+0x%llx", base ? base + 1 : path,
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(a) - reinterpret_cast<uintptr_t>(m)));
    return b;
}

// The held block whose start lies at most 0x400 below `x` (the nearest), as a sentence, or empty
std::string FindHeld(uintptr_t x) {
    const Held* best = nullptr;
    DWORD bestTid = 0;
    uintptr_t bestStart = 0;
    std::lock_guard<std::mutex> lk(g_ringsMx);
    for (Ring* r : g_rings) {
        uint32_t n = r->count.load(std::memory_order_acquire), h = r->head.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n && i < kRingSize; ++i) {
            const Held& e = r->slots[(h + i) % kRingSize];
            uintptr_t s = reinterpret_cast<uintptr_t>(e.p);
            if (s && x >= s && x - s < 0x400 && s > bestStart) best = &e, bestStart = s, bestTid = r->tid;
        }
    }
    bool bestBack = false;
    for (RbBuf* b : g_rbBufs) {  // quarantine = rebuild: the records, newest of an address first
        uint64_t head = b->head.load(std::memory_order_acquire), ret = b->ret.load(std::memory_order_acquire),
                 tail = b->tail.load(std::memory_order_acquire);
        for (uint64_t c = head; c < tail; ++c) {
            const Held& e = b->base[c % kRbMax];
            uintptr_t s = reinterpret_cast<uintptr_t>(e.p);
            if (s && x >= s && x - s < 0x400 &&
                (s > bestStart || (s == bestStart && best && e.tick >= best->tick)))
                best = &e, bestStart = s, bestTid = b->tid, bestBack = c < ret;
        }
    }
    if (!best) return {};
    char b[256];
    snprintf(b, sizeof b, "lies 0x%llx into a block freed %.2f s ago on thread %lu%s, freed from:",
             static_cast<unsigned long long>(x - bestStart), (GetTickCount64() - best->tick) / 1000.0, bestTid,
             bestBack ? " (given back since, still poisoned when not used again)" : "");
    std::string r = b;
    for (int i = 0; i < kFrames && best->frames[i]; ++i) r += "\n    " + Place(best->frames[i]);
    return r;
}

std::atomic<int> g_faultsLogged{0};

// A poison value (0xdddd | ring | record): the record it names, as a sentence, or empty (not one, or overwritten)
std::string FindTag(uint64_t v) {
    if ((v >> 48) != 0xdddd) return {};
    uint32_t ring = static_cast<uint32_t>((v >> 40) & 0xff);
    uint64_t rec = v & 0xffffffffffull;
    std::lock_guard<std::mutex> lk(g_ringsMx);
    if (ring >= g_rbBufs.size()) return {};
    RbBuf* b = g_rbBufs[ring];
    uint64_t head = b->head.load(std::memory_order_acquire), tail = b->tail.load(std::memory_order_acquire);
    uint64_t c = (tail & ~0xffffffffffull) | rec;  // the full counter: the newest that ends in these bits
    if (c >= tail) c -= 0x10000000000ull;
    if (c < head || c >= tail) return "a poisoned block whose record is gone (ring overwritten)";
    const Held& e = b->base[c % kRbMax];
    char s[200];
    snprintf(s, sizeof s, "the poison of block %p, freed %.2f s ago on thread %lu, freed from:", e.p,
             (GetTickCount64() - e.tick) / 1000.0, b->tid);
    std::string r = s;
    for (int i = 0; i < kFrames && e.frames[i]; ++i) r += "\n    " + Place(e.frames[i]);
    return r;
}

// The places the held blocks were freed from: the first frame in the counted modules (or the nearest frame), the most first
std::string TopPlaces() {
    std::map<void*, int> count;
    int n = g_modCount.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lk(g_ringsMx);
        for (Ring* r : g_rings) {
            uint32_t c = r->count.load(std::memory_order_acquire), h = r->head.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < c && i < kRingSize; ++i) {
                const Held& e = r->slots[(h + i) % kRingSize];
                void* place = e.frames[0];
                for (int f = 0; f < kFrames && e.frames[f]; ++f) {
                    auto a = reinterpret_cast<uintptr_t>(e.frames[f]);
                    bool mod = false;
                    for (int m = 0; m < n && !mod; ++m) mod = a >= g_modLo[m] && a < g_modHi[m];
                    if (mod) {
                        place = e.frames[f];
                        break;
                    }
                }
                ++count[place];
            }
        }
    }
    std::vector<std::pair<int, void*>> v;
    for (auto& [p, c] : count) v.push_back({c, p});
    std::sort(v.rbegin(), v.rend());
    std::string r;
    for (size_t i = 0; i < v.size() && i < 20; ++i) r += std::to_string(v[i].first) + " " + Place(v[i].second) + "\n";
    return r.empty() ? "nothing held" : r;
}

void CmdQuarantine(const char* args, char* reply, uint32_t size) {
    if (args && !strncmp(args, "top", 3)) {
        snprintf(reply, size, "%s", TopPlaces().c_str());
        return;
    }
    if (args && !strncmp(args, "find ", 5)) {
        std::string s = FindHeld(static_cast<uintptr_t>(strtoull(args + 5, nullptr, 16)));
        snprintf(reply, size, "%s", s.empty() ? "not in a held block" : s.c_str());
        return;
    }
    if (g_mode == 3) {
        snprintf(reply, size, "quarantine rebuild, %llu ms after each rebuild: window %s, %lld held now, %llu held in all",
                 static_cast<unsigned long long>(g_holdMs), WindowOpen() ? "open" : "closed",
                 static_cast<long long>(g_heldNow.load()), static_cast<unsigned long long>(g_heldTotal.load()));
        return;
    }
    uint64_t held = 0, now = 0;
    size_t rings = 0;
    {
        std::lock_guard<std::mutex> lk(g_ringsMx);
        rings = g_rings.size();
        for (Ring* r : g_rings) held += r->held, now += r->count;
    }
    snprintf(reply, size, "quarantine %s%s, %llu ms: %llu frees seen, %llu held in all, %llu held now in %zu thread rings, %llu passed",
             g_mode == 2 ? "poison" : g_mode == 1 ? "hold" : "off", g_onlyMods ? " (only mods)" : "",
             static_cast<unsigned long long>(g_holdMs),
             static_cast<unsigned long long>(g_frees.load()), static_cast<unsigned long long>(held),
             static_cast<unsigned long long>(now), rings, static_cast<unsigned long long>(g_passed.load()));
}

}  // namespace

// The graphics rebuild (lifecycle.cpp, around SetVarValue): 0 before it opens the window, 1 after it starts its end
void QuarantineRebuild(int phase) {
    if (!phase) g_lastRebuild = ~0ull;  // a series goes on (nothing goes back during a rebuild)
    if (g_keepCall) {
        if (!phase) {
            g_curCall.clear();
            g_curCall.reserve(200000);
            g_callMbBefore = PrivateMb();
            g_callTid = GetCurrentThreadId();
        } else if (DWORD tid = g_callTid.exchange(0)) {
            size_t n = g_curCall.size();
            std::lock_guard<std::mutex> lk(g_keptMx);
            if (n) g_gens.push_back({tid, std::move(g_curCall)});
            g_curCall = {};
            g_keptNow += n;
            g_seriesKept += n;
            ++g_seriesRebuilds;
            // too many for the heap: the middle of the series goes back (the first rebuild's frees stay, they are the
            // old data whatever kept a pointer from before the series points into)
            while (g_keptNow > kKeptMax && g_gens.size() > 2) {
                KeptGen& g = g_gens[1];
                if (g.tid != tid) break;
                for (void* b : g.blocks) oFree(b);
                g_keptNow -= g.blocks.size();
                g_seriesDropped += g.blocks.size();
                g_gens.erase(g_gens.begin() + 1);
            }
            Log("rebuild fix: %zu block(s) the rebuild freed are kept until %.0f s after the last rebuild (private memory "
                "%+lld MB, the game's heap %lld MB; %llu kept now)", n, g_keepMs / 1000.0,
                static_cast<long long>(PrivateMb()) - static_cast<long long>(g_callMbBefore), HeapMb(),
                static_cast<unsigned long long>(g_keptNow));
            if (g_firstFree[0]) {
                std::string s;
                for (int i = 0; i < 32 && g_firstFree[i]; ++i) s += "\n    " + Place(g_firstFree[i]);
                Log("rebuild fix: the first free inside the rebuild came from:%s", s.c_str());
                g_firstFree[0] = nullptr;
            }
        }
    }
    if (phase) g_lastRebuild = GetTickCount64();
    if (phase) g_hcRebuildAt = GetTickCount64();
    if (g_keepRet) g_fixUntil = phase ? GetTickCount64() + 5000 : ~0ull;  // the resize fix's window
    if (g_mode != 3) return;
    if (!phase) {
        if (!WindowOpen() && g_heldNow.load() <= 0) {
            g_mbAtOpen = g_mbPeak = PrivateMb();
            g_openedAt = GetTickCount64();
        }
        g_windowUntil = ~0ull;
    } else {
        g_windowUntil = GetTickCount64() + g_holdMs;
    }
}

// The fixes' blocks go back when the series is over: `rebuild_fix_seconds` after its last rebuild and the resize
// fix's window closed, kBackPerTick a tick, those of this thread (the main thread: the rebuild and the first ticks
// after it run there)
void KeepTick(float, void*) {
    ULONGLONG last = g_lastRebuild.load(std::memory_order_relaxed);
    if (!last || last == ~0ull || GetTickCount64() - last < g_keepMs || FixWindow()) return;
    DWORD me = GetCurrentThreadId();
    std::lock_guard<std::mutex> lk(g_keptMx);
    size_t budget = kBackPerTick;
    for (auto it = g_gens.begin(); it != g_gens.end() && budget;) {
        if (it->tid != me) {
            ++it;
            continue;
        }
        while (!it->blocks.empty() && budget) {
            oFree(it->blocks.back());
            it->blocks.pop_back();
            --g_keptNow;
            --budget;
        }
        if (it->blocks.empty()) it = g_gens.erase(it);
        else ++it;
    }
    bool others = false;
    for (const KeptGen& g : g_gens) others = others || g.tid != me;
    if (!g_gens.empty() && !others) return;  // more next tick
    Log("rebuild fix: the series is over (%llu rebuild(s)): %llu block(s) given back to the game%s, the game's heap %lld MB "
        "now", static_cast<unsigned long long>(g_seriesRebuilds), static_cast<unsigned long long>(g_seriesKept),
        g_seriesDropped ? (", " + std::to_string(g_seriesDropped) + " of them early (too many)").c_str() : "", HeapMb());
    if (others) Log("rebuild fix: %llu block(s) freed on another thread stay", static_cast<unsigned long long>(g_keptNow));
    g_gens.clear();  // (those stay allocated: never freed on a thread of their own)
    g_keptNow = 0;
    g_seriesRebuilds = g_seriesKept = g_seriesDropped = 0;
    g_lastRebuild = 0;
}

// ---- heap check (20.11.) ----
size_t g_hcSize = 16;
ULONGLONG g_hcEveryMs = 250;
std::map<uintptr_t, int> g_hcReported;  // page -> 1 (main thread only)
uint64_t g_hcScans = 0, g_hcPages = 0, g_hcLinks = 0;

std::string HcBytes(uintptr_t a, size_t n) {
    std::string r;
    for (size_t o = 0; o < n; o += 8) {
        uint64_t v = 0;
        char line[160];
        if (!SafeRead(reinterpret_cast<void*>(a + o), &v, 8)) {
            snprintf(line, sizeof line, "\n      %016llx  ??", static_cast<unsigned long long>(a + o));
            r += line;
            continue;
        }
        char txt[9];
        for (int i = 0; i < 8; ++i) {
            unsigned char c = static_cast<unsigned char>(v >> (8 * i));
            txt[i] = c >= 32 && c < 127 ? static_cast<char>(c) : '.';
        }
        txt[8] = 0;
        snprintf(line, sizeof line, "\n      %016llx  %016llx  %s", static_cast<unsigned long long>(a + o),
                 static_cast<unsigned long long>(v), txt);
        r += line;
        if (v > 0x10000 && (v >> 48) == 0) {
            std::string pl = Place(reinterpret_cast<void*>(v));
            if (pl != "?") r += "  " + pl;
        }
    }
    return r;
}

// one page: its free list from the head (+0x40), each link on the grid of 16 and inside the page
void HcPage(uintptr_t page) {
    uint64_t link = 0;
    if (!SafeRead(reinterpret_cast<void*>(page + 0x40), &link, 8)) return;
    uintptr_t at = page + 0x40;  // where the link was read
    for (int i = 0; i < 8192 && link; ++i) {
        bool inPage = link >= page + 0x40 && link < page + 0x10000;
        if (!inPage || (link & 0xf)) {
            // read again: an allocation on another thread may have changed it in between
            uint64_t again = 0;
            if (!SafeRead(reinterpret_cast<void*>(at), &again, 8) || again != link) return;
            if (g_hcReported.count(page)) return;
            g_hcReported[page] = 1;
            ULONGLONG rb = g_hcRebuildAt.load();
            char head[320];
            snprintf(head, sizeof head, "heap check: a CROOKED free-list link in the %zu-byte page %p: at %p (the %d. link) "
                     "stands %016llx (%s)%s", g_hcSize, reinterpret_cast<void*>(page), reinterpret_cast<void*>(at), i,
                     static_cast<unsigned long long>(link), inPage ? "off the grid" : "outside the page",
                     rb ? (", " + std::to_string((GetTickCount64() - rb) / 1000.0) + " s after the last graphics rebuild").c_str()
                        : ", no graphics rebuild yet");
            Log("%s; the bytes around the block holding it:%s\n    and around the value:%s", head,
                HcBytes((at & ~uintptr_t(0xf)) - 0x40, 0x80).c_str(), HcBytes((link & ~uintptr_t(0xf)) - 0x20, 0x50).c_str());
            return;
        }
        at = static_cast<uintptr_t>(link);
        ++g_hcLinks;
        if (!SafeRead(reinterpret_cast<void*>(at), &link, 8)) return;
    }
}

void HeapCheckTick(float, void*) {
    static ULONGLONG last = 0, lastLog = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < g_hcEveryMs || !g_allocator) return;
    last = now;
    uintptr_t a = *g_allocator & ~uintptr_t(7);
    if (!a) return;
    uintptr_t lo = *reinterpret_cast<const uintptr_t*>(a + 0xc900), hi = *reinterpret_cast<const uintptr_t*>(a + 0xc908);
    ++g_hcScans;
    for (uintptr_t r = lo; r < hi;) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(reinterpret_cast<void*>(r), &mi, sizeof mi)) break;
        uintptr_t end = reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            for (uintptr_t p = (r + 0xffff) & ~uintptr_t(0xffff); p + 0x48 <= end && p < hi; p += 0x10000) {
                uint32_t size = 0;
                if (!SafeRead(reinterpret_cast<void*>(p + 0x20), &size, 4) || size != g_hcSize) continue;
                ++g_hcPages;
                HcPage(p);
            }
        }
        r = end > r ? end : r + 0x10000;
    }
    if (now - lastLog >= 30000) {
        lastLog = now;
        Log("heap check: %llu scan(s), %llu page visit(s), %llu link(s) followed, %zu crooked page(s) so far",
            static_cast<unsigned long long>(g_hcScans), static_cast<unsigned long long>(g_hcPages),
            static_cast<unsigned long long>(g_hcLinks), g_hcReported.size());
    }
}

// The reader: the faulting instruction and up to 10 callers, unwound from the fault's context
std::string ReaderStack(const CONTEXT& ctx) {
    CONTEXT c = ctx;
    std::string r = "    at " + Place(reinterpret_cast<void*>(c.Rip));
    for (int i = 0; i < 10; ++i) {
        DWORD64 base = 0;
        if (auto* fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr)) {
            void* handlerData = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &handlerData, &frame, nullptr);
        } else {  // a leaf: the return address on top of the stack
            DWORD64 ret = 0;
            if (!SafeRead(reinterpret_cast<void*>(c.Rsp), &ret, 8)) break;
            c.Rip = ret;
            c.Rsp += 8;
        }
        if (c.Rip < 0x10000) break;
        r += "\n    from " + Place(reinterpret_cast<void*>(c.Rip));
    }
    return r;
}

// A fault outside a guard: the held block the fault address or a register points into (three faults logged)
void QuarantineOnFault(EXCEPTION_POINTERS* ep) {
    if (!g_mode || (g_mode == 3 && !g_rbPoison) || !ep || !ep->ExceptionRecord || !ep->ContextRecord) return;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return;
    // poison: only the faults through 0xdddd... (a guard's caught reads elsewhere stay out of it); tagged: every
    // fault, its registers looked up in the records (a block freed and given out again carries no poison any more)
    if (g_mode >= 2 && g_mode != 5) {
        uint64_t fa = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
        if (fa != ~0ull && (fa >> 48) != 0xdddd) return;
    }
    if (g_faultsLogged.fetch_add(1) >= 6) return;
    const CONTEXT& c = *ep->ContextRecord;
    struct R {
        const char* n;
        uint64_t v;
    } regs[] = {{"fault address", ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0},
                {"rax", c.Rax}, {"rbx", c.Rbx}, {"rcx", c.Rcx}, {"rdx", c.Rdx}, {"rsi", c.Rsi}, {"rdi", c.Rdi},
                {"rbp", c.Rbp}, {"r8", c.R8}, {"r9", c.R9}, {"r10", c.R10}, {"r11", c.R11}, {"r12", c.R12},
                {"r13", c.R13}, {"r14", c.R14}, {"r15", c.R15}};
    bool any = false;
    for (const auto& r : regs) {
        if (r.v < 0x10000) continue;
        std::string s = FindHeld(static_cast<uintptr_t>(r.v));
        if (s.empty()) continue;
        Log("quarantine: at the fault, %s (%016llx) %s", r.n, static_cast<unsigned long long>(r.v), s.c_str());
        any = true;
    }
    for (const auto& r : regs) {  // a register that carries a tagged poison names the block and its free
        std::string s = FindTag(r.v);
        if (s.empty()) continue;
        Log("quarantine: at the fault, %s (%016llx) is %s", r.n, static_cast<unsigned long long>(r.v), s.c_str());
        any = true;
    }
    uint64_t a = regs[0].v;
    bool poisoned = g_mode >= 2 && (a == ~0ull || (a >> 48) == 0xdddd);
    if (!any && poisoned)
        Log("quarantine: the fault read through 0xdddd... (a poisoned block), its free is older than the ring holds");
    if (any || poisoned) Log("quarantine: the reader (thread %lu):\n%s", GetCurrentThreadId(), ReaderStack(c).c_str());
}

void InstallQuarantine() {
    std::string m = ConfigGet("quarantine", "off");
    g_mode = m == "poison" ? 2 : m == "hold" || m == "on" ? 1 : m == "rebuild" ? 3 : m == "small" ? 4 : m == "tagged" ? 5 : 0;
    RegisterCommand("quarantine", &CmdQuarantine, "quarantine [find <hex address>]: freed memory held back (config quarantine = hold | poison)");
    g_keepCall = ConfigBool("rebuild_fix", true);
    if (ConfigBool("resize_fix", true))
        if (uintptr_t a = Address("RenderResize::FreeOld"), c = Address("RenderResize::Call"); a && c) {
            g_keepRet = a + 8;     // mov rcx, r15 (3) + call (5)
            g_keepCaller = c + 9;  // lea rcx, [rsi+0x78] (4) + call (5)
        }
    g_keepMs = static_cast<ULONGLONG>(atof(ConfigGet("rebuild_fix_seconds", "60").c_str()) * 1000);
    // Memory::Free begins with `mov rcx, [allocator]` and ends with `lock add [count], rax` (minus the size freed): the
    // allocator (its small-block heap, for the poison) and the bytes in use (for the log)
    if (auto* f = reinterpret_cast<const uint8_t*>(Address("Memory::Free"))) {
        for (int i = 0; i < 0x20; ++i)
            if (f[i] == 0x48 && f[i + 1] == 0x8b && f[i + 2] == 0x0d) {
                int32_t d;
                memcpy(&d, f + i + 3, 4);
                g_allocator = reinterpret_cast<const uintptr_t*>(f + i + 7 + d);
                break;
            }
        for (int i = 0; i < 0xa0; ++i)
            if (f[i] == 0xf0 && f[i + 1] == 0x48 && f[i + 2] == 0x01 && f[i + 3] == 0x05) {
                int32_t d;
                memcpy(&d, f + i + 4, 4);
                g_heapBytes = reinterpret_cast<const int64_t*>(f + i + 8 + d);
                break;
            }
    }
    if (g_keepRet || g_keepCall) RegisterTick(&KeepTick, nullptr);
    if (ConfigBool("heap_check", false) && g_allocator) {
        g_hcSize = static_cast<size_t>(atoi(ConfigGet("heap_check_size", "16").c_str()));
        g_hcEveryMs = static_cast<ULONGLONG>(atoi(ConfigGet("heap_check_ms", "250").c_str()));
        RegisterTick(&HeapCheckTick, nullptr);
        Log("heap check: on - the %zu-byte pages' free lists walked every %llu ms", g_hcSize,
            static_cast<unsigned long long>(g_hcEveryMs));
    }
    if (!g_mode) {
        if (!g_keepRet && !g_keepCall) return;
        void* target = reinterpret_cast<void*>(Address("Memory::Free"));
        if (!target || !Hook(target, reinterpret_cast<void*>(&FreeHook), reinterpret_cast<void**>(&oFree))) {
            g_keepRet = 0;
            g_keepCall = false;
            Log("resize fix, rebuild fix: Memory::Free not hooked - off");
            return;
        }
        Log("resize fix %s, rebuild fix %s (what a graphics rebuild frees stays until %.0f s after the last one: an "
            "upscaler switched in the game)", g_keepRet ? "on" : "off", g_keepCall ? "on" : "off", g_keepMs / 1000.0);
        return;
    }
    Log("resize fix %s, rebuild fix %s", g_keepRet ? "on" : "off", g_keepCall ? "on" : "off");
    g_holdMs = static_cast<ULONGLONG>(atof(ConfigGet("quarantine_seconds", g_mode == 3 ? "10" : g_mode == 5 ? "2" : "5").c_str()) * 1000);
    if (g_mode == 3) RegisterTick(&RebuildTick, nullptr);
    g_rbPoison = (g_mode == 3 && ConfigBool("quarantine_poison", false)) || g_mode >= 4;
    if (g_mode == 4 || g_mode == 5) {
        g_smallSize = static_cast<size_t>(atoi(ConfigGet("quarantine_small_size", "16").c_str()));
        RegisterTick(&RebuildTick, nullptr);  // (its log: blocks held, writes found)
    }
    std::string only = ConfigGet("quarantine_only", "all");
    g_onlyMods = only != "all";
    if (g_onlyMods && only != "mods") {
        g_onlyPart.clear();
        for (char ch : only) g_onlyPart += static_cast<wchar_t>(tolower(static_cast<unsigned char>(ch)));
        for (auto& ch : g_onlyPart) if (ch == L'/') ch = L'\\';
    }
    if (g_onlyMods) {
        FunctionRange(TickHookAddress(), &g_passLo[0], &g_passHi[0]);
        FunctionRange(DispatchHookAddress(), &g_passLo[1], &g_passHi[1]);
        ReadModRanges();
        RegisterTick(&ModRangesTick, nullptr);  // plugins loaded later join
    }
    if (g_holdMs < 100) g_holdMs = 100;
    void* target = reinterpret_cast<void*>(Address("Memory::Free"));
    if (!target || !Hook(target, reinterpret_cast<void*>(&FreeHook), reinterpret_cast<void**>(&oFree))) {
        g_mode = 0;
        Log("quarantine: Memory::Free not hooked - off");
        return;
    }
    if (g_mode == 5) {
        Log("quarantine: tagged - every freed block%s held %.1f s, poisoned with a tag of its record (its stack found "
            "from the poison after it went back)", g_onlyMods ? " (only frees from mods)" : "", g_holdMs / 1000.0);
        return;
    }
    if (g_mode == 4) {
        Log("quarantine: small - every freed %zu-byte block held %.1f s, poisoned, with its stack; a write into it after "
            "its free is logged when it goes back", g_smallSize, g_holdMs / 1000.0);
        return;
    }
    if (g_mode == 3) {
        Log("quarantine: rebuild - every free from a graphics rebuild until %.1f s after it is held back%s", g_holdMs / 1000.0,
            g_rbPoison ? ", poisoned, with its stack (quarantine_poison: a reader of it crashes at once and is named)" : "");
        return;
    }
    Log("quarantine: %s, freed memory held %.1f s per thread%s (a debugging aid: more memory, a little slower)",
        g_mode == 2 ? "poison" : "hold", g_holdMs / 1000.0,
        g_onlyMods ? (", only frees from modules under tw3se\\ whose path holds \"" + std::string(g_onlyPart.begin(), g_onlyPart.end()) + "\"").c_str() : "");
}

}  // namespace tw3se
