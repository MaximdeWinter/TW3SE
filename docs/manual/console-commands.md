# Console commands

TW3SE opens the pipe `\\.\pipe\tw3se`. Write one line, read one answer. Commands run on the game's main thread. Use
`tools\tw3se.py` or open the pipe yourself.

```
python tools\tw3se.py ping            # -> pong
python tools\tw3se.py "props thePlayer vital"
```

In the REDkit editor the pipe is `\\.\pipe\tw3se_redkit` (`python tools\tw3se.py --redkit <command>`). A game with
`pipe = <name>` in its `tw3se.config` has `\\.\pipe\<name>` (`python tools\tw3se.py --pipe <name> <command>`, or
the variable `TW3SE_PIPE`). If two games ask for the same name, the first one gets the pipe and the log of the
second one says `PIPE TAKEN`.

An answer starting with `error ` is a failure, anything else is the result. Plugins can add their own commands,
`help` lists all of them.

## General

| Command | What it does |
|---|---|
| `ping` | answers `pong` |
| `help` | all commands, TW3SE's and the plugins' |
| `version` | TW3SE version, interface version, game build |
| `plugins` | the loaded plugins |
| `report` | writes `tw3se\out\report.txt` for a bug report: versions, settings, plugins, Lua mods, the game's mods and DLCs, the script stack, the last Lua errors with tracebacks, the last 200 lines of `tw3se.log` |
| `luamods [reload <name>]` | the Lua mods; `reload` takes one out and loads it again |
| `plugin reload <name>` | unloads a plugin and loads its DLL again (see the Plugin API) |
| `luaprof [reset]` | the time each Lua mod spends per tick, by kind (`on_tick`, hooks, natives); `reset` starts again |
| `scriptprof on\|off\|reset\|[n]` | the WitcherScript profiler: `on` counts, `scriptprof` shows the n functions (20) with the most own time, with their mod |
| `lockeys` | TW3SE's own texts for the game's text keys (the Lua mods' option pages): how many, how often the game asked, how often TW3SE answered. `lockeys off` puts the game's own handler back (for a check: is a missing text TW3SE's doing?), `lockeys on` TW3SE's again |
| `crashtest [fastfail\|failfast_api\|ntterminate]` | crashes the game on purpose; the next start writes `tw3se\out\crash_report.txt` (`fastfail` ends the game past every handler, `failfast_api` and `ntterminate` the two ways the report names with their caller) |
| `ui open\|close\|state` | the in-game UI |

## Controlling the game from a tool

| Command | What it does |
|---|---|
| `exec <command>` | runs a console command like the script debugger does: any `exec function`, `addfact(...)` and so on. Several commands separated by byte 0x1f run in the same tick. Answers `ok <n>` or `error <k> of <n> failed: <command>` |
| `events [seq]` | the lines scripts sent after position `seq`. `events` alone gives the current position. Answer: `seq <last>` (and ` lost <n>` if lines were dropped), then one line each |

`events` gets the lines of `TW3SE_Emit(text)` and the `LogChannel` lines of the channels in `events_channels`
(default `Cj`). `LogChannel` needs `script_logs = on` (see [Installation](installation.md)). Every reader keeps its
own position, so several tools can read at once. TW3SE keeps the last 8192 lines. One answer holds about 3800
bytes, ask again if it is full.

```
python tools\tw3se.py events                # -> seq 41
python tools\tw3se.py exec "my_report(3)"   # an exec function that calls TW3SE_Emit("found 3")
python tools\tw3se.py events 41             # -> seq 42, found 3
```

The pipe takes several clients at once.

## Scripts and Lua

| Command | What it does |
|---|---|
| `reloadscripts force` | compiles all WitcherScript again and runs the new code at once. See [Live reload](live-reload.md) |
| `reloadscripts` | the state of the script system |
| `lua <code>` | runs Lua code and answers its output or result. See [Lua](../LUA.md) |
| `luafile <name>` | runs `tw3se\lua\<name>.lua` |
| `luatest` | runs the Lua tests in `tw3se\lua\tests\` and answers pass/fail |
| `catalog` | writes every type and function of the game to `tw3se\catalog\rtti.json` |

## DLC and quests

| Command | What it does |
|---|---|
| `livedlc <dlc folder>` | mounts and registers a DLC while the game runs. Its quests start after a save load |
| `mount <dlc folder>` | mounts the content of a DLC folder (the folder that holds `content\`) |
| `dlcscan [type]` | lets the game's DLC manager look for new `.reddlc` files |
| `state` | the game's and the DLC manager's flags |
| `questlive [on\|off]` | each quest block the game starts goes to `events` as `qb\|on\|<GUID>` |

A quest from a new DLC folder: copy the folder into `dlc\`, send `livedlc <path>`, wait about 2 seconds, load a save.

## Classes and functions

| Command | What it does |
|---|---|
| `natives` | the registered WitcherScript natives and their parameter counts |
| `props <global\|Class> [filter]` | field names and offsets of a class, into the log |
| `methods <global\|Class> [filter]` | method names of a class, into the log |
| `findfn <name>` | a global function (opcode, parameter count) |

`<global>` is one of `theGame`, `thePlayer`, `theCamera`, `theHUD`, `theSound`, `theDebug`, `theTimer`,
`theInput`, `theTelemetry`. `props thePlayer vital` logs the player's fields with "vital" in the name.

## Calling functions and fields

| Command | What it does |
|---|---|
| `callfn <fn> [args]` | calls a global native and answers the int result. Arguments: `i:5 f:1.5 s:text n:name b:1` |
| `callm <method> [arg]` | calls a method on `thePlayer` |
| `global <name> [field] [value]` | reads or sets a field of a global |
| `player [field] [value]` | reads or sets a field of `thePlayer` (needs a loaded world) |

`player vitality 1000` sets the field `vitality` to 1000.

## World and overlay

| Command | What it does |
|---|---|
| `worldedit <line>` | adds a world edit to `tw3se\world.txt` and applies it right away. See [World edits](world-edit.md) |
| `worldlist` | how many edits are loaded and applied, how many objects are loaded |
| `worldfind <d0> <d1> <d2> <d3>` | is this object loaded, its class and its effects |
| `worldremove`, `worldmove`, `worldeffect`, `worldappearance` | short forms of `worldedit remove ...` and so on |
| `playertp [x y z]` | teleports the player, or answers where the player is |
| `umbra [on\|off]` | the game's visibility culling. Off draws everything in view (costs frame time, for debugging) |
| `overlaytest on\|draw\|off` | a test overlay: `draw` shows a watermark, `off` removes it |

## When the game hangs

When the game's main thread stands for 8 seconds, TW3SE writes `tw3se\out\hang_<time>.txt`. It holds what each
thread does inside TW3SE, which code stopped threads (and which of them are still stopped), the last 2000 steps TW3SE
took and the stack of every thread. If the game still stands 30 seconds later, a second report follows. While a
save loads the time is that of the start below (180 seconds): loading stands the main thread longer than 8 seconds by
design. The newest 20 reports stay, older ones are deleted.
`hang_seconds = <s>` in `tw3se.config` sets the time (`0` turns the reports off), `hang_dump = on` adds a minidump.

A game that has not reached its first frame 180 seconds after TW3SE started gets one report too (a start that hangs).
`start_hang_seconds = <s>` sets that time, for loading a save too; `0` turns that report off.

The commands with `!` run on the pipe's own thread, so they answer while the game hangs.

| Command | What it does |
|---|---|
| `!status` | when the last tick was and what each thread does inside TW3SE |
| `!crumbs [n]` | the last n steps TW3SE took (default 40) |
| `!suspends [n]` | the last n times a thread was stopped or started again, with the code that did it (default 60) |
| `!dump` | writes a report now |
| `hangtest [s]` | holds the main thread for s seconds (default 12), so the watchdog writes a report |
| `guardtest` | one error inside TW3SE on purpose; the game has to go on |
| `poseitems [off\|data\|engine]` | how a watched figure finds the items that hang on it (`posehook_items` in `tw3se.config`), and what the search costs: entities known, waiting, the longest tick |

`tools\watch.py` watches the game from a process of its own and writes the same report into `build\watch\`. It
keeps working when every thread of the game stands. It needs `tw3se.pdb` next to `tw3se.dll`.

## Engine analysis

`help` also lists commands for looking into the engine (`enginedump`, `hwwatch`, `peek`, `fndump` and others). They
write to `tw3se.log` and `tw3se\out\`.
