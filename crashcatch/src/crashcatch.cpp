#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <conio.h>
#include <dbghelp.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <map>
#include <initializer_list>
#include <set>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Fmt(const char* f, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return std::string(b, n < 0 ? 0 : std::min<size_t>(static_cast<size_t>(n), sizeof b - 1));
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring BaseName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

std::wstring DirName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"." : p.substr(0, i);
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string ReadFileText(const std::wstring& p, size_t maxTail = 0) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    uint64_t from = 0;
    if (maxTail && static_cast<uint64_t>(size.QuadPart) > maxTail) from = static_cast<uint64_t>(size.QuadPart) - maxTail;
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(from);
    SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
    std::string s(static_cast<size_t>(size.QuadPart - static_cast<LONGLONG>(from)), '\0');
    DWORD got = 0;
    if (!s.empty()) ReadFile(h, s.data(), static_cast<DWORD>(s.size()), &got, nullptr);
    s.resize(got);
    CloseHandle(h);
    return s;
}

bool WriteFileText(const std::wstring& p, const std::string& s) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &w, nullptr) != 0;
    CloseHandle(h);
    return ok;
}

std::string Stamp(const SYSTEMTIME& t, bool forFile) {
    return forFile ? Fmt("%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond)
                   : Fmt("%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

SYSTEMTIME Now() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return t;
}

std::string Clock(const SYSTEMTIME& t) { return Fmt("%02u:%02u:%02u.%03u", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds); }

std::string KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::string r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = Utf8(p);
    CoTaskMemFree(p);
    return r;
}

std::string Anonymous(std::string s) {
    std::string prof = KnownFolder(FOLDERID_Profile);
    if (prof.empty()) return s;
    std::string low = s, lp = prof;
    for (auto& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (auto& c : lp) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    std::string out;
    size_t i = 0;
    for (size_t at; (at = low.find(lp, i)) != std::string::npos; i = at + lp.size()) out += s.substr(i, at - i) + "%USERPROFILE%";
    return out + s.substr(i);
}

enum Colour { kGrey = 8, kWhite = 15, kGreen = 10, kYellow = 14, kRed = 12 };

void SetColour(int c) { SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), static_cast<WORD>(c)); }

void Say(const char* f, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    SYSTEMTIME t = Now();
    SetColour(kGrey);
    printf("%02u:%02u:%02u  %s\n", t.wHour, t.wMinute, t.wSecond, b);
    SetColour(7);
    fflush(stdout);
}

void Tell(int colour, const char* f, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    SetColour(colour);
    printf("\n%s\n", b);
    SetColour(7);
    fflush(stdout);
}

bool CopyFileToClipboardNow(const std::wstring& path) {
    size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!g) return false;
    auto* df = static_cast<DROPFILES*>(GlobalLock(g));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy(reinterpret_cast<char*>(df) + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
    GlobalUnlock(g);
    if (!OpenClipboard(nullptr)) {
        GlobalFree(g);
        return false;
    }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_HDROP, g) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(g);
    return ok;
}

bool CopyFileToClipboard(const std::wstring& path) {
    struct Job {
        std::wstring path;
        bool ok = false;
    };
    auto* job = new Job{path};
    HANDLE t = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            auto* j = static_cast<Job*>(p);
            j->ok = CopyFileToClipboardNow(j->path);
            return 0;
        },
        job, 0, nullptr);
    if (!t) {
        delete job;
        return false;
    }
    if (WaitForSingleObject(t, 3000) != WAIT_OBJECT_0) {
        CloseHandle(t);
        return false;
    }
    CloseHandle(t);
    bool ok = job->ok;
    delete job;
    return ok;
}

const char* CodeText(DWORD c) {
    switch (c) {
        case 0: return "normal exit";
        case 1: return "ended with code 1 (Task Manager and many crash handlers end a process with it)";
        case 0xC0000005: return "access violation";
        case 0xC0000409: return "stack buffer overrun or fail-fast (the process ended itself at once)";
        case 0xC0000602: return "fail-fast";
        case 0xC00000FD: return "stack overflow";
        case 0xC0000374: return "heap corruption";
        case 0x80000003: return "breakpoint";
        case 0x80000004: return "single step";
        case 0xC000001D: return "illegal instruction";
        case 0xC0000096: return "privileged instruction";
        case 0xC0000094: return "integer division by zero";
        case 0xC0000095: return "integer overflow";
        case 0xC0000006: return "in-page error (a file or the page file could not be read)";
        case 0xC0000017: return "out of memory";
        case 0xC000012D: return "out of memory (the page file is full)";
        case 0xC0000135: return "a DLL was not found";
        case 0xC0000142: return "a DLL failed to start";
        case 0xC0000420: return "assertion failure";
        case 0xC000013A: return "ended by Ctrl+C or a closed console";
        case 0x40010004: return "ended by a debugger";
        case 0xE06D7363: return "C++ exception";
        case 0x406D1388: return "thread name";
        case 0x887A0005: return "DXGI: GPU device removed";
        case 0x887A0006: return "DXGI: GPU device hung";
        case 0x887A0007: return "DXGI: GPU device reset";
        default: return nullptr;
    }
}

std::string CodeLine(DWORD c) {
    const char* t = CodeText(c);
    return t ? Fmt("0x%08lx (%s)", static_cast<unsigned long>(c), t) : Fmt("0x%08lx", static_cast<unsigned long>(c));
}

bool CanEnd(DWORD c) {
    switch (c) {
        case 0xC0000005: case 0xC0000409: case 0xC0000602: case 0xC00000FD: case 0xC0000374: case 0x80000003:
        case 0xC000001D: case 0xC0000096: case 0xC0000094: case 0xC0000095: case 0xC0000006: case 0xC0000420:
        case 0xC000008C: case 0xC0000002:
            return true;
        default:
            return false;
    }
}

#include "stage2.inc"

struct Module {
    uint64_t base = 0, size = 0;
    std::wstring path;
    bool loaded = true;
    SYSTEMTIME at{};
    uint32_t stamp = 0, sum = 0;
};

struct Fault {
    SYSTEMTIME at{};
    DWORD code = 0, tid = 0;
    bool first = true;
    uint64_t addr = 0;
    ULONG_PTR info[EXCEPTION_MAXIMUM_PARAMETERS]{};
    DWORD ninfo = 0;
    std::string where, regs, bytes, stack;
    std::string verdict;
    std::string objects;
    std::wstring dump;
};

struct Options {
    std::wstring exe = L"witcher3.exe";
    std::wstring folder;
    DWORD pid = 0;
    bool keep = false, full = false, quiet = false;
    bool saves = true;
    std::wstring launch;
};

struct Session {
    Options opt;
    DWORD pid = 0;
    HANDLE proc = nullptr;
    std::wstring exePath;
    DWORD firstTid = 0;
    std::map<DWORD, HANDLE> threads;
    std::vector<Module> mods;
    std::vector<Fault> faults;
    size_t faultsSeen = 0;
    std::map<DWORD, uint64_t> counts;
    std::set<uint64_t> dumpedAt;
    std::map<uint64_t, uint64_t> seenAt;
    int dumps = 0, hangDumps = 0;
    std::vector<std::string> odsLines;
    uint64_t odsCount = 0;
    bool attachBreakSeen = false;
    SYSTEMTIME start{};
    ULONGLONG startTick = 0;
    std::wstring folder;
    std::vector<std::string> notes;
    bool symOk = false;
    bool endedNormal = false;
    ULONGLONG created = 0;
    ULONGLONG lastFaultTick = 0;
    uint64_t minCommitFree = UINT64_MAX;
    uint64_t commitLimit = 0;
    Sampler sampler;
    std::string waits;
    OtherCopies others;
};

std::wstring g_reports;
std::atomic<bool> g_stop{false};

