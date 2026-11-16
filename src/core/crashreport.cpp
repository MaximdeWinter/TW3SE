// TW3SE core - the crash report: a game session that did not end normally leaves tw3se\out\crash_report.txt at the
// next start.
//
// A player whose game crashes cannot open the TW3SE Menu for "Copy report", and the next start empties tw3se.log.
// So each game session leaves a marker, tw3se\out\session.txt, from the start on. The normal end (RtlExitUserProcess
// with code 0, diag.cpp) deletes it. Another exit code, a TerminateProcess of the game itself and the first faults
// that reach the game's crash handler are written into it. A marker found at the start belongs to a session that
// ended otherwise (a crash, or the process was killed): its lines and the last session's tw3se.log go into
// crash_report.txt, before LogOpen empties the log. The fault path writes with WriteFile into stack buffers only.
#include "core.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

namespace tw3se {
namespace {

wchar_t g_marker[MAX_PATH] = L"";  // set when the session's marker is written; empty in the editor and when passive
std::string g_lastMarker;          // the last session's marker, if it was still there
std::string g_lastLog;             // the last session's tw3se.log, read before LogOpen empties it
volatile LONG g_faults = 0;

std::wstring OutDir() { return Dir() + L"\\out"; }

void Append(const char* text, size_t n) {
    if (!g_marker[0]) return;
    HANDLE h = CreateFileW(g_marker, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, text, static_cast<DWORD>(n), &w, nullptr);
    CloseHandle(h);
}

const char* FaultText(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "division by zero";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds";
        case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
        case 0xc0000409: return "stack buffer overrun";
        // (a driver's or a library's own stop: 11.11. the game ended with 0x80000003 after an upscaler and frame
        // generation switch, and the report had no place for it)
        case EXCEPTION_BREAKPOINT: return "breakpoint (int 3)";
        default: return nullptr;
    }
}

std::string Now() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char s[32];
    snprintf(s, sizeof s, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return s;
}

// The first `head` and the last `tail` lines of a text, or all of it when it is not longer.
std::string HeadTail(const std::string& s, size_t head, size_t tail) {
    size_t lines = 0;
    for (char c : s)
        if (c == '\n') ++lines;
    if (lines <= head + tail) return s;
    size_t a = 0;
    for (size_t n = 0; n < head && a < s.size(); ++a)
        if (s[a] == '\n') ++n;
    size_t b = s.size();
    for (size_t n = 0; b > a && n <= tail;)
        if (s[--b] == '\n') ++n;
    char gap[64];
    snprintf(gap, sizeof gap, "\n... (%zu lines left out) ...\n", lines - head - tail);
    return s.substr(0, a) + gap + s.substr(b + 1);
}

// --- the names of functions, for a fault: the function a fault lies in comes from the exe's exception table
// (RtlLookupFunctionEntry: its start, the same for every player with the same build - a fingerprint of the bug).
// Its name comes from a table built once, 60 ticks after the start (the natives are registered then): the engine
// functions TW3SE knows by name (addresses.inc) and the handler of every native of the script system (RTTI: a global
// native's handler in GScriptNativeTable, a method's in GScriptMethodTable, by its opcode). Sorted, never changed
// after it is built: the fault path only reads it.
std::vector<uintptr_t> g_nameAt;
std::vector<std::string> g_nameOf;
std::atomic<bool> g_namesReady{false};

void BuildNames() {
    std::vector<std::pair<uintptr_t, std::string>> all;
    ForEachAddress([](const char* n, uintptr_t a, void* c) {
        if (a >= ImageBase()) static_cast<std::vector<std::pair<uintptr_t, std::string>>*>(c)->push_back({a, n});
    }, &all);
    auto natives = reinterpret_cast<void**>(Address("GScriptNativeTable"));
    auto methods = reinterpret_cast<char*>(Address("GScriptMethodTable"));
    struct Ctx {
        std::vector<std::pair<uintptr_t, std::string>>* all;
        void** natives;
        char* methods;
    } ctx{&all, natives, methods};
    if (natives || methods) {
        RttiFunctions([](void* fn, void* c) {
            auto& x = *static_cast<Ctx*>(c);
            uint32_t flags = 0, opcode = 0, nameId = 0;
            void* cls = nullptr;
            if (!SafeRead(static_cast<char*>(fn) + 0x18, &flags, 4) || !(flags & 1)) return;  // natives only
            SafeRead(static_cast<char*>(fn) + 0xe0, &opcode, 4);
            SafeRead(static_cast<char*>(fn) + 0x10, &nameId, 4);
            SafeRead(static_cast<char*>(fn) + 0x08, &cls, 8);
            if (!opcode || opcode > 0x10000) return;
            void* handler = nullptr;
            if (flags & 2) {
                if (x.natives) SafeRead(&x.natives[opcode], &handler, 8);
            } else if (x.methods) {
                SafeRead(x.methods + static_cast<size_t>(opcode) * 24, &handler, 8);
            }
            const char* n = GetCNameString(nameId);
            if (!handler || !n) return;
            std::string name = std::string("native ") + (cls ? RttiName(cls) + "." : "") + n;
            x.all->push_back({reinterpret_cast<uintptr_t>(handler), name});
        }, &ctx);
    }
    std::sort(all.begin(), all.end());
    g_nameAt.reserve(all.size());
    g_nameOf.reserve(all.size());
    for (auto& [a, n] : all) {
        if (!g_nameAt.empty() && g_nameAt.back() == a) continue;  // the first name of an address
        g_nameAt.push_back(a);
        g_nameOf.push_back(std::move(n));
    }
    g_namesReady.store(true, std::memory_order_release);
    Log("crash report: %zu function names for faults", g_nameAt.size());
    // the same names for a debugger outside the game (crashcatch: tw3se\out\names.txt, "<rva hex> <name>"), written
    // when the build or the count changed
    char head[96];
    snprintf(head, sizeof head, "# witcher3.exe build %08x, %zu names\n", GameBuild(), g_nameAt.size());
    std::wstring file = OutDir() + L"\\names.txt";
    if (ReadTextFile(file).rfind(head, 0) != 0) {
        std::string text = head;
        for (size_t i = 0; i < g_nameAt.size(); ++i) {
            char rva[24];
            snprintf(rva, sizeof rva, "%llx ", static_cast<unsigned long long>(g_nameAt[i] - ImageBase()));
            text += rva + g_nameOf[i] + "\n";
        }
        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD w = 0;
        if (h != INVALID_HANDLE_VALUE) {
            WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &w, nullptr);
            CloseHandle(h);
        }
    }
}

