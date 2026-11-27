# Lua

TW3SE runs Lua 5.4 inside the game and inside the REDkit editor. A Lua script reads and writes the fields of any
game object and calls the game's functions, native and WitcherScript, while the game runs. A script runs as soon
as it is sent.

<!-- toc -->
**Contents:** [Where Lua runs](#where-lua-runs), [A mod's folder](#a-mods-folder), [Objects](#objects), [Calling functions](#calling-functions), [Latent functions](#latent-functions), [Looking things up](#looking-things-up), [Game events](#game-events), [Hooks (change behaviour)](#hooks-change-behaviour), [Where the scripts are](#where-the-scripts-are), [New objects](#new-objects), [Every frame and on a timer](#every-frame-and-on-a-timer), [Keys](#keys), [Sounds](#sounds), [Time, weather and light](#time-weather-and-light), [Camera](#camera), [Stats, abilities and buffs](#stats-abilities-and-buffs), [Inventory and money](#inventory-and-money), [Horse](#horse), [Spawning and AI](#spawning-and-ai), [Effects and light](#effects-and-light), [Materials](#materials), [Blend shapes](#blend-shapes), [Physics](#physics), [Animation and figures](#animation-and-figures), [Drawing over the world](#drawing-over-the-world), [The game's HUD](#the-games-hud), [Gwent](#gwent), [Quests, map pins and facts](#quests-map-pins-and-facts), [Texts by string id](#texts-by-string-id), [The world](#the-world), [Engine settings](#engine-settings), [Scenes](#scenes), [Console commands and script lines](#console-commands-and-script-lines), [Which clicks the UI takes](#which-clicks-the-ui-takes), [Keys and the mouse as Windows has them](#keys-and-the-mouse-as-windows-has-them), [Files and folders](#files-and-folders), [Network](#network), [Data that goes with the save](#data-that-goes-with-the-save), [Objects you keep](#objects-you-keep), [Text and boxes over the game](#text-and-boxes-over-the-game), [The skeleton of a figure](#the-skeleton-of-a-figure), [Windows in the in-game UI](#windows-in-the-in-game-ui), [Settings in the TW3SE Menu](#settings-in-the-tw3se-menu), [Helpers](#helpers), [WitcherScript calls Lua](#witcherscript-calls-lua), [Functions for WitcherScript, written in Lua](#functions-for-witcherscript-written-in-lua), [Mods that load again](#mods-that-load-again), [Mods that use other mods](#mods-that-use-other-mods), [Where the time goes](#where-the-time-goes), [Examples](#examples), [Tests](#tests)
<!-- /toc -->

## Where Lua runs

- Console: `python tools/tw3se.py lua <code>` runs one line and prints the result.
- `python tools/tw3se.py luafile <name>` runs `bin\x64_dx12\tw3se\lua\<name>.lua`.
- Files in `bin\x64_dx12\tw3se\lua\autorun\` run once, when the game starts.
- A mod is a folder with an `init.lua` (see "A mod's folder"). It runs at startup. Save one of its `.lua` files and
  it loads again within a second (see "Mods that load again").
- `require("name")` loads a module of the mod's own folder, else `tw3se\lua\name.lua`.
- In the REDkit editor the same folders are in `bin\x64_RedKit\tw3se\`.

Lua runs on the game's main thread. A Lua error will be logged in `tw3se.log` and the game will continue to work
normally. The mod's last error stands at the mod in **F2** > **Mods**. The first error of a mod also shows a line over
the game for 8 seconds ("TW3SE: mods/modBetterWolves has an error (F2 > Mods)"). Once the mod loads cleanly, the
error goes from the list.

Three more things end as an error of the call, and the game goes on:

- A memory fault inside a `w3` function (a bad read in the game's data, for example). The message names the place:
  `'load' stopped on a bad read at 0x10 in witcher3.exe+0x1234`.
- Lua code that runs longer than 5 seconds at a time, an endless loop for example (`lua_time_limit` in
  `tw3se.config`). The console's `lua`, `luafile` and `luatest` run as long as they need.
- More memory than all Lua mods may use together, 1 GB (`lua_memory_limit`): the error is "not enough memory".

An `on_tick` function that ends in an error is taken off. A hook that ends in an error stays, and the game's call
goes on to the original; a hook or native that runs into the time or memory limit is taken off. The other mods'
functions go on.

## A mod's folder

Everything of a mod is in one folder. It can be in two places:

- `<game>\Mods\<modName>\tw3se\`, beside the mod's `content` folder. Vortex and a player unpacking the zip put it
  there with the rest of the mod.
- `bin\x64_dx12\tw3se\mods\<name>\`

```
Mods\modBetterWolves\
    content\scripts\...              the mod's WitcherScript, as before
    tw3se\
        init.lua                     runs at startup (the mod's Lua, its settings in the TW3SE Menu)
        wolves.lua                   require("wolves")
        ui\panel.lua                 require("ui.panel")
        natives\wolves.lua           w3.native(...) before the scripts load
        files\characters\...         loose files (see Loose files)
```

`require` finds the mod's own modules first, by the name with dots for folders. Each mod keeps its own: two mods can
each have a `util.lua`. Any other name comes from `tw3se\lua\` (`require("lib.w3util")`).

A mod switched off in the game's `mods.settings` (`Enabled=0` or `Enabled=false`) is off for TW3SE too. Two mod
folders with the same name: the one in `tw3se\mods` counts, and `tw3se.log` names the other.

## Objects

```lua
local game = w3.global("theGame")    -- also thePlayer, theCamera, theSound, theInput ...
local p = w3.player()                -- the player (nil in the main menu)

print(w3.classof(p))                 -- W3PlayerWitcher
print(p.equippedSign)                -- ST_Axii
p.equippedSign = "ST_Igni"           -- write a field
print(p:GetVoicetag())               -- GERALT (a native method)
print(game.activeWorld:GetDepotPath())  -- levels\novigrad\novigrad.w2w
```

A field is read by its name, inherited fields too. What Lua gets:

| Game type | In Lua |
|---|---|
| int, float, bool | number, boolean |
| name, string | string |
| enum | the value's name (`"ST_Axii"`) |
| object (handle, pointer) | an object |
| struct (Vector, ...) | a view into the object: `v.X`, `v.Y` |
| array | a live view: `#a`, `a[i]`, `a[i] = v`, `a:insert(v)`, `a:remove(i)`, `a:to_table()` |
| fixed array (`[6]Uint32`) | a live view of fixed length |
| static array (`static:4,Uint32`) | a live view of its current elements |
| map (`map<name, bool>`) | a live view: `m[key]`, `m[key] = v`, `m[key] = nil`, `#m`, `pairs(m)` |
| EngineQsTransform | `{pos = {X, Y, Z, W}, rot = {X, Y, Z, W}, scale = {X, Y, Z, W}}` (rot: a quaternion) |
| tags (TagList) | a live array of names (`e.tags:insert("my_tag")`) |
| a text id (LocalizedString) | the text in the game's language |
| bitfield | a number; `w3.enum` gives the bit names with their values |
| EngineTransform | `{pos = {X, Y, Z}, rot = {Roll, Pitch, Yaw}, scale = {X, Y, Z}}` |
| a resource by path (soft handle) | the path as a string |
| EntityHandle | the entity while it is loaded, else nil |

Lua writes numbers, booleans, enums (as name or number), bitfields (as number, bit name or list of bit names), names
and strings. An array element is written in place (`a[i] = v`). A struct field or element takes a table of its
fields or a struct value of the same type (`light.color = {Red = 255, Green = 80, Blue = 20, Alpha = 255}`). An
array grows and shrinks: `a:insert(v)` appends, `a:insert(pos, v)` inserts, `a:remove(i)` removes and returns the
element, `a[#a + 1] = v` appends. This works for numbers, names, enums, strings, objects (the array holds each one)
and structs (`a:insert({typeName = "poison"})`). Removing a struct that holds strings or objects returns `true`. A
whole array field takes a table or another array of the same element type (`t.usedAppearances = {"a", "b"}`,
`t.flatCompiledData = other.flatCompiledData`). A pointer field or element takes the object itself; the game's
object it came from keeps owning it. An object as a whole is read-only.

A map works like a Lua table:

```lua
local m = menu.btPresetMap          -- map<name, bool>
m.NoAirDrain = true                 -- add or change
print(m.NoAirDrain, #m)             -- true, the number of entries
m.NoAirDrain = nil                  -- delete
for k, v in pairs(m) do print(k, v) end
menu.btPresetMap = {WeightlessItems = true}   -- exactly these entries
```

`m:to_table()` gives a copy as a Lua table, `m:keys()` the keys, `m:clear()` deletes every entry and returns how
many there were. A key named `to_table`, `keys` or `clear` is read through `m:to_table()[key]`.

## Calling functions

```lua
w3.call("StrLen", "witcher")                 -- 7, a native function
w3.call("GetWitcherPlayer")                  -- the player, a WitcherScript function
w3.call("GetWeatherConditionName")           -- WT_Clear
game:LoadLastGameInit(false)                 -- a native method
p:GetHorseManager()                          -- a WitcherScript method
p:GetAttitude(p)                             -- AIA_Friendly (an object as argument)
w3.call("VecLength", {X = 3, Y = 4, Z = 0, W = 0})   -- 5.0 (a struct from a table)
w3.call("VecLength", p:GetWorldPosition())   -- a struct the game returned
```

Lua calls native functions and WitcherScript functions, global ones (`w3.call`) and methods (`obj:Name(...)`).
Arguments can be:

| Parameter | From Lua |
|---|---|
| int, float, bool, string, name | number, boolean, string |
| enum | the value's name or its number |
| object | an object Lua got from a field, a return value or `w3.global` (its class is checked) |
| struct of numbers, enums and names (Vector, EulerAngles, ...) | a struct value or a table of its fields |
| array (out or in) | a table; an out array is filled by the function and **returned as an extra value** |
| out value (`out string`, `out name`, `out int`, `out Vector`, ...) | a value or a placeholder; the filled value is **returned as an extra value** |

A by-reference parameter (an array, or an `out` value) comes back as an extra return value, in parameter order,
after the function's own return. Pass a value for each one: `{}` for an array the function fills (or a table of
values for one it reads), and for an `out` value a value of its kind. The function sees that value (a number, bool,
enum or plain struct; an `out` string or name starts empty), as a WitcherScript caller's variable would be seen:

```lua
local ok, names = w3.call("GetWeatherNames", {})          -- names is the filled list
local ents = w3.global("theGame"):GetEntitiesByTag("PLAYER", {})   -- the entities with that tag
local ok, left, right = w3.call("StrSplitFirst", "a.b.c", ".", "", "")   -- left = "a", right = "b.c"
local path, appr = w3.player():GetTemplatePathAndAppearance("", "")      -- out string and out name
```

An `out` value works for a number, bool, string, name, enum, and a plain struct (`out Vector`, `out Matrix`). Lua
refuses, with the reason:

- an out/by-reference struct that holds strings or arrays (a plain struct is fine)
- a string inside a struct argument

22 global natives of the game, and more methods, are registered in C++ alone, and the scripts never import them
(`ExampleDouble`, `GetGame`, `Rand`, `RoundFEx`, `IsPointFree`, `GetFreeReachablePoint` ...). `w3.declare` gives one
its signature in WitcherScript's words, and `w3.call` (or the method) uses it from then on:

```lua
w3.declare("ExampleDouble", "int -> int")
print(w3.call("ExampleDouble", 21))                   -- 42
w3.declare("GetGame", "-> CGame")
w3.declare("CActor.SomeNative", "name, out array<name>, optional float -> bool")   -- a method
```

The types are `int`, `float`, `bool`, `string`, `name`, `byte`, `array<T>`, a struct (`Vector`) or a class (passed as
a handle); `out` and `optional` stand in front. A function the scripts declare is refused: `w3.call` knows its
parameters already. A wrong signature makes the game read the arguments wrong; the call is guarded, the result is
the declaration's.

## Latent functions

A latent function runs over several frames (`latent function` in WitcherScript). Lua calls it like any other and
gets a latent object back at once:

```lua
local th = w3.call("MyLatentFunction", 7)    -- starts on a script thread of the game
print(th.done)                               -- true once the function has ended

w3.run(function()
    local th = w3.player():ActionMoveTo(pos)   -- a latent native: runs through ActionMoveToAsync
    th:wait(10)                                -- waits for the end (at most 10 s)
    w3.global("theGame"):FadeOut(0.5):wait()
end)
```

- A latent WitcherScript function or method runs on its own script thread. Its return value is dropped.
- A latent native with an `...Async` twin (`ActionMoveTo`, `FadeIn`, `FadeOut` and others) is started through the
  twin. `th.result` is what the twin returned. `th.done` turns true when the actor's action or the fade has ended.
- A latent native that has no twin raises an error with the reason. `Sleep` and `SleepOneFrame` are refused with a pointer
  to `w3.wait` and `w3.wait_frames`.
- `th:wait(seconds)` works inside `w3.run`.

## Looking things up

```lua
w3.entities()               -- every pre-placed entity currently loaded (a table of objects)
w3.entities("CActor")       -- only those of a class (and its subclasses)
w3.all("CNewNPC")           -- every live object of a class: spawned ones, managers and quest objects too
w3.all("CComponent", 50)    -- at most 50
w3.near(30, "CActor")       -- loaded entities within 30 m of the player (optionally of a class)
w3.near(pos, 30, "CActor")  -- the same around a position (a struct or a table with X, Y, Z)
w3.by_tag("mq1001_vampire") -- entities with a tag (a table of objects)
w3.by_guid("...")           -- the entity with a pre-placed GUID (as four hex dwords), or nil
w3.guid(entity)             -- an entity's pre-placed GUID as "aaaaaaaa-...", or nil for a run-time object
w3.find("weather")          -- names of the classes and enums that contain the text
w3.class("CR4Player")       -- { name, parent, size, fields = {...}, methods = {...} }
w3.fields(p)                -- the fields of an object, with type and offset
w3.dump(p, "health")        -- all fields (name, type, current value) as text, optional name filter
w3.enum("EDeathType")       -- { EDT_Default = 0, EDT_IgniDeath = 1, ... }
w3.enum("EDrawableFlags")   -- a bitfield: { DF_IsVisible = 1, DF_CastShadows = 2, ... }
```

`w3.all` walks every object the engine holds (a few hundred thousand in a big area, a few milliseconds). The pattern
objects inside entity templates stay out; `w3.all(class, limit, true)` takes them along.

The console command `catalog` writes every type and function of the game to `tw3se\catalog\rtti.json`.
`tools/catalog.py` searches it:

```
python tools/catalog.py --game "C:\...\The Witcher 3" find weather
python tools/catalog.py --game "C:\...\The Witcher 3" class CR4Player --all
python tools/catalog.py --game "C:\...\The Witcher 3" fn LoadLastGameInit
```

The variable `TW3SE_GAME` (the game folder) saves the `--game` each time.

`python tools/catalog_html.py --game "C:\...\The Witcher 3"` writes the same catalog as a web page
(`build\catalog_site\index.html`): a search over all names, every class with its parents, the classes derived
from it, its fields and methods, every enum with its values.

## Game events

`w3.on(name, fn)` runs `fn(e)` each time something happens in the game. `e` is a table with what belongs to it.
`w3.off(h)` stops it.

```lua
w3.on("death", function(e)
  if e.killer == w3.player() then w3.hud.message("Killed " .. e.victim:GetDisplayName(), 2) end
end)
w3.on("item_added", function(e) if e.player then print(e.count .. " x " .. e.name) end end)
```

| Event | When | `e` |
|---|---|---|
| `hit` | an actor or object takes damage | `victim`, `attacker`, `damage` (health and essence), `action` |
| `death` | an actor dies | `victim`, `killer`, `action` |
| `item_added` | an item goes into an inventory | `owner`, `player` (true for the player's), `name`, `count`, `ids` |
| `item_removed` | an item leaves an inventory | `owner`, `player`, `name`, `id`, `count` |
| `consume` | the player eats, drinks, uses an item or drinks a potion | `name`, `id`, `potion` (true for a potion) |
| `level_up` | the player gains a level | `level` |
| `sign` | the player casts a sign | `sign` (`ST_Igni` ...) |
| `combat_start`, `combat_end` | the player's combat starts or ends (Geralt and Ciri) | `player` |
| `fact` | a quest fact is added (`FactsAdd`) | `name`, `value` |
| `spawn` | an NPC enters the world | `npc` |
| `scene_start`, `scene_end` | a dialogue or cutscene starts or ends | |
| `quest` | a quest's status changes (checked every 2 s) | `title`, `status`, `old` |
| `world` | the player arrives in another world (checked every second) | `path`, `old` |
| `hour` | the game's hour changes | `hour`, `day` |
| `load`, `save` | as `w3.on_load` and `w3.on_save` | |

An event function that raises an error counts for its mod (**F2** > **Mods**); the other mods' functions still run.
A mod that is taken out loses its functions. `w3.events()` lists the names. To change what the game does, hook the
function behind the event (see "Hooks").

## Hooks (change behaviour)

Intercept a function of the game. The hook gets the object, the call's arguments, and a function to run the original:

```lua
local h = w3.hook("CActor", "GetVoicetag", function(self, args, orig)
    -- self: the object the method ran on; args: the call's arguments (a table); orig(): run the original
    return "custom"            -- return a value to use as the result (the original is skipped)
end)
w3.unhook(h)                   -- restore the original
```

`fn(self, args, orig)`:
- read or change `args` (e.g. `args[1] = "x"`), then call `orig()` to run the original with them
- `orig()` returns the original's result; return that value (as-is or changed) to use it
- return your own value to **replace** the function; the original stays untouched
- for an **observer**, leave the hook empty: the original runs and keeps its own result

`w3.hook(className, name, fn)` for a method, `w3.hook(name, fn)` for a global. Works on native functions and on
WitcherScript functions and methods alike (most of the game is WitcherScript), and on events. Latent functions
(those that run over several frames) are left out. Hooks are global (every call) and run on the main thread.

Several mods can hook the same function. Their handlers form a chain: `orig()` runs the next handler, the last one
runs the original. A fourth argument sets the order, the highest priority first (default 0, equal ones in the
order they were added):

```lua
local a = w3.hook("StrLen", function(self, args, orig) return orig() + 1 end)        -- inner
local b = w3.hook("StrLen", function(self, args, orig) return orig() * 10 end, 5)    -- outer
w3.call("StrLen", "witcher")   -- (7 + 1) * 10 = 80
w3.unhook(b)                   -- a stays
```

A handler that raises an error, or returns nil and leaves `orig()` alone, passes the call on to the next one. Its
error goes to `tw3se.log` with the script functions that were running (see "Where the scripts are" below). When
the whole chain fails before the original ran, the original runs with the call's own arguments.

### Events

Events are hooked the same way, also the ones the engine raises itself (`OnSpawned` when an entity enters the
world).

```lua
w3.hook("CNewNPC", "OnSpawned", function(self, args, orig)
    local r = orig()                        -- the NPC sets itself up first
    print("spawned: " .. self:GetDisplayName())
    return r
end)
```

An event runs on the class that defines it. `CNewNPC.OnSpawned` comes for every NPC, `CGameplayEntity.OnSpawned`
for chests, doors and other objects too (each calls the one above it with `super`). For a function with an `out`
parameter (like `OnGameCameraTick`), `orig()` always takes the call's own arguments, so the out value goes where
the game expects it.

### Virtual methods

The engine's own C++ classes call many of their functions through a vtable. `w3.vhook` puts a handler on one slot:

```lua
local h = w3.vhook(entity, 140, function(self, a, orig)   -- CEntity slot 140: TeleportWithRotation
    print(a[1], a[2], a[3])     -- the raw arguments (rdx, r8, r9); a.f1, a.f2, a.f3 the float registers
    return 0                    -- skips the original; nil runs it
end)
w3.vunhook(h)
```

The slot belongs to the object's class. Every object of that class (and of subclasses that share the vtable) runs
through the handler on the main thread, calls on other threads go to the original. The arguments come as raw
numbers: an integer, or the address of a value (`w3.peek`, `w3.object(address)` for an object). `orig()` runs the
original with `a` as it is then and returns its integer result and its float result. A number the handler returns
goes back as the result (an integer as it is, a fraction as a float), as do an object or a boolean.
`w3.vslot(object, slot)` gives the address of the function in a slot (as an offset in `witcher3.exe`), for finding
the right slot. Up to 64 slots can be hooked, one handler each.

### Engine functions by name

An engine function has one of these names:

- a name of TW3SE's address list (the names plugins pass to `GetAddress`): `"CDrawableComponent::RefreshRenderProxies"`
- a slot of a class's vtable: `"CEntity::vf140"`, or `"CEntity::vf0@10"` for the vtable at offset 0x10 of the
  object. TW3SE finds the class by its name in witcher3.exe, so the name holds after a patch as long as the class
  keeps its slots
- an offset in witcher3.exe: `"0x2263570"` or `"witcher3.exe+0x2263570"`

`tools/enginemap.py` (in the SDK) lists every class of the exe with its slots and names what TW3SE knows of them:
`python tools/enginemap.py class CEntity`.

```lua
w3.engine_addr("CEntity::vf140")     -- the function's offset in witcher3.exe, or nil and why
```

`w3.engine_call(fn, sig, ...)` calls an engine function. `fn` is a name as above or an offset (as `w3.vslot` and
`w3.engine_addr` give it). `sig` has one letter for each argument, then `>` and the letter of the result:

| Letter | Argument | Result |
|---|---|---|
| `i` | an integer | an integer |
| `p` | an object, an address or `nil` | an address |
| `s` | a string (lives until the call returns) | the string at the returned address |
| `b` | a boolean | a boolean |
| `f`, `d` | a number as a float or a double | a number |
| `o` | | the object at the returned address |
| `v` | | nothing |

```lua
local cls = w3.engine_call(w3.vslot(w3.player(), 1), "p>p", w3.player())    -- the player's GetClass
local y = w3.engine_call("0x14fe8a0", "pf>f", nil, 2.5)                       -- 2.5
```

Up to 16 arguments. The function runs on the main thread. A fault inside it ends the call with a Lua error, and the
game goes on. Arguments the function does not expect (a wrong `sig`) can crash the game.

`w3.engine_hook(name, fn)` runs `fn` before an engine function. `a[1]` to `a[4]` are its register arguments (`rcx`, `rdx`, `r8`, `r9`) as integers, `a[5]` to `a[8]`
the next four from the stack (as many as the function has), `a.name` the name. A table the function returns puts
new values into the four registers. The engine function always runs.

```lua
local id = w3.engine_hook("CDrawableComponent::RefreshRenderProxies", function(a)
    print("refreshed", a[1] == w3.addr(component))
end)
w3.engine_unhook(id)
w3.engine_hooks()    -- {{name, calls, handlers}, ...}: what is hooked and how often it ran
```

The function runs for calls on the main thread; calls on other threads pass by (and count in `calls`). Up to 16
engine functions can be hooked, each with any number of functions.

## Where the scripts are

```lua
for _, f in ipairs(w3.script_stack()) do print(f.name, f.file, f.line) end
```

`w3.script_stack()` lists the WitcherScript functions running now, innermost first: `{name, file, line}`, for a
caller the line of the call. It is empty unless Lua runs inside a script call (a hook, a native written in Lua). The
same list goes to `tw3se.log` when a hook fails or the game faults, and into the watchdog's hang report.

```lua
w3.on_line("mymod.ws", 42, function(info)   -- line 42 of your mod's mymod.ws
    print(info.name, info.vars.count)    -- the function, its parameters and locals by name
    info.set("count", 0)                 -- changes a variable before the line runs
end)
```

`w3.on_line(file, line, fn)` runs `fn(info)` each time the game is about to run that line: `info.file`,
`info.line`, `info.name`, `info.self` (the object), `info.vars` and `info.set(name, value)` (only inside `fn`).
`file` is the end of the path as the game keeps it: `game\player\playerwitcher.ws` for the game's scripts, the file
name for a mod's (any case). `w3.off_line(h)` ends the watch.

## New objects

`w3.new(className [, parent])` makes an object of a scripted class, the way `new <className> in <parent>` does in
WitcherScript (parent: `theGame` when left out). Its fields have their default values:

```lua
local data = w3.new("W3NotificationData")
```

The object lives while a Lua variable or a field of the game holds it. Put into a pointer field or an array of
pointers (`ptr:` in the type), or passed to a function of the game (an AI action to `ForceAIBehavior`), it belongs to
the game and stays until the game frees it.
`w3.discard(obj)` lets it go at the next tick and returns true; while another Lua value still holds it, it stays and
the answer is false. An object of the game's is refused, a passed one too (an entity goes with `e:Destroy()`).

`w3.defaults(className)` gives the default values of a class's fields, its parents' too: those of a fresh object,
which goes again at once. A script class's `default` lines show there:

```lua
print(w3.defaults("W3MessagePopupData").progress)   -- -1 (messagePopup.ws: default progress = -1)
```

`w3.new` makes engine objects too, a template for example. `w3.copy(target, source)` copies every field it can from
one object into another of the same class, into arrays and nested structs, and returns the fields it left out. An
NPC spawns from a template built this way:

```lua
local t = w3.new("CEntityTemplate")
w3.copy(t, w3.load([[characters\npc_entities\monsters\wraith_lvl1.w2ent]]))
local npc = w3.spawn(t)
```

Pointer fields are copied as pointers: the copy uses the source's objects (a template's `templateParams`). Keep the
source in a Lua variable while the copy is in use.

The compiled data of a template (`flatCompiledData`) names its appearances. Keep their names: the NPC takes its
meshes from the appearance with the name it knows.

`w3.files.alias(path, newPath)` puts a game file under a second path. Loaded from the new path, it is a resource of
its own with the same contents. The folder of the new path has to exist in the game. Returns true, or nil and the
reason.

```lua
w3.files.alias([[characters\npc_entities\monsters\wraith_lvl1.w2ent]],
               [[characters\npc_entities\monsters\wraith_mine.w2ent]])
local t = w3.load([[characters\npc_entities\monsters\wraith_mine.w2ent]])   -- another template than the game's
```

`w3.files.set(path, bytes)` gives a file new contents for this session, from memory. The next load of the file reads
them; what is already in the world keeps the old version. A new path becomes a new file of the game (its folder
has to exist in the game). `w3.files.clear(path)` brings back the game's own contents. Both return true, or
nil and the reason.

A loose file put into `tw3se\files\` while the game runs is a new file of the game the same way (see
[Loose files](manual/loose-files.md)).

`w3.attach(entity, template)` puts the components of a template onto an entity in the world, the way the game adds
the parts of an appearance. `w3.detach(entity, template)` takes them off. The entity needs a `CAppearanceComponent`
(NPCs and Geralt have one). `template` is a `CEntityTemplate` or its path.

## Every frame and on a timer

```lua
w3.on_tick(function()
    -- runs once per game tick
end)
w3.every(5, function() print("five seconds") end)        -- every 5 seconds
w3.after(2, function() print("once, 2 s later") end)
w3.watch(p, "equippedSign", function(new, old)           -- when a field changes
    print(old .. " -> " .. new)
end)
```

A tick function that raises an error will be removed and the error logged. `w3.watch` stops looking when the
object is gone. `w3.on_tick`, `w3.every`, `w3.after` and `w3.watch` return a handle for `w3.off_tick(h)`,
`w3.on_draw` one for `w3.off_draw(h)`; each stops the function.

`w3.on_reload(fn)` runs `fn` after every live reload of WitcherScript (`reloadscripts force`). `w3.off_reload(h)`
stops it.

## Keys

```lua
w3.on_key("F6", function() print("F6") end)              -- once per press
local h = w3.on_key("Ctrl+Shift+K", function() end)
w3.off_key(h)
if w3.key_down("Shift") then ... end                     -- held right now
```

`w3.on_key(key, fn)` runs `fn` once each time the key goes down. Its modifiers have to match exactly: `F6` fires
for F6 alone, `Ctrl+F6` for F6 with Ctrl held. `w3.key_down(key)` is true while the key is held, also when more modifiers are held. Keys count
while the game has the focus and the in-game UI is closed.

Key names: `A`-`Z`, `0`-`9`, `F1`-`F24`, `Num0`-`Num9`, `Space`, `Enter`, `Esc`, `Tab`, `Backspace`, `Shift`,
`Ctrl`, `Alt`, `Up`, `Down`, `Left`, `Right`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`, `Mouse1`-`Mouse5`
and more, or a Windows key code like `0x70`. Modifiers come first, joined with `+`. An unknown name raises an error.

The controller's buttons are keys too: `Pad_A`, `Pad_B`, `Pad_X`, `Pad_Y`, `Pad_LB`, `Pad_RB`, `Pad_LT`, `Pad_RT`,
`Pad_LS` and `Pad_RS` (a stick pressed), `Pad_Back`, `Pad_Start`, `Pad_Up`, `Pad_Down`, `Pad_Left`, `Pad_Right`. A
button before the `+` has to be held: `w3.on_key("Pad_LB+Pad_Y", fn)`. The game sees the buttons as well, so a
combination with `Pad_LB` or `Pad_RB` gets in its way least. TW3SE reads the first XInput controller (an Xbox
controller, or one that Steam Input shows as one). A key field in the TW3SE Menu takes controller buttons too.

### Key bindings in the game's menu

```lua
w3.input.action{name = "MyMod_Howl", key = "H", pad = "Pad_Y", label = "Howl", on = function() howl() end}
```

`w3.input.action{...}` gives the mod an action of the game's own: it stands under **Options** > **Key Bindings**,
the player binds it there like any other, and `on()` runs when it is pressed. `key` is one key (`A`-`Z`, `0`-`9`,
`F1`-`F24`, `Num0`-`Num9`, `Space` ...), `pad` one controller button (`Pad_A` ... `Pad_RT`), `label` the text in
the menu (a table by language too), `contexts` the game's input contexts it works in (by default exploration,
combat, horse, swimming, diving and boat, for Geralt and Ciri). A WitcherScript mod hears it with
`theInput.RegisterListener(this, 'OnHowl', 'MyMod_Howl')`.

