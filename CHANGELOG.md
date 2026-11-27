# Changelog

## Next

**New:**

- `w3.texture.write(path, dds)` puts the pixels of a DDS into a loaded texture while the game runs, and again
  whenever the streaming loads it anew; `w3.texture.release(path)`
- `w3.texture.stamp(path, posmap, layers)` puts tattoos and scars on a texture written with `w3.texture.write`, each
  layer placed through a position map of the mesh
- `w3.texture.eye(path, layers, look)` mixes an eye texture from layers (the white and its veins, the iris, a round
  or a cat's pupil) and puts it on the texture
- `crash_dump = on` in tw3se.config: a crash writes a minidump into `tw3se\out` (the memory round the crash, for a
  bug report)

**Fixes:**

- Shift held at the start turned on safe mode while another program was in front (a game started by a tool while
  the player sprinted in another game). Shift counts while the game, the desktop or a launcher is in front
- A mod switched on again in the game's **Mods** menu stayed off for TW3SE after game patch 5.02 (`Enabled=true` in
  `mods.settings`)
- A crash or a hang a few seconds into a new world after a mod used blend shapes or `w3.texture.write` (TW3SE gave
  its render meshes and textures back on a path that left them in the renderer's list)
- A mesh in `tw3se\files` with its `.1.buffer` files beside it stayed invisible
- Lua: an array or a struct taken from an object that is gone gives an error and a line in `tw3se.log` (writing into
  it changed memory the game had handed on)
- An object made with `w3.new` and handed to `w3.engine_call` belongs to the game from then on (Lua discarded it
  while the game still used it)
- A part made anew (another world, a new head or hairstyle) kept the shared mesh after `w3.deform.own`, and
  `w3.deform.release` gave it the old world's mesh
- The crash report names the callers of a call through a null pointer (`?+0x0`)
- The overlay takes half the time per frame on the CPU (a frame's rectangles and text in one draw per texture)

## 1.4.3

**New:**

- Runs on the game's patch 5.02 (5.01 keeps working)
- With another window in front the game draws at most 20 frames a second (`background_fps` in tw3se.config, 0 =
  off; `w3.background_fps(n)` while the game runs)
- `w3.world.objects(pos, radius)`: the merged meshes and the entities around a point, each with an id that stays
  the same after a load (`world:417063`, `entity:<guid>`), its mesh path, place, rotation, box and whether it is
  loaded
- `w3.world.object(id)` gives an object by its id, `w3.world.pick(from, to)` the object a ray hits first
- `w3.anim.reach(actor, {hand, target, weight})`: a hand reaches a point in the world on top of any animation
  (two-bone IK on the arm, blended by weight)
- `w3.world.unstream(id)` takes a merged mesh out of the world at once, with its collision, and it stays out while the
  world streams; `w3.world.restream(id)` brings it back
- `w3.highlight.add(target, {color = 0xFFFF4FB4})`: an outline in the Witcher senses on any entity or merged mesh
  (furniture, walls), in a colour of your own or the game's red or yellow; `remove`, `clear`, `list`
- `w3.patch(place, bytes)` and `w3.unpatch(handle)`: byte patches by engine name, offset or signature, taken back
  with the mod; `w3.find_code(signature)`
- `w3.material.assign(component, index, material)`: another material into one slot of one entity's mesh
- `w3.cloth.of(entity)`, `simulate`, `freeze`, `reach`: a figure's cloth
- Textures of your own: a DDS under a path the game has no texture for is a new texture, with an `.xbm` from
  `tools/new_texture.py` beside it
- `w3.deform.hide(component, bits)` hides triangles of a mesh (one bit per triangle, per chunk);
  `w3.deform.unhide` shows them again, `w3.deform.hide_file` reads the bit fields from a file
- tw3se.log names the graphics cards with their drivers and the other tools in the game: ReShade and its add-ons,
  OptiScaler, overlays, ASI mods (`environment_log = off` leaves it out)

**Fixes:**

- The console pipe takes programs of this PC only (a named pipe can be reached over the network otherwise)
- Blend shape sets on a mesh copy of its own ended after `w3.refresh` on its component
- With OptiScaler as dxgi.dll the game hung as soon as a plugin drew. TW3SE made a swap chain of its own for the
  present hook while the game ran, and OptiScaler took it for the game's. That swap chain is made at the start now
- ReShade add-ons that put their settings into the game's root signatures (RenoDX) skipped the main one while
  `skin_hook = on` was set (a black picture with RenoDX in HDR). The skinning hook binds its slot as a constant buffer
  view now
- With CWSO the game still froze at times while a plugin moved a mesh's vertices (a second place where TW3SE and
  CWSO waited for each other)
- With `overlay = off` the skinning shaders drew with their parameters unbound. The skinning hook stays off then
- With `skin_hook = off` every pipeline with a skinning shader was refused once and made again. Those shaders stay
  out of the pipelines now, and a replaced shader the graphics driver refuses stays out from then on
- A guarded read in the pose hook showed up as an error in crash reports

## 1.4.2

**Fixes:**

- After a crash, tw3se.log names the place of the crash and the functions that called it. Before, only
  crash_report.txt had them, written at the next start
- A crash is in the crash report once (it was there twice)
- The log calls the game version "found by signature". It said "unknown build", which read like a wrong game
  version
- `CSkeletalAnimationContainer::FindAnimation` is found in the game version 5.01. The `w3.anim` functions use it
  (they had a slower lookup of their own)

## 1.4.1

**Fixes:**

- A mesh whose vertices a plugin writes each frame (LiveMeshWrite) flickered: black spots, the world behind it
  showing through. The vertices change once a frame now, before the frame is drawn
- With Cleaner Witcher Senses Outlines (CWSO) the game froze in Witcher Senses while a plugin moved a mesh's
  vertices (blend shapes, live meshes)

## 1.4.0

**New:**

- `w3.engine_call(fn, sig, ...)` calls an engine function with integers, addresses, objects, strings, floats and
  doubles (up to 16 arguments); `w3.engine_addr(name)` gives its offset
- Engine functions by more names: a slot of a class's vtable (`"CEntity::vf140"`, found by the class's name in the
  exe) or an offset in witcher3.exe (`"0x2263570"`), for `w3.engine_hook`, `w3.engine_call` and plugins
- Interface 27: `EngineAddress(name)` and `EngineVtable(className, offset)`
- `tools/enginemap.py` (in the SDK): every function, C++ class, vtable slot, call and text of witcher3.exe, searchable
- `w3.anim.create(actor, name, anim)`: an animation for a figure's rig made at run time, from bone names and frames
  (or float data read from a file), without cooking. The engine samples it like its own
- `w3.anim.run(actor, name, opts)` plays an animation on a figure (loop, speed, weight), `w3.anim.stop(h)` ends it
- Made animations are found by name like the game's own: `w3.anim.play` (slot animations) plays them
- `w3.physics.body{...}`: bodies of your own in the world's physics (box, sphere, capsule; dynamic, kinematic or
  static), with `w3.physics.pose`, `move`, `push`, `speed`, `remove` and `clear`
