// TW3SE core - plugins: DLLs in tw3se\plugins\ (see include/tw3se/tw3se.h).
#include "core.h"
#include "imgui.h"
#include <array>
#include <mutex>
#include <set>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <string>
#include <utility>
#include <vector>

namespace tw3se {
namespace {

// Every function a plugin calls checks what it was given right here, before anything inside TW3SE sees it
// (RegisterOnTick(NULL) would call address 0 in the next tick, RunLua with a NULL reply would write to 0). A NULL
// where a pointer is needed makes the call do nothing and return 0.
char g_noReply[1];  // where a reply goes when the plugin passed none
thread_local alignas(16) uint8_t t_noResult[512];  // where a result goes when the plugin passed none
#define RESULT_OR_SCRATCH(r) ((r) ? (r) : static_cast<void*>(t_noResult))
// A pointer that is not NULL can still point at nothing (apifuzz: FindProperty(0x10, "x") faulted inside).
// The functions that take an object or class run under a guard: a fault there returns NULL / 0. (TW3SE's guards
// take their faults before the game's crash handler, editorprobe.cpp InstallGameGuard.)
#define GUARDED(expr, fail)                     \
    __try {                                     \
        return (expr);                          \
    } __except (EXCEPTION_EXECUTE_HANDLER) {    \
        return (fail);                          \
    }

void ApiLog(const char* plugin, const char* fmt, ...) {
    if (!fmt) return;
    va_list ap;
    va_start(ap, fmt);
    LogV(plugin ? plugin : "plugin", fmt, ap);
    va_end(ap);
}
uintptr_t ApiGetAddress(const char* name) { return name ? Address(name) : 0; }
uintptr_t ApiGetImageBase() { return ImageBase(); }
int ApiHook(void* target, void* detour, void** original) {
    return target && detour && original && Hook(target, detour, original) ? 1 : 0;
}
void ApiRunOnMainThread(TW3SE_Task task, void* context) {
    if (task) RunOnMainThread(task, context);
}
int ApiRegisterCommand(const char* name, TW3SE_Command handler, const char* help) {
    return name && *name && handler && RegisterCommand(name, handler, help ? help : "") ? 1 : 0;
}
int ApiMountDLC(const char* folder, char* reply, uint32_t size) {
    if (!reply) reply = g_noReply, size = sizeof g_noReply;
    return folder && *folder && MountDLC(folder, reply, size) ? 1 : 0;
}
// A plugin's natives run through a thunk of TW3SE's own (the engine keeps the thunk): after `plugin reload` the same
// name calls the new function, and while the plugin is away the thunk reads the parameters away and leaves the
// result at 0, so a script that calls it goes on.
constexpr int kMaxPluginNatives = 512;
struct PluginNative {
    std::string name;
    std::atomic<TW3SE_ScriptNative> fn{nullptr};
    int params = 0;
};
PluginNative g_pn[kMaxPluginNatives];
int g_pnCount = 0;

void SkipParams(void* frame, int params) {
    for (int k = 0; k < params; ++k) {
        alignas(16) uint8_t buf[512] = {};
        ReadScriptParam(frame, buf);
    }
    EndScriptParams(frame);
}

// A plugin's native runs under a guard (no C++ objects here, C2712). A fault ended the game inside the script VM.
bool NativeGuarded(TW3SE_ScriptNative fn, void* self, void* frame, void* result, DWORD* code) {
    __try {
        fn(self, frame, result);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

// After a fault the argument cursor stands wherever the plugin stopped reading: it goes back to the start and the
// arguments are read past, so the script goes on at the right byte with the result as the plugin left it. The native
// is off from then on (it would fault on every call) until its plugin registers it again (plugin reload).
void NativeFaulted(int i, void* frame, void* cursor, DWORD code) {
    SetScriptParamCursor(frame, cursor);
    SkipParams(frame, g_pn[i].params);
    if (g_pn[i].fn.exchange(nullptr))
        Log("plugin native %s crashed (0x%08lx) and is off until its plugin loads again", g_pn[i].name.c_str(), code);
}

template <int I>
void NativeThunk(void* self, void* frame, void* result) {
    TW3SE_ScriptNative fn = g_pn[I].fn.load(std::memory_order_acquire);
    if (!fn) {
        SkipParams(frame, g_pn[I].params);
        return;
    }
    void* cursor = ScriptParamCursor(frame);
    DWORD code = 0;
    if (!NativeGuarded(fn, self, frame, result, &code)) NativeFaulted(I, frame, cursor, code);
}
using ThunkFn = void (*)(void*, void*, void*);
template <size_t... I>
std::array<ThunkFn, sizeof...(I)> MakeThunks(std::index_sequence<I...>) {
    return {&NativeThunk<static_cast<int>(I)>...};
}
const std::array<ThunkFn, kMaxPluginNatives> g_thunks = MakeThunks(std::make_index_sequence<kMaxPluginNatives>{});

int ApiRegisterNative(const char* name, TW3SE_ScriptNative native, int paramCount) {
    if (!name || !*name || !native) return 0;
    for (int i = 0; i < g_pnCount; ++i) {
        if (g_pn[i].name != name) continue;
        if (g_pn[i].fn.load() && g_pn[i].params != paramCount)
            Log("plugins: native %s registered twice - the second one replaces the first", name);
        g_pn[i].params = paramCount;
        g_pn[i].fn.store(native, std::memory_order_release);  // a reloaded plugin: the engine keeps the thunk
        return 1;
    }
    if (g_pnCount >= kMaxPluginNatives) {
        Log("plugins: native %s: more than %d plugin natives", name, kMaxPluginNatives);
        return 0;
    }
    int i = g_pnCount++;
    NoteModNative(name);
    g_pn[i].name = name;
    g_pn[i].params = paramCount;
    g_pn[i].fn.store(native, std::memory_order_release);
    return RegisterNativeAnytime(name, g_thunks[i], paramCount) ? 1 : 0;
}
void ApiReadParam(void* frame, void* out) {
    if (frame && out) ReadScriptParam(frame, out);
}
void ApiEndParams(void* frame) {
    if (frame) EndScriptParams(frame);
}
void ApiRegisterOnTick(TW3SE_TickCallback cb, void* context) {
    if (cb) RegisterTick(cb, context);
}
void* ApiFindProperty(void* cclass, const char* name) {
    if (!cclass || !name) return nullptr;
    GUARDED(FindProperty(cclass, name), nullptr)
}
void* ApiPropertyPtr(void* object, void* cproperty) {
    if (!object || !cproperty) return nullptr;
    GUARDED(PropertyPtr(object, cproperty), nullptr)
}
void* ApiGetObjectClass(void* object) {
    if (!object) return nullptr;
    GUARDED(GetObjectClass(object), nullptr)
}
void* ApiFindClassByName(const char* name) { return name && *name ? FindClassByName(name) : nullptr; }
bool ArgsOk(const char* const* args, int argCount) {  // a count needs the array, and no entry of it may be NULL
    if (argCount < 0 || argCount > 64 || (argCount > 0 && !args)) return false;
    for (int i = 0; i < argCount; ++i)
        if (!args[i]) return false;
    return true;
}
int ApiCallFunction(const char* name, const char* const* args, int argCount, void* self, void* result) {
    return name && *name && ArgsOk(args, argCount) && CallGlobalFunc(name, args, argCount, self, RESULT_OR_SCRATCH(result)) ? 1 : 0;
}
void* ApiGetThePlayer() { return GetThePlayer(); }
void* ApiGetTheGame() { return GetTheGame(); }
int ApiCallMethod(void* object, const char* name, const char* const* args, int argCount, void* result) {
    if (!object || !name || !*name || !ArgsOk(args, argCount)) return 0;
    GUARDED(CallMethodFunc(object, name, args, argCount, RESULT_OR_SCRATCH(result)) ? 1 : 0, 0)
}
void* ApiResolveHandle(void* handle) {
    if (!handle) return nullptr;
    GUARDED(ResolveHandle(handle), nullptr)
}
const char* ApiGetCNameString(uint32_t id) { return GetCNameString(id); }
void ApiRegisterOnPresent(TW3SE_PresentCallback cb, void* context) {
    if (cb) RegisterPresent(cb, context);
}
int ApiCallFunctionV(const char* name, const char* argspec, void* self, void* result) {
    return name && *name && CallFunctionV(name, argspec ? argspec : "", self, RESULT_OR_SCRATCH(result)) ? 1 : 0;
}
int ApiCallMethodV(void* object, const char* name, const char* argspec, void* result) {
    if (!object || !name || !*name) return 0;
    GUARDED(CallMethodV(object, name, argspec ? argspec : "", RESULT_OR_SCRATCH(result)) ? 1 : 0, 0)
}
void ApiRegisterOnOverlayDraw(TW3SE_OverlayDrawCallback cb, void* ctx) {
    if (cb) RegisterOverlayDraw(cb, ctx);
}
void ApiOverlayDrawText(float x, float y, uint32_t rgba, const char* text) {
    if (text) OverlayApiText(x, y, rgba, text);
}
void ApiOverlayDrawRect(float x, float y, float w, float h, uint32_t rgba) { OverlayApiRect(x, y, w, h, rgba); }
void ApiOverlayGetScreenSize(float* w, float* h) {
    float sw = 0, sh = 0;
    OverlayApiScreenSize(&sw, &sh);
    if (w) *w = sw;
    if (h) *h = sh;
}
void ApiRegisterOnGameLoaded(TW3SE_GameLoadedCallback cb, void* ctx) {
    if (cb) RegisterOnGameLoaded(cb, ctx);
}
void* ApiEntityByGuid(const uint32_t guid[4]) { return guid ? WorldEntityByGuid(guid) : nullptr; }
int ApiEntityGuid(void* entity, uint32_t out[4]) {
    if (!entity || !out) return 0;
    GUARDED(WorldEntityGuid(entity, out) ? 1 : 0, 0)
}
int ApiWorldEdit(const char* line) { return line && *line && WorldEditLine(line) ? 1 : 0; }

// Pose/mimic hooks (interface v13, posehook.cpp; declared in core.h). Each plugin may register its own; NULL takes
// the calling plugin's out (the module its return address lies in).
bool CallerRange(void* ret, uintptr_t& lo, uintptr_t& hi) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(ret), &m) || !m)
        return false;
    lo = reinterpret_cast<uintptr_t>(m);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(lo + reinterpret_cast<const IMAGE_DOS_HEADER*>(lo)->e_lfanew);
    hi = lo + nt->OptionalHeader.SizeOfImage;
    return true;
}
void ApiRegisterOnPose(TW3SE_PoseCallback cb, void* ctx) {
    uintptr_t lo, hi;
    if (cb) PoseRegisterCallback(cb, ctx);
    else if (CallerRange(_ReturnAddress(), lo, hi)) PoseCallbacksOffIn(lo, hi);
}
int ApiWatchPose(void* entity, int on) {
    if (!entity) return 0;
    GUARDED(PoseWatch(entity, on), 0)
}
int ApiGetPose(void* entity, int kind, TW3SE_Pose* out) {
    if (!entity || !out) return 0;
    GUARDED(PoseGet(entity, kind, out), 0)
}
int ApiIsEditor() { return IsRedkitEditor() ? 1 : 0; }
void ApiRegisterOnMimic(TW3SE_MimicCallback cb, void* ctx) {
    uintptr_t lo, hi;
    if (cb) MimicRegisterCallback(cb, ctx);
    else if (CallerRange(_ReturnAddress(), lo, hi)) MimicCallbacksOffIn(lo, hi);
}
int ApiWatchMimic(void* entity, int on) {
    if (!entity) return 0;
    GUARDED(MimicWatch(entity, on), 0)
}
int ApiRunLua(const char* code, char* reply, uint32_t size) {
    if (!reply || !size) reply = g_noReply, size = sizeof g_noReply;
    if (!code) {
        reply[0] = 0;
        return 0;
    }
    return LuaRunForPlugin(code, reply, size);
}
int ApiRegisterLuaFunction(const char* name, TW3SE_LuaFunction fn, void* ctx) {
    return name && *name && fn ? LuaRegisterPluginFunction(name, fn, ctx) : 0;
}
std::string SoundOwnerOf(uintptr_t lo);  // below
int32_t ApiPlaySound(const char* file, void* entity, const char* bone, float volume, int loop) {
    if (!file || !*file) return 0;
    HMODULE caller = nullptr;  // the plugin's sounds stop when it is unloaded
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(_ReturnAddress()), &caller);
    std::string owner = caller ? SoundOwnerOf(reinterpret_cast<uintptr_t>(caller)) : "";
    return SoundPlay(file, entity, bone, volume, loop, owner.c_str(), nullptr);
}
int ApiStopSound(int32_t id) { return SoundStop(id) ? 1 : 0; }
int ApiSetSoundVolume(int32_t id, float v) { return SoundVolume(id, v) ? 1 : 0; }
int ApiSoundPlaying(int32_t id) { return SoundPlaying(id) ? 1 : 0; }
int ApiPreloadSound(const char* file) { return file && *file && SoundPreload(file, nullptr) ? 1 : 0; }
int ApiForEachField(void* cls, TW3SE_ItemCallback cb, void* ctx) {
    if (!cls || !cb) return 0;
    GUARDED(ApiForEachItem(cls, false, cb, ctx), 0)
}
int ApiForEachMethod(void* cls, TW3SE_ItemCallback cb, void* ctx) {
    if (!cls || !cb) return 0;
    GUARDED(ApiForEachItem(cls, true, cb, ctx), 0)
}
const char* ApiClassNameG(void* cls) {
    if (!cls) return nullptr;
    GUARDED(ApiClassName(cls), nullptr)
}
void* ApiClassParentG(void* cls) {
    if (!cls) return nullptr;
    GUARDED(ApiClassParent(cls), nullptr)
}

