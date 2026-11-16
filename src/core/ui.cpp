// TW3SE core - the in-game UI: Dear ImGui over the game. One window with tabs: Mods (every mod with
// its settings, lua\lib\modmanager.lua), a Lua console, the tabs mods add; an inspector (lua\lib\inspector.lua) and
// windows for Lua mods (w3.ui.*, lua.cpp).
//
// Three threads take part:
//   main thread   (the game tick): the console key, ImGui's frame (NewFrame .. Render), the console, every Lua
//                 window. Lua runs only here. The finished draw data is copied and handed to the render thread.
//   window thread (the game window's WndProc, subclassed): while the UI is open, keyboard and mouse messages are
//                 queued for ImGui and kept from the game (that alone stops the game's input, tested).
//   render thread (Present, overlay.cpp): ImGui's DX12 backend draws the latest copy onto the back buffer, in the
//                 overlay's own command list on the presentation queue (works with DLSS Frame Generation).
// Nothing is installed until the console key is pressed the first time (console = off in tw3se.config: never).
#include "core.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "lua_ui.h"
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

void TW3SE_ImAssert(const char* expr, const char* file, int line) {
    static int logged = 0;
    if (logged < 200) {  // enough that a test (tools/fuzz_ui.py) sees the asserts it caused
        ++logged;
        const char* f = strrchr(file, '\\');
        tw3se::Log("ui: ImGui check failed: %s (%s:%d)", expr, f ? f + 1 : file, line);
    }
}

namespace tw3se {
std::string LuaRun(const char* code);      // lua.cpp: a Lua chunk, its output (or "error: ...")
void LuaRunUi();                           // lua.cpp: the w3.on_ui functions, inside the frame
void UiFeedApply();                        // lua_ui_draw.cpp: w3.ui.feed's step for this frame
void LuaUiLib(const char* fn);             // lua.cpp: lib.modmanager's draw/tabs, inside the main window
// ui_gpu.cpp: pictures and Qt's text on D3D12
ImDrawList* UiCloneDrawList(const ImDrawList* l);
void UiGpuPrepare(ID3D12Device* dev, ID3D12DescriptorHeap* heap, ID3D12GraphicsCommandList* cl, DXGI_FORMAT fmt, int gen,
                  ImDrawData* dd);
void UiGpuShutdown();
std::string RunCommandLine(const std::string& line);  // console.cpp: a console command, on this thread

namespace {

// --- state shared between the threads ---
std::atomic<bool> g_visible{false};       // the UI is open (input goes to it)
std::atomic<bool> g_overlay{false};       // w3.ui.overlay: Lua windows over the game while the menu is closed
std::atomic<bool> g_pluginOverlay{false}; // a plugin's or .asi mod's windows over the game (ImGuiOverlay, 1.2)
std::atomic<bool> g_capture{false};       // a plugin's own menu is open: all input goes to ImGui (ImGuiCaptureInput)
bool OverlayAny() { return g_overlay.load() || g_pluginOverlay.load(); }
std::atomic<bool> g_contextReady{false};  // ImGui context and font atlas exist (main thread made them)
// TW3SE's menu takes the input only once it is on screen: an overlay that never drew (an AMD card) left the
// game without keyboard and controller while the menu waited to come up
std::atomic<bool> g_menuDrawn{false};     // a frame of the open menu was built since it opened
std::atomic<unsigned long long> g_openedAt{0};  // when the menu opened (GetTickCount64)
bool MenuHolds() { return g_visible.load() && g_menuDrawn.load(); }
// a plugin's menu takes the input only while the UI can draw (with overlay = off it would take keys and mouse
// unseen); TW3SE's own menu does not open then (EnsureInstalled)
bool FullInput() { return MenuHolds() || (g_capture.load() && g_contextReady); }
bool Showing() { return g_visible.load() || FullInput() || OverlayAny(); }
std::atomic<bool> g_cursorFree{false};    // w3.ui.cursor_free: the game may not move or clip the cursor
std::atomic<bool> g_cursorOwn{false};     // w3.ui.cursor_own: the Windows cursor everywhere, never the game's
std::atomic<bool> g_wantMouse{false};     // last frame: the mouse is over a UI window (or drags in it)
std::atomic<bool> g_wantText{false};      // last frame: a text field has the keyboard
std::atomic<int> g_cursorShape{0};        // last frame: the cursor the UI asked for (ImGuiMouseCursor_*)
std::atomic<bool> g_rendererReady{false}; // the DX12 backend is up and the font texture uploaded (render thread)
std::atomic<int> g_rendererGen{0};        // bumped at each backend start: a copy from an older one is dropped
HWND g_hwnd = nullptr;
WNDPROC g_origWndProc = nullptr;
int g_consoleKey = VK_F2;
int g_consoleMods = 0;              // Ctrl 1, Shift 2, Alt 4 (as ParseKeyName gives them)
std::string g_consoleKeyName = "F2";
// a key field waits for a key: the menu key does not toggle. The field says so each frame it waits; when it is no
// longer drawn (another mod chosen, another tab) the wait ends by itself (and the menu key works again)
std::atomic<ULONGLONG> g_captureUntil{0};
std::atomic<bool> g_waitRelease{false};  // a new menu key counts once it was let go (it is down when it is set)
std::mutex g_keyMx;
extern int g_frames;

struct Msg {
    UINT msg;
    WPARAM w;
    LPARAM l;
};
std::mutex g_msgMx;
std::vector<Msg> g_msgs;  // window thread -> main thread

struct Snapshot {  // a copy of one frame's draw data, owned by whoever holds the pointer
    ImDrawData data;
    int gen = 0;
    ~Snapshot() {
        for (ImDrawList* l : data.CmdLists) IM_DELETE(l);
    }
};
std::mutex g_snapMx;
Snapshot* g_pending = nullptr;  // newest frame from the main thread
bool g_clear = false;           // the UI closed: the render thread drops what it shows
Snapshot* g_current = nullptr;  // render thread only

// --- console state (main thread) ---
struct Line {
    std::string text;
    ImU32 colour;
};
std::deque<Line> g_lines;
std::vector<std::string> g_history;
int g_historyPos = -1;
char g_input[1024] = {};
bool g_scrollToEnd = false;
bool g_focusInput = true;
std::vector<std::string> g_queued;  // console lines to run after the frame (a reload or Lua run outside ImGui's frame)
bool g_inspector = false;
bool g_profiler = false;
ImFont* g_mono = nullptr;
float g_scale = 1.0f;  // window height / 1080, at least 1

constexpr ImU32 kColText = IM_COL32(230, 230, 230, 255);
constexpr ImU32 kColEcho = IM_COL32(140, 190, 255, 255);
constexpr ImU32 kColError = IM_COL32(255, 120, 110, 255);

void AddLines(const std::string& text, ImU32 colour) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        g_lines.push_back({text.substr(start, end - start), colour});
        start = end + 1;
    }
    while (g_lines.size() > 2000) g_lines.pop_front();
    g_scrollToEnd = true;
}

