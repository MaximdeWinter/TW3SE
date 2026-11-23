# Plugin API

`TW3SE_PluginLoad` gets a `const TW3SE_Interface*`: a C struct of function pointers (`include/tw3se/tw3se.h`). New
functions are appended at the end with a new interface version. Check `interfaceVersion` before you call anything
above your `minInterface`.

```c
uint32_t interfaceVersion;  // interface version of the running TW3SE
uint32_t extenderVersion;   // (major<<16)|(minor<<8)|patch
uint32_t gameBuild;         // PE timestamp of witcher3.exe
```

The C++ header `tw3se/tw3se.hpp` wraps most of them (named under each entry). Call `tw3se::init(api)` first.

Include `<windows.h>` (and `<d3d12.h>`) before `tw3se.h`: `windows.h` renames `PlaySound` with a macro, and then
it does so in the struct and in your call alike.

## Log and addresses

```c
void       Log(const char* plugin, const char* format, ...);   // one line in tw3se.log
uintptr_t  GetAddress(const char* name);                        // engine address by name, 0 if unknown
uintptr_t  GetImageBase(void);                                  // base address of witcher3.exe
```

Names for `GetAddress` come from TW3SE's address table (`"GContentManager"`, `"CNamesPool::AddEntry"` …).

## Hooks and the main thread

```c
int   Hook(void* target, void* detour, void** original);        // 1 = hooked, *original calls the original
void  RunOnMainThread(TW3SE_Task task, void* context);          // runs task(context) at the end of the next tick
```

Most engine systems take calls only from the main thread. `RunOnMainThread` queues work there from any thread.

A call with NULL where the interface needs a pointer (a name, a callback, a buffer for the reply) returns 0 and
leaves everything as it was. The `result` of the `Call` functions may be NULL to skip the return value.

A crash inside one of your callbacks will be logged, and TW3SE removes that callback. The game's crash handler sees
every crash before a `__try` in your own code does, and it stops all other threads first. A plugin that reads memory
it is unsure of has to check it before reading (`VirtualQuery`).

## Console commands

```c
int RegisterCommand(const char* name, TW3SE_Command handler, const char* help);
// TW3SE_Command: void(const char* args, char* reply, uint32_t replySize)
```

The handler runs on the main thread and writes its answer into `reply`. See [Console commands](console-commands.md).
C++: `tw3se::register_command`.

## Script natives (interface 2)

```c
int  RegisterNative(const char* name, TW3SE_ScriptNative native, int paramCount);
void ReadParam(void* frame, void* out);     // one parameter, in order
void EndParams(void* frame);                 // once, after the last parameter
// TW3SE_ScriptNative: void(void* self, void* frame, void* result)
```

Register in `TW3SE_PluginLoad`. In the handler, read each parameter with `ReadParam` (a `string` arrives as a
`char*`, give the buffer at least 16 bytes), call `EndParams`, write the return value to `result`.

C++: `tw3se::register_native<&Fn>("Name")` does all of this from a normal function.

## Tick (interface 3)

```c
void RegisterOnTick(TW3SE_TickCallback callback, void* context);
// TW3SE_TickCallback: void(float deltaSeconds, void* context)
```

Called once per game tick on the main thread, in menus too. If the callback crashes, it will be logged and
removed. C++: `tw3se::on_tick`.

## Fields (interface 4 and 5)

```c
void* FindProperty(void* cclass, const char* name);   // CProperty* (inherited too) or NULL
void* PropertyPtr(void* object, void* cproperty);     // address of the field in the object
void* GetObjectClass(void* object);                   // the object's CClass*
void* FindClassByName(const char* name);              // CClass* by name or NULL
```

Get the class (`GetObjectClass` or `FindClassByName`), find the field (`FindProperty`), read or write it at
`PropertyPtr`. Works for engine fields and script `var`s.

C++: `tw3se::read_property_of<T>(obj, "name")`, `write_property_of<T>(obj, "name", value)`.
`read_property<T>` / `write_property<T>` take the class yourself.

## Game functions (interface 6)

```c
int CallFunction(const char* name, const char* const* stringArgs, int argCount, void* self, void* result);
```

Calls a global native by name. `self` is NULL for a global function, `result` gets the return value. Returns 1 if
it was called.

```c
const char* a[] = {"witcher"};
int n = 0;
api->CallFunction("StrLen", a, 1, NULL, &n);   // n == 7
```

C++: `tw3se::call_function("StrLen", {"witcher"}, nullptr, &n)`.

## Player, methods, handles, names (interface 7)

```c
void*       GetThePlayer(void);     // the player or NULL (menu)
void*       GetTheGame(void);       // the game object
int         CallMethod(void* object, const char* name, const char* const* stringArgs, int argCount, void* result);
void*       ResolveHandle(void* handle);     // the object behind a handle or NULL
const char* GetCNameString(uint32_t id);     // the text of a CName id or NULL
```

`CallMethod` calls a method of an object (`GetMovingAgentComponent` …). Methods that return an object give a
handle. `ResolveHandle` turns it into the object.

```c
void* handle = NULL;
api->CallMethod(api->GetThePlayer(), "GetMovingAgentComponent", NULL, 0, &handle);
void* component = api->ResolveHandle(handle);
```

C++: `tw3se::the_player()`, `tw3se::the_game()`, `tw3se::call_method(obj, name, {args}, &result)`,
`tw3se::resolve_handle(handle)`, `tw3se::cname_string(id)`.

## Swap chain (interface 8)

```c
void RegisterOnPresent(TW3SE_PresentCallback callback, void* context);
// TW3SE_PresentCallback: void(void* swapChain, void* context)
```

Every frame from the render thread, with the swap chain (`IDXGISwapChain3*`), to draw yourself. If the
callback crashes, it will be logged and removed. C++: `tw3se::on_present`.

## Typed arguments (interface 9)

```c
int CallFunctionV(const char* name, const char* argspec, void* self, void* result);
int CallMethodV(void* object, const char* name, const char* argspec, void* result);
```

