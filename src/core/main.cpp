// TW3SE core (tw3se.dll) - start-up.
//
// Loaded by the dinput8.dll loader while witcher3.exe resolves its imports (loader lock held: do almost nothing).
// DllMain only hooks the exe's entry point. The real start-up runs in that hook: on the main thread, outside the loader
// lock, before the engine initialises - addresses, hooks, plugins, console - and then the game's own entry runs.
#include "core.h"
#include "MinHook.h"
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <vector>

namespace tw3se {
namespace {

std::wstring g_dir;
uintptr_t g_base = 0;
uint32_t g_build = 0;
HMODULE g_self = nullptr;

using Entry_t = int(WINAPI*)(void* peb);  // the loader passes the PEB
Entry_t g_entry = nullptr;

// REDkit editor.exe: the editor's address table grows stage by stage (docs/PLAN_REDKIT_VOLL.md). A feature installs
// in full when every address it names resolves there. Otherwise only its natives are registered, so a script that
// imports them still compiles, and they return their empty value. Every feature on the main thread names "tick"
// (the editor's engine tick hook, InstallMainThread), because without it its work would never run.
struct EditorFeature {
    const char* name;
    void (*install)();
    void (*nativesOnly)();
    const char* needs[6];
};

void NothingToQueue() {}

const EditorFeature kEditorFeatures[] = {
    // without Memory::Free a handle block whose object is gone stays allocated; moves need CEntity.vtTeleport
    {"worldedit", &InstallWorldEdit, &QueueWorldEditNatives, {"CEntity::AttachToWorld", "tick"}},
    // without Memory::Free (the editor has no flat one) a call's out-arrays stay allocated instead of being freed;
    // CClass.properties stands for eb's check of every CClass offset lua.cpp reads (parent, lists, size)
    {"lua", &InstallLua, &QueueLuaNatives,
     {"GScriptGlobals", "GScriptMethodTable", "CRTTISystem::FindType", "CClass.properties", "tick"}},
    // events need only the log hook; `exec` runs through ConsoleExecCommand_ed and checks it itself
    {"link", &InstallLink, &QueueLinkNatives, {"ScriptLogChannelText_ed"}},
    {"foliage", &InstallFoliage, &InstallFoliage, {}},  // natives only; each checks its engine functions itself
    {"lifecycle", &InstallLifecycle, &NothingToQueue, {"CCommonGame::OnGameStart"}},  // OnGameLoaded on Play
    // the lines go out through link's event queue; the block's GUID is read through RTTI, no block fields needed
    {"questlive", &InstallQuestLive, &NothingToQueue, {"CQuestThread::ActivateBlock", "ScriptLogChannelText_ed"}},
    {"catalog", &InstallCatalog, &NothingToQueue, {"tick"}},  // reads the RTTI maps via the editor's fields
    // off unless config posehook=diag|on|selfcb; the pose fields sit 0x10 further back in the editor (posehook.cpp)
    {"posehook", &InstallPoseHook, &NothingToQueue, {"Combine_Format1", "Combine_Format2"}},
};

void StartUpEditor() {
    Log("REDkit editor detected - installing the features its address table covers");
    InstallEditorGuard();  // first: TW3SE's __except blocks work only with it
    InstallHwWatch();      // `hwwatch` for the analysis side (config hwwatch1..4 arms at start-up)
    StartStackSampler();  // finds the editor's per-frame chain (config stacksample=on)
    InstallEditorProbes();  // candidates, checked live (config attachprobe=on)
    InstallDump();          // `enginedump` for the analysis side
    InstallScripting();
    bool tick = InstallMainThread();
    Log("main thread: %s", tick ? "editor engine tick hooked" : "no tick in the editor table");
    for (const EditorFeature& f : kEditorFeatures) {
        const char* missing = nullptr;
        for (const char* n : f.needs)
            if (n && !(std::strcmp(n, "tick") == 0 ? tick : Address(n) != 0)) {
                missing = n;
                break;
            }
        if (missing) {
            f.nativesOnly();
            Log("%s: natives only in the REDkit editor (missing %s)", f.name, missing);
        } else {
            f.install();
        }
    }
    StartConsole();  // pipe tw3se_redkit
    LoadPlugins();   // bin\x64_RedKit\tw3se\plugins; a plugin tells the editor by gameBuild
    Log("ready (REDkit editor)");
}

bool g_safe = false;
bool g_noPlugins = false, g_noLua = false;  // safe mode, or one half of it

// When a game patch changed a function TW3SE cannot start without, it stays passive: Lua mods do not run, and WitcherScript mods
// that import TW3SE's functions fail to compile (the game closes by itself). A player sees "it crashes". So once per
// game build a message box says what it is (tw3se\out\passive_build.txt keeps the build it was shown for;
// passive_message = off leaves it out). The engine has no window yet: the box comes before the game's.
void TellPassiveOnce() {
    if (!ConfigBool("passive_message", true) || IsRedkitEditor()) return;
    std::wstring out = g_dir + L"\\out";
    CreateDirectoryW(out.c_str(), nullptr);
    std::wstring file = out + L"\\passive_build.txt";
    char build[16];
    snprintf(build, sizeof build, "%08x", g_build);
    if (ReadTextFile(file) == build) return;
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, build, static_cast<DWORD>(strlen(build)), &w, nullptr);
        CloseHandle(h);
    }
    wchar_t text[640];
    swprintf_s(text,
               L"TW3SE %d.%d.%d does not support this version of the game yet (build %hs).\n\n"
               L"TW3SE stays off until an update of TW3SE supports it. Lua mods do not run, and mods that need TW3SE "
               L"may close the game at its start.\n\nThis message shows once for this game version.",
               TW3SE_VERSION_MAJOR, TW3SE_VERSION_MINOR, TW3SE_VERSION_PATCH, build);
    MessageBoxW(nullptr, text, L"TW3SE", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
}

// The store the game came from, by what each one leaves in the game folder: GOG a goggame-<id>.info, the Epic Games
// Store an .egstore folder, the Microsoft Store a MicrosoftGame.config, Steam its steamapps folder in the path. (Galaxy64.dll comes with every version.)
const char* GameStore() {
    std::wstring root = g_dir + L"\\..\\..\\..";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + L"\\goggame-*.info").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        return "GOG";
    }
    if (GetFileAttributesW((root + L"\\.egstore").c_str()) != INVALID_FILE_ATTRIBUTES) return "Epic";
    if (GetFileAttributesW((root + L"\\MicrosoftGame.config").c_str()) != INVALID_FILE_ATTRIBUTES) return "Microsoft Store";
    std::wstring low = g_dir;
    for (wchar_t& c : low) c = towlower(c);
    if (low.find(L"\\steamapps\\") != std::wstring::npos) return "Steam";
    return "unknown";
}