- `w3.physics.joint(kind, a, b, opts)`: joints between bodies or to the world (ball, hinge, fixed, slider, rope with
  length, spring and break force)
- `w3.mem.alloc`, `read`, `write`, `free`: memory for `w3.engine_call`'s structs and out values. Engine functions
  also by the text they print (`"text:..."`)
- `w3.figure.parts(actor)`, `part`, `show`, `mesh`: a figure's look in parts (its own and its mounted items'), on,
  off and with another mesh of the same skeleton
- `w3.material.own(entity)`: one entity's materials of its own, so a material change counts for it alone
- `w3.anim.shape(actor, bones)`: a figure's proportions by bone (scale, length, offset, turn; a scale for the whole
  part below a bone or for the bone alone), on the figure every frame
- `w3.anim.record(actor)` and `w3.anim.record_stop(actor, fps)` record a figure's pose as an animation;
  `w3.anim.save(path, anim)` and `w3.anim.load(path)` keep it as a file
- `w3.figure.look(actor)` and `w3.figure.apply(actor, look)`: a figure's look (shapes, parts, added parts) as a
  table to keep with `w3.store` and put back
- Dual quaternion skinning for a skinning hook slot: `w3.skinhook.dqs(slot, true)`, interface 28
  `SkinHookDqs(slot, on)`. Joints that bend far keep their volume; bone scale works as in the game. With ray
  tracing on, the ray traced picture uses it too
- Interface 29, the skinning hook: 256 slots. `SkinHookOverlayRegion` puts several pictures side by side into the
  skin overlay (a body, a head, a part with a UV of its own), up to 1448 x 1448 pixels. `SkinHookChunkSlot` draws one
  chunk of a mesh with a slot of its own (a part whose UV overlaps the rest of the mesh)
