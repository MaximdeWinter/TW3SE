// TW3SE core - game lifecycle events, for anything that has to re-apply itself when a game starts or a save loads
// (world edits, persistent mod state). TW3SE hooks the two engine entry points that
// mean "the world is now coming up" - CCommonGame::OnGameStart (new game / world start) and
// CGameSessionManager::RestoreSession (a save being loaded) - and after the engine's own work runs we fire an
// "on game loaded" event to registered C++ plugin callbacks. (It fires to C++ only: see FireGameLoaded for why
// a WitcherScript function can't be driven from here.)
#include "core.h"
#include <atomic>
#include <vector>

namespace tw3se {
namespace {

struct LoadedCb {
    TW3SE_GameLoadedCallback cb;
    void* context;
    bool dead;
};
std::vector<LoadedCb> g_loadedCbs;

struct RebuildCb {
    TW3SE_RenderRebuildCallback cb;
    void* context;
    bool dead;
};
std::vector<RebuildCb> g_rebuildCbs;

// Both hooks are engine methods; a 4-argument pass-through matches them (same shape the DLC-manager hooks use
// on this engine). We only need to run after the original and forward its result.
using GameFn_t = void* (*)(void*, void*, void*, void*);
GameFn_t oOnGameStart = nullptr;
GameFn_t oRestoreSession = nullptr;
std::atomic<int> g_starting{0};  // inside OnGameStart or RestoreSession: a save loading, the world being built

bool FireOneGuarded(TW3SE_GameLoadedCallback cb, void* ctx, DWORD* code) {
    __try {
        cb(ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

// There is no "call a WitcherScript function on load" here: CallGlobalFunc dispatches only
// native functions (opcode -> native table); a user `exec function` is script bytecode (opcode 0) and would be
// found but never actually run. A WS-side applier therefore can't hang off a C++ call; persistent world edits
// are re-applied from C++ instead (see the entity-attach path). This event is for C++ plugins.
void FireGameLoaded(const char* which) {
    Log("lifecycle: game loaded (%s) - firing %u callback(s)", which, static_cast<unsigned>(g_loadedCbs.size()));
    DecalsGameLoaded();
    LuaGameLoaded();
    size_t n = g_loadedCbs.size();
    for (size_t i = 0; i < n; ++i) {
        if (g_loadedCbs[i].dead) continue;
        DWORD code = 0;
        if (!FireOneGuarded(g_loadedCbs[i].cb, g_loadedCbs[i].context, &code)) {
            g_loadedCbs[i].dead = true;
            Log("lifecycle: OnGameLoaded callback %p dropped: exception 0x%08x", g_loadedCbs[i].cb, code);
        }
    }
}

bool FireRebuildGuarded(TW3SE_RenderRebuildCallback cb, int phase, void* ctx, DWORD* code) {
    __try {
        cb(phase, ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

void* OnGameStartHook(void* self, void* a2, void* a3, void* a4) {
    ++g_starting;
    void* r = oOnGameStart(self, a2, a3, a4);
    --g_starting;
    FireGameLoaded("OnGameStart");
    return r;
}
void* RestoreSessionHook(void* self, void* a2, void* a3, void* a4) {
    ++g_starting;
    void* r = oRestoreSession(self, a2, a3, a4);
    --g_starting;
    FireGameLoaded("RestoreSession");
    return r;
}

}  // namespace

// The watchdog (any thread): the engine is starting a game or loading a save right now.
bool GameStarting() { return g_starting.load(std::memory_order_relaxed) > 0; }

// Register a callback fired after the game starts or a save loads (interface v11). C++: tw3se::on_game_loaded.
void RegisterOnGameLoaded(TW3SE_GameLoadedCallback cb, void* context) {
    if (cb) g_loadedCbs.push_back({cb, context, false});
}

// A plugin is unloaded: its game-loaded callbacks stop (their code lies in [lo, hi)).
int DropGameLoadedIn(uintptr_t lo, uintptr_t hi) {
    int n = 0;
    for (LoadedCb& c : g_loadedCbs) {
        uintptr_t f = reinterpret_cast<uintptr_t>(c.cb);
        if (!c.dead && f >= lo && f < hi) c.dead = true, ++n;
    }
    return n;
}

// Before (phase 0) and after (phase 1) the game rebuilds its render resources: the options' SetVarValue of a graphics
// group (the hook is TW3SE's Lua library, lua_lib.inc), main thread. Plugins let go of the game's render data before
// and take it again after (interface v25).
void FireRenderRebuild(int phase) {
    if (!phase) QuarantineRebuild(0);  // quarantine = rebuild: the frees of the rebuild are held back
    size_t n = g_rebuildCbs.size(), live = 0;
    for (size_t i = 0; i < n; ++i) {
        if (g_rebuildCbs[i].dead) continue;
        ++live;
        DWORD code = 0;
        if (!FireRebuildGuarded(g_rebuildCbs[i].cb, phase, g_rebuildCbs[i].context, &code)) {
            g_rebuildCbs[i].dead = true;
            Log("lifecycle: OnRenderRebuild callback %p dropped: exception 0x%08x", g_rebuildCbs[i].cb, code);
        }
    }
    if (live) Log("lifecycle: render rebuild, %s - %u plugin callback(s)", phase ? "after" : "before",
                  static_cast<unsigned>(live));
    if (phase) QuarantineRebuild(1);
}

int RegisterOnRenderRebuild(TW3SE_RenderRebuildCallback cb, void* context) {
    if (!cb) return 0;
    g_rebuildCbs.push_back({cb, context, false});
    return 1;
}

int DropRenderRebuildIn(uintptr_t lo, uintptr_t hi) {
    int n = 0;
    for (RebuildCb& c : g_rebuildCbs) {
        uintptr_t f = reinterpret_cast<uintptr_t>(c.cb);
        if (!c.dead && f >= lo && f < hi) c.dead = true, ++n;
    }
    return n;
}

void InstallLifecycle() {
    HookNamed("CCommonGame::OnGameStart", reinterpret_cast<void*>(&OnGameStartHook),
              reinterpret_cast<void**>(&oOnGameStart));
    HookNamed("CGameSessionManager::RestoreSession", reinterpret_cast<void*>(&RestoreSessionHook),
              reinterpret_cast<void**>(&oRestoreSession));
    Log("lifecycle: installed (OnGameStart=%s RestoreSession=%s)", oOnGameStart ? "ok" : "no",
        oRestoreSession ? "ok" : "no");
}

}  // namespace tw3se