const Module* ModuleAt(const Session& s, uint64_t a) {
    const Module* best = nullptr;
    for (const auto& m : s.mods)
        if (a >= m.base && a < m.base + m.size && (!best || m.loaded)) best = &m;
    return best;
}

#include "analyze.inc"

std::string Where(const Session& s, uint64_t a) {
    const Module* m = ModuleAt(s, a);
    if (!m) return Fmt("0x%llx", static_cast<unsigned long long>(a));
    std::string r = Fmt("%s+0x%llx", Utf8(BaseName(m->path)).c_str(), static_cast<unsigned long long>(a - m->base));
    if (s.symOk) {
        alignas(SYMBOL_INFOW) char buf[sizeof(SYMBOL_INFOW) + 256 * sizeof(wchar_t)] = {};
        auto* si = reinterpret_cast<SYMBOL_INFOW*>(buf);
        si->SizeOfStruct = sizeof(SYMBOL_INFOW);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_MODULEW64 mi{};
        mi.SizeOfStruct = sizeof mi;
        bool pdb = SymGetModuleInfoW64(s.proc, a, &mi) && mi.SymType == SymPdb;
        if (SymFromAddrW(s.proc, a, &disp, si) && si->Name[0] && disp < (pdb ? 0x100000u : 0x100u))
            return r + Fmt(" (%s+0x%llx)", Utf8(si->Name).c_str(), static_cast<unsigned long long>(disp));
    }
    std::string fn = FuncName(s, a);
    return fn.empty() ? r : r + " (" + fn + ")";
}

std::wstring PathOfHandle(HANDLE f) {
    wchar_t b[MAX_PATH * 2];
    DWORD n = f ? GetFinalPathNameByHandleW(f, b, MAX_PATH * 2, FILE_NAME_NORMALIZED) : 0;
    if (!n || n >= MAX_PATH * 2) return {};
    std::wstring p(b, n);
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + p.substr(8);
    if (p.rfind(L"\\\\?\\", 0) == 0) return p.substr(4);
    return p;
}

uint64_t ImageSize(HANDLE proc, uint64_t base) {
    IMAGE_DOS_HEADER dos{};
    SIZE_T got = 0;
    if (!ReadProcessMemory(proc, reinterpret_cast<void*>(base), &dos, sizeof dos, &got) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return 0x1000;
    IMAGE_NT_HEADERS64 nt{};
    if (!ReadProcessMemory(proc, reinterpret_cast<void*>(base + dos.e_lfanew), &nt, sizeof nt, &got)) return 0x1000;
    return nt.OptionalHeader.SizeOfImage;
}

void AddModule(Session& s, uint64_t base, HANDLE file) {
    Module m;
    m.base = base;
    m.size = ImageSize(s.proc, base);
    m.path = PathOfHandle(file);
    m.at = Now();
    if (s.symOk)
        SymLoadModuleExW(s.proc, file, m.path.empty() ? nullptr : m.path.c_str(), nullptr, base,
                         static_cast<DWORD>(m.size), nullptr, 0);
    s.mods.push_back(m);
}

std::wstring Folder(Session& s) {
    if (s.folder.empty()) {
        std::wstring name = BaseName(s.exePath);
        size_t dot = name.rfind(L'.');
        if (dot != std::wstring::npos) name.resize(dot);
        s.folder = g_reports + L"\\" + name + L"_" + Wide(Stamp(Now(), true));
        CreateDirectoryW(g_reports.c_str(), nullptr);
        CreateDirectoryW(s.folder.c_str(), nullptr);
    }
    return s.folder;
}

BOOL CALLBACK DumpCallback(PVOID, const PMINIDUMP_CALLBACK_INPUT in, PMINIDUMP_CALLBACK_OUTPUT out) {
    if (!in || !out) return TRUE;
    if (in->CallbackType == ModuleCallback) {
        std::wstring p = Lower(in->Module.FullPath ? in->Module.FullPath : L"");
        if (p.find(L"\\tw3se\\") == std::wstring::npos) out->ModuleWriteFlags &= ~ModuleWriteDataSeg;
    }
    return TRUE;
}

bool WriteDump(Session& s, const std::wstring& file, DWORD tid, EXCEPTION_RECORD* rec, CONTEXT* ctx) {
    std::wstring path = Folder(s) + L"\\" + file;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
        s.opt.full ? (MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules |
                      MiniDumpWithFullMemoryInfo)
                   : (MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules |
                      MiniDumpWithHandleData | MiniDumpWithDataSegs | MiniDumpWithFullMemoryInfo));
    EXCEPTION_POINTERS ep{rec, ctx};
    MINIDUMP_EXCEPTION_INFORMATION mei{tid, &ep, FALSE};
    MINIDUMP_CALLBACK_INFORMATION cb{&DumpCallback, nullptr};
    ULONGLONG t0 = GetTickCount64();
    BOOL ok = MiniDumpWriteDump(s.proc, s.pid, f, type, rec ? &mei : nullptr, nullptr, &cb);
    DWORD err = GetLastError();
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    CloseHandle(f);
    if (!ok) {
        DeleteFileW(path.c_str());
        Say("dump %s NOT written (error 0x%08lx)", Utf8(file).c_str(), static_cast<unsigned long>(err));
        return false;
    }
    Say("dump %s written (%.1f MB, %.1f s)", Utf8(file).c_str(), size.QuadPart / 1048576.0, (GetTickCount64() - t0) / 1000.0);
    return true;
}

std::string Regs(const CONTEXT& c) {
    return Fmt("    rax %016llx  rbx %016llx  rcx %016llx  rdx %016llx\r\n"
               "    rsi %016llx  rdi %016llx  rbp %016llx  rsp %016llx\r\n"
               "    r8  %016llx  r9  %016llx  r10 %016llx  r11 %016llx\r\n"
               "    r12 %016llx  r13 %016llx  r14 %016llx  r15 %016llx\r\n"
               "    rip %016llx  eflags %08lx\r\n",
               c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp, c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14,
               c.R15, c.Rip, static_cast<unsigned long>(c.EFlags));
}

std::string Bytes(HANDLE proc, uint64_t at) {
    std::string r;
    auto line = [&](uint64_t from, size_t n, const char* label) {
        unsigned char b[64];
        SIZE_T got = 0;
        if (!ReadProcessMemory(proc, reinterpret_cast<void*>(from), b, n, &got) || !got) {
            r += Fmt("    %s: not readable\r\n", label);
            return;
        }
        r += Fmt("    %s:", label);
        for (SIZE_T i = 0; i < got; ++i) r += Fmt(" %02x", b[i]);
        r += "\r\n";
    };
    line(at - 16, 16, "16 bytes before");
    line(at, 32, "32 bytes from it");
    return r;
}

std::string Stack(Session& s, HANDLE thread, const CONTEXT& c0) {
    std::string r;
    if (!s.symOk) return "    (no stack: dbghelp did not start)\r\n";
    CONTEXT c = c0;
    STACKFRAME64 f{};
    f.AddrPC.Offset = c.Rip, f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = c.Rbp, f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = c.Rsp, f.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 40; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, s.proc, thread, &f, &c, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) || !f.AddrPC.Offset)
            break;
        r += Fmt("    #%02d %s\r\n", i, Where(s, f.AddrPC.Offset).c_str());
    }
    uint64_t q[96];
    SIZE_T got = 0;
    if (ReadProcessMemory(s.proc, reinterpret_cast<void*>(c0.Rsp), q, sizeof q, &got) && got >= 8) {
        std::string raw;
        int n = 0;
        for (size_t i = 0; i < got / 8 && n < 24; ++i) {
            const Module* m = ModuleAt(s, q[i]);
            if (!m || m->size <= 0x1000) continue;
            raw += Fmt("    rsp+0x%03zx %s\r\n", i * 8, Where(s, q[i]).c_str());
            ++n;
        }
        if (!raw.empty()) r += "  addresses on the stack that point into a module:\r\n" + raw;
    }
    return r;
}