// --- input: the game window ---
bool IsInputMsg(UINT m) {
    return (m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_INPUT;
}

bool IsMouseMsg(UINT m) { return m >= WM_MOUSEFIRST && m <= WM_MOUSELAST; }

// the wheel for w3.input.wheel: WM_MOUSEWHEEL, or the raw mouse's wheel while the game sends no legacy messages
std::atomic<int> g_wheelDelta{0};
std::atomic<bool> g_legacyWheel{false};
void CountWheel(UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEWHEEL) {
        g_legacyWheel = true;
        g_wheelDelta += GET_WHEEL_DELTA_WPARAM(w);
    } else if (m == WM_INPUT && !g_legacyWheel.load()) {
        RAWINPUT ri = {};
        UINT sz = sizeof ri;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
            ri.header.dwType == RIM_TYPEMOUSE && (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL))
            g_wheelDelta += static_cast<short>(ri.data.mouse.usButtonData);
    }
}

// the Windows cursor of a shape the UI asked for (the arrow too: over a UI window the Windows cursor, the game's own
// only over the game); nullptr: none asked, the game's own stays
LPCTSTR CursorOf(int shape) {
    switch (shape) {
    case ImGuiMouseCursor_Arrow: return IDC_ARROW;
    case ImGuiMouseCursor_TextInput: return IDC_IBEAM;
    case ImGuiMouseCursor_ResizeAll: return IDC_SIZEALL;
    case ImGuiMouseCursor_ResizeNS: return IDC_SIZENS;
    case ImGuiMouseCursor_ResizeEW: return IDC_SIZEWE;
    case ImGuiMouseCursor_ResizeNESW: return IDC_SIZENESW;
    case ImGuiMouseCursor_ResizeNWSE: return IDC_SIZENWSE;
    case ImGuiMouseCursor_Hand: return IDC_HAND;
    case ImGuiMouseCursor_NotAllowed: return IDC_NO;
    default: return nullptr;
    }
}

// the cursor TW3SE wants now: the UI's shape over a UI window, the arrow elsewhere while the cursor is its own
// (w3.ui.cursor_own); nullptr: the game's
LPCTSTR WantedCursor() {
    if (FullInput() || !OverlayAny()) return nullptr;
    if (g_wantMouse.load()) {
        if (LPCTSTR idc = CursorOf(g_cursorShape.load())) return idc;
    }
    return g_cursorOwn.load() ? IDC_ARROW : nullptr;
}

using SetCursor_t = HCURSOR(WINAPI*)(HCURSOR);
SetCursor_t g_origSetCursor = nullptr;
// the game sets its own cursor without a message (a frame later the Windows one would be gone): while TW3SE wants
// one, every SetCursor of the process sets that one
HCURSOR WINAPI SetCursorHook(HCURSOR c) {
    if (c) {
        if (LPCTSTR idc = WantedCursor()) c = LoadCursor(nullptr, idc);
    }
    return g_origSetCursor(c);
}

