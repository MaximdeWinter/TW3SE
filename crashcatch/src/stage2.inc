#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <winevt.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wevtapi.lib")

std::string EvtText(EVT_HANDLE ev, const wchar_t* provider) {
    std::string msg;
    EVT_HANDLE pub = EvtOpenPublisherMetadata(nullptr, provider, nullptr, 0, 0);
    if (pub) {
        DWORD used = 0;
        EvtFormatMessage(pub, ev, 0, 0, nullptr, EvtFormatMessageEvent, 0, nullptr, &used);
        if (used) {
            std::wstring b(used, L'\0');
            if (EvtFormatMessage(pub, ev, 0, 0, nullptr, EvtFormatMessageEvent, used, b.data(), &used)) {
                b.resize(wcslen(b.c_str()));
                msg = Utf8(b);
            }
        }
        EvtClose(pub);
    }
    for (auto& c : msg)
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    if (msg.size() > 260) msg = msg.substr(0, 257) + "...";
    return msg;
}

std::string Events(const wchar_t* channel, const std::vector<std::wstring>& providers, const SYSTEMTIME& sinceLocal,
                   const std::wstring& mustContain) {
    FILETIME lft, uft;
    SystemTimeToFileTime(&sinceLocal, &lft);
    LocalFileTimeToFileTime(&lft, &uft);
    SYSTEMTIME u;
    FileTimeToSystemTime(&uft, &u);
    std::wstring prov;
    for (const auto& p : providers) prov += (prov.empty() ? L"" : L" or ") + (L"Provider[@Name='" + p + L"']");
    wchar_t q[1024];
    swprintf(q, 1024, L"*[System[(%s) and TimeCreated[@SystemTime>='%04u-%02u-%02uT%02u:%02u:%02u.000Z']]]", prov.c_str(),
             u.wYear, u.wMonth, u.wDay, u.wHour, u.wMinute, u.wSecond);
    EVT_HANDLE rs = EvtQuery(nullptr, channel, q, EvtQueryChannelPath | EvtQueryForwardDirection);
    if (!rs) return {};
    std::string out;
    EVT_HANDLE evs[32];
    DWORD got = 0;
    int lines = 0;
    while (lines < 60 && EvtNext(rs, 32, evs, 2000, 0, &got)) {
        for (DWORD i = 0; i < got; ++i) {
            EVT_HANDLE ctx = EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem);
            DWORD used = 0, props = 0;
            EvtRender(ctx, evs[i], EvtRenderEventValues, 0, nullptr, &used, &props);
            std::vector<BYTE> buf(used);
            std::wstring provider;
            unsigned id = 0;
            SYSTEMTIME at{};
            if (used && EvtRender(ctx, evs[i], EvtRenderEventValues, used, buf.data(), &used, &props)) {
                auto* v = reinterpret_cast<EVT_VARIANT*>(buf.data());
                if (v[EvtSystemProviderName].Type == EvtVarTypeString && v[EvtSystemProviderName].StringVal)
                    provider = v[EvtSystemProviderName].StringVal;
                id = v[EvtSystemEventID].UInt16Val;
                FILETIME ft, lt;
                ULONGLONG t = v[EvtSystemTimeCreated].FileTimeVal;
                ft.dwLowDateTime = static_cast<DWORD>(t), ft.dwHighDateTime = static_cast<DWORD>(t >> 32);
                FileTimeToLocalFileTime(&ft, &lt);
                FileTimeToSystemTime(&lt, &at);
            }
            EvtClose(ctx);
            std::string text = EvtText(evs[i], provider.c_str());
            EvtClose(evs[i]);
            if (!mustContain.empty() && Lower(Wide(text)).find(Lower(mustContain)) == std::wstring::npos) continue;
            out += Fmt("    %02u:%02u:%02u  %s %u: %s\r\n", at.wHour, at.wMinute, at.wSecond, Utf8(provider).c_str(), id,
                       text.empty() ? "(no text)" : text.c_str());
            ++lines;
        }
    }
    EvtClose(rs);
    return out;
}

std::string SessionEvents(const SYSTEMTIME& since, const std::wstring& exeName) {
    std::string r;
    std::string sys = Events(L"System",
                             {L"Display", L"amdkmdag", L"amdwddmg", L"nvlddmkm", L"igfx", L"igfxn", L"dxgkrnl",
                              L"Microsoft-Windows-DxgKrnl", L"Microsoft-Windows-WHEA-Logger", L"Microsoft-Windows-Kernel-Power",
                              L"Microsoft-Windows-Resource-Exhaustion-Detector", L"volmgr", L"disk", L"Ntfs"},
                             since, L"");
    std::string app = Events(L"Application", {L"Application Error", L"Application Hang", L"Windows Error Reporting"}, since,
                             exeName);
    r += sys.empty() ? "    none in the System log (no graphics driver reset, no hardware error, no memory warning)\r\n" : sys;
    if (!app.empty()) r += app;
    return r;
}

