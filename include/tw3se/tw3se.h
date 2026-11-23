/* TW3SE (The Witcher 3 Script Extender) - plugin interface.
 *
 * A plugin is a DLL in <game>\bin\x64_dx12\tw3se\plugins\ that exports:
 *
 *     TW3SE_EXPORT int TW3SE_PluginQuery(TW3SE_PluginInfo* info);          // fill info, return 1 to be loaded
 *     TW3SE_EXPORT int TW3SE_PluginLoad(const TW3SE_Interface* tw3se);     // return 1 on success
 *
 * Both run on the game's main thread before the engine starts up (scripts not compiled, no world yet).
 * Keep the interface pointer; it stays valid for the whole session.
 *
 * Optional (interface 17): `plugin reload <name>` unloads a plugin and loads its DLL again while the game runs.
 *
 *     TW3SE_EXPORT int TW3SE_PluginUnload(void);   // undo what lives outside TW3SE, return the ticks still needed
 *
 * runs on the main thread before that; its commands, natives and Lua functions are off by then, its tick, pose and
 * mimic callbacks keep running for the returned number of ticks (0 = at once). Everything it registered through
 * the interface is taken out after that, then the DLL is freed. The plugin runs from a copy (<name>.live<n>.dll
 * next to it), so a build can overwrite the original; config plugin_reload = off loads the originals.
 *
 * Rules of the interface (it is meant to stay stable for years):
 *   - C ABI only, no C++ types across the boundary.
 *   - New functions are only ever appended; check `interfaceVersion` before using newer ones.
 *   - Strings are UTF-8, zero-terminated. Returned strings stay valid until the next call on the same thread.
 *   - A NULL where a function needs a pointer makes it return 0 and change nothing; the `result` of the Call functions
 *     may be NULL.
 */
#ifndef TW3SE_H
#define TW3SE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define TW3SE_EXTERN_C extern "C"
#else
#define TW3SE_EXTERN_C
#endif
#define TW3SE_EXPORT TW3SE_EXTERN_C __declspec(dllexport)

#define TW3SE_INTERFACE_VERSION 29

#define TW3SE_VERSION_MAJOR 1
#define TW3SE_VERSION_MINOR 4
#define TW3SE_VERSION_PATCH 2

typedef struct TW3SE_PluginInfo {
    uint32_t infoVersion;      /* set by TW3SE: 1 */
    const char* name;          /* set by the plugin: short unique name, e.g. "LiveQuestReload" */
    const char* author;        /* set by the plugin */
    uint32_t version;          /* set by the plugin: its own version, e.g. 0x00010000 for 1.0.0 */
    uint32_t minInterface;     /* set by the plugin: lowest TW3SE_INTERFACE_VERSION it needs */
} TW3SE_PluginInfo;

typedef void (*TW3SE_Task)(void* context);
/* Console command handler: writes its answer to `reply` (size `replySize`). Runs on the main thread. */
typedef void (*TW3SE_Command)(const char* args, char* reply, uint32_t replySize);
/* A function a plugin gives to Lua (interface 16): the Lua arguments as text, the answer as text. Return 1 when it
   worked (Lua gets `reply`), 0 for an error (Lua raises `reply` as the error message). */
typedef int (*TW3SE_LuaFunction)(int argc, const char* const* argv, char* reply, uint32_t replySize, void* context);
/* Per-tick callback: called once every game tick on the main thread (also in the main menu). `deltaSeconds`
   is the frame time. Keep it short; it runs every frame. A callback that faults is dropped (logged once). */
typedef void (*TW3SE_TickCallback)(float deltaSeconds, void* context);
/* Per-present callback: called every frame from the render thread with the live DXGI swap chain (cast to
   IDXGISwapChain3*), for drawing an overlay. A callback that faults is dropped (logged once). */
typedef void (*TW3SE_PresentCallback)(void* swapChain, void* context);
/* Per-frame overlay-draw callback (interface v10): called every frame while the overlay records its command
   list. Inside it, call Interface.OverlayDrawText / OverlayDrawRect to draw; they do nothing outside it. A
   callback that faults is dropped (logged once). */
typedef void (*TW3SE_OverlayDrawCallback)(void* context);
/* Interface 21: called once per frame inside TW3SE's Dear ImGui frame, on the main thread. `imguiContext` is the
   ImGuiContext* to draw into (ImGui::SetCurrentContext). tw3se_imgui.h does all of it. */
typedef void (*TW3SE_ImGuiCallback)(void* imguiContext, void* context);
/* Interface 21: a message on a channel (TW3SE_Send in a script, w3.send in Lua, SendToChannel in a plugin). */
typedef void (*TW3SE_MessageCallback)(const char* channel, const char* message, void* context);

/* Interface 21: a pipeline the game created (RegisterOnPipeline). Shader ids are TW3SE's (FNV-1a 64 of the bytecode,
   the names of the files in tw3se\shaders\dump), the root signature's id the same over its serialized blob. */
typedef struct TW3SE_PipelineInfo {
    void* pipelineState;       /* ID3D12PipelineState* */
    void* rootSignature;       /* ID3D12RootSignature* it uses (an extended one when a plugin added parameters) */
    uint64_t rootSignatureId;  /* the id of the game's signature (as in dump\pipelines.txt "rs=") */
    uint64_t vs, ps, ds, hs, gs, cs;  /* the game's shaders (0: none), before a replacement */
    int compute;
} TW3SE_PipelineInfo;
typedef void (*TW3SE_PipelineCallback)(const TW3SE_PipelineInfo* info, void* context);
/* Interface 21: the game set a pipeline you marked (MarkPipeline) on a command list whose root signature has your
   parameters: bind them now (ID3D12GraphicsCommandList* commandList, at the index RootParamIndex gives). */
typedef void (*TW3SE_BindCallback)(void* commandList, void* pipelineState, void* rootSignature, void* cookie,
                                   void* context);
/* Interface 21: once per frame, before the game's first submission to its graphics queue (ID3D12CommandQueue*):
   submit your own work there first (a compute pass whose result the game's draws read). */
typedef void (*TW3SE_SubmitCallback)(void* commandQueue, void* context);

/* GameState bits (interface 21) */
#define TW3SE_STATE_MAIN_MENU  0x01  /* no player: the main menu (or the game's start) */
#define TW3SE_STATE_IN_GAME    0x02  /* the player is in the world */
#define TW3SE_STATE_LOADING    0x04  /* a loading screen or a black screen */
#define TW3SE_STATE_PAUSED     0x08  /* the game is paused */
#define TW3SE_STATE_GAME_MENU  0x10  /* one of the game's menus is open (inventory, map, options ...) */
#define TW3SE_STATE_SCENE      0x20  /* a dialogue or a cutscene plays */
#define TW3SE_STATE_TW3SE_MENU 0x40  /* the TW3SE Menu (F2) is open */
#define TW3SE_STATE_CAPTURE    0x80  /* a plugin's menu has the input (ImGuiCaptureInput) */

