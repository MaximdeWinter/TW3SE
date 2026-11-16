// TW3SE plugin template, a starting point for a plugin of your own. Copy this folder, rename it and the .dll, change
// the TW3SE_PLUGIN line, and build it the way build.bat builds the example. Put the .dll into
// <game>\bin\x64_dx12\tw3se\plugins\. It uses the C++ layer (tw3se/tw3se.hpp): a WitcherScript native is an
// ordinary typed C++ function, and the layer reads the parameters and writes the result.
#include "tw3se/tw3se.hpp"
#include <cstdio>
#include <windows.h>
#pragma comment(lib, "user32.lib")  // GetAsyncKeyState (hotkey demo)

// A WitcherScript native written as a plain C++ function. A script declares and calls it as:
//     import function TPL_Add(a : int, b : int) : int;
//     ... TPL_Add(40, 2) ...   // -> 42
// (A WitcherScript type keyword - name, string, int, ... - cannot be a parameter name in the import.)
static int TPL_Add(int a, int b) { return a + b; }

// Floats:
//     import function TPL_ScaleF(x : float, factor : float) : float;
static float TPL_ScaleF(float x, float factor) { return x * factor; }

// A bool return and a string parameter (a string return needs interface 21):
//     import function TPL_StartsWithA(word : string) : bool;
static bool TPL_StartsWithA(std::string word) { return !word.empty() && (word[0] == 'a' || word[0] == 'A'); }

// A developer-console command (pipe \\.\pipe\tw3se; send with tools/tw3se.py).
static void TplInfo(const char* args, char* reply, uint32_t size) {
    std::snprintf(reply, size, "template plugin loaded (game build %08x, args: '%s')",
                  tw3se::api()->gameBuild, args ? args : "");
}

// A second command that shows the richer API: calling a game function with a typed argument, and reaching the
// live player object. Everything here needs a newer interface than the template's minimum, so it is only
// registered when the running extender is new enough (see TW3SE_PluginLoad). A feature an older extender may
// lack is gated on api->interfaceVersion.
static void TplDemo(const char* args, char* reply, uint32_t size) {
    // Call a globally registered game function by name with a typed argument spec (interface v9).
    // "s:witcher" passes the string "witcher"; StrLen returns its length.
    int len = 0;
    tw3se::call_function_v("StrLen", "s:witcher", nullptr, &len);

    // Reach the player object (interface v7). NULL in the main menu or before a character spawns - always
    // check, a live-world object is never guaranteed.
    void* player = tw3se::the_player();

    // With a live object in hand you would read or write its fields by name, for example:
    //     float stamina = tw3se::read_property_of<float>(player, "stamina");
    //     tw3se::write_property_of<float>(player, "stamina", 100.0f);
    // The field name must match the class; use the `props` console command on a live object to list them.
    // Left commented so the template never touches an unverified field on your save.

    std::snprintf(reply, size, "StrLen(\"witcher\")=%d via typed call; player=%s",
                  len, player ? "in world" : "null (menu / no spawn)");
}

// A per-tick callback with a hotkey. Called every frame on the main thread; here it fires once
// each time F9 goes from up to down (edge-detected, so it does not repeat while held).
static void OnTick(float /*deltaSeconds*/, void* /*context*/) {
    static bool firstTick = true;
    if (firstTick) {  // one log line at the first tick
        firstTick = false;
        tw3se::api()->Log("Template", "OnTick is firing");
    }
    static bool wasDown = false;
    bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (down && !wasDown) tw3se::api()->Log("Template", "F9 pressed - do your action here");
    wasDown = down;
}

// An on-screen overlay HUD (interface v10). Called every frame while the overlay records; draw with the
// tw3se::overlay_* helpers (pixels from the top-left, colour 0xRRGGBBAA). Registering this callback is what
// turns the overlay on. This is how a mod draws its own HUD over the game.
static void OnOverlayDraw(void* /*context*/) {
    tw3se::overlay_rect(10.0f, 10.0f, 230.0f, 56.0f, 0x00000099);        // dark translucent panel
    tw3se::overlay_text(20.0f, 16.0f, 0x66E0FFFFu, "Template HUD");      // title
    tw3se::overlay_text(20.0f, 36.0f, 0xFFFFFFFFu, "overlay API v10 ok");
}

TW3SE_PLUGIN("Template", "YourName", 0x00010000, /*minInterface*/ 3)  // 3: uses RegisterOnTick

TW3SE_EXPORT int TW3SE_PluginLoad(const TW3SE_Interface* api) {
    tw3se::init(api);  // required before any tw3se:: convenience helper
    api->Log("Template", "loaded (interface %u, build %08x)", api->interfaceVersion, api->gameBuild);

    tw3se::register_command("tplinfo", &TplInfo, "template: show plugin info");

    // The parameter count is taken from each function's arity, so it can never drift from the handler.
    tw3se::register_native<&TPL_Add>("TPL_Add");
    tw3se::register_native<&TPL_ScaleF>("TPL_ScaleF");
    tw3se::register_native<&TPL_StartsWithA>("TPL_StartsWithA");

    tw3se::on_tick(&OnTick);  // fires every frame; here: an F9 hotkey

    // Feature-gate: TplDemo uses the_player (v7) and call_function_v (v9). Register it only when the running
    // extender has them, so the plugin also loads on an older one.
    if (api->interfaceVersion >= 9)
        tw3se::register_command("tpldemo", &TplDemo, "template: typed call + live player (needs interface 9)");

    if (api->interfaceVersion >= 10)
        tw3se::on_overlay_draw(&OnOverlayDraw);  // draws the HUD above; turns the overlay on by itself

    return 1;
}
