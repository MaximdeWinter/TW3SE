# TW3SE Manual

TW3SE adds new script functions and features to The Witcher 3 Remastered and to the REDkit editor. Mods use it in
five ways:

| Layer | You write | What you get |
|---|---|---|
| **Lua** | `.lua` scripts | read and write any object's fields, call any game function, show text and boxes over the game, run each tick |
| **WitcherScript** | `.ws` scripts | new functions: files, saved variables, hotkeys, objects by GUID, placing trees; live reload |
| **World edits** | `tw3se\world.txt` | remove, move or change placed objects, switched by quest facts |
| **C++ plugins** | a `.dll` or your `.asi` | own script functions, engine hooks, object fields, game function calls, ImGui windows, pose hook |
| **External tools** | a pipe client | commands to the running game |

Lua reaches the most of the game with the least setup.

## Pages

- [For mod authors](for-mod-authors.md): what TW3SE gives your mod
- [Coming from another tool](coming-from.md): Witcher Native Bridge, .asi mods, an ImGui overlay of your own
- [Quickstart](quickstart.md): install and check that it runs
- [Installation](installation.md): files, REDkit, other DLL mods, settings
- [Your first Lua mod](first-lua-mod.md): from the template to the zip for Nexus Mods
- [Lua](../LUA.md): read, write, call and draw from Lua; mods as folders
- [Lua API](../api.html): every `w3.` function on one page, with a search field
- [Script natives](script-natives.md): the new WitcherScript functions
- [Live reload](live-reload.md): new script code while the game runs
- [Debugger](debugger.md): breakpoints, steps and variables for WitcherScript and Lua in VS Code
- [TW3SE Menu](ingame-ui.md): the mods with their settings, console, inspector and Lua windows in the game (F2)
- [World edits](world-edit.md): change placed objects with a text file
- [Loose files](loose-files.md): replace game files with files in a folder, change them with `tools\cr2w`
- [Shaders](shaders.md): dump the game's shaders and replace them
- [Blend shapes](blend-shapes.md): move a mesh's vertices with weighted shapes from Lua
- [Your first plugin](first-plugin.md): build a plugin step by step
- [Plugin API](plugin-api.md): every function a plugin gets
- [Addresses](addresses.md): the engine's functions a plugin gets by name
- [Console commands](console-commands.md): the pipe and its commands
- [How it works](how-it-works.md): loader, hooks, the engine's types

## Version

TW3SE 1.2.1, plugin interface 21. Tested in the game on Remastered 5.0 (build `0x6abd8695`) and 5.01 (build
`0x6ac555a3`), Steam, and in the REDkit editor (build `0x6ac574c5`).

New interface fields are appended at the end. Plugins built for an older version keep working.