LRESULT CALLBACK UiWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEWHEEL || m == WM_INPUT) CountWheel(m, w, l);
    // overlay windows that took the mouse and show another cursor (a border, a header, a text field): the game sets
    // its arrow again at every move, and ImGui's backend sets a shape only when it changes
    if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT) {
        if (LPCTSTR idc = WantedCursor()) {
            SetCursor(LoadCursor(nullptr, idc));
            return TRUE;
        }
    }
    if (!FullInput() && OverlayAny() && IsInputMsg(m)) {
        // overlay windows: ImGui sees everything (hover), the game keeps what the UI does not take
        bool take;
        if (m == WM_INPUT) {
            RAWINPUTHEADER hd = {};
            UINT sz = sizeof hd;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_HEADER, &hd, &sz, sizeof(RAWINPUTHEADER));
            take = hd.dwType == RIM_TYPEMOUSE ? g_wantMouse.load() : hd.dwType == RIM_TYPEKEYBOARD && g_wantText.load();
        } else {
            std::lock_guard<std::mutex> lk(g_msgMx);
            if (g_msgs.size() < 4096) g_msgs.push_back({m, w, l});
            take = IsMouseMsg(m) ? g_wantMouse.load() : g_wantText.load();
        }
        if (take) {
            if (m == WM_INPUT) DefWindowProcW(h, m, w, l);
            return 0;
        }
        return CallWindowProcW(g_origWndProc, h, m, w, l);
    }
    if (FullInput() && IsInputMsg(m)) {
        if (m != WM_INPUT) {
            std::lock_guard<std::mutex> lk(g_msgMx);
            if (g_msgs.size() < 4096) g_msgs.push_back({m, w, l});
        }
        if (m == WM_INPUT) DefWindowProcW(h, m, w, l);  // lets Windows clean up the raw input; the game sees none
        return 0;
    }
    if ((FullInput() || g_cursorFree.load()) && m == WM_SETCURSOR) return TRUE;  // ImGui draws the cursor
    return CallWindowProcW(g_origWndProc, h, m, w, l);
}

struct FindCtx {
    HWND best;
    LONG area;
};
BOOL CALLBACK FindWnd(HWND h, LPARAM p) {
    auto c = reinterpret_cast<FindCtx*>(p);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r;
    if (!GetClientRect(h, &r)) return TRUE;
    LONG a = (r.right - r.left) * (r.bottom - r.top);
    if (a > c->area) {
        c->area = a;
        c->best = h;
    }
    return TRUE;
}

// The game moves and clips the cursor for its camera. While the UI is open, these calls are kept from Windows.
using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
using SetCursorPos_t = BOOL(WINAPI*)(int, int);
ClipCursor_t g_origClip = nullptr;
SetCursorPos_t g_origSetPos = nullptr;
BOOL WINAPI ClipCursorHook(const RECT* r) { return FullInput() || g_cursorFree.load() ? TRUE : g_origClip(r); }
BOOL WINAPI SetCursorPosHook(int x, int y) { return MenuHolds() || g_cursorFree.load() ? TRUE : g_origSetPos(x, y); }

bool InstallInput() {
    FindCtx c = {nullptr, 0};
    EnumWindows(&FindWnd, reinterpret_cast<LPARAM>(&c));
    if (!c.best) return false;
    g_hwnd = c.best;
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&UiWndProc)));
    if (HMODULE u = GetModuleHandleW(L"user32.dll")) {
        if (void* f = reinterpret_cast<void*>(GetProcAddress(u, "ClipCursor")))
            Hook(f, reinterpret_cast<void*>(&ClipCursorHook), reinterpret_cast<void**>(&g_origClip));
        if (void* f = reinterpret_cast<void*>(GetProcAddress(u, "SetCursorPos")))
            Hook(f, reinterpret_cast<void*>(&SetCursorPosHook), reinterpret_cast<void**>(&g_origSetPos));
        if (void* f = reinterpret_cast<void*>(GetProcAddress(u, "SetCursor")))
            Hook(f, reinterpret_cast<void*>(&SetCursorHook), reinterpret_cast<void**>(&g_origSetCursor));
    }
    char cls[128] = {};
    GetClassNameA(g_hwnd, cls, sizeof cls);
    Log("ui: input from window %p (%s), window thread %lu, main thread %lu", g_hwnd, cls,
        GetWindowThreadProcessId(g_hwnd, nullptr), GetCurrentThreadId());
    return g_origWndProc != nullptr;
}