TW3SE writes the action into the game's `input.xml` (between its own markers) and its bindings into
`Documents\The Witcher 3\input.settings` where the action has none yet; the player's own bindings stay. Both files
keep a copy (`.tw3se_backup`) from before the first change. The game reads them at its start, so a new action shows
in the menu from the next start. Until then `key` and `pad` run `on()` in the action's contexts.

Caution: on a German keyboard the menu stores **^** as `IK_Tilde`, a key the game never reports there. An action
bound to **^** stays silent.

## Sounds

```lua
local s = w3.sound.play("mods\\modMyMod\\sounds\\hum.ogg", {entity = w3.player(), bone = "head", volume = 0.8, loop = true})
w3.sound.volume(s, 0.3)
w3.sound.stop(s)
```

`w3.sound.play(file [, options])` plays a WAV or OGG file and returns a handle. The file is an absolute path or a
path from the game folder. With `entity` the sound is in 3D at it (or at its `bone`), heard from the camera; called
with the file alone, it plays flat. `volume` 1 plays the file as it is, `loop` repeats it until `w3.sound.stop`. `w3.sound.playing(s)`
tells whether it still plays, `w3.sound.preload(file)` decodes a file ahead. A sound at an entity stops when the
entity is gone, and a mod's sounds stop when the mod is taken out.

