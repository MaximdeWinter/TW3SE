/* TW3SE from an .asi mod (TW3SE 1.2): everything a TW3SE plugin has, from the .asi you ship today.
 *
 * Nothing to link, no export to add. Without TW3SE, connect() returns false and your mod goes on as before.
 * TW3SE loads .asi files itself (bin\x64_dx12, scripts\, plugins\), so your players need no ASI loader of their own.
 *
 *     #include "tw3se/tw3se_asi.h"
 *     #include "tw3se/tw3se_imgui.h"      // when you draw with ImGui
 *
 *     static std::string Version() { return "1.0"; }
 *
 *     extern "C" __declspec(dllexport) void InitializeASI() {   // or the first call of your mod after DllMain
 *         if (!tw3se::asi::connect()) return;                   // no TW3SE: your mod as before
 *         tw3se::register_native<&Version>("MyMod_Version");    // import function MyMod_Version() : string;
 *         tw3se::imgui::on_draw(&DrawMyMenu);
 *     }
 *
 * Natives have to be registered before the game registers its own (InitializeASI and DllMain are early enough).
 * Do not call connect() inside DllMain itself when TW3SE is not loaded yet: it may load tw3se.dll, which Windows
 * allows there only for simple DLLs. InitializeASI is the right place.
 */
#pragma once
#include <windows.h>
#include "tw3se/tw3se.hpp"

namespace tw3se {
namespace asi {

/* Finds TW3SE (loaded, or tw3se\tw3se.dll beside the game), sets tw3se::api(). False when TW3SE is not installed or
 * older than `minInterface`. */
inline bool connect(uint32_t minInterface = 21) {
    if (api()) return api()->interfaceVersion >= minInterface;
    HMODULE m = GetModuleHandleW(L"tw3se.dll");
    if (!m) {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (!n || n >= MAX_PATH) return false;
        while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') --n;
        path[n] = 0;
        if (wcslen(path) + 16 >= MAX_PATH) return false;
        wcscat_s(path, L"tw3se\\tw3se.dll");
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
        m = LoadLibraryW(path);
        if (!m) return false;
    }
    using Get_t = const TW3SE_Interface* (*)();
    auto get = reinterpret_cast<Get_t>(GetProcAddress(m, "TW3SE_GetInterface"));
    const TW3SE_Interface* i = get ? get() : nullptr;
    if (!i || i->interfaceVersion < minInterface) return false;
    init(i);
    return true;
}

}  // namespace asi
}  // namespace tw3se