/* DeformState (interface version 22) */
#define TW3SE_DEFORM_NONE    0  /* no such set (unloaded, or never loaded) */
#define TW3SE_DEFORM_LOADING 1  /* loaded, its data not on the graphics card yet */
#define TW3SE_DEFORM_READY   2
#define TW3SE_DEFORM_LOST    3  /* the game unloaded the mesh or loaded it anew: DeformLoad again */

/* Game-loaded callback (interface v11): called after a new game starts or a save finishes loading, so a plugin
   can re-apply persistent changes (world edits, mod state). A callback that faults is dropped (logged once). */
typedef void (*TW3SE_GameLoadedCallback)(void* context);
/* Graphics rebuild (interface v25): `phase` TW3SE_REBUILD_BEFORE or _AFTER, see RegisterOnRenderRebuild. */
#define TW3SE_REBUILD_BEFORE 0
#define TW3SE_REBUILD_AFTER  1
typedef void (*TW3SE_RenderRebuildCallback)(int phase, void* context);
/* WitcherScript native handler: `self` is the caller's context, `frame` the script stack frame, `result`
   the return slot. Read parameters with Interface.ReadParam(frame, &out) in declaration order; write the
   return value through `result`. See docs/ABI_NOTES.md. */
typedef void (*TW3SE_ScriptNative)(void* self, void* frame, void* result);

/* ---- Pose and mimic hooks (interface version 13) ---- */

/* Which animated component a pose/mimic callback is for. An actor has several animated components (body, head,
   sometimes cloth or hair); a plugin picks the ones it wants by `kind` and ignores the rest. */
typedef enum TW3SE_PoseKind {
    TW3SE_POSE_BODY  = 0,  /* the actor's main body skeleton (the root CAnimatedComponent) */
    TW3SE_POSE_MIMIC = 1,  /* the head's mimic / face component (CMimicComponent) */
    TW3SE_POSE_OTHER = 2   /* another animated component (cloth, hair, an attached entity) */
} TW3SE_PoseKind;

/* One skeleton's pose for a frame. The callback runs after animation and constraints are evaluated and before
   skinning, on the thread that evaluates the pose. `local` is writable: what the plugin writes there is skinned.
   A mesh has its own bone order (slots); `parents[i]` is slot i's own bone in the skeleton GetPose returns and
   `boneNames[i]` its name ("" for the bones past CSkeleton.lodBoneNum_1). `local` holds one 4x4 matrix per slot
   (`localStride` 16), row-major, translation in the last row (DirectX); the game draws local x model, with model the
   animated model matrix of the slot's own bone, and fills local with the bone's inverse bind matrix. To draw a bone
   at model matrix N: local = inverse(bind) x N x inverse(model). A written matrix stays until it is written again:
   put the bones back when you let go. `modelToWorld` is the entity's local-to-world matrix in the same layout, a
   copy for this call. */
typedef struct TW3SE_Pose {
    void*        entity;         /* the CEntity* this pose belongs to */
    void*        component;      /* the CAnimatedComponent* that produced it */
    int32_t      kind;           /* a TW3SE_PoseKind */
    uint32_t     guid[4];        /* the entity's GUID (all zero for a run-time entity) */
    int32_t      boneCount;
    const char* const* boneNames;   /* [boneCount], in pose order */
    const int16_t* parents;         /* GetPose: [boneCount] each bone's parent, -1 for the root.
                                       Pose callback: [boneCount] each slot's OWN index in the skeleton GetPose
                                       returns (the mesh's bone order mapped to the skeleton), not its parent. */
    float*       local;             /* [boneCount * localStride], writable */
    int32_t      localStride;       /* floats per bone (filled by TW3SE) */
    const float* modelToWorld;      /* 4x4 row-major, the entity this frame */
    /* ---- interface version 14 ---- */
    void*        mesh;              /* a stable id for the mesh this combine drives: several meshes of one figure
                                       (torso, legs, head, body) each fire their own callback in a frame with a
                                       distinct `mesh`. Use it to tell them apart within a frame. */
} TW3SE_Pose;
typedef void (*TW3SE_PoseCallback)(void* entity, TW3SE_Pose* pose, void* context);

/* The head's mimic track weights for a frame. The callback runs after the game has mixed the scene's expression
   tracks and before they become face bones. `weights` is writable and arrives already filled with the mixed
   values: overwrite only your own tracks (by index, found once from `trackNames`) and leave the rest, so the
   scene's own lipsync, eyes and brows stay. Negative weights are allowed. */
typedef struct TW3SE_Mimic {
    void*        entity;         /* the CEntity* */
    void*        component;      /* the CMimicComponent* */
    uint32_t     guid[4];
    int32_t      trackCount;
    const char* const* trackNames;  /* [trackCount] */
    float*       weights;           /* [trackCount], writable */
} TW3SE_Mimic;
typedef void (*TW3SE_MimicCallback)(void* entity, TW3SE_Mimic* mimic, void* context);

/* ---- Values (interface version 18) ----
   One value of the game, the way Lua sees it. A name and an enum value travel as a STRING ("EDT_Default"), an int
   enum value as an INT works too. A struct is its bytes laid out as in the game plus its type name; a table of its
   fields is not needed. An array is a list of values. */
typedef enum TW3SE_ValueType {
    TW3SE_NIL    = 0,
    TW3SE_BOOL   = 1,  /* v.i: 0 or 1 */
    TW3SE_INT    = 2,  /* v.i */
    TW3SE_FLOAT  = 3,  /* v.f */
    TW3SE_STRING = 4,  /* v.s: UTF-8; also names and enum values */
    TW3SE_OBJECT = 5,  /* v.object: a CObject* (the object itself, as GetThePlayer returns it) */
    TW3SE_STRUCT = 6,  /* v.data: `count` bytes of the struct `typeName` ("Vector", "EulerAngles") */
    TW3SE_ARRAY  = 7,  /* v.items: `count` values */
    TW3SE_OTHER  = 8   /* a value C has no type for (a function, a latent call): v.s describes it */
} TW3SE_ValueType;

