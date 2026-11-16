# How it works

For plugin authors who want to work below the helpers.

## Loader

The game loads `dinput8.dll` from its own folder. The loader passes the input calls to the system's `dinput8.dll`
and starts `tw3se\tw3se.dll`. TW3SE finds its engine addresses, sets up its hooks, opens the pipe and loads the
plugins. If a step fails, the game runs as usual.

When another mod's ASI loader sits in `dinput8.dll`, it loads `!tw3se.asi` first (the "!" sorts it before every
other `.asi`), and `!tw3se.asi` starts `tw3se\tw3se.dll`. TW3SE starts once, by whichever way comes first. Started
through an ASI loader, it leaves the `.asi` mods to that loader.

Without Witcher Native Bridge in the game folder, `tw3se.dll` loads `tw3se\compat\WitcherNativeBridge.asi` while it loads
itself: a module of forwarders only, so a mod that imports the bridge's functions finds them in `tw3se.dll`.

## Addresses

For build `0x6abd8695` TW3SE has fixed addresses. On other builds it searches the executable for a byte signature
of each function. A function missing from the build stays off, and with it the feature that needs it.
`tw3se.log` lists them (`addresses: not in this build, ...`). TW3SE stays off as a whole only when one of the 13
functions it needs to start is missing (the `FUNC` rows of `addresses.inc`: the main tick, names, RTTI and
native registration).

## Hooks and the main thread

Hooks use MinHook. TW3SE hooks `CCommonGame::Tick` once. Queued tasks, tick callbacks and console commands run
there. An error in one of them will be logged and the game will continue to work normally.

## Script natives

Every function the script VM calls has a `CFunction` and an opcode. TW3SE adds its natives where the engine
registers its own, before scripts compile. A handler has the form `void(self, frame, result)`: it reads its
arguments from the bytecode at `frame+0x30` and writes the return value to `result`.

The compiler takes the parameters from the `import function` line, so TW3SE only needs the handler and the number
of parameters. A string argument is opcode `0x07`, a 4-byte length, then the bytes. `CallFunction` builds this
bytecode to call a native from C++.

Several mods can import the same TW3SE native. A second `import` goes through the engine's own redeclaration path,
which compares the signatures.

## Classes, fields, names

`GetObjectClass(obj)` reads the object's `CClass` (virtual `GetClass`, vtable slot 1).

| Where | What |
|---|---|
| `CClass+0x10` | parent class |
| `CClass+0x30` / `+0x38` | own properties (pointer / count) |
| `CClass+0x48` / `+0x50` | own methods (pointer / count) |
| `CProperty+0x10` | name id |
| `CProperty+0x20` | offset in the object |
| `CProperty+0x24` | flags (`0x20` = script `var`) |

An engine field is at `object + offset`. A script `var` is in the script data block: `*(object+0x20) + offset`.
`PropertyPtr` handles both. Names go through the name pool: `CNamesPool::AddEntry(text)` gives the id, the id
gives the text back.

## Calling functions

A global native is found by name in the function map. Its opcode points into the global dispatch table.

A method (like `GetMovingAgentComponent`) is in its class's method list. Its opcode points into a separate method
table (24-byte entries, handler at byte 0). The same opcode means different things in the two tables. A method
call passes the object as the context.

## Handles

Functions that return an object give a handle: `refCount` at `+0x00`, flags at `+0x04`, the object at `+0x08`.
Use `*(handle+0x08)` (or `ResolveHandle`). The globals (`thePlayer`, `theGame` …) work the same way.

Every object points to its handle block at `+0x08`. When the object dies, the engine sets the object pointer in the
block to null. TW3SE holds a reference on the block of every object it tracks and checks it before use.

## World edits

Every object passes `CEntity::AttachToWorld` when it enters the world. TW3SE hooks the return of that function. A
placed object has the GUID of its layer file at `CEntity+0xcc`. Spawned objects have a counter
(`ffffffff ffffffff <n> 00000000`).

TW3SE keeps each object by its GUID (for `TW3SE_EntityByGuid`) and applies its edits one tick later. Edits with a
condition are checked twice a second. The `world.txt` files are read again on every load.

`remove` uses `SetHideInGame`, stops the running effects and switches off every enabled component.

## exec and events

The pipe's `exec` calls the engine's command executor directly, the same one the script debugger uses. `events`
gets the lines of `TW3SE_Emit` and of `LogChannel`: TW3SE puts its own sink into the script log and passes every
line on. `script_logs = on` keeps the `LogChannel` calls in compiled scripts.

## Live reload

At start the game compiles its scripts in one pass: it collects the script files, compiles them, prepares the
script system (clears the old definitions), puts the new code in and finishes. Live script objects are told before
and after the swap, so they keep their values. `reloadscripts force` runs this same pass again.

The compiler reports its errors only while it puts the new code in, after the old definitions are gone. So TW3SE
keeps a copy of the script files of `Mods\` and `dlc\` from the last pass that worked. When a pass fails, it writes
the copies back, runs the pass once more and then returns the edited files. TW3SE hands the compiler its own
message receiver; each error arrives with file and line.

## REDkit

The editor is its own build of the engine with other addresses. TW3SE has a separate address table for it and
installs a feature only when all addresses of that feature are found. The editor catches faults before a mod's
own handlers do, so TW3SE puts its handler in front for faults from its own code and passes every other fault on to
the editor.
