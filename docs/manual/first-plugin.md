# Your first plugin

A plugin is a DLL in `tw3se\plugins\`. TW3SE loads it when the game starts and hands it a function table (the
[Plugin API](plugin-api.md)). Use it for hooks, object fields, game function calls and code that runs every frame.

This guide uses the C++ header `tw3se/tw3se.hpp`. The plain C header is `tw3se/tw3se.h`.

## Build the template

The SDK has a template plugin. You need Visual Studio 2022 (or its Build Tools) with the C++ x64 tools. Open the
"x64 Native Tools Command Prompt for VS 2022", go to the SDK folder and build it:

```
cl /nologo /O2 /MT /LD /EHsc /W3 /std:c++17 /Iinclude example\template.cpp /link /OUT:example\template.dll
```

Put `template.dll` into `bin\x64_dx12\tw3se\plugins\` and start the game. `tw3se.log` shows
`plugin Template … loaded`.

## Or start in Visual Studio

`vs-template\MyPlugin.sln` in the SDK opens in Visual Studio 2022 and builds as it is (**Release** | **x64**). It
has a script function, a menu on F6 drawn with ImGui and a console command. Copy `bin\Release\MyPlugin.dll` into
`bin\x64_dx12\tw3se\plugins\`.

## The two exports

```cpp
#include "tw3se/tw3se.hpp"

TW3SE_PLUGIN("MyPlugin", "yourname", 0x00010000, /*minInterface*/ 6)

TW3SE_EXPORT int TW3SE_PluginLoad(const TW3SE_Interface* api) {
    tw3se::init(api);                 // first, before any tw3se:: helper
    api->Log("MyPlugin", "loaded");
    return 1;                          // 1 = loaded
}
```

`TW3SE_PLUGIN(name, author, version, minInterface)` writes the export `TW3SE_PluginQuery`. `minInterface` is the
lowest interface version your plugin needs. Set it to the highest version of the functions you call
(`RegisterNative` 2, `RegisterOnTick` 3, fields 4 to 5, `CallFunction` 6). If TW3SE is older, it skips the plugin
and the log says why.

## A console command

```cpp
static void Hello(const char* args, char* reply, uint32_t size) {
    snprintf(reply, size, "hello (game build %08x, args: '%s')", tw3se::api()->gameBuild, args ? args : "");
}

// in TW3SE_PluginLoad:
tw3se::register_command("hello", &Hello, "says hello");
```

Send it through the pipe: `python tools\tw3se.py hello`. See [Console commands](console-commands.md).

## A WitcherScript function

Write a normal C++ function and register it. Parameters and return type come from the signature.

```cpp
static int Add(int a, int b) { return a + b; }

// in TW3SE_PluginLoad:
tw3se::register_native<&Add>("Add");
```

In a script:

```
import function Add(a : int, b : int) : int;
// Add(40, 2) == 42
```

Types: `int`, `float`, `bool`, and `std::string` / `const char*` for `string`. Parameter names in the `import` line
have to differ from type words (`name`, `string`, `int` …).

## Code every frame

`on_tick` calls your function once per game tick on the main thread, in menus too. A hotkey:

```cpp
#include <windows.h>

static void OnTick(float dt, void* ctx) {
    static bool was = false;
    bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (down && !was) tw3se::api()->Log("MyPlugin", "F9");   // once per press
    was = down;
}

// in TW3SE_PluginLoad (minInterface 3):
tw3se::on_tick(&OnTick);
```

If a callback crashes, it will be logged and removed. The game will continue to work normally.

## Fields of a game object

Read or set any field of an object by name. Get the player with `tw3se::the_player()` (null in the menu), the
`self` pointer in a hook, or the return value of a game function.

```cpp
tw3se::write_property_of<float>(obj, "someFloatField", 1000.f);
float v = tw3se::read_property_of<float>(obj, "someFloatField");
```

Engine fields and script `var`s both work.

## Call a game function

```cpp
int len = 0;
tw3se::call_function("StrLen", {"witcher"}, nullptr, &len);   // len == 7
```

Arguments are strings, the return value is int or bool. `self` is the object for a method, `nullptr` for a global
function. More in [How it works](how-it-works.md).

## When things run

`TW3SE_PluginLoad` runs before scripts compile and before the world exists. Register functions and commands there.
For anything that needs the world or the player, use a tick callback or a hook and check for null.
