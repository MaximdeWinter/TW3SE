// The WitcherScript call stack: which script functions run right now, with file and line.
//
// Every script call goes through Dispatch(CFunction*, context, callingFrame, result). A hook on it keeps a stack per
// thread: one entry per call, taken off when the call returns. Every statement in a function body starts with opcode
// 0x0c and its line (`0c LL LL xx xx xx`, the handler skips the five bytes and runs the next opcode); a wrapper on that
// handler writes the line into the top entry. So each entry holds the line of the statement its function runs, for a
// caller the line of the call. A CFunction holds its script file at +0x60 (String: chars +0x60, length +0x68) and the
// line it is defined at +0x78; a method's class is at +0x08, a global function has none.
//
// The stack is written to the log when the game faults outside a TW3SE guard (before the game's crash handler), into
// the watchdog's hang report (the main thread's), and Lua reads it with w3.script_stack().
#include "core.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tw3se {
namespace {

bool PokeGuarded(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Entry {
    void* fn;
    void* frame;    // the frame of the function's body, set by its first statement (null for a native)
    uintptr_t sp;   // the hook's stack position: an entry below the current position belongs to a call that is gone
    uint32_t line;  // the statement running in it, 0 before the first one
    void* ctx;      // the object the function runs on (null for a global function)
    int64_t child;  // the script profiler: the time its calls took (QPC ticks), for its own time
};

// --- the WitcherScript profiler (`scriptprof`, w3.script_profile): per function the calls, the whole time
// and its own time (without the script functions it called), on the main thread, while it is on. Off it costs one
// test per call. Natives called from scripts count too (their own time is all of theirs).
struct ProfStat {
    uint64_t calls = 0;
    int64_t whole = 0, own = 0;
};
std::atomic<bool> g_profOn{false};
std::mutex g_profMx;
std::unordered_map<void*, ProfStat> g_profStats;
int64_t g_profSince = 0;   // QPC when it was switched on or reset
int64_t g_profStopAt = 0;  // QPC when it was switched off: the time counted ends there (so the numbers per second
                           // hold while it stands)

int64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
constexpr int kDepth = 160;
struct Stack {
    Entry e[kDepth];
    std::atomic<int> depth{0};
};

thread_local Stack* t_stack = nullptr;
Stack* g_main = nullptr;
std::atomic<uint64_t> g_calls{0}, g_lines{0}, g_strange{0};

using DispatchFn = bool (*)(void*, void*, void*, void*);
DispatchFn oDispatch = nullptr;
using OpFn = void (*)(void*, void*, void*);
OpFn oLine = nullptr;

Stack* Mine() {
    Stack* s = t_stack;
    if (!s) {
        s = new Stack();
        t_stack = s;
        if (GetCurrentThreadId() == MainThreadId()) g_main = s;
    }
    return s;
}

// A latent script function's new frame (w3.watch_latent, lua.cpp; docs/engine/scripting.md section 17): Dispatch
// pushed it onto the calling frame's thread and returned, the body runs later from the thread's step. Read only.
void (*g_latentStarted)(void* fn, void* ctx, void* frame) = nullptr;
void AfterDispatch(void* fn, void* ctx, void* callingFrame) {
    uint32_t fl = 0;
    if (!SafeRead(static_cast<char*>(fn) + 0x18, &fl, 4) || (fl & 0x41) != 0x40) return;  // latent, not native
    void* thread = nullptr;
    void** frames = nullptr;
    uint32_t n = 0;
    void* top = nullptr;
    void* topFn = nullptr;
    if (!callingFrame || !SafeRead(static_cast<char*>(callingFrame) + 0x40, &thread, 8) || !thread) return;
    if (!SafeRead(static_cast<char*>(thread) + 0x10, &frames, 8) || !SafeRead(static_cast<char*>(thread) + 0x18, &n, 4))
        return;
    if (!frames || n == 0 || n > 4096 || !SafeRead(frames + n - 1, &top, 8) || !top) return;
    if (!SafeRead(static_cast<char*>(top) + 0x18, &topFn, 8) || topFn != fn) return;
    g_latentStarted(fn, ctx, top);
}

bool DispatchHook(void* fn, void* ctx, void* frame, void* result) {
    Stack* s = Mine();
    if (!g_main && GetCurrentThreadId() == MainThreadId()) g_main = s;  // scripts run before the first tick named it
    uintptr_t sp = reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());
    int d = s->depth.load(std::memory_order_relaxed);
    // a fault a guard caught deep inside skipped the pops: those entries lie at or below this call on the stack
    while (d > 0 && d <= kDepth && s->e[d - 1].sp <= sp) --d;
    if (d < kDepth) s->e[d] = {fn, nullptr, sp, 0, ctx, 0};
    s->depth.store(d + 1, std::memory_order_release);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_profOn.load(std::memory_order_relaxed) || s != g_main || d >= kDepth) {
        bool r = oDispatch(fn, ctx, frame, result);
        if (r && g_latentStarted && s == g_main) AfterDispatch(fn, ctx, frame);
        s->depth.store(d, std::memory_order_release);
        return r;
    }
    int64_t t0 = Qpc();
    bool r = oDispatch(fn, ctx, frame, result);
    if (r && g_latentStarted) AfterDispatch(fn, ctx, frame);
    int64_t dt = Qpc() - t0;
    int64_t own = dt - s->e[d].child;
    if (d > 0) s->e[d - 1].child += dt;
    {
        std::lock_guard<std::mutex> l(g_profMx);
        ProfStat& st = g_profStats[fn];
        ++st.calls;
        st.whole += dt;
        st.own += own > 0 ? own : 0;
    }
    s->depth.store(d, std::memory_order_release);
    return r;
}

