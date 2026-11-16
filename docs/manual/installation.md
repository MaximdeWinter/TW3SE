# Installation

## Files

Unpack `TW3SE-1.2.1.zip` into the game folder (the one with `bin` and
`content`):

```
bin\x64_dx12\dinput8.dll           the loader
bin\x64_dx12\!tw3se.asi            TW3SE's start under another mod's ASI loader
bin\x64_dx12\tw3se\tw3se.dll       TW3SE
bin\x64_dx12\tw3se\compat\         Witcher Native Bridge's functions, for the mods made for it
bin\x64_dx12\tw3se\plugins\        plugins (optional, create it for your first one)
```

Start the game as usual. TW3SE writes `bin\x64_dx12\tw3se\tw3se.log`.

To uninstall, delete `dinput8.dll`, `!tw3se.asi` and the `tw3se` folder.

## Vortex

Vortex installs TW3SE into `bin\x64_dx12` from 1.2.0 on, with a second `dinput8.dll` in the game folder (unused).
TW3SE 1.0.1 to 1.1.0 landed in `Mods\bin`; the game loads its DLLs from `bin\x64_dx12`. Remove the old version in
Vortex and install the current version.

If Vortex says "Failed to set game mode" for The Witcher 3 and the **Mods** tab is missing: its Witcher 3 extension
looks for `bin\x64\witcher3.exe`, the folder of the old DX11 game. Make the folder `bin\x64` and copy
`bin\x64_dx12\witcher3.exe` into it, then **Games** > **The Witcher 3** > **Activate**.

The zip has a `fomod` folder for Vortex. Unpacked by hand, it can be deleted.

## Mod Organizer 2, RMMR, Linux

- **Mod Organizer 2**: the game loads `dinput8.dll` before MO2's virtual folder is in place. Copy `dinput8.dll`,
  `!tw3se.asi` and the `tw3se` folder into the real `bin\x64_dx12`.
- **Relapse's Mod Manager Redux** and the Witcher 3 Mod Manager: install the zip as a mod, or unpack it by hand.
- **Linux and Steam Deck (Proton)**: Proton uses its own `dinput8.dll` unless told otherwise. Set the game's launch
  options in Steam to `WINEDLLOVERRIDES="dinput8=n,b" %command%`. With another override already there (OptiScaler:
  `dxgi`), list both: `WINEDLLOVERRIDES="dinput8,dxgi=n,b" %command%`.

## REDkit

Put the same files into REDkit's `bin\x64_RedKit` folder:

```
bin\x64_RedKit\dinput8.dll           the loader
bin\x64_RedKit\tw3se\tw3se.dll       TW3SE
bin\x64_RedKit\tw3se\plugins\        plugins (optional)
```

Start the editor as usual. TW3SE writes `bin\x64_RedKit\tw3se\tw3se.log` and opens the pipe `\\.\pipe\tw3se_redkit`,
so the editor and the game can run side by side. TW3SE loads in the editor only. The other REDkit programs (wcc_lite, the bundle builder,
the launcher) run as they are.

In the editor: the script functions, Lua, world edits, tree placement, the catalog, `exec` and `events` and the quest block
events. A project's scripts go into `<project>\workspace\scripts\local`, with **Use loose scripts in
mod** switched on in the project settings. REDkit saves scripts as UTF-8 with BOM and offers to convert other files.

TW3SE has the addresses of the editor build `0x6ac574c5`. A REDkit update needs a new TW3SE.

## How it loads

The game loads `dinput8.dll` from its own folder. The loader passes the game's input calls to the system's
`dinput8.dll` and starts `tw3se\tw3se.dll`.

## Other mods with DLLs

- **.asi mods**: put them in `bin\x64_dx12`, `scripts\` or `plugins\`. TW3SE loads them and calls their
  `InitializeASI`: TW3SE is their ASI loader. The log lists each one (`asi: ... loaded`).
- **Mods made for Witcher Native Bridge** (Wolven Trainer, Dynamic Grass, Beardmo's Immersive World and more) run on
  TW3SE alone: TW3SE has the bridge's functions. With the bridge installed, both run side by side. The log says
  which one serves them (`native bridge: ...`).
- **The Ultimate ASI Loader as `dinput8.dll`** (it comes with Witcher Native Bridge and other mods): installed after
  TW3SE it replaces TW3SE's `dinput8.dll`. TW3SE starts all the same: that loader loads `!tw3se.asi` first, and TW3SE
  starts from there. The log says `asi: TW3SE started through tw3se.asi`. Each `.asi` loads once.
- **Ultimate ASI Loader as `winmm.dll`**: it loads the .asi mods, TW3SE leaves them to it. Two Ultimate ASI Loaders
  (one as `winmm.dll`, one as `dinput8.dll`) load every `.asi` twice; the log warns (`asi: WARNING - 2 Ultimate ASI
  Loaders`). Keep one.
- **Another mod's `dinput8.dll`** (an input mod): rename it to `dinput8_chain.dll`. TW3SE loads it first and passes
  the game's input calls to it.
- **Mods that hook game functions** (frame generation, ReShade, fixes) use other file names and run alongside. If
  one hooked a function before TW3SE, both hooks run. The log says `hooked by another mod before TW3SE`.

## Tested with

These mods ran together with TW3SE (scripts compiled, `luatest`, live reload):

- Free Transmog Anywhere 2.3.0
- More Skill Slots 1.1.3
- Responsive Movement 1.7.5
- Useful Monster Trophies Remaster 1.1.2
- The Living Continent 1.5
- Sharedutils - All In One 4.0

## Game versions

TW3SE has the addresses for build `0x6abd8695` (Steam, Remastered 5.0). On other builds it searches for each
function itself. If one is missing, TW3SE stays passive and the log names it. .asi mods still load.

## Settings

`tw3se\tw3se.config` is optional. One `key = value` per line, `#` starts a comment.