// --- dynamic decals (interface 19, decal.cpp)
uint32_t SwapRgba(uint32_t c) {  // 0xRRGGBBAA -> the game's Color bytes, red first
    return (c >> 24) | ((c >> 8) & 0xff00) | ((c << 8) & 0xff0000) | (c << 24);
}
void ApiDecalDefaults(TW3SE_Decal* o) {
    if (!o) return;
    DecalDesc d = DecalDefaults();
    *o = {};
    memcpy(o->position, d.pos, sizeof o->position);
    memcpy(o->direction, d.dir, sizeof o->direction);
    memcpy(o->up, d.up, sizeof o->up);
    o->width = d.width, o->height = d.height, o->depth = d.depth;
    o->color = o->specularColor = 0xffffffff;
    o->specularity = d.specularity, o->specularScale = d.specularScale, o->specularBase = d.specularBase;
    o->depthFadePower = d.depthFadePower, o->normalFadeBias = d.normalFadeBias, o->normalFadeScale = d.normalFadeScale;
    o->autoHideDistance = d.autoHideDistance;
    memcpy(o->atlas, d.uv, sizeof o->atlas);
}
int SpawnDecalsImpl(const TW3SE_Decal* in, int count, void* const* ents, int entCount, uint32_t* ids) {
    int made = 0;
    for (int i = 0; i < count; ++i) {
        const TW3SE_Decal& s = in[i];
        std::string err;
        DecalDesc d = DecalDefaults();
        memcpy(d.pos, s.position, sizeof d.pos);
        memcpy(d.dir, s.direction, sizeof d.dir);
        memcpy(d.up, s.up, sizeof d.up);
        d.width = s.width, d.height = s.height, d.depth = s.depth;
        d.diffuse = DecalTexture(s.diffuse, err);
        d.normal = s.normal && *s.normal ? DecalTexture(s.normal, err) : nullptr;
        d.color = SwapRgba(s.color), d.specularColor = SwapRgba(s.specularColor);
        d.specularity = s.specularity, d.specularScale = s.specularScale, d.specularBase = s.specularBase;
        d.lifetime = s.lifetime, d.fadeIn = s.fadeIn, d.fadeOut = s.fadeOut;
        d.projection = s.projection, d.doubleSided = s.doubleSided, d.priority = s.priority;
        d.staticOnly = s.staticOnly;
        d.depthFadePower = s.depthFadePower, d.normalFadeBias = s.normalFadeBias, d.normalFadeScale = s.normalFadeScale;
        d.autoHideDistance = s.autoHideDistance;
        if (s.atlas[0] != 0 || s.atlas[1] != 0) memcpy(d.uv, s.atlas, sizeof d.uv);
        uint32_t id = 0;
        if (d.diffuse && (!s.normal || !*s.normal || d.normal)) id = DecalSpawn(d, entCount > 0 ? ents : nullptr, entCount, err);
        if (!id) ApiSetError(err.c_str());
        if (ids) ids[i] = id;
        if (id) ++made;
    }
    return made;
}
int ApiSpawnDecals(const TW3SE_Decal* in, int count, void* const* ents, int entCount, uint32_t* ids) {
    if (!in || count <= 0 || (entCount > 0 && !ents)) return 0;
    GUARDED(SpawnDecalsImpl(in, count, ents, entCount, ids), 0)
}
int ApiRemoveDecal(uint32_t id) { GUARDED(DecalRemove(id) ? 1 : 0, 0) }
int ApiClearDecals(void* entity) { GUARDED(DecalClear(entity), 0) }
int ApiSetDecalColor(uint32_t id, uint32_t color, uint32_t spec, float specularity, float scale, float base) {
    GUARDED(DecalSetColor(id, SwapRgba(color), SwapRgba(spec), specularity, scale, base) ? 1 : 0, 0)
}
int ApiDecalCount() { GUARDED(DecalCount(), 0) }
// perMesh 1..127, total / chunks: the engine's DynamicDecalsLimit / DynamicDecalsChunkLimit; 0 leaves one as it is
int ApiLuaReplyBytes(const void* data, size_t size) { return LuaReplyBytes(data, size); }
void ApiReturnString(void* result, const char* utf8, uint32_t length) {
    if (result) SetScriptResultString(result, utf8 ? utf8 : "", utf8 ? length : 0);
}
uint32_t ApiNameFromString(const char* text) {
    if (!text || !*text || strcmp(text, "None") == 0) return 0;
    return InternName(text);
}
// ImGui for plugins and .asi mods (interface 21). Overlay and input are asked for per module: on while one wants it.
std::mutex g_imMx;
std::set<uintptr_t> g_imOverlay, g_imCapture;  // the modules (image base) that want them
uintptr_t ModuleOf(void* ret) {
    HMODULE m = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(ret), &m) ? reinterpret_cast<uintptr_t>(m) : 0;
}
int ApiRegisterOnImGui(TW3SE_ImGuiCallback cb, void* ctx, int ver, uint32_t ioSize, uint32_t styleSize) {
    if (!cb) return 0;
    if (ver != IMGUI_VERSION_NUM || ioSize != sizeof(ImGuiIO) || styleSize != sizeof(ImGuiStyle)) {
        Log("plugins: an ImGui window callback refused - built with ImGui %d (ImGuiIO %u, ImGuiStyle %u bytes), TW3SE "
            "has ImGui %d (%u, %u) with its imconfig: compile the ImGui from TW3SE's SDK (include\\tw3se\\imgui)",
            ver, ioSize, styleSize, IMGUI_VERSION_NUM, static_cast<unsigned>(sizeof(ImGuiIO)),
            static_cast<unsigned>(sizeof(ImGuiStyle)));
        return 0;
    }
    UiAddPluginDraw(cb, ctx);
    return 1;
}
void ApiImGuiAllocators(void** a, void** f, void** u) { UiImGuiAllocators(a, f, u); }
void ImSet(std::set<uintptr_t>& s, uintptr_t m, bool on, bool capture) {
    bool any;
    {
        std::lock_guard<std::mutex> lk(g_imMx);
        if (on) s.insert(m);
        else s.erase(m);
        any = !s.empty();
    }
    if (capture) UiSetCapture(any);
    else UiSetPluginOverlay(any);
}
void ApiImGuiOverlay(int on) { ImSet(g_imOverlay, ModuleOf(_ReturnAddress()), on != 0, false); }
void ApiImGuiCaptureInput(int on) { ImSet(g_imCapture, ModuleOf(_ReturnAddress()), on != 0, true); }
void ApiSendToChannel(const char* channel, const char* message) { MessageSend(channel, message); }
int ApiRegisterOnMessage(const char* channel, TW3SE_MessageCallback cb, void* ctx) {
    return MessageListen(channel, cb, ctx);
}
bool StateCall(void* obj, const char* fn, TW3SE_Value* out) {
    TW3SE_Value r[2] = {};
    if (!obj || ApiValueCall(obj, fn, nullptr, 0, r, 2) < 1) return false;
    *out = r[0];
    return true;
}
bool StateBool(void* obj, const char* fn) {
    TW3SE_Value v{};
    return StateCall(obj, fn, &v) && v.type == TW3SE_BOOL && v.v.i != 0;
}
uint32_t ApiGameState() {
    if (!OnMainThread()) return 0;
    uint32_t s = 0;
    void* game = GetTheGame();
    void* player = GetThePlayer();
    s |= player ? TW3SE_STATE_IN_GAME : TW3SE_STATE_MAIN_MENU;
    if (game) {
        if (StateBool(game, "IsPaused")) s |= TW3SE_STATE_PAUSED;
        if (StateBool(game, "IsLoadingScreenVideoPlaying") || StateBool(game, "IsBlackscreen")) s |= TW3SE_STATE_LOADING;
        if (player && StateBool(game, "IsDialogOrCutscenePlaying")) s |= TW3SE_STATE_SCENE;
        TW3SE_Value gui{};
        if (StateCall(game, "GetGuiManager", &gui) && gui.type == TW3SE_OBJECT && StateBool(gui.v.object, "IsAnyMenu"))
            s |= TW3SE_STATE_GAME_MENU;
    }
    if (UiIsOpen()) s |= TW3SE_STATE_TW3SE_MENU;
    if (UiCaptureOn()) s |= TW3SE_STATE_CAPTURE;
    return s;
}
int ApiPadRead(uint32_t* buttons, float* axes) { return PadRead(buttons, axes); }
int DropImGuiIn(uintptr_t lo, uintptr_t hi) {
    int n = UiDropPluginDrawsIn(lo, hi);
    for (int k = 0; k < 2; ++k) {
        std::set<uintptr_t>& s = k ? g_imCapture : g_imOverlay;
        bool had;
        {
            std::lock_guard<std::mutex> lk(g_imMx);
            had = s.erase(lo) > 0;
        }
        (void)hi;
        if (had) ImSet(s, 0, false, k == 1), ++n;
    }
    return n;
}
int ApiSetDecalBudget(int perMesh, int total, int chunks) {
    int ok = 1;
    if (perMesh > 0 && DecalPerMesh(perMesh) < 0) ok = 0;
    std::string out, err;
    if (total > 0 && !EngineConfig("Rendering", "DynamicDecalsLimit", std::to_string(total).c_str(), out, err)) ok = 0;
    if (chunks > 0 && !EngineConfig("Rendering", "DynamicDecalsChunkLimit", std::to_string(chunks).c_str(), out, err)) ok = 0;
    return ok;
}