typedef struct TW3SE_Value {
    int32_t type;          /* a TW3SE_ValueType */
    uint32_t count;        /* STRUCT: its size in bytes; ARRAY: the number of items */
    const char* typeName;  /* STRUCT: its type, needed when you pass one */
    union {
        int64_t i;
        double f;
        const char* s;
        void* object;
        const void* data;
        struct TW3SE_Value* items;
    } v;
} TW3SE_Value;

/* A hooked WitcherScript or native function (HookScript). `args` holds the call's arguments and is writable: what you
   write there is what CallOriginal and the next handler get. Return 1 and fill `result` to give the call your result
   (the original is skipped unless you called CallOriginal); return 0 to leave it to the next handler and, at the end,
   the original. Runs on the main thread. `call` is valid during the callback only. */
typedef struct TW3SE_HookCall TW3SE_HookCall;
typedef int (*TW3SE_ScriptHook)(void* self, TW3SE_Value* args, int argCount, TW3SE_HookCall* call,
                                TW3SE_Value* result, void* context);

/* One field or method of a class (ForEachField / ForEachMethod). Return 1 to stop. The strings live during the
   callback; copy what you keep. */
typedef struct TW3SE_ItemInfo {
    const char* name;
    const char* typeName;   /* field: its type; method: the return type ("" for none) */
    const char* ownerClass; /* the class that declares it (a parent for an inherited one) */
    uint32_t offset;        /* field: its offset (in the script data block for a script field); method: 0 */
    uint32_t flags;         /* field: CProperty flags; method: CFunction flags (0x40 latent, 0x1 native) */
    void* item;             /* the CProperty* or CFunction* */
} TW3SE_ItemInfo;
typedef int (*TW3SE_ItemCallback)(const TW3SE_ItemInfo* info, void* context);
/* One object (ForEachObject). Return 1 to stop. */
typedef int (*TW3SE_ObjectCallback)(void* object, void* context);

/* A dynamic decal (interface 19). Colours are 0xRRGGBBAA. Sizes in metres: width and height of the picture, depth
   along `direction` (half in front of `position`, half behind). */
typedef struct TW3SE_Decal {
    float position[3];        /* the middle of the decal in the world */
    float direction[3];       /* the projection direction, onto the surface */
    float up[3];              /* the top of the picture, in its plane (turns it) */
    float width, height, depth;
    const char* diffuse;      /* texture path (.xbm), its alpha is the shape */
    const char* normal;       /* texture path or NULL (the surface's normals) */
    uint32_t color;           /* multiplies the texture */
    uint32_t specularColor;
    float specularity, specularScale, specularBase;  /* gloss; specularity -1: from the texture */
    float lifetime;           /* seconds, 0: until removed */
    float fadeIn, fadeOut;    /* seconds */
    int32_t projection;       /* 0 straight (same size through the depth), 1 sphere (smaller towards the ends) */
    int32_t doubleSided;      /* 1: also on surfaces facing away */
    int32_t priority;         /* 1: kept longer when the renderer is over its budget */
    int32_t staticOnly;       /* 1: only on static geometry */
    float depthFadePower, normalFadeBias, normalFadeScale;
    float autoHideDistance;   /* metres, below 0: from the decal's size */
    float atlas[4];           /* the part of the texture: scale u, scale v, offset u, offset v (0.5 each: all) */
} TW3SE_Decal;