- `tw3se\shaders\blend.txt` changes how a pixel shader's pipelines blend (`<shader id> <target> alpha | alpha_all |
  keep`)
- `w3.figure.preview(actor, opts)` and `w3.figure.preview_end()`: the camera and a light on a figure, around it by
  yaw
- `w3.anim.bones(actor)` lists a rig's bones; `w3.anim.shape` on a head's `CMimicComponent` shapes the face (nose,
  chin, eyes, cheeks, 180 bones on Geralt)
- `w3.physics.parts(actor)`: a figure's bodies by bone name, for poses and joints (a rope at an NPC's hand)
- `w3.figure.add(actor, mesh or template)` and `w3.figure.remove(handle)`: new parts on a figure that follow its
  skeleton (clothing, hair, armour from any mesh or item template)
- `w3.items.add` and `w3.items.ability` work at once while the game runs; a second call changes the item
- `w3.items.mount(path)` and `w3.items.unmount(handle)` load an item XML of your own while the game runs
- New loose files in `tw3se\files\` and `w3.files.set` on a new path are in the game at once (in a folder the game
  has)

## 1.3.4

**New:**

- Interface 26: live meshes. A plugin writes a mesh's vertex positions and normals, every frame if it wants, and the
  game skins and draws them in every pass (`LiveMeshOpen`, `LiveMeshWrite`, `LiveMeshBounds`, `LiveMeshInfo`,
  `LiveMeshState`, `LiveMeshClose`). Lua: `w3.deform.live`
- Interface 26: `TextureWrite` puts a plugin's pixels into a loaded texture; TW3SE encodes them to the texture's
  format (BC3, RGBA8, BGRA8)
- `tools/xbm_placeholder.py` makes a texture of any size to paint with `TextureWrite` (a loose file; each
  `w3.files.alias` of it is a texture of its own)
- Interface 26: `DeformNormals` - the normals of a blend set or a live mesh follow its moved triangles
- Interface 26, the skinning hook: 32 capsules with a soft edge or drawing the skin in (`SkinHookCapsulesEx`; the
  skin's normal turns with the dent), Delta Mush on a slot's mesh (`SkinHookDeltaMush`, `SkinHookMushMask`), a
  picture over the skin (`SkinHookOverlay`) and its material (`SkinHookOverlayMaterial`: wet, matte, height).
  `tools/skin_build.py` makes the shaders for them (82 skinning shaders, 8 skin pixel shaders)
- Interface 26: `interfaceSize` and `TW3SE_HAS(api, Function)` tell which functions the running TW3SE has
- The TW3SE Menu, the console and Lua windows show Cyrillic letters (Russian, Ukrainian, Belarusian, Serbian,
  Bulgarian)
- `tw3se.log` names the Microsoft Store version of the game in its first line

**Fixes:**

- In some games F2 froze the game: the menu never came on screen and still took the keyboard and the controller,
  and a second F2 kept it open. The menu now takes the input once it is on screen. A menu that is not drawn after
  20 seconds closes again, and `tw3se.log` says where the overlay stopped
- Texts of `tw3se\strings` (and the item names of `w3.items`) were missing where Flash asks for a text by its key
  (`[[key]]`, the loot feed on the left). They are there now, and `GetLocStringByKey` finds them too
- After a switch of the text language in the menu, the texts of `tw3se\strings` stayed in the old language until
  the game started again. They follow the switch within a few seconds now
- `w3.key_down` and `w3.on_key` report no keys while a text field has the keyboard (typing in one mod's field
  turned on another mod's key). F1 to F24, the mouse and the controller still count
- Clicking through the graphics options at speed (frame generation, the upscaler) crashed the game seconds later.
  TW3SE applies the changed options together, once the clicks pause for a second or the options are left
  (`graphics_apply = live` in `tw3se.config`: each click at once, as before)
- With DLSS Frame Generation, the game could hang at F2 while a mod had blend shapes loaded. TW3SE's blend shape
  work goes to the game's own queue now
- With DLSS Frame Generation, a resize of the game's window at the wrong moment turned the overlay off until the
  game started again

**Changes:**

- What TW3SE keeps after a graphics option is applied (1.3.3) goes back 60 seconds after the last switch of a
  series (`rebuild_fix_seconds`)

## 1.3.3

**New:**

- World edit `tag <guid> <tag>`: an object the game placed gets a tag. A quest or scene that names things by tag
  reaches it then: a candle for `SetLights`, a door for `DoorChangeState` and a scene's door event, an effect, a
  container
- World edit `light <guid> on|off`: a candle, a torch or a brazier of the world turned on or off. With `if <fact>`
  it follows a fact a scene sets at a second
- World edit `anim <guid> <move> [back|loop]`: an object of the world plays one of its own moves, for example a
  gate going up
- `CallFunctionV` / `CallMethodV`: `e:N` passes the value of an enum (0 to 255)
- Interface 25: `RegisterOnRenderRebuild` - a plugin hears before and after the game rebuilds its render resources
  (a graphics option applied)

**Fixes:**

- Switching the upscaler in the game's options crashed the game a few seconds later, more often with mods. The game
  frees render data while lens flares, the physics and render proxies still read it. TW3SE keeps what the game
  frees in that moment (about 2 MB a switch)
- Switching FSR Frame Generation on or off crashed the game at once, in the main menu too. The game makes a new
  swap chain then, and the overlay (TW3SE Menu, plugins that draw) still held the old one. The overlay now lets go
  of it first and is made anew on the new one. Switching XeSS Frame Generation while you play hangs the game. With
  `overlay_new_swapchain = off` in `tw3se.config` it keeps running, and the overlay stays off until the game starts
  again
- TW3SE's own calls of a method written in WitcherScript (`AddTag`, used by the world edits) now run it. Before,
  such a call reported success and the method never ran
- Conditional world edits follow a fact within a quarter of a second (was half a second)

## 1.3.2

**Fixes:**

- A graphics option switched while a mod had blend shapes loaded (`w3.deform`) left the blended vertices in the
  mesh. The mod's next set took them for the original and added its shapes once more, and the body got more dents
  with each switch. The mesh now gets its original vertices back before the switch
- A blend set that waited for the game's first draw of its mesh drew the mesh out of shape for a few frames. When
  the set ended in that time, the mesh stayed so
- An object made with `w3.new` and passed to a function of the game (an AI action to `ForceAIBehavior`) was
  destroyed when Lua let go of it, while the game still used it. The game crashed a few seconds later (seen after a
  scene's end). Such an object now belongs to the game, as one put into a pointer field

## 1.3.1

**Fixes:**

- TW3SE's guards stay in front of every vectored exception handler of the game. A driver that adds and removes its
  handler all the time (AMD, `atcuf64.dll`) took all 32 places after a few seconds, and each further handler was a
  line in `tw3se.log` (400 000 lines in an evening). A removed handler now frees its place, and the log names a
  handler once
- A plugin's pose callback could get a mesh whose bone table and matrices lie elsewhere, every frame (seen in a game
  with path tracing). TW3SE read the wrong place for it (each read caught, about 77 a second) and handed the plugin
  what it found there to write into. Such a mesh is now left out of the pose callbacks, and `tw3se.log` names it
- Switching path tracing in the game's options crashed the game while a mod had blend shapes loaded
  (`w3.deform`). Before a graphics option is applied, TW3SE now ends every blend set, and the mod loads it again
  (its state is `lost`, as after the game unloads a mesh). `w3.deform.on_reset(fn)` is called then
- A blend set on a mesh loaded a moment before (a body copied for a scene) could take the mesh's vertices before
  they had reached the GPU, and the body stood in spikes (seen on an AMD card). A new set now reads the mesh after
  the game has drawn it
- The pose hook read a component's skeleton at a wrong place hundreds to thousands of times a minute (each read
  caught, 46 000 in ten minutes in one game). It checks the place first now
- A Lua hook that gave back a value where the game expects none wrote it to address 0 (caught, about 370 times a
  second while a save loaded with one comfort mod). TW3SE leaves such a value out
- A second press of the menu key soon after the game's start closed the menu again before it was on screen (the
  overlay draws its first frame up to 10 s after the game ticks). The menu stays open until it is drawn, and
  `tw3se.log` names each press of the menu key with what it did

**Changes:**

- The switch **Solid hair with ray tracing** (`rt_hair_fix`) is gone. It drew all hair harder, and the game's own
  hair is back

## 1.3.0

**Crash resistance:**

- A memory fault inside a `w3` function ends that call with a Lua error that names the place. Before, it switched
  every Lua mod off for the rest of the session
- Lua code that runs longer than 5 seconds at a time (an endless loop) is stopped with an error of its mod
  (`lua_time_limit` in `tw3se.config`). The console's `lua`, `luafile` and `luatest` run as long as they need
- All Lua mods together use at most 1 GB (`lua_memory_limit`). Above it, the code that asks gets "not enough memory"
- A hook or a Lua native that runs into one of these limits is taken off; the game's call passes it by
- A plugin that crashes in `TW3SE_PluginQuery` or `TW3SE_PluginLoad` stays off, and the game starts. A line over the
  game says so
- A plugin native that crashes ends that call; the script goes on with the default value, and the native stays off
  until its plugin loads again
- The faults TW3SE's guards take now include a division by zero and a bad instruction, past the game's crash handler
- TW3SE is built with `/EHa`: when a guard takes a fault, the C++ objects of the frames it leaves are freed and
  their locks released
- A plugin's console command that crashes answers with an error; an `.asi` mod that crashes in `InitializeASI` is
  logged, and the game starts

**Fixes:**

- With ray tracing and DLSS, FSR or XeSS, Roach's tail showed what lies behind it. TW3SE holds the game's
  `HairStochasticGBuffer` off: **F2** > **Mods** > **TW3SE** > **Solid hair with ray tracing** (on), `rt_hair_fix`
  in `tw3se.config`
- While a save loads, the watchdog waits `start_hang_seconds` (180) before it writes a hang report, in the world
  still 8 s. Every load had left one or two reports in `tw3se\out`, and one was taken for the reason a game ended
- `w3.camera.to_screen` and `w3.view.to_screen` gave nearly the same pixel for every point of a table `{X, Y, Z}`:
  the game took it as a direction. A position from the game (`GetWorldPosition`) was right before too
- `w3.anim.rest_bones(path)` named in the main menu (an autorun) did nothing after a save loaded: the world uses a new
  object of the rig. TW3SE now follows the path after every load. It also fills when the engine runs only the start
  of a dangle's update
- Crash after a figure with an own mesh copy and blend shapes (`w3.deform.own`) was destroyed, often only minutes
  later. The set wrote into the copy after the game had freed it. A set now holds the mesh until it ends, and the
  sets of a copy end at the next tick when its figure goes or its component gets another mesh
- `w3.every` stood still after a save loaded, as long as the engine time had gone back (up to a minute)
- The first press of the menu key froze the game for 6 to 7 seconds. The menu opens at once. Text in Qt's style
  (`w3.ui.draw.text` with a size) comes a few seconds later, in ImGui's font until then, and takes 100 MB less
  memory

**Changes:**

- The mod list shows a plugin with the name of a Lua mod in that mod's entry
- `w3.anim.rest_bones(rig, {names})` gives bones a mod adds to a figure's own rig their rest pose on their parents
- `w3.world.cast`, `floors`, `stand`, `walk`, `fits`, `free_spot` and `heightmap`: where a person stands and walks and
  where a box fits, from every hit of a ray and what it hit
- `w3.world.merged(pos, radius)` lists the houses, walls and rocks the game merged, with their mesh, box and turn
- `w3.world.place(mesh, pos, rot)` puts any mesh of the game into the world with its collision, from its path
  alone; `w3.world.move`, `w3.world.remove`
- `w3.ui.table` takes column widths, sorting by a click on a header (`w3.ui.table_sort`) and a height in which it
  scrolls under a fixed header; `w3.ui.rows` draws only the rows on screen. `w3.ui.selectable` spans a table row
  and fits its text. New: `w3.ui.select` (Ctrl and Shift selection), drag and drop (`w3.ui.drag`, `w3.ui.drop`,
  `w3.ui.dragging`), right-click menus (`w3.ui.context`, `w3.ui.menu_item`, `w3.ui.menu`), `w3.ui.tooltip(fn)`,
  `w3.ui.icon` (80 icons of Windows' icon font), `w3.ui.align`
- `w3.ui.gizmo` moves a place in the world with the mouse and turns its heading (arrows, a dot for the ground, a
  ring); `w3.ui.over_window`
- `w3.face.set(actor, {track = weight})` moves a face from Lua, every frame, in place of the game's weights or on top
  of them (`"add"`); `w3.face.clear`, `w3.face.tracks`
- `w3.declare(name, "int, name -> bool")` gives a native that is registered in C++ alone its parameters, so
  `w3.call` can pass them (`ExampleDouble`, `GetGame`, `IsPointFree` ...)
- `w3.engine_hook(name, fn)` runs a Lua function before an engine function of the address list, with its register
  arguments; `w3.engine_unhook`, `w3.engine_hooks`
- `w3.defaults(class)` gives the default values of a class's fields; `w3.discard(obj)` lets an object of `w3.new` go
  at once
- `w3.scene.stop()` ends running scenes, quest scenes too (before, a scene from `w3.scene.play` ran to its end)
- `w3.scene.time()`, `w3.scene.pause(on)` and `w3.scene.seek(t)` hold a running scene and move it through its section,
  forwards and backwards, with poses and camera as in playback; `w3.scene.play(path, input, {at = t, paused = true})`
  starts there. They keep the first running scene while it plays, so a tick handler with them costs little;
  `w3.scene.time()` gives nil for a scene that was stopped
- `w3.scene.elements()`: the lines, pauses and choices of the current section with their start, length and time,
  and the one playing now
- `w3.anim.sample(actor, name, t)` reads any animation of a figure's sets at any second, through the game's own
  sampler; `w3.anim.pose(actor, {{name, t, weight}, ...})` lays a blend of them on the figure every frame, inside or
  outside a scene, until `w3.anim.pose_clear`
- `w3.view.rect(x, y, w, h)` draws the game's whole picture into a rectangle of the screen, for an editor with panels
  around it; TW3SE's UI stays full screen. `w3.view.to_screen(pos)` gives the pixel inside it
- `w3.native("CActor.MyFunc", fn)` makes a method of a native class for WitcherScript (`@addMethod(CActor) import
  function ...`); the Lua function gets the object first
- `w3.new_enum(name, {VALUE = number})` makes an enum the scripts use by name
- `w3.anim.add_set(actor, set)` gives a figure in the world one more animation set; `w3.anim.remove_set`
- `w3.watch_latent(name, fn)` sees a latent WitcherScript function start (its arguments) and end (its result); a
  latent call from Lua gives its result as `th.result`
- `w3.anim.rest_bones(rig)`: the bones of a dangle rig the dangle leaves alone get their rest pose (a mod's own bones
  in a dangle rig flew apart)
- Plugins (interface 24): a mesh skinned to a dangle rig reaches the pose callback as OTHER, with the rig's bones;
  `GetComponentPose` reads one component's pose, a dangle's too; `w3.pose(component)` in Lua. A watched figure takes
  along every entity hung on it, not only items
- `w3.world.terrain_raise(pos, radius, metres)` raises or lowers the terrain's picture; `w3.world.terrain_height(pos)`
- The console command `posecombines` lists the meshes the pose hook sees: component, skeleton, owner
- `tw3se\out` keeps the crash reports of the last ten crashes (`crash_report_<date>_<time>.txt`); the newest is
  `crash_report.txt`
- After a game patch TW3SE starts when it finds its 13 base functions (main tick, names, RTTI, native registration).
  A feature whose function the patch changed stays off, and `tw3se.log` names it (`addresses: not in this build`).
  Before, one missing function of any feature kept all of TW3SE off
- The first line of `tw3se.log` names the store of the game: Steam, GOG or Epic
- SDK: `example\from_bridge.cpp` is Witcher Native Bridge's own example as a TW3SE plugin, each native beside the
  bridge's lines, with the bridge's script unchanged (`from_bridge.ws`)
- `w3.text.set` gives a text to a string id of a mod's own (checked: scripts read it with `GetLocStringById`)
- `w3.world.sector` and the other sector functions find a layer at once (each call went through every object of the
  game, and a loop over the layers stood the game still for minutes)

## 1.2.2

**Fixes:**

- Blend shapes and the mod.io script protection work again after the game update of 29.10. TW3SE finds both in the
  game's code now
- The game could end itself a few seconds after `w3.deform.release` right after `w3.deform.unload` on a copy. The set
  now ends with the copy
- Crash in `w3.deform.load` and `w3.deform.dump` when TW3SE reads a wrong vertex buffer. Both now return nil and the
  reason
- The pose hook's item search spreads its work over the frames. With a watched figure the game stalled every 5
  seconds (up to 64 ms on 1.2.0), and the figure's head item lost its callback for a frame each time

**Changes:**

- Plugins load and drive blend shapes (interface 22): `DeformOwn`, `DeformLoad`, `DeformWeights` (from the pose
  callback too), `DeformState`, `DeformUnload`, `DeformRelease`, the same sets and copies as Lua's `w3.deform`
- `WatchPose` and `WatchMimic` on a component or an item entity fire for it alone, and the item search stays off.
  Watch Geralt's head item or its CMimicComponent to get his face alone
- `GetPose` with a watched component reads the pose of its entity. `WatchMimic` alone is enough for
  `TW3SE_POSE_MIMIC`
- The log names an object given to `WatchPose` or `WatchMimic` that is neither an entity nor a component
- The console command `poseitems` shows what the item search costs
- `w3.scene.on_choice(fn)` changes the text of a dialogue's choices, as `w3.scene.on_line` does with a line
- The Lua docs on `w3.text.set` corrected: a story scene's lines keep the game's text (`w3.scene.on_line` changes
  what a line shows)

## 1.2.1

**Fixes:**

- A mod whose scripts compiled without TW3SE stopped the game's start with TW3SE (a `Log` or `LogChannel` call with
  wrong arguments). TW3SE now compiles the scripts once more without statement lines, and the game starts
- After scripts that did not compile at the start, the game's compiled scripts stayed moved aside, also after
  removing TW3SE. TW3SE now puts them back at once
- Crash a few seconds after a Lua call with its out array left off (`inv:GetAllItems()`); the array now comes back
  as an extra value
- A hitch of about 120 ms every one to three seconds with a plugin that uses the pose hook. TW3SE read the whole
  memory map of the game on the main thread for each unknown address
- Hang reports filled `tw3se\out` (loading a save can stand 8 s: 1 to 3 reports per start). The newest 20 stay
- Crash seconds to minutes after spawning an NPC from `w3.anim.with_sets`. An object from `w3.new` in a pointer
  field or an array of pointers now stays

**Changes:**

- Mods that declare the same struct, class or field are named in `tw3se.log` at the start, before the game's
  compile stops ("already defined", "already exported"); the error lines name both files too
- Mods that bring the same game script file are named in `tw3se.log` at the start (the game takes one, the other
  mod's changes are missing: "Could not find function"); Script Merger fixes that
- When the game's scripts do not compile, a message box shows the first errors and the mods they come from
  (`script_error_message = off` leaves it out)
- The log names each compiled-scripts file the game loads (REDkit and mod.io mods); when one does not fit, the crash
  report names that mod
- The crash report and Copy report list the mod.io and Steam Workshop mods
- Lua hooks come off while the game adds the scripts of mod.io mods at the main menu, and go back on after it (a
  crash report with Witcher's Essentials pointed there)

## 1.2.0

**New:**

- Mods made for Witcher Native Bridge work with TW3SE alone
- Mods that bring their own ASI loader (dinput8.dll) work together with TW3SE
- Plugins and .asi mods can draw their own windows (Dear ImGui)
- .asi mods can use TW3SE through one header file
- Mods can send messages to each other (scripts, Lua and plugins)
- Scripts can keep values in the save and send web requests
- Mod texts from simple text files, in every language of the game
- Plugins can change the game's shaders (what mods used ReShade for)
- Plugins can read the game's state (menu, loading, scene) and the controller
- A tool that lets two mods change the same script file (`tools\to_annotations.py`)
- A report when the game hangs at the start
- More in the crash report (what Windows saw, which module ended the game)
- Manual pages: For mod authors, Coming from another tool, Addresses
- SDK: a Visual Studio project to start a plugin

**Fixes:**

- Crash after loading a save with Witcher Native Bridge mods (Beardmo's Immersive World)
- Crash when changing the resolution in the options
- Vortex installed TW3SE into the wrong folder
- The Epic version closed at the start, before the first frame
- After the start the game window stayed in the background (or minimized) until you clicked it
- With `overlay = off` the TW3SE Menu opened unseen and blocked keyboard and mouse
- With `console = off` the controller combination still opened the TW3SE Menu
- A Lua mod's panel covered the TW3SE Menu
- **F2** > **Mods** listed empty mod folders
- **Options** > **Mods** showed raw ids for missing translations
- **Options** > **Mods** opened only the first 10 mod pages (from the 11th mod on, each page sits in a submenu)
- A mesh swapped with `w3.set_mesh` stayed in the world when the game removed it

**Changes:**

- The game window comes to the front at the start (switch in **F2** > **Mods** > **TW3SE**)
- **Copy report** includes the last crash report
- Plugin natives take and return strings and names
- Plugin interface 21

## 1.1.0

- `w3.on(name, fn)`: the game's events for mods with what belongs to them: `hit`, `death`, `item_added`,
  `item_removed`, `consume`, `level_up`, `sign`, `combat_start`, `combat_end`, `fact`, `spawn`, `scene_start`,
  `scene_end`, `quest`, `world`, `hour`, `load`, `save`. `w3.off(h)`, `w3.events()`, `lua\examples\events.lua`
- A Lua mod's checkboxes, sliders and choices also show in the game's **Options** > **Mods**, from the next start;
  a change in either menu shows in the other (`w3.mod{game_menu = false}` leaves the page out)
- After a crash, the next start writes `tw3se\out\crash_report.txt`: how the last session ended, the first faults,
  the mods and the last session's `tw3se.log`. Also when the game closed itself because the scripts failed to compile.
  The console command `crashtest` crashes the game to try it
- A compile error that names a `TW3SE_` function missing in this TW3SE says in `tw3se.log` that the mod needs a
  newer TW3SE
- A mod's TW3SE part is one folder, `Mods\<modName>\tw3se\` beside its `content` (or `tw3se\mods\<name>\` as
  before): `init.lua`, its modules, `natives\` and `files\`
- `require` finds a mod's own modules first (`require("ui.panel")` is `ui\panel.lua` in its folder), each mod its own
- Loose files from the `files` folder of each mod. The same file from two places: the log names both
- A mod switched off in `mods.settings` is off for TW3SE too
- Types of every `w3.` function for the editor in `tw3se\lua\types\w3api.lua`; the SDK has them with the game's
  classes (`types\w3.lua`), `tools\gen_types.py` and a mod template (`lua-mod-template`)
- The manual page "Your first Lua mod": from the template to the zip for Nexus Mods (`tools\pack_mod.py` packs it)
- `w3.mod{tw3se = "1.1.0"}`: on an older TW3SE the mod stops and names the version it needs. `w3.version()`
- `w3.use("modOther")`: what another mod's `init.lua` returns, that mod loaded first; the mods that use it load
  again after it
- **F2** > **Mods** shows a mod's last error while it ran too (a tick function, a hook). The first error of a mod shows
  a line over the game for 8 s (`error_notice = off` in `tw3se.config` leaves it out)
- A Lua mod in `Mods\<modName>\tw3se` is one entry in **F2** > **Mods**
- **F2** > **Mods** shows the Lua mods that use more than 1 ms of each frame, with their time
- `w3.debug.watch(label, fn)`: live values over the game while a mod is built; `w3.draw.size(text)`
- Examples on the events: damage numbers, bounty, kill stats, rest anywhere, sign refund, toxicity warning, dev watch
- `docs\api.html`: every `w3.` function on one page with a search field (`tools\gen_api_html.py`)
- `tools\bundle_extract.py`: game files out of the bundles by a path pattern (`--list` for the paths); in the SDK
- `w3.ai.follow` and `w3.ai.companion`: an NPC follows the player with the game's follow tree and fights beside them; `lua\examples\companion.lua`
- Safe mode: Shift held while the game starts (or `safe_mode = on`) starts TW3SE without Lua mods and plugins. Scripts that import their functions still compile (those functions do nothing then). The crash report names the Lua mod that ran at the fault
- After a game patch that TW3SE does not support yet, a message at the start says so, once per game version (`passive_message = off`)
- The WitcherScript profiler: **F2** > **Console** > **Profiler** > **WitcherScript** (or `scriptprof`) shows the time of every script function and its mod, to find the WitcherScript mod that makes the game stutter. `w3.script_profile`
- The crash report names the function a fault lies in: its start (the same for every player of a game version) and, for a native of the script system or an engine function TW3SE knows, its name
- `w3.input.action{name, key, pad, label, on}`: an action of a mod in the game's **Options** > **Key Bindings**, bindable there with keyboard and controller (in the menu from the next start, its own key works from the first)
- `w3.items.add{...}` and `w3.items.ability{...}`: items and abilities of your own, with name and description by
  language; the game has them from the next start (a DLC of TW3SE's, `dlctw3se`)
- `w3.store()`: a mod's table across every save and new game (`tw3se\store\<name>.lua`)
- `w3.hud.ask(text, fn)`: the game's own message window with Yes/No, OK/Cancel or OK; `fn(yes)` gets the answer
- The inspector's **Pick**: the object under the mouse in the world opens with a click
- **F2** > **Mods** > **Conflicts**: the scripts and game files more than one mod brings, which mod counts, and
  **Let ... win** (its Priority in `mods.settings`)
- **F2** > **Mods** > **New mod**: a mod of your own from the template or an example, in an editor window in the
  game; **Save** loads it again and shows its error. **Edit** and **Open folder** for every Lua mod.
  `w3.ui.input_multiline`
- `w3.mod` labels, tips and descriptions by language (`{EN = "Wolf level", DE = "Wolfsstufe"}`); `w3.lang()` and
  `w3.tr(t)`
- The controller: its buttons are keys for `w3.on_key` and `w3.key_down` (`Pad_A`, `Pad_LB+Pad_Y` ...) and for the key
  fields of the TW3SE Menu. Both sticks held for 0.5 s open and close the menu (`menu_pad`), the controller moves
  through it, and while it is open the controller works only there
- **F2** > **Mods**: the pin next to a mod's name puts it at the top of the list and gives it a tab of its own. A mod
  with an error stands in red
- Every window of the TW3SE Menu has its own X to close it. Windows in front of the menu stay in front after **F2**
- `w3.hud.message(text, seconds)` shows its text for the seconds given. Up to 1.0.1 the game got them as
  milliseconds and the text was gone at once
- `w3.ui.window(title, fn, w, h, true)` gives the window an X (the second return value is false once it is clicked)

## 1.0.1

- Vortex installs TW3SE into `bin\x64_dx12`. With 1.0.0 it put the files into `bin\x64`, and mods that need TW3SE
  failed with script errors
- The README lists the credits (Tenour, the libraries and their authors)

## 1.0.0

For The Witcher 3 Remastered 5.0 and 5.01 and the REDkit editor (build `0x6ac574c5`).

**TW3SE Menu** (F2)
- One window with tabs: **Mods**, **Console** and the tabs mods add
- Its key is a setting in the menu (**Mods** > **TW3SE** > **Menu key**), at once; key names and combinations
  (`Ctrl+F2`) in `console_key` too
- **Mods**: every mod in one list with a search field (names, authors, settings): Lua mods, plugins and the
  WitcherScript mods in `Mods`. Mods with a page in the game's options menu show their settings there, saved to the
  game's settings file
- `w3.mod{...}`: a Lua mod's entry with its settings (checkbox, slider, number, choice, text, key, header, button),
  kept in `tw3se\vars.txt`, `on_change`, and a tab of its own; `w3.mods()`
- `w3.ui.tabs`, `tab`, `tooltip`, `wrap`, `dim`, `slider_int`, `spacing`, `key`; a hint in `input_text`, a width
  for `child`
- `w3.object(address, className)` sees an object as a class (the options menu's `CInGameConfigWrapper`)
- A Lua console with history; lines starting with `/` are console commands
- An inspector: every field of an object, live, changeable; objects and arrays open on a click; a button calls a
  parameterless method
- Windows for Lua mods: `w3.on_ui(fn)` with `w3.ui.window`, `button`, `checkbox`, `input_text`, `slider`, `table`
  and more
- A profiler window: the time of each Lua mod over the last second
- **Copy report**: a bug report on the clipboard (with the script stack and the last Lua errors and their
  tracebacks); `report` and `w3.report()` write the same
- Dear ImGui (MIT), drawn like the overlay (works with DLSS Frame Generation)

**Debugger**
- WitcherScript in VS Code: breakpoints with conditions and log points, step over, into and out, the calls with
  their variables and `self`, Lua expressions in Watch, hovers and the Debug Console, variables changed in place
  (`debugger = on`, the extension in `tools\vscode-witcherscript`)
- Lua in the same debugger: breakpoints in Lua mods and `tw3se\lua` files, steps, locals and upvalues

**Live reload**
- `TW3SE_OnScriptsReloaded(obj, 'OnMyReload')`: TW3SE calls the event after every reload, for new keys and other
  init code (the game's own `OnScriptReloaded` leaves a mod's objects out)
- `w3.on_reload(fn)` for Lua
- Lua hooks stay on across a reload
- `script_lines` (on by default, a switch in the TW3SE Menu > Mods > TW3SE): the game compiles its scripts with the
  line of each statement, so bug reports and script stacks name file and line, and the debugger's breakpoints and `w3.on_line`
  work for every player; a switch counts from the next start, the game compiles once
- A mod script that does not compile at the game's start: its file, line and error are in `tw3se.log`
  ("compiler ERROR"); the game ends itself as it always does (`compile_messages = off` in `tw3se.config` leaves it)

**Lua**
- Hooks on events the engine raises itself (`OnSpawned` when an entity enters the world), with `super` chains
- `orig()` and Lua calls pass structs that no game function returns (`SEntitySpawnData`); for a function with an
  `out` parameter `orig()` takes the call's own arguments
- When a hook chain fails before the original ran, the original runs
- Several mods can hook the same function: the handlers form a chain, ordered by priority; `w3.unhook` removes
  one handler
- `w3.new(class [, parent])` makes objects of scripted classes
- `w3.pose(entity)`: the whole skeleton of a figure, bone names, parents and positions
- A Lua mod loads again when one of its files is saved; everything it added is taken out first (`luamods`)
- `w3.native(name, fn)`: functions for WitcherScript written in Lua (`tw3se\lua\natives\` at the game start)
- `w3.off_tick`, `w3.off_draw`
- Tags read as a live list of names, text ids as their text
- `w3.run(fn)` with `w3.wait`, `w3.wait_frames` and `w3.wait_until`: sequences over several frames
- `w3.load(path)` loads a resource
- A Lua variable keeps its game object alive; a destroyed object raises "is gone" on use
- Calls that return objects leave the game's reference counts as they were
- `w3.on_key("Ctrl+F6", fn)`, `w3.key_down` and `w3.off_key`: keys by name
- `luaprof` and `w3.profile()`: the time each mod spends, per `on_tick`, hook and native
- A hook handler and a `w3.run` sequence run as the mod that made them (taken out with it)
- `w3.saved()`: a table per mod that is saved with the game and filled again after a load; `w3.on_load`,
  `w3.on_save`
- `w3.off_load`, `w3.off_save` and `w3.off_reload`; `nil` leaves an optional argument out, also in the middle
- `w3.pose` watches a figure while it asks for it (10 s), `w3.pose_stop` ends the watch at once
- Arrays grow and shrink with any element: strings, objects and structs too (`a:insert({typeName = "x"})`)
- Latent functions: a latent WitcherScript function runs on a script thread of the game, a latent native through
  its `...Async` twin; Lua gets a latent object with `done` and `wait()`
- Every field of the player reads as a value: bitfields (bit names in `w3.enum`, written by name), `EngineTransform`,
  resources by path (soft handles), `EntityHandle` as its entity, fixed arrays
- `w3.all(class)`: every live object of a class, spawned ones, managers and quest objects too
- Libraries for the game's systems: `w3.time`, `w3.weather`, `w3.env`, `w3.camera`, `w3.stats`, `w3.inv`,
  `w3.horse`, `w3.spawn`, `w3.ai`, `w3.fx`, `w3.light`, `w3.hud`, `w3.gwent`, `w3.quest`, `w3.fact`, `w3.world`,
  `w3.scene`
- Drawing at world positions: `w3.draw.text_at`, `marker_at`, `line_at`, `label`, and `w3.draw.line`
- A struct field takes a table of its fields or a struct value (`light.color = {...}`)
- Maps read and write like Lua tables: `m[key]`, `m[key] = v`, `m[key] = nil`, `#m`, `pairs(m)`; static arrays and
  `EngineQsTransform` read as values
