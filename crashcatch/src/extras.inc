#include <wct.h>

std::string WaitChainsNow(DWORD pid) {
    HWCT wct = OpenThreadWaitChainSession(0, nullptr);
    if (!wct) return "    (wait chains not available)\r\n";
    std::string r;
    int cycles = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT];
        DWORD n = WCT_MAX_NODE_COUNT;
        BOOL cycle = FALSE;
        if (!GetThreadWaitChain(wct, 0, WCT_OUT_OF_PROC_FLAG | WCT_OUT_OF_PROC_CS_FLAG, te.th32ThreadID, &n, nodes, &cycle) ||
            n < 3)
            continue;
        std::string line = Fmt("    thread %lu", te.th32ThreadID);
        bool other = false;
        for (DWORD i = 1; i < n; ++i) {
            const auto& x = nodes[i];
            if (x.ObjectType == WctThreadType) {
                line += Fmt(" held by thread %lu%s", x.ThreadObject.ThreadId,
                            x.ThreadObject.ProcessId != pid ? Fmt(" of process %lu", x.ThreadObject.ProcessId).c_str() : "");
                other = true;
            } else {
                static const char* kinds[] = {"?", "a critical section", "a send message", "a mutex", "an ALPC call", "a COM call",
                                              "a thread wait", "a process wait", "a thread", "a COM activation", "an unknown lock",
                                              "an SMB I/O", "a socket I/O"};
                const char* k = x.ObjectType < sizeof kinds / sizeof *kinds ? kinds[x.ObjectType] : "a lock";
                line += Fmt(" waits on %s%s", k, x.LockObject.ObjectName[0] ? (" \"" + Utf8(x.LockObject.ObjectName) + "\"").c_str() : "");
            }
        }
        if (!other) continue;
        if (cycle) ++cycles, line += "  <- A DEADLOCK: the chain comes back to itself";
        r += line + "\r\n";
    }
    CloseHandle(snap);
    CloseThreadWaitChainSession(wct);
    if (r.empty()) return "    no thread waits on a lock another thread holds\r\n";
    if (cycles) r = Fmt("    %d thread(s) in a deadlock: they wait on each other, none can go on\r\n", cycles) + r;
    return r;
}

std::string WaitChains(DWORD pid) {
    struct Job {
        DWORD pid;
        std::string out;
    };
    auto* job = new Job{pid, {}};
    HANDLE t = CreateThread(
        nullptr, 0,
        [](void* p) -> DWORD {
            auto* j = static_cast<Job*>(p);
            j->out = WaitChainsNow(j->pid);
            return 0;
        },
        job, 0, nullptr);
    if (!t) {
        delete job;
        return "    (wait chains not read)\r\n";
    }
    if (WaitForSingleObject(t, 5000) != WAIT_OBJECT_0) {
        CloseHandle(t);
        return "    (wait chains not read: Windows did not answer within 5 s)\r\n";
    }
    CloseHandle(t);
    std::string r = job->out;
    delete job;
    return r;
}

std::string LogBefore(const std::string& log, const SYSTEMTIME& at, int seconds) {
    int atMs = ((at.wHour * 60 + at.wMinute) * 60 + at.wSecond) * 1000 + at.wMilliseconds;
    std::string r;
    bool in = false;
    int lines = 0, skipped = 0;
    size_t i = 0;
    while (i < log.size() && lines < 80) {
        size_t e = log.find('\n', i);
        std::string line = log.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = e == std::string::npos ? log.size() : e + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        int h, m, s, ms;
        if (line.size() > 12 && sscanf_s(line.c_str(), "%2d:%2d:%2d.%3d", &h, &m, &s, &ms) == 4 && line[2] == ':') {
            int t = ((h * 60 + m) * 60 + s) * 1000 + ms;
            int d = atMs - t;
            if (d < -43200000) d += 86400000;
            in = d <= seconds * 1000 && d >= -1000;
            if (in && (line.find("command list vtable") != std::string::npos || line.find("dump written") != std::string::npos)) {
                ++skipped;
                continue;
            }
        }
        if (in) r += "    " + line + "\r\n", ++lines;
    }
    if (skipped) r += Fmt("    (%d lines of draw hooks and dumps left out)\r\n", skipped);
    return r;
}