Arguments as text, separated by spaces: `i:5` int, `f:1.5` float, `s:text` string, `b:1` bool, `n:fire` name,
`e:1` the value of an enum (0 to 255, the number `w3.enum` names).

```c
int n = 0;
api->CallFunctionV("StrLen", "s:witcher", NULL, &n);     // n == 7
api->CallMethodV(entity, "PlayEffect", "n:fire", NULL);  // plays the fire effect
```

C++: `tw3se::call_function_v(name, argspec, self, &result)`, `tw3se::call_method_v(obj, name, argspec, &result)`.

## Overlay (interface 10)

Text and boxes over the game, every frame. Works with DLSS and FSR Frame Generation.

```c
void RegisterOnOverlayDraw(TW3SE_OverlayDrawCallback callback, void* context);
// TW3SE_OverlayDrawCallback: void(void* context)
void OverlayDrawText(float x, float y, uint32_t rgba, const char* text);
void OverlayDrawRect(float x, float y, float w, float h, uint32_t rgba);
void OverlayGetScreenSize(float* outWidth, float* outHeight);
```

Draw inside the callback. Coordinates are pixels from the top left, colours `0xRRGGBBAA`. Text is a monospace font.

```c
void MyHud(void* ctx) {
    tw3se::overlay_rect(10, 10, 230, 56, 0x000000A0);
    tw3se::overlay_text(20, 16, 0x66E0FFFF, "My Mod");
    tw3se::overlay_text(20, 36, 0xFFFFFFFF, "HP 100/100");
}
// in TW3SE_PluginLoad, when api->interfaceVersion >= 10:
tw3se::on_overlay_draw(&MyHud);
```

## Game loaded (interface 11)

```c
void RegisterOnGameLoaded(TW3SE_GameLoadedCallback callback, void* context);
// TW3SE_GameLoadedCallback: void(void* context)
```

After a new game starts and after every save load, on the main thread. Put your changes back here: a load restores
the world from the save.

```c
void OnLoaded(void* ctx) {
    // apply this plugin's changes again, the player exists now
}
if (api->interfaceVersion >= 11) api->RegisterOnGameLoaded(&OnLoaded, nullptr);
```

C++: `tw3se::on_game_loaded`.

## Graphics rebuild (interface 25)

```c
int RegisterOnRenderRebuild(TW3SE_RenderRebuildCallback callback, void* context);
// TW3SE_RenderRebuildCallback: void(int phase, void* context)
// phase: TW3SE_REBUILD_BEFORE (0) or TW3SE_REBUILD_AFTER (1)
```

When the player applies a graphics option (upscaler, anti-aliasing, path tracing, ray tracing ...), the game
rebuilds its render resources at once and frees thousands of its objects. A pointer you kept into them then points
at freed memory, and the game crashes seconds later somewhere else.

- `TW3SE_REBUILD_BEFORE` comes right before the rebuild. Let go of every pointer into the game's render, skinning
  and mesh data, and stop what reads them.
- `TW3SE_REBUILD_AFTER` comes when the rebuild is done. Look everything up again from the game.

Both run on the main thread. One change can rebuild more than once, and each BEFORE has its AFTER. Your blend shape
sets end before the rebuild (`TW3SE_DEFORM_LOST`): load them again after it.

```c
void OnRebuild(int phase, void* ctx) {
    if (phase == TW3SE_REBUILD_BEFORE) ForgetRenderPointers();
    else LookThemUpAgain();
}
if (api->interfaceVersion >= 25) api->RegisterOnRenderRebuild(&OnRebuild, nullptr);
```

C++: `tw3se::on_render_rebuild`.

## Objects and world edits (interface 12)

```c
void* EntityByGuid(const uint32_t guid[4]);          // the loaded object with this GUID or NULL
int   EntityGuid(void* entity, uint32_t outGuid[4]); // GUID of an object (0 for spawned objects)
int   WorldEdit(const char* line);                   // one world.txt line, for this session
```

Any placed object by its GUID. Keep the GUID between ticks: an object unloads when the player walks away and comes
back as a new object with the same GUID.

```c
uint32_t lamp[4] = {0xbd5160b6, 0x45e69f5e, 0xcf66a598, 0x084ca441};   // a street lamp in Novigrad
if (api->interfaceVersion >= 12) {
    if (void* e = api->EntityByGuid(lamp)) api->CallMethodV(e, "StopEffect", "n:fire", nullptr);
    api->WorldEdit("remove bd5160b6 45e69f5e cf66a598 084ca441 if mymod_lamp_broken");
}
```

C++: `tw3se::entity_by_guid` (GUID as text works too), `tw3se::entity_guid`, `tw3se::world_edit`.

## Pose hook (interface 13 and 14)

```c
void RegisterOnPose(TW3SE_PoseCallback callback, void* context);
int  WatchPose(void* entity, int on);                  // 1 = watch this entity, 0 = stop
// TW3SE_PoseCallback: void(void* entity, TW3SE_Pose* pose, void* context)
```

Change the bones of a figure every frame, after its animation and before the game draws it. The callback fires for
the entities you watch, once per mesh and frame (torso, legs, head and body each fire with their own `mesh`).

```c
typedef struct TW3SE_Pose {
    void*        entity;         // the CEntity
    void*        component;      // the CAnimatedComponent
    int32_t      kind;           // TW3SE_POSE_BODY, _MIMIC or _OTHER
    uint32_t     guid[4];        // the entity's GUID (zero for a spawned one)
    int32_t      boneCount;
    const char* const* boneNames;   // [boneCount] in the callback: the name of each slot ("" past the resident bones)
    const int16_t* parents;         // [boneCount] in the callback: each slot's own index in GetPose's skeleton
    float*       local;             // [boneCount * localStride], writable
    int32_t      localStride;       // 16
    const float* modelToWorld;      // the entity this frame, 4x4
    void*        mesh;              // interface 14: which mesh this call drives
} TW3SE_Pose;
```

A mesh has its own bone order (its slots). `parents[i]` is the skeleton bone of slot `i` (its index in what
`GetPose` returns), `boneNames[i]` its name. All matrices are 4x4, row-major, translation in the last row.