- Dialogues: `w3.scene.add_choice` puts choices of your own under the game's, `w3.scene.on_line` changes subtitles,
  `w3.scene.choices` and `choose`
- `w3.text.set(id, text)`: any text of the game by its string id
- `w3.debug.show(true)`: the shapes scripts hand to `CVisualDebug` are drawn on screen
- The quest graph: `w3.quest.threads`, `blocks`, `children`, `activate`
- An `out` argument hands its value to the function (`GetUserMapPinLimits(1, 30)` sets the limits)
- A struct holding strings is copied field by field into an argument or a field
- An array the engine hands to a hooked script function by value reads in the hook
- The script functions running, with file and line: in `tw3se.log` when a hook fails or the game faults, in the
  hang report, and `w3.script_stack()`
- `w3.on_line(file, line, fn)`: a function runs before a script line, with the variables of the script function
  (`info.vars`, `info.set`)
- `w3.vhook(object, slot, fn)`: hooks on the virtual methods of the engine's classes; `w3.vslot`, `w3.object`
- `w3.dir()`: TW3SE's folder
- `w3.material`: a material's parameters read and changed; `w3.material.apply(entity)` shows the change on an entity
  that is on screen already, `w3.refresh(component)` draws one component anew
- A whole array field takes a table or another array; pointer fields and elements take an object
- `w3.copy(target, source)`: an object's fields into another; with `w3.new("CEntityTemplate")` an NPC spawns from a
  template built in Lua