// --- ImGui context (main thread) ---
bool CreateContext() {
    ULONGLONG t0 = GetTickCount64();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // window positions are not kept (a file next to the game nobody asked for)
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;  // pad.cpp: the game's rests
    io.MouseDrawCursor = true;
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableDebugLog = true;
    io.ConfigErrorRecoveryEnableTooltip = true;
    RECT r = {0, 0, 1920, 1080};
    GetClientRect(g_hwnd, &r);
    float scale = static_cast<float>(r.bottom - r.top) / 1080.0f;
    if (scale < 1.0f) scale = 1.0f;
    g_scale = scale;
    float px = 18.0f * scale;
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring fonts = std::wstring(win) + L"\\Fonts\\";
    auto narrow = [](const std::wstring& s) { return std::string(s.begin(), s.end()); };
    std::string ui = narrow(fonts + L"segoeui.ttf"), mono = narrow(fonts + L"consola.ttf");
    // Latin with its accents, Cyrillic (Russian, Ukrainian, Belarusian, Serbian, Bulgarian), the general punctuation (ImGui's default is 0x20-0xFF)
    static const ImWchar text[] = {0x0020, 0x017F, 0x0400, 0x045F, 0x0490, 0x0491, 0x2000, 0x206F, 0};
    ImFont* main = GetFileAttributesA(ui.c_str()) != INVALID_FILE_ATTRIBUTES
                       ? io.Fonts->AddFontFromFileTTF(ui.c_str(), px, nullptr, text)
                       : nullptr;
    if (!main) io.Fonts->AddFontDefault();
    // w3.ui.icon's icons from Windows' icon font in the main font (the mod list's pin and unpin among them)
    for (const wchar_t* name : {L"segmdl2.ttf", L"SegoeIcons.ttf"}) {
        std::string icons = narrow(fonts + name);
        if (!main || GetFileAttributesA(icons.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        static std::vector<ImWchar> ranges;  // ImGui keeps the pointer until the atlas is built
        int n = 0;
        const UiIconName* list = UiIcons(&n);
        ranges.clear();
        for (int i = 0; i < n; ++i) ranges.push_back(list[i].code), ranges.push_back(list[i].code);
        ranges.push_back(0);
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.GlyphOffset.y = 2.0f * scale;
        io.Fonts->AddFontFromFileTTF(icons.c_str(), px * 0.85f, &merge, ranges.data());
        break;
    }
    g_mono = GetFileAttributesA(mono.c_str()) != INVALID_FILE_ATTRIBUTES
                 ? io.Fonts->AddFontFromFileTTF(mono.c_str(), px, nullptr, text)
                 : nullptr;
    int extra = UiAddFonts();  // Segoe UI at exact pixel sizes for w3.ui.font (lua_ui_draw.cpp)
    UiKeyRepeatFromWindows();
    g_uiLog = &Log;
    UiMakeTextFontsLater();  // Qt's text (lua_ui_text.cpp): seconds of rasterising, on a thread of its own
    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);  // built here, so the render thread only uploads it
    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 6.0f;
    st.FrameRounding = 4.0f;
    st.Colors[ImGuiCol_WindowBg].w = 0.92f;
    st.ScaleAllSizes(scale);
    if (!ImGui_ImplWin32_Init(g_hwnd)) return false;
    Log("ui: ImGui %s ready in %llu ms (font %.0f px, %d sized fonts, atlas %dx%d; Qt's text fonts follow)", IMGUI_VERSION,
        GetTickCount64() - t0, px, extra, w, h);
    return true;
}

// The present hook, the input hook and ImGui, made once (the menu or an overlay window needs them).
bool EnsureInstalled() {
    static bool tried = false, ok = false;
    if (!tried) {
        tried = true;
        // the present hook draws the UI; without it (overlay = off) a menu would take keys and mouse unseen
        if (!InstallOverlay(true)) Log("ui: the TW3SE Menu needs the overlay (overlay = off in tw3se.config) - it stays closed");
        else ok = InstallInput() && CreateContext();
        if (!ok) Log("ui: not available (no overlay, no game window or ImGui start failed)");
        g_contextReady = ok;
        if (ok)
            AddLines("TW3SE console. Lua: return w3.player():GetMoney()   Console command: /help   Close: the console key",
                     kColEcho);    }
    return ok;
}

bool g_uiOff = false;  // console = off: no in-game UI, also not by the controller's combination (pad.cpp)

void Open() {
    if (g_uiOff || !EnsureInstalled()) return;
    g_menuDrawn = false;
    g_openedAt = GetTickCount64();
    g_visible = true;
    g_focusInput = true;
    if (g_origClip) g_origClip(nullptr);  // let the cursor move freely
    Log("ui: open");
}

void Close() {
    g_visible = false;
    g_captureUntil = 0;  // a key field that waited stops (else the menu key could not open the menu again)
    if (!OverlayAny() && !g_capture) {
        std::lock_guard<std::mutex> lk(g_snapMx);
        delete g_pending;
        g_pending = nullptr;
        g_clear = true;
    }
    {
        std::lock_guard<std::mutex> lk(g_msgMx);
        g_msgs.clear();
    }
    Log("ui: closed");
}

// --- the console window ---
int HistoryCallback(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag != ImGuiInputTextFlags_CallbackHistory || g_history.empty()) return 0;
    int prev = g_historyPos;
    if (d->EventKey == ImGuiKey_UpArrow)
        g_historyPos = g_historyPos < 0 ? static_cast<int>(g_history.size()) - 1 : (g_historyPos > 0 ? g_historyPos - 1 : 0);
    else if (d->EventKey == ImGuiKey_DownArrow && g_historyPos >= 0)
        g_historyPos = g_historyPos + 1 < static_cast<int>(g_history.size()) ? g_historyPos + 1 : -1;
    if (prev != g_historyPos) {
        d->DeleteChars(0, d->BufTextLen);
        if (g_historyPos >= 0) d->InsertChars(0, g_history[g_historyPos].c_str());
    }
    return 0;
}

