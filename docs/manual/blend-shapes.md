# Blend shapes

TW3SE moves the vertices of a mesh in the game with blend shapes. A shape is a list of offsets, one per vertex. You
load a file with any number of shapes onto a mesh and give each shape a weight from Lua. The graphics card adds the
weighted offsets to the mesh's original positions before the game draws it, so every pass of the game draws the
moved mesh, with its skeleton animation. With path tracing on, light and shadows follow the moved mesh.

A mesh in the game is shared by every entity that uses it. Loading shapes onto a mesh changes it on all of them. To
change one entity alone, give its component a mesh of its own first (see [One entity alone](#one-entity-alone)).

## From Lua

```lua
local wraith = w3.spawn([[characters\npc_entities\monsters\wraith_lvl1.w2ent]])
local head = w3.deform.meshes(wraith)["h_01__wraith"]          -- {name = component}

w3.deform.dump(head, [[deform\h_01__wraith.twdb]])             -- the original, for your tool
local set = assert(w3.deform.load(head, [[deform\h_01__wraith.twdf]]))
w3.deform.set(set, "inflate", 0.7)                             -- by name
w3.deform.set(set, 2, 0.3)                                     -- or by number (from 1)
w3.deform.weights(set, {inflate = 1, twist = -0.5})
w3.deform.unload(set)                                          -- the original comes back
```

| Function | What it does |
|---|---|
| `w3.deform.meshes(entity)` | the entity's mesh components by name |
| `w3.deform.load(mesh, path [, range])` | loads a shape file onto the mesh; returns the set, or nil and the reason |
| `w3.deform.set(set, shape, weight)` | one weight (shape by name or number) |
| `w3.deform.weights(set, t)` | several weights from a table |
| `w3.deform.names(set)` | the shape names in file order |
| `w3.deform.state(set)` | `"loading"`, `"ready"`, `"lost"` or `"none"` |
| `w3.deform.unload(set)` | puts the original positions back and removes the set |
| `w3.deform.dump(mesh, path)` | writes the mesh's original positions and normals to a file |

`mesh` is a `CMeshComponent` or a `CMesh`. A relative `path` starts in the `tw3se` folder
(`bin\x64_dx12\tw3se`).

Weights can change every tick (`w3.on_tick`). A changed weight shows in the next frame. The positions are stored in
a box around the mesh with 16 bits per axis. On load the box grows by `range` times the largest sum of offsets a
vertex has (`range` is 1 when left out). Set `range` to the largest weight you plan to use, for example 3 for
coefficients between -3 and 3. A weight beyond `range` can push vertices to the edge of the box, where they stop.

A set turns `"lost"` when the game unloads the mesh or loads it again. Load the set again to bring the shapes back.
One mesh holds one set at a time. Unload a mesh's set before `w3.deform.dump`.

## One entity alone

`w3.deform.own(mesh)` gives one mesh component a copy of its mesh. The copy has its own vertices on the graphics card,
so a set on it moves this entity only. The copy is ready a few ticks later; `w3.deform.loaded(mesh)` turns true
then.

```lua
local head = w3.deform.meshes(npc)["h_01__wraith"]
w3.deform.own(head)
w3.run(function()
  w3.wait_until(function() return w3.deform.loaded(head) end)
  local set = assert(w3.deform.load(head, [[deform\h_01__wraith.twdf]]))
  w3.deform.set(set, "inflate", 0.7)
end)
```

| Function | What it does |
|---|---|
| `w3.deform.own(mesh)` | the component gets a copy of its mesh; returns the copy |
| `w3.deform.loaded(mesh)` | true when the component's mesh is on the graphics card |
| `w3.deform.release(mesh)` | the component gets the shared mesh back (unload the copy's set first) |

Each copy is a second path to the game's mesh file (`<mesh>__own1.w2mesh`, `__own2`, ...), made with
`w3.files.alias`. The copies last until the game is closed. Release the copies before the game saves.

## Live meshes

A live mesh takes its positions and normals from your mod each frame, in place of shapes. `w3.deform.live(mesh)`
gives the component a copy of its mesh and returns an id. The id is `"loading"` in `w3.deform.state` until the copy
is on the graphics card, then `"ready"`.

```lua
local id = w3.deform.live(strand)
w3.on_tick(function()
  if w3.deform.state(id) ~= "ready" then return end
  w3.deform.live_write(id, 0, positions, normals)   -- {x, y, z, x, y, z, ...} for every vertex of chunk 0
end)
```

| Function | What it does |
|---|---|
| `w3.deform.live(mesh)` | the component gets a copy of its mesh; returns the live mesh's id |
| `w3.deform.live_write(id, chunk, positions [, normals])` | the chunk's positions (and normals) from now on, chunk from 0 |
| `w3.deform.live_info(id, chunk)` | the chunk's vertex count and the mesh's chunk count |
| `w3.deform.live_bounds(id, min, max)` | the box the positions are stored in, `{x, y, z}` each |
| `w3.deform.live_close(id)` | the component gets the shared mesh back |

Positions are in the mesh's model space before skinning, one per vertex in the order of `w3.deform.dump`. The
cooker splits a vertex where its corners have other UVs and puts the vertices in an order of its own, so take the
order from a dump of the cooked mesh. A live mesh turns `"lost"` when the game rebuilds its render resources or the
copy goes; close it and open it again then. A plugin writes faster than Lua (interface 26, `LiveMeshWrite`).

## Normals

A shape file can carry normal offsets too, one per vertex and shape. TW3SE adds the weighted normal offsets to the
original normal, makes the sum unit length and turns the tangent with it. A file with position offsets only leaves
normals and tangents as they were, so the light falls on a moved surface as it fell on the original one.

## Files

A mesh has chunks, one per material and level of detail. `w3.deform.dump` writes the vertices of every chunk in the
order the game stores them. A shape file gives offsets for the chunks you want to move. A chunk the file leaves out
keeps its shape.

`tools\twdf.py` reads and writes both kinds:

```
python tools\twdf.py info deform\h_01__wraith.twdb          chunks, LOD, vertex count, bounds
python tools\twdf.py demo h_01__wraith.twdb h_01__wraith.twdf
```

`demo` writes three shapes on every chunk to try it out: `inflate` (5 cm along the normal), `stretch` (30 % taller
from the middle) and `twist` (45 degrees around the vertical axis from bottom to top), with the normal offsets of
`stretch` and `twist` (`--no-normals` leaves them out). From Python:

```python
import twdf
base = twdf.read_twdb("h_01__wraith.twdb")         # {chunk: (mask, positions (n, 3), normals (n, 3))}
deltas = {0: my_offsets}                           # {chunk: array (shapes, n, 3)}, metres
deltas[2] = twdf.transfer(base, deltas, 0, 2)      # chunk 0's offsets on chunk 2 (a lower LOD), nearest vertex
twdf.write_twdf("h_01__wraith.twdf", ["smile", "blink"], deltas)
twdf.write_twdf("h_01__wraith.twdf", ["smile", "blink"], deltas, normal_offsets)   # same shape as deltas
```

Both formats are little endian.

Dump, `.twdb`:

| Field | Type |
|---|---|
| `"TWDB"`, version 1, chunk count | 4 bytes, u32, u32 |
| per chunk: index, LOD mask, vertex count n | u32, u32, u32 |
| positions | n x 3 float32, metres, the mesh's own space |
| normals | n x 3 float32 |

Shapes, `.twdf`:

| Field | Type |
|---|---|
| `"TWDF"`, version (1 or 2), shape count K, chunk count | 4 bytes, u32, u32, u32 |
| version 2: flags (1 = normal offsets) | u32 |
| K names | u16 length, UTF-8 bytes |
| per chunk: index, vertex count n | u32, u32 |
| offsets | K x n x 3 float32, shape by shape, metres, the mesh's own space |
| with flag 1: normal offsets | K x n x 3 float32, shape by shape |

The vertex count of each chunk in the file has to match the mesh. For a chunk with another count, `w3.deform.load`
returns nil and the chunk's number.