- `w3.attach(entity, template)`, `w3.detach`: a template's components on an entity in the world
- `w3.physics.gravity`: the gravity of the world's PhysX scene, read and set
- Network: `w3.http.get`, `post`, `request` (HTTP and HTTPS), `w3.net.connect` (TCP) and `w3.net.udp`, in the
  background with callbacks on the main thread
- `w3.config(group, name [, value])`, `w3.config_list`: the engine's own settings (about 1700: rendering, post
  process, path tracer, Umbra, shadows, physics, gameplay), read and set while the game runs
- `w3.world.sector`, `sector_near`, `sector_hide`, `sector_restore`: buildings, walls and rocks of the world (the
  merged geometry of a layer), listed and taken away while the game runs
- `w3.sound.load_bank`, `unload_bank`, `bank_loaded`: Wwise banks, the game's and your own from loose files
- `w3.figure.head`, `head_reset`: Geralt's head as the barber sets it; `w3.figure.furs`: HairWorks components;
  `w3.figure.fur_color`, `fur_reset`: the colour of hair and fur at root and tip
- `w3.anim.with_sets(template, sets)`: NPCs that spawn with more animation sets
- `w3.physics` (impulses, speed, mass, ragdoll, collisions), the game's sounds in `w3.sound` (`event`, `switch`,
  `state`, `param`), `w3.anim` (slot animations, behaviour variables and events, face animations), `w3.figure`
  (appearances, voiced lines)