For each slot the game draws `local × model`, where `model` is the animated model matrix of the slot's own bone
(`GetPose(...).local[parents[i] * 16]`). The game fills `local` with the bone's inverse bind matrix. To draw a bone at
the model matrix `N`, write `local = inverse(bind) × N × inverse(model)`, with `bind` the bone's bind matrix from
the mesh. A written `local` stays until you write it again: put the game's value back when you let go.
`modelToWorld` has the same layout.

```c
void OnPose(void* entity, TW3SE_Pose* p, void* ctx) {
    if (p->kind != TW3SE_POSE_BODY) return;
    for (int i = 0; i < p->boneCount; ++i)          // slots differ per mesh
        if (!strcmp(p->boneNames[i], "head")) {
            float* m = p->local + i * p->localStride;   // the head's slot in this mesh
            // write all 16 floats, see the formula above
        }
}
// in TW3SE_PluginLoad, when api->interfaceVersion >= 13:
api->RegisterOnPose(&OnPose, nullptr);
// later, on the main thread:
api->WatchPose(api->GetThePlayer(), 1);
```

The callback runs on the game's animation thread: keep it short and touch only the pose. The hook installs with the
first `RegisterOnPose`; `posehook = off` in `tw3se.config` keeps it out. Each plugin can have its own callback (up
to 16); they run one after the other on the same pose. `RegisterOnPose(NULL, NULL)` takes your plugin's callback out.

`GetPose(entity, TW3SE_POSE_BODY, &pose)` fills `pose` with the whole skeleton of a watched figure from its last
frame: `boneCount`, `local` = the model-space 4x4 of every bone (row-major, translation in the last row,
`localStride` 16), `modelToWorld`, and `boneNames` and `parents` (the root has -1, each parent comes before its
children). The bones are in the game's run-time order. The first ones (`CSkeleton.lodBoneNum_1`, 40 on Geralt:
pelvis, spine, head, arms, hands, legs) have their names and parents; the bones after them have the name "" and
the parent -1. It returns 1 when a pose is there: the figure is watched with `WatchPose` and has had a frame since.
It works from a pose callback too, also for the figure the callback is about. `local` is a copy: it stays valid
until the next `GetPose` of the same figure, and the game keeps its own pose when you write to it. It holds the
animated pose of the frame; the writes of pose callbacks reach only their own mesh.

Each callback carries the kind of its component: `TW3SE_POSE_BODY` for the figure's root animated component,
`TW3SE_POSE_MIMIC` for the head (a CMimicComponent), `TW3SE_POSE_OTHER` for the rest. `GetPose(entity,
TW3SE_POSE_MIMIC, &pose)` reads the head's face skeleton the same way.

`WatchPose` and `WatchMimic` take a figure, one of its components or an item entity. A figure takes the items that
hang on it along: Geralt's head is an item entity with its own CMimicComponent, and it fires with the player. TW3SE
finds these items with a search over the figures in view, which runs while a figure is watched (the console command
`poseitems` shows what it costs). A component or an item watched itself fires alone, and the search stays off. To watch only
Geralt's face, watch his head item or its CMimicComponent; for his body, his root animated component. `GetPose` with
a watched component reads the pose of its entity, and `WatchMimic` alone is enough for `TW3SE_POSE_MIMIC`.

### The face

```c
void RegisterOnMimic(TW3SE_MimicCallback callback, void* context);
int  WatchMimic(void* entity, int on);                 // 1 = watch this entity, 0 = stop
// TW3SE_MimicCallback: void(void* entity, TW3SE_Mimic* mimic, void* context)
```

Change the face of a figure every frame. The callback gets the weights of the head's mimic tracks after the game
has mixed the scene's expressions and before they become face bones. `weights` arrives filled; overwrite your own
tracks and leave the others, so the scene's lip sync, eyes and brows stay. As with poses, each plugin can have its own
callback, and `RegisterOnMimic(NULL, NULL)` takes yours out.

```c
void OnMimic(void* entity, TW3SE_Mimic* m, void* ctx) {
    for (int i = 0; i < m->trackCount; ++i)
        if (!strcmp(m->trackNames[i], myTrack)) m->weights[i] = 0.6f;   // find the index once and keep it
}
// in TW3SE_PluginLoad:
api->RegisterOnMimic(&OnMimic, nullptr);
// later, on the main thread:
api->WatchMimic(entity, 1);
```

The callback runs on the animation thread, like the pose callback. The console command `mimic` shows whether the
hook is in and how often it fired. The hook is in the game only (the REDkit editor leaves it out).

## Load a plugin again (interface 17)