std::string ThreadLabel(const Session& s, DWORD tid) {
    return tid == s.firstTid ? Fmt("thread %lu (the process's first thread, usually the main thread)", tid)
                             : Fmt("thread %lu", tid);
}

void OnException(Session& s, const DEBUG_EVENT& ev, DWORD& cont) {
    const EXCEPTION_RECORD& er = ev.u.Exception.ExceptionRecord;
    DWORD code = er.ExceptionCode;
    bool first = ev.u.Exception.dwFirstChance != 0;
    cont = DBG_EXCEPTION_NOT_HANDLED;
    if (!s.attachBreakSeen && code == EXCEPTION_BREAKPOINT) {
        s.attachBreakSeen = true;
        cont = DBG_CONTINUE;
        return;
    }
    s.counts[code]++;
    if (!CanEnd(code)) return;
    uint64_t addr = reinterpret_cast<uint64_t>(er.ExceptionAddress);
    uint64_t times = ++s.seenAt[addr];
    if (first && times > 20) {
        ++s.faultsSeen;
        if (times == 21) Say("the fault at %s repeats: from now on only counted", Where(s, addr).c_str());
        return;
    }
    Fault f;
    f.at = Now();
    f.code = code, f.tid = ev.dwThreadId, f.first = first, f.addr = addr;
    f.ninfo = std::min<DWORD>(er.NumberParameters, EXCEPTION_MAXIMUM_PARAMETERS);
    for (DWORD i = 0; i < f.ninfo; ++i) f.info[i] = er.ExceptionInformation[i];
    f.where = Where(s, addr);
    HANDLE th = s.threads.count(ev.dwThreadId) ? s.threads[ev.dwThreadId] : nullptr;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_ALL;
    bool haveCtx = th && GetThreadContext(th, &ctx);
    if (haveCtx) {
        f.regs = Regs(ctx);
        f.bytes = Bytes(s.proc, ctx.Rip);
        f.stack = Stack(s, th, ctx);
        f.objects = RegObjects(s, ctx);
    }
    MEMORYSTATUSEX ms{sizeof ms};
    uint64_t commitFree = GlobalMemoryStatusEx(&ms) ? ms.ullAvailPageFile : UINT64_MAX;
    f.verdict = Verdict(s, er, ctx, haveCtx, commitFree);
    if (!first && !s.faults.empty() && s.faults.back().addr == addr && s.faults.back().tid == f.tid) {
        s.faults.back().first = false;
        Say("nothing in the game handled it: the game ends");
        return;
    }
    ++s.faultsSeen;
    bool dump = !s.dumpedAt.count(addr) && (s.dumps < 4 || !first);
    if (dump) {
        std::wstring name = Wide(Fmt("fault_%zu.dmp", s.faultsSeen));
        EXCEPTION_RECORD rec = er;
        if (WriteDump(s, name, ev.dwThreadId, &rec, haveCtx ? &ctx : nullptr)) {
            f.dump = name;
            ++s.dumps;
            s.dumpedAt.insert(addr);
        }
    }
    Say("%s %s at %s, %s", first ? "an error inside the game (it may handle it itself):" : "UNHANDLED:", CodeLine(code).c_str(),
        f.where.c_str(), ThreadLabel(s, f.tid).c_str());
    s.lastFaultTick = GetTickCount64();
    s.faults.push_back(std::move(f));
    if (s.faults.size() > 40) s.faults.erase(s.faults.begin());
}

