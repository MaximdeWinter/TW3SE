# Debugger

TW3SE lets you debug WitcherScript and Lua in VS Code. You set breakpoints in your `.ws` and `.lua` files, step
through the code and look at the variables of each call while the game stands still.

## What is needed?

- VS Code
- `debugger = on` in `bin\x64_dx12\tw3se\tw3se.config`
- Script lines on (the default: TW3SE Menu > **Mods** > **TW3SE**); WitcherScript breakpoints need them
- The extension: copy the folder `tools\vscode-witcherscript` from the `TW3SE-SDK` zip to
  `%USERPROFILE%\.vscode\extensions\` and restart VS Code

## Start

1. Start the game.
2. Open the folder with your scripts in VS Code.
3. **Run and Debug** > **create a launch.json file** > **WitcherScript (TW3SE)**.
4. Set a breakpoint in a `.ws` file and press F5.

When the game reaches the line, it stops. VS Code shows the calls that led there, the variables of each call and
the object it runs on (`self`). F10 steps over a line, F11 into a call, Shift+F11 out of it, F5 lets the game go on.

The game stands still while it waits at a breakpoint. After a few seconds Windows marks its window as not
responding. The game goes on when you continue.

## Breakpoints

The game knows a script by its path below `content\scripts`, for example `game\player\r4player.ws`. A breakpoint in
`Mods\modMine\content\scripts\game\player\r4player.ws` and one in a copy of that file in your work folder are the
same breakpoint.

A breakpoint can have a condition (right click on the line number > **Add Conditional Breakpoint...**). It stops
only when the condition is true. A log point writes a message to the Debug Console and goes on; `{expression}` in
the message is filled in.

## Expressions

Conditions, the Watch list, hovers and the Debug Console take Lua expressions. The call's variables are there by
name, its object as `self`, and the whole TW3SE library:

```
n * 2
self.name
w3.player():GetLevel()
```

A variable in the Variables view can be changed with a double click. The script goes on with the new value.

## Lua

Breakpoints, steps and variables work the same in Lua mods and in files under `tw3se\lua`. A Lua file is known by
its path below the `tw3se` folder (`mods\mymod\init.lua`), so a copy in your work folder works too. VS Code shows
a Lua call's locals and upvalues. Code in a `w3.run` sequence runs past Lua breakpoints. While a Lua breakpoint is
set, every Lua mod runs a little slower.

## Script files

VS Code opens the files of the calls from these folders, the first match wins:

1. `scriptRoots` in `launch.json`
2. the folder open in VS Code (also its `scripts` and `content\scripts`)
3. the game's mods (`Mods\<mod>\content\scripts`), in load order
4. the game's DLC
5. the game's own scripts (`content\content0\scripts`)

```json
{
  "type": "witcherscript",
  "request": "attach",
  "name": "Attach to The Witcher 3",
  "port": 29170,
  "scriptRoots": ["C:\\Work\\modMine\\content\\scripts"]
}
```

`port` is `debugger_port` from `tw3se.config` (default 29170). The debugger listens on this PC only.