// Blend shapes (interface 22). Own, release, loaded and load go through Lua's w3.deform, so a copy made by a plugin
// and one made by a Lua mod are the same (one table of copies, the same file names); the weights go straight to
// blendshape.cpp, from any thread.
std::string DeformLua(void* comp, const char* call) {
    char code[1400];
    snprintf(code, sizeof code,
             "local c = w3.object(%llu) if not c then return 'err not an object' end "
             "local ok, a, b = pcall(%s) if not ok then return 'err ' .. tostring(a) end "
             "if a == nil or a == false then return 'err ' .. tostring(b) end return 'ok ' .. tostring(a)",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(comp)), call);
    if (!OnMainThread()) return "err main thread only (RunOnMainThread)";
    char reply[512];
    LuaRunForPlugin(code, reply, sizeof reply);
    return reply;
}
int DeformResult(const std::string& r) {
    if (r.rfind("ok ", 0) == 0) return 1;
    ApiSetError(r.rfind("err ", 0) == 0 ? r.c_str() + 4 : r.c_str());
    return 0;
}
int ApiDeformOwn(void* comp) {
    if (!comp) return 0;
    return DeformResult(DeformLua(comp, "w3.deform.own, c"));
}
int ApiDeformLoaded(void* comp) {
    return comp && DeformLua(comp, "function() return w3.deform.loaded(c) end") == "ok true" ? 1 : 0;
}
int ApiDeformRelease(void* comp) {
    if (!comp) return 0;
    std::string r = DeformLua(comp, "w3.deform.release, c");
    return DeformResult(r == "err nil" ? "err the component has no copy of its own" : r);
}
int32_t ApiDeformLoad(void* comp, const char* path, float range) {
    if (!comp || !path || !*path || strstr(path, "]==]")) {
        ApiSetError("no component or no path");
        return 0;
    }
    char call[700];
    snprintf(call, sizeof call, "w3.deform.load, c, [==[%s]==], %.9g", path, range > 0 ? range : 1.0f);
    std::string r = DeformLua(comp, call);
    return DeformResult(r) ? static_cast<int32_t>(atoi(r.c_str() + 3)) : 0;
}
int ApiDeformWeights(int32_t set, const float* w, int n) { return BlendWeights(set, w, n); }
int ApiDeformState(int32_t set) {
    std::string s = BlendState(set);
    return s == "loading" ? TW3SE_DEFORM_LOADING : s == "ready" ? TW3SE_DEFORM_READY
                                                 : s == "lost"  ? TW3SE_DEFORM_LOST : TW3SE_DEFORM_NONE;
}
int ApiDeformUnload(int32_t set) { return BlendUnload(set) ? 1 : 0; }
int ApiDeformShapeIndex(int32_t set, const char* name) { return name ? BlendShapeIndex(set, name) : -1; }