// The console tab: the output, the input line, the window switches.
void DrawConsoleTab() {
    if (ImGui::Button("Clear")) g_lines.clear();
    ImGui::SameLine();
    if (ImGui::Checkbox("Inspector", &g_inspector))
        g_queued.push_back(std::string("\x01lib.inspector ") + (g_inspector ? "true" : "false"));
    ImGui::SameLine();
    if (ImGui::Checkbox("Profiler", &g_profiler))
        g_queued.push_back(std::string("\x01lib.profiler ") + (g_profiler ? "true" : "false"));
    ImGui::SameLine();
    if (ImGui::Button("Copy report")) g_queued.push_back("\x02report");
    ImGui::SameLine();
    ImGui::TextDisabled("%s closes the menu", UiMenuKey().c_str());
    ImGui::Separator();
    float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginChild("out", ImVec2(0, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        if (g_mono) ImGui::PushFont(g_mono);
        for (const Line& l : g_lines) {
            ImGui::PushStyleColor(ImGuiCol_Text, l.colour);
            ImGui::TextUnformatted(l.text.c_str());
            ImGui::PopStyleColor();
        }
        if (g_mono) ImGui::PopFont();
        if (g_scrollToEnd) ImGui::SetScrollHereY(1.0f);
        g_scrollToEnd = false;
    }
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1);
    if (g_focusInput) {
        ImGui::SetKeyboardFocusHere();
        g_focusInput = false;
    }
    if (ImGui::InputText("##in", g_input, sizeof g_input,
                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, &HistoryCallback)) {
        std::string line = g_input;
        g_input[0] = 0;
        if (!line.empty()) {
            g_history.push_back(line);
            if (g_history.size() > 200) g_history.erase(g_history.begin());
            g_historyPos = -1;
            g_queued.push_back(line);
        }
        g_focusInput = true;
    }
}

