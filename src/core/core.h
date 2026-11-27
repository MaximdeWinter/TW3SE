// TW3SE core - shared declarations between the core's parts.
#pragma once
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>
#include "tw3se/tw3se.h"

struct ID3D12Device;
struct D3D12_GRAPHICS_PIPELINE_STATE_DESC;

namespace tw3se {

// --- log.cpp: <game>\bin\x64_dx12\tw3se\tw3se.log
void LogOpen(const std::wstring& path);
void Log(const char* fmt, ...);
void LogV(const char* prefix, const char* fmt, va_list ap);

// --- posehook.cpp: pose/mimic hooks (plugin interface v13)
void InstallPoseHook();  // engine combine hook; off unless config posehook=diag|on
void InstallHotReload();  // `reloadscripts force`: WitcherScript live reload in the game (hotreload.cpp)
// Dev self-test (w3.pose_selftest): when the combine fill's output buffer equals `buf` (a watched entity's
// *(component+0xe00)), the hook twists bone `bone`'s local matrix before the engine computes model-space -
// proves the override path end to end without a plugin. buf=0 disables it.
void PoseSelftestTarget(void* comp, int bone);  // comp = component (B-0xddc) to twist, or (void*)1 = broad
void PoseSelftestSig(void* comp, int bone, int sigIdx, int sigVal);  // twist only in the submesh PAR[sigIdx]==sigVal
void PoseWave(void* comp, int bone, int sigIdx, int sigVal, int on);  // live per-frame oscillation of a bone
void PoseRestore(void* comp, int bone, int sigIdx, int sigVal);       // write a bone's saved original back
void PoseMeshList(void* comp, int on);           // enumerate a component's distinct combines (meshes) to the log
void PoseMapTarget(void* comp);                  // arm a one-shot A+0x18 remap dump for this component
void PoseRegisterCallback(TW3SE_PoseCallback cb, void* ctx);
int PoseWatch(void* entity, int on);
int PoseWatchBy(void* entity, int on, int by);  // by: 1 the plugin API and w3.pose_watch, 2 w3.pose (ends by itself)
int PoseGet(void* entity, int kind, TW3SE_Pose* out);
int PoseGetComponent(void* component, TW3SE_Pose* out);  // interface 24: one animated component's own pose
std::string PoseRestBones(void* skeleton, bool on, const std::vector<std::string>* names = nullptr);  // a dangle rig whose unmapped bones get the rest pose
std::string PoseRestStats();
// w3.view.rect (overlay.cpp): the game's picture drawn into a rectangle of the screen, the UI full screen around it
void OverlayViewRect(bool on, float x, float y, float w, float h);
bool OverlayViewGet(float* x, float* y, float* w, float* h, float* screenW, float* screenH);
void PoseRestForget(void* skeleton);  // by address, for a rig object that is gone
// w3.anim.sample / w3.anim.pose: an animation of the figure's sets at second t, as a pose or laid on it every frame
int PoseSample(void* component, const char* name, float t, TW3SE_Pose* out, float* duration, std::string* err);
std::string PoseLayersSet(void* component, const std::vector<std::tuple<std::string, float, float>>& layers);
// w3.anim.create: an engine animation for the component's rig, frames x bones x 10 floats; "" on success
std::string AnimCreate(void* component, const char* name, float fps, const std::vector<std::string>& boneNames,
                       const float* data, uint32_t frames, uint32_t* mapped);
std::string AnimPublish(void* set, void* component, const char* name);  // into the engine's lookup by name
std::string AnimRecordStart(void* component, float maxSeconds);         // w3.anim.record
std::string AnimBoneNames(void* component, std::vector<std::string>* names);  // the rig's bone names
// w3.anim.shape: per bone {scale xyz, offset xyz, rot xyzw, length, inherit} on the figure every frame; empty: off
std::string AnimShapeSet(void* component, const std::vector<std::pair<std::string, std::array<float, 12>>>& bones);
std::string AnimRecordStop(void* component, float fps, std::vector<std::string>* bones, std::vector<float>* data);
// w3.anim.reach: an arm's two-bone IK to a point in the world, every frame; hand 'l' / 'r', any other or weight 0: off
std::string AnimReachSet(void* component, char hand, const float* target, float weight, float ahead, float out);
std::string PoseLayerStats();
void MimicRegisterCallback(TW3SE_MimicCallback cb, void* ctx);
int MimicWatch(void* entity, int on);
int MimicWatchBy(void* entity, int on, int by);  // by: 1 the plugin API, 4 w3.face.set, 8 w3.face.tracks
// w3.face: weights by track name written into every frame of the face (in place of the game's, or added on top)
int FaceSet(void* entity, const std::vector<std::pair<std::string, float>>& weights, bool add, const std::string& owner);
int FaceClear(void* entity);
int FaceClearOwner(const std::string& owner);
std::vector<std::string> FaceTracks(void* entity);
void FaceExpire();  // each tick: a watch only w3.face.tracks asked for ends 5 s after the last call

// --- paths (main.cpp)
const std::wstring& Dir();          // <game>\bin\x64_dx12\tw3se
uintptr_t ImageBase();
uint32_t GameBuild();               // PE timestamp of witcher3.exe

// --- addresses.cpp: named engine functions and globals (generated table, signatures for other builds)
bool ResolveAddresses();            // false: something essential is missing - extender stays passive
bool IsRedkitEditor();              // running inside the REDkit editor.exe (features install per main.cpp's table)
// Memory::AllocDefault with the right ABI for this build (retail and the REDkit editor differ). Null on failure.
// memClass is used by the editor build only (0x18 = what the engine passes for its CFunctions).
void* EngineAlloc(size_t size, uint32_t align, uint16_t memClass = 0x18);
uintptr_t Address(const char* name);  // 0 if unknown
// enginecall.cpp: an engine function by an address-list name, "Class::vfN[@offset]" or "0x<rva>"; 0 and *err if none
uintptr_t EngineResolve(const std::string& name, std::string* err);
uintptr_t EngineVtable(const std::string& cls, uint32_t offset);  // a class's vtable from the exe's RTTI, 0 if none
void ExeText(const uint8_t** begin, size_t* size);  // witcher3.exe's code section in memory (for own scans)
uint32_t FieldOr(const char* name, uint32_t retail);  // a FIELD's value from the table, else the retail offset
std::vector<uintptr_t> SignatureScan(const char* sig, size_t max);  // every match in witcher3.exe's code
// patch.cpp: byte patches (w3.patch); a place is an integer (below 0x10000000 an offset), an engine name or a signature
uintptr_t PatchPlace(const std::string* name, uintptr_t value, std::string* err);
int PatchBytes(uintptr_t at, const std::vector<uint8_t>& bytes, const std::string& owner, std::string* err);
bool PatchRestore(int id);
int PatchRestoreOwner(const std::string& owner);
template <class T> T Fn(const char* name) { return reinterpret_cast<T>(Address(name)); }

// --- hooks (main.cpp, MinHook)
bool Hook(void* target, void* detour, void** original);
// Resolve a named engine address and hook it; logs "hook: <name> not hooked" and leaves *orig null on failure.
void HookNamed(const char* name, void* detour, void** orig);

// --- mainthread.cpp: work for the main thread, run at the end of CCommonGame::Tick (REDkit editor: its engine tick)
bool InstallMainThread();
void RunOnMainThread(TW3SE_Task task, void* context);
bool OnMainThread();
bool MainThreadTicking();  // the tick hook ran in the last 2 s (REDkit editor: only while Play runs)
// Runs `fn` on the main thread and waits for it (from another thread). False on timeout.
bool RunAndWait(TW3SE_Task task, void* context, uint32_t timeoutMs);
// Registers a per-tick callback (plugin OnTick), fired once per CCommonGame::Tick on the main thread.
void RegisterTick(TW3SE_TickCallback callback, void* context);

// --- console.cpp: developer console on \\.\pipe\tw3se (one line in, one reply out)
void StartConsole();
bool RegisterCommand(const char* name, TW3SE_Command handler, const char* help);

// --- link.cpp: what an external tool drives the game through (exec commands, TW3SE_Emit lines)
void InstallLink();
void QueueLinkNatives();  // only the natives (REDkit editor while the link's addresses are missing)
std::string EventsReply(const char* args);  // the `events` reply, answered on the pipe thread
void LinkEmit(const char* line);             // one `events` line from the core (as TW3SE_Emit from scripts)
std::string LinkExec(const char* cmds);      // the pipe's `exec` for Lua (main thread): "ok <n>" / "error ..."
uint64_t LinkLines(uint64_t since, std::vector<std::string>* out, uint64_t* lost);  // `events` for Lua
uint64_t LinkSeq();                          // the newest line's number

// --- config.cpp: tw3se\tw3se.config (key = value)
void LoadConfig();
std::string ConfigGet(const char* key, const char* def);
bool ConfigBool(const char* key, bool def);

// --- content.cpp: mount DLC folders into the running game
bool MountDLC(const char* dlcFolder, char* reply, uint32_t replySize);
// mount + register a DLC with the DLC manager (the live-load step; a save reload then activates its quests)
bool LiveLoadDLC(const char* dlcFolder, char* reply, uint32_t replySize);
void RegisterContentCommands();
void TickContent();  // run once per tick (deferred post-mount DLC scan)
int DLCScansDone();    // post-mount registration scans run so far (w3.dlc_scans)
bool DLCScanPending(); // one is still to come

// --- scripting.cpp: register TW3SE's own (and plugins') WitcherScript natives
void InstallScripting();
void TickVars();   // once per tick: the variable store to tw3se\vars.txt, at most once a second, when changed
void FlushVars();  // at the game's exit: what TickVars has not written yet
bool VarGet(const std::string& key, std::string& value);   // the store (tw3se\vars.txt) for C++ and Lua
bool VarSet(const std::string& key, const std::string* value);  // nullptr removes the key; false over the limits
void QueueNative(const char* name, void (*native)(void*, void*, void*), int params);  // queue a native
bool RegisterNativeAnytime(const char* name, void (*native)(void*, void*, void*), int params);  // after start: at once
// a native method of a class (docs/engine/scripting.md section 15); OurMethod("Class.Name"): its CFunction
bool RegisterNativeMethodAnytime(const char* cls, const char* name, void (*native)(void*, void*, void*));
void* OurMethod(const std::string& full);
// an enum of a mod (section 16 of docs/engine/scripting.md): scripts use it and its values by name
bool RegisterEnumAnytime(const std::string& name, const std::vector<std::pair<std::string, int32_t>>& values);
bool NativesRegistered();  // the engine's native registration point has passed (the engine is up)
void NoteModNative(const char* name);  // a plugin's or a Lua mod's native: safe mode stands in for it (lua.cpp)
std::vector<std::string> ModNativesLastTime();  // those of the last start without safe mode (out/natives_of_mods.txt)
bool NativeKnown(const std::string& name);  // a native of that name is queued or registered (TW3SE's, plugins', Lua's)
void* OurNative(const char* name);  // a TW3SE native's CFunction (TW3SE's own list, then the engine's map)
// Call a globally-registered WitcherScript native from C++ with string args; result receives int/bool.
bool CallGlobalFunc(const char* name, const char* const* stringArgs, int argCount, void* self, void* result);
// Call a method-native on an object (class function list) with string args; result receives int/bool/handle.
bool CallMethodFunc(void* object, const char* method, const char* const* stringArgs, int argCount, void* result);
// Typed-argument variants: argspec is "i:N f:N s:text b:1" (int/float/string/bool-true).
bool CallFunctionV(const char* name, const char* argspec, void* self, void* result);
bool CallMethodV(void* object, const char* method, const char* argspec, void* result);
// Calls a native CFunction with argument bytecode that ends in 0x20: a global through GScriptNativeTable, a method
// through GScriptMethodTable. False when it has no handler or the native faulted (*faulted, then logged).
// localsBase: ScriptFrame+0x20 (FINDINGS_OUTPARAM3.md) - the base an embedded CProperty* (opcode 0x0f,
// out/by-reference arguments) resolves its offset against. Null is fine unless the bytecode uses 0x0f.
// applyThisAdjust: for a method, add the method table's this-adjustment (entry byte 8) to `self` before the handler
// (the dispatcher does this; the state-machine methods of IScriptable need +16). A hook that already received an
// adjusted context from the dispatcher passes false so it is not applied twice.
bool InvokeNative(void* fn, void* self, const uint8_t* code, void* result, bool* faulted, void* localsBase = nullptr,
                  bool applyThisAdjust = true);
// The script thread the NEXT InvokeNative frame carries at +0x40 (consumed by that call; main thread only).
extern void* g_invokeThread;
// The parameter block the NEXT InvokeNative/InvokeScript frame carries at +0x28, read by opcode 0x10 (structs with
// strings, arrays or objects handed by value; consumed by that call; main thread only).
extern void* g_invokeParams;
// Runs a WitcherScript function's original body through the VM dispatcher (for a hook's orig()), via a local
// native-bit-cleared copy of its CFunction so the shared, hook-flagged original stays untouched. Needs Address("Dispatch").
bool InvokeScript(void* fn, void* self, const uint8_t* code, void* result, bool* faulted, void* localsBase = nullptr);
// Runs a hooked script function's original body with the caller's real frame (reentrant/off-thread fallback):
// momentarily clears the native bit on the real CFunction so the dispatcher interprets it, then restores it.
bool RunScriptOriginal(void* fn, void* ctx, void* frame, void* result);
// Script events: the engine's own C++ calls into script functions reach Lua hooks (scripting.cpp, lua.cpp)
bool EnsureScriptCallHook();
bool ScriptLinesOn();  // hotreload.cpp: this session's scripts were compiled with statement lines (script_lines)
bool IsHookedScriptFunction(void* fn);
uint32_t InternName(const char* name);  // the CName id of `name` (interned), 0 when the pool is unavailable
bool CallRawHandler(void* handler, void* ctx, void* frame, void* result);  // call a native handler, SEH-guarded (hooks)
void* FindGlobalFn(const char* name);  // a global function (CFunction*) by name, or null
uint32_t ScriptGlobalOffsetOf(const char* name);  // GScriptGlobals offset of theGame/thePlayer/..., 0 if unknown
bool WriteStringField(void* field, const char* text, size_t len);  // overwrite a String field (frees the old one)
void SetScriptResultString(void* result, const char* text, size_t len);  // write a fresh String result slot
void ReadScriptParam(void* frame, void* out);  // read one parameter inside a native handler
void EndScriptParams(void* frame);  // skip the end-of-arguments marker (once, after reading all parameters)
void* ScriptParamCursor(void* frame);                 // where the argument cursor stands
void SetScriptParamCursor(void* frame, void* cursor);  // put it back (a native that faulted halfway)

// --- properties.cpp: generic RTTI property access (6a, docs/engine/scripting.md section 8)
void* FindProperty(void* cclass, const char* name);  // CProperty* (incl. inherited) or null
void* PropertyPtr(void* object, void* cproperty);     // address of the field inside the object, or null
void* GetObjectClass(void* object);                   // an object's CClass* (virtual GetClass, slot 1)
void* FindClassByName(const char* name);              // a CClass* by class name (CRTTISystem::FindType)
uint32_t CountProperties(void* cclass);               // number of properties incl. inherited (diagnostic)
const char* GetCNameString(uint32_t id);              // the string behind a CName id (id -> name)
void* GetScriptGlobal(uint32_t fieldOffset);          // a predefined WS global by its GScriptGlobals offset
void* GetThePlayer();                                  // the current player object, or null (menu/no spawn)
void* GetTheGame();                                    // the game object
void* ResolveHandle(void* handle);                     // real object at handle+0x08 (object/handle returns), or null
bool EntityEffectNames(void* ent, char* out, size_t size);  // its template's effects, "fire,smoke" (scripting.cpp)

// --- catalog.cpp: every registered RTTI type and function -> tw3se\catalog\rtti.json (`catalog`), and the type
// helpers built on the same layout (docs/engine/scripting.md section 11)
void InstallCatalog();
bool SafeRead(const void* src, void* dst, size_t n);  // memcpy that turns an access violation into false
bool MemReadable(const void* p, size_t n);  // committed, readable, no guard page (VirtualQuery, cached per thread)
int RttiKind(void* type);          // 0 simple, 1 enum, 2 class, 3 array, 6 pointer, 7 handle, 11 fundamental ...; -1
uint32_t RttiSize(void* type);     // size in bytes, 0 when unknown
std::string RttiName(void* type);  // "Int32", "handle:CEntity", "array:2,0,CName" ...; "" when unknown
void* RttiFindType(const char* name);  // any registered type by name, or null
bool RttiEnumName(void* enumType, int32_t value, std::string& out);   // value -> value name
bool RttiEnumValue(void* enumType, const char* name, int32_t* out);   // value name -> value
template <class F> void RttiForEachType(F visit);  // visit(const char* name, void* type) for every registered type
void RttiTypes(void (*visit)(const char* name, void* type, void* ctx), void* ctx);
template <class F> void RttiForEachType(F visit) {
    RttiTypes([](const char* n, void* t, void* c) { (*static_cast<F*>(c))(n, t); }, &visit);
}
void RttiFunctions(void (*visit)(void* fn, void* ctx), void* ctx);  // every global function and class method
void RttiEnumNodes(void* enumType, void (*visit)(const char* name, int32_t value, void* ctx), void* ctx);
template <class F> void RttiEnumValues(void* enumType, F visit) {  // visit(const char* name, int32_t value)
    RttiEnumNodes(enumType, [](const char* n, int32_t v, void* c) { (*static_cast<F*>(c))(n, v); }, &visit);
}

// --- lua.cpp: Lua on top of the RTTI (`lua`, `luafile`, tw3se\lua\autorun)
void InstallLua();
void QueueLuaNatives();  // only the natives; TW3SE_Lua then answers that Lua is off
void LuaScriptsRecompiling();  // the scripts are being compiled again: pointers Lua borrowed from functions are stale
void LuaScriptsReloaded();     // after a reload pass: the Lua hooks go back on the new functions
void LuaGameLoaded();          // lifecycle.cpp: a game started or a save loaded (w3.saved tables fill on the next tick)

// --- plugins.cpp
void LoadPlugins();
void WnbCompatEarly();  // wnb.cpp, from DllMain
void LoadWnbCompat();   // wnb.cpp: stand in for Witcher Native Bridge when it is not installed
bool WnbCompatActive();
void LoadAsiPlugins();
void CheckAsiLoaders();  // asi.cpp: two Ultimate ASI Loaders beside the game (every .asi twice)  // asi.cpp: the .asi mods the ASI loader in dinput8.dll used to load
const TW3SE_Interface* Interface();

// --- overlay.cpp: D3D12 present hook (overlay foundation)
bool InstallOverlay(bool needed = false);  // true: the present hook is in (the overlay can draw)
void OverlayWhyNoDraw(char* out, size_t size);  // presents seen, overlay calls, where the overlay stopped (for the log)

// --- ui.cpp: the in-game UI (Dear ImGui): console, inspector, w3.ui windows; opens with console_key (F2)
void InstallUi();

// --- watchdog.cpp: what is going on when the game hangs (tw3se\out\hang_*.txt, `!status` on the pipe)
void InstallWatchdog();
void Crumb(const char* what, uintptr_t a = 0, uintptr_t b = 0, uintptr_t c = 0);  // `what`: a string literal
struct DoingScope {  // what this thread does inside TW3SE until the scope ends (`what`: a string literal)
    DoingScope(const char* what, uintptr_t arg = 0);
    ~DoingScope();
    DoingScope(const DoingScope&) = delete;
    DoingScope& operator=(const DoingScope&) = delete;
    void* slot_ = nullptr;
    const char* prevWhat_ = nullptr;
    uintptr_t prevArg_ = 0;
};
std::string WatchdogCommand(const std::string& line);  // `!status` ... on the pipe's thread

// --- scriptstack.cpp: the WitcherScript call stack with file and line
void InstallScriptStack();
struct ScriptStackFrameInfo {
    void* fn;
    std::string name;  // "CR4Player.OnSpawned", a global function without a class
    std::string file;  // "game\player\r4player.ws", "" for a native
    uint32_t line;     // the statement running in it (for a caller: the call), 0 when unknown
    void* frame;       // its frame (locals at +0x20), null for a native
    void* ctx;         // the object it runs on, null for a global function
};
int ScriptStackRead(ScriptStackFrameInfo* out, int max);  // this thread's, innermost first
std::string ScriptStackText(bool mainThread);              // as lines for a log or report
void ScriptStackOnFault(EXCEPTION_POINTERS* ep);           // a fault outside a guard: the stack to the log (3 times)
void InstallQuarantine();                                  // config quarantine = hold | poison: freed memory held back
void InstallAttachFix();                                 // config attach_fix (on): attachment lists changed by one thread at a time
void InstallDred();                                        // config dred = on: D3D12 DRED before the game makes its device
void DredOnPresent(void* swapChain, long hr);              // after each present: a device gone writes out\dred.txt
void DredOnFault();                                        // a fault: the device gone writes out\dred.txt
void QuarantineRebuild(int phase);  // the graphics rebuild: 0 before (window opens), 1 after (closes later)
void QuarantineOnFault(EXCEPTION_POINTERS* ep);            // a fault: the held block an address points into, its free
void* TickHookAddress();                                   // pass-through hooks on nearly every stack (quarantine)
void* DispatchHookAddress();
std::string ScriptFunctionFile(void* fn);                  // a CFunction's script file, "" for a native
std::string ScriptFunctionName(void* fn);                  // "Class.Name" or "Name"
// a latent script function's new frame, right after Dispatch pushed it (w3.watch_latent); null turns it off
void SetLatentStartCallback(void (*cb)(void* fn, void* ctx, void* frame));
// Watched lines (F4): the callback runs before a statement whose line number is watched (any file: it checks)
using ScriptLineFn = void (*)(void* fn, void* ctx, void* frame, uint32_t line);
void ScriptLineCallback(ScriptLineFn fn);
void ScriptLineWatch(uint32_t line, bool on);
// Stepping (the debugger): the next main-thread statement calls the step callback once. Mode 1 any statement, 2 one
// at this thread's call depth or above (step over), 3 one above it (step out), 0 off.
void ScriptStepCallback(ScriptLineFn fn);
void ScriptStep(int mode);
// --- debugger.cpp: the script debugger's connection (one client on 127.0.0.1, lines both ways)
bool DebugListen(int port);              // starts the server once; false if the port cannot be had
bool DebugConnected();
bool DebugRecv(std::string& line, int timeoutMs);
void DebugSend(const std::string& line);  // one line (a newline is added)
void NoteMainThreadAlive();               // mainthread.cpp: a halt in the debugger is no hang for the watchdog

// --- reload.cpp: game files with new contents while the game runs
void* FindDiskFile(const char* path);                                   // the depot's CDiskFile of a path, or null
std::string SetFileContents(const char* path, std::vector<uint8_t> bytes, bool drop);  // "" or the reason
std::string ClearFileContents(const char* path, bool drop);                      // the game's own contents again
std::string ReloadLooseFile(const char* path);                          // tw3se\files\<path> read again
std::string FileAlias(const char* src, const char* dst);  // the bundled file under a second path too; "" or the reason
std::string NewDiskFile(const char* path);               // a new file in the depot (contents: SetFileContents)

// --- error reports: console.cpp builds the report, lua.cpp keeps the last Lua errors with their tracebacks
std::string BuildReport(bool* written = nullptr);  // also written to tw3se\out\report.txt
std::string LuaRecentErrors();                     // newest first, with mod and time
std::string ReadTextFile(const std::wstring& path);  // console.cpp: a whole file ("" if missing), readable while written
std::string FolderNames(const std::wstring& dir);    // console.cpp: the folders in dir, comma-separated, or "(none)"
std::string ModioText();  // console.cpp: the mod.io and Steam Workshop mods and the game's files about them, for reports

// --- modfolders.cpp: one mod, one folder - tw3se\mods\<name>\ or <game>\Mods\<modName>\tw3se\ (init.lua, natives\,
// files\, its modules); sorted, tw3se\mods first, the mods switched off in mods.settings left out
struct ModFolder {
    std::string name;   // the folder's name (UTF-8): <name> or <modName>
    std::wstring dir;   // the mod's TW3SE folder
    bool inGameMods;    // in <game>\Mods\<modName>\tw3se
};
std::vector<ModFolder> ModFolders();
bool GameModSwitchedOff(const std::wstring& name);  // <game>\Mods\<name> is switched off in mods.settings

// --- scriptstack.cpp: the WitcherScript profiler (scriptprof, w3.script_profile)
struct ScriptProfRow {
    std::string fn, file, mod;  // Class.Function, its script file ("" for a native), the mod of that file
    uint64_t calls = 0;
    double wholeMs = 0, ownMs = 0;
};
void ScriptProfileSet(bool on, bool reset);
bool ScriptProfileOn();
std::vector<ScriptProfRow> ScriptProfile(double* seconds);  // by own time, the most first (200 at most)

// --- safe mode (main.cpp): Shift held while the game starts, or safe_mode = on: no Lua mods, no plugins
bool SafeMode();
bool NoPlugins();   // safe mode, or safe_mode = plugins
bool NoLuaMods();   // safe mode, or safe_mode = lua
bool PluginsLeftOut();  // plugins_off named a plugin that was there: the natives list stays as it was
bool LuaModsLeftOut();  // lua_mods_off named a Lua mod that was there: the same
void StubModNatives(const std::vector<std::wstring>& natDirs, bool pluginsOut);  // lua.cpp
void ShowNotice(const std::string& text);  // lua.cpp: a line over the game for 8 s
const char* CurrentLuaMod();               // lua.cpp: the Lua mod running right now ("" for none)

// --- lockeys.cpp: GetLocStringByKeyExt answers TW3SE's own localisation keys in C (the Lua mods' option pages)
bool LocKeySet(const char* key, const char* text);  // false when the handler could not go on

// --- crashreport.cpp: a session that did not end normally leaves tw3se\out\crash_report.txt at the next start
void FocusStart();  // focus.cpp: the game window gets the keyboard at the start (focus_at_start)
void TakeLastSession();   // before LogOpen: the last session's marker and log, if it did not end normally
void WriteCrashReport();  // after LoadConfig: crash_report.txt from them (nothing after a normal end)
void StartSession();      // the marker tw3se\out\session.txt, once diag.cpp sees the process exit
void SessionExit(long code, bool terminate);  // diag.cpp: code 0 deletes the marker, anything else goes into it
void SessionFault(EXCEPTION_POINTERS* ep);    // editorprobe.cpp: a fault the game's crash handler gets (the first 3)
void SessionNotesToLog();                     // diag.cpp: at an end that was not normal, the marker's notes into tw3se.log
bool GuardableFault(DWORD code);              // editorprobe.cpp: an exception a TW3SE guard may take (not a stack overflow)
std::string AddressPlace(uintptr_t a);        // scripting.cpp: "witcher3.exe+0x1234 (name)" for a log line
int RttiSlot(void* type, int slot);           // catalog.cpp: an IRTTIType slot (5.01's number) in this build, -1 unknown
void SessionNote(const char* what);           // a failure that ends with code 0 (hotreload.cpp: scripts that did not compile)
void SessionNoteFor(const char* what, unsigned seconds);  // the same, kept only if the game ends within `seconds`

// --- net.cpp: HTTP, TCP and UDP from Lua (J4); every result comes back through NetPoll on the main thread
struct NetEvent {
    int id = 0;
    std::string kind;  // "http", "connect", "data", "close", "udp"
    int status = 0;
    std::string body, headers, error, host;
    int port = 0;
};
int NetHttp(const std::string& method, const std::string& url, const std::string& headers, const std::string& body,
            int timeoutMs);
int NetConnect(const std::string& host, int port);
int NetUdp(int port, std::string& err);
std::string NetSend(int id, const std::string& data, const char* host, int port);  // host: UDP
void NetClose(int id);
std::vector<NetEvent> NetPoll();

// --- sector.cpp: the merged geometry of a layer (CSectorData): buildings, walls, rocks
struct SectorObject {
    uint8_t type, flags;  // type 1 mesh, 2 collision, 3 decal
    uint16_t radius;
    float pos[3];
};
int SectorObjects(void* sector, std::vector<SectorObject>* out);  // the count (-1: not a sector block)
struct SectorMesh {
    uint32_t index, resource, group;  // the object's index (from 0), its resource's, its group (world block)
    float matrix[12];          // its axes as rows, then its position
    float box[6];              // the resource's box in the mesh's own space: min x, y, z, max x, y, z
    float min[3], max[3];      // its box in the world
    uint64_t hash;             // FNV-1a 64 over the resource's depot path
    bool collision;            // a collision object follows it
};
int SectorMeshes(void* sector, std::vector<SectorMesh>* out, const float* center = nullptr,
                 float radius = 0, int64_t only = -1);  // the meshes (-1: not a sector block); only: one index
void* WorldSectorData(void* world);  // the world's CSectorDataMerged, or null
std::string MeshPathOfHash(uint64_t hash);  // a mesh's depot path by its FNV-1a 64 hash, "" when unknown
bool SectorSetPos(void* sector, uint32_t index, const float* pos);
int SectorProxies(void* world, const uint32_t* ids, size_t n, void** out);  // render proxies of streamed-in objects
int SectorUnstream(void* world, uint32_t id, bool on);  // a merged mesh out of the world at once (-1: no streamer)
int SectorUnstreamed(void* world, uint32_t id);         // switched off by SectorUnstream: 1, 0, -1
// --- highlight.cpp: the outlines of the Witcher senses on any render proxy
bool ProxyChannels(void* proxy, uint8_t set, uint8_t clear, uint8_t* before);  // false: not a render proxy
bool FocusGroupColor(int group, float* rgba, const float* set);  // group 0..3 (premultiplied colour); set may be null
uint8_t FocusExtraColor(int slot, const float* rgba, uint8_t bit = 0);  // colour 0 or 1 (null: off; bit: another) -> its bit
std::string FocusProbe();  // 24.11.: the pipelines of the focus effect's quads (w3._focus_probe)
uint64_t FocusCensus(int on, uint64_t out[9]);  // the stencil bits of the frame's pixels (w3._focus_census)
// shaders.cpp tells highlight.cpp of each graphics pipeline the game creates (the focus mask's is copied)
void HighlightNotePipeline(::ID3D12Device* dev, const ::D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, uint64_t ps);
void InstallHighlight();

// --- configvars.cpp: the engine's configuration variables (Rendering/UseUmbraOcclusion ...)
bool EngineConfig(const char* group, const char* name, const char* value, std::string& out, std::string& err);
std::vector<std::string> EngineConfigList(const char* filter);  // "group/name\ttype", sorted

// --- physx.cpp: the PhysX scene under the game's physics
std::string ObjectTypeName(void* obj);                         // MSVC RTTI name (".?AVNpScene@physx@@"), "" if none
void* ObjectFieldOfType(void* obj, size_t span, const char* type);  // first pointer field to an object of that type
// --- terrain.cpp: a colour layer over the terrain (CRenderCommand_SetTerrainCustomOverlay); rgba null clears it
std::string TerrainOverlay(void* clipMap, uint32_t width, uint32_t height, const uint8_t* rgba);
std::string TerrainProbe(void* clip, float wx, float wy, int spacing, float* height, uint32_t* raw);  // terrain.cpp
std::string TerrainRaise(void* clip, float wx, float wy, float radius, float delta, int* texels, int* pieces);
// scenectl.cpp: a running scene's time, pause and seek (CStoryScenePlayer*)
std::string SceneTime(void* player, float* t, float* length, void** section);
std::string ScenePause(void* player, bool on);
std::string SceneSeek(void* player, float t);
struct SceneElement { int index; float start, duration, local; bool running; };
std::string SceneElements(void* player, std::vector<SceneElement>* out, int* current);
void* VtableOfType(const char* typeName);  // a class's vtable by its RTTI name (".?AVCRenderCommand_X@@"), or null
// blendshape.cpp: a reference of TW3SE's on a render object (a render mesh, a render texture) given back the engine's
// way, on the render thread (CRenderCommand_ReleaseRenderObjectOnRenderThread); false when that is not there (then
// the caller's own release is the only way left)
bool ReleaseOnRenderThread(void* renderObject);
// --- decal.cpp: dynamic decals (the renderer's CRenderDynamicDecal, docs/FINDINGS_DECAL.md)
struct DecalDesc {
    float pos[3], dir[3], up[3];      // centre, projection direction, up of the picture in its plane
    float width, height, depth;       // metres; depth along dir, half in front of pos and half behind
    void* diffuse;                    // CBitmapTexture (alpha = shape)
    void* normal;                     // CBitmapTexture or null (the scene's normals)
    uint32_t color, specularColor;    // RGBA bytes, red first
    float specularity, specularScale, specularBase;
    float lifetime, fadeIn, fadeOut;  // lifetime <= 0: until removed
    int32_t projection, doubleSided, priority, staticOnly;
    float depthFadePower, normalFadeBias, normalFadeScale, autoHideDistance;  // autoHideDistance < 0: the engine's
    float uv[4];                      // atlas cell: scale u, scale v, offset u, offset v (0.5 each: the whole picture)
    int32_t raw7c = -1, raw7e = -1;   // test switches for two init bytes of open meaning (-1: the engine's default)
};
DecalDesc DecalDefaults();
// id, 0 on an error (err says why); entities null: everything in the box, else only their meshes
uint32_t DecalSpawn(const DecalDesc& d, void* const* entities, int entityCount, std::string& err);
bool DecalRemove(uint32_t id);
int DecalClear(void* entity);  // the decals on this entity (null: all of them); how many
int DecalCount();
bool DecalSetColor(uint32_t id, uint32_t color, uint32_t specularColor, float specularity, float specularScale,
                   float specularBase);
void DecalsGameLoaded();  // lifecycle.cpp: the decals went with the old world
void* DecalTexture(const char* path, std::string& err);  // a CBitmapTexture by depot path, loaded once and held
int DecalPerMesh(int n);  // decals one mesh takes (engine: 2, up to 127); n <= 0 reads; the limit before, -1 unknown
void* PhysxScene(void* world);                                 // the world's physx::NpScene, or null
std::string PhysxGravity(void* scene, float* xyz, bool set);   // "" or the reason; xyz holds the gravity after
uint64_t ReloadReaders();
void InstallReload();  // the watch on tw3se\files and the `files` command

// --- deform.cpp: vertex deformation (measuring: where a mesh's vertices are on the GPU)
void InstallDeform();
void DeformProbe(uint32_t indexCount);  // log the bindings of draws with this index count (6 times)
size_t DeformBuffersSeen();
int DeformWatchRange(uint64_t gpuLo, uint64_t gpuHi);  // a slot that notes when a draw binds this vertex range, or -1
bool DeformWatchSeen(int slot);
void DeformWatchEnd(int slot);
bool DeformDrawHooksLive();  // IASetVertexBuffers of the game's lists is hooked and has run
std::string DeformStats();
void DeformNoteSubmitted(void* const* lists, UINT n);  // overlay.cpp's ExecuteCommandLists hook
// overlay.cpp's ExecuteCommandLists hook: the game's queues counted (the one it submits the most is the blend shapes')
void DeformBeforeSubmit(void* queue, unsigned lists);
void DeformOnPresent();  // overlay.cpp's present hook: the blend shapes' work, recorded and submitted between frames

// --- blendshape.cpp: blend shapes on a CRenderMesh (deltas from a TWDF file, weights from Lua)
// A set from the file onto the mesh; `range` = the largest |weight| planned (the box grows for it). 0 + *err if not.
int BlendLoad(uintptr_t renderMesh, const std::wstring& path, float range, std::string* err);
bool BlendWeight(int set, int shape, float weight);  // shape from 0
int BlendWeights(int set, const float* weights, int count);  // file order; how many were taken (0: no such set)
int BlendShapeIndex(int set, const std::string& name);  // -1 if none
std::vector<std::string> BlendNames(int set);
std::string BlendState(int set);  // "loading", "ready", "lost" (mesh gone or loaded anew), "none"
bool BlendUnload(int set);        // the original back, then the set is gone
int HideOpen(uintptr_t renderMesh, std::string* err);  // w3.deform.hide: the mesh's hide (triangles drawn as nothing)
bool HideBits(int hide, uint32_t chunk, const uint8_t* bits, size_t n, std::string* err);  // 1 bit per triangle
bool HideClose(int hide);         // the original indices back
std::string HideState(int hide);  // "loading", "ready", "lost", "none"
int HideForget(uintptr_t renderMesh);
int BlendForget(uintptr_t renderMesh);  // the mesh goes away: its sets end without writing the original back
std::string IndexDump(uintptr_t renderMesh, const std::wstring& path, bool vertices = false);  // measuring: chunk
// table + index buffer (or the vertex buffer)
void BlendQueueRaw(void* res, uint64_t off, uint64_t size, const std::vector<uint8_t>& head,
                   const std::wstring& path);
bool SkinHookCapture(int slot, const std::wstring& path, std::string* err);  // the RT skinning's positions of a slot
bool SkinHookDqs(int slot, bool on, std::string* err);  // dual quaternion skinning for the slot's mesh
bool SkinHookDeltaMush(int slot, int iterations, std::string* err);  // mush.cpp; iterations 0 = off
bool SkinHookMushMask(int slot, int chunk, const float* mask, int count, std::string* err);
std::string MushDump(int slot, const std::wstring& path);
bool SkinHookOverlay(int slot, const void* rgba8, int size, std::string* err);  // the skin overlay of a slot
bool SkinHookOverlayPlane(int slot, int plane, const void* rgba8, int size, std::string* err);  // 0 colour, 1 material
bool SkinHookOverlayRegion(int slot, int plane, const void* rgba8, int size, int base, std::string* err);  // at base
int SkinHookChunkSlot(int slot, int chunk, std::string* err);  // a chunk drawn with a slot of its own (an alias)
int BlendResetAll();  // main thread, before the game rebuilds its render resources: every set ends, references back
// Live meshes (interface 26): a set whose positions and normals come from a plugin each frame. LiveNew: its id
// ("loading", no mesh yet); LiveAttach once the component's copy has its render mesh; BlendState gives its state.
int LiveNew();
bool LiveAttach(int id, uintptr_t renderMesh, std::string* err);
bool BlendNormals(int set, int mode, std::string* err);  // DeformNormals: 1 = from the moved triangles
bool LiveFail(int id);   // the copy never loaded: lost
bool LiveClose(int id);  // ends it without a write (the copy goes back to the game)
bool LiveInfo(int id, int chunk, int* count, int* chunks);
bool LiveWrite(int id, int chunk, const float* pos, const float* nrm, int count, std::string* err);  // any thread
bool LiveBounds(int id, const float* mn, const float* mx);  // any thread
// texwrite.cpp: pixels into a loaded texture; TexProbe measures where its D3D12 resource is
std::string TexProbe(const char* depotPath, const std::wstring& outFile);
bool TextureWrite(const char* depotPath, const void* rgba8, int w, int h, int mips, std::string* err);  // main thread
bool TexturePin(const char* depotPath, const std::wstring& ddsFile, std::string* err);  // w3.texture.write, main thread
bool TextureUnpin(const char* depotPath);                                                // w3.texture.release
void UiFrameTimes(uint64_t* frames, uint64_t* us, uint64_t* publishUs, bool reset);  // ui.cpp, overlaystats
void InstallStreamlineWatch();      // slui.cpp: the game's Streamline tags watched (overlay start)
std::string StreamlineTagStats();  // slui.cpp: what the game tagged, by type
// w3.texture.stamp: tattoos and scars on a pinned BC1 texture, through a position map (texwrite.cpp)
struct TexStampLayer {
    std::wstring motif;                       // TWMO: rgb the colour ratio * 128, a the cover
    float centre[3], normal[3], up[3], right[3];  // model space, m
    float size = 0.1f, depth = 0.04f, opacity = 1.0f;
    float facing = 0.15f;                     // texels whose normal turns further from the stamp's are left out
    bool flip = false, tint = false;
    float colour[3] = {1, 1, 1};              // with tint: the ink's own colour (0..1), the motif's alpha its amount
};
bool TextureStamp(const char* depotPath, const std::wstring& posmap, const std::vector<TexStampLayer>& layers,
                  std::string* err);
// w3.texture.eye: an eye texture mixed from layers (TWEY) and pinned on the texture (texwrite.cpp)
struct TexEyeLook {
    bool cat = false;                                 // a cat's slit and iris, else round
    float veins = 0.35f;                              // 0 none, 0.35 a usual eye's, 1 bloodshot
    float white[3] = {240 / 255.0f, 236 / 255.0f, 232 / 255.0f};
    float vein[3] = {0.59f, 0.12f, 0.12f};
    float outer[3] = {0.47f, 0.67f, 0.82f}, inner[3] = {0.78f, 0.88f, 0.94f};
    float pupil[3] = {0.03f, 0.03f, 0.04f};
};
bool TextureEye(const char* depotPath, const std::wstring& layers, const TexEyeLook& look, std::string* err);
bool TexWritesPending();  // blendshape.cpp's SubmitWork: TexRecord records the finished writes into its list
bool TexRecord(void* list, void* device, std::vector<IUnknown*>* keep);  // an ID3D12GraphicsCommandList, an ID3D12Device
std::string TexStats();
// mush.cpp: Delta Mush on a skinning hook slot (the patched vertex shaders write the skinned positions to u15 and add
// t15's offsets; its passes run in blendshape.cpp's submit)
bool MushActive();
void MushRecord(void* list, void* device, std::vector<IUnknown*>* keep);
bool MeshBuffers(uintptr_t rm, void** vbRes, uint64_t* vbOff, uint32_t* vbSize, void** ibRes, uint64_t* ibOff,
                 uint32_t* ibSize);
std::string TexReadback(const char* depotPath, const std::wstring& outFile);  // tests: the top level as the GPU holds it
std::vector<uint8_t> TexTestPicture(int w, int h, float t);
// skinhook.cpp: the skinning shader hook (skin_hook = on, tw3se\shaders\skinhook.txt)
void InstallSkinHook();
int SkinHookAdd(uintptr_t renderMesh, std::string* err, bool low = false);  // its slot 1..256 (low: 1..8 first), 0 on an error
bool SkinHookRemove(int slot);
bool SkinHookCapsules(int slot, const float* caps, int n);  // n x (a.xyz, radius, b.xyz, strength), model space
bool SkinHookCapsulesEx(int slot, const float* caps, int n);  // n x 12: ... strength, falloff, mode, 0, 0
bool SkinHookWeights(int slot, int chunk, const float* weights, int count, std::string* err);  // one a vertex
std::vector<uint32_t> SkinHookChunks(int slot);  // the vertex count of each chunk of the slot's mesh
int SkinHookState(int slot);  // 0 no mesh, 2 active, 3 lost
void SkinHookNoteVs(uint64_t id, const void* bytecode, size_t size);  // shaders.cpp: each vertex shader of a pipeline
bool SkinHookKeepsOut(uint64_t id);  // skinhook.cpp: one of skinhook.txt's shaders while the hook is off
bool SkinHookOn();
std::string SkinHookStats();
std::string BlendDump(uintptr_t renderMesh, const std::wstring& path, bool now = false);  // now: also with a set (as moved)  // base positions + normals as TWDB
std::string BlendStats();  // time spent before the game's submits since the last call

// --- shaders.cpp: the game's shaders dumped and replaced by id
void InstallShaders();

// --- loosefiles.cpp: tw3se\files\ packed into Mods\mod00000_TW3SE_Files at the start
void BuildLooseFiles();

// --- vhook.cpp: hooks on virtual methods, one vtable slot each
struct VRegs {
    uint64_t rcx, rdx, r8, r9;  // the integer argument registers (rcx: this)
    uint64_t xmm[4];            // xmm0-xmm3, the low 8 bytes; after VHookCallOriginal xmm[0] holds a float result
};
using VHandler = uint64_t (*)(int id, VRegs* regs, uint64_t* stackArgs, void* user);  // the value goes to rax and xmm0
int VHookInstall(void* vtable, int slot, VHandler handler, void* user, std::string& err);  // an id, -1 with err
bool VHookRemove(int id);
uint64_t VHookCallOriginal(int id, VRegs* regs, uint64_t* stackArgs);
void* VHookOriginal(int id);
DWORD MainThreadId();                // mainthread.cpp
bool GameStarting();                  // lifecycle.cpp: inside OnGameStart or RestoreSession (a save loading)
uint64_t LastTickMs();               // GetTickCount64 of the last tick (0 = none yet)
std::string NearestAddressName(uintptr_t a);  // addresses.cpp: "CCommonGame::Tick+0x1c" or ""
void ForEachAddress(void (*visit)(const char* name, uintptr_t a, void* ctx), void* ctx);  // addresses.cpp: every name

// --- sound.cpp: own sounds (XAudio2), interface 17, TW3SE_PlaySound, w3.sound
void InstallSound();
int32_t SoundPlay(const char* file, void* entity, const char* bone, float volume, int loop, const char* owner,
                  std::string* err);  // a handle, 0 on an error
bool SoundStop(int32_t id);
bool SoundVolume(int32_t id, float volume);
bool SoundPlaying(int32_t id);
bool SoundPreload(const char* file, std::string* err);
int SoundStopOwner(const std::string& owner);  // a Lua mod taken out: its sounds stop

// --- plugin reload (plugins.cpp): each registry drops what lies in an unloaded plugin's image [lo, hi)
int UnregisterCommandsIn(uintptr_t lo, uintptr_t hi);  // console.cpp
int DropTicksIn(uintptr_t lo, uintptr_t hi);           // mainthread.cpp (ticks and queued tasks)
int DropPresentIn(uintptr_t lo, uintptr_t hi);         // overlay.cpp (present and overlay draw)
int DropGameLoadedIn(uintptr_t lo, uintptr_t hi);      // lifecycle.cpp
int UnhookIn(uintptr_t lo, uintptr_t hi);              // main.cpp (hooks made through Hook)
int LuaDropPluginFunctionsIn(uintptr_t lo, uintptr_t hi);  // lua.cpp
void PoseWatchCounts(int* pose, int* mimic);       // posehook.cpp: figures watched for poses and for the face
int DropPoseCallbacksIn(uintptr_t lo, uintptr_t hi);
int PoseCallbacksOffIn(uintptr_t lo, uintptr_t hi);   // posehook.cpp: the pose callbacks whose code lies in [lo, hi)
int MimicCallbacksOffIn(uintptr_t lo, uintptr_t hi);  // the same for the mimic callbacks   // posehook.cpp (waits until none runs)
int DropPluginNativesIn(uintptr_t lo, uintptr_t hi);   // plugins.cpp
bool PluginReload(const std::string& name, std::string& msg);  // `plugin reload`, w3.plugin_reload
struct PluginSummary {
    std::string name, author;
    uint32_t version;   // 0xMMmmpp
    std::string file;   // plugins\<file>.dll
    std::string state;  // loaded, failed, reloading
};
std::vector<PluginSummary> PluginList();  // plugins.cpp, for F2's mod list
bool UiIsOpen();
bool UiTextHasKeys();  // a text field (ImGui's or a mod's, w3.ui.want_text) had the keyboard last frame
void UiToggle();  // the TW3SE Menu opens or closes (ui.cpp)
void UiSetWindowFlag(const std::string& which, bool on);  // ui.cpp: the Inspector / Profiler box follows a window's X

// --- pad.cpp: the controller (XInput): its buttons as keys, the menu by both sticks, the game's pad resting in the menu
uint32_t PadMask();                     // the buttons held now: bits 0-15 XInput's, 16 LT, 17 RT
int PadBit(const std::string& lower);   // "pad_a" -> 12, -1 for another name
const char* PadName(int bit);           // 12 -> "Pad_A"
void PadTick();                         // the main thread, each tick
void InstallPad();
// The TW3SE Menu's key ("F2", "Ctrl+F2"): set while the game runs (false: not a key name), its name now, and a key
// field waiting for a key (the menu key does not toggle the menu meanwhile)
bool UiSetMenuKey(const std::string& name);
std::string UiMenuKey();
void UiCaptureKeys(bool on);
bool ParseKeyName(const std::string& spec, int& vk, int& mods);  // lua.cpp: "Ctrl+Shift+K" -> key and modifiers

// --- lua.cpp for plugins (interface 16): run Lua, give Lua a plugin function (w3.plugin.<name>)
int LuaRunForPlugin(const char* code, char* reply, uint32_t size);
int LuaRegisterPluginFunction(const char* name, TW3SE_LuaFunction fn, void* ctx);
int LuaReplyBytes(const void* data, size_t size);  // a plugin's Lua function: its reply as bytes (interface 20)
// Typed values for plugins (interface 18, lua.cpp): calls, fields, script hooks and the catalog through the Lua code.
int ApiValueCall(void* object, const char* name, const TW3SE_Value* args, int argc, TW3SE_Value* results, int maxResults);
int ApiValueGet(void* object, const char* field, TW3SE_Value* out);
int ApiValueSet(void* object, const char* field, const TW3SE_Value* value);
const char* ApiValueLastError();
void ApiSetError(const char* reason);  // what LastError gives next
int ApiValueHook(const char* cls, const char* name, TW3SE_ScriptHook fn, void* ctx, int priority);
int ApiValueUnhook(int id);
int ApiValueCallOriginal(TW3SE_HookCall* call, TW3SE_Value* result);
int LuaDropPluginHooksIn(uintptr_t lo, uintptr_t hi);
const char* ApiClassName(void* cls);
void* ApiClassParent(void* cls);
int ApiForEachItem(void* cls, bool methods, TW3SE_ItemCallback cb, void* ctx);
int ApiEnumValue(const char* enumName, const char* valueName, int64_t* out);
int ApiForEachObject(const char* className, TW3SE_ObjectCallback cb, void* ctx);
// loctext.cpp: localized texts replaced by string id
bool LocTextSet(uint32_t id, const char* text, const std::string& owner);
bool LocTextClear(uint32_t id);
int LocTextDropOwner(const std::string& owner);
int LocTextCount();
void RegisterPresent(TW3SE_PresentCallback callback, void* context);
int GetBackgroundFps();          // overlay.cpp: the frame cap while another window is in front (0: none)
void SetBackgroundFps(int fps);
void RegisterOverlayDraw(TW3SE_OverlayDrawCallback cb, void* ctx);  // interface v10
// ui.cpp, plugin interface 21: ImGui for plugins and .asi mods
void UiAddPluginDraw(TW3SE_ImGuiCallback cb, void* ctx);
int UiDropPluginDrawsIn(uintptr_t lo, uintptr_t hi);
void UiSetPluginOverlay(bool on);
void UiSetCapture(bool on);
bool UiCaptureOn();
void UiImGuiAllocators(void** alloc, void** freeFn, void** user);
// messages.cpp (interface 21): channels between scripts, Lua and plugins
void MessageSend(const char* channel, const char* text);
int MessageListen(const char* channel, TW3SE_MessageCallback cb, void* ctx);
int DropMessageListenersIn(uintptr_t lo, uintptr_t hi);
std::vector<std::pair<std::string, std::string>> MessageTakeForLua();
void QueueMessageNatives();
int PadRead(uint32_t* buttons, float* axes);  // pad.cpp: the first controller, sticks and triggers too
// render.cpp (interface 21): pipelines, shaders from memory, root parameters, bind callbacks
void RenderNoteSubmitted(void* const* lists, UINT n);  // overlay.cpp's ExecuteCommandLists hook
int RenderOnPipeline(TW3SE_PipelineCallback cb, void* ctx);
int RenderShaderReplace(uint64_t id, const void* data, size_t size);
int RenderAddRootParam(uint64_t rsId, const void* param);
int RenderRootParamIndex(void* rs, int handle);
int RenderMarkPipeline(void* pso, TW3SE_BindCallback cb, void* cookie, void* ctx);
int DropRenderIn(uintptr_t lo, uintptr_t hi);
int RenderOnSubmit(TW3SE_SubmitCallback cb, void* ctx);
void RenderBeforeSubmit(void* queue, void* gameQueue);  // overlay.cpp's ExecuteCommandLists hook
void RenderOnPresent();                                 // overlay.cpp's present hook
bool RenderPsoTrace(int frames, const std::wstring& path);   // 19.11.: w3._pso_trace (render.cpp)
void OverlayApiText(float x, float y, uint32_t rgba, const char* text);
void OverlayTextSize(const char* text, float* w, float* h);  // the overlay font's size of a text, in pixels
void OverlayApiRect(float x, float y, float w, float h, uint32_t rgba);
void OverlayApiScreenSize(float* w, float* h);

// --- diag.cpp: exit stacks, DLC manager trace
void InstallDiagnostics();
void StartStackSampler();  // stacksample.cpp: main-loop stack samples to a file in out (config stacksample=on)
void InstallEditorProbes();  // editorprobe.cpp: REDkit diagnostics (config attachprobe=on)
void InstallGameGuard();    // editorprobe.cpp: the same in the game, in front of every vectored handler registered later
void InstallEditorGuard();  // editorprobe.cpp: TW3SE guards take their own faults before the editor crash handler
void InstallEnvironLog();  // environ.cpp: graphics cards and the other tools in the game, to tw3se.log
void InstallHwWatch();  // hwwatch.cpp: `hwwatch` hardware watchpoints (who calls / writes / reads), game and editor
LONG CALLBACK HwWatchVectored(EXCEPTION_POINTERS* ep);
std::string HwWatchCommand(const char* args);  // the `hwwatch` command, for Lua (w3.hwwatch) on the main thread  // its handler, first in the editor's crash handler hook
void InstallDump();  // dump.cpp: `enginedump` (globals, vtables, object dumps, threads), game and editor

// --- lifecycle.cpp: game start / save load events (interface v11)
void InstallLifecycle();
void RegisterOnGameLoaded(TW3SE_GameLoadedCallback cb, void* context);
void FireRenderRebuild(int phase);  // main thread: 0 before, 1 after the game rebuilds its render resources
int RegisterOnRenderRebuild(TW3SE_RenderRebuildCallback cb, void* context);  // interface v25
int DropRenderRebuildIn(uintptr_t lo, uintptr_t hi);

// --- worldedit.cpp: world edits and the register of the loaded objects of the game
void InstallWorldEdit();
void QueueWorldEditNatives();  // only the natives; without the attach hook they return false/NULL
void* WorldEntityByGuid(const uint32_t guid[4]);  // the loaded object with this GUID, or null
void* CurrentWorld();  // the CWorld the last entity attached to (the loaded world), or null
// Visits every tracked live entity (pre-placed, lasting-GUID objects currently loaded) as (entity, handle block).
// Snapshots under the lock first, so the callback may run game/Lua code safely.
void WorldForEachEntity(void (*visit)(void* entity, void* block, void* ctx), void* ctx);
bool WorldEntityGuid(void* entity, uint32_t out[4]);  // false for a run-time object (counter GUID)
bool WorldEditLine(const char* line);  // one world.txt line for the session, applied at once if loaded

// --- sector.cpp: the merged geometry of the world (CSectorData); for now the Umbra switch
void InstallSectorEdit();

// --- foliage.cpp: trees of the foliage (render instances), measurement stage
void InstallFoliage();

// --- questlive.cpp: the quest blocks the game activates, for the editor's graph (live)
void InstallQuestLive();

}  // namespace tw3se