// The skinning hook (interface 23, skinhook.cpp). Add goes through Lua's w3.skinhook.add (the render mesh of the
// component, as for the blend shapes).
int ApiSkinHookActive() { return SkinHookOn() ? 1 : 0; }
int ApiSkinHookAdd(void* comp) {
    if (!comp) return 0;
    std::string r = DeformLua(comp, "w3.skinhook.add, c");
    return DeformResult(r) ? atoi(r.c_str() + 3) : 0;
}
int ApiSkinHookCapsules(int slot, const float* caps, int n) { return SkinHookCapsules(slot, caps, n) ? 1 : 0; }
int ApiSkinHookCapsulesEx(int slot, const float* caps, int n) { return SkinHookCapsulesEx(slot, caps, n) ? 1 : 0; }
int ApiSkinHookOverlay(int slot, const void* rgba8, int size) {
    std::string err;
    if (SkinHookOverlay(slot, rgba8, size, &err)) return 1;
    ApiSetError(err.c_str());
    return 0;
}
void* ApiEngineAddress(const char* name) {
    if (!name) return nullptr;
    std::string err;
    uintptr_t a = EngineResolve(name, &err);
    if (!a) ApiSetError(err.c_str());
    return reinterpret_cast<void*>(a);
}
void* ApiEngineVtable(const char* className, uint32_t offset) {
    return className ? reinterpret_cast<void*>(EngineVtable(className, offset)) : nullptr;
}
int ApiSkinHookOverlayMaterial(int slot, const void* rgba8, int size) {
    std::string err;
    if (SkinHookOverlayPlane(slot, 1, rgba8, size, &err)) return 1;
    ApiSetError(err.c_str());
    return 0;
}
int ApiSkinHookOverlayRegion(int slot, int plane, const void* rgba8, int size, int base) {
    std::string err;
    if (SkinHookOverlayRegion(slot, plane, rgba8, size, base, &err)) return 1;
    ApiSetError(err.c_str());
    return 0;
}
int ApiSkinHookChunkSlot(int slot, int chunk) {
    std::string err;
    const int s = SkinHookChunkSlot(slot, chunk, &err);
    if (!s) ApiSetError(err.c_str());
    return s;
}
int ApiSkinHookDqs(int slot, int on) {
    std::string err;
    if (SkinHookDqs(slot, on != 0, &err)) return 1;
    ApiSetError(err.c_str());
    return 0;
}
int ApiSkinHookDeltaMush(int slot, int iterations) {
    std::string err;
    if (SkinHookDeltaMush(slot, iterations, &err)) return 1;
    ApiSetError(err.c_str());
    return 0;
}
int ApiSkinHookMushMask(int slot, int chunk, const float* mask, int count) {
    std::string err;
    if (SkinHookMushMask(slot, chunk, mask, count, &err)) return 1;
    ApiSetError(err.c_str());
    return 0;
}
int ApiSkinHookState(int slot) {
    int st = SkinHookState(slot);
    return st == 2 ? TW3SE_DEFORM_READY : st == 3 ? TW3SE_DEFORM_LOST : TW3SE_DEFORM_NONE;
}
int ApiSkinHookRemove(int slot) { return SkinHookRemove(slot) ? 1 : 0; }
int ApiGetComponentPose(void* component, TW3SE_Pose* out) {  // interface 24
    if (!component || !out) return 0;
    GUARDED(PoseGetComponent(component, out), 0)
}
int ApiRegisterOnRenderRebuild(TW3SE_RenderRebuildCallback cb, void* ctx) {  // interface 25
    return RegisterOnRenderRebuild(cb, ctx);
}
// Live meshes (interface 26). Open and close go through Lua's w3.deform.live (the copy is the same as w3.deform.own's);
// info, write, bounds and state go straight to blendshape.cpp, from any thread.
int32_t ApiLiveMeshOpen(void* comp) {
    if (!comp) return 0;
    std::string r = DeformLua(comp, "w3.deform.live, c");
    return DeformResult(r) ? static_cast<int32_t>(atoi(r.c_str() + 3)) : 0;
}
int ApiLiveMeshInfo(int32_t id, int chunk, int* count, int* chunks) { return LiveInfo(id, chunk, count, chunks) ? 1 : 0; }
int ApiLiveMeshWrite(int32_t id, int chunk, const float* pos, const float* nrm, int count) {
    std::string err;
    if (LiveWrite(id, chunk, pos, nrm, count, &err)) return 1;
    ApiSetError(err.c_str());
    return 0;
}
int ApiLiveMeshBounds(int32_t id, const float* mn, const float* mx) { return LiveBounds(id, mn, mx) ? 1 : 0; }
int ApiLiveMeshState(int32_t id) { return ApiDeformState(id); }
int ApiLiveMeshClose(int32_t id) {
    if (!OnMainThread()) {
        ApiSetError("main thread only (RunOnMainThread)");
        return 0;
    }
    char code[96], reply[64];
    snprintf(code, sizeof code, "return tostring(w3.deform.live_close(%d))", static_cast<int>(id));
    LuaRunForPlugin(code, reply, sizeof reply);
    if (strstr(reply, "true")) return 1;
    ApiSetError(strstr(reply, "false") ? "no such live mesh" : reply);
    return 0;
}
int ApiTextureWrite(const char* path, const void* rgba8, int w, int h, int mips) {  // interface 26, texwrite.cpp
    std::string err;
    if (TextureWrite(path, rgba8, w, h, mips, &err)) return 1;
    ApiSetError(err.c_str());
    return 0;
}
int ApiDeformNormals(int32_t id, int mode) {  // interface 26
    std::string err;
    if (BlendNormals(id, mode ? 1 : 0, &err)) return 1;
    ApiSetError(err.c_str());
    return 0;
}
int ApiSkinHookWeights(int slot, int chunk, const float* w, int n) {
    std::string err;
    if (SkinHookWeights(slot, chunk, w, n, &err)) return 1;
    ApiSetError(err.c_str());
    return 0;
}