The game's own sounds (its Wwise events):

```lua
w3.sound.event("gui_global_denied")                           -- flat, as the menus play it
w3.sound.event("monster_cloud_giant_cmb_weapon_hit_add", npc, "head")   -- at an entity, at its bone
w3.sound.switch("hit_location", "head", npc)                  -- a switch, at an entity or for the whole game
w3.sound.state("game_state", "exploration")                   -- a state (music and mood)
w3.sound.param("monster_count", 3)                            -- a parameter (a third value: at an entity)
```

A Wwise bank of your own goes into `tw3se\files` (see [Loose files](manual/loose-files.md)) and is there from the
next start. `w3.sound.load_bank("my_bank.bnk")` loads it, then its events play with `w3.sound.event`.
`w3.sound.unload_bank(name)` and `w3.sound.bank_loaded(name)` work on the game's banks too.

## Time, weather and light

```lua
local t = w3.time.get()          -- {day, hour, minute, second, seconds}
w3.time.set(22, 30)              -- 22:30 today (a third value sets the day)
w3.time.add(8)                   -- eight hours later, as meditation does
w3.time.speed(1.0)               -- game hours per real minute (the game's own is 0.25)
w3.weather.set("WT_Rain_Storm", 10)   -- a storm in 10 s; w3.weather.names() lists them
print(w3.weather.get(), w3.weather.rain())
w3.env.fake_time(0)              -- the light of midnight, w3.env.fake_time(nil) ends it
```

`w3.time()`, called as a function, stays the real clock in seconds. `w3.env.all()` lists the environment definitions,
`w3.env.activate(path [, priority, blend, seconds])` puts one over the area's and returns an id for
`w3.env.deactivate(id)`.

## Camera

```lua
print(w3.camera.pos(), w3.camera.fov())
w3.camera.fov(90)                -- holds 90 until w3.camera.fov(false)
w3.camera.free(true)             -- the free camera; w3.camera.move(pos, rot) places it
local x, y = w3.camera.to_screen(w3.player():GetWorldPosition())   -- 0..1 on the screen, nil behind the camera
w3.camera.shake(0.5)
```

The game pulls the field of view back to its own within 3 seconds, so `w3.camera.fov(d)` sets it every frame until
`w3.camera.fov(false)`. `w3.camera.rot()`, `forward()` and `heading()` give the camera's direction.

The game's picture can sit in a rectangle of the screen, like an editor's viewport between its panels:

```lua
w3.view.rect(320, 72, 768, 360)    -- screen pixels; gives back x, y, w, h of the picture as drawn
local x, y = w3.view.to_screen(w3.player():GetWorldPosition())   -- pixels where it is drawn now
w3.view.rect(false)                -- full screen again
```

The whole camera frame is scaled into the rectangle at its own aspect, centred, with black around it. What the game
draws into its picture goes along: its HUD, its menus, its mouse pointer. TW3SE's overlay and in-game UI stay full
screen on top. Each frame costs one copy of the picture on the graphics card. `w3.view.rect()` without arguments
gives the picture's place, or nil when it is off.

## Stats, abilities and buffs

```lua
local v, vmax = w3.stats.get("vitality")       -- also toxicity, stamina, focus, air ... (BCS_ names work too)
w3.stats.set("toxicity", 0)
w3.stats.buff("wellfed")                       -- EET_WellFed; w3.stats.unbuff, has_buff, buffs()
print(w3.stats.has_ability("ConGeralt"))      -- true; add_ability, remove_ability, abilities()
print(w3.stats.level(), w3.stats.health())
w3.stats.add_xp(500)
```

Each function takes an actor as its last argument (default the player): `w3.stats.get("vitality", npc)`.

## Inventory and money

```lua
w3.inv.add("Torch", 3)
print(w3.inv.count("Torch"))
w3.inv.remove("Torch", 1)
for _, it in ipairs(w3.inv.items()) do print(it.name, it.count) end
w3.inv.add_money(100)                          -- money(), remove_money()
```

An entity as the last argument uses its inventory: `w3.inv.items(merchant)`. `w3.inv.equip(id)`,
`w3.inv.unequip(id)` and `w3.inv.price(id)` take an item id from `w3.inv.items()`.

### Items of your own

```lua
w3.items.ability{name = "mymod_swallow_plus", stats = {vitality_regen = {type = "add", min = 40, max = 60}}}
w3.items.add{name = "mymod_swallow_plus_potion", category = "potion", price = 250, stackable = 1,
             icon = "icons/inventory/potions/swallow-64x64.png", tags = {"Potion", "SingletonItem"},
             abilities = {"mymod_swallow_plus", "default_potion_stats"},
             text = {EN = "Swallow Plus", DE = "Schwalbe Plus"}, description = "Heals more"}
```