// Watched lines (F4): one bit per line number, so an unwatched statement costs one test.
uint64_t g_watchBits[65536 / 64];
std::atomic<int> g_watchCount{0};
ScriptLineFn g_lineFn = nullptr;
thread_local bool t_inLineFn = false;
// Stepping (the debugger, F4): the next statement on the main thread at all (1), at the same call depth or above (2:
// step over), above it (3: step out) calls g_stepFn once.
std::atomic<int> g_stepMode{0};
int g_stepDepth = 0;
ScriptLineFn g_stepFn = nullptr;
void* g_lineFns[32] = {};
std::atomic<int> g_lineFnCount{0};

void LineHook(void* ctx, void* frame, void* result) {
    const uint8_t* cursor = *reinterpret_cast<uint8_t* const*>(static_cast<char*>(frame) + 0x30);
    uint16_t line = *reinterpret_cast<const uint16_t*>(cursor);
    if (Stack* s = t_stack) {
        int d = s->depth.load(std::memory_order_relaxed);
        if (d > 0 && d <= kDepth) {
            Entry& e = s->e[d - 1];
            if (*reinterpret_cast<void* const*>(static_cast<char*>(frame) + 0x18) == e.fn) {
                e.frame = frame;
                e.line = line;
            } else {
                g_strange.fetch_add(1, std::memory_order_relaxed);  // a body run past Dispatch (a resumed thread ...)
            }
        }
    }
    g_lines.fetch_add(1, std::memory_order_relaxed);
    {  // the first functions seen with statement lines (`scriptstack` lists them)
        void* f = *reinterpret_cast<void* const*>(static_cast<char*>(frame) + 0x18);
        int n = g_lineFnCount.load(std::memory_order_relaxed);
        bool known = false;
        for (int k = 0; k < n && !known; ++k) known = g_lineFns[k] == f;
        if (!known && n < 32) {
            g_lineFns[n] = f;
            g_lineFnCount.store(n + 1, std::memory_order_relaxed);
        }
    }
    if (int mode = g_stepMode.load(std::memory_order_relaxed); mode && t_stack && t_stack == g_main && !t_inLineFn) {
        int d = t_stack->depth.load(std::memory_order_relaxed);
        if (mode == 1 || (mode == 2 && d <= g_stepDepth) || (mode == 3 && d < g_stepDepth)) {
            g_stepMode.store(0, std::memory_order_relaxed);  // one stop; the debugger asks again for the next
            if (ScriptLineFn fn = g_stepFn) {
                t_inLineFn = true;
                fn(*reinterpret_cast<void* const*>(static_cast<char*>(frame) + 0x18), ctx, frame, line);
                t_inLineFn = false;
            }
        }
    }
    if ((g_watchBits[line >> 6] >> (line & 63)) & 1) {
        ScriptLineFn fn = g_lineFn;
        if (fn && !t_inLineFn) {
            t_inLineFn = true;  // a watched line inside the watcher's own script calls does not call it again
            fn(*reinterpret_cast<void* const*>(static_cast<char*>(frame) + 0x18), ctx, frame, line);
            t_inLineFn = false;
        }
    }
    oLine(ctx, frame, result);
}

