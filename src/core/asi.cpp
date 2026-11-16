// ASI plugins: mods that come as .asi files were loaded by the Ultimate ASI Loader, which sits in
// dinput8.dll - the place TW3SE's loader takes. TW3SE loads them instead, at the moment that loader does by default
// (the exe's entry point, outside the loader lock): from bin\x64_dx12, its scripts\ and its plugins\ folder, and
// calls InitializeASI where a plugin exports it, as that loader does. Also when TW3SE itself stays passive (an unknown
// game build): those mods have nothing to do with it.
// Not when the Ultimate ASI Loader sits beside it under a name of its own (winmm.dll, dsound.dll - the game imports
// both): it loads them; nor with `asi = off` in tw3se.config. As dinput8_chain.dll the loader does not chain it.
#include "core.h"
#include "asi_loader_detect.h"

namespace tw3se {

namespace {

std::wstring ExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    return dir.substr(0, dir.find_last_of(L"\\/"));
}

using InitializeAsi_t = void (*)();

// A mod's InitializeASI under a guard: a crash in it ended the game's start. No C++ objects here (C2712).
bool InitGuarded(InitializeAsi_t init, DWORD* code) {
    __try {
        init();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

}  // namespace

// Two Ultimate ASI Loaders beside the game (one as winmm.dll, one as dinput8.dll from another mod's zip ...) load
// every .asi twice, and many mods break on their second InitializeASI (the warning on mod pages). Said once
// at TW3SE's start, whichever way TW3SE came in.
void CheckAsiLoaders() {
    const std::wstring bin = ExeDir();
    std::wstring found;
    int n = 0;
    for (const wchar_t* name : {L"dinput8.dll", L"winmm.dll", L"dsound.dll", L"version.dll", L"dxgi.dll", L"d3d12.dll",
                                L"wininet.dll", L"winhttp.dll", L"xinput9_1_0.dll", L"hid.dll"}) {
        if (!IsAsiLoaderFile(bin + L"\\" + name)) continue;
        found += (n ? L", " : L"") + std::wstring(name);
        ++n;
    }
    if (n > 1)
        Log("asi: WARNING - %d Ultimate ASI Loaders beside the game (%ls): each loads every .asi, so every .asi mod loads "
            "%d times. Keep one (TW3SE loads .asi mods itself: none is needed)", n, found.c_str(), n);
}

void LoadAsiPlugins() {
    if (!ConfigBool("asi", true)) {
        Log("asi: off (tw3se.config)");
        return;
    }
    const std::wstring bin = ExeDir();
    if (IsAsiLoaderFile(bin + L"\\dinput8_chain.dll"))
        Log("asi: dinput8_chain.dll is the Ultimate ASI Loader - not chained (under that name it stops the game); "
            "TW3SE loads the .asi mods. To keep the ASI loader, name it winmm.dll");
    for (const wchar_t* name : {L"winmm.dll", L"dsound.dll", L"version.dll", L"dxgi.dll", L"d3d12.dll",
                                L"wininet.dll", L"winhttp.dll", L"xinput9_1_0.dll", L"hid.dll"}) {
        if (IsAsiLoaderFile(bin + L"\\" + name)) {
            Log("asi: the Ultimate ASI Loader is here as %ls - .asi mods are left to it", name);
            return;
        }
    }
    int loaded = 0;
    for (const wchar_t* sub : {L"", L"\\scripts", L"\\plugins"}) {
        const std::wstring dir = bin + sub;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*.asi").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring name = std::wstring(*sub ? sub + 1 : L"") + (*sub ? L"\\" : L"") + fd.cFileName;
            if (_wcsicmp(fd.cFileName, L"!tw3se.asi") == 0) continue;  // TW3SE's own entry for other ASI loaders
            if (GetModuleHandleW((dir + L"\\" + fd.cFileName).c_str())) {  // another loader had it already
                Log("asi: %ls was loaded already (by another loader) - not again", name.c_str());
                continue;
            }
            HMODULE m = LoadLibraryW((dir + L"\\" + fd.cFileName).c_str());
            if (!m) {
                Log("asi: %ls could not be loaded (%lu)", name.c_str(), GetLastError());
                continue;
            }
            DWORD code = 0;
            auto init = reinterpret_cast<InitializeAsi_t>(GetProcAddress(m, "InitializeASI"));
            if (init && !InitGuarded(init, &code)) {
                Log("asi: %ls crashed in InitializeASI (0x%08lx); what it set up before stays, the game starts",
                    name.c_str(), code);
                continue;
            }
            Log("asi: %ls loaded", name.c_str());
            ++loaded;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (loaded) Log("asi: %d plugin(s) loaded", loaded);
}

}  // namespace tw3se