void StartUp() {
    TakeLastSession();  // before LogOpen empties tw3se.log
    LogOpen(g_dir + L"\\tw3se.log");
    Log("TW3SE %d.%d.%d in pid %lu, witcher3.exe build %08x, store %s", TW3SE_VERSION_MAJOR, TW3SE_VERSION_MINOR,
        TW3SE_VERSION_PATCH, GetCurrentProcessId(), g_build, GameStore());
    LoadConfig();
    // safe mode: Shift held while the game starts (or safe_mode = on): the Lua mods and the plugins stay out, to see
    // whether one of them makes the game crash
    g_safe = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 || ConfigBool("safe_mode", false);
    if (g_safe) Log("safe mode: %s - no Lua mods, no plugins", ConfigBool("safe_mode", false) ? "safe_mode = on" : "Shift held");
    // one half of it (to find which half a crash comes with): safe_mode = plugins | lua
    std::string half = ConfigGet("safe_mode", "off");
    g_noPlugins = g_safe || half == "plugins";
    g_noLua = g_safe || half == "lua";
    if (!g_safe && g_noPlugins) Log("safe mode, half: safe_mode = plugins - no plugins, the Lua mods run");
    if (!g_safe && g_noLua) Log("safe mode, half: safe_mode = lua - no Lua mods, the plugins run");
    WriteCrashReport();  // tw3se\out\crash_report.txt when the last session did not end normally
    InstallDred();       // DRED before the game makes its D3D12 device (dred = on)
    if (!ResolveAddresses()) {
        Log("this build of the game is not supported yet - TW3SE stays passive");
        TellPassiveOnce();
        return;
    }
    if (IsRedkitEditor()) {
        StartUpEditor();
        return;
    }
    FocusStart();        // the window in front now: the game window may take the focus from it at the first ticks
    BuildLooseFiles();   // tw3se\files\ into a mod's bundle, before the game scans its mods
    InstallGameGuard();  // first: the game's crash handler must not see the faults TW3SE's guards take
    InstallDiagnostics();
    InstallHwWatch();
    if (!InstallMainThread()) {
        Log("main thread hook failed - TW3SE stays passive");
        return;
    }
    RegisterContentCommands();
    InstallScripting();
    InstallScriptStack();  // the script call stack with file and line, for faults, hangs and w3.script_stack()
    InstallQuarantine();   // freed memory held back with its free's stack (config quarantine; off by default)
    InstallCatalog();  // `catalog`: all RTTI types and functions to a file
    InstallDump();     // `enginedump`: globals, vtables, object dumps, threads (compared with the REDkit editor's)
    InstallLua();  // Lua on top of the RTTI (`lua`, `luafile`, tw3se\lua\autorun)
    InstallLink();  // exec + events for an external tool
    InstallOverlay();  // D3D12 present hook (overlay foundation); safe no-op if it cannot install
    InstallShaders();  // pipeline creation: shaders dumped / replaced (only with tw3se\shaders or shader_dump)
    InstallSkinHook();  // the skinning shader hook (skin_hook = on): before the game makes its root signatures
    InstallReload();   // files changed in tw3se\files load anew while the game runs
    InstallDeform();   // vertex deformation (for now: measuring where meshes are on the GPU)
    InstallLifecycle();  // OnGameLoaded event (game start / save load)
    InstallWorldEdit();  // entity-attach hook (world-edit persistence foundation)
    InstallSectorEdit();  // the merged geometry of the world (buildings): for now the Umbra switch
    InstallFoliage();  // trees of the foliage: measurement natives
    InstallQuestLive();  // quest blocks activated, for the editor's graph (off until asked for)
    InstallPoseHook();  // per-frame pose combine hook for live bone control (off unless config posehook=diag|on)
    InstallHotReload();  // `reloadscripts force`: recompile WitcherScript live
    InstallUi();  // the in-game console and inspector (installs itself at the first press of console_key)
    InstallPad();  // the controller: its buttons as keys, the menu by both sticks, the game's pad resting in the menu
    InstallSound();  // own sounds through XAudio2 (starts with the first sound)
    InstallWatchdog();  // a report in tw3se\out\hang_*.txt when the main thread stands
    StartConsole();
    LoadPlugins();
    LoadWnbCompat();  // before the .asi mods: those made for Witcher Native Bridge find its functions in TW3SE
    CheckAsiLoaders();
    Log("ready");
}

