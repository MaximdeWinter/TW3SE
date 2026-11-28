# crashcatch

crashcatch writes a report with minidumps when The Witcher 3 crashes, and a report of the running game when something
looks wrong and you press Ctrl+F12. Start it before the game, play as usual, and send the zip it leaves.

It watches the game as a debugger. So it sees every fault of the game first, on every thread, before the game's own
crash handler: also a fail-fast, a stack overflow and a game that ends with exit code 0 after a crash.

## How to use it

1. Unpack `crashcatch.exe` into any folder.
2. Start `crashcatch.exe`. A window opens and waits for the game.
3. Start the game as usual. crashcatch finds it and says "watching witcher3.exe".
4. Leave the window open while you play.

After a crash, crashcatch shows the one file to send (`reports\witcher3_<date>_<time>.zip`) in its window and in a
message, opens its folder and copies the file: Ctrl+V pastes it into Discord, a forum post or a mail.
After a normal end crashcatch deletes what it wrote and waits for the next start of the game.

Something looks wrong and the game runs on (a texture that is missing or black, a broken mesh, a figure that stands
still): press **Ctrl+F12** in the game while it is on screen. crashcatch takes a picture of the game, collects what
the mods say about themselves and opens a window that asks what is wrong. Write a sentence and click **Save the
report**. The game goes on, and crashcatch shows the zip to send.

Keys in the crashcatch window:

- **D** does the same as Ctrl+F12
- **Q** stops watching. The game goes on

If the game runs as administrator, start crashcatch as administrator too.

With Steam, crashcatch can start with the game: in the game's **Properties** > **Launch Options** write
`"C:\path\to\crashcatch.exe" %command%`. crashcatch then starts the game, watches it and closes after it.

With Mod Organizer 2: add `crashcatch.exe` in MO2 as an executable and start it from there. It then sees the files
MO2 gives the game, `tw3se.log` among them.

## What is in the zip?

- `report.txt`: how the game ended (the exit code and what it means), each fault with its place, the registers, the
  code around it and the stack, the modules with their versions, the graphics settings, the system
- on top of `report.txt`, what the last fault likely was, in plain sentences (a null pointer, an object used after
  its memory was given back, a pointer that holds data, an id of 0, out of memory), and the fault's place as its
  fingerprint