void OnDebugString(Session& s, const DEBUG_EVENT& ev) {
    const OUTPUT_DEBUG_STRING_INFO& o = ev.u.DebugString;
    ++s.odsCount;
    size_t n = std::min<size_t>(o.nDebugStringLength, 1024);
    std::string text;
    SIZE_T got = 0;
    if (o.fUnicode) {
        std::wstring w(n, L'\0');
        if (ReadProcessMemory(s.proc, o.lpDebugStringData, w.data(), n * 2, &got)) text = Utf8(w.substr(0, got / 2));
    } else {
        text.resize(n);
        if (ReadProcessMemory(s.proc, o.lpDebugStringData, text.data(), n, &got)) text.resize(got);
        else text.clear();
    }
    while (!text.empty() && (text.back() == '\0' || text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (text.empty()) return;
    s.odsLines.push_back(Clock(Now()) + " " + text);
    if (s.odsLines.size() > 300) s.odsLines.erase(s.odsLines.begin(), s.odsLines.begin() + 50);
}

void HideDebugger(Session& s) {
    using NtQIP_t = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    auto q = reinterpret_cast<NtQIP_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    PROCESS_BASIC_INFORMATION pbi{};
    if (!q || q(s.proc, ProcessBasicInformation, &pbi, sizeof pbi, nullptr) < 0 || !pbi.PebBaseAddress) return;
    BYTE zero = 0;
    SIZE_T w = 0;
    WriteProcessMemory(s.proc, reinterpret_cast<BYTE*>(pbi.PebBaseAddress) + 2, &zero, 1, &w);
}

HWND MainWindow(DWORD pid) {
    struct Find {
        DWORD pid;
        HWND best;
        LONG area;
    } f{pid, nullptr, 0};
    EnumWindows([](HWND w, LPARAM p) -> BOOL {
        auto& f = *reinterpret_cast<Find*>(p);
        DWORD wp = 0;
        GetWindowThreadProcessId(w, &wp);
        if (wp != f.pid || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
        RECT r{};
        GetWindowRect(w, &r);
        LONG a = (r.right - r.left) * (r.bottom - r.top);
        if (a >= f.area) f.best = w, f.area = a;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.best;
}

std::string Version(const std::wstring& path) {
    DWORD h = 0, n = GetFileVersionInfoSizeW(path.c_str(), &h);
    if (!n) return {};
    std::vector<char> b(n);
    if (!GetFileVersionInfoW(path.c_str(), 0, n, b.data())) return {};
    std::string r;
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (VerQueryValueW(b.data(), L"\\", reinterpret_cast<void**>(&fi), &len) && fi)
        r = Fmt("%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS),
                LOWORD(fi->dwFileVersionLS));
    struct Lang {
        WORD lang, cp;
    }* tr = nullptr;
    if (VerQueryValueW(b.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &len) && tr && len >= 4) {
        wchar_t key[80];
        swprintf(key, 80, L"\\StringFileInfo\\%04x%04x\\CompanyName", tr->lang, tr->cp);
        wchar_t* v = nullptr;
        if (VerQueryValueW(b.data(), key, reinterpret_cast<void**>(&v), &len) && v && *v) r += ", " + Utf8(v);
    }
    return r;
}

std::string SystemText() {
    std::string r;
    using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto gv = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW v{sizeof v};
    if (gv && gv(&v) == 0) r += Fmt("Windows %lu.%lu build %lu\r\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    MEMORYSTATUSEX m{sizeof m};
    if (GlobalMemoryStatusEx(&m))
        r += Fmt("RAM %.1f GB (%.1f GB free), page file and RAM together %.1f GB (%.1f GB free)\r\n",
                 m.ullTotalPhys / 1073741824.0, m.ullAvailPhys / 1073741824.0, m.ullTotalPageFile / 1073741824.0,
                 m.ullAvailPageFile / 1073741824.0);
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    r += Fmt("%lu logical processors\r\n", si.dwNumberOfProcessors);
    return r;
}

std::string GraphicsText(const std::wstring& file) {
    std::string t = ReadFileText(file);
    if (t.empty()) return {};
    static const char* keys[] = {"raytrac", "pathtrac", "ptenable", "rtenable", "dlss", "fsr", "xess", "framegen",
                                 "upscal", "reflex", "hairworks", "dxr", "rtx", "fullscreen", "vsync"};
    std::string r, section;
    size_t i = 0;
    while (i < t.size()) {
        size_t e = t.find('\n', i);
        std::string line = t.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = e == std::string::npos ? t.size() : e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line[0] == '[') {
            section = line;
            continue;
        }
        std::string low = line;
        for (auto& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        for (const char* k : keys)
            if (low.find(k) != std::string::npos) {
                r += "    " + section + " " + line + "\r\n";
                break;
            }
    }
    return r;
}

std::vector<std::wstring> BinDirs(const Session& s) {
    std::wstring bin = DirName(s.exePath);
    std::vector<std::wstring> out{bin};
    std::wstring root = DirName(DirName(bin));
    std::wstring rel = bin.size() > root.size() ? bin.substr(root.size()) : L"";
    for (const auto& m : s.mods)
        if (Lower(BaseName(m.path)) == L"usvfs_x64.dll") out.push_back(DirName(m.path) + L"\\overwrite" + rel);
    return out;
}

std::wstring Newest(const Session& s, const std::wstring& rel) {
    std::wstring best;
    FILETIME bestT{};
    for (const auto& d : BinDirs(s)) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        std::wstring p = d + rel;
        if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) && (best.empty() || CompareFileTime(&a.ftLastWriteTime, &bestT) > 0))
            best = p, bestT = a.ftLastWriteTime;
    }
    return best;
}

std::string Tw3seMarker(const Session& s) {
    std::wstring at = Newest(s, L"\\tw3se\\out\\session.txt");
    std::string t = at.empty() ? std::string() : ReadFileText(at);
    return t.find(Fmt(", pid %lu,", s.pid)) != std::string::npos ? t : std::string();
}

void CopyIfThere(const std::wstring& from, const std::wstring& to, size_t maxTail = 0) {
    if (from.empty() || !Exists(from)) return;
    if (maxTail) WriteFileText(to, ReadFileText(from, maxTail));
    else CopyFileW(from.c_str(), to.c_str(), FALSE);
}

void CopyNewer(const std::wstring& dir, const std::wstring& pattern, const std::wstring& to, const SYSTEMTIME& since) {
    FILETIME sinceFt, sinceUtc;
    SystemTimeToFileTime(&since, &sinceFt);
    LocalFileTimeToFileTime(&sinceFt, &sinceUtc);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (CompareFileTime(&fd.ftLastWriteTime, &sinceUtc) >= 0)
            CopyFileW((dir + L"\\" + fd.cFileName).c_str(), (to + L"\\" + fd.cFileName).c_str(), FALSE);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

std::string FaultText(const Session& s, const Fault& f) {
    std::string r = Fmt("%s  %s%s at %s\r\n  %s", Clock(f.at).c_str(), f.first ? "" : "UNHANDLED (second chance) ",
                        CodeLine(f.code).c_str(), f.where.c_str(), ThreadLabel(s, f.tid).c_str());
    if (f.code == 0xC0000005 && f.ninfo >= 2)
        r += Fmt(", %s of 0x%llx", f.info[0] == 0 ? "read" : f.info[0] == 1 ? "write" : f.info[0] == 8 ? "execution" : "access",
                 static_cast<unsigned long long>(f.info[1]));
    if (f.code == 0xC0000409 && f.ninfo >= 1) r += Fmt(", fail-fast code %llu", static_cast<unsigned long long>(f.info[0]));
    r += f.dump.empty() ? "\r\n" : ", dump " + Utf8(f.dump) + "\r\n";
    if (!f.verdict.empty()) r += "  what it likely was:\r\n" + f.verdict;
    if (!f.objects.empty()) r += "  what the registers point at:\r\n" + f.objects;
    if (!f.regs.empty()) r += "  registers:\r\n" + f.regs + "  code:\r\n" + f.bytes + "  stack:\r\n" + f.stack;
    return r + "\r\n";
}

#include "diagnose.inc"
#include "deep.inc"
#include "bisect.inc"
#include "summary.inc"
#include "extras.inc"

#include "dumpread.inc"
#include "bugreport.inc"

enum class How { Ended, Detached, ByHand };

std::wstring Finish(Session& s, DWORD exitCode, How how) {
    bool detached = how == How::Detached, byHand = how == How::ByHand;
    SYSTEMTIME end = Now();
    std::string marker = Tw3seMarker(s);
    bool soonAfterFault = !byHand && s.lastFaultTick && GetTickCount64() - s.lastFaultTick < 10000;
    bool normal = how == How::Ended && exitCode == 0 && marker.empty() && !soonAfterFault;
    bool outside = how == How::Ended && exitCode == 0 && !marker.empty() && !soonAfterFault && s.faults.empty();
    s.endedNormal = normal;
    ULONGLONG minutes = (GetTickCount64() - s.startTick) / 60000;
    if (byHand) Say("a report by hand, the game goes on");
    else if (detached) Say("stopped watching the game (it goes on)");
    else if (exitCode == 0 && !normal)
        Say("the game ended with exit code 0 after %llu min, %s: a crash", minutes,
            soonAfterFault ? "right after a fault" : "but TW3SE did not see a normal end");
    else Say("the game ended: exit code %s, after %llu min", CodeLine(exitCode).c_str(), minutes);
    if (detached && s.folder.empty() && s.faults.empty()) return {};
    if (normal && (s.opt.keep ? s.folder.empty() : true)) {
        if (!s.folder.empty() && !s.opt.keep) {
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileW((s.folder + L"\\*").c_str(), &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((s.folder + L"\\" + fd.cFileName).c_str());
                while (FindNextFileW(f, &fd));
                FindClose(f);
            }
            RemoveDirectoryW(s.folder.c_str());
        }
        if (s.faultsSeen) Say("%zu error%s the game handled itself", s.faultsSeen, s.faultsSeen == 1 ? "" : "s");
        Tell(kGreen, "The game ended normally. Nothing to send.\nStart the game again, or close this window.");
        return {};
    }
    std::wstring dir = Folder(s);
    std::wstring gameBin = DirName(s.exePath);
    std::string docs = KnownFolder(FOLDERID_Documents);
    std::wstring w3docs = Wide(docs) + L"\\The Witcher 3";

    std::string r = "crashcatch report\r\n\r\n";
    r += Fmt("%s, process %lu, watched from %s to %s (%llu min)\r\n", Utf8(BaseName(s.exePath)).c_str(), s.pid,
             Stamp(s.start, false).c_str(), Stamp(end, false).c_str(), minutes);
    if (byHand) r += "The player reported a problem while the game ran (Ctrl+F12 or the key D), " +
                     Stamp(g_bug.at, false) + ".\r\n";
    if (byHand && g_bug.done) {
        r += "\r\n[what the player says is wrong]\r\n" + (g_bug.said.empty() ? std::string("(nothing written)") : g_bug.said) +
             "\r\n";
        if (!g_bug.top.empty()) r += "\r\n" + g_bug.top;
        r += "\r\n";
    }
    else if (detached) r += "Watching stopped by hand, the game was still running.\r\n";
    else
        r += "The game ended with exit code " + CodeLine(exitCode) +
             (normal ? ", a normal end.\r\n"
                     : exitCode != 0 ? ".\r\n"
                     : soonAfterFault ? Fmt(", %.1f s after the last fault (a crash handler in the game ends it so).\r\n",
                                            (GetTickCount64() - s.lastFaultTick) / 1000.0)
                                      : outside ? ", without an error, and TW3SE saw no exit of the game itself: it was "
                                                  "ended from outside (the Task Manager, a launcher) or it vanished.\r\n"
                                                : ", but TW3SE did not see a normal end (see TW3SE below).\r\n");
    if (!s.faults.empty()) {
        const Fault& last = s.faults.back();
        r += Fmt("The last fault before the end: %s at %s, %s.\r\n", CodeLine(last.code).c_str(), last.where.c_str(),
                 ThreadLabel(s, last.tid).c_str());
        if (!last.verdict.empty()) r += "What it likely was:\r\n" + last.verdict;
        std::string known = KnownCrash(FaultText(s, last));
        if (!known.empty()) r += "Known (known_crashes.txt): " + known + "\r\n";
        r += "Fingerprint: " + last.where.substr(0, last.where.find(' ')) + " (the place of the fault; the same place in "
             "another report of the same game build is the same crash)\r\n";
    } else if (how == How::Ended) {
        r += "No fault was seen: the game ended without an exception (by its own exit, an exit of a DLL in it, or from "
             "outside).\r\n";
    }
    for (const auto& n : s.notes) r += n + "\r\n";
    if (how == How::Ended) Sleep(2000);
    std::string events = SessionEvents(s.start, BaseName(s.exePath));
    r += EventsVerdict(events) + s.others.Text(s.exePath);
    {
        std::wstring dred = Newest(s, L"\\tw3se\\out\\dred.txt");
        WIN32_FILE_ATTRIBUTE_DATA a;
        FILETIME since, sinceUtc;
        SystemTimeToFileTime(&s.start, &since);
        LocalFileTimeToFileTime(&since, &sinceUtc);
        if (!dred.empty() && GetFileAttributesExW(dred.c_str(), GetFileExInfoStandard, &a) &&
            CompareFileTime(&a.ftLastWriteTime, &sinceUtc) >= 0)
            r += "The graphics card dropped the game's device (a GPU crash): dred.txt names the GPU work that did not "
                 "finish and the address it faulted on.\r\n";
    }
    if (s.minCommitFree != UINT64_MAX) {
        r += Fmt("Memory Windows had left to give out (RAM and page file together), the lowest while the game ran: %.1f GB "
                 "of %.1f GB.", s.minCommitFree / 1073741824.0, s.commitLimit / 1073741824.0);
        r += s.minCommitFree < (6ull << 30) ? " Little: a game that asks for more then crashes. A larger page file helps "
                                              "(Windows: page file size managed by the system).\r\n"
                                            : "\r\n";
    }

    if (!s.faults.empty()) {
        std::string before = LogBefore(ReadFileText(Newest(s, L"\\tw3se\\tw3se.log"), 4u << 20), s.faults.back().at, 15);
        if (!before.empty()) r += "\r\n[TW3SE's log in the 15 s before the last fault]\r\n" + before;
    }
    r += "\r\n[Windows events while the game ran (graphics driver, hardware errors, memory, the game's own entries)]\r\n" + events;
    r += "\r\n[memory over time]\r\n" + s.sampler.Summary();
    if (byHand && g_bug.done) r += g_bug.body;
    if (byHand) s.waits = WaitChains(s.pid);
    if (!s.waits.empty()) r += "\r\n[threads waiting on each other (at the hang, or now for a report by hand)]\r\n" + s.waits;
    if (!g_watch.empty()) r += "\r\n[watched addresses (--watch)]\r\n" + WatchText();
    WriteFileText(dir + L"\\memory.csv", s.sampler.Csv());

    r += "\r\n[faults, oldest first: first chance = before any handler inside the game, the last one before the end is "
         "the likely cause]\r\n";
    if (s.faults.empty()) r += "none\r\n";
    if (s.faultsSeen > s.faults.size()) r += Fmt("(%zu earlier ones left out)\r\n", s.faultsSeen - s.faults.size());
    for (const auto& f : s.faults) r += FaultText(s, f);

    r += "\r\n[TW3SE]\r\n";
    if (!Exists(gameBin + L"\\tw3se\\tw3se.dll")) r += "not installed\r\n";
    else if (marker.empty() && how != How::Ended) r += "tw3se\\out\\session.txt of this session not found\r\n";
    else if (marker.empty()) r += "session marker gone: TW3SE saw a normal end\r\n";
    else r += "tw3se\\out\\session.txt of this session (TW3SE did not see a normal end):\r\n" + marker + "\r\n";

    bool repeats = false;
    for (const auto& [a, n] : s.seenAt) repeats |= n > 20;
    if (repeats) {
        r += "\r\n[faults that repeated, how often]\r\n";
        for (const auto& [a, n] : s.seenAt)
            if (n > 20) r += Fmt("    %s: %llu\r\n", Where(s, a).c_str(), static_cast<unsigned long long>(n));
    }
    r += "\r\n[exceptions of the session, by code]\r\n";
    for (const auto& [c, n] : s.counts) r += Fmt("    %s: %llu\r\n", CodeLine(c).c_str(), static_cast<unsigned long long>(n));
    r += Fmt("    debug output lines: %llu\r\n", static_cast<unsigned long long>(s.odsCount));

    std::string gfx = GraphicsText(w3docs + L"\\dx12user.settings");
    std::string gfx11 = GraphicsText(w3docs + L"\\user.settings");
    r += "\r\n[graphics settings (dx12user.settings, user.settings)]\r\n" + (gfx.empty() && gfx11.empty() ? "not found\r\n" : gfx + gfx11);
    if (g_diag.on) {
        r += "\r\n[diagnose round: this start ran \"" +
             (g_diag.applied >= 0 ? g_diag.vs[g_diag.applied].name : std::string("as installed")) + "\"]\r\n" + DiagTable();
    }
    r += "\r\n[system]\r\n" + SystemText() + GpuText();
    std::string mods = ModFoldersText(DirName(DirName(DirName(s.exePath))));
    if (!mods.empty()) r += "\r\n[mod folders of the game]\r\n" + mods;

    r += "\r\n[debug output, the last lines]\r\n";
    size_t from = s.odsLines.size() > 200 ? s.odsLines.size() - 200 : 0;
    for (size_t i = from; i < s.odsLines.size(); ++i) r += s.odsLines[i] + "\r\n";
    if (s.odsLines.empty()) r += "none\r\n";

    r += "\r\n[modules, in load order]\r\n";
    for (const auto& m : s.mods) {
        std::string v = Version(m.path);
        r += Fmt("    %s %016llx %8llx %s%s%s\r\n", Clock(m.at).c_str(), static_cast<unsigned long long>(m.base),
                 static_cast<unsigned long long>(m.size), Utf8(m.path.empty() ? L"?" : m.path).c_str(),
                 v.empty() ? "" : ("  [" + v + "]").c_str(), m.loaded ? "" : "  (unloaded)");
    }

    std::wstring gameRoot = DirName(DirName(DirName(s.exePath)));
    WriteFileText(dir + L"\\mods_manifest.txt", Manifest(gameRoot));
    if (s.opt.saves) r += "\r\n[the newest save, to load the same moment]\r\n" + CopySaves(dir, 1);
    CopyIfThere(Newest(s, L"\\tw3se\\tw3se.log"), dir + L"\\tw3se.log", 4u << 20);
    CopyIfThere(Newest(s, L"\\tw3se\\tw3se.config"), dir + L"\\tw3se.config");
    CopyIfThere(Newest(s, L"\\tw3se\\out\\session.txt"), dir + L"\\session.txt");
    for (const auto& d : BinDirs(s)) {
        CopyNewer(d + L"\\tw3se\\out", L"hang_*.txt", dir, s.start);
        CopyNewer(d + L"\\tw3se\\out", L"crash_report*.txt", dir, s.start);
        CopyNewer(d + L"\\tw3se\\out", L"dred*.txt", dir, s.start);
    }
    CopyIfThere(w3docs + L"\\dx12user.settings", dir + L"\\dx12user.settings");
    CopyIfThere(w3docs + L"\\user.settings", dir + L"\\user.settings");
    CopyIfThere(w3docs + L"\\mods.settings", dir + L"\\mods.settings");
    CopyIfThere(w3docs + L"\\scriptslog.txt", dir + L"\\scriptslog.txt", 1u << 20);

    r += "\r\n[files in this folder]\r\n";
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                r += Fmt("    %s (%.1f MB)\r\n", Utf8(fd.cFileName).c_str(),
                         ((static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow) / 1048576.0);
        while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    WriteFileText(dir + L"\\report.txt", Anonymous(r));

    std::wstring zip = dir + L".zip";
    auto makeZip = [&] {
        DeleteFileW(zip.c_str());
        std::wstring cmd = L"tar.exe -a -c -f \"" + zip + L"\" -C \"" + dir + L"\" .";
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        bool ok = false;
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 600000);
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            ok = code == 0 && Exists(zip);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        return ok;
    };
    bool zipped = makeZip();
    uint64_t kLimit = 18ull << 20;
    if (const char* t = getenv("CRASHCATCH_ZIP_LIMIT_MB")) kLimit = static_cast<uint64_t>(atof(t) * 1048576);
    auto zipSize = [&] {
        WIN32_FILE_ATTRIBUTE_DATA a;
        return GetFileAttributesExW(zip.c_str(), GetFileExInfoStandard, &a) ? (static_cast<uint64_t>(a.nFileSizeHigh) << 32) | a.nFileSizeLow : 0;
    };
    std::wstring keep = s.faults.empty() ? std::wstring() : s.faults.back().dump;
    std::vector<std::string> leftOut;
    std::wstring aside = dir + L"_not_sent";
    auto moveAside = [&](const std::wstring& name) {
        CreateDirectoryW(aside.c_str(), nullptr);
        if (MoveFileExW((dir + L"\\" + name).c_str(), (aside + L"\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING)) leftOut.push_back(Utf8(name));
    };
    for (int step = 0; zipped && zipSize() > kLimit && step < 3; ++step) {
        if (step == 0) {
            moveAside(L"saves");
        } else if (step == 1) {
            for (auto [name, tail] : {std::pair<const wchar_t*, size_t>{L"tw3se.log", 1u << 20}, {L"scriptslog.txt", 256u << 10}})
                if (Exists(dir + L"\\" + name)) {
                    WriteFileText(dir + L"\\" + name, ReadFileText(dir + L"\\" + name, tail));
                    leftOut.push_back(Utf8(name) + " (its head)");
                }
        } else {
            WIN32_FIND_DATAW dd;
            HANDLE h = FindFirstFileW((dir + L"\\*.dmp").c_str(), &dd);
            std::vector<std::wstring> dumps;
            if (h != INVALID_HANDLE_VALUE) {
                do dumps.push_back(dd.cFileName);
                while (FindNextFileW(h, &dd));
                FindClose(h);
            }
            for (const auto& d : dumps)
                if (d != keep) moveAside(d);
        }
        std::string note = "\r\n[left out of the zip to keep it below 18 MB (they stay in " + Utf8(BaseName(aside)) + ")]\r\n";
        for (const auto& l : leftOut) note += "    " + l + "\r\n";
        WriteFileText(dir + L"\\report.txt", Anonymous(r + note));
        zipped = makeZip();
    }
    Say("report written: %s", Utf8(dir + L"\\report.txt").c_str());
    std::wstring send = zipped ? zip : dir + L"\\report.txt";
    bool copied = CopyFileToClipboard(send);
    if (byHand) Tell(kGreen, "Report saved. The game goes on.%s", g_bug.top.empty() ? "" : "\nSee the top of report.txt: a mod says something is wrong.");
    else if (detached) Tell(kWhite, "crashcatch stopped watching and saved what it saw.");
    else if (outside) Tell(kYellow, "The game closed without an error. If you closed it yourself, there is nothing to send.");
    else Tell(kRed, "The game crashed. crashcatch saved a report.");
    Tell(kWhite, "%s\n  %s%s", outside ? "If it closed by itself, please send this file:" : "Please send this file:", Utf8(send).c_str(),
         copied ? "\nIt is copied: paste it with Ctrl+V into Discord, a forum post or a mail." : "");
    if (!byHand && !detached && !s.opt.quiet) {
        std::wstring head = outside ? L"The game closed without an error.\n\nIf you closed it yourself, there is nothing to send. "
                                      L"If it closed by itself, crashcatch saved a report. Please send this file:\n"
                                    : L"The game crashed.\n\ncrashcatch saved a report. Please send this file:\n";
        std::wstring msg = head + BaseName(send) +
                           (copied ? L"\n\nIt is copied: paste it with Ctrl+V into Discord, a forum post or a mail." : L"") +
                           L"\n\nThe folder with the file opens when you click OK.";
        MessageBoxW(nullptr, msg.c_str(), L"crashcatch", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST);
    }
    return send;
}

DWORD FindGame(const Options& o, const std::set<DWORD>& done) {
    if (o.pid) return done.count(o.pid) ? 0 : o.pid;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof pe};
    DWORD best = 0;
    ULONGLONG bestT = 0;
    std::wstring want = Lower(o.exe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (Lower(pe.szExeFile) != want || done.count(pe.th32ProcessID)) continue;
        ULONGLONG t = 0;
        bool inFolder = o.folder.empty();
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
            FILETIME c, e, k, u;
            if (GetProcessTimes(h, &c, &e, &k, &u)) t = (static_cast<ULONGLONG>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
            wchar_t path[MAX_PATH * 2];
            DWORD n = MAX_PATH * 2;
            if (!inFolder && QueryFullProcessImageNameW(h, 0, path, &n)) inFolder = Lower(path).rfind(Lower(o.folder), 0) == 0;
            CloseHandle(h);
        }
        if (!inFolder) continue;
        if (!best || t > bestT) best = pe.th32ProcessID, bestT = t;
    }
    CloseHandle(snap);
    return best;
}

std::wstring Watch(const Options& o, DWORD pid) {
    Session s;
    s.opt = o;
    s.pid = pid;
    g_names.clear();
    g_namesFile.clear();
    s.start = Now();
    s.startTick = GetTickCount64();
    DWORD e = 0;
    for (int i = 0;; ++i) {
        if (DebugActiveProcess(pid)) {
            e = 0;
            break;
        }
        e = GetLastError();
        if (e == ERROR_ACCESS_DENIED || i >= 19) break;
        Sleep(500);
    }
    BOOL watched = FALSE;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid)) {
        CheckRemoteDebuggerPresent(h, &watched);
        CloseHandle(h);
    }
    if (e) {
        Say("cannot watch process %lu (error %lu)", pid, e);
        if (e == ERROR_ACCESS_DENIED)
            Tell(kRed, "crashcatch cannot watch the game: it runs as administrator.\n"
                       "Close the game and crashcatch, start crashcatch with a right-click > Run as administrator, then the game.");
        else if (watched)
            Tell(kRed, "crashcatch cannot watch the game: another crashcatch window (or another debugger) watches it already.\n"
                       "Close the game and every crashcatch window, start this crashcatch, then the game.");
        else
            Tell(kRed, "crashcatch could not watch the game (error %lu). Close the game, then start it again: crashcatch waits.", e);
        return {};
    }
    DebugSetProcessKillOnExit(FALSE);
    Say("watching %ls, process %lu", o.exe.c_str(), pid);
    bool hotkey = RegisterHotKey(nullptr, 0xCC12, MOD_CONTROL | MOD_NOREPEAT, VK_F12) != 0;
    Tell(kGreen, "The game is running and crashcatch watches it. Play as usual and leave this window open.\n"
                 "Something looks wrong, but the game goes on? Press %s: crashcatch saves a report right away.",
         hotkey ? "Ctrl+F12 in the game (or D here)" : "D here (Ctrl+F12 is taken by another program)");
    std::thread bugThread;
    DWORD exitCode = 0;
    bool ended = false, detached = false;
    ULONGLONG hungSince = 0, lastHangCheck = 0, nextHangDump = 0, lastSample = 0;
    while (!ended) {
        DEBUG_EVENT ev{};
        if (!WaitForDebugEventEx(&ev, 200)) {
            ULONGLONG now = GetTickCount64();
            int key = _kbhit() ? toupper(_getwch()) : 0;
            MSG hk;
            while (PeekMessageW(&hk, nullptr, WM_HOTKEY, WM_HOTKEY, PM_REMOVE))
                if (hk.wParam == 0xCC12) key = 'D';
            if (g_stop || key == 'Q') {
                DebugActiveProcessStop(pid);
                detached = true;
                break;
            }
            if (key == 'D' && s.proc && !g_bug.busy) {
                g_bug.busy = true;
                g_bug.done = false;
                g_bug.said.clear();
                g_bug.at = Now();
                g_bug.dir = Folder(s);
                g_bug.gameBin = DirName(s.exePath);
                g_bug.gameRoot = DirName(DirName(DirName(s.exePath)));
                g_bug.window = MainWindow(pid);
                Tell(kWhite, "Saving a report: crashcatch asks what is wrong in a window of its own.");
                if (bugThread.joinable()) bugThread.join();
                bugThread = std::thread(BugThread);
            }
            if (g_bug.busy && g_bug.done) {
                if (bugThread.joinable()) bugThread.join();
                WriteDump(s, L"now.dmp", 0, nullptr, nullptr);
                std::wstring out = Finish(s, 0, How::ByHand);
                g_bug.busy = false;
                g_bug.done = false;
                if (!out.empty() && !o.quiet) {
                    std::wstring arg = L"/select,\"" + out + L"\"";
                    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
                }
                s.folder.clear();
                s.notes.clear();
            }
            if (now - lastSample >= 5000 && s.proc) {
                lastSample = now;
                s.sampler.Sample(s.proc);
                s.others.Check(s.exePath, pid);
            }
            if (now - lastHangCheck >= 1000 && s.proc) {
                lastHangCheck = now;
                MEMORYSTATUSEX ms{sizeof ms};
                if (GlobalMemoryStatusEx(&ms)) {
                    s.minCommitFree = std::min<uint64_t>(s.minCommitFree, ms.ullAvailPageFile);
                    s.commitLimit = ms.ullTotalPageFile;
                }
                HWND w = MainWindow(pid);
                bool hung = w && IsHungAppWindow(w);
                if (!hung) hungSince = 0;
                else if (!hungSince) hungSince = now;
                else if (now - hungSince >= 20000 && now >= nextHangDump && s.hangDumps < 2) {
                    std::wstring name = Wide(Fmt("hang_%d.dmp", s.hangDumps + 1));
                    Say("the game's window has not answered for 20 s");
                    Tell(kYellow, "The game has not responded for 20 seconds. crashcatch saved how it stands.\n"
                                  "If it stays frozen, end the game (Alt+F4, or the Task Manager): crashcatch then saves the report to send.");
                    s.waits = WaitChains(pid);
                    if (WriteDump(s, name, 0, nullptr, nullptr)) {
                        s.notes.push_back(Fmt("%s: the game's window did not answer for 20 s, dump %s.", Clock(Now()).c_str(),
                                              Utf8(name).c_str()));
                    }
                    ++s.hangDumps;
                    nextHangDump = now + 600000;
                }
            }
            continue;
        }
        DWORD cont = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT: {
                auto& c = ev.u.CreateProcessInfo;
                s.proc = c.hProcess;
                s.firstTid = ev.dwThreadId;
                s.threads[ev.dwThreadId] = c.hThread;
                s.exePath = PathOfHandle(c.hFile);
                FILETIME cr, ex, k, u;
                if (GetProcessTimes(s.proc, &cr, &ex, &k, &u))
                    s.created = (static_cast<ULONGLONG>(cr.dwHighDateTime) << 32) | cr.dwLowDateTime;
                SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
                std::wstring searchPath = DirName(s.exePath);
                s.symOk = SymInitializeW(s.proc, searchPath.c_str(), FALSE) != 0;
                AddModule(s, reinterpret_cast<uint64_t>(c.lpBaseOfImage), c.hFile);
                if (c.hFile) CloseHandle(c.hFile);
                HideDebugger(s);
                s.sampler.Start(pid);
                s.others.Check(s.exePath, pid);
                break;
            }
            case CREATE_THREAD_DEBUG_EVENT:
                s.threads[ev.dwThreadId] = ev.u.CreateThread.hThread;
                if (!g_watch.empty()) ArmAll(s, ev.dwThreadId);
                break;
            case EXIT_THREAD_DEBUG_EVENT:
                s.threads.erase(ev.dwThreadId);
                break;
            case LOAD_DLL_DEBUG_EVENT:
                AddModule(s, reinterpret_cast<uint64_t>(ev.u.LoadDll.lpBaseOfDll), ev.u.LoadDll.hFile);
                if (!g_watch.empty()) ArmAll(s, ev.dwThreadId);
                if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
                break;
            case UNLOAD_DLL_DEBUG_EVENT:
                for (auto& m : s.mods)
                    if (m.loaded && m.base == reinterpret_cast<uint64_t>(ev.u.UnloadDll.lpBaseOfDll)) {
                        m.loaded = false;
                        if (s.symOk) SymUnloadModule64(s.proc, m.base);
                    }
                break;
            case OUTPUT_DEBUG_STRING_EVENT:
                OnDebugString(s, ev);
                break;
            case EXCEPTION_DEBUG_EVENT:
                if (ev.u.Exception.ExceptionRecord.ExceptionCode == EXCEPTION_SINGLE_STEP && OnWatchHit(s, ev)) break;
                OnException(s, ev, cont);
                break;
            case EXIT_PROCESS_DEBUG_EVENT:
                exitCode = ev.u.ExitProcess.dwExitCode;
                ended = true;
                break;
            default:
                break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    if (bugThread.joinable()) bugThread.join();
    g_bug.busy = g_bug.done = false;
    if (hotkey) UnregisterHotKey(nullptr, 0xCC12);
    std::wstring out = Finish(s, exitCode, detached ? How::Detached : How::Ended);
    if (g_diag.on && !s.exePath.empty()) {
        std::wstring mo2;
        for (const auto& m : s.mods)
            if (Lower(BaseName(m.path)) == L"usvfs_x64.dll") mo2 = DirName(m.path);
        RecordEnd(s.exePath, mo2, s.created, (GetTickCount64() - s.startTick) / 60000.0, !detached && !s.endedNormal,
                  !detached);
    }
    s.sampler.Stop();
    if (g_bis.on && !s.exePath.empty()) {
        bool mo2 = false;
        for (const auto& m : s.mods) mo2 |= Lower(BaseName(m.path)) == L"usvfs_x64.dll";
        RecordBisect((GetTickCount64() - s.startTick) / 60000.0, !detached && !s.endedNormal, !detached, mo2);
    }
    if (s.symOk) SymCleanup(s.proc);
    return out;
}

BOOL WINAPI OnCtrl(DWORD) {
    g_stop = true;
    Sleep(1500);
    if (g_diag.on) RestoreAll();
    if (g_bis.on) BisectRestoreAll();
    DeepRestore();
    return TRUE;
}

}

int wmain(int argc, wchar_t** argv) {
    Options o;
    std::wstring analyze, summary, checkFiles;
    bool exeGiven = false;
    std::vector<std::wstring> images;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--analyze" && i + 1 < argc) analyze = argv[++i];
        else if (a == L"--summary" && i + 1 < argc) summary = argv[++i];
        else if (a == L"--checkfiles" && i + 1 < argc) checkFiles = argv[++i];
        else if (a == L"--images" && i + 1 < argc) images.push_back(argv[++i]);
        else if (a == L"--keep") o.keep = true;
        else if (a == L"--full") o.full = true;
        else if (a == L"--quiet") o.quiet = true;
        else if (a == L"--save") o.saves = true;
        else if (a == L"--nosave") o.saves = false;
        else if (a == L"--deep") g_deep.on = true;
        else if (a == L"--watch" && i + 1 < argc) ParseWatch(argv[++i]);
        else if (!a.empty() && a[0] != L'-' && a.find_first_of(L"\\/") != std::wstring::npos && Exists(a)) {
            o.launch = L"\"" + a + L"\"";
            for (int k = i + 1; k < argc; ++k) o.launch += L" \"" + std::wstring(argv[k]) + L"\"";
            break;
        }
        else if (a == L"--diagnose") g_diag.on = true;
        else if (a == L"--bisect" && i + 1 < argc) g_bis.on = true, g_bis.root = argv[++i];
        else if (a == L"--minutes" && i + 1 < argc) g_bis.minutes = _wtof(argv[++i]);
        else if (a == L"--game" && i + 1 < argc) o.folder = argv[++i];
        else if (a == L"--pid" && i + 1 < argc) o.pid = wcstoul(argv[++i], nullptr, 10);
        else if (!a.empty() && a[0] != L'-') o.exe = a, exeGiven = true;
        else {
            printf("crashcatch [game.exe] [--game folder] [--pid N] [--keep] [--full] [--quiet] [--diagnose]\n"
                   "crashcatch --analyze <report folder | .zip | .dmp> [--images <folder>]...\n"
                   "crashcatch --bisect <game folder> [--minutes N]\n"
                   "crashcatch --summary <folder with reports>\n"
                   "crashcatch --checkfiles <game folder>   (each mod's crashcatch_files.txt against the disk)\n"
                   "crashcatch [options] <path of the game.exe> [its arguments]   (Steam: crashcatch.exe %%command%%)\n"
                   "  more options: --save (the newest saves into a report), --watch <module+0xrva|0xaddr>[:len][:w|rw]\n");
            return 2;
        }
    }
    {
        wchar_t me[MAX_PATH];
        GetModuleFileNameW(nullptr, me, MAX_PATH);
        g_selfDir = DirName(me);
    }
    if (!analyze.empty()) return Analyze(analyze, images);
    if (!summary.empty()) return Summary(summary);
    if (!checkFiles.empty()) {
        auto [top, list] = InstallCheck(checkFiles);
        printf("%s", list.c_str());
        return top.empty() ? 0 : 1;
    }
    SetConsoleCtrlHandler(&OnCtrl, TRUE);
    SetConsoleTitleW(L"crashcatch");
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_reports = DirName(self) + L"\\reports";
    g_selfDir = DirName(self);
    CreateDirectoryW(g_reports.c_str(), nullptr);
    std::wstring probe = g_reports + L"\\.write_test";
    if (!WriteFileText(probe, "")) {
        g_reports = Wide(KnownFolder(FOLDERID_Documents)) + L"\\crashcatch";
        CreateDirectoryW(g_reports.c_str(), nullptr);
    }
    DeleteFileW(probe.c_str());
    if (g_diag.on) {
        g_diagFile = DirName(self) + L"\\diagnose.txt";
        g_diagState = g_reports + L"\\diagnose_state.txt";
        std::string err;
        if (!LoadVariants(err)) {
            printf("crashcatch --diagnose: %s\n", err.c_str());
            return 2;
        }
        LoadDiagState();
        RestoreAll();
        printf("Diagnose round: each start of the game runs one line of diagnose.txt. Close crashcatch with Q or the\n"
               "window's X: it puts every change back.\n");
        PrintTable();
        ApplyNext();
    }
    g_deepState = g_reports + L"\\deep_state.txt";
    DeepRestore();
    if (g_deep.on) {
        std::string err;
        if (!DeepApply(err)) {
            Tell(kRed, "crashcatch deep: %s", err.c_str());
            return 2;
        }
    }
    if (g_bis.on) {
        if (g_diag.on) {
            printf("crashcatch: --bisect and --diagnose do not go together\n");
            return 2;
        }
        if (Exists(g_bis.root + L"\\bin")) o.folder = g_bis.root;
        g_bisFile = DirName(self) + L"\\bisect.txt";
        g_bisState = g_reports + L"\\bisect_state.txt";
        for (bool said = false; FindGame(o, {});) {
            if (!said) printf("crashcatch --bisect: close the game first, then it goes on.\n");
            said = true;
            Sleep(1000);
        }
        std::string err;
        if (!StartBisect(err)) {
            printf("crashcatch --bisect: %s\n", err.c_str());
            return 2;
        }
        printf("Bisect: each start of the game runs with half of the suspected mods, the others are parked in\n"
               "%s. Close crashcatch with Q or the window's X: it moves every mod back.\n",
               Utf8(g_bis.root + L"\\crashcatch_parked").c_str());
        PrintBisect();
        ApplyRound();
    }
    SetColour(kWhite);
    printf("crashcatch saves a report when the game crashes. All you do:\n\n"
           "  1. Leave this window open.\n"
           "  2. Start the game as usual and play.\n"
           "  3. If it crashes, crashcatch shows you the one file to send.\n\n"
           "Something looks wrong, but the game goes on? Press Ctrl+F12 in the game (or D in this window): crashcatch\n"
           "takes a picture, asks what is wrong and saves a report right away.\n");
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{sizeof pe};
        int others = 0;
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (Lower(pe.szExeFile) == L"crashcatch.exe" && pe.th32ProcessID != GetCurrentProcessId()) ++others;
        CloseHandle(snap);
        if (others && !o.quiet)
            Tell(kYellow, "Another crashcatch window is open. Close it: only one crashcatch can watch the game.");
    }
    if (g_deep.on) {
        SetColour(kYellow);
        printf("\nDeep mode: this time TW3SE also checks freed memory and GPU crashes. The game may run a little slower.\n"
               "crashcatch switches it off again when you close this window.\n");
    }
    SetColour(kGrey);
    printf("(watches %ls; reports go to %s; Q stops watching)\n", o.exe.c_str(), Utf8(g_reports).c_str());
    SetColour(7);
    if (!o.launch.empty()) {
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        std::wstring cmd = o.launch;
        std::wstring exe = cmd.substr(1, cmd.find(L'"', 1) - 1);
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, DirName(exe).c_str(), &si, &pi)) {
            printf("crashcatch: could not start %s (error %lu)\n", Utf8(exe).c_str(), GetLastError());
            return 2;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Say("started %ls", BaseName(exe).c_str());
        if (!exeGiven && Lower(BaseName(exe)).find(L"launcher") == std::wstring::npos) o.exe = BaseName(exe);
    }
    std::set<DWORD> done;
    std::wstring last;
    bool waitingSaid = false;
    while (!g_stop) {
        DWORD pid = FindGame(o, done);
        if (!pid) {
            if (!waitingSaid) Tell(kWhite, "Waiting for the game... start it now.");
            waitingSaid = true;
            if (o.pid) break;
            Sleep(250);
            if (_kbhit() && toupper(_getwch()) == 'Q') break;
            continue;
        }
        waitingSaid = false;
        done.insert(pid);
        std::wstring out = Watch(o, pid);
        if (g_diag.on && !g_stop) ApplyNext();
        if (g_bis.on && !g_stop) {
            ApplyRound();
            if (!g_bis.result.empty()) break;
        }
        if (!out.empty()) {
            last = out;
            if (!o.quiet) {
                std::wstring arg = L"/select,\"" + out + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            }
        }
        if (o.pid || !o.launch.empty()) break;
    }
    if (g_diag.on) RestoreAll();
    if (g_bis.on) BisectRestoreAll();
    DeepRestore();
    if (!o.quiet && !g_stop) {
        Tell(kWhite, "Press a key to close this window.");
        _getwch();
    }
    return 0;
}