// TW3SE starts once, by whichever way comes first: the exe's entry point (hooked by DllMain) or tw3se.asi under
// another mod's ASI loader (TW3SE_StartFromAsi: a mod that brings the Ultimate ASI Loader as dinput8.dll
// replaces TW3SE's loader; the loader then starts TW3SE through tw3se.asi). Started that way, that loader loads the
// .asi mods.
std::atomic<bool> g_started{false};
std::atomic<bool> g_viaAsi{false};

int WINAPI EntryHook(void* peb) {
    if (!g_started.exchange(true)) {
        StartUp();
        if (!g_viaAsi) LoadAsiPlugins();  // also when TW3SE stays passive: those mods have nothing to do with it
        else Log("asi: TW3SE came through tw3se.asi - the ASI loader that loaded it loads the .asi mods");
    }
    return g_entry(peb);
}

bool LoaderLockHeld() {
    auto peb = reinterpret_cast<const uint8_t*>(__readgsqword(0x60));
    auto lock = *reinterpret_cast<RTL_CRITICAL_SECTION* const*>(peb + 0x110);
    return lock && reinterpret_cast<DWORD_PTR>(lock->OwningThread) == GetCurrentThreadId();
}

}  // namespace

const std::wstring& Dir() { return g_dir; }
bool SafeMode() { return g_safe; }
bool NoPlugins() { return g_noPlugins; }
bool NoLuaMods() { return g_noLua; }
uintptr_t ImageBase() { return g_base; }
uint32_t GameBuild() { return g_build; }