std::string FnText(void* fn) {
    std::string out;
    void* cls = nullptr;
    uint32_t nameId = 0;
    SafeRead(static_cast<char*>(fn) + 0x08, &cls, 8);
    SafeRead(static_cast<char*>(fn) + 0x10, &nameId, 4);
    if (cls) {
        std::string c = RttiName(cls);
        if (!c.empty()) out = c + ".";
    }
    const char* n = GetCNameString(nameId);
    out += n ? n : "?";
    return out;
}

std::string FileOf(void* fn) {
    const char* chars = nullptr;
    uint32_t len = 0;
    if (!SafeRead(static_cast<char*>(fn) + 0x60, &chars, 8) || !SafeRead(static_cast<char*>(fn) + 0x68, &len, 4) ||
        !chars || len < 2 || len > 400)
        return "";
    std::string s(len - 1, '\0');
    if (!SafeRead(chars, s.data(), s.size())) return "";
    return s;
}

// Reads a stack (also another thread's: every entry through SafeRead, the depth once).
std::string Text(Stack* s, const char* indent) {
    if (!s) return std::string(indent) + "(no script call on this thread)\n";
    int d = s->depth.load(std::memory_order_acquire);
    if (d <= 0) return std::string(indent) + "(no script function running)\n";
    std::string out;
    if (d > kDepth) {
        char b[96];
        snprintf(b, sizeof b, "%s(%d calls deep, the innermost %d not kept)\n", indent, d, d - kDepth);
        out += b;
        d = kDepth;
    }
    for (int i = d - 1; i >= 0; --i) {
        Entry e{};
        if (!SafeRead(&s->e[i], &e, sizeof e) || !e.fn) continue;
        std::string file = FileOf(e.fn);
        char b[64] = "";
        if (!file.empty() && e.line) snprintf(b, sizeof b, ":%u", e.line);
        out += std::string(indent) + FnText(e.fn) + (file.empty() ? "  (native)" : "  (" + file + b + ")") + "\n";
    }
    return out;
}

const char* FaultName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "division by zero";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds";
        default: return nullptr;
    }
}

std::atomic<int> g_faultsLogged{0};

// The line opcode's handler is in the table only once the engine has registered its natives (empty at
// start-up). The table itself stays as the game set it: other mods check that its entries point into witcher3.exe
// (Witcher Native Bridge reads no parameter otherwise, FIXES.md F1). So the handler's own code gets the hook, once.
void** g_table = nullptr;
bool g_lineHooked = false;
void WrapLineOpcode(float, void*) {
    if (g_lineHooked) return;
    void* cur = nullptr;
    if (!g_table || !SafeRead(&g_table[0x0c], &cur, 8) || !cur) return;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        ImageBase() + reinterpret_cast<const IMAGE_DOS_HEADER*>(ImageBase())->e_lfanew);
    uintptr_t at = reinterpret_cast<uintptr_t>(cur);
    g_lineHooked = true;  // one try: a handler outside the game (another mod's) or a failed hook stays as it is
    if (at < ImageBase() || at >= ImageBase() + nt->OptionalHeader.SizeOfImage) {
        Log("scriptstack: opcode 0x0c handler lies outside witcher3.exe (another mod's) - no statement lines");
        return;
    }
    if (!Hook(cur, reinterpret_cast<void*>(&LineHook), reinterpret_cast<void**>(&oLine))) {
        Log("scriptstack: opcode 0x0c handler could not be hooked - no statement lines");
        return;
    }
    Log("scriptstack: statement lines on (opcode 0x0c handler 0x%llx hooked, the table unchanged)",
        static_cast<unsigned long long>(at - ImageBase()));
}

void CmdScriptStack(const char*, char* reply, uint32_t size) {
    std::string t = "scriptstack: " + std::to_string(g_calls.load()) + " calls, " + std::to_string(g_lines.load()) +
                    " statements, " + std::to_string(g_strange.load()) + " outside a call; main thread:\n" +
                    Text(g_main, "  ");
    int n = g_lineFnCount.load();
    if (n) {
        t += "functions seen with statement lines:";
        for (int k = 0; k < n; ++k) t += " " + FnText(g_lineFns[k]);
    }
    strncpy_s(reply, size, t.c_str(), _TRUNCATE);
}

}  // namespace

void* DispatchHookAddress() { return reinterpret_cast<void*>(&DispatchHook); }  // the quarantine's filter

std::string ScriptStackText(bool mainThread) { return Text(mainThread ? g_main : t_stack, "  "); }