std::string EventsVerdict(const std::string& ev) {
    std::string r;
    if (ev.find("Display 4101") != std::string::npos || ev.find("amdkmdag") != std::string::npos ||
        ev.find("nvlddmkm") != std::string::npos || ev.find("dxgkrnl") != std::string::npos ||
        ev.find("DxgKrnl") != std::string::npos)
        r += "The graphics driver reported a problem or was reset while the game ran (see [Windows events]): a driver, an "
             "overclock or the card itself, more likely than a mod.\r\n";
    if (ev.find("WHEA-Logger") != std::string::npos)
        r += "Windows logged a hardware error while the game ran (WHEA, see [Windows events]): the processor, memory or a "
             "card, often an overclock or undervolt.\r\n";
    if (ev.find("Resource-Exhaustion-Detector") != std::string::npos)
        r += "Windows ran short of memory while the game ran (see [Windows events]).\r\n";
    if (ev.find("Kernel-Power 41") != std::string::npos)
        r += "The PC restarted without shutting down while the game ran (Kernel-Power 41): power or hardware.\r\n";
    return r;
}

std::string GpuText() {
    std::string r;
    std::set<std::wstring> cards;
    std::set<std::pair<DWORD, LONG>> luids;
    IDXGIFactory1* f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory >= (256ull << 20) &&
                luids.insert({d.AdapterLuid.LowPart, d.AdapterLuid.HighPart}).second && !cards.count(d.Description)) {
                r += Fmt("GPU %s, %.1f GB of its own memory\r\n", Utf8(d.Description).c_str(), d.DedicatedVideoMemory / 1073741824.0);
                cards.insert(d.Description);
            }
            a->Release();
        }
        f->Release();
    }
    HKEY cls;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}", 0,
                      KEY_READ, &cls) == ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            wchar_t sub[64];
            DWORD n = 64;
            if (RegEnumKeyExW(cls, i, sub, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            wchar_t desc[256] = L"", ver[64] = L"", date[64] = L"";
            DWORD sz = sizeof desc;
            if (RegGetValueW(cls, sub, L"DriverDesc", RRF_RT_REG_SZ, nullptr, desc, &sz) != ERROR_SUCCESS) continue;
            if (!cards.empty() && !cards.count(desc)) continue;
            sz = sizeof ver;
            RegGetValueW(cls, sub, L"DriverVersion", RRF_RT_REG_SZ, nullptr, ver, &sz);
            sz = sizeof date;
            RegGetValueW(cls, sub, L"DriverDate", RRF_RT_REG_SZ, nullptr, date, &sz);
            r += Fmt("driver %s: version %s (%s)\r\n", Utf8(desc).c_str(), Utf8(ver).c_str(), Utf8(date).c_str());
        }
        RegCloseKey(cls);
    }
    return r;
}