void NamesTick(float, void*) {
    static int ticks = 0;
    if (++ticks == 60) BuildNames();
}

const char* NameAt(uintptr_t a) {  // the fault path: a binary search over the finished table, or null
    if (!g_namesReady.load(std::memory_order_acquire) || g_nameAt.empty()) return nullptr;
    size_t lo = 0, hi = g_nameAt.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_nameAt[mid] < a) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_nameAt.size() && g_nameAt[lo] == a ? g_nameOf[lo].c_str() : nullptr;
}

// `crashtest`: a fault outside every guard on a thread of its own - the game crashes, the next start writes the report.
void CmdCrashTest(const char* args, char* reply, uint32_t size) {
    if (args && (!strcmp(args, "failfast_api") || !strcmp(args, "ntterminate"))) {  // the ends diag.cpp writes down
        static bool api = false;
        api = !strcmp(args, "failfast_api");
        Log("crashtest: the game ends by %s on purpose", api ? "RaiseFailFastException" : "NtTerminateProcess");
        HANDLE t = CreateThread(nullptr, 0, [](void*) -> DWORD {
            Sleep(200);
            if (api) {
                RaiseFailFastException(nullptr, nullptr, 0);
            } else if (auto nt = reinterpret_cast<LONG(NTAPI*)(HANDLE, LONG)>(
                           GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtTerminateProcess"))) {
                nt(GetCurrentProcess(), static_cast<LONG>(0xC0DE));
            }
            return 0;
        }, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        snprintf(reply, size, "the game ends now; the next start's crash report says how");
        return;
    }
    if (args && !strcmp(args, "fastfail")) {  // past every handler, as a fail-fast of the runtime: only Windows sees it
        Log("crashtest: the game ends by a fail-fast on purpose");
        HANDLE t = CreateThread(nullptr, 0, [](void*) -> DWORD {
            Sleep(200);
            __fastfail(7);  // FAST_FAIL_FATAL_APP_EXIT
            return 0;
        }, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        snprintf(reply, size, "the game ends now; the next start's crash report has what Windows saw");
        return;
    }
    Log("crashtest: the game crashes on purpose");
    HANDLE t = CreateThread(nullptr, 0, [](void*) -> DWORD {
        Sleep(200);  // the console's reply goes out first
        *reinterpret_cast<volatile int*>(static_cast<uintptr_t>(0x10)) = 1;
        return 0;
    }, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    snprintf(reply, size, "the game crashes now; tw3se\\out\\crash_report.txt is written at the next start");
}

// --- what Windows saw: a crash that passed every handler (a fail-fast, a stack overflow, a fault on
// a thread before TW3SE's guard) leaves no "fault:" in the marker. Windows Error Reporting still writes event 1000
// ("Application Error") into the Application log, which a normal user may read: the faulting module, the exception
// code and the offset. The event of the last session's process id (the last two days) goes into the report. The
// values come by position (Windows 10 writes <Data> without names): 0 app, 3 module, 6 code, 7 offset, 8 process id,
// 11 module path.
using EvtQuery_t = HANDLE(WINAPI*)(HANDLE, LPCWSTR, LPCWSTR, DWORD);
using EvtNext_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE*, DWORD, DWORD, DWORD*);
using EvtRender_t = BOOL(WINAPI*)(HANDLE, HANDLE, DWORD, DWORD, PVOID, DWORD*, DWORD*);
using EvtClose_t = BOOL(WINAPI*)(HANDLE);

std::vector<std::string> EventData(const std::wstring& xml) {
    std::vector<std::string> out;
    size_t at = 0;
    while ((at = xml.find(L"<Data", at)) != std::wstring::npos) {
        size_t open = xml.find(L'>', at);
        if (open == std::wstring::npos) break;
        if (xml[open - 1] == L'/') {  // <Data/>: empty
            out.emplace_back();
            at = open;
            continue;
        }
        size_t close = xml.find(L"</Data>", open);
        if (close == std::wstring::npos) break;
        std::wstring v = xml.substr(open + 1, close - open - 1);
        int n = WideCharToMultiByte(CP_UTF8, 0, v.c_str(), static_cast<int>(v.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, v.c_str(), static_cast<int>(v.size()), s.data(), n, nullptr, nullptr);
        out.push_back(s);
        at = close;
    }
    return out;
}

std::string WindowsSaw(unsigned long pid) {
    HMODULE m = LoadLibraryW(L"wevtapi.dll");
    if (!m) return {};
    auto query = reinterpret_cast<EvtQuery_t>(GetProcAddress(m, "EvtQuery"));
    auto next = reinterpret_cast<EvtNext_t>(GetProcAddress(m, "EvtNext"));
    auto render = reinterpret_cast<EvtRender_t>(GetProcAddress(m, "EvtRender"));
    auto close = reinterpret_cast<EvtClose_t>(GetProcAddress(m, "EvtClose"));
    std::string found;
    if (query && next && render && close) {
        const wchar_t* xpath = L"*[System[Provider[@Name='Application Error'] and EventID=1000 and "
                               L"TimeCreated[timediff(@SystemTime) <= 172800000]]]";
        // EvtQueryChannelPath 0x1 | EvtQueryReverseDirection 0x200: the newest first
        HANDLE q = query(nullptr, L"Application", xpath, 0x1 | 0x200);
        HANDLE ev[16];
        DWORD got = 0;
        while (q && found.empty() && next(q, 16, ev, 1000, 0, &got) && got) {
            for (DWORD i = 0; i < got; ++i) {
                DWORD used = 0, props = 0;
                render(nullptr, ev[i], 1 /* EvtRenderEventXml */, 0, nullptr, &used, &props);
                std::wstring xml(used / sizeof(wchar_t) + 1, L'\0');
                if (found.empty() && used &&
                    render(nullptr, ev[i], 1, static_cast<DWORD>(xml.size() * sizeof(wchar_t)), xml.data(), &used,
                           &props)) {
                    auto d = EventData(xml);
                    if (d.size() > 8 && _stricmp(d[0].c_str(), "witcher3.exe") == 0 &&
                        strtoul(d[8].c_str(), nullptr, 0) == pid) {
                        unsigned long code = strtoul(d[6].c_str(), nullptr, 16);
                        const char* what = FaultText(code);
                        bool other = _stricmp(d[3].c_str(), d[0].c_str()) != 0 && d.size() > 11 && !d[11].empty();
                        char line[512];
                        snprintf(line, sizeof line, "%s ended: exception 0x%08lx%s%s%s at %s+0x%llx%s%s%s\r\n",
                                 d[0].c_str(), code, what ? " (" : "", what ? what : "", what ? ")" : "",
                                 d[3].c_str(), strtoull(d[7].c_str(), nullptr, 16), other ? " (" : "",
                                 other ? d[11].c_str() : "", other ? ")" : "");
                        found = line;
                    }
                }
                close(ev[i]);
            }
        }
        if (q) close(q);
    }
    FreeLibrary(m);
    return found;
}

// The report before goes to crash_report_<its time>.txt; the newest ten of those stay (so the reports of several crashes in one
// evening are all kept).
void KeepOldReports(const std::wstring& dir) {
    std::wstring cur = dir + L"\\crash_report.txt";
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW(cur.c_str(), GetFileExInfoStandard, &a)) {
        FILETIME local;
        SYSTEMTIME st;
        FileTimeToLocalFileTime(&a.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &st);
        wchar_t name[64];
        swprintf(name, 64, L"\\crash_report_%04u%02u%02u_%02u%02u%02u.txt", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
        MoveFileExW(cur.c_str(), (dir + name).c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    std::vector<std::wstring> old;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\crash_report_*.txt").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do old.push_back(fd.cFileName); while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(old.begin(), old.end());  // the names carry the time, so this is oldest first
    for (size_t i = 0; i + 10 < old.size(); ++i) DeleteFileW((dir + L"\\" + old[i]).c_str());
}

}  // namespace

void TakeLastSession() {
    std::wstring marker = OutDir() + L"\\session.txt";
    g_lastMarker = ReadTextFile(marker);
    if (GetFileAttributesW(marker.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileW(marker.c_str());
    g_lastLog = ReadTextFile(Dir() + L"\\tw3se.log");
}

void WriteCrashReport() {
    if (g_lastMarker.empty() && g_lastLog.empty()) return;
    std::wstring game = Dir() + L"\\..\\..\\..";
    std::string r = "TW3SE crash report\r\n\r\nThe last game session did not end normally (a crash, or the game was "
                    "ended from outside). Written at the next start, " + Now() + ".\r\n";
    r += "\r\n[the last session]\r\n" + g_lastMarker;
    if (g_lastMarker.find("ended:") == std::string::npos && g_lastMarker.find("fault:") == std::string::npos)
        r += "(no exit and no fault seen: the process was ended without its exit code, by a crash or from outside)\r\n";
    std::string windows;
    size_t p = g_lastMarker.find(", pid ");
    if (p != std::string::npos) windows = WindowsSaw(strtoul(g_lastMarker.c_str() + p + 6, nullptr, 10));
    r += "\r\n[what Windows saw (Application log, event 1000)]\r\n" +
         (windows.empty() ? std::string("nothing for this process (the game's crash handling keeps most crashes from "
                                        "Windows; or the game was ended from outside)\r\n")
                          : windows);
    if (g_lastMarker.find("safe mode") == std::string::npos)
        r += "\r\nTo see whether a Lua mod or a plugin of TW3SE causes it: hold Shift while the game starts (safe mode:"
             " no Lua mods, no plugins). If the game crashes the same way then, the cause lies elsewhere.\r\n";
    r += "\r\n[tw3se.config]\r\n" + ReadTextFile(Dir() + L"\\tw3se.config") + "\r\n";
    r += "\r\n[Mods] " + FolderNames(game + L"\\Mods") + "\r\n[dlc] " + FolderNames(game + L"\\dlc") + "\r\n";
    r += ModioText();
    r += "\r\n[tw3se.log of the last session]\r\n" + HeadTail(g_lastLog, 120, 400) + "\r\n";
    std::wstring out = OutDir();
    CreateDirectoryW(out.c_str(), nullptr);
    KeepOldReports(out);
    out += L"\\crash_report.txt";
    HANDLE h = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, r.data(), static_cast<DWORD>(r.size()), &w, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    Log("crash report: the last session did not end normally - %s", ok ? "tw3se\\out\\crash_report.txt written"
                                                                        : "tw3se\\out\\crash_report.txt NOT written");
    if (!windows.empty()) {
        windows.resize(windows.size() - 2);  // without the line end
        Log("crash report: Windows saw: %s", windows.c_str());
    }
    g_lastMarker.clear();
    g_lastLog.clear();
}

void StartSession() {
    RegisterCommand("crashtest", &CmdCrashTest, "crashtest [fastfail]: crash the game on purpose (to see the crash report; fastfail: past every handler)");
    RegisterTick(&NamesTick, nullptr);  // the function names for faults, 60 ticks in
    std::wstring out = OutDir();
    CreateDirectoryW(out.c_str(), nullptr);
    std::wstring marker = out + L"\\session.txt";
    char text[160];
    int n = snprintf(text, sizeof text, "started %s, pid %lu, TW3SE %d.%d.%d, witcher3.exe build %08x%s\r\n",
                     Now().c_str(), GetCurrentProcessId(), TW3SE_VERSION_MAJOR, TW3SE_VERSION_MINOR, TW3SE_VERSION_PATCH,
                     GameBuild(), SafeMode() ? ", safe mode (no Lua mods, no plugins)" : "");
    HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log("crash report: tw3se\\out\\session.txt not written (%lu) - no crash report after this session",
            GetLastError());
        return;
    }
    DWORD w = 0;
    WriteFile(h, text, static_cast<DWORD>(n), &w, nullptr);
    CloseHandle(h);
    wcsncpy_s(g_marker, marker.c_str(), _TRUNCATE);
}

volatile bool g_keepMarker = false;  // the session failed though it may end with code 0 (scripts that did not compile)

void SessionNote(const char* what) {
    g_keepMarker = true;
    char text[256];
    int n = snprintf(text, sizeof text, "%s\r\n", what);
    if (n > 0) Append(text, static_cast<size_t>(n < static_cast<int>(sizeof text) ? n : sizeof text - 1));
}

// A note that keeps the marker only if the game ends within `seconds` (after a mod's compiled scripts fail to
// load, the game ends itself with code 0 about 30 s later; when it runs on instead, a normal end stays normal).
volatile ULONGLONG g_keepUntil = 0;
void SessionNoteFor(const char* what, unsigned seconds) {
    char text[512];
    int n = snprintf(text, sizeof text, "%s\r\n", what);
    if (n > 0) Append(text, static_cast<size_t>(n < static_cast<int>(sizeof text) ? n : sizeof text - 1));
    g_keepUntil = GetTickCount64() + seconds * 1000ull;
}

void SessionExit(long code, bool terminate) {
    if (!g_marker[0]) return;
    if (code == 0 && !terminate && !g_keepMarker && GetTickCount64() >= g_keepUntil) {  // the normal end
        DeleteFileW(g_marker);
        g_marker[0] = 0;
        return;
    }
    char text[96];
    int n = snprintf(text, sizeof text, "ended: %s, code %ld (0x%08lx)\r\n",
                     terminate ? "TerminateProcess of the game itself" : "process exit", code,
                     static_cast<unsigned long>(code));
    Append(text, static_cast<size_t>(n));
}

void SessionFault(EXCEPTION_POINTERS* ep) {
    if (!g_marker[0] || !ep || !ep->ExceptionRecord) return;
    const char* what = FaultText(ep->ExceptionRecord->ExceptionCode);
    if (!what || InterlockedIncrement(&g_faults) > 3) return;
    uintptr_t at = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    HMODULE m = nullptr;
    char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(at), &m) && m)
        GetModuleFileNameA(m, mod, MAX_PATH);
    const char* base = strrchr(mod, '\\');
    char text[MAX_PATH + 512];
    bool main = GetCurrentThreadId() == MainThreadId();
    const char* lua = main ? CurrentLuaMod() : "";  // the Lua mod that ran then (only the main thread runs Lua)
    // the function it lies in: its start from the exception table (the same for every player of this build), its
    // name when TW3SE knows one
    char where[320] = "";
    DWORD64 image = 0;
    if (PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(static_cast<DWORD64>(at), &image, nullptr)) {
        uintptr_t start = static_cast<uintptr_t>(image) + rf->BeginAddress;
        const char* name = NameAt(start);
        snprintf(where, sizeof where, ", in the function at %s+0x%llx%s%.200s%s", base ? base + 1 : mod,
                 static_cast<unsigned long long>(start - reinterpret_cast<uintptr_t>(m)), name ? " (" : "",
                 name ? name : "", name ? ")" : "");
    }
    int n = snprintf(text, sizeof text, "fault: %s at %s+0x%llx on thread %lu (%s)%s%s%.120s%s\r\n", what,
                     base ? base + 1 : mod, static_cast<unsigned long long>(at - reinterpret_cast<uintptr_t>(m)),
                     GetCurrentThreadId(), main ? "the main thread" : "not the main thread", where,
                     lua && *lua ? ", while the Lua mod " : "", lua ? lua : "", lua && *lua ? " ran" : "");
    // a long module path and function name can come past the buffer: snprintf cuts, n is what it wanted
    if (n > 0) Append(text, (std::min)(static_cast<size_t>(n), sizeof text - 1));
    // the callers, unwound from the fault's own registers (for a load that faults in the engine's name table, only
    // the stack says which loader got there)
    if (!ep->ContextRecord) return;
    CONTEXT ctx = *ep->ContextRecord;
    for (int k = 0; k < 24 && ctx.Rip; ++k) {
        HMODULE fm = nullptr;
        char fmod[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(ctx.Rip), &fm) && fm)
            GetModuleFileNameA(fm, fmod, MAX_PATH);
        const char* fb = strrchr(fmod, '\\');
        int m2 = snprintf(text, sizeof text, "  #%02d %s+0x%llx\r\n", k, fb ? fb + 1 : fmod,
                          static_cast<unsigned long long>(ctx.Rip - reinterpret_cast<uintptr_t>(fm)));
        if (m2 > 0) Append(text, (std::min)(static_cast<size_t>(m2), sizeof text - 1));
        DWORD64 img = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &img, nullptr);
        if (!f) {  // a leaf function: its return address is on top of the stack
            if (!SafeRead(reinterpret_cast<void*>(ctx.Rsp), &ctx.Rip, 8)) break;
            ctx.Rsp += 8;
            continue;
        }
        void* handlerData = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, ctx.Rip, f, &ctx, &handlerData, &frame, nullptr);
    }
}

}  // namespace tw3se