- at each fault, the function of each code address and the class of each object a register points at (read from the
  game's type information)
- the Windows events while the game ran: graphics driver resets, hardware errors (WHEA), low memory, the game's own
  crash entries
- `memory.csv`: Windows' free memory, the game's memory and the graphics memory every 5 s, with a summary in
  `report.txt`
- the graphics cards with their memory and driver version, the folders in `Mods` and `dlc`, and another copy of the
  game when one ran at the same time
- TW3SE's log of the 15 seconds before the fault, with the script stack TW3SE writes at a fault
- at a hang, which thread waits on which lock held by which thread; a deadlock is named
- `mods_manifest.txt`: every file in `Mods`, `dlc` and the plugin folders with size, date and a fingerprint, to set up
  the same mods at home
- `dred.txt` after a GPU crash, when TW3SE's `dred` is on (see below)
- the newest save of the game, to load the same moment at home (`--nosave` leaves it out)

A report by Ctrl+F12 or **D** also has:

- on top of `report.txt`, what the player wrote, the mods' checks that failed and the mod files that differ from
  what the mod shipped
- `screenshot.jpg`: the game's window at the moment of the key (black when the game runs in exclusive full screen)
- the checks of each mod that takes part (see For mod authors), and `mods_state.txt` with each mod's state
- `tw3se_report.txt`: TW3SE's own report (versions, plugins, Lua mods, settings, the end of its log)
- each mod's files against its `crashcatch_files.txt`: files missing, changed, or of another version

The zip stays below 18 MB, so it fits into Discord (20 MB a file) and a mail. When it would be larger, crashcatch
leaves out the save first, then the older parts of the logs, then the dumps other than the one of the crash. What
it left out stays next to the report in `<report>_not_sent`, and `report.txt` lists it.
- `fault_<n>.dmp`: a minidump at each fault, four in a session at most
- `hang_<n>.dmp`: a minidump when the game's window stands still for 20 s
- `now.dmp`: the minidump of a report by Ctrl+F12 or **D**
- `tw3se.log`, `session.txt` and the hang reports of TW3SE, when TW3SE is installed
- `user.settings`, `dx12user.settings`, `mods.settings` and `scriptslog.txt` from Documents\The Witcher 3

`report.txt` writes your user folder as `%USERPROFILE%`. The dumps and logs contain file paths of your PC, your
user name among them.

## Options

```
crashcatch.exe [game.exe] [--game folder] [--pid N] [--keep] [--full] [--quiet] [--diagnose]
crashcatch.exe --bisect <game folder> [--minutes N]
crashcatch.exe --analyze <report folder | .zip | .dmp> [--images <folder>]
crashcatch.exe --summary <folder with reports>
crashcatch.exe --checkfiles <game folder>
crashcatch.exe [options] <path of the game's exe> [its arguments]
```

- `game.exe`: the process to wait for, `witcher3.exe` by default. crashcatch works with other games too
- `--game <folder>`: watch only a game whose exe lies in this folder
- `--pid N`: watch this process (when two games of the same name run)
- `--keep`: keep the dumps after a normal end too
- `--full`: full memory dumps (several GB each)
- `--quiet`: for scripts (the window closes by itself, the report folder stays closed)
- `--nosave`: the report goes without the save
- `--deep`: deep mode (see below)
- `--watch <place>[:length][:w|rw]`: tells who writes (or reads) a place in memory, with the stack of each access;
  the place as `witcher3.exe+0x5a56ef8` or an address, up to four
- the path of the game's exe as the last argument: crashcatch starts the game with the arguments after it
- `--checkfiles <game folder>`: checks each mod's files against its `crashcatch_files.txt` and prints the result,
  also with the game closed (exit code 1 when a file differs)

A fault the game handles itself also gets a dump; the game stands about a second while it is written. Ends with
exit code 0 within 10 s after a fault count as a crash.

## Diagnose round

`crashcatch.exe --diagnose` (or **diagnose.bat**) finds the part that makes the game crash. Each start of the game
runs with one line of `diagnose.txt` changed, the line played least so far. After each end crashcatch shows a table:
starts, minutes and crashes for each line. Play as usual over several starts; a line whose crashes stop is the
cause.

`diagnose.txt` lies next to `crashcatch.exe` (crashcatch writes it at the first `--diagnose`). A line is a name, a
colon and the changes, separated by `;`:

```
no pose hook: tw3se.config posehook = off
no path tracing: dx12user.settings [Rendering/RT/PathTracer] PTEnable = false
```

`tw3se.config` is TW3SE's file in `bin\x64_dx12\tw3se`, `dx12user.settings` the game's settings in Documents\The
Witcher 3. crashcatch puts every change back when the game ends and when you close it with **Q** or the window's X.
After crashcatch was ended another way, its next start puts them back. The first start of a round runs as
installed: crashcatch learns there where the game is. The table stays in `reports\diagnose_state.txt` and goes into
every report.

## Finding the mod

`crashcatch.exe --bisect <game folder>` (or drag the game folder onto **bisect.bat**) finds the mod a crash comes
with. Each start of the game runs with half of the mods still suspected. The others wait in
`<game folder>\crashcatch_parked` (a move on the same drive).

- The game crashes: the culprit is in the half that was on
- The game runs 20 minutes (`--minutes N` sets another time) and you end it: the culprit is in the other half
- A shorter start that ends normally is played again with the same half

After a few starts one mod is left, and crashcatch writes it to `reports\bisect_result.txt`. The mods are the folders
in `Mods` and `dlc`; the game's own DLC stay. Mods that need each other go on one line of `bisect.txt` next to
`crashcatch.exe`, they are switched off and on together:

```
modMyArmor modMyArmorPatch dlcmyarmor
```

crashcatch moves every mod back when the search ends and when you close it with **Q** or the window's X. After
crashcatch was ended another way, its next start moves them back and goes on with the search. Close the game before
you start a search. Under Mod Organizer 2 the mods lie in MO2's own folder: halve them in MO2.

## A report afterwards

`crashcatch.exe --analyze <folder>` reads the dumps of a report again and writes `analysis.txt` into it: the
sentences, the classes and the function names as at the fault, and the stacks of `report.txt` and TW3SE's
`crash_report.txt` with names. A zip is unpacked next to itself first.

The code of the game comes from its files on this PC. For a report from another PC, `--images <folder>` names a
folder to look in, the game's `bin\x64_dx12` for example. A file is used only when it is the build of the dump.

## Many reports at once

`crashcatch.exe --summary <folder>` reads every report in a folder (folders and zips) and writes `summary.txt`:
the reports grouped by the place of their crash, with how many came from how many PCs, the known cause, the
graphics cards, the TW3SE versions, the mods every report of a group had, and the mods only that group had.

## Deep mode

When a crash stays unclear after a normal report, start **deep.bat** in place of `crashcatch.exe`. For this session
TW3SE (1.3.3 and later) also checks freed memory and GPU crashes; the game may run a little slower. crashcatch
switches both off again when you close its window, and at its next start when it was ended another way. Deep mode
sets the two switches below in TW3SE's `tw3se.config`; they can also be set by hand.

## TW3SE switches for debugging

Two lines in TW3SE's `tw3se.config` give crashcatch more to say. Both cost a little speed and are meant for a few
sessions with a crash, then off again.

- `quarantine = poison`: freed memory of the game is held back for 5 seconds and marked. A use after free then
  crashes at once where the freed memory is used, and `tw3se.log` names which free it was, with its stack.
  `quarantine = hold` holds it back unmarked: a crash that stops with it is a use after free
- `dred = on`: after a GPU crash ("device removed") TW3SE writes `tw3se\out\dred.txt`: the GPU work still
  running at the crash and the address the graphics card faulted on

## Known crashes

`known_crashes.txt` next to `crashcatch.exe` holds crashes whose cause is known. A line is text that stands in the
fault (its place, a function or class name), a `|`, and what it is:

```
witcher3.exe+0x209e18a | A crash seconds after a scene's end ... Fixed in TW3SE 1.3.2.
```

The first line that fits the last fault goes on top of the report and of `analysis.txt`. A place holds for one build
of the game.

TW3SE 1.3.3 and later write the names of the engine functions they know to `tw3se\out\names.txt`; crashcatch uses
them for the function names.

## For mod authors

Your mod can put its own state and checks into every report by Ctrl+F12, and crashcatch can check that its files are
the ones you shipped. Both are optional.

**State and checks** (a TW3SE Lua mod). Add an entry to the table `TW3SE_REPORT`:

```lua
TW3SE_REPORT = TW3SE_REPORT or {}
TW3SE_REPORT["My Mod"] = {
  version = "1.2",
  state = function() return {mode = "on", items = 3} end,
  checks = function()
    return {
      {name = "the textures load", ok = w3.load([[my_mod\textures\a.xbm]]) ~= nil, why = "the bundle is not loaded"},
    }
  end,
}
```

- `state` returns anything. A table is written out with its keys, five levels deep, into `mods_state.txt`
- `checks` returns a list of checks. `why` is shown when `ok` is false. A failed check goes on top of `report.txt`
- crashcatch calls both through TW3SE's pipe when the player presses Ctrl+F12. An error in one of them is written
  into the report and the other mods go on

**Your files.** Ship a file `crashcatch_files.txt` in your mod's folder (`Mods\<your mod>` or `dlc\<your dlc>`). The
first line is a `#` with the name and version, then one line per file your mod puts into the game folder: its
SHA-256, its size in bytes and its path from the game folder.

```
# My Mod 1.2
9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08 4096 Mods\modMyMod\content\blob0.bundle
60303ae22b998861bce3b28f33eec1be758a213c86c93c076dbe9f558c11c752 1873 bin\x64_dx12\tw3se\plugins\my_mod.dll
```

crashcatch reads it from the disk, so it also checks a mod that failed to load. It lists each file that is missing,
of another size or of other content.

## Build

`build.bat` with the Visual Studio 2022 Build Tools (x64) makes `build\crashcatch.exe`.
