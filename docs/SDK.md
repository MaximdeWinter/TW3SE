# TW3SE Plugin SDK

A plugin is a DLL in `bin\x64_dx12\tw3se\plugins\` (REDkit: `bin\x64_RedKit\tw3se\plugins\`). It exports two
functions and gets the engine through a C interface (`tw3se/tw3se.h`). The C++17 header `tw3se/tw3se.hpp` turns a
normal typed function into a WitcherScript native and wraps most other calls.

The full guide is the manual: [Your first plugin](manual/first-plugin.md) and [Plugin API](manual/plugin-api.md).

## The smallest plugin

```cpp
#include "tw3se/tw3se.hpp"
#include <stdio.h>

static int Add(int a, int b) { return a + b; }   // import function Add(a : int, b : int) : int;

static void Hello(const char* args, char* reply, uint32_t size) { snprintf(reply, size, "hello"); }

TW3SE_PLUGIN("MyPlugin", "me", 0x00010000, /*minInterface*/ 2)

TW3SE_EXPORT int TW3SE_PluginLoad(const TW3SE_Interface* api) {
    tw3se::init(api);
    tw3se::register_native<&Add>("Add");              // parameters from the C++ signature
    tw3se::register_command("hello", &Hello, "says hello");
    return 1;
}
```

Build it in the "x64 Native Tools Command Prompt for VS 2022", in the SDK folder:

```
cl /nologo /O2 /MT /LD /EHsc /W3 /std:c++17 /Iinclude example\template.cpp /link /OUT:example\template.dll
```

Put the DLL into `tw3se\plugins\`. Set `minInterface` to the highest interface version whose functions you use.

## What the interface has

| Version | Functions |
|---|---|
| 1 | `Log`, `GetAddress`, `GetImageBase`, `Hook`, `RunOnMainThread`, `RegisterCommand`, `MountDLC` |
| 2 | `RegisterNative`, `ReadParam`, `EndParams`: own script functions |
| 3 | `RegisterOnTick` |
| 4, 5 | `FindProperty`, `PropertyPtr`, `GetObjectClass`, `FindClassByName`: any field by name |
| 6, 7 | `CallFunction`, `CallMethod`, `GetThePlayer`, `GetTheGame`, `ResolveHandle`, `GetCNameString` |
| 8 | `RegisterOnPresent`: the swap chain every frame |
| 9 | `CallFunctionV`, `CallMethodV`: typed arguments |
| 10 | `RegisterOnOverlayDraw`, `OverlayDrawText`, `OverlayDrawRect`, `OverlayGetScreenSize` |
| 11 | `RegisterOnGameLoaded` |
| 12 | `EntityByGuid`, `EntityGuid`, `WorldEdit` |
| 13, 14 | `RegisterOnPose`, `WatchPose`: the bones of a figure every frame |
| 15 | `IsEditor`: 1 inside the REDkit editor |
| 16 | `RunLua`, `RegisterLuaFunction`: run Lua, give Lua mods your own functions |
| 17 | `PlaySound`, `StopSound`, `SetSoundVolume`, `SoundPlaying`, `PreloadSound`: WAV and OGG files, flat or in 3D |
| 18 | `Call`, `GetField`, `SetField`, `HookScript`, `CallOriginal`, the catalog: every function and field with typed values |
| 19 | `SpawnDecals` and the other decal functions |
| 20 | `LuaReplyBytes` |
| 21 | `ReturnString`, `NameFromString`: strings and names in natives; `RegisterOnImGui`, `ImGuiOverlay`, `ImGuiCaptureInput`: windows with Dear ImGui; `SendToChannel`, `RegisterOnMessage`, `GameState`, `PadRead`; `RegisterOnPipeline`, `ShaderReplace`, `RootSignatureAddParam`, `RootParamIndex`, `MarkPipeline`, `RegisterOnSubmit`: data of your own in the game's shaders |
| 22 | `DeformOwn`, `DeformLoad`, `DeformWeights` and the other blend shape functions |
| 23 | `SkinHookAdd`, `SkinHookCapsules` and the other skinning hook functions |
| 24 | `GetComponentPose`: one component's pose, dangle meshes in the pose callback |
| 25 | `RegisterOnRenderRebuild`: before and after the game rebuilds its graphics |

New functions are appended at the end. Check `interfaceVersion` before you call a newer one. A plugin built for an
older version keeps working.

## What is in the SDK

| File | What |
|---|---|
| `include\tw3se\tw3se.h`, `tw3se.hpp` | the interface and the C++ helpers |
| `include\tw3se\tw3se_imgui.h`, `imgui\` | windows with Dear ImGui (compile `imgui\imgui*.cpp` into your DLL) |
| `include\tw3se\tw3se_asi.h` | TW3SE from an `.asi` mod, nothing to link |
| `example\example.cpp`, `template.cpp` | the smallest plugins (C and C++) |
| `example\imgui_example.cpp` | a menu of your own on F7 that reads and gives crowns; natives with strings and names |
| `example\asi_example.cpp` | an `.asi` mod that uses TW3SE: a native, messages to and from scripts, the game's state |
| `example\render_example.cpp` | data of your own in the game's shaders (what a ReShade add-on was needed for) |
| `vs-template\MyPlugin.sln` | a Visual Studio 2022 project to start from |
| `tools\to_annotations.py` | a mod's replaced game scripts as annotations (two mods change one file, both changes count) |

Mods made for Witcher Native Bridge run on TW3SE as they are; [Coming from another tool](manual/coming-from.md) shows
the move to TW3SE's own functions.

## A plugin's own data

Keep it in a file of your own, outside the save. The file natives (`TW3SE_WriteNamed` and `TW3SE_ReadNamed` in
`tw3se\out\`) or plain C++ file I/O do the job.

## Further reading

- `docs/manual/`: the manual
- `docs/ABI_NOTES.md`: how a native reads its parameters and returns a value
- `docs/engine/`: engine reference (script VM, RTTI, content and DLC, lifecycle, input, world)