struct Sampler {
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER procGpu = nullptr, adapterGpu = nullptr;
    DWORD pid = 0;
    struct Row {
        SYSTEMTIME at;
        double commitFreeGb, gamePrivateGb, gameGpuGb, gpuUsedGb;
    };
    std::vector<Row> rows;
    bool Start(DWORD p) {
        pid = p;
        if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return false;
        PdhAddEnglishCounterW(q, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &procGpu);
        PdhAddEnglishCounterW(q, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &adapterGpu);
        PdhCollectQueryData(q);
        return true;
    }
    double Sum(PDH_HCOUNTER c, bool ofPid) {
        if (!c) return -1;
        DWORD size = 0, n = 0;
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &n, nullptr) != PDH_MORE_DATA || !size) return -1;
        std::vector<BYTE> b(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(b.data());
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &n, items) != ERROR_SUCCESS) return -1;
        std::wstring want = L"pid_" + std::to_wstring(pid) + L"_";
        double sum = 0;
        bool any = false;
        for (DWORD i = 0; i < n; ++i) {
            if (ofPid && !wcsstr(items[i].szName, want.c_str())) continue;
            if (items[i].FmtValue.CStatus == ERROR_SUCCESS) sum += items[i].FmtValue.doubleValue, any = true;
        }
        return any ? sum : -1;
    }
    void Sample(HANDLE proc) {
        Row r{};
        r.at = Now();
        MEMORYSTATUSEX ms{sizeof ms};
        r.commitFreeGb = GlobalMemoryStatusEx(&ms) ? ms.ullAvailPageFile / 1073741824.0 : -1;
        PROCESS_MEMORY_COUNTERS_EX pm{};
        pm.cb = sizeof pm;
        r.gamePrivateGb = GetProcessMemoryInfo(proc, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm)
                              ? pm.PrivateUsage / 1073741824.0 : -1;
        r.gameGpuGb = r.gpuUsedGb = -1;
        if (q && PdhCollectQueryData(q) == ERROR_SUCCESS) {
            double g = Sum(procGpu, true), a = Sum(adapterGpu, false);
            r.gameGpuGb = g < 0 ? -1 : g / 1073741824.0;
            r.gpuUsedGb = a < 0 ? -1 : a / 1073741824.0;
        }
        rows.push_back(r);
        if (rows.size() > 20000) rows.erase(rows.begin(), rows.begin() + 10000);
    }
    void Stop() {
        if (q) PdhCloseQuery(q);
        q = nullptr;
    }
    std::string Csv() const {
        std::string r = "time,windows_commit_free_gb,game_private_gb,game_gpu_memory_gb,gpu_memory_used_gb\r\n";
        for (const auto& x : rows)
            r += Fmt("%02u:%02u:%02u,%.2f,%.2f,%.2f,%.2f\r\n", x.at.wHour, x.at.wMinute, x.at.wSecond, x.commitFreeGb,
                     x.gamePrivateGb, x.gameGpuGb, x.gpuUsedGb);
        return r;
    }
    std::string Summary() const {
        if (rows.empty()) return "    no samples\r\n";
        auto col = [&](const char* name, double Row::*f, bool low) {
            double first = rows.front().*f, last = rows.back().*f, ext = first;
            for (const auto& x : rows)
                if (x.*f >= 0) ext = low ? std::min(ext, x.*f) : std::max(ext, x.*f);
            for (const auto& x : rows)
                if (x.*f >= 0) {
                    first = x.*f;
                    break;
                }
            if (first < 0 && last < 0) return Fmt("    %-34s not available\r\n", name);
            if (ext < 0) ext = first;
            return Fmt("    %-34s %6.1f GB at the start, %s %6.1f GB, %6.1f GB at the end\r\n", name, first,
                       low ? "lowest " : "highest", ext, last);
        };
        return col("Windows memory left to give out", &Row::commitFreeGb, true) +
               col("the game's own memory", &Row::gamePrivateGb, false) +
               col("the game's graphics memory", &Row::gameGpuGb, false) +
               col("graphics memory used (all programs)", &Row::gpuUsedGb, false) +
               Fmt("    (%zu samples every 5 s in memory.csv)\r\n", rows.size());
    }
};

struct OtherCopies {
    std::map<DWORD, std::pair<SYSTEMTIME, SYSTEMTIME>> seen;
    void Check(const std::wstring& exe, DWORD self) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return;
        PROCESSENTRY32W pe{sizeof pe};
        std::wstring want = Lower(BaseName(exe));
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (pe.th32ProcessID != self && Lower(pe.szExeFile) == want) {
                auto it = seen.find(pe.th32ProcessID);
                if (it == seen.end()) {
                    seen[pe.th32ProcessID] = {Now(), Now()};
                    Say("another %ls runs at the same time: process %lu", pe.szExeFile, pe.th32ProcessID);
                } else {
                    it->second.second = Now();
                }
            }
        CloseHandle(snap);
    }
    std::string Text(const std::wstring& exe) const {
        std::string r;
        for (const auto& [pid, t] : seen)
            r += Fmt("Another %s ran at the same time: process %lu, seen from %s to %s. Two copies of the game share their "
                     "log files and twice the memory.\r\n",
                     Utf8(BaseName(exe)).c_str(), pid, Clock(t.first).substr(0, 8).c_str(), Clock(t.second).substr(0, 8).c_str());
        return r;
    }
};

void FolderStats(const std::wstring& dir, uint64_t& files, uint64_t& bytes, FILETIME& newest, int depth) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 8) FolderStats(dir + L"\\" + n, files, bytes, newest, depth + 1);
        } else {
            ++files;
            bytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            if (CompareFileTime(&fd.ftLastWriteTime, &newest) > 0) newest = fd.ftLastWriteTime;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::string ModFoldersText(const std::wstring& gameRoot) {
    std::string r;
    for (const wchar_t* sub : {L"Mods", L"dlc"}) {
        std::wstring dir = gameRoot + L"\\" + sub;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            uint64_t files = 0, bytes = 0;
            FILETIME newest{};
            FolderStats(dir + L"\\" + n, files, bytes, newest, 0);
            FILETIME lt;
            SYSTEMTIME st{};
            FileTimeToLocalFileTime(&newest, &lt);
            FileTimeToSystemTime(&lt, &st);
            r += Fmt("    %s\\%s: %llu files, %.1f MB, newest %04u-%02u-%02u\r\n", Utf8(sub).c_str(), Utf8(n).c_str(),
                     static_cast<unsigned long long>(files), bytes / 1048576.0, st.wYear, st.wMonth, st.wDay);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return r;
}
