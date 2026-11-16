// TW3SE core - work for the game's main thread.
//
// Most engine systems may only be touched from the main thread ("DLC Manager can only be called from main thread").
// Tasks queued from anywhere run at the end of CCommonGame::Tick (once per frame, also in the main menu).
#include "core.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace tw3se {
namespace {

using Tick_t = void (*)(void* game, float timeDelta);  // timeDelta arrives in xmm1
Tick_t g_tick = nullptr;
DWORD g_mainThread = 0;
std::atomic<ULONGLONG> g_lastTick{0};  // GetTickCount64 of the last CCommonGame::Tick

struct Item {
    TW3SE_Task task;
    void* context;
};
std::mutex g_mx;
std::deque<Item> g_queue;

// SEH must live in a function without C++ objects that need unwinding (C2712).
bool RunGuarded(TW3SE_Task task, void* context, DWORD* code) {
    __try {
        task(context);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

// Per-tick callbacks (plugin OnTick). Registered on the main thread during plugin load (or later); fired in
// TickHook. A callback that faults is marked dead and skipped from then on, so one bad plugin cannot crash or
// spam every frame.
struct TickCb {
    TW3SE_TickCallback cb;
    void* context;
    bool dead;
};
std::vector<TickCb> g_ticks;

bool RunTickGuarded(TW3SE_TickCallback cb, float dt, void* context, DWORD* code) {
    __try {
        cb(dt, context);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

void TickHook(void* game, float timeDelta) {
    g_mainThread = GetCurrentThreadId();
    g_lastTick.store(GetTickCount64(), std::memory_order_relaxed);
    Crumb("tick: engine");
    {
        DoingScope d("the engine's tick (TW3SE waits for it)");
        g_tick(game, timeDelta);
    }
    Crumb("tick: TW3SE");
    DoingScope doing("TW3SE's part of the tick");
    TickContent();  // deferred post-mount DLC scan, if one is pending
    TickVars();     // the variable store to disk, when it changed
    PadTick();      // the controller's combination for the TW3SE Menu
    std::deque<Item> items;
    {
        std::lock_guard<std::mutex> l(g_mx);
        items.swap(g_queue);
    }
    for (const Item& it : items) {
        Crumb("main thread task", reinterpret_cast<uintptr_t>(it.task));
        DWORD code = 0;
        if (!RunGuarded(it.task, it.context, &code)) Log("main thread task %p failed: exception 0x%08x", it.task, code);
    }
    // Index-based with the length fixed up front: a callback that registers another tick (push_back may
    // reallocate) stays safe, and the new one simply runs from the next tick on.
    size_t n = g_ticks.size();
    for (size_t i = 0; i < n; ++i) {
        if (g_ticks[i].dead) continue;
        Crumb("tick callback", reinterpret_cast<uintptr_t>(g_ticks[i].cb));
        DoingScope d("a tick callback", reinterpret_cast<uintptr_t>(g_ticks[i].cb));
        DWORD code = 0;
        if (!RunTickGuarded(g_ticks[i].cb, timeDelta, g_ticks[i].context, &code)) {
            g_ticks[i].dead = true;
            Log("tick callback %p dropped: exception 0x%08x", g_ticks[i].cb, code);
        }
    }
}

struct Waiter {
    TW3SE_Task task;
    void* context;
    std::mutex mx;
    std::condition_variable cv;
    bool done = false;
};

void RunWaiter(void* p) {
    auto w = static_cast<Waiter*>(p);
    DWORD code = 0;
    if (!RunGuarded(w->task, w->context, &code)) Log("main thread task %p failed: exception 0x%08x", w->task, code);
    std::lock_guard<std::mutex> l(w->mx);
    w->done = true;
    w->cv.notify_all();
}

}  // namespace

void* TickHookAddress() { return reinterpret_cast<void*>(&TickHook); }  // the quarantine's filter

// The REDkit editor has no game tick while nobody plays. Its engine tick (CEditorEngine, same shape: object,
// frame time) runs every frame in edit mode and during Play, so the main-thread work runs in both.
bool InstallMainThread() {
    void* tick = reinterpret_cast<void*>(Address(IsRedkitEditor() ? "EditorEngineTick_ed" : "CCommonGame::Tick"));
    return tick && Hook(tick, reinterpret_cast<void*>(&TickHook), reinterpret_cast<void**>(&g_tick));
}

// False while the tick does not run (loading, or the hook could not be installed).
bool MainThreadTicking() {
    return g_tick && GetTickCount64() - g_lastTick.load(std::memory_order_relaxed) < 2000;
}

DWORD MainThreadId() { return g_mainThread; }
uint64_t LastTickMs() { return g_lastTick.load(std::memory_order_relaxed); }
void NoteMainThreadAlive() { g_lastTick.store(GetTickCount64(), std::memory_order_relaxed); }

bool OnMainThread() { return g_mainThread && GetCurrentThreadId() == g_mainThread; }

void RunOnMainThread(TW3SE_Task task, void* context) {
    if (!task) return;
    std::lock_guard<std::mutex> l(g_mx);
    g_queue.push_back({task, context});
}

// Registered on the main thread (plugin load runs there). Fired in TickHook on the same thread, so no lock.
// A plugin is unloaded: its tick callbacks stop and its queued tasks are dropped (their code lies in [lo, hi)).
int DropTicksIn(uintptr_t lo, uintptr_t hi) {
    int n = 0;
    for (TickCb& t : g_ticks) {
        uintptr_t f = reinterpret_cast<uintptr_t>(t.cb);
        if (!t.dead && f >= lo && f < hi) {
            t.dead = true;
            ++n;
        }
    }
    std::lock_guard<std::mutex> l(g_mx);
    for (auto it = g_queue.begin(); it != g_queue.end();) {
        uintptr_t f = reinterpret_cast<uintptr_t>(it->task);
        if (f >= lo && f < hi) {
            it = g_queue.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

void RegisterTick(TW3SE_TickCallback callback, void* context) {
    if (callback) g_ticks.push_back({callback, context, false});
}

bool RunAndWait(TW3SE_Task task, void* context, uint32_t timeoutMs) {
    if (OnMainThread()) {
        task(context);
        return true;
    }
    // The waiter must outlive a task that is already running when we give up: leaked on timeout on purpose.
    auto w = new Waiter{task, context};
    RunOnMainThread(&RunWaiter, w);
    std::unique_lock<std::mutex> l(w->mx);
    if (!w->cv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return w->done; })) return false;
    l.unlock();
    delete w;
    return true;
}

}  // namespace tw3se
