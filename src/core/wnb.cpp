// TW3SE core - the Witcher Native Bridge API: mods built against WitcherNativeBridge.lib run on TW3SE alone.
//
// Such a mod's .asi imports WNB_* from a module named WitcherNativeBridge.asi. TW3SE exports the same functions; when
// the bridge itself is not installed, TW3SE loads tw3se\compat\WitcherNativeBridge.asi before the .asi mods, a module
// of forwarders only (src\compat\wnb.cpp), so Windows finds the bridge's functions in tw3se.dll. With the bridge
// installed nothing of this runs: the bridge does its own work beside TW3SE (FIXES.md F1).
//
// The functions behave as the bridge's (github.com/rejuvenate7/WitcherNativeBridge, src/NativeBridge.cpp, API
// version 2): Read* read the next parameter, Return* step past the end-of-arguments marker and write the result. A
// string parameter arrives as the game's own string (UTF-8 bytes, size with the terminator) in a WNB_String; a string
// result is given as UTF-16 and stored as UTF-8.
#include "core.h"

#include <string>

namespace tw3se {
namespace {

struct WnbString {  // WNB_String: the game's String, which the bridge types as wchar_t* though it holds UTF-8
    void* data;
    uint32_t size;
    uint32_t padding;
};

bool g_compat = false;  // tw3se\compat\WitcherNativeBridge.asi loaded (the bridge itself is not installed)

void Advance(void* frame) {
    if (!frame) return;
    auto cursor = reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(frame) + 0x30);
    if (*cursor) *cursor += 1;
}

bool ReadGuarded(void* frame, void* out) {
    __try {
        ReadScriptParam(frame, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string Utf8(const wchar_t* s, size_t n) {
    if (!s || !n) return {};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string out(bytes > 0 ? bytes : 0, '\0');
    if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(n), out.data(), bytes, nullptr, nullptr);
    return out;
}

// The bridge's two script functions (modNativeBridge\content\scripts\imports.ws), when TW3SE stands in for it
void NatGetApiVersion(void*, void* frame, void* result) {
    Advance(frame);
    if (result) *static_cast<int32_t*>(result) = 2;
}
void NatStringToName(void*, void* frame, void* result) {
    alignas(16) uint8_t buf[16] = {};
    ReadScriptParam(frame, buf);
    EndScriptParams(frame);
    const char* s = *reinterpret_cast<char**>(buf);
    if (result) *static_cast<uint32_t*>(result) = s && *s && strcmp(s, "None") != 0 ? InternName(s) : 0;
}

bool FileThere(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

}  // namespace

// In DllMain, before any ASI loader can load a mod made for the bridge (the Ultimate ASI Loader as winmm.dll may load
// them before TW3SE's start-up): the module of forwarders only, when the bridge is not installed.
int g_early = 0;  // 1 loaded, 2 the bridge is installed, 3 the file is missing, 4 it did not load
DWORD g_earlyErr = 0;
void WnbCompatEarly() {
    {  // native_bridge = off in tw3se.config (read here: DllMain comes before LoadConfig)
        std::string cfg = ReadTextFile(Dir() + L"\\tw3se.config");
        for (size_t at = 0; (at = cfg.find("native_bridge", at)) != std::string::npos; ++at) {
            size_t bol = cfg.rfind('\n', at);
            bol = bol == std::string::npos ? 0 : bol + 1;
            if (cfg.find_first_not_of(" \t", bol) != at) continue;  // the key at the start of a line
            size_t eq = cfg.find('=', at), eol = cfg.find('\n', at);
            if (eq == std::string::npos || (eol != std::string::npos && eq > eol)) continue;
            std::string v = cfg.substr(eq + 1, eol == std::string::npos ? std::string::npos : eol - eq - 1);
            if (v.find("off") != std::string::npos) {
                g_early = 5;
                return;
            }
        }
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring bin(exe);
    bin = bin.substr(0, bin.find_last_of(L"\\/"));
    for (const wchar_t* sub : {L"", L"\\scripts", L"\\plugins"}) {
        if (FileThere(bin + sub + L"\\WitcherNativeBridge.asi")) {
            g_early = 2;
            return;
        }
    }
    std::wstring mine = Dir() + L"\\compat\\WitcherNativeBridge.asi";
    if (!FileThere(mine)) {
        g_early = 3;
        return;
    }
    if (!LoadLibraryW(mine.c_str())) {
        g_early = 4;
        g_earlyErr = GetLastError();
        return;
    }
    g_early = 1;
}

// At the end of the start-up: say what WnbCompatEarly found; standing in, the bridge's two script functions too.
void LoadWnbCompat() {
    switch (g_early) {
        case 2: Log("native bridge: WitcherNativeBridge.asi is installed - it does its own work"); return;
        case 3: Log("native bridge: tw3se\\compat\\WitcherNativeBridge.asi missing - mods made for the bridge need it"); return;
        case 4: Log("native bridge: tw3se\\compat\\WitcherNativeBridge.asi could not be loaded (%lu)", g_earlyErr); return;
        case 5: Log("native bridge: off (native_bridge = off in tw3se.config)"); return;
        case 1: break;
        default: return;
    }
    g_compat = true;
    RegisterNativeAnytime("WNB_GetApiVersion", &NatGetApiVersion, 0);
    RegisterNativeAnytime("WNB_StringToName", &NatStringToName, 1);
    Log("native bridge: TW3SE stands in for Witcher Native Bridge (API 2) - its mods run without it");
}

bool WnbCompatActive() { return g_compat; }

}  // namespace tw3se

using namespace tw3se;

#define WNB_EXPORT extern "C" __declspec(dllexport)

WNB_EXPORT uint32_t WNB_GetApiVersion() { return 2; }

WNB_EXPORT bool WNB_RegisterNative(const char* name, void (*impl)(void*, void*, void*)) {
    if (!name || !*name || !impl) return false;
    NoteModNative(name);
    bool ok = RegisterNativeAnytime(name, impl, -1);
    Log("native bridge: %s registered by a mod for the bridge%s", name, ok ? "" : " - FAILED");
    return ok;
}

WNB_EXPORT int WNB_ReadIntParameter(void* frame) {
    alignas(16) uint8_t b[16] = {};
    ReadGuarded(frame, b);
    return *reinterpret_cast<int32_t*>(b);
}
WNB_EXPORT bool WNB_ReadBoolParameter(void* frame) {
    alignas(16) uint8_t b[16] = {};
    ReadGuarded(frame, b);
    return b[0] != 0;
}
WNB_EXPORT float WNB_ReadFloatParameter(void* frame) {
    alignas(16) uint8_t b[16] = {};
    ReadGuarded(frame, b);
    return *reinterpret_cast<float*>(b);
}
WNB_EXPORT int32_t WNB_ReadNameParameter(void* frame) {
    alignas(16) uint8_t b[16] = {};
    ReadGuarded(frame, b);
    return *reinterpret_cast<int32_t*>(b);
}
WNB_EXPORT int WNB_ReadStringParameter(void* frame, WnbString* text) {
    if (!text) return -1;
    alignas(16) uint8_t b[16] = {};
    if (!ReadGuarded(frame, b)) return -3;
    memcpy(text, b, sizeof *text);
    return static_cast<int>(text->size);
}

WNB_EXPORT void WNB_ReturnVoid(void* frame) { Advance(frame); }
WNB_EXPORT void WNB_ReturnInt(void* frame, void* result, int value) {
    Advance(frame);
    if (result) *static_cast<int32_t*>(result) = value;
}
WNB_EXPORT void WNB_ReturnBool(void* frame, void* result, bool value) {
    Advance(frame);
    if (result) *static_cast<bool*>(result) = value;
}
WNB_EXPORT void WNB_ReturnFloat(void* frame, void* result, float value) {
    Advance(frame);
    if (result) *static_cast<float*>(result) = value;
}
WNB_EXPORT bool WNB_ReturnString(void* frame, void* result, const wchar_t* value, uint32_t length) {
    Advance(frame);
    if (!value || !result) return false;
    std::string s = Utf8(value, length);
    SetScriptResultString(result, s.data(), s.size());
    return true;
}
WNB_EXPORT void WNB_ReturnName(void* frame, void* result, int32_t value) {
    Advance(frame);
    if (result) *static_cast<int32_t*>(result) = value;
}
WNB_EXPORT void WNB_ReturnNameFromString(void* frame, void* result, const wchar_t* value) {
    Advance(frame);
    if (!result) return;
    std::string s = value ? Utf8(value, wcslen(value)) : std::string();
    *static_cast<uint32_t*>(result) = !s.empty() && s != "None" ? InternName(s.c_str()) : 0;
}
