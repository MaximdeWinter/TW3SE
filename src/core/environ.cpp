// The environment in tw3se.log (25.11.: a black screen with ReShade's RenoDX, no crash and no report; the log showed
// neither the graphics card nor the other tools in the game): at the first tick the graphics cards with their
// drivers, and every module of the process that is neither Windows' nor shipped with the game by its makers -
// overlays, ReShade and its add-ons, frame generation and upscaler swaps, ASI mods. Two minutes later the modules
// that came since (overlays come in late).
#include "core.h"

#include <dxgi1_6.h>
#include <psapi.h>
#include <windows.h>

#include <cwchar>
#include <string>
#include <unordered_set>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "version.lib")

namespace tw3se {
namespace {

std::unordered_set<HMODULE> g_told;
double g_firstAt = -1;
bool g_second = false;

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// a module's company, product and version from its version resource ("" when it has none)
void VersionOf(const std::wstring& path, std::wstring& company, std::wstring& product, std::wstring& version) {
    DWORD h = 0, n = GetFileVersionInfoSizeW(path.c_str(), &h);
    if (!n) return;
    std::string buf(n, '\0');
    if (!GetFileVersionInfoW(path.c_str(), 0, n, buf.data())) return;
    struct Lang { WORD lang, cp; }* lp = nullptr;
    UINT ln = 0;
    wchar_t key[64] = L"\\StringFileInfo\\040904b0\\";
    if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&lp), &ln) && ln >= 4)
        swprintf(key, 64, L"\\StringFileInfo\\%04x%04x\\", lp->lang, lp->cp);
    auto get = [&](const wchar_t* name, std::wstring& out) {
        wchar_t* v = nullptr;
        UINT vn = 0;
        if (VerQueryValueW(buf.data(), (std::wstring(key) + name).c_str(), reinterpret_cast<void**>(&v), &vn) && v && vn)
            out.assign(v, wcsnlen(v, vn));
    };
    get(L"CompanyName", company);
    get(L"ProductName", product);
    get(L"ProductVersion", version);
    if (version.empty()) get(L"FileVersion", version);
}

// the makers of what the game ships in its own folder (their files there are the game's)
bool GameShipped(const std::wstring& company) {
    static const wchar_t* kMakers[] = {L"microsoft", L"nvidia", L"advanced micro devices", L"amd", L"intel",
                                       L"cd projekt", L"cdprojekt", L"gog", L"valve", L"rad game tools",
                                       L"epic games", L"the curl", L"daniel stenberg"};
    std::wstring c = Lower(company);
    for (const wchar_t* m : kMakers)
        if (c.find(m) != std::wstring::npos) return true;
    return false;
}

// what is everywhere and draws nothing: Windows' own, the Steam client (its overlay stays), drivers' telemetry
bool Background(const std::wstring& low, const std::wstring& company) {
    std::wstring c = Lower(company);
    if (c.find(L"microsoft") != std::wstring::npos || c.find(L"apple") != std::wstring::npos) return true;
    if (c.find(L"valve") != std::wstring::npos) return low.find(L"overlay") == std::wstring::npos;
    if (c.find(L"nvidia") != std::wstring::npos) return low.find(L"telemetry") != std::wstring::npos;
    return false;
}

void LogModules(bool late) {
    HMODULE mods[1024];
    DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    wchar_t win[MAX_PATH] = {}, exe[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring winDir = Lower(win), gameDir = Lower(exe), tw = Lower(Dir());
    for (int up = 0; up < 3; ++up) gameDir = gameDir.substr(0, gameDir.find_last_of(L'\\'));  // (bin\x64_dx12\exe)
    gameDir += L'\\';
    int n = 0;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; ++i) {
        if (!g_told.insert(mods[i]).second) continue;
        wchar_t p[MAX_PATH] = {};
        if (!GetModuleFileNameW(mods[i], p, MAX_PATH)) continue;
        std::wstring path = p, low = Lower(path);
        if (low.rfind(winDir, 0) == 0 || low.rfind(tw, 0) == 0 || mods[i] == GetModuleHandleW(nullptr)) continue;
        std::wstring company, product, version;
        VersionOf(path, company, product, version);
        const bool inGame = low.rfind(gameDir, 0) == 0;
        if (inGame && GameShipped(company)) continue;  // (a file of the game's own; a swapped one has another maker)
        if (inGame && (low.find(L"xaudio2_9redist") != std::wstring::npos || low.find(L"redgalaxy") != std::wstring::npos))
            continue;  // (the game's, without a maker)
        if (!inGame && Background(low, company)) continue;
        if (Lower(product).find(L"tw3se") != std::wstring::npos) continue;  // (its loader)
        std::wstring name = inGame ? path.substr(gameDir.size()) : path;
        Log("environment: %s%s %s%s%s%s", late ? "came later: " : "", Narrow(name).c_str(),
            product.empty() ? "" : Narrow(product).c_str(), version.empty() ? "" : " ", Narrow(version).c_str(),
            company.empty() ? "" : (" (" + Narrow(company) + ")").c_str());
        ++n;
    }
    if (!late && !n) Log("environment: no module of other tools in the game");
}

// ReShade's add-ons and the ASI mods next to the game, also those not loaded (yet)
void LogFolderFiles() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L'\\') + 1);
    std::string list;
    int n = 0;
    const struct { const wchar_t* pat; bool named; } kFiles[] = {
        {L"*.addon64", false}, {L"*.addon", false}, {L"*.asi", false}, {L"*.ini", true}};
    for (const auto& k : kFiles) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + k.pat).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring f = Lower(fd.cFileName);
            if (k.named && f.find(L"reshade") == std::wstring::npos && f.find(L"opti") == std::wstring::npos)
                continue;  // (of the .ini files only ReShade's and OptiScaler's)
            list += (n++ ? ", " : "") + Narrow(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (n) Log("environment: next to the game: %s", list.c_str());
}

void LogAdapters() {
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))) || !f) return;
    IDXGIAdapter1* a = nullptr;
    std::unordered_set<std::wstring> seen;  // (one card can be listed more than once: name and memory said once)
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND && i < 8; ++i) {
        DXGI_ADAPTER_DESC1 d = {};
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            seen.insert(std::wstring(d.Description) + std::to_wstring(d.DedicatedVideoMemory)).second) {
            LARGE_INTEGER umd = {};
            char drv[48] = "?";
            if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                snprintf(drv, sizeof drv, "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart),
                         HIWORD(umd.LowPart), LOWORD(umd.LowPart));
            const char* vendor = d.VendorId == 0x10de ? "NVIDIA" : d.VendorId == 0x1002 ? "AMD" : d.VendorId == 0x8086 ? "Intel" : "other";
            Log("environment: graphics card: %s (%s, %llu MB), driver %s", Narrow(d.Description).c_str(), vendor,
                static_cast<unsigned long long>(d.DedicatedVideoMemory >> 20), drv);
        }
        a->Release();
    }
    f->Release();
}

void OnTick(float, void*) {
    if (g_second) return;
    const double now = GetTickCount64() / 1000.0;
    if (g_firstAt < 0) {
        g_firstAt = now;
        LogAdapters();
        LogModules(false);
        LogFolderFiles();
    } else if (now - g_firstAt > 120) {
        g_second = true;
        LogModules(true);
    }
}

}  // namespace

void InstallEnvironLog() {
    if (!ConfigBool("environment_log", true)) return;
    RegisterTick(&OnTick, nullptr);
}

}  // namespace tw3se