TW3SE_Interface g_api = {
    TW3SE_INTERFACE_VERSION,
    (TW3SE_VERSION_MAJOR << 16) | (TW3SE_VERSION_MINOR << 8) | TW3SE_VERSION_PATCH,
    0,
    sizeof(TW3SE_Interface),
    &ApiLog,
    &ApiGetAddress,
    &ApiGetImageBase,
    &ApiHook,
    &ApiRunOnMainThread,
    &ApiRegisterCommand,
    &ApiMountDLC,
    &ApiRegisterNative,
    &ApiReadParam,
    &ApiEndParams,
    &ApiRegisterOnTick,
    &ApiFindProperty,
    &ApiPropertyPtr,
    &ApiGetObjectClass,
    &ApiFindClassByName,
    &ApiCallFunction,
    &ApiGetThePlayer,
    &ApiGetTheGame,
    &ApiCallMethod,
    &ApiResolveHandle,
    &ApiGetCNameString,
    &ApiRegisterOnPresent,
    &ApiCallFunctionV,
    &ApiCallMethodV,
    &ApiRegisterOnOverlayDraw,
    &ApiOverlayDrawText,
    &ApiOverlayDrawRect,
    &ApiOverlayGetScreenSize,
    &ApiRegisterOnGameLoaded,
    &ApiEntityByGuid,
    &ApiEntityGuid,
    &ApiWorldEdit,
    &ApiRegisterOnPose,
    &ApiWatchPose,
    &ApiGetPose,
    &ApiRegisterOnMimic,
    &ApiWatchMimic,
    &ApiIsEditor,
    &ApiRunLua,
    &ApiRegisterLuaFunction,
    &ApiPlaySound,
    &ApiStopSound,
    &ApiSetSoundVolume,
    &ApiSoundPlaying,
    &ApiPreloadSound,
    &ApiValueCall,
    &ApiValueGet,
    &ApiValueSet,
    &ApiValueLastError,
    &ApiValueHook,
    &ApiValueUnhook,
    &ApiValueCallOriginal,
    &ApiClassNameG,
    &ApiClassParentG,
    &ApiForEachField,
    &ApiForEachMethod,
    &ApiEnumValue,
    &ApiForEachObject,
    &ApiDecalDefaults,
    &ApiSpawnDecals,
    &ApiRemoveDecal,
    &ApiClearDecals,
    &ApiSetDecalColor,
    &ApiDecalCount,
    &ApiSetDecalBudget,
    &ApiLuaReplyBytes,
    &ApiReturnString,
    &ApiNameFromString,
    &ApiRegisterOnImGui,
    &ApiImGuiAllocators,
    &ApiImGuiOverlay,
    &ApiImGuiCaptureInput,
    &ApiSendToChannel,
    &ApiRegisterOnMessage,
    &ApiGameState,
    &ApiPadRead,
    &RenderOnPipeline,
    &RenderShaderReplace,
    &RenderAddRootParam,
    &RenderRootParamIndex,
    &RenderMarkPipeline,
    &RenderOnSubmit,
    &ApiDeformOwn,
    &ApiDeformLoaded,
    &ApiDeformRelease,
    &ApiDeformLoad,
    &ApiDeformWeights,
    &ApiDeformState,
    &ApiDeformUnload,
    &ApiDeformShapeIndex,
    &ApiSkinHookActive,
    &ApiSkinHookAdd,
    &ApiSkinHookCapsules,
    &ApiSkinHookState,
    &ApiSkinHookRemove,
    &ApiSkinHookWeights,
    &ApiGetComponentPose,
    &ApiRegisterOnRenderRebuild,
    &ApiLiveMeshOpen,
    &ApiLiveMeshInfo,
    &ApiLiveMeshWrite,
    &ApiLiveMeshBounds,
    &ApiLiveMeshState,
    &ApiLiveMeshClose,
    &ApiTextureWrite,
    &ApiDeformNormals,
    &ApiSkinHookCapsulesEx,
    &ApiSkinHookDeltaMush,
    &ApiSkinHookMushMask,
    &ApiSkinHookOverlay,
    &ApiSkinHookOverlayMaterial,
    &ApiEngineAddress,
    &ApiEngineVtable,
    &ApiSkinHookDqs,
    &ApiSkinHookOverlayRegion,
    &ApiSkinHookChunkSlot,
};

