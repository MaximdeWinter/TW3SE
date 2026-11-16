# Live reload

Change a WitcherScript file while the game runs, reload, and the new code runs. The game keeps its
state: objects of your scripts keep their values and the world stays as it is.

## Reload

1. Save your changed `.ws` file in `Mods\<your mod>\content\scripts`.
2. Send `reloadscripts force` through the pipe:

```
python tools\tw3se.py reloadscripts force
```

The answer when it worked:

```
reloadscripts: ok - ok, 1 file(s) changed - #1 in 4805 ms
```

The game stands still while it compiles all scripts (a few seconds).

## What carries over

- Objects your scripts made stay, with the values of their fields
- A field you added to a class is there on the objects that already exist, with its default value
- Timers keep running
- Changed functions run with their new code at their next call

## Compile errors

The answer names each error with file and line:

```
reloadscripts: failed - compile errors, the last good scripts run again | [mymod]local\mymod.ws(6): syntax error, unexpected ';', near ';'
```

The game keeps the scripts of the last reload that worked. Your file stays as you wrote it: fix it and reload again.

TW3SE keeps a copy of every `.ws` file in `Mods\` and `dlc\`, from the game start and after every reload that
worked. After a failed compile it puts these copies back for one more pass, then your edited files return to disk.
This takes as long as a second reload.

Caution: the copies cover `Mods\` and `dlc\`. A broken file in `content\content0\scripts` (the game's own
scripts) stops all scripts after a reload. Restart the game after such a change.

## Code that runs after a reload

A reload swaps the code. Init functions ran at the save load, so a new line in one (a `RegisterListener` for a new
key) takes effect after the next save load. Register an event that TW3SE calls after every reload, and put such lines
into it:

```
import function TW3SE_OnScriptsReloaded(obj : IScriptable, eventName : name);

class MyModInput extends CObject {
    public function Init() {
        theInput.RegisterListener(this, 'OnMyKey', 'MyKey');
        TW3SE_OnScriptsReloaded(this, 'OnMyReload');
    }

    event OnMyReload() {
        theInput.RegisterListener(this, 'OnMyKey', 'MyKey');
        theInput.RegisterListener(this, 'OnMyNewKey', 'MyNewKey');
    }
}
```

The same object and event again replace the entry, so `Init` can run at every load. A destroyed object drops out.
The game's own `OnScriptReloaded` event reaches the game's NPCs and leaves a mod's objects out. In Lua,
`w3.on_reload(fn)` does the same.

## Good to know

- A reload takes every `.ws` file in the `Mods\` and `dlc\` folders that were there at the game start. A mod
  folder added later comes in with the next start
- `reloadscripts` alone answers the state of the script system and how many files TW3SE keeps as the last good
  state
- Every reload uses about 30 MB more memory until the next start
- Each step of a reload is in `tw3se.log` (`reload:` lines), with its time
- In the REDkit editor, **Reload scripts** in the editor does the same
