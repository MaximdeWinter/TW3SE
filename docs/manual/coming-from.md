# Coming from another tool

Mods built on Witcher Native Bridge, the Ultimate ASI Loader or an overlay of their own run on TW3SE, most of them as
they are. This page shows what you can drop and what replaces it.

## Witcher Native Bridge

Mods made for Witcher Native Bridge run on TW3SE alone. TW3SE has the bridge's functions (API 2), so your players
need neither `WitcherNativeBridge.asi` nor the bridge's `dinput8.dll`. When the bridge is installed, it does its own
work and both run side by side. `tw3se.log` says which of the two serves your mod:

```
native bridge: TW3SE stands in for Witcher Native Bridge (API 2) - its mods run without it
native bridge: MyMod_Heal registered by a mod for the bridge
```

To move your mod to TW3SE, write each native as a normal C++ function:

Before (the bridge):

```cpp
void Heal(void*, void* frame, void* result) {
    float amount = WNB_ReadFloatParameter(frame);
    bool full = WNB_ReadBoolParameter(frame);
    WNB_ReturnInt(frame, result, DoHeal(amount, full));
}
WNB_RegisterNative("MyMod_Heal", &Heal);
```

After (TW3SE):

```cpp
static int Heal(float amount, bool full) { return DoHeal(amount, full); }
tw3se::register_native<&Heal>("MyMod_Heal");
```

The script keeps its `import function MyMod_Heal(amount : float, full : bool) : int;`. Parameters and the return can
be `int`, `bool`, `float`, `std::string` and `tw3se::name`. `TW3SE_StringToName` gives a script the name of a text.

The SDK has the bridge's own example moved to TW3SE: `example\from_bridge.cpp` with its five natives (int, bool,
float, string, name), each with the bridge's lines above it, and `example\from_bridge.ws`, the bridge's script
unchanged. The string native shows the biggest step: the bridge reads a `WNB_String` and converts it to UTF-16 and
back, TW3SE hands over a `std::string` in UTF-8.

## An .asi mod and the Ultimate ASI Loader

TW3SE loads every `.asi` in `bin\x64_dx12`, `scripts\` and `plugins\` and calls its `InitializeASI`, as the Ultimate
ASI Loader does. Ship your `.asi` alone and write "Requires TW3SE" on your page.

Another mod may bring the Ultimate ASI Loader as `dinput8.dll` and replace TW3SE's. TW3SE then starts through
`!tw3se.asi`, which that loader loads first. Each `.asi` loads once.

Your `.asi` reaches all of TW3SE through one header. When TW3SE is missing, `connect()` returns false and your mod
runs as before:

```cpp
#include "tw3se/tw3se_asi.h"

extern "C" __declspec(dllexport) void InitializeASI() {
    if (!tw3se::asi::connect()) return;   // no TW3SE: your mod as before
    tw3se::register_native<&Heal>("MyMod_Heal");
}
```

## An ImGui overlay of your own

A menu of your own is one draw function. TW3SE draws your windows in its overlay with its fonts and cursor and takes
care of DirectX and the window's input:

```cpp
#include "tw3se/tw3se_imgui.h"

static bool g_open = false;
static void Draw() {
    if (!g_open) return;
    ImGui::Begin("My trainer", &g_open);
    if (ImGui::Button("Give 100 crowns")) tw3se::call(tw3se::the_player(), "AddMoney", {tw3se::value(100)});
    ImGui::End();
}