using Query_t = int (*)(TW3SE_PluginInfo*);
using Load_t = int (*)(const TW3SE_Interface*);
using Unload_t = int (*)();

// --- plugins that load again (`plugin reload <name>`). Each plugin DLL is loaded from a copy next to it
// (<name>.live<n>.dll), so a build can overwrite the original while the game runs. Unloading: the plugin's optional
// TW3SE_PluginUnload runs and answers how many ticks it still needs (its tick, pose and mimic callbacks go on for
// them, to put bones back); its commands, natives and Lua functions go at once. Then every registry drops what lies
// in its image (ticks, present and overlay callbacks, game-loaded callbacks, pose and mimic callbacks, hooks made
// through the interface, queued main-thread tasks, its sounds), a few more ticks pass for the render thread, and the
// copy is freed. The original is copied and loaded again: PluginQuery, PluginLoad, the same interface.
enum class PState { Loaded, Unloading, Freeing, Failed };
struct PluginRec {
    std::string name;       // from PluginQuery
    std::string author;     // from PluginQuery (F2's mod list)
    uint32_t version = 0;   // from PluginQuery, 0xMMmmpp
    std::wstring original;  // plugins\<file>.dll
    std::wstring loaded;    // the copy it runs from (or the original with plugin_reload = off)
    HMODULE module = nullptr;
    uintptr_t lo = 0, hi = 0;  // its image
    PState state = PState::Loaded;
    int wait = 0;           // ticks left in Unloading / Freeing
};
std::vector<PluginRec> g_plugins;
bool g_copies = true;  // plugin_reload = off: load the originals (no reload)
int g_copyNo = 0;

std::string Narrow(const std::wstring& w) {
    char b[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof b, nullptr, nullptr);
    return b;
}
std::wstring FileName(const std::wstring& p) { return p.substr(p.find_last_of(L"\\/") + 1); }
bool IsCopy(const std::wstring& file) { return file.find(L".live") != std::wstring::npos; }

