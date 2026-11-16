// TW3SE core - the game window gets the keyboard at the start.
//
// The game (5.0) shows a small loading window first and makes its real window (W2ViewportClass) seconds later.
// Windows' foreground lock refuses that late window the focus, so keyboard and controller go to the window the player
// used before (Steam, a browser, the desktop) until the player clicks into the game. Fix mods do this from outside
// (W3 Launch Focus Fix, Nexus 14330, an AutoHotkey script) or through a native (Window Focus Fix, Nexus 14784).
// In exclusive fullscreen the game even ends minimized after it compiled the scripts (Keep It Up, Nexus 14807;
// measured: minimized without this, in front with it).
//
// TW3SE notes the window in front when it starts. At the first ticks the game window comes to the front if the
// player has not touched keyboard or mouse since then (a launcher that closed handed the focus on), or if the window
// in front is still that one, one of the game's own, the desktop or the taskbar. A window the player switched to
// while the game loaded keeps the focus. Tried for the first 10 seconds of ticks, then never again.
// `focus_at_start = off` in tw3se.config or the switch in F2 > Mods > TW3SE turns it off.
#include "core.h"

#include <string>

namespace tw3se {
namespace {

HWND g_fgAtStart = nullptr;
DWORD g_inputAtStart = 0;  // GetLastInputInfo's time when TW3SE started
DWORD g_tickStart = 0;
bool g_done = false;

struct Biggest {
    HWND best;
    LONG area;
};
BOOL CALLBACK FindGameWnd(HWND h, LPARAM p) {
    auto c = reinterpret_cast<Biggest*>(p);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    char cls[64] = {};
    GetClassNameA(h, cls, sizeof cls);
    if (strcmp(cls, "W2ViewportClass") != 0) return TRUE;
    RECT r;
    if (!GetClientRect(h, &r)) return TRUE;
    LONG a = (r.right - r.left) * (r.bottom - r.top);
    if (a > c->area) {
        c->area = a;
        c->best = h;
    }
    return TRUE;
}

std::string Describe(HWND h) {
    if (!h) return "no window";
    char cls[64] = {};
    GetClassNameA(h, cls, sizeof cls);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    std::string exe = "?";
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        char path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameA(p, 0, path, &n)) {
            const char* b = strrchr(path, '\\');
            exe = b ? b + 1 : path;
        }
        CloseHandle(p);
    }
    return exe + " (" + cls + ")";
}

DWORD LastInput() {
    LASTINPUTINFO li = {sizeof li};
    return GetLastInputInfo(&li) ? li.dwTime : 0;
}

// The window in front may give the focus to the game: no input since the start, or the window in front is the one in
// front at the start, one of the game's own, the desktop or the taskbar
bool MayTake(HWND fg) {
    if (!fg || fg == g_fgAtStart || LastInput() == g_inputAtStart) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return true;
    char cls[64] = {};
    GetClassNameA(fg, cls, sizeof cls);
    return !strcmp(cls, "Progman") || !strcmp(cls, "WorkerW") || !strcmp(cls, "Shell_TrayWnd");
}

void Front(HWND game, HWND fg) {
    // the thread of the window in front lends its input state for the call (the foreground lock refuses it otherwise)
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool attached = fgThread && fgThread != me && AttachThreadInput(fgThread, me, TRUE);
    if (IsIconic(game)) ShowWindow(game, SW_RESTORE);
    SetForegroundWindow(game);
    BringWindowToTop(game);
    SetFocus(game);
    if (attached) AttachThreadInput(fgThread, me, FALSE);
}

void OnTick(float, void*) {
    if (g_done) return;
    if (!g_tickStart) g_tickStart = GetTickCount();
    if (GetTickCount() - g_tickStart > 10000) {
        g_done = true;
        Log("focus: the game window did not get the focus at the start (%s in front)",
            Describe(GetForegroundWindow()).c_str());
        return;
    }
    static DWORD last = 0;  // a look twice a second
    if (GetTickCount() - last < 500) return;
    last = GetTickCount();
    Biggest b = {nullptr, 0};
    EnumWindows(&FindGameWnd, reinterpret_cast<LPARAM>(&b));
    if (!b.best) return;
    HWND fg = GetForegroundWindow();
    if (fg == b.best) {
        g_done = true;
        return;
    }
    if (!MayTake(fg)) {
        g_done = true;
        Log("focus: %s keeps the focus (switched to while the game started)", Describe(fg).c_str());
        return;
    }
    std::string was = Describe(fg);
    Front(b.best, fg);
    if (GetForegroundWindow() == b.best) {
        g_done = true;
        Log("focus: the game window to the front (%s had it)", was.c_str());
    }
}

}  // namespace

void FocusStart() {
    std::string menu;  // F2 > Mods > TW3SE > Game window in front at the start
    if (!ConfigBool("focus_at_start", true) || (VarGet("mod/TW3SE/focus_at_start", menu) && menu == "false")) return;
    g_fgAtStart = GetForegroundWindow();
    g_inputAtStart = LastInput();
    RegisterTick(&OnTick, nullptr);
}

}  // namespace tw3se