`w3.items.add{...}` makes a new item, `w3.items.ability{...}` a new ability for it. The fields are those of the
game's item XML (`gameplay\items\*.xml`, `tools\bundle_extract.py` copies them out): `category`, `price`,
`stackable`, `grid_size`, `ability_slots`, `icon`, `tags`, `abilities` (the item's base abilities), and `attrs` for
any other attribute. A stat as a plain number is an `add` (`quality = 5`). `text` and `description` are the name
and the description in the game, a table by language too.

The item is in the game at once: `w3.inv.add("mymod_swallow_plus_potion")`. A second call with the same name changes
it (a new price, other abilities). TW3SE writes the items of every mod into a DLC of its own
(`tw3se\files\dlc\dlctw3se\`) as well, and from the next start on the game loads them from there.

`w3.items.mount(path)` loads an item XML of your own while the game runs, in the format of the game's
`gameplay\items\*.xml`. The path is a depot path, for example a loose file in `tw3se\files\` or a file made with
`w3.files.set`. It returns a handle, or nil and the reason. `w3.items.unmount(handle)` takes the definitions away
again. Items already in an inventory stay there.

```lua
w3.files.set([[gameplay\items\mymod_items.xml]], w3.fs.read(w3.dir() .. [[\mymod_items.xml]]))
local m = w3.items.mount([[gameplay\items\mymod_items.xml]])
```

Caution: a save that holds such an item loses it when the mod is gone.

## Horse

`w3.horse.summon()` whistles for Roach, `w3.horse.get()` is Roach while she is in the world,
`w3.horse.mounted()` the horse the player sits on, `w3.horse.items()` her saddle, bags and trophy.
`w3.vehicle()` is the horse or boat in use, or nil.

## Spawning and AI

```lua
local npc = w3.spawn("characters\\npc_entities\\monsters\\wraith_lvl1.w2ent")   -- 3 m in front of the player
w3.ai.set_attitude(npc, "friendly")            -- towards the player (a third value: towards another actor)
w3.ai.immortal(npc, true)
local id = w3.ai.move_to(npc, w3.ahead(1), true)   -- runs to a point; w3.ai.cancel(npc, id) stops it
w3.destroy(npc)
```

`w3.spawn(template [, pos, yaw])` takes a path or a template and returns the entity. It lasts until the next save load.
`w3.ahead(d)` is the point `d` metres in front of the player.

`w3.ai.force(npc, treeClass, priority, setup)` puts one of the game's behaviour trees over the NPC's own and returns
an id; `setup(tree)` fills its parameters first:

```lua
w3.ai.force(npc, "CAIMoveToAction", "BTAP_Emergency", function(t) t.params.targetTag = "PLAYER" end)
```

```lua
local wolf = w3.spawn([[characters\npc_entities\monsters\wolf_lvl1.w2ent]])
w3.ai.companion(wolf, {immortal = true})       -- friendly, follows the player, fights beside them
```

`w3.ai.follow(npc [, opts])` puts the game's follow tree (`CAIFollowAction`) over the NPC below its combat: a fight
comes first, then it follows again. `opts.distance` (2.5 m), `opts.run = false` (it walks), `opts.teleport = false`
(it stays behind; by default it appears near the player when `opts.catchup`, 40 m, behind), `opts.target` (a tag,
`"PLAYER"`). `w3.ai.companion(npc [, opts])` makes the NPC friendly first, and immortal with `opts.immortal`. Both
return the id for `w3.ai.cancel`.

Also: `w3.ai.attitude(npc)`, `reset_attitude`, `group`, `set_group(npc, group)`, `reset_group`, `var(npc, name [,
value])` for a behaviour variable, `stop`, `kill`, `in_combat`, `target`. An NPC follows a tree only with an AI of
its own: a crowd NPC spawned outside its community stands still, and its health reads -1.

## Effects and light

```lua
w3.fx.play(entity, "fire")                     -- stop(entity [, name]), active, intensity
local lamp, light = w3.light.spawn(w3.ahead(2), {color = {255, 120, 40}, radius = 8, brightness = 40})
w3.light.set(light, {on = false})
for _, l in ipairs(w3.light.near(15)) do w3.light.set(l, {color = {80, 80, 255}}) end
```

`w3.light.spawn` places a light of its own (a hand torch with its meshes and flame hidden). `w3.light.of(entity)`
gives an entity's lights, `w3.light.near(radius [, pos])` the lights around a point.

### Decals

A decal is a picture projected onto the surfaces in front of it, the way the game paints blood and footprints. On a
figure it moves with the figure.

```lua
local p = w3.player():GetWorldPosition()
local id = w3.decal.spawn{pos = {p.X, p.Y, p.Z}, dir = {0, 0, -1}, roll = 30, size = {0.3, 0.2, 0.4},
                          diffuse = [[fx\textures\blood\blood_splash_spray_small_3.xbm]],
                          color = {255, 255, 255, 255}}
w3.decal.spawn{pos = hit, dir = toward, size = 0.05, diffuse = tex, on = npc}   -- only on this NPC
local ids = w3.decal.spawn_many(list, npc)     -- many in one call
w3.decal.color(id, {200, 200, 180, 255}, {specularity = 0.2})
w3.decal.remove(id)                             -- clear(npc) takes all decals made on it, clear() all of yours
```

`pos` is the middle, `dir` the direction the picture is projected in (straight down when left out). `up` sets the
top of the picture, `roll` turns it by degrees around `dir` when `up` is left out. `size` is `{width, height, depth}`
in metres or one number for all three; the depth reaches half in front of `pos` and half behind. `diffuse` and
`normal` are texture paths, the diffuse's alpha is the shape. A decal stays until `remove` or `clear`; `life` ends it
after that many seconds (`fade_out` blends it out). It lands on every surface in its box, figures too. `on` puts it
only on the meshes of an entity or a list of entities; a mesh component in the list stands for itself alone (a decal on one piece of clothing, not on the skin under it). The player's gear is entities of its own.

Other fields: `sphere` (smaller towards the ends of the depth), `double_sided`, `static_only`, `priority`,
`specularity`, `spec_scale`, `spec_base`, `spec_color`, `fade_in`, `hide_distance`, `atlas` ({scale u, scale v,
offset u, offset v}, 0.5 each for the whole picture).

One mesh takes 2 decals in the game; a third one stays invisible. A figure is several meshes (body, legs,
head ...), and a decal on it lands on most of them. `w3.decal.per_mesh(127)` lets a mesh take up to 127 (it returns
the number before, `w3.decal.per_mesh(2)` is the game's again). With 127 decals on a figure the frame rate stayed the
same (measured). The renderer also keeps a budget of decals in the world: `w3.decal.limits()` gives the engine's decal
settings, `w3.decal.limits({DynamicDecalsLimit = 2000, DynamicDecalsChunkLimit = 20000})` sets them. Loading a save or travelling to another world
takes every decal away.

## Materials

```lua
for _, m in ipairs(w3.material.of(npc)) do print(m.index, m.class) end   -- the materials of its meshes
local lamp = w3.material.of(npc)[2].material
print(w3.material.get(lamp, "EmissivePower"))
w3.material.set(lamp, "EmisiveColor", {Red = 255, Green = 0, Blue = 0, Alpha = 255})
```

`w3.material.params(material)` gives every parameter of a material instance by name (numbers, colours, vectors,
textures as their resource). A material belongs to its mesh, so a change counts for every entity with that mesh. It
shows on the entities that appear after the change (a new spawn, objects loaded as you travel). An entity on screen
keeps the values it was made with until `w3.material.apply(entity)` draws its meshes anew.

`w3.material.own(entity)` gives one entity materials of its own: each of its mesh components gets its mesh loaded a
second time, with its own material instances, so `w3.material.set` on them changes this entity only (a skin colour
for one figure). It returns how many meshes became its own. HairWorks fur (`CFurComponent`) keeps the shared ones.

`w3.refresh(component)` draws one component anew from its current data. It returns false for a component outside
the world.

## Blend shapes

```lua
local head = w3.deform.meshes(npc)["h_01__wraith"]
local set = assert(w3.deform.load(head, [[deform\h_01__wraith.twdf]]))
w3.deform.set(set, "inflate", 0.7)
w3.deform.unload(set)
```

Shapes from a file move the vertices of a mesh, each with a weight. The mesh changes on every entity that uses it;
`w3.deform.own(head)` first gives one entity a mesh of its own. The file format, `w3.deform.dump` and
`tools\twdf.py` are on the page [Blend shapes](manual/blend-shapes.md).

`w3.set_mesh(component, mesh)` gives a running mesh component another mesh (a `CMesh`, for example from `w3.load`).
The entity shows it in the next frames. The game gives the component a new handle then: the Lua value passed in
moves with it, other Lua values of the same component turn invalid (`w3.valid`); fetch the component again.

## Physics

```lua
w3.physics.impulse(barrel, {X = 0, Y = 0, Z = 200})   -- a push; torque(entity, v) turns it
w3.physics.velocity(barrel, {X = 0, Y = 0, Z = 6})    -- sets the speed, returns it
print(w3.physics.mass(barrel))
w3.physics.ragdoll(npc)                                -- the game's ragdoll effect; ragdoll(npc, false) ends it
```

`w3.physics.gravity()` gives the gravity of the world's PhysX scene as `{X, Y, Z}` (the game's is
`{X = 0, Y = 0, Z = -9.81}`); `w3.physics.gravity(v)` sets it. A body lying still sleeps and wakes when something
pushes it, so a new gravity moves the bodies in motion first (a ragdoll that is still falling, a crate right after a
hit).

`w3.physics.ragdoll(npc)` works on NPCs with an effect manager: monsters, soldiers and enemies. Villagers and other
crowd NPCs have none (the game leaves it out for `ENGT_Commoner`). An AI forced with `w3.ai.force` keeps the NPC from
falling over.

These work on entities with a body the game simulates (`w3.physics.bodies(entity)` lists them): crates, barrels and
other loose objects, near the player and while the game runs. `w3.physics.collisions(actor, false)` lets an actor
walk through others, `w3.physics.kinematic(entity, false)` hands it to the physics.

### Bodies of your own

```lua
local p = w3.player():GetWorldPosition()
local box = w3.physics.body{shape = "box", size = 0.5, mass = 10, pos = {p.X + 1.5, p.Y, p.Z + 2}}
local ball = w3.physics.body{shape = "sphere", radius = 0.2, mass = 2, pos = {p.X - 1.5, p.Y, p.Z + 1}}
w3.physics.push(ball, {0, 10, 0}, "velocity")
local hand = w3.physics.body{shape = "capsule", radius = 0.05, height = 0.2, kinematic = true, pos = p}
w3.physics.move(hand, {p.X, p.Y, p.Z + 1})          -- each frame, to follow a bone: it pushes what it meets
print(w3.physics.pose(box).pos.Z)
w3.physics.remove(box)                                -- w3.physics.clear() takes all of them out
```

`w3.physics.body(opts)` puts a body of its own into the world's physics: `shape` `"box"` (`size` the full size, one
number or `{x, y, z}`), `"sphere"` (`radius`) or `"capsule"` (`radius`, `height`, along X), `pos`, `rot` (a quaternion
`{x, y, z, w}`), `mass` (10), `friction` (0.5), `bounce` (0.1). It collides like the game's loose objects. A
`kinematic` body moves only where `w3.physics.move` sends it and pushes the bodies in its way, a `static` one stays.

`w3.physics.pose(body)` gives `{pos, rot}`, `w3.physics.move(body, pos, rot)` moves it, `w3.physics.push(body, v,
mode)` adds a force (`"force"`, `"impulse"`, `"velocity"`, `"acceleration"`), `w3.physics.speed(body [, v])` gives
or sets its speed. The bodies have no mesh: to see one, place an object of the game where it is each frame. A body
stays in the world until `w3.physics.remove`, `w3.physics.clear` or the next load of a save.

```lua
local rope = w3.physics.joint("rope", anchor, ball, {length = 1})            -- at most 1 m apart
local pin  = w3.physics.joint("ball", nil, box, {at = {x, y, z}})            -- hangs at a point of the world
local weld = w3.physics.joint("fixed", a, b, {break_force = 500})            -- breaks above 500 N
w3.physics.unjoint(rope)
```

`w3.physics.joint(kind, a, b, opts)` joins two bodies: `"ball"` (turns freely), `"hinge"`, `"fixed"`, `"slider"` and
`"rope"`. `a` and `b` are bodies of `w3.physics.body`, the address of a PhysX actor, or `nil` for the world. The joint
sits at `opts.at` (a point of the world; default: where `b` is). A rope runs from the middle of one body to the
middle of the other (or `opts.at_a`, `opts.at_b`), at most `opts.length` long (default: the distance now);
`opts.stiffness` and `opts.damping` make it a spring. `opts.break_force` lets the joint break above that force.
`w3.physics.clear` takes the joints out too.

`w3.physics.parts(actor)` gives the bodies of a figure by bone name (`pelvis`, `head`, `l_hand`, `r_foot`, ...). They
move with the figure's animation, also after its death. `w3.physics.pose` reads them, and a joint holds a body of
your own to them:

```lua
local hand = w3.physics.parts(npc).r_hand
local lamp = w3.physics.body{shape = "sphere", radius = 0.1, mass = 2, pos = w3.physics.pose(hand).pos}
w3.physics.joint("rope", hand, lamp, {length = 0.5})
```

## Animation and figures

```lua
w3.anim.play(w3.player(), "geralt_mutation_11", {slot = "PLAYER_SLOT"})   -- a slot animation
w3.anim.var(npc, "Ragdoll_Weight", 1)                 -- a behaviour graph variable; without a value: read it
w3.anim.event(npc, "Ragdoll")                         -- a behaviour graph event (a third value true: forced)
print(w3.figure.appearance(npc), table.concat(w3.figure.appearances(npc), ", "))
w3.figure.appearance(npc, "wraith_02")                -- one of its template's appearances
```

`w3.anim.play(actor, name [, {slot, blend_in, blend_out, loop}])` plays an animation of the actor's animation sets in
a slot (`NPC_ANIM_SLOT` by default); an actor whose AI is busy may cut it off. `w3.anim.mimic(actor, name)` plays a
face animation. `w3.figure.line(actor, id)` speaks a line of the game by its string id, with subtitle, and
`w3.figure.voiceset(actor, name)` plays one of the actor's voice sets.

```lua
w3.face.set(w3.player(), {jaw_open_a = 1, corner_left_up = 0.5, corner_right_up = 0.5})
w3.face.set(npc, {brow_left_up = 0.3}, "add")   -- on top of what the game plays (a scene, lip sync)
w3.face.clear(w3.player())
```

`w3.face.set(actor, weights [, "add"])` moves the face by its tracks, every frame until `w3.face.clear(actor)`. The
weights stand in place of the game's own; with `"add"` they go on top of them. A new `set` replaces the last one,
an empty table clears. `w3.face.tracks(actor)` lists the track names (Geralt has 148: `jaw_open_a`, `lips_out`,
`brow_left_up` ...); the first call starts watching the face and returns an empty list, a call a moment later has
them. Geralt's face is his head item: the player works, and so does the head item itself. A mod that is taken out
gives its faces back to the game.

`w3.figure.head(name)` gives Geralt one of the barber's heads (`"head_0"` is clean-shaven); `w3.figure.head_reset()`
brings his own back. `w3.figure.furs(entity)` lists the entity's HairWorks hair and fur components.

```lua
local fur = w3.figure.furs(wolf)[1]
w3.figure.fur_color(fur, {Red = 200, Green = 30, Blue = 30, Alpha = 255}, {Red = 255, Green = 80, Blue = 80, Alpha = 255})
w3.figure.fur_reset(fur)                              -- the colours and textures from before
```

`w3.figure.fur_color(component, root, tip)` colours the fur at the root and at the tip (either may be `nil`). The
colour replaces the colour texture of that part. The colours belong to the fur resource, so every entity with the
same fur changes with it (all wolves of one kind). HairWorks has to be on in the game's settings.

```lua
local t = w3.anim.with_sets(template, {"animations\\man\\combat\\man_geralt_signs.w2anims"})
local npc = w3.spawn(t)
npc:GetRootAnimatedComponent():PlaySkeletalAnimationAsync("man_geralt_aard_front", true)
```

`w3.anim.with_sets(template, sets)` gives back a copy of the template with more animation sets. NPCs spawned from it
can play the animations of those sets.

A figure that is in the world already takes one more set with `w3.anim.add_set`:

```lua
local set = w3.anim.add_set(npc, [[animations\man\combat\man_geralt_signs.w2anims]])
npc:GetRootAnimatedComponent():PlaySkeletalAnimationAsync("man_geralt_aard_front", true)   -- true now
w3.anim.remove_set(npc, set)
```

The set's animations play by name from then on and win over the figure's own of the same name. It takes an actor (its
root animated component) or a `CAnimatedComponent`, and a path or a loaded set. TW3SE keeps the set loaded while the
figure has it. When the game builds the figure's sets anew (the figure is set up again after it left the world), the
added set is gone: add it again.

Any animation of a figure's sets can be read at any second, and laid on the figure in place of what it does:

```lua
local p = w3.anim.sample(w3.player(), "high_standing_determined_gesture_explain_01", 1.2)
-- like w3.pose: p.count, p.duration, p.modelToWorld, p.bones[i] = {name, parent, x, y, z}
w3.anim.pose(w3.player(), {{"high_standing_determined_gesture_explain_01", 1.2, 1}})
w3.anim.pose(w3.player(), {{"anim_a", 0.4, 0.5}, {"anim_b", 2.0, 0.5}})   -- halfway between the two
w3.anim.pose_clear(w3.player())
```

Both go through the game's own sampler, so every compression works: the right hand of `geralt_mutation_11` sampled
at 1.2 s lies 0.1 cm from where the game's own playback has it at 1.2 s. `w3.anim.sample` gives the pose in the figure's
own space, the space of `w3.pose`; its bones are the figure's skeleton, and bones outside the animation keep the
rest pose. While the animation's data is still streaming in it returns nil and an empty string: ask again on a later
frame. A name missing from the figure's sets gives nil and the reason.

`w3.anim.pose(actor, layers)` lays the blend of the layers `{name, t, weight}` on the figure from the next frame on,
inside or outside a scene, until `w3.anim.pose_clear`. Call it again with new times to move it; the figure stands
still between calls. Weights that add up to less than 1 leave the rest to what the figure would otherwise do. The pose
goes in before the step that makes the skeleton's model space, so every mesh, hair and dangle follows it. A figure
that leaves the world keeps its entry until `pose_clear`.

A figure's container also holds the sets a scene pushed onto it, some of them made for another skeleton. Sampled on
this figure, such an animation gives a broken pose (a hand a metre behind the back): the sampler writes
bones in the order of the skeleton the set was made for.

### A figure's parts

```lua
for _, part in ipairs(w3.figure.parts(w3.player())) do print(part.item, part.name, part.mesh) end
local crown = w3.figure.part(w3.player(), "crown")       -- by component name or a piece of its mesh path
w3.figure.show(crown, false)                             -- off; true brings it back
w3.figure.mesh(w3.figure.part(w3.player(), "gloves"), [[characters\models\geralt\armor\armor__viper\s_01_mg__viper.w2mesh]])
```

`w3.figure.parts(actor)` lists the mesh parts of a figure and of its mounted items (head, hair, armour pieces), each
`{component, name, mesh, item, visible}`. `w3.figure.show(part, on)` turns a part on or off, `w3.figure.mesh(part,
mesh)` gives it another mesh (a path or a `CMesh`). A part that follows the skeleton takes any mesh made for the same
skeleton: the bones are matched anew by name.

`w3.figure.add(actor, what)` gives a figure a new part that follows its skeleton. `what` is a mesh path (`.w2mesh`),
a template path (`.w2ent`, an item's for example) or a `CEntityTemplate`. It returns a handle and the new parts, in
the form of `w3.figure.parts`. `w3.figure.remove(handle)` takes the part off. A part lasts until the next load of a save.

```lua
local h = w3.figure.add(w3.player(), [[items\bodyparts\geralt_items\gloves\witcher_viper\g_01_mg__viper_lvl3.w2ent]])
w3.figure.remove(h)
```

The colours of an item are in its template. A mesh path alone draws a mesh with a colour mask in red.

`w3.figure.look(actor)` gives the look your mods gave a figure as a plain table: the shapes of body and face, the
parts turned off or given another mesh, and the added parts. `w3.figure.apply(actor, look)` puts it on a figure
again, after a save load for example, and returns the handles of the added parts.

```lua
local s = w3.store("mylooks")
s.geralt = w3.figure.look(w3.player())
-- later
w3.figure.apply(w3.player(), w3.store("mylooks").geralt)
```

`w3.figure.preview(actor, opts)` points the free camera at a figure from the front and puts a light of its own beside
the camera, for a character editor. `opts`: `yaw` (degrees around the figure), `distance` (2.5 m), `height` (the
point looked at, 1.2 m above the feet), `lift` (the camera higher by that), `fov`, `light = false`, `color`,
`brightness`. A second call moves the view. `w3.figure.preview_end()` gives the game's camera back.

```lua
w3.figure.preview(w3.player(), {yaw = 30, distance = 1.0, height = 1.65, fov = 40})   -- the face
w3.figure.preview_end()
```

### Animations made at run time

```lua
w3.anim.create(w3.player(), "my_wave", {fps = 30, bones = {"r_shoulder", "r_bicep"}, frames = {
    {{rot = {0, 0, 0, 1}}, {}},                        -- frame 1: the shoulder turned, the bicep as at rest
    {{rot = {0, 0, 0.7071, 0.7071}}, {rot = {0.5, 0, 0, 0.866}}},
}})
local h = w3.anim.run(w3.player(), "my_wave", {loop = true, speed = 1, weight = 1})
w3.anim.stop(h)
```

`w3.anim.create(actor, name, anim)` makes an animation for the actor's rig, without cooking. Each frame has one
entry per name in `bones`, each `{pos = {x, y, z}, rot = {x, y, z, w}, scale = {x, y, z}}` local to the bone's
parent. What an entry leaves out takes the rig's rest pose, so a rotation alone keeps the bone in its place. Bones
the rig lacks are left out, and the rig's bones the animation does not name keep the figure's own pose. It returns
how many of the bones are in the rig. `raw` in place of `frames` takes the frames as one string of float32 values,
10 per bone (position xyz, rotation xyzw, scale xyz; NaN takes the rest pose's value), for animations read from a
file.

The animation is the engine's own kind (an uncompressed animation buffer), and the engine samples it. It goes into
a set of its own in the figure's animation sets, so everything that looks an animation up by name on that figure
finds it: `w3.anim.play` (a slot animation), `w3.anim.pose`, `w3.anim.sample`, `w3.anim.run`. A second `create` with
the same name replaces it.

`w3.anim.run(actor, name, opts)` plays an animation of the figure, made or the game's, as a pose layer:
`loop` (default true), `speed`, `weight`. Without loop it ends by itself at the end.

### Proportions

```lua
w3.anim.shape(w3.player(), {
    Root    = {scale = 1.1, inherit = true},   -- the whole figure 10 % taller
    r_bicep = {scale = 1.2, only = true},      -- a thicker upper arm, the forearm and the hand as they were
    r_hand  = {length = 1.1},                  -- a longer forearm: the hand 10 % further from the elbow
    head    = {scale = 0.95},
})
w3.anim.shape(w3.player(), {})                 -- back to the figure's own
```

`w3.anim.shape(actor, bones)` changes the figure's bones every frame, after its animation: per bone name `scale` (one
number or `{x, y, z}`), `offset` (`{x, y, z}` in the parent's space), `rot` (a quaternion `{x, y, z, w}` after the
bone's own turn) and `length` (the bone's distance to its parent times this). Every mesh, hair and dangle follows.
A scale makes the bones below the bone as big as well and leaves them in their places. `inherit = true` also moves
them apart, so the whole part grows evenly. `only = true` keeps the scale on the bone itself. A call replaces the
figure's last shape; a bone the rig lacks is an error.

`w3.anim.bones(actor)` lists the bone names of a figure's rig. A face has a rig of its own in the head's
`CMimicComponent` (Geralt's has 180 bones: `nose`, `chin`, `jaw`, `left_eye`, `left_chick1`, `upper_lip`, ...), and
`w3.anim.shape` on that component shapes the face. The face's expressions and lip sync go on as before.

```lua
local face
for _, p in ipairs(w3.figure.parts(w3.player())) do
  local e = p.component:GetEntity()
  face = face or (e:GetComponentsByClassName("CMimicComponent") or {})[1]
end
print(table.concat(w3.anim.bones(face), ", "))
w3.anim.shape(face, {nose = {scale = 1.2}, chin = {offset = {0, 0.01, 0}}, left_eye = {scale = 1.1}, right_eye = {scale = 1.1}})
```

### Recording

```lua
w3.anim.record(w3.player(), 30)                          -- at most 30 seconds
-- ... the figure moves: the game, a scene, w3.anim.pose ...
local a = w3.anim.record_stop(w3.player(), 30)          -- {fps, bones, raw} at 30 frames a second
w3.anim.save(w3.fs.folder("tw3se") .. "\\wave.tw3anim", a)
w3.anim.create(npc, "wave", w3.anim.load(w3.fs.folder("tw3se") .. "\\wave.tw3anim"))
```

`w3.anim.record(actor, maxSeconds)` writes the figure's pose down every frame from the next frame on, whatever moves
it. `w3.anim.record_stop(actor, fps)` ends it and gives the recording at a fixed rate, with every bone of the rig, as
`w3.anim.create` takes it. `w3.anim.save(path, anim)` and `w3.anim.load(path)` keep it as a file.

## Drawing over the world

Inside `w3.on_draw`:

```lua
w3.on_draw(function()
  for _, npc in ipairs(w3.near(20, "CNewNPC")) do w3.draw.label(npc, npc:GetDisplayName()) end
  w3.draw.line_at(w3.player():GetWorldPosition(), w3.ahead(5), 0xFFFF00FF)
end)
```

`w3.draw.text_at(pos, text [, colour])`, `marker_at(pos [, size, colour])`, `line_at(a, b [, colour, width])` and
`label(entity, text)` draw at world positions and return false when the point is off the screen.
`w3.draw.line(x1, y1, x2, y2, colour, width)` draws in pixels.

```lua
w3.debug.show(true)                            -- the debug shapes of the game and of mods on screen
local h = w3.debug.watch("health", function() return w3.player():GetHealth() end)
w3.debug.unwatch(h)
```

`w3.debug.watch(label, fn)` shows `label: value` over the game every frame, in a box below the top left corner.
Floats show with two decimals, a position as three numbers. A value that raises an error shows `error` and the
message. `w3.draw.size(text)` gives the width and height of `w3.draw.text` in pixels.

The retail game drops what scripts hand to `CVisualDebug` (`AddSphere`, `AddText`, `AddBox`, `AddLine`, `AddArrow`,
`AddAxis`, `AddBar`). `w3.debug.show(true)` draws these shapes with the overlay, at the entity they belong to, until
their time runs out or `Remove...` takes them away. `w3.debug.show(false)` stops it, `w3.debug.count()` gives the
number of shapes kept.

## The game's HUD

```lua
w3.hud.enabled("MinimapModule", false)         -- a HUD module off; w3.hud.modules() lists them
w3.hud.visible(false)                          -- the whole HUD, like the player's own toggle
w3.hud.message("Saved", 3)                     -- a notification for 3 s
w3.hud.oneliner("Hm.", npc)                    -- a line over a figure; w3.hud.hide_oneliner(npc)
w3.hud.ask("Take the bribe?", function(yes) if yes then w3.inv.add_money(50) end end, {title = "Guard"})
```

`w3.hud.ask(text, fn [, opts])` opens the game's own message window. `fn(yes)` runs when the player answers: `true`
for Yes or OK. `opts.buttons` is `"yes_no"` (the default), `"ok_cancel"` or `"ok"`, `opts.title` the window's title.
Texts can be tables by language.

## Gwent

```lua
for _, c in ipairs(w3.gwent.collection()) do print(c.card, c.count) end
w3.gwent.add("triss", 2)                       -- copies of a card (1 to 1000) by its name from w3.gwent.cards();
                                               -- remove, count, has
print(#w3.gwent.cards(), w3.gwent.deck())      -- every card {name, title, power, faction, index}; the selected deck
w3.gwent.unlock("GwintFaction_Skellige")       -- unlocked(faction); deck(faction) selects one
```

## Quests, map pins and facts

```lua
for _, q in ipairs(w3.quest.list("JS_Active")) do print(q.title) end
local title, entry = w3.quest.tracked()        -- w3.quest.track(entry), w3.quest.status(entry)
w3.quest.toggle_pin(w3.ahead(20))              -- a map pin of the player's own there, or away again
w3.fact.add("mymod_visits")                    -- get, set(name, n), remove, exists
```

`w3.quest.pins()` lists the player's own map pins `{x, y, type, area, id}`. `w3.quest.toggle_pin(pos, type)` puts
a pin of type 1 to 7 (the map menu's symbols, 1 by default) or the waypoint (type 0, one at a time). A pin already at
that spot is taken away. It returns ok, the id added and the id removed.

The quest graph, live:

```lua
for _, b in ipairs(w3.quest.blocks()) do print(b.class) end   -- the active blocks of every running quest thread
w3.quest.activate(thread, block)                -- starts a block of that thread's graph
```

`w3.quest.threads()` lists the running quest threads (sub-threads too), `w3.quest.blocks([thread])` their active
blocks `{block, class, thread}`, `w3.quest.children(thread)` a thread's sub-threads.

## Texts by string id

```lua
w3.text.set(1131989, "Another line")            -- where the game's menus show string 1131989
print(w3.text.get(1131989))
w3.text.clear(1131989)                         -- the game's text again
```

A text set this way is shown where the game's scripts and menus read the id (`GetLocStringById`: item names,
labels). It stays until it is cleared or the mod is unloaded. An id the game does not have works too: a mod picks a
number of its own (`2112000000` and up) and scripts read it with `GetLocStringById`. Story scenes read their lines
another way and keep the game's text: `w3.scene.on_line` and `w3.scene.on_choice` change what a scene shows.

Texts by key (menu labels, option names, item names) can come from text files, one `key = text` per line, in
`Mods\<modName>\tw3se\strings\<language>.txt` or `tw3se\strings\<language>.txt` (`en.txt`, `de.txt` ...). The game
shows the text of its language, English when the language has none, and follows a switch of the language in the
menu. Scripts (`GetLocStringByKey`, `GetLocStringByKeyExt`) and Flash (`[[key]]`) find these keys. `#` starts a
comment line, `\n` in a text is a line break. TW3SE reads the files once the game runs.

## The world

```lua
print(w3.world.path())                         -- levels\kaer_morhen\kaer_morhen.w2w
local g = w3.world.group_of(entity)            -- the layer group the entity is in
w3.world.hide_group(g)                         -- its entities go; w3.world.show_group(g) brings them back
local z = w3.world.ground(w3.ahead(10))        -- the ground's height there
local hit, pos, normal = w3.world.trace(a, b)  -- a ray through the static world
```

`w3.world.layers([filter])` lists the paths of the loaded layers. A layer group is the folder of its layers under
the world's folder, `""` for the layers right in it. Also: `nav_z(pos)` (the navmesh height), `water(pos)` (level,
depth), `safe_spot(pos)` (a free navmesh spot), `sphere(pos, radius)`, `terrain()` (size, tiles).

The game merges buildings, walls and rocks per layer into its sector data:

```lua
for _, o in ipairs(w3.world.sector_near(w3.player():GetWorldPosition(), 40)) do
    print(o.layer, o.index, o.type, o.radius, o.distance)          -- type "mesh", "collision" or "decal"
end
w3.world.sector_hide(layer, index)     -- the object goes (a mesh with its collision) when its layer attaches next
w3.world.sector_hide(layer, index, true)   -- at once: the layer group is hidden and shown again
w3.world.sector_restore(layer, true)   -- everything hidden in that layer comes back (true: at once)
```

`w3.world.sector(layer)` lists the merged objects of one loaded layer (a path from `w3.world.layers()`). With `true`
the whole layer group is shown again after the change: its other entities appear anew, and a group the game keeps
loaded but hidden (a quest's snow, for example) becomes visible.

### Where things fit

```lua
for _, h in ipairs(w3.world.cast(a, b)) do print(h.dist, h.what) end   -- every hit of the ray, nearest first
local s = w3.world.stand(pos)      -- s.ok, s.ground; s.why: "something is in the way 0.78 m above the ground"
local w = w3.world.walk(a, b)      -- w.ok, w.reached, w.length, w.why
local f = w3.world.fits({pos = pos, size = {x = 1, y = 0.6, z = 0.8}, yaw = 90})   -- f.ok, f.rests, f.cuts, f.why
local box = w3.world.free_spot({pos = pos, size = {x = 1, y = 0.6, z = 0.8}})       -- the nearest place it fits
```

`w3.world.cast(a, b)` gives every hit along a ray: `pos`, `normal`, `dist` in metres and `what`. `what` is
`"entity"` (then `entity` and `component` are set), `"terrain"`, or `"world"`: houses, walls, rocks and static meshes.
A ray downwards hits the upper faces only, so it finds every floor of a house under its roof.

`w3.world.floors(pos)` lists those surfaces at a point, top first, each with its `z`, its `slope` in degrees and its
`headroom` (the height up to the next surface above). `covered` marks a surface with another one less than 0.3 m
above it, a plank over the ground for example.

`w3.world.stand(pos)` says whether a person can stand at a point: ground within 0.5 m of `pos.Z` and at most 45
degrees steep, water less than 0.6 m deep, and room for a person 1.8 m tall and 0.35 m in radius. `why` says what is wrong.
`w3.world.walk(a, b)` goes from `a` to `b` in steps of 0.25 m. The ground may rise 0.45 m or fall 1 m per step, and the
person has to fit at each one. Both take options: `radius`, `height`, `step`, `slope`, `stride`, `climb`, `drop`, and
`people = true` (other people are in the way too).

`w3.world.fits(box)` says whether a box fits. `pos` is the middle of its underside, `size` is in its own axes, `yaw` in
degrees. `rests` says whether its corners have ground under them within 0.1 m. The volume is tested with spheres on a
grid: a gap thinner than a third of the box's smaller side (at most 0.25 m) can go unseen.
`w3.world.free_spot(box, {radius = 5})` searches in rings for the nearest place where it fits.

`w3.world.heightmap(pos, width, depth, step)` gives the floor's height on a grid (`z[row][col]`, `false` where there is
none). With `{top = true}` each cell has the highest surface.

```lua
for _, m in ipairs(w3.world.merged(w3.player():GetWorldPosition(), 20)) do
    print(m.mesh, m.size.x, m.size.y, m.size.z, m.yaw, m.distance)
end
```

`w3.world.merged(pos, radius)` lists the houses, walls and rocks the game merged into its sector data, nearest first.
Each has its `mesh` path, `origin`, `axes`, `rot` (`{Pitch, Yaw, Roll}`) and `scale`, its box in the world (`min`,
`max`), its `size` in its own axes and the `distance` from the point to its box. `kind` is `"proxy"` for a house as the game draws it from afar and
`"volume"` for weather and sound volumes. `group` is the layer a mesh came from: the parts of one building share it.
The first call reads the tables of the game's bundles to name the meshes (about 0.2 s). `w3.world.path_hash(path)`
gives the hash a mesh has in the merged data.

```lua
local wall = w3.world.place([[environment\architecture\human\common\cellar\cellar_mod_wall_flat.w2mesh]], pos, 90)
wall = w3.world.move(wall, w3.ahead(5), {Pitch = 0, Yaw = 45, Roll = 0})
w3.world.remove(wall)
```

`w3.world.place(mesh, pos, rot, {scale = {x, y, z}})` puts any mesh of the game into the world with its collision,
from its path alone. `rot` is a yaw in degrees or `{Pitch, Yaw, Roll}`. It returns the entity; the game leaves it out
of its saves. A static mesh's collision stays where the mesh was made, so `w3.world.move` makes it anew and returns
the new entity. A merged mesh placed with its own `origin`, `rot` and `scale` lies exactly on the original.
`w3.world.angles(axes)` gives the `{Pitch, Yaw, Roll}` and the scale of three axes.

## Engine settings

```lua
for _, v in ipairs(w3.config_list("fog")) do print(v.path, v.type) end   -- PostProcess/AllowFog bool ...
w3.config("PostProcess", "AllowFog", false)        -- no fog from the next frame on
print(w3.config("Rendering", "UseUmbraOcclusion")) -- true: objects behind others are left out (Umbra)
```

The engine has about 1700 settings of its own, in groups like `Rendering`, `PostProcess`,
`Rendering/RT/PathTracer`, `Rendering/Shadows`, `Physics` and `Gameplay`. `w3.config(group, name)` reads one,
`w3.config(group, name, value)` sets it. A setting counts from the next time the engine uses it: many each frame
(`Visuals/GammaValue`, `PostProcess/AllowFog`), others only when a world loads or the settings menu applies them
(`Rendering/TerrainErrorMetricMultiplier`, `Rendering/HairWorksLevel`).

## Scenes

```lua
w3.scene.play("quests\\...\\my_scene.w2scene", "Input")   -- from one of its inputs
print(w3.scene.playing())
```

A scene finds its actors by the tags it asks for (`w3.scene.inputs_tags(scene)`). `w3.scene.stop()` ends every
running scene at the next tick, quest scenes too, and returns how many; the actors the scene spawned go with it.
`w3.scene.stop(player)` ends one scene, given by its `CStoryScenePlayer` (`w3.all("CStoryScenePlayer")`).
`w3.scene.stop(actor)` calls the game's `StopAllScenes` on the actor, which ends its gameplay scenes only.

A running scene can be held and moved through its current section like a video, forwards and backwards:

```lua
local s = w3.scene.time()        -- {section = "q107 janek is not here", t = 3.2, length = 17.6}, nil without a scene
w3.scene.pause(true)             -- people, camera, events and voice stand in their frame; false plays on
w3.scene.seek(7.5)               -- to second 7.5 of the section, paused or playing
w3.scene.play(path, "Input", {at = 7.5, paused = true})   -- starts there and waits
local els, cur = w3.scene.elements()   -- the section's lines, pauses and choices; cur is the one playing now
-- els[3] = {index = 3, start = 1.70, duration = 4.76, t = 2.38, running = true}
```

A seek goes through the game's own section player, so poses, camera, placements and look-ats are those of normal
playback at that second: in q107_08_janek_not_here the right hand lands within 0.1 to 0.7 cm of where playback had
it, the camera within 0.3 cm. Where a section blends a gesture with the actor's own idle, the idle keeps a clock of
its own that runs on while the scene is paused, and two normal playbacks already lie about a centimetre apart there.

A seek backwards restarts the section and runs every event up to the time in one call. A time past the end is held
just before it, or the section would finish. Each function takes a `CStoryScenePlayer` as its last argument; without
one it uses the first running scene. `w3.scene.seek` fails while a scene is changing its section; the next tick
works.

Lines and choices of a dialogue on screen:

```lua
w3.scene.on_line(function(text) return text .. " (heard)" end)   -- the subtitle shown; nil keeps it
w3.scene.on_choice(function(text) if text == "Farewell." then return "Bye." end end)   -- a choice's text
w3.scene.add_choice("Ask about the weather", function()
  w3.hud.message("Cold, as always", 3)
end)
w3.scene.add_choice("Leave", function() return 2 end, function(choices) return #choices >= 2 end)
for i, c in ipairs(w3.scene.choices()) do print(i, c.text, c.disabled) end
w3.scene.choose(1)                              -- the first choice, as if the player picked it
```

`add_choice(text, fn [, when])` puts a choice of your own under the game's choices of every dialogue, or of those
whose choices pass `when(choices)`. When the player picks it, `fn()` runs and the game's choices come back on screen.
If `fn` returns a number, that choice of the game's is taken (1 is the first). The voice of a line stays the game's,
`on_line` changes its subtitle, `on_choice` the text of a choice. `w3.scene.remove_choices()` takes every choice, line
and choice function away.

## Console commands and script lines

```lua
local ok, err = w3.exec('addfact("my_fact", 1)')       -- a console command (an exec function too)
local seq = w3.link.mark()                              -- the newest line's number
-- later
local seq2, lines = w3.link.since(seq)                  -- the lines scripts sent after it
for _, line in ipairs(lines) do print(line) end
```

`w3.exec(cmd)` runs console commands, the way the console and the pipe's `exec` run them. Several commands go in one
string with `"\x1f"` between them; they run in order. It returns `true`, or `false` and the error.

`w3.link.since(seq)` gives the lines scripts sent with `TW3SE_Emit` and with `LogChannel` on the channels in
`events_channels` (default `Cj`), after the line numbered `seq`. It returns the last line's number for the next call,
the lines, and how many lines were dropped in between (the oldest go after 8192). Each reader keeps its own number.

`w3.command(line)` runs a TW3SE command (the core's or a plugin's), the way the TW3SE console runs it, and returns
its reply as text:

```lua
print(w3.command("questlive on"))
```

## Which clicks the UI takes

With `w3.ui.overlay(true)` a window of your mod takes the mouse where it lies and the game gets it elsewhere. A
window over the whole screen that only takes clicks on some of its parts says so every frame:

```lua
w3.ui.take_mouse(over_my_bar)      -- true: the UI has the mouse this frame; false: the game has it
```

Without the call ImGui decides (the mouse over one of your windows).

## Clicks and keys from a script

`w3.ui.feed` gives the UI a click, a key or text as if they came from the mouse and the keyboard (for a test or a
remote control of your mod). Each call is one step; the steps reach the UI one per frame, in order:

```lua
w3.ui.feed{x = 400, y = 300}                     -- the mouse there (screen pixels)
w3.ui.feed{button = 0, down = true}              -- left pressed (0 left, 1 right, 2 middle) ...
w3.ui.feed{button = 0, down = false}             -- ... and let go: a click
w3.ui.feed{key = "Enter", down = true}           -- a key by ImGui's name, then down = false
w3.ui.feed{chars = "Hello"}                      -- typed text
w3.ui.feed{ctrl = true}                          -- Ctrl held (shift, alt the same; false lets go)
local waiting, held = w3.ui.feed_pending()
```

The mouse stays where the last step put it for 30 frames; then the real cursor has it again. The game gets none of
these.

## Keys and the mouse as Windows has them

```lua
if w3.input.down(0x57) then print("W held") end          -- a virtual-key code; true only with the game in front
local x, y = w3.input.cursor()                           -- the cursor on the screen
local cx, cy, cw, ch = w3.input.client()                 -- the game's picture on the screen
w3.input.set_cursor(cx + cw // 2, cy + ch // 2)          -- the cursor put there
local notches = w3.input.wheel()                         -- wheel notches since the last call
```

These read Windows' own state, whatever the UI or the game does with the input. `w3.input.front()` tells whether
the game is the window in front. `set_cursor` works while the UI is open too (the game's own calls to move the
cursor are held back then).

## Files and folders

```lua
local docs = w3.fs.folder("documents")                   -- also "appdata", "localappdata", "temp", "game", "tw3se"
w3.fs.mkdir(docs .. "\\MyMod\\saves")                    -- with every folder above it
w3.fs.write(docs .. "\\MyMod\\saves\\a.txt", "hello")
print(w3.fs.read(docs .. "\\MyMod\\saves\\a.txt"))
for _, e in ipairs(w3.fs.list(docs .. "\\MyMod")) do print(e.name, e.dir, e.size, e.mtime) end
```

Paths are UTF-8, so a folder with an umlaut works (Lua's `io.open` takes the ANSI code page). `w3.fs.write` writes a
file next to the target first and then puts it in its place, so the old file stays whole until the new one is done.
`list` sorts by name. `exists`, `isdir`, `mtime` (seconds since 1970), `size`, `remove`, `rmdir`, `rename` and `copy`
take paths too. On an error a function returns `nil` and the reason.

## Programs

```lua
local p = w3.proc.run('"C:\\Tools\\convert.exe" in.png out.dds', {cwd = "C:\\Work"})
-- later, each tick
for _, line in ipairs(w3.proc.lines(p)) do print(line) end   -- its output since the last call
local code = w3.proc.exit(p)                                  -- nil while it runs, then its exit code
if code then w3.proc.close(p) end

w3.proc.open("C:\\Users\\Me\\Documents\\MyMod")               -- the folder in Explorer
w3.proc.open("https://www.nexusmods.com")                     -- the page in the browser
w3.proc.open("C:\\MyMod\\report.zip", {select = true})        -- its folder, the file chosen
```

`run` starts a program hidden and reads its output (stdout and stderr) while it runs. The game goes on meanwhile.
`kill` ends it. `open` opens a file, folder or page the way Windows opens it,
in a window of its own.

## Network

```lua
w3.http.get("https://example.com/data.json", function(status, body, headers, err)
    print(status, #body)                                  -- 200 and the answer; status 0 and err when none came
end)
w3.http.post("http://127.0.0.1:8080/score", "kills=3", function(status, body) end,
             {headers = {["Content-Type"] = "application/x-www-form-urlencoded"}, timeout = 5})

local conn = w3.net.connect("127.0.0.1", 9000, {
    on_connect = function(c) c:send("hello\n") end,
    on_data = function(c, data) print(data) end,
    on_close = function(c, err) end,
})
local sock = w3.net.udp(9001, function(data, host, port) print(data) end)   -- this PC only
sock:send("127.0.0.1", 9002, "ping")
```

Requests and sockets work in the background; the callbacks run on the game's main thread in a later tick. The game
never waits for the network. `w3.http.request(opts, fn)` takes `method`, `url`, `headers`, `body` and `timeout`
(seconds, 30 by default). A UDP socket listens on 127.0.0.1 only. An error in a callback will be logged and the
other callbacks go on.

## Messages

```lua
local h = w3.on_message("mymod.cmd", function(text, channel) print("got " .. text) end)   -- "*" for every channel
w3.send("mymod.ui", "opened")
w3.off_message(h)
```

A message is a channel name and a text. `w3.send` hands it to every listener of the channel: other Lua mods, C++
plugins (`RegisterOnMessage`) and scripts (`TW3SE_Receive(channel)`). A script sends with `TW3SE_Send`. Listeners run
on the main thread, in a later tick. The sender gets its own messages too, so two directions use two channels.

## Data that goes with the save

```lua
local data = w3.saved()                  -- this mod's table for the current save
data.kills = (data.kills or 0) + 1
w3.on_load(function() print("kills in this save: " .. tostring(data.kills)) end)
w3.on_save(function() data.crowns = w3.player():GetMoney() end)       -- right before it is written
```

`w3.saved([name])` returns a table that is saved with the game. When the game saves, TW3SE writes every such table
to `tw3se\saves\<id>.lua`. After a load the table holds the values from that save (it stays the same table, so a
`local` keeps working). After a new game it is empty. Numbers, strings, booleans and tables are kept; functions and
game objects are left out. Outside a mod (the console) it needs a name.

`w3.on_load(fn)` runs after a save loaded or a new game started, `w3.on_save(fn)` right before the tables are
written. Both return a handle for `w3.off_load(h)` and `w3.off_save(h)`. The id is two facts (`tw3se_save_id` and
`tw3se_save_id_hi`), so each save knows its file.

`w3.store([name])` is a table that stays across every save and new game: totals, unlocks, a mod's own
preferences. TW3SE keeps it in `tw3se\store\<name>.lua` and writes it within 2 seconds of a change. It keeps the
same values as `w3.saved`.

```lua
local stats = w3.store()
w3.on("death", function(e)
  if e.killer == w3.player() then stats.kills = (stats.kills or 0) + 1 end
end)
```

## Objects you keep

A Lua variable keeps its game object alive. Once the game destroys the object (a killed and unloaded NPC, an entity
after `Destroy`), every read, write or call on it raises "the ... is gone". `w3.valid(obj)` tells before use:

```lua
if w3.valid(target) then target:SetTemporaryAttitudeGroup('hostile', AGP_Default) end
```

## Text and boxes over the game

```lua
w3.on_draw(function()
    local p = w3.player()
    if not p then return end
    w3.draw.rect(20, 20, 240, 56, 0x202020C0)
    w3.draw.text(32, 28, 0xFFFFFFFF, "Crowns " .. p:GetMoney())
end)
local w, h = w3.screen_size()
```

`w3.draw.text(x, y, colour, text)` and `w3.draw.rect(x, y, width, height, colour)` work inside an `on_draw`
function. Coordinates are pixels from the top left, colours `0xRRGGBBAA`. The drawing shows with DLSS Frame
Generation too.

## The skeleton of a figure

`w3.pose(entity)` gives the whole skeleton of a figure from the last frame, in the figure's own space (metres):

```lua
local p = w3.pose(w3.player())          -- nil on the first call: from then on the figure is watched
-- p.count, p.modelToWorld (16 numbers, row-major), p.bones[i] = {name, parent, x, y, z}
```

`parent` counts from 0, the root has -1. Names and parents are there for the first 40 bones (pelvis, spine, head,
arms, hands, legs); the bones after them have the name "".

`w3.pose(entity, "mimic")` gives the face skeleton of the head in the same form.

The figure stays watched while `w3.pose` asks for it. 10 seconds after the last call the watch ends by itself,
`w3.pose_stop(entity)` ends it at once.

`w3.pose(component)` gives one component's pose with the skeleton it is skinned from, no watch needed: an animated
component its own skeleton, a `CAnimDangleComponent` (breasts, dresses, a mod's own dangle rig) its rig, every bone
named.

```lua
w3.anim.rest_bones([[characters\mymod\my_dangle.w2rig]])
```

`w3.anim.rest_bones(rig)` gives the bones of a dangle rig that the dangle neither takes from the figure nor simulates
their rest pose each frame, for every figure that uses the rig. Without it such bones keep whatever was in memory, and
a mesh skinned to them flies apart. `w3.anim.rest_bones(rig, false)` takes it off.

```lua
w3.anim.rest_bones([[characters\base_entities\man_base\man_base.w2rig]], {"my_root", "my_01", "my_02"})
```

`w3.anim.rest_bones(rig, {names})` is for bones a mod adds to a figure's own rig. The game's animations have no
track for them, and a standing figure holds them in a pose of the game's. Each bone named gets its rest pose on its
parent every frame, for every figure with the rig, before plugins get the pose. A child that is not named stays
where the animation put it, so name the children too. Each bone has to hang on one of the rig's first
`lodBoneNum_1` bones (40 on `man_base`) or on another bone of the list; the error names the bone that does not.

Name the rig by its path when the call comes early, in an autorun or in the main menu. A world that ends unloads the
rig, and the figures of the next one use a new object of it; TW3SE looks the path up again after every load and
every few seconds. A rig given as an object stays that object.

## Windows in the in-game UI

F2 opens the TW3SE Menu (its key: **Mods** > **TW3SE** > **Menu key**). It has a Lua console and an inspector,
and every Lua mod can add windows. While it is open, keyboard and mouse go to it.

```lua
local clicks = 0
w3.on_ui(function()
  w3.ui.window("My mod", function()
    w3.ui.text("Crowns: " .. w3.player():GetMoney())
    if w3.ui.button("Click") then clicks = clicks + 1 end
    w3.ui.text("clicked " .. clicks .. " times")
  end, 320, 160)
end)
```

`w3.on_ui(fn)` runs `fn` every frame while the UI is open and returns a handle for `w3.off_ui(handle)`. A function
that raises an error will be removed, the error logged, and the windows it left open are closed.

| Function | What it shows | Returns |
|---|---|---|
| `window(title, fn [, w, h [, closable]])` | a window, `fn` draws its content; with `closable` its title bar has an X | true while it is unfolded; with `closable` also false once the X was clicked (stop drawing it then) |
| `child(id, height, fn [, width])` | a scrolling region (0: the rest) | |
| `tabs(id, fn)` with `tab(label, fn)` | tabs; `fn` of a tab draws it while it is chosen | |
| `tree(label, fn)`, `collapsing(label, fn)` | a part that opens on a click | |
| `table(id, {"A", "B"}, fn [, opts])` with `row()` and `col()` | a table with headers ([Tables](#tables-lists-and-drag-and-drop)) | |
| `rows(count, fn)` | `fn(i)` for the rows that can be seen | |
| `id(key, fn)` | own ids for the widgets in `fn` (the same labels in a loop) | |
| `text(s [, 0xRRGGBBAA])` | text | |
| `wrap(s)`, `dim(s)` | text that breaks at the edge, grey text | |
| `button(label)` | a button | true when clicked |
| `checkbox(label, value)` | a checkbox | changed, value |
| `input_text(label, text [, on_enter [, hint]])` | a text field (`hint`: grey text while it is empty) | changed, text |
| `input_number(label, n)` | a number field (Enter takes the value) | changed, n |
| `slider(label, value, min, max)`, `slider_int(...)` | a slider | changed, value |
| `combo(label, current, {items})` | a drop-down list | changed, value |
| `selectable(label, selected [, opts])` | a line to select | clicked, double-clicked |
| `select(set, key [, keys])` | what a click does to a selection (Ctrl, Shift) | |
| `drag(kind, value [, preview])`, `drop(kind)`, `dragging([kind])` | drag and drop | see below |
| `context(fn [, id])` | a menu on a right-click on the widget before it | true while it is open |
| `menu_item(label [, shortcut [, checked [, enabled]]])`, `menu(label, fn)` | a line of a menu, a menu in a menu | true when clicked |
| `icon(name)` | | an icon as text for a label, `icon()` the list of names |
| `tooltip(s)`, `tooltip(fn)` | a tooltip on the widget before it; `fn` draws it | |
| `key()` | | the key pressed this frame (`"F6"`, `"Ctrl+K"`), or nil |
| `same_line()`, `separator()`, `spacing()`, `width(w)` | layout | |
| `align()` | text on this line at the height of a button's label | |
| `is_open()` | | the UI is open |

The calls with a function close what they opened, also when the function raises an error.

### Tables, lists and drag and drop

A list of scenes that sorts on a click on a header, keeps its header while it scrolls and fills the window:

```lua
local cols = {"Name", {"Stages", width = 50}, {"Added", fit = true, sort = "desc"}, {"", width = 30, sort = false}}
w3.ui.table("scenes", cols, {height = 0}, function()
  local col, asc, changed = w3.ui.table_sort()
  if changed then sort_scenes(col, asc) end
  w3.ui.rows(#scenes, function(i)
    local s = scenes[i]
    w3.ui.col()
    local clicked, double = w3.ui.selectable(s.name, selected[s.id], {span = true, overlap = true})
    if clicked then w3.ui.select(selected, s.id, shown_ids) end
    if double then play(s) end
    w3.ui.drag("scene", s.id, s.name)
    w3.ui.col() w3.ui.align() w3.ui.text(s.stages)
    w3.ui.col() w3.ui.align() w3.ui.text(s.added)
    w3.ui.col() if w3.ui.button(w3.ui.icon("Play")) then play(s) end
  end)
end)
```

A column is its header, or a table with the header first:

| Field | What it does |
|---|---|
| `width = 50` | a fixed width in pixels |
| `fit = true` | as wide as its widest cell |
| `stretch = 2` | a share of the width left (a plain column has a share of 1) |
| `sort = "asc"`, `sort = "desc"` | the table starts sorted by this column |
| `sort = false` | a click on its header leaves the order as it is |
| `resize = false` | its width stays as set |
| `hide = true` | hidden until the header's right-click menu shows it (with `hideable`) |

The options come after the function or before it:

| Option | What it does |
|---|---|
| `height = 300` | the table scrolls inside 300 pixels; its header stays at the top. `height = 0`: down to the window's bottom |
| `sortable = true` | a click on a header sorts (on by itself when a column has `sort = "asc"` or `"desc"`) |
| `multi_sort = true` | Shift+click on a second header sorts by both |
| `scroll_x = true`, `freeze = 1` | the table scrolls sideways; `freeze` columns stay on the left |
| `reorder = true`, `hideable = true` | columns can be dragged to another place, hidden from the header's right-click menu |
| `borders`, `row_bg`, `resizable`, `headers` | on unless set to false |

`table_sort()` gives the column to sort by, true for ascending, and `changed`. `changed` is true on the first frame
and after a click on a header; the list is sorted then. `table_sort(true)` gives every column of a `multi_sort`
table as `{{column, ascending}, ...}`.

`rows(count, fn)` calls `fn(i)` only for the rows on screen, so a list of thousands costs as much as the part you
see. In a table each call is a row. Each row has its own ids, so labels may repeat. The rows need one height.

`selectable` options:

- `span = true`: the whole row of the table, as high as a button. `align()` puts the text of the other cells on its
  line
- `overlap = true`: buttons in the row still take their clicks
- `fit = true`: as wide as its text, for several on one line. A selectable reaches to the right edge otherwise and
  covers the next one on its line
- `width`, `height`, `disabled = true`

`select(set, key, keys)` works as a file list does. A click selects that row alone, Ctrl+click adds it or takes it
away, and Shift+click selects from the last row clicked to this one. `set` is a table of `key = true`, `keys` the
keys in the order shown.

Drag and drop moves a Lua value from one widget to another:

```lua
for i, entry in ipairs(queue) do
  w3.ui.id(i, function()
    w3.ui.selectable(entry.name, false)
    w3.ui.drag("queue", i)                     -- this line can be dragged, the value is its place
    local from, over = w3.ui.drop("queue")      -- a line let go on this one
    if from then table.insert(queue, i, table.remove(queue, from)) end
    local id = w3.ui.drop("scene")              -- a scene from the table above
    if id then table.insert(queue, i, scene_by_id[id]) end
  end)
end
```

`drag(kind, value, preview)` comes after the widget that is dragged. `preview` is the text at the mouse, or a
function that draws it; text and numbers show themselves. `drop(kind)` comes after the widget that takes it and
gives the value let go on it in that frame, and as second value the one held over it now. `dragging(kind)` gives
the value dragged now, to show where it can go. A kind is a name of up to 32 characters.

`context(fn)` after a widget opens a menu on a right-click:

```lua
w3.ui.context(function()
  if w3.ui.menu_item("Play", "Enter") then play(s) end
  if w3.ui.menu_item("Favourite", nil, s.fav) then s.fav = not s.fav end
  w3.ui.menu("Add to", function()
    for _, list in ipairs(lists) do if w3.ui.menu_item(list.name) then add(list, s) end end
  end)
end)
```

`icon(name)` gives an icon of Windows' icon font as text: `w3.ui.button(w3.ui.icon("Play") .. " Play")`. The
names: Play, Pause, Stop, Next, Previous, Shuffle, Repeat, Volume, Mute, Up, Down, Left, Right, ChevronUp,
ChevronDown, ChevronLeft, ChevronRight, Lock, Unlock, Add, Remove, Delete, Close, Check, Edit, Save, Folder, File,
Copy, Paste, Undo, Redo, Refresh, Search, Filter, Sort, Settings, More, Menu, List, Star, StarFill, Heart,
HeartFill, Eye, Hide, Info, Warning, Error, Help, Person, People, Camera, Video, Picture, Clock, Calendar, Home,
Pin, Unpin, Link, Map, Location, Globe, Move, Rotate, ZoomIn, ZoomOut, FullScreen, Download, Upload, Sync, Flag,
Tag, Bulb, Gripper, Mail, Light, Code, Bug. They are in the menu's main font, at its size.

### A place and a heading in the world

`w3.ui.gizmo` lets you move a place in the world with the mouse and turn its heading, for a scene's start or an
object:

```lua
local pos, yaw = w3.player():GetWorldPosition(), 0
w3.on_ui(function()
  local changed, p, y = w3.ui.gizmo("start", pos, yaw, {ground = true, snap_angle = 15})
  if changed then pos, yaw = p, y end
end)
```

It is drawn over the game's picture, behind the windows, while the TW3SE Menu is open:

- the red, green and blue arrows move the place along X, Y and Z
- the dot in the middle moves it on the ground plane
- the yellow ring turns the heading, the yellow line shows it (yaw 0 is north, 90 west)

It gives `changed`, the place, the heading and `active` (true while it is held). It keeps the size it has on
screen at any distance. Options:

| Option | What it does |
|---|---|
| `ground = true` | the dot keeps the place on the floor below the mouse |
| `snap = 0.5`, `snap_angle = 15` | steps in metres and degrees |
| `size = 1` | its size in metres |
| `move = false`, `lift = false`, `turn = false` | leave out the arrows, the Z arrow, the ring |

A click over a window belongs to the window. `w3.ui.over_window()` is true while the mouse is over one, for a
tool of your own that takes clicks in the world.

`w3.ui.draw.text(x, y, colour, s, size, bold)` draws text as Qt does at the sizes the fonts are made for (9 to 28
px). `w3.ui.draw.text_scaled(x, y, colour, s, size, bold)` draws it at any size, in grey shades: the next bigger
font's letters drawn smaller (a view that zooms).

## Settings in the TW3SE Menu

`w3.mod` puts your mod into the **Mods** tab of the TW3SE Menu, with its settings and, if you want, a tab of its own:

```lua
local cfg = w3.mod{
  name = "Better Wolves", version = "1.0", author = "you",
  description = "Wolves with a level of your choice.",
  settings = {
    {key = "enabled", default = true, label = "Enabled"},
    {key = "level", type = "slider", min = 1, max = 50, int = true, default = 10, label = "Wolf level"},
    {key = "mode", type = "choice", items = {"Calm", "Wild"}, label = "Mode", tip = "How they behave"},
    {type = "header", label = "Keys"},
    {key = "howl", type = "key", default = "F7", label = "Howl"},
    {type = "button", label = "Spawn a wolf", run = function() w3.spawn([[characters\npc_entities\monsters\wolf_lvl1.w2ent]]) end},
  },
  on_change = function(key, value, old) print(key .. " is now " .. tostring(value)) end,
  tab = function() w3.ui.text("Level " .. cfg.level) end,   -- optional: a tab of its own
}
if cfg.enabled then print(cfg.level) end
cfg.level = 20     -- sets it: kept, shown in the menu, on_change runs
cfg.reset()        -- every setting back to its default; cfg.reset("level") one
```

A setting has a `key`, a `label` (the key when left out), a `default` and a `tip` (a tooltip). The type comes from
the default when `type` is left out:

| `type` | What it shows | Its value |
|---|---|---|
| `bool` | a checkbox | true or false |
| `slider` | a slider from `min` to `max` (`int = true`: whole numbers) | a number |
| `number` | a number field | a number |
| `choice` | a drop-down list of `items` | one of the items |
| `text` | a text field (Enter takes the text) | a string |
| `key` | a button that waits for a key | a key name for `w3.on_key` |
| `header` | a heading over the settings below it | |
| `button` | a button that runs `run` | |

The checkboxes, sliders and choices also show in the game's own menu, **Options** > **Mods** > the mod's name, and
work with a controller there. A change in either menu shows in the other. TW3SE writes the page as
`bin\config\r4game\user_config_matrix\pc\tw3se_<name>.xml`; the game reads it at its start, so a new or changed page
shows from the next start. Number, text, key and button settings stay in the TW3SE Menu. `game_menu = false` leaves
the page out.

A `label`, `tip` or `description` can be a table by language, `{EN = "Wolf level", DE = "Wolfsstufe"}`. The menu
shows the game's language, else `EN`. `w3.lang()` gives the game's text and voice language (`"DE", "EN"`), `w3.tr(t)`
the text of such a table for your own windows.

`tw3se = "1.2.0"` names the TW3SE version the mod needs. On an older TW3SE the mod stops at `w3.mod` with "Better
Wolves needs TW3SE 1.2.0 or newer (this is 1.1.0)", in **F2** > **Mods** and in `tw3se.log`. `w3.version()` gives
the version as text and as three numbers (`"1.2.0", 1, 2, 0`).

The values are kept in `tw3se\vars.txt` and come back at the next start. A value set from Lua has to fit its setting
(a choice one of its items); a number outside a slider's range is set to the nearest end. `on_change(key, value,
old)` runs after each change, from the menu and from Lua. `w3.mods()` lists the mods `w3.mod` registered. A mod that is
taken out (`luamods`, a changed file) leaves the menu and comes back when it loads again; its values stay.
`lua\examples\mod_settings.lua` is a whole mod to copy.

### A menu for a WitcherScript mod

A `.ws` mod gets its settings into the TW3SE Menu with a small Lua file beside it,
`Mods\<your mod>\tw3se\init.lua`, that holds only the `w3.mod{...}` call. The scripts read each value
with `TW3SE_GetVar("mod/<name>/<key>")`, as text: `true` or `false`, a number, a choice's item, a key name. The
default is there too before the player changes anything.

```
import function TW3SE_GetVar(key : string) : string;

function BetterWolvesLevel() : int {
    return StringToInt(TW3SE_GetVar("mod/Better Wolves/level"), 10);
}

function BetterWolvesOn() : bool {
    return TW3SE_GetVar("mod/Better Wolves/enabled") == "true";
}
```

A change in the menu is there at the next read.

## Helpers

`tw3se\lua\lib\w3util.lua` has short forms for common jobs: `heal`, `give_money`, `weather`, `set_hour`, `teleport`,
`ground_z`, `raycast`, `near`, `nearest`, `spawn`, `give_item`, `fact` and more.

```lua
local u = require("lib.w3util")
u.give_money(500)
u.teleport(100, 200, u.ground_z(100, 200, 50))
```

`u.spawn` takes any template except the player's (`w3.player().template`). A second player entity takes over every
input listener of Geralt. Once it is gone, Sprint, Esc and attacks stay dead until the save is loaded again.

## WitcherScript calls Lua

A `.ws` mod can run Lua and read the result back:

```
import function TW3SE_Lua(code : string) : string;

exec function example() {
    var crowns : string;
    crowns = TW3SE_Lua("return w3.player():GetMoney()");
}
```

`TW3SE_Lua(code)` runs the code and returns its output or returned value as a string. So Lua calls WitcherScript
(through `w3.call` and methods) and WitcherScript calls Lua.

## Functions for WitcherScript, written in Lua

`w3.native(name, fn)` gives WitcherScript a new function. The script imports it like any native, and the
`import function` line sets the parameters and the return:

```lua
-- Mods\modMyMod\tw3se\natives\mymod.lua
w3.native("MyMod_Greet", function(name, who) return "hello " .. name .. " from " .. w3.classof(who) end)
```

```
import function MyMod_Greet(n : string, who : CObject) : string;
```

Files in `tw3se\lua\natives\` and in the `natives` folder of a mod run when the game starts, before the scripts
load, so the scripts find their functions right away. At that moment the engine is still starting: those files only declare functions. A `w3.native`
from a mod or the console is there for scripts after the next `reloadscripts force`. The same name again replaces
the Lua function at once.

A name with a class makes a method of that class. The Lua function gets the object first:

```lua
-- Mods\modMyMod\tw3se\natives\mymod.lua
w3.native("CActor.MyMod_Shout", function(actor, times) return times * 2 end)
w3.new_enum("EMyModMode", {MMM_Off = 0, MMM_On = 1})
```

```
@addMethod(CActor) import final function MyMod_Shout(times : int) : int;

function MyMod_Test() : int {
    var mode : EMyModMode;
    mode = MMM_On;
    return thePlayer.MyMod_Shout(21);
}
```

The class has to be one of the game's native classes. `w3.new_enum(name, {VALUE = number, ...})` makes an enum the
scripts use by its name and its value names, with no declaration in a script (a script that declares one of the same
name does not compile). Give the values a prefix of your mod: scripts name them without the enum. Both belong in a
natives file, so they are there when the scripts compile.

## Mods that load again

When you save a `.lua` file in a mod's folder (or a file in `lua\autorun\`), TW3SE takes the mod
out and loads it again within a second. Taking it out removes everything it added: its `on_tick`, `on_draw`,
`on_ui`, `on_reload`, `on_key`, `on_load` and `on_save` functions, its hooks, its `w3.run` sequences and the modules its `require` loaded. A new mod folder loads by
itself, a deleted one is taken out. The console command `luamods` lists the mods, `luamods reload <name>` loads
one again by hand. `lua_autoreload = off` in `tw3se.config` turns the watching off.

## Mods that use other mods

`w3.use(name)` gives a mod what another mod's `init.lua` returns. That mod loads first if it is still waiting for
its turn. `name` is the other mod's folder.

```lua
-- Mods\modWolfKit\tw3se\init.lua
local kit = {}
function kit.spawn_pack(n) for i = 1, n do w3.spawn([[characters\npc_entities\monsters\wolf_lvl1.w2ent]]) end end
return kit
```

```lua
-- Mods\modWolfNights\tw3se\init.lua
local kit = w3.use("modWolfKit")
w3.on_key("F7", function() kit.spawn_pack(3) end)
```

When the used mod loads again, the mods that use it load again after it. If the player lacks it, `w3.use` stops the
mod that asks with "mods/modWolfNights needs the mod modWolfKit (it is not installed)". For an `init.lua` that ends
with no `return`, `w3.use` gives `true`.

## Where the time goes

The console command `luaprof` lists the time each mod spends, split by kind (`on_tick`, `on_draw`, `on_key`, `run`,
`hook <function>`, `native <function>`), the most first:

```
lua time over 3600 tick(s), 60 s (own time, ms per tick / calls / slowest call):
  mods/mymod               on_tick                                 0.210      3600    1.940
  mods/mymod               hook CActor::GetHealth                  0.045     51200    0.012
```

Each line counts its own time: a hook that fires inside an `on_tick` function counts for the hook, and the game's
original under `orig()` counts for nobody. `luaprof reset` starts the count again. `w3.profile([reset])` returns the
same as a list of `{mod, kind, calls, ms, max, per_tick}` and the number of ticks.

`w3.script_profile("on")` counts the time of every WitcherScript function; `w3.script_profile()` returns them by
their own time, `{fn, file, mod, calls, own, whole}` (ms over the whole count), the seconds it counted and whether
it counts. `"off"` stops it, `"reset"` starts again.

## Examples

`lua\examples\` has small, complete mods: god mode, add money, set the weather, heal on a hotkey. Copy one into
`lua\autorun\` to run it at startup. `lua\tests\` shows the same calls as checks.

## Tests

`python tools/tw3se.py luatest` runs the Lua tests in `lua\tests\` inside the game and reports pass/fail. A test
file calls `test("name", function() ... end)` and asserts with `eq`, `check`, `near` and `is_error`.