```
# TW3SE's own script functions (default: on)
natives = off

# compile the game's scripts with the line of each statement, as with -debugscripts (default: on; also in the
# TW3SE Menu > Mods > TW3SE). File and line in bug reports, the debugger's breakpoints and w3.on_line need it
script_lines = off

# keep Log/LogChannel in compiled scripts, as with -debugscripts (default: off; script_lines keeps them too)
script_logs = on

# compile errors at the game's start into tw3se.log (default: on)
compile_messages = off

# a message box when the game's scripts do not compile, with the first errors and the mods they come from
# (default: on)
script_error_message = off

# load a Lua mod again when one of its files is saved (default: on)
lua_autoreload = off

# a line over the game for 8 s at the first error of a Lua mod (default: on)
error_notice = off

# stop Lua code that runs longer than this many seconds at a time, as an error of its mod (default: 5, 0 turns it
# off; the console's lua, luafile and luatest run as long as they need)
lua_time_limit = 10

# the memory all Lua mods may use together, in MB (default: 1024, 0 turns it off)
lua_memory_limit = 4096

# safe mode: no Lua mods and no plugins (default: off; Shift held while the game starts does the same once)
safe_mode = on

# the message at the start when TW3SE does not support the game's version yet, once per version (default: on)
passive_message = off

# load a file saved in tw3se\files anew at its next use (default: on)
files_watch = off

# run plugins from a copy, so `plugin reload` can load a new build (default: on)
plugin_reload = off

# write tw3se\out\hang_<time>.txt when the game stands this many seconds (default: 8, 0 turns it off); with a
# minidump (default: off)
hang_seconds = 0
hang_dump = on

# write every shader of the game to tw3se\shaders\dump (default: off)
shader_dump = on

# LogChannel channels that the `events` command hands out (default: Cj)
events_channels = Cj, MyMod

# the TW3SE Menu: its key by name (default: F2; a key set in the menu, Mods > TW3SE > Menu key, wins), or off
console_key = Ctrl+F2
console = off

# the TW3SE Menu with a controller: buttons held together for 0.5 s (default: Pad_LS+Pad_RS, both sticks), or off
menu_pad = Pad_LB+Pad_RB+Pad_Y

# the console pipe's name (default: tw3se, in REDkit tw3se_redkit). A second game install that runs at the same
# time needs its own name; tools reach it with tw3se.py --pipe tw3se_bg or the variable TW3SE_PIPE
pipe = tw3se_bg

# overlay for plugins that draw and for the TW3SE Menu (default: auto, on when something draws; off: the menu stays closed)
overlay = off

# when frame generation is switched while you play, the game makes a new swap chain: the overlay is made anew on
# it (default: rebuild), or goes off until the next start (off). With rebuild, switching XeSS frame generation
# hangs the game
overlay_new_swapchain = off

# the pose hook for plugins (default: auto, installed when a plugin asks for it)
posehook = off

# log class and GUID of every placed object, to find GUIDs for world edits (default: off)
worldedit_log = on

# load .asi mods (default: on)
asi = off

# stand in for Witcher Native Bridge when it is not installed (default: on)
native_bridge = off

# TW3SE's fix of the game's Options > Mods list (default: on; it stays out by itself when another mod fixes it)
mods_menu_fix = off

# a watchdog report when the game has not drawn its first frame this many seconds after TW3SE started (default: 180)
start_hang_seconds = 0

# the game window gets the keyboard and controller when it appears at the start (default: on; a window you switched
# to while the game loaded keeps them)
focus_at_start = off

# the WitcherScript debugger for VS Code (default: off) and its port on this PC (default: 29170)
debugger = on
debugger_port = 29170
```

The game keeps its compiled scripts in `content\content0\x64.final.redscripts` and compiles them again only when a
script changes. When `script_lines` is switched, TW3SE has the game compile them again at the next start (that start
takes a few seconds longer). After switching `script_logs`, delete that file yourself. With `script_lines` or
`script_logs` on, every `LogChannel` of every script builds its text (same cost as `-debugscripts`).

With `script_lines` the compiler also checks every `Log` and `LogChannel` call. When a mod's script fails only that
check, TW3SE compiles the scripts once more without lines and the game starts as it would without TW3SE.
`tw3se.log` names the error (`scripts: the scripts did not compile with statement lines`). The lines stay off until
a script changes. When the scripts do not compile at all, TW3SE puts the game's compiled scripts back where they
were.

An error with "already defined" or "already exported" means two mods declare the same struct, class or property.
Its line in `tw3se.log` ends with the files that declare it (`declared in Mods\modA\..., Mods\modB\...`). Keep it in
one of them. TW3SE looks for these at the start, before the game compiles: `scripts: WARNING - ... both declare ...`
names the two files and what they both declare. When the scripts do not compile, a message box shows the first
errors and the mods they come from (`script_error_message = off` leaves it out).

Two mods that bring the same game script file (for example both `game\player\playerwitcher.ws`): the game takes
one of them, and the other mod's changes to that file are missing. Its scripts then fail with "Could not find
function". `scripts: WARNING - modA and modB both bring ...` names them at the start. Script Merger makes one file of
them (in `mod0000_MergedFiles`); with that folder there, TW3SE leaves this check to it.

The log also names each compiled-scripts file the game loads: its own cache, and the scripts of REDkit and mod.io
mods (`scripts: compiled scripts loaded: ...`). When one does not fit the game's scripts, the game takes it out and
closes about half a minute later. `scripts: compiled scripts did NOT load: ...` names that mod, and the crash report
at the next start does too. The crash report and **Copy report** list the mod.io and Steam Workshop mods.