The console command `plugin reload <name>` (Lua: `w3.plugin_reload(name)`) unloads a plugin and loads its DLL again
while the game runs. Build the plugin, copy it into `tw3se\plugins\`, reload it: the game keeps running.

TW3SE runs each plugin from a copy next to it (`example.live1.dll`), so the original can be overwritten at any
time. `plugin_reload = off` in `tw3se.config` loads the originals (then a reload needs a restart).

```c
TW3SE_EXPORT int TW3SE_PluginUnload(void) {
    // stop your threads, put written bones back, WatchPose(e, 0)
    return 0;   // or the number of ticks you still need
}
```

`TW3SE_PluginUnload` is optional and runs on the main thread first. From then on the plugin's commands, natives and
Lua functions are off. Its tick, pose and mimic callbacks keep running for the number of ticks it returns: a pose
callback can put the bones back there. Then TW3SE takes out everything else the plugin registered (tick, present,
overlay and game-loaded callbacks, hooks made with `Hook`, queued main-thread tasks, its sounds) and frees the DLL.
The new DLL runs `TW3SE_PluginQuery` and `TW3SE_PluginLoad` as at the start and gets the same interface.

A native keeps its name across the reload: scripts that import it call the new function. While the plugin is away,
a call reads its arguments and returns 0. Data that has to outlive the reload goes into a file or into Lua
(`RunLua`). Hooks a plugin makes with its own MinHook come off in `TW3SE_PluginUnload`.

## Editor or game (interface 15)

```c
int IsEditor(void);    // 1 inside the REDkit editor, 0 in the game
```

In the editor a plugin's features work in its Play mode. A few engine addresses exist only in the game:
`GetAddress` answers 0 for them there. C++: `tw3se::is_editor()`.

## Lua (interface 16)

```c
int RunLua(const char* code, char* reply, uint32_t replySize);
int RegisterLuaFunction(const char* name, TW3SE_LuaFunction function, void* context);
// TW3SE_LuaFunction: int(int argc, const char* const* argv, char* reply, uint32_t replySize, void* context)
```

`RegisterLuaFunction` gives Lua mods `w3.plugin.<name>(...)`. The arguments arrive as text, the reply goes back to
Lua as a string. Return 1 when it worked; return 0 and Lua raises your reply as the error. Register it in
`TW3SE_PluginLoad`: the function is there for Lua from its start.

```c
static int Add(int argc, const char* const* argv, char* reply, uint32_t size, void*) {
    if (argc < 2) { snprintf(reply, size, "add needs two numbers"); return 0; }
    snprintf(reply, size, "%g", atof(argv[0]) + atof(argv[1]));
    return 1;
}
api->RegisterLuaFunction("my_add", &Add, NULL);   // Lua: w3.plugin.my_add(2, 3) -> "5"
```

`RunLua` runs Lua code like the console command `lua` and writes its output or returned values into `reply`. It
returns 1 when the code ran, 0 on an error. It runs on the main thread: from another thread use `RunOnMainThread`.
C++: `tw3se::run_lua(code, reply, size)`, `tw3se::register_lua_function(name, fn)`.

## Sounds (interface 17)

```c
int32_t PlaySound(const char* file, void* entity, const char* bone, float volume, int loop);  // a handle, 0 = error
int     StopSound(int32_t sound);
int     SetSoundVolume(int32_t sound, float volume);
int     SoundPlaying(int32_t sound);
int     PreloadSound(const char* file);
```

Play a WAV or OGG file through XAudio2. `file` is an absolute path or a path from the game folder
(`mods\modMyMod\sounds\door.ogg`). With an `entity` the sound is in 3D at it, or at its bone `bone` (NULL for the
entity itself), and you hear it from the camera. With `entity` NULL it plays flat. `volume` 1 plays the file as it
is (0 counts as 1), `loop` 1 repeats it until `StopSound`. A sound at an entity stops when the entity is gone.

The functions work from any thread, the pose callback too. A file is decoded on its first use and kept;
`PreloadSound` decodes it ahead, on the main thread, so the first `PlaySound` is quick. An error (a missing file, a
format XAudio2 refuses) will be logged, and `PlaySound` returns 0. The console command `sound` shows how many
sounds play, `sound play <file>` plays one.

```c
int32_t s = api->PlaySound("mods\\modMyMod\\sounds\\breath.ogg", entity, "head", 0.7f, 1);
api->SetSoundVolume(s, 0.3f);
api->StopSound(s);
```

C++: `tw3se::play_sound(file, entity, bone, volume, loop)`, `stop_sound`, `set_sound_volume`, `sound_playing`,
`preload_sound`.

## Calls, fields, hooks and the catalog (interface 18)

A plugin reaches what Lua reaches: every function of the game with arguments of any type, every field, hooks on every
function, the classes with their fields and methods. Values travel as a `TW3SE_Value`:

| `type` | Value | Field |
|---|---|---|
| `TW3SE_NIL` | nil (leaves an optional argument out) | |
| `TW3SE_BOOL`, `TW3SE_INT` | a number | `v.i` |
| `TW3SE_FLOAT` | a number | `v.f` |
| `TW3SE_STRING` | a string, a name, an enum value (`"EDT_Default"`) | `v.s` |
| `TW3SE_OBJECT` | an object of the game | `v.object` |
| `TW3SE_STRUCT` | a struct: its bytes as in the game, `typeName` and the size in `count` | `v.data` |
| `TW3SE_ARRAY` | `count` values | `v.items` |
| `TW3SE_OTHER` | a latent call, a function: its description | `v.s` |

```c
int         Call(void* object, const char* name, const TW3SE_Value* args, int argCount, TW3SE_Value* results, int maxResults);
int         GetField(void* object, const char* field, TW3SE_Value* out);
int         SetField(void* object, const char* field, const TW3SE_Value* value);
const char* LastError(void);
```

`Call` with `object` NULL calls a global function, else the object's method. It writes the return value and then each
out argument into `results` and returns how many there are, or -1 on an error. `LastError` says why. A latent function
starts on a script thread of the game and comes back as an `OTHER`.

```c
TW3SE_Value a[4] = {{TW3SE_STRING}, {TW3SE_STRING}, {TW3SE_STRING}, {TW3SE_STRING}};
a[0].v.s = "a.b.c"; a[1].v.s = "."; a[2].v.s = ""; a[3].v.s = "";
TW3SE_Value r[3];
int n = api->Call(NULL, "StrSplitFirst", a, 4, r, 3);   // n == 3: true, "a", "b.c"

