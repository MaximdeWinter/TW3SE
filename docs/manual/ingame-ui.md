# TW3SE Menu

The TW3SE Menu opens over the game with F2: your mods with their settings, a console and an inspector. Mods can add
a tab of their own. F2 again closes it.

The key is a setting of the menu itself: **Mods** > **TW3SE** > **Menu key**, click it and press the new key (a
combination like Ctrl+F2 works too). It counts at once and is kept. `console_key` in `tw3se.config` sets the key
until it is changed in the menu. TW3SE's entry also switches its fixes for the game: **Game window in front at the
start** and **Fix the game's Options > Mods**.

The first press takes a few seconds. While the UI is open, keyboard and mouse go to it and Geralt stands still.

With a controller, hold both sticks pressed in (**Pad_LS** and **Pad_RS**) for half a second to open or close the
menu. The D-pad and the left stick move between the fields, A clicks, B goes back. The game gets the controller too while
the menu is open. `menu_pad` in `tw3se.config` sets another combination.

## Mods

The **Mods** tab lists every mod: Lua mods, plugins and the WitcherScript mods in the game's `Mods` folder. The
search field above the list finds mods by name, author and the names of their settings.

Click a mod to see its version, author and settings on the right. A change counts at once and is kept:

- Mods with a page in the game's options menu (**Options** > **Mods**) show the same settings here. TW3SE writes
  them to the game's settings file, as the menu does.
- Lua mods show the settings they declare with `w3.mod` (see [Lua](../LUA.md)). **Reset to defaults** puts them
  back. A key setting waits for the next key you press (Esc keeps the old one).

A mod with an error (while it loaded or while it ran) has `(!)` before its name; its last error shows on the right.

**Conflicts** lists the files that more than one mod in `Mods` brings: scripts (`content\scripts`) and game files
(the `content` bundles), with the mod that counts. That is the mod with the lowest **Priority** in
`Documents\The Witcher 3\mods.settings`, else the first by name. **Let ... win** gives a mod a lower Priority than
the others of its conflict, from the next start; TW3SE keeps the old file as `mods.settings.tw3se_backup`. Vortex
may set its own order again at its next deploy.

**New mod** makes a mod of your own: a name, and the template or one of the examples to start from. TW3SE writes
`Mods\mod<Name>\tw3se\init.lua`, the mod loads within a second, and the file opens in an editor window. **Save**
(or Ctrl+S) loads the mod again; the line below the text shows its error or **Loaded**. A Lua mod's **Edit** opens
its `init.lua` the same way, **Open folder** shows its folder in Explorer.

## Console

The **Console** tab runs Lua and console commands. Type Lua into the line at the bottom and press Enter. The result
shows above:

```
return w3.player():GetMoney()
```

A line starting with `/` is a console command (the same as on the pipe):

```
/version
/reloadscripts force
```

Up and Down bring back earlier lines. **Clear** empties the output.

## Inspector

The **Inspector** box in the console opens it. It shows every field of an object with its type and value, starting
with the player.

- **Object (Lua)** takes any Lua expression that gives an object (`w3.near(10)[1]`, `w3.global('theGame')`)
- **Pick** puts a ring on the object nearest to the mouse in the world (up to 60 m away); a click opens it, Esc stops
- **Filter** shows the fields whose name contains the text
- Numbers, checkboxes, texts and enums change the field when you press Enter or click
- A blue button in the value column opens that object; the bar above leads back
- Arrays open with the small triangle
- **Methods** lists the class's methods; **Call** runs one of the parameterless ones

From the console: `require('lib.inspector').look('w3.near(10)[1]')`.

## Profiler

The **Profiler** box opens a window with the time each Lua mod spent over the last second: per kind (`on_tick`,
`on_draw`, `hook <function>`, `native <function>` …), in milliseconds per tick, with the number of calls and the
slowest call. Rows above 1 ms per tick are red. The console command `luaprof` gives the same since the start.

The **WitcherScript** tab counts the time of every script function while **Count** is on: its own time (the time in its
own lines), its whole time and its calls, in ms per second of play, the most first, with the
mod its script file belongs to. A function above 2 ms per second is red. This finds the WitcherScript mod that
makes the game stutter. Counting costs a little itself; **Stop** ends it, the numbers stay until **Reset**. The
console command `scriptprof on`, `scriptprof` and `scriptprof off` do the same.

## Bug reports

**Copy report** puts everything a bug report needs on the clipboard: versions, settings, plugins, Lua mods, the
game's mods, the script functions running, the last Lua errors with their tracebacks, the end of `tw3se.log` and the
head of the last crash report (how that session ended, its faults, what Windows saw). The
same text goes to `tw3se\out\report.txt`; the console command `report` and `w3.report()` write it too.

After a session that ended in a crash, was ended from outside or closed because the scripts failed to compile, the
next start writes `tw3se\out\crash_report.txt`: how that session ended, the first faults with their module and
offset, the function each lies in (its start is the same for every player of a game version, so two reports with
the same start show the same bug; a native of the script system or an engine function TW3SE knows with its name),
the Lua mod that ran at the fault, what Windows logged for the crash when it logged one (module, exception
code and offset from the Application log), the mods and DLCs, and that session's `tw3se.log`. The console command
`crashtest` crashes the game on purpose, to see one.

Shift held while the game starts (or `safe_mode = on` in `tw3se.config`) starts TW3SE in safe mode: no Lua mods
(with their `natives` and `files`) and no plugins, and a line over the game says so. If the game crashes the same way then, the cause lies elsewhere.
A WitcherScript mod that imports a function of a plugin or of a Lua mod's `natives` folder still compiles: in safe
mode that function does nothing and returns its default value. TW3SE knows these functions from the last start
without safe mode.

## Windows and tabs of your own

A Lua mod adds a window with `w3.on_ui` and the `w3.ui` functions, see [Lua](../LUA.md). `lua\examples\ui_window.lua`
shows one with text, a button and a slider. `w3.mod{tab = fn}` puts a tab of its own into TW3SE's window.

## Settings

In `tw3se.config`:

```
console_key = 0x71     # the key, a virtual-key code (0x71 = F2)
console = off          # no in-game UI
```

The UI uses Dear ImGui (MIT). It draws like the overlay and works with DLSS and FSR Frame Generation.
