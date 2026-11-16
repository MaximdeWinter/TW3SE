# World edits

Change objects that are placed in the world: remove a house, set it on fire, swap it for its burnt version, move a
cart. The change stays after a load. An edit can depend on a quest fact, so the house burns during one quest step
and is a ruin afterwards. When the fact changes, the edit switches while you play.

## world.txt

One edit per line, `#` starts a comment.

```
remove      <d0> <d1> <d2> <d3>                           [if <fact>] [unless <fact>]
effect      <d0> <d1> <d2> <d3> <effect>                  [if <fact>] [unless <fact>]
appearance  <d0> <d1> <d2> <d3> <appearance> [else <a>]   [if <fact>] [unless <fact>]
move        <d0> <d1> <d2> <d3> <x> <y> <z> <pitch> <yaw> <roll>   [if ...] [unless ...]
tag         <d0> <d1> <d2> <d3> <tag>                        [if <fact>] [unless <fact>]
light       <d0> <d1> <d2> <d3> on|off                       [if <fact>] [unless <fact>]
anim        <d0> <d1> <d2> <d3> <move> [back|loop]            [if <fact>] [unless <fact>]
removeclass <Class>
```

`<d0> <d1> <d2> <d3>` is the object's GUID.

- `remove`: the object disappears with its effects, light, sound, collision and interaction. When the condition
  ends, it comes back as it was
- `effect`: plays one of the object's effects
- `appearance`: switches its appearance. `else <a>` is the appearance it gets when the condition ends
- `move`: puts it somewhere else. When the condition ends, it goes back
- `tag`: gives it a tag. A quest or scene that names things by tag reaches it then: a candle for `SetLights`, a
  door for `DoorChangeState` and a scene's door event, an effect, a container. When the condition ends, the tag
  goes again
- `light`: turns a candle, a torch or a brazier on or off (its gameplay light, as the game's `SetLights`). When the
  condition ends, it switches back
- `anim`: plays one of the object's own moves, for example `raise` of a gate (its property animations, as the
  game's `PlayPropertyAnimation`). `back` plays it backwards, `loop` again and again. When the condition ends, it
  goes back (a loop stops). An object that comes into the world while the edit holds, for example after a load,
  is there at once
- `removeclass`: removes every object of a class
- `if <fact>`: only while the fact is above 0. `unless <fact>`: only while it is 0. Both together: "during the
  quest" (`if q_fire_started unless q_fire_out`)

A farmhouse that burns from step 2 and is a ruin after step 4:

```
effect     3f1a09c2 4b7d21e0 9c55e1a7 0d2e6f41 fire               if q_house_burning unless q_house_burnt
appearance 3f1a09c2 4b7d21e0 9c55e1a7 0d2e6f41 burnt else intact  if q_house_burnt
```

The effects and appearances depend on the object's template. If one is missing, the log says so.

## Where the file goes

TW3SE reads these at start and on every load:

| File | For |
|---|---|
| `dlc\<your dlc>\tw3se\world.txt` | a quest's edits |
| `Mods\<your mod>\tw3se\world.txt` | a mod's edits |
| `bin\x64_dx12\tw3se\world.txt` | the player's own edits and what the console adds |

Each mod keeps its own file. Uninstalling a mod takes its edits with it.

## Finding a GUID

- From a script: `TW3SE_EntityGuid(entity)`
- From the log: `worldedit_log = on` in `tw3se.config` logs class and GUID of the first 400 objects of every load

GUIDs like `ffffffff ffffffff <n> 00000000` belong to objects the game spawns while you play (crowd NPCs, items).
They change on every load.

## While the game runs

An edit added while you play reaches the object right away if it is loaded. The console's `worldedit` and the
script function `TW3SE_WorldEdit` work this way. A script gets any loaded object with `TW3SE_EntityByGuid`
([Script natives](script-natives.md)).

## Console

| Command | What it does |
|---|---|
| `worldedit <line>` | adds one line to `tw3se\world.txt` and applies it right away |
| `worldlist` | how many edits are loaded and applied, how many objects are loaded |
| `worldfind <d0> <d1> <d2> <d3>` | is this object loaded, its class and its effects |
| `worldremove`, `worldmove`, `worldeffect`, `worldappearance` | short forms of `worldedit remove ...` and so on |

## How it works

Every object passes `CEntity::AttachToWorld` when it enters the world. TW3SE hooks it and applies the object's edit
one tick later. Objects with a condition are checked twice a second while they are loaded.
