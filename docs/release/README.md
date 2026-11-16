# TW3SE (The Witcher 3 Script Extender)

TW3SE is a tool for mods. It adds new script functions and features to The Witcher 3 Remastered 5.0 and 5.01
(DirectX 12) and to the REDkit editor. Tested on Steam.

## Install

1. Unpack the zip into the game folder (the one with `bin` and `content`), or install it with Vortex.
2. Start the game as usual.
3. To check that it works, open `bin\x64_dx12\tw3se\tw3se.log`. The first line shows the version.

## REDkit

Copy `bin\x64_dx12\dinput8.dll` and the `bin\x64_dx12\tw3se` folder into REDkit's `bin\x64_RedKit` folder. Start the
editor as usual. The log is `bin\x64_RedKit\tw3se\tw3se.log`.

## Other mods with DLLs

- **.asi mods** stay in `bin\x64_dx12`. TW3SE loads them: it is their ASI loader.
- **Mods made for Witcher Native Bridge** run on TW3SE alone. With the bridge installed, both run side by side.
- **The Ultimate ASI Loader as `dinput8.dll`** (it comes with Witcher Native Bridge and other mods) may replace
  TW3SE's `dinput8.dll`. TW3SE starts all the same: that loader loads `!tw3se.asi` first.
- **Ultimate ASI Loader as `winmm.dll`**: it loads the .asi mods.
- **Another mod's `dinput8.dll`** (an input mod): rename it to `dinput8_chain.dll`. TW3SE loads it first.

## Update

Unpack the new zip over the old files. `tw3se.config`, `vars.txt` and `world.txt` stay.

After a patch of the game, TW3SE finds the game's functions again by their bytes and starts. A feature whose function
the patch changed stays off until an update of TW3SE; `tw3se.log` names it. When the patch changed one of the few
functions TW3SE needs to start, TW3SE stays off as a whole, and a message at the game's start says so (once
for each game version).

## Uninstall

Delete `dinput8.dll`, `!tw3se.asi` and the `tw3se` folder in `bin\x64_dx12` (and in `bin\x64_RedKit`).

## Bugs

Post in the Bugs tab what happened and attach a report:

- After a crash, or when the game closed by itself: start the game once more. TW3SE then writes
  `bin\x64_dx12\tw3se\out\crash_report.txt` (the mods, how the game ended and the log of that session).
- While the game runs: open the TW3SE Menu (F2), click **Console** > **Copy report** and paste the report into the
  post (it is in `tw3se\out\report.txt` too).

To see whether a Lua mod or a plugin of TW3SE causes a crash, hold Shift while the game starts (from the click on
**Play** until the first logo). TW3SE then starts in safe mode: no Lua mods, no plugins.

## For mod authors

The `TW3SE-SDK` file has the headers (with the ImGui TW3SE runs), four example plugins (an ImGui menu and an .asi
mod among them), a Visual Studio project to start from, an example script and the manual. Start with
`docs\manual\index.md`; mods made for another tool start with `docs\manual\coming-from.md`.

## Credits

- MaximdeWinter: idea, design, testing in the game and direction
- Tenour: the pose and mimic hooks (a figure's bones and face, changed every frame) and the live mesh deformation
  (blend shapes, `w3.deform`)
- Code written together with Claude

TW3SE uses:
- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu, with the Hacker Disassembler Engine by
  Vyacheslav Patkov (BSD 2-Clause)
- [Lua 5.4](https://www.lua.org) by Lua.org, PUC-Rio (MIT)
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut (MIT)
- [stb_vorbis](https://github.com/nothings/stb) by Sean Barrett (public domain or MIT)

The license texts are in `THIRD_PARTY_NOTICES.txt`.
