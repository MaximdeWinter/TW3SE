// TW3SE core - the controller: its buttons as keys for Lua ("Pad_A", "Pad_LB+Pad_Y"), the TW3SE Menu by holding both
// sticks, and no controller input for the game while the menu is open.
//
// TW3SE reads the first XInput controller through xinput1_4.dll (ImGui's backend loads the same one for the menu's
// navigation). The game reads its controller through XInputGetState of xinput9_1_0.dll, its own code (no forwarder):
// a hook there hands the game an idle controller while the TW3SE Menu is open. The game can read its controller
// through GameInput as well (GameInputRedist.dll); with gameinput_hold a reading's gamepad state is held back too
// (GameInput below). Bits 0-15 are XInput's buttons, 16 and 17 the left
// and right trigger (pressed past 30 of 255).
#include "core.h"
#include "lua_ui.h"

#include <atomic>
#include <intrin.h>
#include <xinput.h>
#include <GameInput.h>

namespace tw3se {
namespace {

using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t g_read = nullptr;     // xinput1_4 (or 1_3): TW3SE's own reading
XInputGetState_t g_gameRead = nullptr;  // the game's xinput9_1_0 XInputGetState, hooked
uint32_t g_mask = 0;
ULONGLONG g_readAt = 0, g_retryAt = 0;
bool g_connected = false;

constexpr BYTE kTrigger = 30;

uint32_t MaskOf(const XINPUT_GAMEPAD& g) {
    uint32_t m = g.wButtons;
    if (g.bLeftTrigger > kTrigger) m |= 1u << 16;
    if (g.bRightTrigger > kTrigger) m |= 1u << 17;
    return m;
}

DWORD WINAPI GameReadHook(DWORD user, XINPUT_STATE* s) {
    DWORD r = g_gameRead(user, s);
    if (r == ERROR_SUCCESS && s && (UiIsOpen() || UiCaptureOn()))  // TW3SE's menu or a plugin's (1.2)
        ZeroMemory(&s->Gamepad, sizeof s->Gamepad);  // packet number stays
    return r;
}

// --- GameInput (F4). The game asks GameInput for readings (IGameInput::GetCurrentReading / GetNextReading)
// and takes the gamepad's sticks and buttons out of each with IGameInputReading::GetGamepadState. While a menu is
// open, that state comes back resting. The slots are those of the Windows SDK's GameInput.h (10.0.26100, the v0
// interface GameInput.dll serves): IGameInput 4 and 5, IGameInputReading 22. The IGameInput vtable is patched on an
// object of our own at the start (the game's object shares it); a reading's vtable on the first reading the game
// gets. A vtable slot is one aligned pointer: an input thread sees the old function or the new one.
using GiCurrent_t = HRESULT(STDMETHODCALLTYPE*)(IGameInput*, GameInputKind, IGameInputDevice*, IGameInputReading**);
using GiNext_t = HRESULT(STDMETHODCALLTYPE*)(IGameInput*, IGameInputReading*, GameInputKind, IGameInputDevice*,
                                             IGameInputReading**);
using GiPad_t = bool(STDMETHODCALLTYPE*)(IGameInputReading*, GameInputGamepadState*);
constexpr int kGiCurrent = 4, kGiNext = 5, kGiPad = 22;
GiCurrent_t g_giCurrent = nullptr;
GiNext_t g_giNext = nullptr;
std::atomic<GiPad_t> g_giPad{nullptr};
std::atomic<bool> g_giPadPatched{false};
std::atomic<uint64_t> g_giHeld{0};  // gamepad states handed back resting (the `pad` log line, tests)

bool PatchSlot(void** vtbl, int slot, void* fn) {  // the old pointer must be stored by the caller first
    DWORD old = 0;
    if (!VirtualProtect(&vtbl[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
    InterlockedExchangePointer(&vtbl[slot], fn);
    VirtualProtect(&vtbl[slot], sizeof(void*), old, &old);
    return true;
}

bool STDMETHODCALLTYPE GiPadHook(IGameInputReading* r, GameInputGamepadState* s) {
    GiPad_t orig = g_giPad.load(std::memory_order_acquire);
    bool ok = orig ? orig(r, s) : false;
    if (ok && s && (UiIsOpen() || UiCaptureOn())) {
        ZeroMemory(s, sizeof *s);
        g_giHeld.fetch_add(1, std::memory_order_relaxed);
    }
    return ok;
}

void SeeReading(IGameInputReading* r) {  // the reading class's vtable, patched once
    if (!r || g_giPadPatched.load(std::memory_order_acquire) || g_giPadPatched.exchange(true)) return;
    void** vt = *reinterpret_cast<void***>(r);
    g_giPad.store(reinterpret_cast<GiPad_t>(vt[kGiPad]), std::memory_order_release);
    if (!PatchSlot(vt, kGiPad, reinterpret_cast<void*>(&GiPadHook))) g_giPadPatched = false;
}

std::atomic<uint64_t> g_giAsked{0};       // readings the game asked for (found or not)
std::atomic<uint32_t> g_giKinds{0};       // the kinds it asked for (GameInputKind bits)

HRESULT STDMETHODCALLTYPE GiCurrentHook(IGameInput* gi, GameInputKind k, IGameInputDevice* d, IGameInputReading** r) {
    g_giAsked.fetch_add(1, std::memory_order_relaxed);
    g_giKinds.fetch_or(static_cast<uint32_t>(k), std::memory_order_relaxed);
    HRESULT h = g_giCurrent(gi, k, d, r);
    if (SUCCEEDED(h) && r) SeeReading(*r);
    return h;
}
HRESULT STDMETHODCALLTYPE GiNextHook(IGameInput* gi, IGameInputReading* ref, GameInputKind k, IGameInputDevice* d,
                                     IGameInputReading** r) {
    g_giAsked.fetch_add(1, std::memory_order_relaxed);
    g_giKinds.fetch_or(static_cast<uint32_t>(k), std::memory_order_relaxed);
    HRESULT h = g_giNext(gi, ref, k, d, r);
    if (SUCCEEDED(h) && r) SeeReading(*r);
    return h;
}

// Which GameInputCreate the game calls (the log names the caller once): GameInput.dll serves the v0 interface the
// slots above belong to; the redistributable's own export serves a newer one with other slots.
using GiCreate_t = HRESULT(WINAPI*)(IGameInput**);
GiCreate_t g_giCreateV0 = nullptr, g_giCreateRedist = nullptr;
std::atomic<int> g_giCreateLogs{0};
void LogCreate(const char* which, void* caller, IGameInput* gi) {
    if (g_giCreateLogs.fetch_add(1) >= 4) return;
    Log("pad: GameInputCreate of %s called from %s (object vtable %p)", which,
        AddressPlace(reinterpret_cast<uintptr_t>(caller)).c_str(), gi ? *reinterpret_cast<void**>(gi) : nullptr);
}
HRESULT WINAPI GiCreateV0Hook(IGameInput** out) {
    HRESULT h = g_giCreateV0(out);
    LogCreate("GameInput.dll", _ReturnAddress(), SUCCEEDED(h) && out ? *out : nullptr);
    return h;
}
HRESULT WINAPI GiCreateRedistHook(IGameInput** out) {
    HRESULT h = g_giCreateRedist(out);
    LogCreate("GameInputRedist.dll", _ReturnAddress(), SUCCEEDED(h) && out ? *out : nullptr);
    return h;
}

std::string InstallGameInput() {
    HMODULE v0 = LoadLibraryW(L"GameInput.dll");
    auto create = v0 ? reinterpret_cast<GiCreate_t>(GetProcAddress(v0, "GameInputCreate")) : nullptr;
    if (!create) return "no GameInput.dll";
    if (HMODULE redist = GetModuleHandleW(L"GameInputRedist.dll"))
        if (void* f = reinterpret_cast<void*>(GetProcAddress(redist, "GameInputCreate")))
            Hook(f, reinterpret_cast<void*>(&GiCreateRedistHook), reinterpret_cast<void**>(&g_giCreateRedist));
    IGameInput* gi = nullptr;
    if (FAILED(create(&gi)) || !gi) return "GameInputCreate failed";
    Hook(reinterpret_cast<void*>(create), reinterpret_cast<void*>(&GiCreateV0Hook), reinterpret_cast<void**>(&g_giCreateV0));
    void** vt = *reinterpret_cast<void***>(gi);  // the object stays: the patched vtable is shared with the game's
    g_giCurrent = reinterpret_cast<GiCurrent_t>(vt[kGiCurrent]);
    g_giNext = reinterpret_cast<GiNext_t>(vt[kGiNext]);
    bool a = PatchSlot(vt, kGiCurrent, reinterpret_cast<void*>(&GiCurrentHook));
    bool b = PatchSlot(vt, kGiNext, reinterpret_cast<void*>(&GiNextHook));
    return a && b ? "GameInput held back too" : "GameInput NOT held back (its vtable could not be written)";
}

// held both sticks (Pad_LS + Pad_RS, or menu_pad in tw3se.config) for 0.5 s: the TW3SE Menu opens or closes
uint32_t g_menuMask = 0;
ULONGLONG g_menuSince = 0;
bool g_menuDone = false;

}  // namespace

uint32_t PadMask() {
    if (!g_read) return 0;
    ULONGLONG now = GetTickCount64();
    if (now - g_readAt < 8) return g_mask;  // once per frame is enough
    if (!g_connected && now < g_retryAt) return 0;  // a missing controller is slow to ask: once a second
    g_readAt = now;
    XINPUT_STATE s{};
    g_connected = g_read(0, &s) == ERROR_SUCCESS;
    g_mask = g_connected ? MaskOf(s.Gamepad) : 0;
    if (!g_connected) g_retryAt = now + 1000;
    return g_mask;
}

// The whole state for plugins (interface 21): buttons as PadMask, sticks -1..1 past XInput's dead zones, triggers 0..1.
int PadRead(uint32_t* buttons, float* axes) {
    if (buttons) *buttons = 0;
    if (axes) for (int k = 0; k < 6; ++k) axes[k] = 0.0f;
    if (!g_read) return 0;
    XINPUT_STATE s{};
    if (g_read(0, &s) != ERROR_SUCCESS) return 0;
    if (buttons) *buttons = MaskOf(s.Gamepad);
    if (axes) {
        auto stick = [](SHORT v, SHORT dead) {
            int a = v < 0 ? -v : v;
            if (a <= dead) return 0.0f;
            float f = (a - dead) / static_cast<float>(32767 - dead);
            if (f > 1.0f) f = 1.0f;
            return v < 0 ? -f : f;
        };
        axes[0] = stick(s.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        axes[1] = stick(s.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
        axes[2] = stick(s.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        axes[3] = stick(s.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
        axes[4] = s.Gamepad.bLeftTrigger / 255.0f;
        axes[5] = s.Gamepad.bRightTrigger / 255.0f;
    }
    return 1;
}
// "pad_a" -> its bit (0-17), -1 for another name. The names: Pad_A, B, X, Y, LB, RB, LT, RT, LS, RS (the sticks
// pressed), Back, Start, Up, Down, Left, Right.
int PadBit(const std::string& lower) {
    static const std::pair<const char*, int> names[] = {
        {"pad_up", 0},   {"pad_down", 1}, {"pad_left", 2},  {"pad_right", 3}, {"pad_start", 4}, {"pad_back", 5},
        {"pad_ls", 6},   {"pad_rs", 7},   {"pad_lb", 8},    {"pad_rb", 9},    {"pad_a", 12},    {"pad_b", 13},
        {"pad_x", 14},   {"pad_y", 15},   {"pad_lt", 16},   {"pad_rt", 17}};
    for (const auto& [n, b] : names)
        if (lower == n) return b;
    return -1;
}

const char* PadName(int bit) {
    static const char* names[18] = {"Pad_Up", "Pad_Down", "Pad_Left", "Pad_Right", "Pad_Start", "Pad_Back", "Pad_LS",
                                    "Pad_RS", "Pad_LB", "Pad_RB", nullptr, nullptr, "Pad_A", "Pad_B", "Pad_X", "Pad_Y",
                                    "Pad_LT", "Pad_RT"};
    return bit >= 0 && bit < 18 ? names[bit] : nullptr;
}

void PadTick() {  // the main thread, once per tick: the menu's controller combination
    if (!g_menuMask) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool held = pid == GetCurrentProcessId() && (PadMask() & g_menuMask) == g_menuMask;
    ULONGLONG now = GetTickCount64();
    if (!held) {
        g_menuSince = 0;
        g_menuDone = false;
        return;
    }
    if (!g_menuSince) g_menuSince = now;
    if (!g_menuDone && now - g_menuSince >= 500) {
        g_menuDone = true;
        UiToggle();
    }
}

namespace {
// `pad`: the controller as TW3SE sees it, and what was held back from the game (tests, bug reports)
void CmdPad(const char*, char* reply, uint32_t size) {
    snprintf(reply, size, "pad: XInput %s, buttons 0x%05x; GameInput: %llu reading(s) asked for (kinds 0x%x), reading "
             "patched %s, %llu gamepad state(s) held back; a menu %s",
             g_read ? (g_connected ? "connected" : "no controller") : "missing", PadMask(),
             static_cast<unsigned long long>(g_giAsked.load()), g_giKinds.load(),
             g_giPadPatched.load() ? "yes" : "no (no reading seen yet)", static_cast<unsigned long long>(g_giHeld.load()),
             UiIsOpen() || UiCaptureOn() ? "is open" : "is closed");
}
}  // namespace

void InstallPad() {
    g_uiPadMask = &PadMask;  // w3.ui.key takes controller buttons too (a key field in the TW3SE Menu)
    g_uiPadName = &PadName;
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll"}) {
        if (HMODULE m = LoadLibraryW(dll)) {
            g_read = reinterpret_cast<XInputGetState_t>(GetProcAddress(m, "XInputGetState"));
            if (g_read) break;
        }
    }
    std::string combo = ConfigGet("menu_pad", "Pad_LS+Pad_RS");
    std::string low = combo;
    for (char& c : low) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (low != "off" && !low.empty()) {
        size_t start = 0;
        while (start <= low.size()) {
            size_t plus = low.find('+', start);
            int b = PadBit(low.substr(start, plus == std::string::npos ? std::string::npos : plus - start));
            if (b < 0) {
                Log("pad: menu_pad = %s - not a controller combination (Pad_LS+Pad_RS), the menu has none", combo.c_str());
                g_menuMask = 0;
                break;
            }
            g_menuMask |= 1u << b;
            if (plus == std::string::npos) break;
            start = plus + 1;
        }
    }
    HMODULE game = GetModuleHandleW(L"xinput9_1_0.dll");
    void* fn = game ? reinterpret_cast<void*>(GetProcAddress(game, "XInputGetState")) : nullptr;
    bool blocked = fn && Hook(fn, reinterpret_cast<void*>(&GameReadHook), reinterpret_cast<void**>(&g_gameRead));
    // GameInput only on request (gameinput_hold = on)
    std::string gi = ConfigBool("gameinput_hold", false) ? InstallGameInput() : "GameInput left alone (gameinput_hold = on)";
    RegisterCommand("pad", &CmdPad, "pad: the controller, and what TW3SE held back from the game while a menu is open");
    Log("pad: %s; the game's XInput controller %s while a menu is open, %s; menu: %s", g_read ? "XInput read" : "no XInput",
        blocked ? "held back" : "is NOT held back", gi.c_str(),
        g_menuMask ? (combo + " held 0.5 s").c_str() : "no controller combination");
}

}  // namespace tw3se
