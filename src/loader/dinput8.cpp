// TW3SE loader (dinput8.dll) - the game imports DirectInput8Create from dinput8.dll, and Windows looks for it in the
// game's folder first. This DLL forwards that one function to the system's dinput8.dll and loads the extender core,
// tw3se\tw3se.dll, from the same folder. Nothing else: a missing or broken core only means the game runs without TW3SE.
// Another mod's dinput8.dll (an ASI loader, an input mod) renamed to dinput8_chain.dll is loaded first and gets the
// game's DirectInput8Create, as if it were still in its place.
// Built without the C runtime, on kernel32 alone (with the static runtime, virus scanners take this small DLL for a
// trojan).
#include <windows.h>

#pragma function(memset)
extern "C" void* __cdecl memset(void* dst, int c, size_t n) {  // the compiler may emit calls to it
    unsigned char* p = static_cast<unsigned char*>(dst);
    while (n--) *p++ = static_cast<unsigned char>(c);
    return dst;
}

namespace {

HMODULE g_real = nullptr;
HMODULE g_chain = nullptr;  // another mod's dinput8.dll, renamed dinput8_chain.dll

// dst = a + b; false when it does not fit into MAX_PATH
bool Join(wchar_t* dst, const wchar_t* a, const wchar_t* b) {
    int n = 0;
    const wchar_t* parts[] = {a, b};
    for (const wchar_t* s : parts)
        for (; *s; ++s) {
            if (n >= MAX_PATH - 1) return false;
            dst[n++] = *s;
        }
    dst[n] = 0;
    return true;
}

bool Contains(const char* data, SIZE_T size, const char* text) {
    SIZE_T len = 0;
    while (text[len]) ++len;
    for (SIZE_T i = 0; i + len <= size; ++i) {
        SIZE_T k = 0;
        while (k < len && data[i + k] == text[k]) ++k;
        if (k == len) return true;
    }
    return false;
}

// Is this file the Ultimate ASI Loader? (Under the name dinput8_chain.dll it stops the game: it decides by
// its own file name what to be, and an unknown one opens a dialog. Its file carries its name as text.) The core has
// the same check in asi_loader_detect.h.
bool IsAsiLoaderFile(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= (16 << 20)) {
        if (HANDLE m = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr)) {
            if (const char* p = static_cast<const char*>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0))) {
                SIZE_T n = static_cast<SIZE_T>(size.QuadPart);
                found = Contains(p, n, "Ultimate-ASI-Loader") || Contains(p, n, "Ultimate ASI Loader");
                UnmapViewOfFile(p);
            }
            CloseHandle(m);
        }
    }
    CloseHandle(h);
    return found;
}

bool SameName(const wchar_t* a, const wchar_t* b) {
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

FARPROC Real(const char* name) {
    if (g_chain) {
        if (FARPROC f = GetProcAddress(g_chain, name)) return f;
    }
    if (!g_real) {
        wchar_t sys[MAX_PATH], path[MAX_PATH];
        UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n && n < MAX_PATH && Join(path, sys, L"\\dinput8.dll")) g_real = LoadLibraryW(path);
    }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}

using Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
Create_t g_create = nullptr;

}  // namespace

// g_create is taken in DllMain, before any overlay is in the game (the Epic overlay hooks
// DirectInput8Create of "dinput8.dll" - this file - and answers GetProcAddress for that name with its own detour.
// Asked at the first call, the system's DirectInput8Create came back as that detour, whose original is this function
// again: the two called each other until the stack overflowed, before the first frame)
extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* o,
                                                                    LPUNKNOWN u) {
    if (!g_create) g_create = reinterpret_cast<Create_t>(Real("DirectInput8Create"));
    return g_create ? g_create(h, v, r, o, u) : E_FAIL;
}

// The DLL's entry point (/ENTRY:DllMain, there is no runtime to call it)
extern "C" BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        wchar_t dir[MAX_PATH], path[MAX_PATH];
        DWORD n = GetModuleFileNameW(self, dir, MAX_PATH);
        if (!n || n >= MAX_PATH) return TRUE;
        while (n > 0 && dir[n - 1] != L'\\' && dir[n - 1] != L'/') --n;
        dir[n ? n - 1 : 0] = 0;
        // (not the Ultimate ASI Loader: under this name it stops the game - TW3SE loads the .asi mods then)
        if (Join(path, dir, L"\\dinput8_chain.dll") && !IsAsiLoaderFile(path))
            g_chain = LoadLibraryW(path);  // (null when there is none)
        g_create = reinterpret_cast<Create_t>(Real("DirectInput8Create"));
        // The core only hooks the exe's entry point here; its start-up runs later, outside the loader lock.
        // Only in the game and the REDkit editor. REDkit's other programs (wcc_lite, bundlebuilder, r4Launcher) import
        // dinput8.dll from the same folder: they get the forwarding and nothing else.
        wchar_t exe[MAX_PATH];
        DWORD e = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (!e || e >= MAX_PATH) return TRUE;
        const wchar_t* name = exe + e;
        while (name > exe && name[-1] != L'\\' && name[-1] != L'/') --name;
        if ((SameName(name, L"witcher3.exe") || SameName(name, L"editor.exe")) && Join(path, dir, L"\\tw3se\\tw3se.dll"))
            LoadLibraryW(path);
    }
    return TRUE;
}