std::string ScriptFunctionFile(void* fn) { return FileOf(fn); }
std::string ScriptFunctionName(void* fn) { return FnText(fn); }

void ScriptLineCallback(ScriptLineFn fn) { g_lineFn = fn; }

void ScriptStepCallback(ScriptLineFn fn) { g_stepFn = fn; }
void ScriptStep(int mode) {
    g_stepDepth = t_stack ? t_stack->depth.load(std::memory_order_relaxed) : 0;
    g_stepMode.store(mode, std::memory_order_relaxed);
}

void ScriptLineWatch(uint32_t line, bool on) {
    if (line > 0xffff) return;
    uint64_t bit = 1ull << (line & 63);
    if (on) g_watchBits[line >> 6] |= bit;
    else g_watchBits[line >> 6] &= ~bit;
}

int ScriptStackRead(ScriptStackFrameInfo* out, int max) {
    Stack* s = t_stack;
    if (!s) return 0;
    int d = s->depth.load(std::memory_order_acquire);
    if (d > kDepth) d = kDepth;
    int n = 0;
    for (int i = d - 1; i >= 0 && n < max; --i) {
        const Entry& e = s->e[i];
        if (!e.fn) continue;
        out[n].fn = e.fn;
        out[n].name = FnText(e.fn);
        out[n].file = FileOf(e.fn);
        out[n].line = e.line;
        out[n].frame = e.frame;
        out[n].ctx = e.ctx;
        ++n;
    }
    return n;
}

void ScriptStackOnFault(EXCEPTION_POINTERS* ep) {
    if (!oDispatch || !ep || !ep->ExceptionRecord) return;
    const char* what = FaultName(ep->ExceptionRecord->ExceptionCode);
    if (!what || !t_stack || t_stack->depth.load() <= 0) return;
    if (g_faultsLogged.fetch_add(1) >= 3) return;  // the first faults tell it; a crash loop must not fill the log
    uintptr_t at = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    HMODULE m = nullptr;
    char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(at), &m))
        GetModuleFileNameA(m, mod, MAX_PATH);
    const char* base = strrchr(mod, '\\');
    Log("fault: %s at %s+0x%llx on thread %lu while scripts ran; script stack, innermost first:\n%s", what,
        base ? base + 1 : mod, static_cast<unsigned long long>(at - reinterpret_cast<uintptr_t>(m)),
        GetCurrentThreadId(), Text(t_stack, "  ").c_str());
}