typedef struct TW3SE_Interface {
    uint32_t interfaceVersion;  /* TW3SE_INTERFACE_VERSION of the running extender */
    uint32_t extenderVersion;   /* (major << 16) | (minor << 8) | patch */
    uint32_t gameBuild;         /* PE timestamp of witcher3.exe */
    uint32_t interfaceSize;     /* sizeof(TW3SE_Interface) of the running extender (0 before interface 26): a function
                                   lies past it when the extender is older than your header */

    /* Writes a line to tw3se.log, prefixed with the plugin name. printf-style. */
    void (*Log)(const char* plugin, const char* format, ...);

    /* Address of a named engine function or global, 0 if unknown in this build of the game.
       Names as in TW3SE's address table, e.g. "CContentManager::AttachPackage", "GContentManager". */
    uintptr_t (*GetAddress)(const char* name);

    /* Base address of witcher3.exe in memory. */
    uintptr_t (*GetImageBase)(void);

    /* Hooks `target` (an engine function): `detour` is called instead, `*original` receives a callable
       pointer to the original. Returns 1 on success. Hooks are enabled immediately. */
    int (*Hook)(void* target, void* detour, void** original);

    /* Runs `task(context)` on the game's main thread at the end of the next game tick. Thread-safe. */
    void (*RunOnMainThread)(TW3SE_Task task, void* context);

    /* Adds a command to the developer console (pipe \\.\pipe\tw3se, see tools/tw3se.py). Returns 1 on success. */
    int (*RegisterCommand)(const char* name, TW3SE_Command handler, const char* help);

    /* Mounts a DLC folder (the one containing content\) into the running game. Main thread only.
       Writes a short result to `reply`. Returns 1 on success. */
    int (*MountDLC)(const char* dlcFolder, char* reply, uint32_t replySize);

    /* --- interface version 2 --- */

    /* Registers a new WitcherScript native under `name` with `paramCount` parameters. Call during
       TW3SE_PluginLoad (before the engine registers natives). A script then uses it via
       `import function <name>(...) : <type>;` - the declared parameter count must equal `paramCount`. Returns 1. */
    int (*RegisterNative)(const char* name, TW3SE_ScriptNative native, int paramCount);

    /* Reads one parameter of the calling script into `out` (inside a TW3SE_ScriptNative handler), in
       declaration order. A String arrives as a pointer to char* in `out` (give >=16 bytes); int/bool/enum as
       4 bytes. See docs/ABI_NOTES.md. */
    void (*ReadParam)(void* frame, void* out);

    /* Call once inside a native handler, AFTER reading all parameters and before returning. Required even for
       a native with no parameters. */
    void (*EndParams)(void* frame);

    /* --- interface version 3 --- */

    /* Registers `callback` to be called once every game tick on the main thread (also in the main menu), with
       the frame time in seconds and `context`. Useful for hotkey polling, timers and overlays. A callback that
       faults is dropped and logged once. It stays registered while the plugin is loaded. */
    void (*RegisterOnTick)(TW3SE_TickCallback callback, void* context);

    /* --- interface version 4 --- */

    /* Generic RTTI property access: read or write any reflected field of an object. `cclass` is the object's
       CClass* (from RTTI); FindProperty walks the inheritance chain and returns the field's descriptor
       (CProperty*), or NULL. PropertyPtr returns the address of that field inside `object` - cast it to the
       field's known type (int/float: 4 bytes, bool: 1, handle: 8, string: a char* at the front) and read or
       write it. Example: *(float*)PropertyPtr(player, FindProperty(playerClass, "vitality")) = 1000.f;
       (A CClass* by name or an object's class: GetObjectClass and FindClassByName, interface 5.) */
    void* (*FindProperty)(void* cclass, const char* name);
    void* (*PropertyPtr)(void* object, void* cproperty);

    /* --- interface version 5 --- */

    /* The two ways to obtain a CClass* for the property functions above. GetObjectClass returns an object's
       own class (virtual GetClass, vtable slot 1) - use it when you have an object (e.g. `this` in a hook, or
       the player). FindClassByName looks a class up by name (no object needed). With either, the full chain is
       e.g.:  void* cls = GetObjectClass(player);
              *(float*)PropertyPtr(player, FindProperty(cls, "vitality")) = 1000.f;
       FindClassByName passes the name through the engine's generic RTTI-type lookup and validates the result
       is a class (not an enum/simple type), returning NULL otherwise. */
    void* (*GetObjectClass)(void* object);
    void* (*FindClassByName)(const char* name);

    /* --- interface version 6 --- */

    /* Calls a globally-registered WitcherScript native from C++. `stringArgs`/`argCount` are the string
       arguments (only string arguments are supported for now); `self` is the context (NULL for plain globals);
       `result` receives the return value (int/bool, caller-sized). Returns 1 if the function was found and
       invoked, 0 otherwise. Example:
         const char* a[] = {"hello"}; int n = 0; api->CallFunction("StrLen", a, 1, NULL, &n);  // n == 5 */
    int (*CallFunction)(const char* name, const char* const* stringArgs, int argCount, void* self, void* result);

    /* --- interface version 7 --- */

    /* The current player object, or NULL in the main menu / before a spawn; and the game object. Both are
       ready to pass to GetObjectClass / the property functions. */
    void* (*GetThePlayer)(void);
    void* (*GetTheGame)(void);

    /* Calls a method-native on `object` (a function on the object's class, like GetMovingAgentComponent) with
       string arguments. `result` receives the return value. Returns 1 if found and called. A method that
       returns an object/component returns a handle (see ResolveHandle). */
    int (*CallMethod)(void* object, const char* name, const char* const* stringArgs, int argCount, void* result);

    /* The real object inside a handle return value (object/component returns are reference-counted handles,
       not raw pointers). Pass it the handle; it returns the object pointer at handle+0x08, or NULL. */
    void* (*ResolveHandle)(void* handle);

    /* The text of a CName id (for reflection: a property's or function's name id). Valid for the session. */
    const char* (*GetCNameString)(uint32_t id);

    /* --- interface version 8 --- */

    /* Registers a per-present callback for drawing an overlay. It is called every frame from the render
       thread with the live DXGI swap chain (cast to IDXGISwapChain3*) and `context`. Obtain the device and
       back buffer from the swap chain to draw. A callback that faults is dropped and logged. No unregister. */
    void (*RegisterOnPresent)(TW3SE_PresentCallback callback, void* context);

    /* --- interface version 9 --- */

    /* Call a global function / a method with typed arguments, given as a space-separated typespec string:
       "i:N" int, "f:N" float, "s:text" string, "n:text" name, "b:1" / "b:0" bool, "e:N" an enum's value (0 to
       255); a bare token is a string. For example
       api->CallFunctionV("StrLen", "s:witcher", NULL, &n) or api->CallMethodV(obj, "SetThing", "i:5", &r).
       `self` is the context (NULL for a plain global). Returns 1 if called. (These are the argument types it
       takes; Call (interface 18) takes TW3SE_Value.) */
    int (*CallFunctionV)(const char* name, const char* argspec, void* self, void* result);
    int (*CallMethodV)(void* object, const char* name, const char* argspec, void* result);

    /* ---- Overlay drawing (interface version 10) ----
       Register a per-frame draw callback, then draw inside it. Coordinates are in pixels from the top-left;
       OverlayGetScreenSize gives the back-buffer size. Colour is 0xRRGGBBAA. The draw calls only take effect
       while your callback is running (the overlay is recording its command list then). Drawing works over the
       game's own HUD and coexists with DLSS Frame Generation. */
    void (*RegisterOnOverlayDraw)(TW3SE_OverlayDrawCallback callback, void* context);
    void (*OverlayDrawText)(float x, float y, uint32_t rgba, const char* text);
    void (*OverlayDrawRect)(float x, float y, float w, float h, uint32_t rgba);
    void (*OverlayGetScreenSize)(float* outWidth, float* outHeight);

    /* ---- Game lifecycle (interface version 11) ----
       Register a callback fired after a new game starts or a save finishes loading. Use it to re-apply changes
       that must survive a reload (removed/moved world objects, persistent state) by reading them back from disk
       (see the file functions) and applying them. Runs on the main thread. */
    void (*RegisterOnGameLoaded)(TW3SE_GameLoadedCallback callback, void* context);

    /* ---- Objects of the game and world edits (interface version 12) ----
       EntityByGuid: the loaded object with this GUID (the 16 bytes a script reads as four hex dwords with
       TW3SE_EntityGuid), or NULL when it is not loaded right now. Use the pointer within the current tick and
       keep the GUID, not the pointer: the object can stream out. EntityGuid writes an entity's GUID; returns 0
       for an entity the game made at run time (its GUID is a counter that does not come back).
       WorldEdit applies one world.txt line (docs/manual/world-edit.md) for this session, at once if the object
       is loaded; returns 1 if the line was read. What has to hold after a load goes into the mod's
       tw3se\world.txt. Main thread. */
    void* (*EntityByGuid)(const uint32_t guid[4]);
    int (*EntityGuid)(void* entity, uint32_t outGuid[4]);
    int (*WorldEdit)(const char* line);

    /* ---- Pose and mimic hooks (interface version 13) ----
       Live skeleton and face control for an animated entity, per frame, after animation and before skinning.
       RegisterOnPose / RegisterOnMimic install a callback (each plugin its own, up to 16; NULL takes the calling plugin's
       out); it fires only for entities turned on with WatchPose /
       WatchMimic (per entity, so idle NPCs cost nothing). `on` is 1 to watch, 0 to stop; returns 1 on success.
       WatchPose takes the entity (every mesh it renders fires, each with its own `mesh`) or one of its animated
       components; call it on the main thread. The engine hook is installed by the first RegisterOnPose (config
       posehook=off keeps it out).
       GetPose reads another watched entity's last evaluated pose (read-only; one frame of delay is fine) into
       `out` for a given TW3SE_PoseKind - for an interaction where one skeleton needs another's bones. BODY is the
       root animated component's skeleton, MIMIC the head's (CMimicComponent) face skeleton, both in model space
       with names. Returns 1 if a pose was written (0 until that component was seen in a frame). Each pose callback
       carries the kind of its component: MIMIC for the head, BODY for the root animated component, OTHER for the
       rest. WatchPose on an entity that is watched already changes nothing (0 first, then 1 to read it again). A watched figure
       takes the items that hang on it along (Geralt's head is an item entity with its own CMimicComponent); a
       search over the figures in view finds them while a figure is watched. A component or an item entity watched
       itself fires alone and needs no search (Geralt's face alone: watch his head item or its CMimicComponent).
       GetPose with a watched component reads its entity's pose; WatchMimic alone is enough for MIMIC. The callbacks
       run on the engine's animation thread; see docs/engine/pose.md. */
    void (*RegisterOnPose)(TW3SE_PoseCallback callback, void* context);
    int (*WatchPose)(void* entity, int on);
    int (*GetPose)(void* entity, int kind, TW3SE_Pose* out);
    void (*RegisterOnMimic)(TW3SE_MimicCallback callback, void* context);
    int (*WatchMimic)(void* entity, int on);

    /* ---- Host (interface version 15) ----
       1 when TW3SE runs inside the REDkit editor (editor.exe), 0 in the game. In the editor a plugin's features
       work in its Play mode; some engine addresses differ, GetAddress answers 0 for those the editor lacks. */
    int (*IsEditor)(void);

    /* ---- Lua (interface version 16) ----
       RunLua runs Lua code like the console command `lua` and writes its output or returned values into `reply`;
       1 when it ran, 0 on an error (the message in `reply`). Main thread only (RunOnMainThread from elsewhere).
       RegisterLuaFunction gives Lua mods w3.plugin.<name>(...): the arguments arrive as text, the reply goes back as
       a string. The same name again replaces the function. */
    int (*RunLua)(const char* code, char* reply, uint32_t replySize);
    int (*RegisterLuaFunction)(const char* name, TW3SE_LuaFunction function, void* context);

    /* ---- Sounds (interface version 17) ----
       PlaySound plays a WAV or OGG file through XAudio2 and returns a handle, 0 on an error (logged). `file` is an
       absolute path or one from the game folder ("mods\modX\sounds\a.ogg"). With an entity the sound is in
       3D at it, or at its bone `bone` (NULL: the entity), heard from the camera; entity NULL plays it flat.
       `volume` 1 is the file as it is (0 counts as 1), `loop` 1 repeats it until StopSound. A sound at an entity
       ends when the entity is gone. Any thread. A file is decoded on its first use and kept; PreloadSound decodes
       it ahead (a big file takes a moment, do it on the main thread). The functions return 1 when the sound was
       there. */
    int32_t (*PlaySound)(const char* file, void* entity, const char* bone, float volume, int loop);
    int (*StopSound)(int32_t sound);
    int (*SetSoundVolume)(int32_t sound, float volume);
    int (*SoundPlaying)(int32_t sound);
    int (*PreloadSound)(const char* file);

    /* ---- Calls, fields, hooks and the catalog with typed values (interface version 18) ----
       The same reach as Lua: every function of the game (native or WitcherScript, global or method) with arguments of
       any type, every field, hooks on every function. All of them run on the main thread (RunOnMainThread from
       elsewhere). Values a function hands back (strings, struct bytes, array items) stay valid until the next of
       these calls on the thread; an object pointer within the current tick.

       Call: object NULL calls a global function, else the method `name` of the object. Writes the return value and
       then each out argument into `results` (up to `maxResults`) and returns their number, -1 on an error
       (LastError says why). A latent function starts on a script thread of the game and comes back as an OTHER.
       GetField / SetField read and write a field by name (a script field too); 1 when it worked.
       HookScript puts `hook` in front of a function (className NULL for a global); several handlers form a chain, the
       highest priority first. Returns an id for Unhook, 0 on an error. CallOriginal (inside the hook) runs the rest
       of the chain and the original with the current `args`, writes its return into `result`, 1 when it ran.
       ClassName / ClassParent / ForEachField / ForEachMethod read the catalog (a CClass* from FindClassByName or
       GetObjectClass; fields and methods include inherited ones). EnumValue gives the number of a value name.
       ForEachObject visits every live object of a class and its subclasses (entities, components, quest objects,
       spawned ones too; the pattern objects inside entity templates left out) and returns how many it visited; the
       callback may make or destroy objects. */
    int (*Call)(void* object, const char* name, const TW3SE_Value* args, int argCount, TW3SE_Value* results, int maxResults);
    int (*GetField)(void* object, const char* field, TW3SE_Value* out);
    int (*SetField)(void* object, const char* field, const TW3SE_Value* value);
    const char* (*LastError)(void);
    int (*HookScript)(const char* className, const char* name, TW3SE_ScriptHook hook, void* context, int priority);
    int (*Unhook)(int id);
    int (*CallOriginal)(TW3SE_HookCall* call, TW3SE_Value* result);
    const char* (*ClassName)(void* cclass);
    void* (*ClassParent)(void* cclass);
    int (*ForEachField)(void* cclass, TW3SE_ItemCallback callback, void* context);
    int (*ForEachMethod)(void* cclass, TW3SE_ItemCallback callback, void* context);
    int (*EnumValue)(const char* enumName, const char* valueName, int64_t* out);
    int (*ForEachObject)(const char* className, TW3SE_ObjectCallback callback, void* context);

    /* ---- Dynamic decals (interface version 19) ----
       A decal is a picture projected onto the surfaces in its box, the way the game paints blood and footprints.
       On a skinned figure it moves with the figure. DecalDefaults fills a TW3SE_Decal with working values; set
       position, direction, size and the diffuse texture, then SpawnDecals. `entities` NULL paints every surface in
       the box (world, objects and figures), else only the meshes of these entities (a mesh component in the list: only that mesh). SpawnDecals writes one id per
       decal to `outIds` (0 for one that failed, LastError says why) and returns how many were made. A decal with
       lifetime 0 stays until RemoveDecal or ClearDecals; loading a save or changing the world takes every decal away.
       The renderer keeps a budget of decals and drops the oldest past it. Main thread only. */
    void (*DecalDefaults)(TW3SE_Decal* decal);
    int (*SpawnDecals)(const TW3SE_Decal* decals, int count, void* const* entities, int entityCount, uint32_t* outIds);
    int (*RemoveDecal)(uint32_t id);
    int (*ClearDecals)(void* entity);  /* the decals spawned on this entity (NULL: all of yours); returns how many */
    int (*SetDecalColor)(uint32_t id, uint32_t color, uint32_t specularColor, float specularity, float specularScale,
                         float specularBase);
    int (*DecalCount)(void);
    /* How many decals the renderer keeps: perMesh decals on one mesh (the game: 2, up to 127), total decals in the
       world and chunks (the pieces a decal leaves on each mesh it touches). 0 leaves a value as it is. Returns 1 when
       every value was set. */
    int (*SetDecalBudget)(int perMesh, int total, int chunks);

    /* ---- Lua replies of any length (interface version 20) ----
       Inside a TW3SE_LuaFunction that returns 1: LuaReplyBytes makes Lua get these `size` bytes (zero bytes and all,
       any length) instead of `reply` - a file's data without writing it to disk first. Copied at once. Only during
       the call, on its thread; returns 0 elsewhere. */
    int (*LuaReplyBytes)(const void* data, size_t size);

    /* ---- Natives with strings and names (interface version 21) ----
       ReturnString writes a WitcherScript string (UTF-8, `length` bytes) into a native's result slot. A `name`
       parameter arrives through ReadParam as its 4-byte id; NameFromString gives the id of a text (0 for "" and
       "None"), GetCNameString the text of an id. Return a name by writing its id into the result slot. */
    void (*ReturnString)(void* result, const char* utf8, uint32_t length);
    uint32_t (*NameFromString)(const char* text);

    /* ---- Dear ImGui for plugins and .asi mods (interface version 21) ----
       Draw your own windows with TW3SE's ImGui (the same overlay, fonts, cursor and input handling as TW3SE's menu):
       compile Dear ImGui IMGUI_VERSION_NUM from the SDK (include\tw3se\imgui, its imconfig) into your DLL and use
       tw3se_imgui.h. RegisterOnImGui refuses another ImGui version or data layout (0, and why in tw3se.log).
       ImGuiAllocators gives TW3SE's ImGui allocator (ImGui::SetAllocatorFunctions), so memory crosses the DLLs.
       ImGuiOverlay(1): your windows show over the game while TW3SE's menu is closed (the game keeps the input the
       windows do not take). ImGuiCaptureInput(1): your menu is open - every key and mouse message goes to ImGui, the
       cursor shows and moves freely; 0 gives the input back. Main thread only. Each plugin's settings count for it. */
    int (*RegisterOnImGui)(TW3SE_ImGuiCallback callback, void* context, int imguiVersionNum, uint32_t ioSize,
                           uint32_t styleSize);
    void (*ImGuiAllocators)(void** allocFn, void** freeFn, void** userData);
    void (*ImGuiOverlay)(int on);
    void (*ImGuiCaptureInput)(int on);

    /* ---- Messages, the game's state, the controller (interface version 21) ----
       SendToChannel hands `message` to every listener of `channel`: plugins (RegisterOnMessage, channel NULL or "*"
       for all), Lua (w3.on_message) and scripts (TW3SE_Receive(channel) gives the next one). Listeners run on the
       main thread; from another thread the message goes out at the next tick.
       GameState: TW3SE_STATE_* bits, asked from the game on the main thread (0 elsewhere).
       PadRead: the first XInput controller. `buttons` bits 0-15 are XInput's, 16 the left and 17 the right trigger
       (pressed past 30 of 255); `axes` (6 floats, may be NULL): left stick x, y, right stick x, y (-1..1, dead zone
       applied), left and right trigger (0..1). Returns 0 without a controller. The game keeps its controller while a
       menu is open (it reads it through GameInput too, which TW3SE does not hold back yet). */
    void (*SendToChannel)(const char* channel, const char* message);  /* (not "SendMessage": a macro of windows.h) */
    int (*RegisterOnMessage)(const char* channel, TW3SE_MessageCallback callback, void* context);
    uint32_t (*GameState)(void);
    int (*PadRead)(uint32_t* buttons, float* axes);

    /* ---- Rendering (interface version 21) ----
       What a ReShade add-on needs, on the game's own D3D12 objects. Register in TW3SE_PluginLoad: the game creates
       its root signatures and most pipelines while it starts.
       RegisterOnPipeline: called for every pipeline the game creates, with its shaders and root signature.
       ShaderReplace: a shader of the game replaced by your bytecode (DXIL, same inputs and outputs) in every pipeline
       created after the call (tw3se\shaders\<id>.cso does the same from a file).
       RootSignatureAddParam: appends a D3D12_ROOT_PARAMETER1 (a table's ranges are copied) to the game's root
       signature with that id when the game creates it; returns a handle (0 on an error). The game's parameters keep
       their indices. RootParamIndex: the index of your parameter in a root signature, -1 when it has none.
       MarkPipeline: whenever the game sets this pipeline (or a new root signature while it is set) on a command list
       whose signature has plugin parameters, `callback` runs on the recording thread: bind your parameters there.
       NULL as callback unmarks. Command lists recorded before the first submission after your first call are not
       seen. */
    int (*RegisterOnPipeline)(TW3SE_PipelineCallback callback, void* context);
    int (*ShaderReplace)(uint64_t shaderId, const void* bytecode, size_t size);
    int (*RootSignatureAddParam)(uint64_t rootSignatureId, const void* d3d12RootParameter1);
    int (*RootParamIndex)(void* rootSignature, int handle);
    int (*MarkPipeline)(void* pipelineState, TW3SE_BindCallback callback, void* cookie, void* context);
    /* Once per frame, before the game's first submission to the queue it presents with: your command lists go to
       that queue first (ExecuteCommandLists inside the callback). The thread is the game's render thread. */
    int (*RegisterOnSubmit)(TW3SE_SubmitCallback callback, void* context);

    /* ---- Blend shapes (interface version 22) ----
       Lua's w3.deform for plugins (docs/manual/blend-shapes.md): a .twdf file of shapes on a mesh, moved on the
       graphics card by weights, before skinning, in every pass. `meshComponent` is a CMeshComponent (or a class
       derived from it); a relative `path` starts in the tw3se folder.
       DeformOwn gives the component a copy of its mesh, so a set on it moves this entity only (1 when done); the copy
       is on the graphics card a few ticks later, DeformLoaded turns 1 then. DeformRelease gives the shared mesh back
       (unload the copy's set first; release the copies before the game saves). DeformLoad returns the set's id, 0 on
       an error (LastError says why); `range` is the largest |weight| planned (0 counts as 1). These four: main thread
       only, the copies are the same as Lua's.
       DeformWeights writes `count` weights in file order (more than the set has are left out) and returns how many it
       took, 0 for no such set. Any thread, a pose callback too; the weights show in the next frame the game draws.
       DeformState: a TW3SE_DEFORM_* value. DeformUnload puts the original positions back and ends the set (1 when
       there was one). DeformShapeIndex: a shape's index by name, -1 if the set has none of that name. */
    int (*DeformOwn)(void* meshComponent);
    int (*DeformLoaded)(void* meshComponent);
    int (*DeformRelease)(void* meshComponent);
    int32_t (*DeformLoad)(void* meshComponent, const char* path, float range);
    int (*DeformWeights)(int32_t set, const float* weights, int count);
    int (*DeformState)(int32_t set);
    int (*DeformUnload)(int32_t set);
    int (*DeformShapeIndex)(int32_t set, const char* name);

    /* ---- The skinning hook (interface version 23) ----
       Capsules that push a mesh's skinned vertices out to their surface, in the game's own skinning shaders (every
       pass: shadows and motion vectors too). Needs `skin_hook = on` in tw3se.config and the patched shaders with
       tw3se\shaders\skinhook.txt (tools/skin_build.py, made for the game's build). SkinHookActive: 1 when it is on.
       SkinHookAdd gives a CMeshComponent's mesh a slot (1..8; 0 and LastError on an error; main thread). The slot
       belongs to the mesh: every entity with that mesh is pushed (DeformOwn first for one entity alone).
       SkinHookCapsules sets the slot's capsules from now on, any thread (the pose callback too): `count` of them,
       8 floats each: a.xyz, radius, b.xyz, strength (1 = out to the surface), in the mesh's model space, metres;
       at most 8. SkinHookState: TW3SE_DEFORM_NONE, _READY or _LOST (the game loaded the mesh anew: add it again).
       SkinHookRemove frees the slot.
       SkinHookWeights: one float a vertex of a chunk of the slot's mesh (`count` = the chunk's vertex count, as in
       w3.deform.dump's TWDB file), the push times it: 0 none, 1 all (a zone mask, stiffness). A chunk without weights
       is pushed fully; count 0 takes the chunk's weights away. From the next frame on; 0 and LastError on an error
       (no mesh in the slot, a chunk not in it, a count not the chunk's, more than 1M floats for all slots). */
    int (*SkinHookActive)(void);
    int (*SkinHookAdd)(void* meshComponent);
    int (*SkinHookCapsules)(int slot, const float* capsules, int count);
    int (*SkinHookState)(int slot);
    int (*SkinHookRemove)(int slot);
    int (*SkinHookWeights)(int slot, int chunk, const float* weights, int count);

    /* ---- One component's pose, dangle meshes (interface version 24) ----
       A watched figure takes along every entity hung on it, not only items: also a dangle entity pulled in by a
       CExternalProxyComponent. A mesh skinned to a dangle's own rig (CAnimDangleComponent with its own .w2rig)
       now reaches the pose callback: kind OTHER, `entity` the watched figure, `component` the CAnimDangleComponent,
       `parents[slot]` the slot's bone in that rig, `boneNames[slot]` its name (every bone of the rig is named),
       `local` the mesh's writable matrices as for any other mesh, `modelToWorld` the figure's.
       GetComponentPose reads the last evaluated pose of one component into `out`, in the model space it is skinned
       from, with that skeleton's names and parents (bones without an animation track too): for an animated
       component its own skeleton and pose buffer, modelToWorld of its entity; for a CAnimDangleComponent its rig and
       the dangle's buffer (in the model space of the figure it hangs on), `entity` that figure. Works for figures
       nobody watches. `kind` is MIMIC for a CMimicComponent, else OTHER. Read-only; returns 1 when written, 0 for
       an object that is neither or has no pose yet. */
    int (*GetComponentPose)(void* component, TW3SE_Pose* out);

    /* ---- The graphics rebuild (interface version 25) ----
       A graphics option applied in the options (upscaler, anti-aliasing, path tracing, ray tracing ...) makes the
       game rebuild its render resources at once and free thousands of its objects. `callback` runs on the main
       thread with TW3SE_REBUILD_BEFORE right before that: let go of every pointer into the game's render, skinning
       and mesh data you keep, and stop what reads them. With TW3SE_REBUILD_AFTER once the rebuild is done: look
       them up again in the game. One change can rebuild more than once; each
       BEFORE has its AFTER. Your blend shape sets end before it (TW3SE_DEFORM_LOST): load them again after.
       A callback that faults is dropped (logged once). Returns 1 when registered. */
    int (*RegisterOnRenderRebuild)(TW3SE_RenderRebuildCallback callback, void* context);

    /* ---- Live meshes (interface version 26) ----
       A mesh whose vertex positions and normals the plugin writes, before skinning, in every pass of the game.
       LiveMeshOpen gives the CMeshComponent a copy of its mesh (as DeformOwn) and returns the live mesh's id, 0 on an
       error (LastError says why). Its state is TW3SE_DEFORM_LOADING until the copy is on the graphics card and the
       game has drawn it, then _READY. LiveMeshClose gives the component the shared mesh back. These two: main thread
       only. Close the copies before the game saves.
       LiveMeshInfo: the vertex count of a chunk and the mesh's chunk count, 1 once the copy has loaded.
       LiveMeshWrite: `count` positions (and normals, NULL keeps the mesh's) of a chunk, 3 floats each, in the mesh's
       model space before skinning, metres; `count` is the chunk's vertex count (LiveMeshInfo). Any thread, a pose
       callback too; the newest write of a chunk shows in the next frame the game draws, older ones are dropped. A
       write while LOADING is kept until READY. Bone indices and weights stay the mesh's; tangents are turned to stay
       at a right angle to the new normal. 0 and LastError on an error.
       LiveMeshBounds: the box the positions are stored in (3 floats each, model space); positions outside it are
       clamped to it. Without it the mesh's own box holds. Any thread.
       LiveMeshState: a TW3SE_DEFORM_* value; LOST after a graphics rebuild (before TW3SE_REBUILD_BEFORE) or when the
       copy went (the figure destroyed, another mesh set): Close it, then Open again. */
    int32_t (*LiveMeshOpen)(void* meshComponent);
    int (*LiveMeshInfo)(int32_t id, int chunk, int* vertexCount, int* chunkCount);
    int (*LiveMeshWrite)(int32_t id, int chunk, const float* positions, const float* normals, int count);
    int (*LiveMeshBounds)(int32_t id, const float* min, const float* max);
    int (*LiveMeshState)(int32_t id);
    int (*LiveMeshClose)(int32_t id);

    /* TextureWrite: the pixels of a loaded texture (.xbm by depot path), from a frame soon after on, for every decal
       and material that shows it. `rgba8`: `mips` levels one after another, top level first, 4 bytes a pixel (red
       first); with fewer levels than the texture has, TW3SE makes the rest (box filter). `width` and `height` are
       the texture's. TW3SE encodes to the texture's format (BC3, RGBA8, BGRA8) on a thread of its own and returns at
       once; a newer write of a texture drops an older one still waiting. The texture has to be resident in full: a
       texture of a streamed group is resident at 32 x 32 only, cook it in GUIWithAlpha. Main thread; 0 and LastError
       for a path not loaded, another size or another format. */
    int (*TextureWrite)(const char* depotPath, const void* rgba8, int width, int height, int mips);

    /* DeformNormals: `mode` 1 makes the normals and tangents of a blend set (DeformLoad) or a live mesh
       (LiveMeshOpen) follow its moved triangles: each vertex's own normal and tangent are turned as the surface around
       it turned, so hard edges and baked normals stay. It takes the place of a file's normal deltas and of the normals
       given to LiveMeshWrite. Copies of a vertex along a UV seam turn as one. 0 = off (the file's or plugin's normals,
       or the mesh's own). Any thread; 0 and LastError for an unknown id or a mesh whose index buffer was not found. */
    int (*DeformNormals)(int32_t setOrLiveMesh, int mode);

    /* SkinHookCapsulesEx: as SkinHookCapsules, up to 32 capsules of 12 floats: a.xyz, radius, b.xyz, strength,
       falloff, mode, 0, 0 (model space, metres). mode >= 0 pushes out: a vertex's distance to the segment ab becomes
       the smooth maximum of it and the radius over `falloff` (0: out to the surface, as SkinHookCapsules; more: a
       soft bulge from `falloff` beyond the surface on). mode < 0 draws in: a vertex outside, within `falloff` of
       the surface, is drawn onto it (a strap). Times strength and the vertex's weight (SkinHookWeights). The
       meshes of one figure skin into the same model space: the same capsules given to two slots press one mesh
       against the other (a thigh's capsules on the belly). SkinHookCapsules takes 8 of them now too. Any thread. */
    int (*SkinHookCapsulesEx)(int slot, const float* capsules, int count);

    /* Delta Mush on a skinning hook slot: the mesh's skinned shape is smoothed (`iterations` steps over its triangles,
       8 is a good start; 0 = off) and its rest detail put back in the frame of the smoothed surface, so a joint that
       bends keeps its volume. One frame behind the skinning. The mesh is read back once when it is turned on (ready a
       few frames later). SkinHookMushMask: one float a vertex of a chunk, 0..1 how much of it the vertex takes (1
       unless given); count is the chunk's vertex count. Both: any thread, 0 on an error (LastError). */
    int (*SkinHookDeltaMush)(int slot, int iterations);
    int (*SkinHookMushMask)(int slot, int chunk, const float* mask, int count);

    /* SkinHookOverlay: a picture over the skin of a slot's mesh, `size` x `size` texels RGBA8 (red first; alpha = how
       much of it covers the skin), at the mesh's UV, mixed into the skin's colour before the light; the subsurface
       stays. For wetness, dirt, redness without decals. Up to 512; 0 takes it away. Needs the patched skin pixel
       shaders (tools/skin_build.py). Any thread; the picture shows from the next frame on. */
    int (*SkinHookOverlay)(int slot, const void* rgba8, int size);

    /* SkinHookOverlayMaterial: the second picture of a slot's overlay, the same texels and UV, RGBA8: R = wet (the
       game's own rain look on the skin: glossier, the colour deeper), G = matte (rougher, for dried spots),
       B = height (its slope tilts the normal, so the edge of a thick drop catches the light), A unused. NULL with
       a size: the plane clear. A size other than the colour's clears the colour (and the other way round). Needs
       the shaders of skin_build.py (skinhook.txt `overlay 2`); with older ones it does nothing. */
    int (*SkinHookOverlayMaterial)(int slot, const void* rgba8, int size);

    /* ---- interface 27 ---- */

    /* EngineAddress: an engine function by name, NULL if there is none (LastError says why). The name is one of
       GetAddress's, a class's vtable slot ("CEntity::vf140", "CEntity::vf0@10" for the vtable at offset 0x10 of the
       object), found by the class's name in the exe's RTTI, or an offset in the exe ("0x2263570"). Any thread. */
    void* (*EngineAddress)(const char* name);

    /* EngineVtable: a class's vtable from the exe's RTTI (offset 0 = the main one), NULL if the exe has none. */
    void* (*EngineVtable)(const char* className, uint32_t offset);

    /* ---- interface 28 ---- */

    /* SkinHookDqs: dual quaternion skinning for a slot's mesh (on 1, off 0). The patched shaders skin it anew from the
       game's bones and the mesh's weights; joints keep their volume where linear skinning folds them in. Delta Mush
       and the capsules work on top of it. Needs the shaders of skin_build.py (skinhook.txt `dqs 1`). Any thread;
       from the next frame on. */
    int (*SkinHookDqs)(int slot, int on);

    /* ---- interface 29 ---- */

    /* SkinHookOverlayRegion: a plane (0 colour, 1 material) of a slot's overlay at a place the caller lays out: `base`
       texels from the start of the buffer all pictures share (2097152 texels a plane), `size` x `size`, at the mesh's
       UV. Several pictures side by side (a body, a head, a piece with a UV of its own); the caller keeps them apart.
       Any slot, an alias of SkinHookChunkSlot too. NULL with a size: the plane clear; size 0: the overlay off. */
    int (*SkinHookOverlayRegion)(int slot, int plane, const void* rgba8, int size, int base);

    /* SkinHookChunkSlot: a chunk of a slot's mesh drawn with a slot of its own - the same skinning (capsules, DQS),
       its own overlay (SkinHookOverlayRegion), for a chunk whose UV overlaps the others'. Answers the alias slot (the
       same one again for the same chunk), 0 on an error. chunk -1: the slot's aliases gone. Any thread. */
    int (*SkinHookChunkSlot)(int slot, int chunk);
} TW3SE_Interface;

/* Whether the running extender has `field` (a function added after interface 25):
   if (TW3SE_HAS(api, SkinHookOverlay)) api->SkinHookOverlay(...); */
#define TW3SE_HAS(api, field) ((api)->interfaceSize >= offsetof(TW3SE_Interface, field) + sizeof((api)->field))

#endif /* TW3SE_H */