// The main window: Mods (every mod with its settings, lua\lib\modmanager.lua), Console, then the tabs mods add.
void DrawMain() {
    ImGui::SetNextWindowSize(ImVec2(1000 * g_scale, 600 * g_scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
    // NoBringToFrontOnFocus: the menu stays behind the windows it opens (Conflicts, the editor, the inspector ...),
    // also when F2 opens it again and its input line takes the keys
    if (!ImGui::Begin("TW3SE Menu", nullptr, ImGuiWindowFlags_NoBringToFrontOnFocus)) {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("main", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton)) {
        if (ImGui::BeginTabItem("Mods")) {
            LuaUiLib("draw");
            ImGui::EndTabItem();
        }
        static bool consoleWas = false;
        bool consoleNow = ImGui::BeginTabItem("Console");
        if (consoleNow) {
            if (!consoleWas) g_focusInput = true;  // the input line takes the keys when the tab comes up
            DrawConsoleTab();
            ImGui::EndTabItem();
        }
        consoleWas = consoleNow;
        LuaUiLib("tabs");  // a mod's own tab (w3.mod{tab = fn})
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// Runs what the console asked for, after the frame (a reload or a Lua chunk in the middle of ImGui's frame could
// leave it half built).
void RunQueued() {
    std::vector<std::string> q;
    q.swap(g_queued);
    for (const std::string& line : q) {
        if (line[0] == '\x02') {  // internal: the "Copy report" button
            bool written = false;
            std::string r = BuildReport(&written);
            ImGui::SetClipboardText(r.c_str());
            AddLines("report copied to the clipboard" + std::string(written ? ", also in tw3se\\out\\report.txt" : ""),
                     kColText);
            continue;
        }
        if (line[0] == '\x01') {  // internal: a window switch ("\x01lib.inspector true")
            size_t sp = line.find(' ');
            LuaRun(("require('" + line.substr(1, sp - 1) + "').show(" + line.substr(sp + 1) + ")").c_str());
            continue;
        }
        AddLines("> " + line, kColEcho);
        std::string r = line[0] == '/' ? RunCommandLine(line.substr(1)) : LuaRun(line.c_str());
        bool err = r.rfind("error", 0) == 0;
        AddLines(r, err ? kColError : kColText);
    }
}

// --- plugins and .asi mods drawing with ImGui (plugin interface 21) ---
struct PluginDraw {
    TW3SE_ImGuiCallback cb;
    void* ctx;
};
std::vector<PluginDraw> g_pluginDraws;  // main thread only (registered through RunOnMainThread when needed)
std::mutex g_pluginDrawMx;

bool CallPluginDraw(TW3SE_ImGuiCallback cb, void* imgui, void* ctx) {
    __try {
        cb(imgui, ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RunPluginDraws() {
    std::vector<PluginDraw> list;
    {
        std::lock_guard<std::mutex> lk(g_pluginDrawMx);
        list = g_pluginDraws;
    }
    for (const PluginDraw& d : list) {
        ImGuiErrorRecoveryState st;
        ImGui::ErrorRecoveryStoreState(&st);
        bool ok = CallPluginDraw(d.cb, ImGui::GetCurrentContext(), d.ctx);
        ImGui::ErrorRecoveryTryToRecoverState(&st);  // a Begin without End, a pushed style: the next one starts clean
        if (ok) continue;
        Log("ui: a plugin's ImGui callback faulted - it is taken out");
        std::lock_guard<std::mutex> lk(g_pluginDrawMx);
        for (size_t i = 0; i < g_pluginDraws.size(); ++i)
            if (g_pluginDraws[i].cb == d.cb && g_pluginDraws[i].ctx == d.ctx) {
                g_pluginDraws.erase(g_pluginDraws.begin() + static_cast<long>(i));
                break;
            }
    }
}

// Copies the frame's draw data for the render thread.
void Publish() {
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || !dd->Valid) return;
    auto s = new Snapshot();
    s->data.Valid = true;
    s->data.DisplayPos = dd->DisplayPos;
    s->data.DisplaySize = dd->DisplaySize;
    s->data.FramebufferScale = dd->FramebufferScale;
    s->data.TotalIdxCount = dd->TotalIdxCount;
    s->data.TotalVtxCount = dd->TotalVtxCount;
    for (ImDrawList* l : dd->CmdLists) s->data.CmdLists.push_back(UiCloneDrawList(l));  // with callback data
    s->data.CmdListsCount = s->data.CmdLists.Size;
    s->gen = g_rendererGen.load();
    std::lock_guard<std::mutex> lk(g_snapMx);
    delete g_pending;
    g_pending = s;
    g_clear = false;
}

// The menu key (and the controller's combination, `ui toggle`): open, close, or - when the menu is open but has not
// been drawn yet - stay open. The overlay draws its first frame up to 10 s after the game's first tick (it waits for
// pictures of the game, which loads then); a second press in that time closed the menu before anyone saw it.
void MenuKey(const char* how) {
    if (g_visible && !g_menuDrawn) {
        Log("ui: %s pressed while the menu comes up (the overlay does not draw yet) - it stays open", how);
        return;
    }
    if (g_visible) {
        Close();
    } else {
        Open();
        if (!g_visible) Log("ui: %s pressed - the menu cannot open (see the lines before)", how);
        else if (!g_rendererReady) Log("ui: the menu opens when the overlay draws its first frame");
    }
}

void Frame() {
    {
        std::vector<Msg> msgs;
        {
            std::lock_guard<std::mutex> lk(g_msgMx);
            msgs.swap(g_msgs);
        }
        for (const Msg& m : msgs) {
            if ((m.msg == WM_KEYDOWN || m.msg == WM_KEYUP) && static_cast<int>(m.w) == g_consoleKey) continue;
            ImGui_ImplWin32_WndProcHandler(g_hwnd, m.msg, m.w, m.l);
        }
    }
    ImGui_ImplWin32_NewFrame();
    UiFeedApply();
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = FullInput() || g_cursorFree;  // over the game in play, the game's own cursor (none) stays
    ImGui::NewFrame();
    if (g_visible) {
        DrawMain();
        g_menuDrawn = true;
    }
    g_panelsBehind = g_visible.load();  // a mod's full-screen panel goes behind the open menu (lua_ui_draw.cpp)
    LuaRunUi();
    RunPluginDraws();
    ImGui::Render();
    bool lm = false;
    g_wantMouse = UiTakeLuaMouse(&lm) ? lm : io.WantCaptureMouse;   // (w3.ui.take_mouse decides when it was called)
    g_wantText = io.WantTextInput || UiTakeLuaWantText();  // ImGui's text fields and Lua's (w3.ui.want_text)
    g_cursorShape = ImGui::GetMouseCursor();   // (the window thread sets it at the next WM_SETCURSOR)
    // the cursor wanted changed without a move (a window under the mouse, the editor on): the window thread sets it
    static LPCTSTR posted = nullptr;
    LPCTSTR want = WantedCursor();
    if (want != posted && g_hwnd && GetForegroundWindow() == g_hwnd) {
        posted = want;
        if (want) PostMessageW(g_hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(g_hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    }
    Publish();
    ++g_frames;
}

void OnTick(float, void*) {
    static bool was = false;
    int vk, mods;
    {
        std::lock_guard<std::mutex> lk(g_keyMx);
        vk = g_consoleKey;
        mods = g_consoleMods;
    }
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (g_waitRelease) {
        if (!down) g_waitRelease = false;
        down = false;
    }
    int held = ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 1 : 0) | ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 2 : 0) |
               ((GetAsyncKeyState(VK_MENU) & 0x8000) ? 4 : 0);
    bool pressed = down && !was && held == mods && GetTickCount64() >= g_captureUntil.load();
    was = down;
    DWORD fgPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
    if (pressed && fgPid != GetCurrentProcessId()) {
        static int told = 0;
        if (told++ < 3) Log("ui: menu key pressed while another window has the focus - nothing happens");
    }
    if (pressed && fgPid == GetCurrentProcessId()) MenuKey("key");  // only while the game has the focus
    // a menu the overlay has not drawn in 20 s closes again; the log says where the overlay stopped
    if (g_visible && !g_menuDrawn && GetTickCount64() - g_openedAt.load() > 20000) {
        char why[256];
        OverlayWhyNoDraw(why, sizeof why);
        Log("ui: the overlay has not drawn the menu in 20 s - it closes again (%s)", why);
        Close();
    }
    if ((OverlayAny() || g_capture) && !g_contextReady) EnsureInstalled();
    if (Showing() && g_contextReady && g_rendererReady) Frame();
    RunQueued();
}

}  // namespace

// --- render thread (overlay.cpp) ---
void UiRender(ID3D12Device* dev, ID3D12CommandQueue* queue, int frames, DXGI_FORMAT fmt, ID3D12DescriptorHeap* heap,
              D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu, ID3D12GraphicsCommandList* cl) {
    if (!g_contextReady) return;
    if (!g_rendererReady) {
        ImGui_ImplDX12_InitInfo info;
        info.Device = dev;
        info.CommandQueue = queue;
        info.NumFramesInFlight = frames;
        info.RTVFormat = fmt;
        info.SrvDescriptorHeap = heap;
        info.LegacySingleSrvCpuDescriptor = cpu;
        info.LegacySingleSrvGpuDescriptor = gpu;
        if (!ImGui_ImplDX12_Init(&info) || !ImGui_ImplDX12_CreateDeviceObjects()) {
            Log("ui: DX12 backend failed to start");
            g_contextReady = false;
            return;
        }
        ++g_rendererGen;
        g_rendererReady = true;
        Log("ui: DX12 backend ready (%d frames, format %d)", frames, static_cast<int>(fmt));
        return;  // the first frame with this backend is built on the next tick
    }
    {
        std::lock_guard<std::mutex> lk(g_snapMx);
        if (g_clear) {
            delete g_current;
            g_current = nullptr;
            g_clear = false;
        }
        if (g_pending) {
            delete g_current;
            g_current = g_pending;
            g_pending = nullptr;
        }
    }
    if (!g_current || g_current->gen != g_rendererGen.load() || !Showing()) return;
    UiGpuPrepare(dev, heap, cl, fmt, g_rendererGen.load(), &g_current->data);  // pictures, the text shader
    ImGui_ImplDX12_RenderDrawData(&g_current->data, cl);
}

bool UiWantsDraw() { return g_contextReady && (Showing() || !g_rendererReady); }

// w3.ui.overlay(on): the Lua windows stay over the game while the menu is closed (lua_ui_draw.cpp)
void UiSetOverlay(bool on) {
    g_overlay = on;
    if (!Showing()) {
        std::lock_guard<std::mutex> lk(g_snapMx);
        delete g_pending;
        g_pending = nullptr;
        g_clear = true;
        g_wantMouse = g_wantText = false;
    }
}
// w3.ui.cursor_free(on): the cursor is the player's (the game may not centre or clip it), ImGui draws it
void UiSetCursorFree(bool on) {
    g_cursorFree = on;
    if (on && g_origClip) g_origClip(nullptr);
}
// w3.ui.cursor_own(on): the Windows cursor over the whole game (the editor), the game's own never shows
void UiSetCursorOwn(bool on) { g_cursorOwn = on; }
bool UiOverlayOn() { return g_overlay; }

// The overlay drops its device objects (a resize): the backend goes with them and starts again on the next frame.
void UiRenderShutdown() {
    if (!g_rendererReady) return;
    g_rendererReady = false;
    UiGpuShutdown();
    ImGui_ImplDX12_Shutdown();
    std::lock_guard<std::mutex> lk(g_snapMx);
    delete g_current;
    g_current = nullptr;
}

bool UiIsOpen() { return MenuHolds(); }
bool UiTextHasKeys() { return g_wantText.load(); }
void UiSetWindowFlag(const std::string& which, bool on) {  // a window's X (lib.inspector, lib.profiler): its box
    if (which == "inspector") g_inspector = on;
    else if (which == "profiler") g_profiler = on;
}
void UiToggle() { MenuKey("the controller's menu combination"); }  // pad.cpp
bool UiFrameRuns() { return Showing(); }

// --- w3.input (lua_input.cpp): Windows' own key and mouse state, as an app reads it next to the game ---
bool UiGameInFront() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
bool UiKeyDown(int vk) { return UiGameInFront() && (GetAsyncKeyState(vk) & 0x8000) != 0; }
bool UiCursorPos(int* x, int* y) {
    POINT p;
    if (!GetCursorPos(&p)) return false;
    *x = p.x, *y = p.y;
    return true;
}
// the cursor put somewhere (past TW3SE's own SetCursorPos guard, which keeps the game's camera from moving it)
bool UiSetCursorPos(int x, int y) { return g_origSetPos ? g_origSetPos(x, y) != 0 : SetCursorPos(x, y) != 0; }
// the game's picture on the screen: its window's client area
bool UiClientRect(int* x, int* y, int* w, int* h) {
    if (!g_hwnd) return false;
    RECT r;
    if (!GetClientRect(g_hwnd, &r)) return false;
    POINT p = {0, 0};
    ClientToScreen(g_hwnd, &p);
    *x = p.x, *y = p.y, *w = r.right - r.left, *h = r.bottom - r.top;
    return true;
}
// wheel notches since the last call (120 = one)
double UiTakeWheel() { return g_wheelDelta.exchange(0) / 120.0; }

bool UiSetMenuKey(const std::string& name) {
    int vk = 0, mods = 0;
    // a controller button is no menu key (its combination is menu_pad, pad.cpp)
    if (!ParseKeyName(name, vk, mods) || vk == VK_ESCAPE || vk == VK_LBUTTON || vk == VK_RBUTTON || vk >= 0x100 || mods > 7)
        return false;
    std::lock_guard<std::mutex> lk(g_keyMx);
    g_consoleKey = vk;
    g_consoleMods = mods;
    g_consoleKeyName = name;
    g_waitRelease = true;
    return true;
}
std::string UiMenuKey() {
    std::lock_guard<std::mutex> lk(g_keyMx);
    return g_consoleKeyName;
}
void UiCaptureKeys(bool on) { g_captureUntil = on ? GetTickCount64() + 300 : 0; }

namespace {
int g_frames = 0;  // ImGui frames built since the start (the `ui state` answer)

void CmdUi(const char* args, char* reply, uint32_t size) {
    std::string a = args ? args : "";
    if (a == "open" && !g_visible) Open();
    else if (a == "close" && g_visible) Close();
    else if (a == "toggle") MenuKey("ui toggle");
    else if (!a.empty() && a != "state" && a != "open" && a != "close") {
        strncpy_s(reply, size, "usage: ui open|close|toggle|state", _TRUNCATE);
        return;
    }
    snprintf(reply, size, "ui: %s, context %s, renderer %s (gen %d), %d frame(s), key 0x%02x",
             g_visible ? "open" : "closed", g_contextReady ? "ready" : "none", g_rendererReady ? "ready" : "not yet",
             g_rendererGen.load(), g_frames, g_consoleKey);
}
}  // namespace

void InstallUi() {
    std::string mode = ConfigGet("console", "on");
    if (mode == "off" || mode == "0" || mode == "false" || mode == "no") {
        g_uiOff = true;  // the controller's menu combination too
        Log("ui: off (config console = off)");
        return;
    }
    // the key set in the menu (TW3SE > Menu key) wins over console_key in tw3se.config; a name ("F2", "Ctrl+F2") or
    // a virtual-key code ("0x71")
    // (the store keeps the default beside the value: the same text means the player set nothing in the menu)
    std::string key, keyDefault;
    VarGet("mod/TW3SE/menu_key.default", keyDefault);
    if (!VarGet("mod/TW3SE/menu_key", key) || key.empty() || key == keyDefault) key = ConfigGet("console_key", "F2");
    if (!UiSetMenuKey(key)) {
        Log("ui: menu key '%s' is not a key name, F2 instead", key.c_str());
        UiSetMenuKey("F2");
    }
    g_waitRelease = false;
    RegisterTick(&OnTick, nullptr);
    RegisterCommand("ui", &CmdUi, "ui open|close|toggle|state: the TW3SE Menu (toggle: as its key; TW3SE > Menu key, console_key)");
    Log("ui: menu key %s (installs on the first press)", UiMenuKey().c_str());
}


// --- plugin interface 21: ImGui for plugins and .asi mods ---
void UiAddPluginDraw(TW3SE_ImGuiCallback cb, void* ctx) {
    std::lock_guard<std::mutex> lk(g_pluginDrawMx);
    g_pluginDraws.push_back({cb, ctx});
}
int UiDropPluginDrawsIn(uintptr_t lo, uintptr_t hi) {
    std::lock_guard<std::mutex> lk(g_pluginDrawMx);
    int n = 0;
    for (size_t i = g_pluginDraws.size(); i-- > 0;) {
        auto a = reinterpret_cast<uintptr_t>(g_pluginDraws[i].cb);
        if (a >= lo && a < hi) {
            g_pluginDraws.erase(g_pluginDraws.begin() + static_cast<long>(i));
            ++n;
        }
    }
    return n;
}
void UiSetPluginOverlay(bool on) {
    g_pluginOverlay = on;
    if (!Showing()) {
        std::lock_guard<std::mutex> lk(g_snapMx);
        delete g_pending;
        g_pending = nullptr;
        g_clear = true;
        g_wantMouse = g_wantText = false;
    }
}
void UiSetCapture(bool on) {
    if (on == g_capture.load()) return;
    if (on && !g_contextReady) EnsureInstalled();
    g_capture = on;
    if (on && g_origClip) g_origClip(nullptr);  // the cursor moves freely while the menu is open
    if (!on) {
        std::lock_guard<std::mutex> lk(g_msgMx);
        g_msgs.clear();
    }
    if (!Showing()) {
        std::lock_guard<std::mutex> lk(g_snapMx);
        delete g_pending;
        g_pending = nullptr;
        g_clear = true;
    }
}
bool UiCaptureOn() { return g_capture && g_contextReady; }
void UiImGuiAllocators(void** alloc, void** freeFn, void** user) {
    ImGuiMemAllocFunc a = nullptr;
    ImGuiMemFreeFunc f = nullptr;
    void* u = nullptr;
    ImGui::GetAllocatorFunctions(&a, &f, &u);
    if (alloc) *alloc = reinterpret_cast<void*>(a);
    if (freeFn) *freeFn = reinterpret_cast<void*>(f);
    if (user) *user = u;
}

}  // namespace tw3se
