# Your first Lua mod

A Lua mod is a folder with an `init.lua`. This page builds one from the template in the SDK, tries it in the game
and packs it for Nexus Mods.

## What is needed?

- The game with TW3SE
- The `TW3SE-SDK` zip
- An editor. VS Code with the extension **Lua** (by sumneko) completes `w3.` with every function of TW3SE.

## 1. Copy the template

Copy the `Mods` folder from `lua-mod-template` in the SDK into the game folder. The game then has:

```
Mods\modMyMod\tw3se\init.lua         the mod
Mods\modMyMod\tw3se\.luarc.json      for the editor
```

Rename `modMyMod` to the name of your mod. The game's mod folders start with `mod`.

## 2. Types for the editor

Open the folder `Mods\modMyMod\tw3se` in VS Code. `.luarc.json` points the Lua extension to
`bin\x64_dx12\tw3se\lua\types`, where TW3SE keeps `w3api.lua` with every `w3.` function.

For the game's classes, copy `types\w3.lua` from the SDK into the same folder. Then `w3.player():` lists every
method of the player with its parameters. The template brings snippets: type `w3mod`, `w3on`, `w3key`, `w3hook`, `w3tick`, `w3ask` or
`w3draw` and press Tab. After a game patch, the console command `catalog` and
`python tools\gen_types.py --game "<game folder>"` write a new `w3.lua`.

## 3. Try it

Start the game and load a save. Press F7 and the player heals. **F2** > **Mods** shows **My Mod** with its two
settings.

Change `init.lua` and save it while the game runs: the mod loads again within a second. `print(...)` writes a line
into `bin\x64_dx12\tw3se\tw3se.log`. Every Lua error is in `tw3se.log`, with its file and line. The mod's last error
stands at the mod in **F2** > **Mods**, and its first one shows a line over the game for 8 seconds.

## 4. What goes into the folder

| File | What it is |
|---|---|
| `init.lua` | runs at startup |
| `<name>.lua`, `<folder>\<name>.lua` | modules: `require("name")`, `require("folder.name")` |
| `natives\*.lua` | `w3.native(...)` for the mod's WitcherScript, before the scripts load |
| `files\...` | loose files under their depot path (see [Loose files](loose-files.md)) |

A mod with WitcherScript has its `content` folder beside `tw3se`, as every mod of the game has. The whole API is in
[Lua](../LUA.md), small complete mods are in `bin\x64_dx12\tw3se\lua\examples`.

## 5. On Nexus Mods

Pack the mod so that `Mods` is at the top of the zip:

```
modMyMod.zip
    Mods\modMyMod\tw3se\init.lua
    Mods\modMyMod\content\...        the WitcherScript, if the mod has some
```

`python tools\pack_mod.py modMyMod --game "<game folder>"` writes this zip. It leaves out `.luarc.json`, `.vscode`
and the other files only your editor reads. Players unpack it into the game folder. Name TW3SE
on the mod page as a requirement, with the version your mod was made with. The same version goes into `w3.mod`
(`tw3se = "1.2.0"`): on an older TW3SE the player then reads which version the mod needs.