namespace {
// The mod a script file belongs to: a function of a mod's script names only its file, so the files under
// <game>\Mods\*\content\scripts give the mod (read again each minute); a path like game\player\... is the game's.
std::unordered_map<std::string, std::string> g_fileMods;
ULONGLONG g_fileModsAt = 0;

void ScanModScripts(const std::wstring& dir, const std::string& mod, int depth) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 8) ScanModScripts(dir + L"\\" + n, mod, depth + 1);
            continue;
        }
        char b[MAX_PATH * 2];
        WideCharToMultiByte(CP_UTF8, 0, n.c_str(), -1, b, sizeof b, nullptr, nullptr);
        std::string low = b;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
        g_fileMods.emplace(low, mod);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::string ModOfFile(const std::string& file) {
    if (file.empty()) return "";
    if (file.find('\\') != std::string::npos || file.find('/') != std::string::npos) return "(game)";
    ULONGLONG now = GetTickCount64();
    if (now - g_fileModsAt > 60000 || g_fileMods.empty()) {
        g_fileModsAt = now;
        g_fileMods.clear();
        std::wstring root = Dir();
        for (int i = 0; i < 3; ++i) root = root.substr(0, root.find_last_of(L"\\/"));
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((root + L"\\Mods\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                char b[MAX_PATH * 2];
                WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, b, sizeof b, nullptr, nullptr);
                ScanModScripts(root + L"\\Mods\\" + fd.cFileName + L"\\content\\scripts", b, 0);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    std::string low = file;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    auto it = g_fileMods.find(low);
    return it != g_fileMods.end() ? it->second : "";
}

void CmdScriptProf(const char* args, char* reply, uint32_t size) {
    std::string a = args ? args : "";
    if (a == "on" || a == "off" || a == "reset") {
        ScriptProfileSet(a != "off", a != "off");
        snprintf(reply, size, "scriptprof %s", a == "off" ? "off (the numbers stay until reset)" : "on, counting from now");
        return;
    }
    int top = a.empty() ? 20 : atoi(a.c_str());
    if (top <= 0) top = 20;
    double seconds = 0;
    std::vector<ScriptProfRow> rows = ScriptProfile(&seconds);
    std::string out;
    char b[512];
    snprintf(b, sizeof b, "WitcherScript time over %.1f s (%s), own ms per second / whole ms per second / calls per second:\n",
             seconds, ScriptProfileOn() ? "on" : "off");
    out += b;
    for (int i = 0; i < static_cast<int>(rows.size()) && i < top; ++i) {
        const ScriptProfRow& r = rows[i];
        snprintf(b, sizeof b, "  %8.3f %8.3f %9.0f  %s  %s%s\n", r.ownMs / seconds, r.wholeMs / seconds, r.calls / seconds,
                 r.fn.c_str(), r.file.empty() ? "(native)" : r.file.c_str(), r.mod.empty() ? "" : ("  [" + r.mod + "]").c_str());
        out += b;
    }
    if (rows.empty()) out += "  (nothing yet: scriptprof on, play a while, then scriptprof)\n";
    strncpy_s(reply, size, out.c_str(), _TRUNCATE);
}
}  // namespace

void ScriptProfileSet(bool on, bool reset) {
    int64_t now = Qpc();
    if (reset) {
        std::lock_guard<std::mutex> l(g_profMx);
        g_profStats.clear();
        g_profSince = now;
        g_profStopAt = on ? 0 : now;
    }
    if (on && !g_profSince) g_profSince = now;
    if (on && g_profStopAt) {  // on again: the time it stood is left out
        g_profSince += now - g_profStopAt;
        g_profStopAt = 0;
    }
    if (!on && g_profOn.load() && !g_profStopAt) g_profStopAt = now;
    g_profOn.store(on);
    Log("scriptprof: %s%s", on ? "on" : "off", reset ? " (reset)" : "");
}

bool ScriptProfileOn() { return g_profOn.load(); }

// The functions by their own time, the most first; seconds: how long it counted.
std::vector<ScriptProfRow> ScriptProfile(double* seconds) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    std::vector<std::pair<void*, ProfStat>> copy;
    {
        std::lock_guard<std::mutex> l(g_profMx);
        copy.assign(g_profStats.begin(), g_profStats.end());
    }
    double ms = 1000.0 / static_cast<double>(f.QuadPart);
    int64_t end = g_profStopAt ? g_profStopAt : Qpc();
    if (seconds) *seconds = g_profSince ? (std::max)(0.001, (end - g_profSince) * ms / 1000.0) : 0.001;
    std::sort(copy.begin(), copy.end(), [](const auto& x, const auto& y) { return x.second.own > y.second.own; });
    std::vector<ScriptProfRow> rows;
    for (size_t i = 0; i < copy.size() && i < 200; ++i) {
        ScriptProfRow r;
        r.fn = FnText(copy[i].first);
        r.file = FileOf(copy[i].first);
        r.mod = ModOfFile(r.file);
        r.calls = copy[i].second.calls;
        r.wholeMs = copy[i].second.whole * ms;
        r.ownMs = copy[i].second.own * ms;
        rows.push_back(std::move(r));
    }
    return rows;
}

void SetLatentStartCallback(void (*cb)(void* fn, void* ctx, void* frame)) { g_latentStarted = cb; }

void InstallScriptStack() {
    if (!ConfigBool("scriptstack", true)) return;
    void* dispatch = reinterpret_cast<void*>(Address("Dispatch"));
    auto table = reinterpret_cast<void**>(Address("GScriptNativeTable"));
    if (!dispatch || !table) {
        Log("scriptstack: Dispatch or the opcode table missing - no script stack");
        return;
    }
    if (!Hook(dispatch, reinterpret_cast<void*>(&DispatchHook), reinterpret_cast<void**>(&oDispatch))) {
        Log("scriptstack: Dispatch could not be hooked - no script stack");
        return;
    }
    g_table = table;
    RegisterTick(&WrapLineOpcode, nullptr);  // the opcode table fills when the engine registers its natives
    RegisterCommand("scriptstack", &CmdScriptStack, "scriptstack: the script calls the main thread runs now, and counts");
    RegisterCommand("scriptprof", &CmdScriptProf,
                    "scriptprof on|off|reset|[n]: the time of each WitcherScript function (own, whole, calls), the n most");
    Log("scriptstack: on (Dispatch hooked; statement lines through opcode 0x0c from the first tick)");
}

}  // namespace tw3se