TW3SE_Value v;
api->GetField(player, "equippedSign", &v);              // v.v.s == "ST_Axii"
```

Strings, struct bytes and array items that come back stay valid until the next of these calls; copy what you keep.
An object pointer is good for the current tick. All of them run on the main thread.

```c
int HookScript(const char* className, const char* name, TW3SE_ScriptHook hook, void* context, int priority);
int Unhook(int id);
int CallOriginal(TW3SE_HookCall* call, TW3SE_Value* result);
// TW3SE_ScriptHook: int(void* self, TW3SE_Value* args, int argCount, TW3SE_HookCall* call, TW3SE_Value* result, void* context)
```

`HookScript` puts your function in front of a native or WitcherScript function (`className` NULL for a global). It
shares the chain with Lua's `w3.hook`: the highest priority runs first. In the hook, `args` holds the call's arguments
and is writable. `CallOriginal` runs the rest of the chain and the original with them. Return 1 with `result` filled
to give the call your result, or 0 to pass the call on. `HookScript` returns an id for `Unhook`. When the plugin is
loaded again, its hooks leave the chain.

```c
static int PlusOne(void* self, TW3SE_Value* args, int argc, TW3SE_HookCall* call, TW3SE_Value* result, void* ctx) {
    TW3SE_Value r;
    if (!api->CallOriginal(call, &r)) return 0;
    result->type = TW3SE_INT;
    result->v.i = r.v.i + 1;
    return 1;
}
api->HookScript(NULL, "StrLen", &PlusOne, NULL, 0);     // StrLen("witcher") is 8 now
```

```c
const char* ClassName(void* cclass);
void*       ClassParent(void* cclass);
int         ForEachField(void* cclass, TW3SE_ItemCallback callback, void* context);
int         ForEachMethod(void* cclass, TW3SE_ItemCallback callback, void* context);
int         EnumValue(const char* enumName, const char* valueName, int64_t* out);
int         ForEachObject(const char* className, TW3SE_ObjectCallback callback, void* context);
// TW3SE_ItemCallback: int(const TW3SE_ItemInfo* info, void* context), return 1 to stop
// TW3SE_ObjectCallback: int(void* object, void* context), return 1 to stop
```

`ForEachObject` visits every live object of a class and its subclasses: entities, spawned ones too, components,
managers and quest objects. The pattern objects inside entity templates stay out. The callback may make or destroy
objects.

`ForEachField` and `ForEachMethod` visit a class's own and inherited fields and methods with name, type, the class
that declares them, offset and flags. They return how many they visited. The strings in `info` live during the
callback.

C++: `tw3se::call(object, name, {tw3se::value("witcher")})` returns a `std::vector<TW3SE_Value>`; `value(...)`,
`object(ptr)`, `structure(type, data, size)`, `get_field`, `set_field`, `last_error`.

## Decals (interface 19)

```c
void DecalDefaults(TW3SE_Decal* decal);
int  SpawnDecals(const TW3SE_Decal* decals, int count, void* const* entities, int entityCount, uint32_t* outIds);
int  RemoveDecal(uint32_t id);
int  ClearDecals(void* entity);      // the decals made on this entity, NULL: all of yours
int  SetDecalColor(uint32_t id, uint32_t color, uint32_t specularColor, float specularity, float specularScale,
                   float specularBase);
int  DecalCount(void);
int  SetDecalBudget(int perMesh, int total, int chunks);
```

A decal is a picture projected onto the surfaces in its box, the way the game paints blood. On a figure it moves with
the figure. `DecalDefaults` fills a `TW3SE_Decal` with working values; set position, direction, size and the diffuse
texture, then spawn. `entities` NULL paints every surface in the box, else only the meshes of these entities.

```c
TW3SE_Decal d;
api->DecalDefaults(&d);
d.position[0] = x; d.position[1] = y; d.position[2] = z;
d.direction[2] = -1;                                   // straight down
d.width = d.height = 0.05f; d.depth = 0.1f;
d.diffuse = "characters\mymod\fx\splat_d.xbm";
uint32_t id;
api->SpawnDecals(&d, 1, &npc, 1, &id);                 // only on this NPC
```

One mesh takes 2 decals in the game. `SetDecalBudget(127, 2000, 20000)` lets a mesh take up to 127 and raises the
engine's limits for the whole world (0 leaves a value as it is).

`SpawnDecals` writes one id per decal (0 for one that failed, `LastError` says why) and returns how many it made.
Hundreds in one call work. A decal with `lifetime` 0 stays until `RemoveDecal` or `ClearDecals`. Loading a save or
changing the world takes every decal away. Colours are `0xRRGGBBAA`. Main thread only.

## Lua replies in bytes (interface 20)

```c
int LuaReplyBytes(const void* data, size_t size);
```

Inside a Lua function of your plugin: Lua gets these bytes as its string when the function returns 1, any length and
with zero bytes in it. Use it to hand a file's data straight to Lua. The bytes are copied at
once. It works only during the call (it returns 0 elsewhere).

```c
static int ReadFileFn(int argc, const char* const* argv, char* reply, uint32_t size, void*) {
    std::vector<char> data = LoadSomething(argv[0]);
    api->LuaReplyBytes(data.data(), data.size());   // Lua: local bytes = w3.plugin.my_read("x")
    return 1;
}
```

## Strings and names in natives (interface 21)

```c
void     ReturnString(void* result, const char* utf8, uint32_t length);
uint32_t NameFromString(const char* text);   // 0 for "" and "None"
```

A `name` parameter arrives through `ReadParam` as its 4-byte id; return a name by writing its id into `result`.
`GetCNameString` gives the text of an id.

C++: `register_native` takes and returns `std::string` and `tw3se::name` too:

```cpp
static std::string Greet(std::string who) { return "Hello, " + who; }
static tw3se::name Tag(std::string text) { return tw3se::name(text.c_str()); }
tw3se::register_native<&Greet>("MyMod_Greet");   // import function MyMod_Greet(who : string) : string;
tw3se::register_native<&Tag>("MyMod_Tag");       // import function MyMod_Tag(text : string) : name;
```

## Dear ImGui windows (interface 21)

```c
int  RegisterOnImGui(TW3SE_ImGuiCallback callback, void* context, int imguiVersionNum, uint32_t ioSize,
                     uint32_t styleSize);