namespace {
struct HookRec {
    void* target;
    void* detour;
};
std::mutex g_hookMx;
std::vector<HookRec> g_hookList;  // every hook, so an unloaded plugin's can be taken off
}  // namespace

bool Hook(void* target, void* detour, void** original) {
    bool ok = MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
    if (ok) {
        std::lock_guard<std::mutex> l(g_hookMx);
        g_hookList.push_back({target, detour});
    }
    return ok;
}

// A plugin is unloaded: the hooks whose detour lies in [lo, hi) come off, the originals run again.
int UnhookIn(uintptr_t lo, uintptr_t hi) {
    std::lock_guard<std::mutex> l(g_hookMx);
    int n = 0;
    for (auto it = g_hookList.begin(); it != g_hookList.end();) {
        uintptr_t d = reinterpret_cast<uintptr_t>(it->detour);
        if (d >= lo && d < hi) {
            MH_DisableHook(it->target);
            MH_RemoveHook(it->target);
            it = g_hookList.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

void HookNamed(const char* name, void* detour, void** orig) {
    void* target = reinterpret_cast<void*>(Address(name));
    if (!target || !Hook(target, detour, orig)) Log("hook: %s not hooked", name);
}

}  // namespace tw3se

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved) {
    using namespace tw3se;
    if (reason == DLL_PROCESS_DETACH && reserved) FlushVars();  // the game exits: the store's last changes
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_self = self;
    DisableThreadLibraryCalls(self);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/"));
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        g_base + reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base)->e_lfanew);
    g_build = nt->FileHeader.TimeDateStamp;
    void* entry = reinterpret_cast<void*>(g_base + nt->OptionalHeader.AddressOfEntryPoint);
    if (MH_Initialize() != MH_OK || !Hook(entry, reinterpret_cast<void*>(&EntryHook), reinterpret_cast<void**>(&g_entry)))
        return TRUE;  // stays passive; the game runs as without TW3SE
    WnbCompatEarly();  // before any ASI loader loads a mod made for Witcher Native Bridge
    return TRUE;
}

// The core's own exports, for tools and for loaders other than dinput8.dll.
extern "C" __declspec(dllexport) const TW3SE_Interface* TW3SE_GetInterface() { return tw3se::Interface(); }

// tw3se.asi calls this from InitializeASI. Under the loader lock the game has not reached its entry point yet: the
// entry hook starts TW3SE then. Without it the entry point is past (the ASI loader runs from there): start now.
extern "C" __declspec(dllexport) int TW3SE_StartFromAsi() {
    using namespace tw3se;
    g_viaAsi = true;
    if (g_started) return 1;
    if (LoaderLockHeld()) return 1;
    if (!g_started.exchange(true)) {
        StartUp();
        Log("asi: TW3SE started through tw3se.asi - the ASI loader that loaded it loads the .asi mods");
    }
    return 1;
}