**Files**
- Loose files: a game file in `tw3se\files\` under its depot path replaces the game's; TW3SE packs them into
  `Mods\mod00000_TW3SE_Files` at the start
- A file saved in `tw3se\files\` while the game runs loads anew at its next use (the console command `files`,
  `w3.files.reload`)
- `tools\cr2w`: read game files out of the bundles, change them, write them back (Python)
- `w3.files.alias(path, newPath)`: a game file under a second path, loaded from there as a resource of its own
- Textures as loose files: a DDS under the path of a game texture replaces its pixels (TW3SE writes a
  `texture.cache` at the start); `tools\texcache.py` lists the game's textures and writes one as DDS
- Sounds as loose files: a `.bnk` or `.wem` replaces the game's sound file of that name (a `soundspc.cache` of the
  mod); `tools\soundcache.py`

**Shaders**
- A shader in `tw3se\shaders\<id>.cso` replaces the game's shader with that id; `shader_dump = on` writes every
  shader of the game with its id; `tools\shader_find.py`, the console command `shaders`

**Blend shapes**
- `w3.deform`: shapes from a file move a mesh's vertices, each with a weight from Lua; every pass of the game draws
  the moved mesh; weights can change every tick
- `w3.deform.dump` writes a mesh's original positions and normals; `tools\twdf.py` reads them and writes shape files
- Shape files carry normal offsets too (format version 2)
- `w3.deform.own(mesh)`: one entity gets a copy of its mesh, so its shapes move it alone; `w3.set_mesh(component,
  mesh)` gives a running component another mesh

**Plugins** (interface 16 to 18)
- `Call`, `GetField` and `SetField` with typed values: every function and field of the game, arguments of any type,
  out arguments back
- `HookScript` and `CallOriginal`: hooks on native and WitcherScript functions, in one chain with Lua's
- `ClassName`, `ClassParent`, `ForEachField`, `ForEachMethod`, `EnumValue`, `ForEachObject`
- `GetPose` gives the whole skeleton of a watched figure in model space, with names and parents
- The pose callback names each bone of a mesh (`boneNames`); `parents` there is each slot's own skeleton bone
- `RunLua` and `RegisterLuaFunction`: a plugin runs Lua and gives Lua mods its own functions (`w3.plugin.<name>`)
- Each pose callback carries the kind of its component (body, head, other); `GetPose` reads the body from the
  root animated component and the face skeleton with `TW3SE_POSE_MIMIC` (`w3.pose(entity, "mimic")` in Lua)
- The mimic callback works: `RegisterOnMimic` and `WatchMimic` give the mixed track weights of the head every
  frame, with names, writable; console command `mimic`
- A watched figure takes the items that hang on it along, such as Geralt's head
- Several plugins can each register a pose and a mimic callback (up to 16); `RegisterOnPose(NULL, NULL)` takes only
  the calling plugin's out
- Every interface function checks its pointers: NULL returns 0, `result` of the `Call` functions may be NULL
- A pose or mimic callback that crashes will be logged and removed, like a tick callback

**WitcherScript**
- Fields by name: `TW3SE_GetInt`, `GetFloat`, `GetBool`, `GetString`, `GetObject`, the `Set` functions, `HasField`,
  `HasMethod`
- Calls by name with text arguments: `TW3SE_Call`, `TW3SE_CallObject`
- The catalog: `TW3SE_ClassOf`, `ClassParent`, `Fields`, `Methods`, `EnumValue`, `EnumName`
- `TW3SE_HookEvent`: an event of your object after each call of any function; `TW3SE_Unhook`, `TW3SE_LastError`
- `TW3SE_AllObjects` and `TW3SE_ObjectAt`: every live object of a class

**Plugin reload**
- `plugin reload <name>` (Lua `w3.plugin_reload`) unloads a plugin and loads its DLL again while the game runs;
  plugins run from a copy, so a build can overwrite the original
- Optional export `TW3SE_PluginUnload`: the plugin cleans up and asks for a few more ticks; natives keep their names
  across the reload

**Sounds**
- WAV and OGG files through XAudio2: flat or in 3D at an entity or one of its bones, with volume, loop and a
  handle to stop them
- For plugins (interface 17), WitcherScript (`TW3SE_PlaySound`, `TW3SE_StopSound`, `TW3SE_SetSoundVolume`,
  `TW3SE_SoundPlaying`) and Lua (`w3.sound`); console command `sound`

**Hang reports**
- When the game stands for 8 s, TW3SE writes `tw3se\out\hang_<time>.txt`: what each thread does, which code
  stopped threads, the last steps TW3SE took and every thread's stack (`hang_seconds` in `tw3se.config`)
- `!status`, `!crumbs`, `!suspends` and `!dump` answer on the pipe while the game hangs; `hangtest` holds the main
  thread on purpose
- `tools\watch.py` watches the game from a process of its own

**Fixes**
- On 5.01, a game function another mod hooked before TW3SE made TW3SE passive (its bytes no longer matched); TW3SE
  finds it behind the other mod's jump now, and both hooks run
- The game hung when TW3SE caught an error in its own code: the game's crash handler had stopped every other thread
  first, and they stayed stopped (seen with Geralt's head). Errors inside TW3SE go to TW3SE now
- `w3.saved` tables were empty after a load (the game reports each load twice)
- A save keeps 16 bits of a fact: the save id of `w3.saved` is two facts now
- An out array of objects (`FindGameplayEntitiesInRange`) kept every object it returned; an object argument kept
  its reference when a later argument was wrong
- `''` and `'None'` are the empty name
- `w3.every`, `w3.after`, `w3.watch` and `w3.on_key` check their arguments: a missing function or a wrong time
  is an error; `w3.after` and `w3.watch` take their tick out when they are done; all three return a handle
- The file natives refuse Windows device names (`CON`, `NUL`, `COM1` ...): `TW3SE_ReadNamed("CON")` waited for
  console input and held the game; sounds and `luafile` read files on disk only
- `TW3SE_SetVar` takes at most 64 KiB a value and 16 MiB in all; `vars.txt` over 4 MiB was skipped at the start,
  with every mod's variables
- `w3.ui` checks its input: `row` and `col` work inside a table, a window needs a title, sizes need finite numbers
  (ImGui read through a NULL table)

**Console**
- `report` writes what a bug report needs into one file
- The pipe's name comes from `pipe = <name>` in `tw3se.config`, so two game installs run side by side;
  `tools\tw3se.py --pipe <name>` or the variable `TW3SE_PIPE`
- `tools/catalog_html.py` writes the catalog as a web page with a search
- The SDK has the pipe tool `tools\tw3se.py` and the catalog search `tools\catalog.py` (`--game <game folder>`)

## 0.2.0

For The Witcher 3 Remastered 5.0 and 5.01 and the REDkit editor (build `0x6ac574c5`).

**Lua 5.4**
- Read and write any field of any object by name, of any type
- Call the game's functions, native and WitcherScript, with numbers, strings, names, enums, objects, structs,
  arrays and `out` values
- Hook functions of the game (`w3.hook`): change arguments, replace or observe the result
- Show text and boxes over the game (`w3.on_draw`)
- Run code each tick, on a timer or when a field changes (`w3.on_tick`, `w3.every`, `w3.after`, `w3.watch`)
- Find objects by class, tag, GUID or distance
- Mods as folders (`tw3se\mods\<name>\init.lua`), autorun files, the helper library `lib\w3util.lua`, examples
- WitcherScript runs Lua with `TW3SE_Lua(code)`
- Console commands `lua`, `luafile`, `luatest`

**Live reload**
- Live reload of WitcherScript: `reloadscripts force` compiles all scripts again while the game runs
- Objects of your scripts keep their values, new fields appear on them
- A compile error names file and line; the game keeps the last scripts that compiled

**REDkit**
- TW3SE loads in the REDkit editor: script functions, Lua, world edits, tree placement, the catalog, `exec` and `events`
- Its own pipe `\\.\pipe\tw3se_redkit`, so editor and game run side by side
- Plugins load from `bin\x64_RedKit\tw3se\plugins`

**WitcherScript functions**
- Place trees in the world and remove them: `TW3SE_FoliagePlant`, `TW3SE_FoliageRemoveAt`
- `TW3SE_Lua`

**Plugins** (interface 15)
- Pose hook: change the bones of a watched figure every frame (`RegisterOnPose`, `WatchPose`), with the bone
  names, parents and the figure's world matrix; each mesh of a figure fires on its own
- `IsEditor`: a plugin knows whether it runs in the REDkit editor

**Tools**
- `catalog` writes every class, field, enum and function of the game to `tw3se\catalog\rtti.json`;
  `tools/catalog.py` searches it, `tools/gen_types.py` writes editor completion for Lua
- `questlive`: each quest block the game starts goes to `events`
- `tools/update_check.py` checks the address table against a new game or REDkit build

## 0.1.1

For The Witcher 3 Remastered 5.01 (`witcher3.exe` build `0x6ac555a3`) and 5.0.

- Works on patch 5.01. TW3SE 0.1.0 stays off there: the patch moved two places inside one game function.
- Places inside a game function are found by a short byte pattern. When a patch moves them, TW3SE finds them again.
  Anything missing stays off.

## 0.1.0

First release. For The Witcher 3 Remastered 5.0 (`witcher3.exe` build `0x6abd8695`).

**Loading**
- A `dinput8.dll` loader starts TW3SE before the game's code
- Engine functions are found by signature on other game builds
- An error in one feature will be logged and the game will continue to work normally

**WitcherScript functions**
- Files in `tw3se\out\`: write, append, read, size, delete
- Variables outside the save (written once a second and on exit)
- Settings from `tw3se.config`, rebindable hotkeys, key state per frame
- Objects by GUID: `TW3SE_EntityGuid`, `TW3SE_EntityByGuid`, `TW3SE_EntityEffects`, `TW3SE_EntityTemplate`,
  `TW3SE_HideEntity`
- `TW3SE_WorldEdit` and `TW3SE_Emit`
- Several mods can import the same function

**World edits** (`tw3se\world.txt` in a mod or DLC folder)
- Remove, effect, appearance, move
- Conditions on quest facts (`if` / `unless`), switched while you play
- Applied every time the objects load

**Plugins** (C++, interface 12)
- Own script functions and console commands
- Tick and frame callbacks, engine hooks, object fields by name
- Game function calls with typed arguments (int, float, string, name, bool)
- D3D12 overlay (works with DLSS)
- Game loaded event, objects by GUID, world edits

**Tools**
- The pipe `\\.\pipe\tw3se` with console commands
- `exec` and `events` for external tools
- `script_logs`: keeps `LogChannel` in compiled scripts

**Other DLL mods**
- .asi mods load from `bin\x64_dx12`, `scripts\` and `plugins\`
- Ultimate ASI Loader works as `winmm.dll`
- Another mod's `dinput8.dll` works as `dinput8_chain.dll`
- Hooks of other mods on the same function both run

**Settings**
- The overlay starts only when a plugin draws (`overlay = on` forces it)
- `events_channels` defaults to `Cj`