void ImGuiAllocators(void** allocFn, void** freeFn, void** userData);
void ImGuiOverlay(int on);
void ImGuiCaptureInput(int on);
// TW3SE_ImGuiCallback: void(void* imguiContext, void* context)
```

Your callback runs once per frame inside TW3SE's ImGui frame, on the main thread. TW3SE draws the windows in its
overlay with its fonts and cursor. `ImGuiOverlay(1)` shows them over the game while the TW3SE Menu is closed; the
game keeps the rest of the input. `ImGuiCaptureInput(1)` gives every key and mouse message to ImGui, for a
menu of your own (the controller stays with the game); `0` gives it back. Both count per plugin and go away with it. A
callback that faults will be logged and taken out.

Compile Dear ImGui from the SDK (`include\tw3se\imgui`, the version and settings TW3SE runs) into your DLL.
`RegisterOnImGui` refuses another version or data layout and says so in `tw3se.log`. C++: `tw3se_imgui.h` sets
the context and the allocator for you (`tw3se::imgui::on_draw`, `overlay`, `capture_input`).
`plugins\imgui_example\imgui_example.cpp` is a complete menu.

## Messages, the game's state, the controller (interface 21)

```c
void     SendToChannel(const char* channel, const char* message);
int      RegisterOnMessage(const char* channel, TW3SE_MessageCallback callback, void* context);
uint32_t GameState(void);
int      PadRead(uint32_t* buttons, float* axes);
// TW3SE_MessageCallback: void(const char* channel, const char* message, void* context)
```

A message goes to every listener of its channel: plugins (`RegisterOnMessage`, channel `NULL` or `"*"` for all),
Lua (`w3.on_message`) and scripts (`TW3SE_Receive`). Listeners run on the main thread; a message sent from another
thread goes out at the next tick. The sender gets its own messages too, so two directions use two channels.

`GameState` returns `TW3SE_STATE_*` bits: `MAIN_MENU`, `IN_GAME`, `LOADING`, `PAUSED`, `GAME_MENU` (inventory, map,
options ...), `SCENE` (a dialogue or a cutscene), `TW3SE_MENU`, `CAPTURE` (a plugin's menu has the input). Main
thread only (0 elsewhere).

`PadRead` reads the first XInput controller: `buttons` bits 0-15 are XInput's, 16 and 17 the triggers pressed;
`axes` (6 floats, may be NULL) the sticks (-1..1, dead zones applied) and the triggers (0..1). It returns 1 while a
controller is connected.

C++: `tw3se::send`, `tw3se::on_message`, `tw3se::game_state`, `tw3se::pad`. Values that go with the save and web
requests go through TW3SE's Lua: `tw3se::saved_set(mod, key, value)`, `tw3se::saved_get(mod, key)`,
`tw3se::http_request(method, url, body, channel)` (the answer comes on the channel: the status, a line break, the
body).

## From an .asi mod

An `.asi` mod uses all of this as well. `TW3SE_GetInterface` (exported by `tw3se.dll`) returns the same
struct; `tw3se_asi.h` finds it:

```cpp
#include "tw3se/tw3se_asi.h"
extern "C" __declspec(dllexport) void InitializeASI() {
    if (!tw3se::asi::connect()) return;   // false without TW3SE
    tw3se::register_native<&Greet>("MyMod_Greet");
}
```

Register natives in `InitializeASI` (or `DllMain`), before the game registers its own. `plugins\asi_example` is a
complete one.

## Stable interface

The interface only grows: new functions go at the end, with a new version. A function keeps its place, its arguments
and what it does. A plugin built against an older `tw3se.h` runs on every newer TW3SE.

From interface 26 on, `interfaceSize` holds the size of the interface of the running TW3SE (0 in older ones).
`TW3SE_HAS(api, Function)` tells whether a function is there:

```c
if (TW3SE_HAS(api, SkinHookOverlay)) api->SkinHookOverlay(slot, pixels, 512);
```

## Rendering (interface 21)

```c
int RegisterOnPipeline(TW3SE_PipelineCallback callback, void* context);
int ShaderReplace(uint64_t shaderId, const void* bytecode, size_t size);
int RootSignatureAddParam(uint64_t rootSignatureId, const void* d3d12RootParameter1);
int RootParamIndex(void* rootSignature, int handle);
int MarkPipeline(void* pipelineState, TW3SE_BindCallback callback, void* cookie, void* context);
int RegisterOnSubmit(TW3SE_SubmitCallback callback, void* context);
// TW3SE_PipelineCallback: void(const TW3SE_PipelineInfo* info, void* context)
// TW3SE_BindCallback: void(void* commandList, void* pipelineState, void* rootSignature, void* cookie, void* context)
// TW3SE_SubmitCallback: void(void* commandQueue, void* context)
```

What a ReShade add-on does to the game's pictures, on the game's own D3D12 objects. Register in `TW3SE_PluginLoad`:
the game creates its root signatures and most pipelines while it starts.

- `RegisterOnPipeline` hands you every pipeline the game creates: its `ID3D12PipelineState*`, its root signature and
  the ids of its shaders. The ids are the names of the files `shader_dump = on` writes (see [Shaders](shaders.md));
  `dump\pipelines.txt` lists which shaders and which root signature (`rs=`) make each pipeline.
- `ShaderReplace` puts your bytecode in place of a shader of the game in every pipeline created after the call (the
  same as a file in `tw3se\shaders`, from memory). Up to 16 MB; more, or a bad pointer, returns 0.
- `RootSignatureAddParam` appends a `D3D12_ROOT_PARAMETER1` to the root signature with that id when the game creates
  it. The game's parameters keep their indices, its shaders ignore the new one, and every pipeline and every
  `SetGraphicsRootSignature` of the game uses the extended signature. A signature has room for 64 DWORDs; the game's
  main one (245 of its first 274 pipelines) uses 33. `RootParamIndex` gives your parameter's index in a signature.
  A descriptor table takes up to 32 ranges (they are copied); another parameter type than the five of D3D12 or
  a bad pointer returns 0.
- `MarkPipeline` calls you on the recording thread whenever the game sets that pipeline on a command list whose root
  signature has plugin parameters. Bind your data there, at your index:

```cpp
static void Bind(void* list, void*, void* rs, void*, void*) {
    int i = api->RootParamIndex(rs, g_handle);
    if (i >= 0) static_cast<ID3D12GraphicsCommandList*>(list)->SetGraphicsRootConstantBufferView(i, g_bufferVa);
}
```

`RegisterOnSubmit` calls you once per frame, before the game's first submission to the queue it presents with
(`ID3D12CommandQueue*`). Submit your own command lists there (`ExecuteCommandLists`), and they run before the game's
draws: a compute pass whose result your shaders read, or new values for your buffer.

`plugins\render_example\render_example.cpp` in the SDK has all of it in one file. Tested in the game: a root constant
buffer on the main signature, 314 pipelines marked, half a million binds, a list of the plugin's own every frame, and
13 replaced shaders that drew the plugin's colour.

The device for buffers of your own comes from a pipeline (`ID3D12PipelineState::GetDevice`). TW3SE sees the game's
command lists from their first submission after the game's first frame on. The console command `render` counts the
marked pipelines and the calls.

## Blend shapes (interface 22)

```c
int     DeformOwn(void* meshComponent);
int     DeformLoaded(void* meshComponent);
int     DeformRelease(void* meshComponent);
int32_t DeformLoad(void* meshComponent, const char* path, float range);
int     DeformWeights(int32_t set, const float* weights, int count);
int     DeformState(int32_t set);       // TW3SE_DEFORM_NONE, _LOADING, _READY, _LOST
int     DeformUnload(int32_t set);
int     DeformShapeIndex(int32_t set, const char* name);   // -1 if the set has no shape of that name
```

Lua's `w3.deform` for plugins (see [Blend shapes](blend-shapes.md)): a `.twdf` file of shapes on a mesh, moved on the
graphics card by your weights before skinning, in every pass of the game. `meshComponent` is a `CMeshComponent`.

```cpp
api->DeformOwn(torso);                                   // main thread: this woman's own copy of the mesh
// ... a few ticks later, when api->DeformLoaded(torso) is 1:
int32_t set = api->DeformLoad(torso, "deform\torso.twdf", 2.0f);   // 0: LastError says why
// in the pose callback, every frame:
api->DeformWeights(set, weights, shapeCount);            // file order, one call per mesh
```

- `DeformOwn`, `DeformLoaded`, `DeformRelease` and `DeformLoad` run on the main thread. They make the same copies as
  Lua's `w3.deform.own`, so a Lua mod and a plugin can share a component.
- `DeformWeights` runs on any thread, the pose callback too. It returns how many weights it took (the set's shape
  count at most), 0 for an unknown or unloaded set. The weights show in the next frame the game draws.
- A relative `path` starts in the `tw3se` folder. `range` is the largest weight you plan to use (0 counts as 1).
- A set turns `TW3SE_DEFORM_LOST` when the game unloads the mesh or loads it again (far away, another appearance, a
  save loaded). Load it again then. Release the copies before the game saves.

## Live meshes (interface 26)

```c
int32_t LiveMeshOpen(void* meshComponent);
int     LiveMeshInfo(int32_t id, int chunk, int* vertexCount, int* chunkCount);
int     LiveMeshWrite(int32_t id, int chunk, const float* positions, const float* normals, int count);
int     LiveMeshBounds(int32_t id, const float* min, const float* max);
int     LiveMeshState(int32_t id);      // TW3SE_DEFORM_NONE, _LOADING, _READY, _LOST
int     LiveMeshClose(int32_t id);
```

A live mesh is a mesh whose vertex positions and normals your plugin writes, every frame if you want. The game
skins and draws them in every pass, as it does its own vertices. A rope, a strand or a cloth your plugin simulates
becomes a mesh in the game this way.

```cpp
int32_t id = api->LiveMeshOpen(strands);                 // main thread: the component's own copy of its mesh
// each tick, once api->LiveMeshState(id) is TW3SE_DEFORM_READY:
int n = 0, chunks = 0;
api->LiveMeshInfo(id, 0, &n, &chunks);                   // the chunk's vertex count
api->LiveMeshWrite(id, 0, positions, normals, n);        // n x 3 floats each, model space, metres
```

- `LiveMeshOpen` gives the component a copy of its mesh, as `DeformOwn` does, and returns the live mesh's id (0:
  `LastError` says why). It is `TW3SE_DEFORM_LOADING` until the copy is on the graphics card and the game has drawn
  it, then `TW3SE_DEFORM_READY`. A write while it loads is kept and shows when it is ready.
- `LiveMeshWrite` takes the positions of one chunk in the mesh's model space before skinning. `count` is the chunk's
  vertex count (`LiveMeshInfo` gives it once the copy has loaded). `normals` may be `NULL`; the mesh keeps its normals
  then. Bone indices and weights stay the mesh's. Tangents are turned to stay at a right angle to the new normal.
- `LiveMeshWrite`, `LiveMeshBounds`, `LiveMeshInfo` and `LiveMeshState` run on any thread, the pose callback too. The
  newest write of a chunk shows in the next frame the game draws.
- The game stores positions in a box with 16 bits per axis (a box of 4 m holds them to 0.06 mm). `LiveMeshBounds`
  sets that box in model space; a position outside it stops at its edge. Until you set it, the box of the mesh file
  holds.
- The cooker splits vertices where the UVs of a corner differ and puts them in an order of its own. Take the order of
  the cooked mesh from `w3.deform.dump` (one entry per vertex of each chunk), and write your positions in that order.
- A live mesh turns `TW3SE_DEFORM_LOST` before the game rebuilds its render resources (a graphics option applied) and
  when the copy goes (the figure destroyed, another mesh set on the component). Close it and open it again then.
- `LiveMeshOpen` and `LiveMeshClose` run on the main thread. `LiveMeshClose` gives the component the shared mesh back.
  Close the live meshes before the game saves.

Lua has the same as `w3.deform.live(mesh)`, `w3.deform.live_write(id, chunk, positions, normals)` (flat tables, chunk
from 0), `w3.deform.live_bounds(id, min, max)`, `w3.deform.live_info(id, chunk)`, `w3.deform.state(id)` and
`w3.deform.live_close(id)`.

### Texture pixels

```c
int TextureWrite(const char* depotPath, const void* rgba8, int width, int height, int mips);
```

`TextureWrite` puts your pixels into a loaded texture (a `.xbm` by its depot path). Every decal and material that
shows the texture shows them from a frame soon after on.

- `rgba8` holds `mips` levels one after another, top level first, 4 bytes a pixel, red first. With fewer levels than
  the texture has, TW3SE makes the others (each pixel the mean of four).
- `width` and `height` are the texture's. TW3SE encodes the pixels to the texture's format (BC3, RGBA8, BGRA8) on a
  thread of its own and returns at once. A newer write of a texture drops an older one that is still waiting.
- The texture has to be in memory at its full size. The game streams most texture groups and keeps only 32 x 32
  of them; a texture cooked in the group GUIWithAlpha stays at its full size.
- Main thread. 0 and `LastError` for an unknown path, another size or another format.

A texture of your own while the game runs: `tools\xbm_placeholder.py` makes a GUIWithAlpha texture of any size from
the game's own (`python tools\xbm_placeholder.py "<game>" 1024 my_1024.xbm`). Put it under `tw3se\files\` at a depot
path of your own, point a mesh's material at that path, and write its pixels with `TextureWrite`.
`w3.files.alias(path, newPath)` gives it more paths while the game runs, each one a texture of its own of the same
size.

### Normals of moved meshes

```c
int DeformNormals(int32_t setOrLiveMesh, int mode);
```

With `mode` 1 the normals and tangents of a blend set or a live mesh follow its moved triangles. Each vertex keeps
its own normal and tangent, turned as the surface around it turned, so hard edges and baked normals stay. The copies
of a vertex along a UV seam turn as one. Mode 1 takes the place of a file's normal deltas and of the normals given to
`LiveMeshWrite`. Mode 0 puts the mesh's own normals back. Any thread.

## The skinning hook (interface 26)

```c
int SkinHookCapsulesEx(int slot, const float* capsules, int count);
int SkinHookDeltaMush(int slot, int iterations);
int SkinHookMushMask(int slot, int chunk, const float* mask, int count);
int SkinHookOverlay(int slot, const void* rgba8, int size);
int SkinHookOverlayMaterial(int slot, const void* rgba8, int size);
```

They need `skin_hook = on` in `tw3se.config` and the patched shaders in `tw3se\shaders` (`tools/skin_build.py`, made
for the game's build). The DLL and the shaders go in together; a mismatch stands in `tw3se.log`.

- `SkinHookCapsulesEx` takes up to 32 capsules of 12 floats: a.xyz, radius, b.xyz, strength, falloff, mode, 0, 0
  (model space, metres). Mode 0 or more pushes the skin out; with a falloff the edge of the dent is soft. A mode below
  0 draws the skin that lies within the falloff outside the capsule onto it. The skin's normal turns with the dent,
  so the light shows it (shaders of `skin_build.py` of TW3SE 1.3.4 or later). The meshes of one figure are skinned in
  one model space: give the same capsules to two slots and one mesh presses on the other.
- `SkinHookDeltaMush` smooths the skinned mesh (`iterations` steps over its triangles; 8 is a good start, 0 turns it
  off) and puts its rest detail back on the smoothed surface, so a joint that bends keeps its volume. It is one frame
  behind the skinning. TW3SE reads the mesh when you turn it on; the slot is ready a moment later (`tw3se.log`). Turn
  it on while the mesh has its rest shape (blend set weights at 0). `SkinHookMushMask` sets how much each vertex of a
  chunk takes, 0 to 1.
- `SkinHookOverlay` lays a picture over the skin of the slot's mesh: `size` x `size` texels, RGBA8 (red first), up
  to 512, at the mesh's UV. Alpha is how much of it covers the skin. It is mixed into the skin's colour before the
  light, the subsurface stays. Size 0 takes it away. The skin's pixel shaders take it (body, hands, head), clothing
  keeps its own colour.
- `SkinHookOverlayMaterial` is the second picture of the overlay, at the same texels. R makes the skin wet with the
  game's own rain look (glossy, the colour deeper). G makes it matte, for dried spots. B is a height: where it rises
  or falls the normal tilts, so the edge of a thick drop catches the light (B = 255 stands 16 texels high). A is
  unused. A size other than the colour's clears the colour, and the other way round. It needs the shaders of
  `skin_build.py` of TW3SE 1.3.4 or later (`overlay 2` in `skinhook.txt`).

Lua: `w3.skinhook.capsules(slot, {{a = {x, y, z}, b = {x, y, z}, radius = r, strength = s, falloff = k, inward =
true}})`, `w3.skinhook.mush(slot, n)`, `w3.skinhook.mush_mask(slot, chunk, {...})`.
`w3.skinhook.overlay(slot, size, colour, material)` sets both pictures from two strings of 4 x size x size bytes
(nil clears one). `w3.skinhook.capture(slot, path)` writes the skinned positions of the slot's mesh a few frames later (with ray
tracing on).

## Dual quaternion skinning (interface 28)

```c
int SkinHookDqs(int slot, int on);
```

`SkinHookDqs(slot, 1)` skins the slot's mesh with dual quaternions in place of the game's linear blend. The bones and
the weights stay the same. A joint that bends far (a hip at 130 degrees, an elbow, a twisted wrist) keeps its volume,
where the linear blend lets the skin fall in. The scale of a bone (breasts, `w3.anim.shape`) works as in the game.
The normals turn with it, so the light falls right in a fold. Delta Mush and the capsules work on top of it.
`SkinHookDqs(slot, 0)` gives the game's skinning back.

Each bone is blended from the side of its rest pose, the same for every vertex, so neighbouring vertices stay together. A bone turned half a turn or more away from its rest pose can make the blend jump there.

It needs the shaders of `skin_build.py` of TW3SE 1.4.0 or later (`dqs 1` in `skinhook.txt`). Lua:
`w3.skinhook.dqs(slot, true)`.

## Engine functions by name (interface 27)

```c
void* fn = api->EngineAddress("CEntity::vf140");     // NULL if there is none; LastError says why
void* vt = api->EngineVtable("CEntity", 0);          // the class's vtable
```

`EngineAddress` takes a name of the address list (as `GetAddress`), a slot of a class's vtable (`"CEntity::vf140"`,
`"CEntity::vf0@10"` for the vtable at offset 0x10 of the object) or an offset in witcher3.exe (`"0x2263570"`). A slot
is found by the class's name in the exe, so it holds after a patch as long as the class keeps its slots.
`EngineVtable` gives the vtable itself. `tools/enginemap.py class <Name>` lists a class's slots.