tw3se::imgui::on_draw(&Draw);
tw3se::imgui::overlay(true);
// your key opens it: g_open = true; tw3se::imgui::capture_input(true);
```

`capture_input(true)` gives your menu every key and mouse message; the controller stays with the game.
`tw3se::pad()` reads the controller, `tw3se::game_state()` tells the main menu, loading screens, the game's menus,
scenes and pauses apart. Compile `tw3se\imgui\imgui*.cpp` from the SDK into your DLL (the ImGui version TW3SE runs).

## A command queue between C++ and WitcherScript

Some mods send commands as text from C++ to a script that asks for them every frame and splits them again. With
TW3SE, C++ calls the script function itself:

```cpp
tw3se::call(tw3se::the_player(), "AddMoney", {tw3se::value(100)});   // a method of the player
auto money = tw3se::call(tw3se::the_player(), "GetMoney");           // money[0].v.i
auto len = tw3se::call(nullptr, "StrLen", {tw3se::value("witcher")}); // a global function: len[0].v.i == 7
```

For messages both ways there are channels. A script, Lua and a plugin all reach the same listeners:

```
TW3SE_Send("mymod.ui", "opened");             // WitcherScript
var cmd : string = TW3SE_Receive("mymod.cmd"); // the next message of the channel, "" when none
```

```cpp
tw3se::on_message("mymod.ui", &OnUi);          // C++
tw3se::send("mymod.cmd", "heal 100");
```

## Menu XML and .w3strings

Texts for your menu labels, option names and item names can be text files: `Mods\<modName>\tw3se\strings\en.txt`,
`de.txt` and so on, one `key = text` per line. The game shows the text of its language, English when the language
has none.

The menu XML itself can come from TW3SE. A file `Mods\<modName>\tw3se\init.lua` with a few lines makes the page in
**Options** > **Mods** and in the TW3SE Menu, with labels by language:

```lua
w3.mod{name = "MyMod", settings = {
    {key = "god_mode", default = false, label = {EN = "God mode", DE = "Gottmodus"}},
    {key = "damage", type = "slider", min = 0.5, max = 5, default = 1, label = "Damage"},
}}
```

Your script reads the values as from any menu page: TW3SE writes the page to
`bin\config\r4game\user_config_matrix\pc\tw3se_<name>.xml` (group `TW3SE_<name>`), and
`theGame.GetInGameConfigWrapper().GetVarValue(group, var)` reads it with the ids in that file.

In the game's **Options** > **Mods** every mod's page opens (patch 5.0 opens only the first 10). From the 11th entry
on, a page sits in a submenu with one entry, **Settings**. A missing translation shows readable ("Free alchemy"
for `##option_free_alchemy`). **F2** > **Mods** has every mod's settings in one list with a search
field.

## A mod that replaces game scripts

A mod that ships its own copy of a game script replaces the whole file, and two such mods need Script Merger.
`tools\to_annotations.py` (in the SDK) writes what your copy changes as annotations of patch 5.0, which change one
function or add one field:

```
python tools\to_annotations.py Mods\modMyMod
```

A changed function of a class becomes `@replaceMethod(Class)`, a new one `@addMethod(Class)`, a new field
`@addField(Class)`, a changed global function `@replaceMethod function`. The result goes to `Mods\modMyMod_annotated`;
`annotations_report.txt` lists what needs a hand: events (annotations take functions only), states, changed defaults
and fields. Where your function only adds lines before or after the game's code, the report says so: `@wrapMethod`
with `wrappedMethod(...)` keeps other mods' changes to that function. **F2** > **Mods** > **Conflicts** shows the
scripts and files more than one mod brings.

## Saved values and the web

A WitcherScript mod keeps values with the save (`TW3SE_SavedSet(mod, key, value)`, `TW3SE_SavedGet`) and asks the web
(`TW3SE_HttpRequest(method, url, body, channel)`, the answer comes on the channel). Plugins have
`tw3se::saved_set`, `tw3se::saved_get` and `tw3se::http_request`. [Script natives](script-natives.md) lists the import
lines.

## A ReShade add-on

Mods that took ReShade's add-on API to put data into the game's shaders (wind, grass that parts, fog) do the same
with TW3SE's render functions, on the game's own D3D12 objects: every pipeline when the game creates it, a parameter
appended to a root signature of the game, a callback when the game sets a pipeline of yours (bind your buffer there),
shaders replaced from memory, and a moment once per frame before the game draws for your own compute work. ReShade
can go. [Plugin API](plugin-api.md#rendering-interface-21) has the functions,
`render_example.cpp` in the SDK a complete plugin.

## After a game patch

A plugin that takes the engine's functions from TW3SE by name (`GetAddress`, [Addresses](addresses.md)) keeps
working after a patch: TW3SE finds them on the new build by their bytes. Byte patterns of your own need an update
of your mod.