std::string CopySaves(const std::wstring& dir, size_t count) {
    std::wstring saves = Wide(KnownFolder(FOLDERID_Documents)) + L"\\The Witcher 3\\gamesaves";
    std::vector<std::pair<ULONGLONG, std::wstring>> all;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((saves + L"\\*.sav").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "no saves found\r\n";
    do all.push_back({(static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime, fd.cFileName});
    while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(all.rbegin(), all.rend());
    std::string r;
    CreateDirectoryW((dir + L"\\saves").c_str(), nullptr);
    for (size_t i = 0; i < all.size() && i < count; ++i) {
        std::wstring name = all[i].second, png = name.substr(0, name.size() - 4) + L".png";
        CopyFileW((saves + L"\\" + name).c_str(), (dir + L"\\saves\\" + name).c_str(), FALSE);
        CopyFileW((saves + L"\\" + png).c_str(), (dir + L"\\saves\\" + png).c_str(), FALSE);
        r += "    saves\\" + Utf8(name) + "\r\n";
    }
    return r;
}

uint32_t Fnv1a(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    uint32_t hsh = 2166136261u;
    unsigned char b[65536];
    DWORD got = 0;
    while (ReadFile(f, b, sizeof b, &got, nullptr) && got)
        for (DWORD i = 0; i < got; ++i) hsh = (hsh ^ b[i]) * 16777619u;
    CloseHandle(f);
    return hsh;
}

void ManifestDir(const std::wstring& root, const std::wstring& rel, std::string& out, int depth) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\" + rel + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring r = rel + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 10 && Lower(n) != L"crashcatch_parked") ManifestDir(root, r, out, depth + 1);
            continue;
        }
        uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        SYSTEMTIME st;
        FILETIME lt;
        FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lt);
        FileTimeToSystemTime(&lt, &st);
        out += Fmt("%s\t%llu\t%04u-%02u-%02u %02u:%02u\t%s\r\n", Utf8(r).c_str(), static_cast<unsigned long long>(size), st.wYear,
                   st.wMonth, st.wDay, st.wHour, st.wMinute, size < (8u << 20) ? Fmt("%08x", Fnv1a(root + L"\\" + r)).c_str() : "-");
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::string Manifest(const std::wstring& gameRoot) {
    std::string out = "file\tbytes\tchanged\tfingerprint (files below 8 MB)\r\n";
    ManifestDir(gameRoot, L"Mods", out, 0);
    ManifestDir(gameRoot, L"dlc", out, 0);
    ManifestDir(gameRoot, L"bin\\x64_dx12\\tw3se\\plugins", out, 0);
    ManifestDir(gameRoot, L"bin\\x64_dx12\\plugins", out, 0);
    return out;
}

struct Watchpoint {
    std::string spec;
    uint64_t addr = 0;
    int len = 8;
    bool readToo = false;
    uint64_t hits = 0;
    std::vector<std::string> first;
};
std::vector<Watchpoint> g_watch;

bool ParseWatch(const std::wstring& arg) {
    if (g_watch.size() >= 4) return false;
    Watchpoint w;
    w.spec = Utf8(arg);
    std::vector<std::string> parts = Split(w.spec, ':');
    if (parts.size() > 1) w.len = atoi(parts[1].c_str());
    if (parts.size() > 2) w.readToo = parts[2] == "rw";
    if (w.len != 1 && w.len != 2 && w.len != 4 && w.len != 8) w.len = 8;
    g_watch.push_back(w);
    return true;
}

void ResolveWatches(const Session& s) {
    for (auto& w : g_watch) {
        if (w.addr) continue;
        std::string place = Split(w.spec, ':')[0];
        size_t plus = place.find("+0x");
        if (plus == std::string::npos) {
            w.addr = strtoull(place.c_str(), nullptr, 16);
            continue;
        }
        std::wstring mod = Lower(Wide(place.substr(0, plus)));
        for (const auto& m : s.mods)
            if (m.loaded && Lower(BaseName(m.path)) == mod) w.addr = m.base + strtoull(place.c_str() + plus + 3, nullptr, 16);
    }
}

void ArmThread(HANDLE th, bool suspend) {
    if (g_watch.empty() || !th) return;
    if (suspend && SuspendThread(th) == static_cast<DWORD>(-1)) return;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(th, &c)) {
        DWORD64 dr7 = 0;
        DWORD64* drs[4] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
        for (size_t i = 0; i < g_watch.size(); ++i) {
            const auto& w = g_watch[i];
            if (!w.addr) continue;
            *drs[i] = w.addr & ~static_cast<uint64_t>(w.len - 1);
            DWORD64 rw = w.readToo ? 3 : 1, ln = w.len == 1 ? 0 : w.len == 2 ? 1 : w.len == 8 ? 2 : 3;
            dr7 |= (1ull << (i * 2)) | (rw << (16 + i * 4)) | (ln << (18 + i * 4));
        }
        c.Dr7 = dr7;
        SetThreadContext(th, &c);
    }
    if (suspend) ResumeThread(th);
}

void ArmAll(Session& s, DWORD stoppedTid) {
    ResolveWatches(s);
    (void)stoppedTid;
    for (const auto& kv : s.threads) ArmThread(kv.second, false);
}

bool OnWatchHit(Session& s, const DEBUG_EVENT& ev) {
    if (g_watch.empty()) return false;
    HANDLE th = s.threads.count(ev.dwThreadId) ? s.threads[ev.dwThreadId] : nullptr;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_ALL;
    if (!th || !GetThreadContext(th, &c)) return false;
    bool ours = false;
    for (size_t i = 0; i < g_watch.size(); ++i) {
        if (!(c.Dr6 & (1ull << i))) continue;
        ours = true;
        auto& w = g_watch[i];
        ++w.hits;
        if (w.first.size() < 20) {
            uint64_t v = 0;
            ReadSome(s.proc, w.addr, &v, w.len);
            w.first.push_back(Fmt("  %s  %s, value now %llx, the instruction before %s\r\n%s", Clock(Now()).c_str(),
                                  ThreadLabel(s, ev.dwThreadId).c_str(), static_cast<unsigned long long>(v),
                                  Where(s, c.Rip).c_str(), Stack(s, th, c).c_str()));
        }
    }
    if (ours) {
        c.Dr6 = 0;
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        SetThreadContext(th, &c);
    }
    return ours;
}

std::string WatchText() {
    std::string r;
    for (const auto& w : g_watch) {
        r += Fmt("%s (0x%llx, %d bytes, %s): %llu hit(s)\r\n", w.spec.c_str(), static_cast<unsigned long long>(w.addr), w.len,
                 w.readToo ? "read and write" : "write", static_cast<unsigned long long>(w.hits));
        for (const auto& f : w.first) r += f;
    }
    return r;
}