void ImageRange(HMODULE m, uintptr_t& lo, uintptr_t& hi) {
    lo = reinterpret_cast<uintptr_t>(m);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(lo + reinterpret_cast<const IMAGE_DOS_HEADER*>(lo)->e_lfanew);
    hi = lo + nt->OptionalHeader.SizeOfImage;
}

int QueryGuarded(Query_t f, TW3SE_PluginInfo* info, DWORD* code) {  // -1: it crashed
    __try {
        return f(info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return -1;
    }
}
int LoadGuarded(Load_t f, const TW3SE_Interface* api, DWORD* code) {  // -1: it crashed
    __try {
        return f(api);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return -1;
    }
}

int UnloadGuarded(Unload_t f) {
    __try {
        return f();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Loads one plugin from `original` (through a copy when reloading is on). Fills `rec`; false when it did not load.
bool LoadOne(const std::wstring& original, PluginRec& rec) {
    std::wstring file = FileName(original);
    std::wstring path = original;
    if (g_copies) {
        std::wstring stem = original.substr(0, original.size() - 4);  // without .dll
        std::wstring copy = stem + L".live" + std::to_wstring(++g_copyNo) + L".dll";
        if (CopyFileW(original.c_str(), copy.c_str(), FALSE)) path = copy;
        else Log("plugin %ls: no copy to run from (%lu), loads the original (no reload for it)", file.c_str(), GetLastError());
    }
    HMODULE m = LoadLibraryW(path.c_str());
    if (!m) {
        Log("plugin %ls: could not be loaded (%lu)", file.c_str(), GetLastError());
        if (path != original) DeleteFileW(path.c_str());
        return false;
    }
    auto query = reinterpret_cast<Query_t>(GetProcAddress(m, "TW3SE_PluginQuery"));
    auto load = reinterpret_cast<Load_t>(GetProcAddress(m, "TW3SE_PluginLoad"));
    TW3SE_PluginInfo info = {1, nullptr, nullptr, 0, 0};
    const char* why = nullptr;
    DWORD code = 0;
    int asked = query && load ? QueryGuarded(query, &info, &code) : 0;
    if (!query || !load) why = "not a TW3SE plugin (TW3SE_PluginQuery/TW3SE_PluginLoad missing)";
    else if (asked < 0) {
        Log("plugin %ls: TW3SE_PluginQuery crashed (0x%08lx)", file.c_str(), code);
        why = "crashed while asked for its name, it stays off";
    } else if (!asked || !info.name) why = "declined to load";
    else if (info.minInterface > TW3SE_INTERFACE_VERSION) why = "needs a newer TW3SE interface - update TW3SE";
    if (why) {
        Log("plugin %ls: %s", file.c_str(), why);
        FreeLibrary(m);
        if (path != original) DeleteFileW(path.c_str());
        return false;
    }
    rec = PluginRec{};
    rec.name = info.name;
    rec.author = info.author ? info.author : "";
    rec.version = info.version;
    rec.original = original;
    rec.loaded = path;
    rec.module = m;
    ImageRange(m, rec.lo, rec.hi);
    int loaded = LoadGuarded(load, &g_api, &code);
    if (loaded < 0) {  // what it did before the fault stays (a hook, a callback): its callbacks run guarded
        Log("plugin %s: crashed in TW3SE_PluginLoad (0x%08lx), it stays off", info.name, code);
        ShowNotice("TW3SE: the plugin " + rec.name + " crashed while loading and is off (tw3se.log)");
        rec.state = PState::Failed;
        return true;
    }
    if (!loaded) {
        Log("plugin %s: its load failed", info.name);
        rec.state = PState::Failed;  // stays loaded: it may already have hooked something
        return true;
    }
    Log("plugin %s %u.%u.%u by %s loaded%s", info.name, info.version >> 16, (info.version >> 8) & 0xff,
        info.version & 0xff, info.author ? info.author : "?", path != original ? " (from a copy, reloadable)" : "");
    return true;
}

std::string SoundOwnerOf(uintptr_t lo) {  // the owner tag of a plugin's sounds (ApiPlaySound)
    char b[40];
    snprintf(b, sizeof b, "plugin:%llx", static_cast<unsigned long long>(lo));
    return b;
}

void DropAll(PluginRec& r) {  // everything else of the plugin, before its image is freed
    int n = DropTicksIn(r.lo, r.hi) + DropPresentIn(r.lo, r.hi) + DropGameLoadedIn(r.lo, r.hi) +
            DropRenderRebuildIn(r.lo, r.hi) +
            DropPoseCallbacksIn(r.lo, r.hi) + UnhookIn(r.lo, r.hi) + SoundStopOwner(SoundOwnerOf(r.lo)) +
            DropImGuiIn(r.lo, r.hi) + DropMessageListenersIn(r.lo, r.hi) + DropRenderIn(r.lo, r.hi);
    Log("plugin %s: %d callback(s), hook(s) and sound(s) taken out", r.name.c_str(), n);
}

void OnPluginTick(float, void*) {
    for (PluginRec& r : g_plugins) {
        if (r.state == PState::Unloading) {
            if (r.wait > 0) {
                --r.wait;
                continue;
            }
            DropAll(r);
            r.state = PState::Freeing;
            r.wait = 3;  // a present or overlay callback started before still finishes on the render thread
        } else if (r.state == PState::Freeing) {
            if (r.wait-- > 0) continue;
            std::string name = r.name;
            FreeLibrary(r.module);
            if (r.loaded != r.original) DeleteFileW(r.loaded.c_str());
            PluginRec fresh;
            if (LoadOne(r.original, fresh)) {
                r = fresh;
                Log("plugin %s: loaded again", r.name.c_str());
            } else {
                r.module = nullptr;
                r.state = PState::Failed;
                r.lo = r.hi = 0;
                Log("plugin %s: did not load again (see above)", name.c_str());
            }
        }
    }
}

PluginRec* FindPlugin(const std::string& name) {
    for (PluginRec& r : g_plugins) {
        std::wstring stem = FileName(r.original);
        stem = stem.substr(0, stem.size() - 4);
        if (_stricmp(r.name.c_str(), name.c_str()) == 0 || _stricmp(Narrow(stem).c_str(), name.c_str()) == 0) return &r;
    }
    return nullptr;
}

std::vector<std::string> g_loaded;  // names of successfully loaded plugins (for the `plugins` command)

void CmdVersion(const char*, char* reply, uint32_t size) {
    snprintf(reply, size, "TW3SE %u.%u.%u, interface %u, game build %08x", TW3SE_VERSION_MAJOR, TW3SE_VERSION_MINOR,
             TW3SE_VERSION_PATCH, TW3SE_INTERFACE_VERSION, GameBuild());
}

void CmdPlugins(const char*, char* reply, uint32_t size) {
    std::string s = "plugins (" + std::to_string(g_plugins.size()) + "):";
    for (const PluginRec& r : g_plugins) {
        s += " " + r.name;
        if (r.state == PState::Unloading || r.state == PState::Freeing) s += " (reloading)";
        else if (r.state == PState::Failed) s += " (failed)";
    }
    strncpy_s(reply, size, s.c_str(), _TRUNCATE);
}

void CmdPlugin(const char* args, char* reply, uint32_t size) {
    std::string a = args ? args : "";
    if (a.rfind("reload ", 0) != 0) {
        strncpy_s(reply, size, "usage: plugin reload <name>", _TRUNCATE);
        return;
    }
    std::string msg;
    PluginReload(a.substr(7), msg);
    strncpy_s(reply, size, msg.c_str(), _TRUNCATE);
}

}  // namespace

std::vector<PluginSummary> PluginList() {  // F2's mod list (main thread)
    std::vector<PluginSummary> out;
    for (const PluginRec& r : g_plugins) {
        const char* st = r.state == PState::Loaded ? "loaded" : r.state == PState::Failed ? "failed" : "reloading";
        out.push_back({r.name, r.author, r.version, Narrow(FileName(r.original)), st});
    }
    return out;
}

// Starts reloading a plugin (main thread). The answer says what happens; the plugin is back a few ticks later.
bool PluginReload(const std::string& name, std::string& msg) {
    PluginRec* r = FindPlugin(name);
    if (!r) {
        msg = "error no plugin " + name + " (plugins lists them)";
        return false;
    }
    if (r->state == PState::Unloading || r->state == PState::Freeing) {
        msg = r->name + " is reloading already";
        return false;
    }
    if (!r->module) {
        msg = "error " + r->name + " is not loaded";
        return false;
    }
    if (r->loaded == r->original) {
        msg = "error " + r->name + " runs from its original file (plugin_reload = off, or the copy failed): restart the game";
        return false;
    }
    int ticks = 0;
    if (auto unload = reinterpret_cast<Unload_t>(GetProcAddress(r->module, "TW3SE_PluginUnload"))) {
        ticks = UnloadGuarded(unload);
        if (ticks < 0) {
            Log("plugin %s: TW3SE_PluginUnload faulted", r->name.c_str());
            ticks = 0;
        }
        if (ticks > 600) ticks = 600;
    }
    int n = UnregisterCommandsIn(r->lo, r->hi) + DropPluginNativesIn(r->lo, r->hi) + LuaDropPluginFunctionsIn(r->lo, r->hi) +
            LuaDropPluginHooksIn(r->lo, r->hi);
    r->state = PState::Unloading;
    r->wait = ticks;
    Log("plugin %s: unloading (%d command(s)/native(s)/Lua function(s) off, %d tick(s) for the plugin)", r->name.c_str(), n,
        ticks);
    msg = r->name + " unloading, loads again in " + std::to_string(ticks + 5) + " tick(s)";
    return true;
}

const TW3SE_Interface* Interface() { return &g_api; }

// A plugin is unloaded: its natives point nowhere until it registers them again (the thunks stay with the engine).
int DropPluginNativesIn(uintptr_t lo, uintptr_t hi) {
    int n = 0;
    for (int i = 0; i < g_pnCount; ++i) {
        uintptr_t f = reinterpret_cast<uintptr_t>(g_pn[i].fn.load());
        if (f >= lo && f < hi) {
            g_pn[i].fn.store(nullptr);
            ++n;
        }
    }
    return n;
}

bool g_leftOut = false;
bool PluginsLeftOut() { return g_leftOut; }

// plugins_off = myplugin, other: the DLL names (without .dll, any case) not loaded this start
bool PluginOff(const std::wstring& file) {
    std::string stem;  // DLL names are ASCII
    for (wchar_t c : file.substr(file.find_last_of(L'\\') + 1)) stem += static_cast<char>(towlower(c));
    if (stem.size() > 4) stem.resize(stem.size() - 4);
    std::string list = ConfigGet("plugins_off", ""), item;
    for (size_t i = 0; i <= list.size(); ++i) {
        char c = i < list.size() ? list[i] : ',';
        if (c == ',' || c == ' ' || c == ';') {
            if (item == stem) return true;
            item.clear();
        } else {
            item += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
    }
    return false;
}

void LoadPlugins() {
    g_api.gameBuild = GameBuild();
    // the commands first: a TW3SE without a plugins folder (the player's zip) has them too (the loop below
    // returns early when there is no plugin)
    RegisterCommand("version", &CmdVersion, "extender version, interface and game build");
    RegisterCommand("plugins", &CmdPlugins, "list loaded plugins");
    RegisterCommand("plugin", &CmdPlugin, "plugin reload <name>: unload a plugin and load its DLL again");
    RegisterTick(&OnPluginTick, nullptr);
    g_copies = ConfigGet("plugin_reload", "on") != "off";
    if (NoPlugins()) {  // main.cpp: Shift held at the start, or safe_mode = plugins
        Log("plugins: safe mode - none loaded");
        return;
    }
    std::wstring dir = Dir() + L"\\plugins";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Log("plugins: none in %ls", dir.c_str());
        return;
    }
    std::vector<std::wstring> files;
    do {
        std::wstring f = fd.cFileName;
        if (IsCopy(f)) DeleteFileW((dir + L"\\" + f).c_str());  // a copy from the last session
        else files.push_back(dir + L"\\" + f);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    int loaded = 0;
    for (const std::wstring& f : files) {
        if (PluginOff(f)) {
            Log("plugins: %ls left out (plugins_off)", f.substr(f.find_last_of(L'\\') + 1).c_str());
            g_leftOut = true;
            continue;
        }
        PluginRec rec;
        if (!LoadOne(f, rec)) continue;
        g_loaded.push_back(rec.name);
        g_plugins.push_back(rec);
        ++loaded;
    }
    Log("plugins: %d loaded", loaded);
    // the natives of the ones left out answer nothing (the loaded plugins' are queued by now and stay theirs)
    if (g_leftOut) StubModNatives({}, true);
}

}  // namespace tw3se
