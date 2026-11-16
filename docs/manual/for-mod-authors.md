# For mod authors

TW3SE is the base for mods that need more than WitcherScript. It loads with the game and gives your mod the engine,
the screen and the input. Your players install it once.

## What your mod gets

- **New script functions** from Lua, from a C++ plugin or from your `.asi`, with `int`, `float`, `bool`, `string` and
  `name` parameters and returns
- **Every function and field of the game** by name, from Lua and C++: call a script function with its arguments,
  read and write any field, hook any function
- **Windows of your own** with Dear ImGui over the game: TW3SE's overlay, fonts, cursor, keyboard and mouse
  ([Plugin API](plugin-api.md))
- **Messages** between your scripts, your Lua and your C++ on named channels
- **The game's state** (main menu, loading, a menu, a scene, paused) and the controller's sticks and buttons
- **Values that go with the save**, values that stay across all saves, files, web requests and sockets
- **Texts from text files** (`key = text`), in every language the game has
- **The game's events** in Lua (`hit`, `death`, `item_added`, `level_up`, `fact`, `scene_start` and more)
- **Settings pages** in the game's **Options** > **Mods** and in the TW3SE Menu, from a few lines of Lua
- **Items and abilities** of your own, keys in the game's **Key Bindings**
- **World edits**: remove, move and change placed objects from a text file
- **Shaders** of the game replaced with your own, from files or from memory
- **Data of your own in the game's shaders**: a parameter on the game's root signatures, bound when the game draws
  your pipelines, and a moment each frame for your own GPU work (what mods took ReShade's add-on API for)

## While you build

- Scripts compile again while the game runs (about 5 seconds), Lua mods load again when you save a file, plugins
  load again from a new build
- A debugger for WitcherScript and Lua in VS Code: breakpoints, steps, variables
- A profiler for WitcherScript functions and Lua mods
- An inspector for any object in the world (**F2** > **Inspector** > **Pick**)
- A crash report after a crash: the function, the script line and the mod it happened in
- Safe mode (Shift held at the start): Lua mods and plugins stay off, to find the mod that breaks

## For your players

- Every `.asi` mod loads through TW3SE; another mod's ASI loader in `dinput8.dll` starts TW3SE too
- Mods made for Witcher Native Bridge run on TW3SE alone
- Every page in **Options** > **Mods** opens (patch 5.0 opens 10)
- After a game patch, mods that ask TW3SE for the engine's functions by name keep working; TW3SE finds them on the
  new build ([Addresses](addresses.md))

## Where to start

- A Lua mod: [Your first Lua mod](first-lua-mod.md)
- A C++ plugin: [Your first plugin](first-plugin.md), the examples in the SDK (`imgui_example.cpp` is a menu)
- An `.asi` mod or a mod made for Witcher Native Bridge: [Coming from another tool](coming-from.md)
