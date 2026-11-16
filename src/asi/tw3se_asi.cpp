// !tw3se.asi: TW3SE's second way in. The "!" puts it first: ASI loaders load the .asi files in the order of
// their names, and mods made for Witcher Native Bridge look for the bridge's functions when they load.
// Mods that bring the Ultimate ASI Loader as dinput8.dll (Witcher Native Bridge, many others) replace TW3SE's loader
// when they are installed after it. That loader loads every .asi beside
// the game, this one too, and this one starts TW3SE. With TW3SE's own dinput8.dll in place it does nothing: TW3SE runs
// already, and TW3SE does not load this file as a mod.
// No C runtime (like the loader): kernel32 only.
#include <windows.h>

namespace {

HMODULE g_self = nullptr;

bool Join(wchar_t* out, const wchar_t* a, const wchar_t* b) {
    size_t i = 0;
    for (; a[i]; ++i) {
        if (i >= MAX_PATH - 1) return false;
        out[i] = a[i];
    }
    for (size_t j = 0; b[j]; ++j, ++i) {
        if (i >= MAX_PATH - 1) return false;
        out[i] = b[j];
    }
    out[i] = 0;
    return true;
}

HMODULE Core() {
    if (HMODULE m = GetModuleHandleW(L"tw3se.dll")) return m;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_self, dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return nullptr;
    while (n > 0 && dir[n - 1] != L'\\' && dir[n - 1] != L'/') --n;
    dir[n ? n - 1 : 0] = 0;
    // beside the game (bin\x64_dx12\tw3se.asi) or in its scripts\ or plugins\ folder
    if (Join(path, dir, L"\\tw3se\\tw3se.dll") && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        return LoadLibraryW(path);
    if (Join(path, dir, L"\\..\\tw3se\\tw3se.dll") && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        return LoadLibraryW(path);
    return nullptr;
}

bool InGame() {
    wchar_t exe[MAX_PATH];
    DWORD e = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!e || e >= MAX_PATH) return false;
    const wchar_t* name = exe + e;
    while (name > exe && name[-1] != L'\\' && name[-1] != L'/') --name;
    return lstrcmpiW(name, L"witcher3.exe") == 0;
}

}  // namespace

// The Ultimate ASI Loader calls this after loading the file.
extern "C" __declspec(dllexport) void InitializeASI() {
    if (!InGame()) return;
    HMODULE core = Core();
    if (!core) return;
    using Start_t = int (*)();
    if (auto start = reinterpret_cast<Start_t>(GetProcAddress(core, "TW3SE_StartFromAsi"))) start();
}

extern "C" BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = self;
        DisableThreadLibraryCalls(self);
    }
    return TRUE;
}
