// TW3SE core - dynamic decals (docs/FINDINGS_DECAL.md). The engine's own way, as its footprints and wound
// decals go: SDynamicDecalInitParameters (0x100 bytes, filled from its init function's defaults), the renderer makes
// a CRenderDynamicDecal from it (IRender slot), a render command puts it into the world's render scene. On the
// whole scene (AddDynamicDecalToScene: every surface in the box, skinned figures too) or on given render proxies
// only (AddDynamicDecalToSceneForProxies, the wound's way: one figure). A decal on a skinned mesh is skinned with it.
//
// Init parameters (offsets from the start; all read in the engine's footprint, wound and particle code):
//   +0x00 diffuse render texture   +0x08 normal render texture (null: the scene's normals)
//   +0x10 colour (RGBA bytes)      +0x14 specular colour   +0x18 specular scale  +0x1c specular base  +0x20 specularity
//   +0x30 origin (x, y, z, 1)      +0x40 projection direction (x, y, z, 0)       +0x50 up in the picture's plane
//   +0x60 width  +0x64 height      +0x68 near (along the direction, negative = behind the origin)  +0x6c far
//   +0x70 depth fade power  +0x74 normal fade bias  +0x78 normal fade scale  +0x7d double sided (byte)
//   +0x80 a 4x4 matrix over the vectors above (identity; the wound puts its entity's here)
//   +0xc0 only on static geometry (byte)  +0xc4 projection (0 ortho, 1 sphere)  +0xc8 lifetime (+inf: until removed)
//   +0xcc spawn priority  +0xd0 fade-out time  +0xd4 fade-in time  +0xd8 start scale  +0xdc scale time
//   +0xe0 auto hide distance (< 0: from the decal's size)   +0xf0 atlas cell {scale u, scale v, offset u, offset v}
// The CRenderDynamicDecal keeps a copy of the parameters at +0x40 (its colour at +0x50 ...).
//
// Render commands (0x28 bytes from the renderer's command ring is enough for each, committed with vtable slot 1):
//   Add          {vtable, scene, decal, +0x18 byte 1}            scene and decal ref counted at +0x08
//   ForProxies   {vtable, scene, decal, proxies*, u32 count}     each proxy too; the array is freed by the command
//   Remove       {vtable, scene, decal}                          a decal no longer in the scene is skipped; scene
//                                                                null: nothing is removed, the command lets the decal go
#include "core.h"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace tw3se {
namespace {

bool Plausible(const void* p) {
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v > 0x10000 && v < 0x7fffffffffffULL && (v & 7) == 0;
}

template <class T> void Put(uint8_t* b, size_t off, T v) { memcpy(b + off, &v, sizeof v); }

void AddRef(void* o) { InterlockedIncrement(reinterpret_cast<volatile LONG*>(static_cast<char*>(o) + 8)); }
void Release(void* o) {  // IRenderObject slot 2
    using Release_t = void (*)(void*);
    reinterpret_cast<Release_t>((*reinterpret_cast<void***>(o))[2])(o);
}
void Commit(char* cmd) {
    using Commit_t = void (*)(void*);
    reinterpret_cast<Commit_t>((*reinterpret_cast<void***>(cmd))[1])(cmd);
}

struct Decal {
    uint32_t id;
    void* decal;    // our reference
    void* scene;    // the render scene it went into
    void* owner;    // the first entity it was put on, or null
    ULONGLONG ends; // GetTickCount64 after which the renderer has dropped it (0: until removed)
};
std::vector<Decal> g_decals;
uint32_t g_nextId = 1;

void* ActiveWorld() {
    void* game = GetTheGame();
    void* cls = game ? GetObjectClass(game) : nullptr;
    void* prop = cls ? FindProperty(cls, "activeWorld") : nullptr;
    void* p = prop ? PropertyPtr(game, prop) : nullptr;
    void* block = nullptr;
    void* world = nullptr;
    if (!p || !SafeRead(p, &block, 8) || !Plausible(block)) return nullptr;
    if (!SafeRead(static_cast<char*>(block) + 8, &world, 8) || !Plausible(world)) return nullptr;
    return world;
}

void* RenderScene(void* world) {
    uint32_t off = static_cast<uint32_t>(Address("CWorld.renderScene"));
    void* scene = nullptr;
    if (!world || !off || !SafeRead(static_cast<char*>(world) + off, &scene, 8) || !Plausible(scene)) return nullptr;
    return ObjectTypeName(scene).find("RenderScene") != std::string::npos ? scene : nullptr;
}

void* RenderTexture(void* texture) {  // a CBitmapTexture's render resource (borrowed), or null
    uint32_t slot = static_cast<uint32_t>(Address("CBitmapTexture.GetRenderResourceSlot"));
    if (!Plausible(texture) || !slot) return nullptr;
    static void* bitmap = FindClassByName("CBitmapTexture");
    void* cls = GetObjectClass(texture);
    for (int i = 0; cls && cls != bitmap && i < 16; ++i) cls = ApiClassParent(cls);
    if (!cls) return nullptr;
    using Get_t = void* (*)(void*);
    void* r = reinterpret_cast<Get_t>((*reinterpret_cast<void***>(texture))[slot / 8])(texture);
    return Plausible(r) ? r : nullptr;
}

// The render proxy of a drawable component (null: none, or not a drawable)
void* ProxyOf(void* c, void* drawable, uint32_t proxyOff) {
    void* cls = GetObjectClass(c);
    for (int k = 0; cls && cls != drawable && k < 16; ++k) cls = ApiClassParent(cls);
    if (!cls) return nullptr;
    void* proxy = nullptr;
    if (!SafeRead(static_cast<char*>(c) + proxyOff, &proxy, 8) || !Plausible(proxy)) return nullptr;
    if (ObjectTypeName(proxy).rfind(".?AVCRenderProxy_", 0) != 0) return nullptr;
    return proxy;
}

// The render proxies of an entity's drawable components (the wound's way: the component keeps its proxy in a field).
// A drawable component given in place of an entity: its own proxy only (a decal on one piece of
// clothing, not the skin under it)
void Proxies(void* entity, std::vector<void*>& out) {
    static void* drawable = FindClassByName("CDrawableComponent");
    uint32_t proxyOff = static_cast<uint32_t>(Address("CDrawableComponent.renderProxy"));
    if (drawable && proxyOff) {
        void* cls = GetObjectClass(entity);
        for (int k = 0; cls && cls != drawable && k < 16; ++k) cls = ApiClassParent(cls);
        if (cls) {
            if (void* px = ProxyOf(entity, drawable, proxyOff)) out.push_back(px);
            return;
        }
    }
    char* arr = nullptr;
    uint32_t n = 0;
    const uint32_t lo = FieldOr("CEntity.components", 0xf0), co = FieldOr("CEntity.componentCount", 0xf8);
    if (!drawable || !proxyOff || !SafeRead(static_cast<char*>(entity) + lo, &arr, 8) ||
        !SafeRead(static_cast<char*>(entity) + co, &n, 4) || !Plausible(arr) || n > 4096)
        return;
    for (uint32_t i = 0; i < n; ++i) {
        void* c = nullptr;
        if (!SafeRead(arr + i * 8, &c, 8) || !Plausible(c)) continue;
        if (void* px = ProxyOf(c, drawable, proxyOff)) out.push_back(px);
    }
}

void Normalize(float* v) {
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f)
        for (int i = 0; i < 3; ++i) v[i] /= l;
}

// Our reference goes to a Remove command: the render thread takes the decal out of the scene (scene null: only lets
// it go) and frees it there, never in the middle of a frame (the wound's way).
void Let(void* decal, void* scene) {
    static void* vtRemove = VtableOfType(".?AVCRenderCommand_RemoveDynamicDecalFromScene@@");
    auto alloc = reinterpret_cast<char* (*)()>(Address("RenderCommandAlloc28"));
    char* cmd = vtRemove && alloc ? alloc() : nullptr;
    if (!cmd) {
        Release(decal);
        return;
    }
    memset(cmd, 0, 0x28);
    memcpy(cmd, &vtRemove, 8);
    memcpy(cmd + 8, &scene, 8);
    memcpy(cmd + 0x10, &decal, 8);
    if (scene) AddRef(scene);
    Commit(cmd);
}

// The decals the renderer has dropped by their lifetime: our references go back.
void Prune() {
    ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < g_decals.size();) {
        if (g_decals[i].ends && now > g_decals[i].ends) {
            Let(g_decals[i].decal, nullptr);
            g_decals.erase(g_decals.begin() + i);
        } else {
            ++i;
        }
    }
}

bool RemoveAt(size_t i, void* scene) {
    Decal d = g_decals[i];
    g_decals.erase(g_decals.begin() + i);
    Let(d.decal, scene && scene == d.scene ? scene : nullptr);
    return true;
}

}  // namespace

DecalDesc DecalDefaults() {
    DecalDesc d{};
    d.dir[2] = -1.0f;
    d.up[1] = 1.0f;
    d.width = d.height = 0.2f;
    d.depth = 0.2f;
    d.color = d.specularColor = 0xffffffff;
    d.specularity = -1.0f;  // the engine's defaults: specularity -1, scale 0, base 1
    d.specularScale = 0.0f;
    d.specularBase = 1.0f;
    d.depthFadePower = 1.0f;
    d.normalFadeBias = 0.0f;
    d.normalFadeScale = 1.0f;
    d.autoHideDistance = -1.0f;
    d.uv[0] = d.uv[1] = d.uv[2] = d.uv[3] = 0.5f;
    return d;
}

uint32_t DecalSpawn(const DecalDesc& d, void* const* entities, int entityCount, std::string& err) {
    if (!OnMainThread()) return err = "decals are made on the main thread", 0;
    auto init = reinterpret_cast<void* (*)(void*)>(Address("SDynamicDecalInitParameters::Init"));
    void** renderVar = reinterpret_cast<void**>(Address("GRender"));
    uint32_t createSlot = static_cast<uint32_t>(Address("IRender.CreateDynamicDecalSlot"));
    auto alloc = reinterpret_cast<char* (*)()>(Address("RenderCommandAlloc28"));
    static void* vtAdd = VtableOfType(".?AVCRenderCommand_AddDynamicDecalToScene@@");
    static void* vtFor = VtableOfType(".?AVCRenderCommand_AddDynamicDecalToSceneForProxies@@");
    if (!init || !renderVar || !createSlot || !alloc || !vtAdd || !vtFor)
        return err = "dynamic decals were not found in this build", 0;
    void* render = nullptr;
    if (!SafeRead(renderVar, &render, 8) || !Plausible(render)) return err = "no renderer", 0;
    void* scene = RenderScene(ActiveWorld());
    if (!scene) return err = "no world (main menu or loading)", 0;
    void* diffuse = RenderTexture(d.diffuse);
    if (!diffuse) return err = "the diffuse texture is missing or not loaded", 0;
    void* normal = d.normal ? RenderTexture(d.normal) : nullptr;
    if (d.normal && !normal) return err = "the normal texture is not loaded", 0;
    if (!(d.width > 0 && d.height > 0 && d.depth > 0)) return err = "width, height and depth have to be above 0", 0;

    std::vector<void*> proxies;
    for (int i = 0; entities && i < entityCount; ++i)
        if (Plausible(entities[i])) Proxies(entities[i], proxies);
    if (entities && entityCount > 0 && proxies.empty()) return err = "the entities have no drawn meshes", 0;
    static int logged = 0;
    if (!proxies.empty() && logged < 40) {  // which proxies the ForProxies command will keep (their slot 0x80 says)
        ++logged;
        std::string line;
        for (void* px : proxies) {
            using Ok_t = bool (*)(void*);
            bool ok = reinterpret_cast<Ok_t>((*reinterpret_cast<void***>(px))[0x80 / 8])(px);
            line += ObjectTypeName(px).substr(4) + (ok ? "+ " : "- ");
        }
        Log("decal: %zu proxies: %s", proxies.size(), line.c_str());
    }

    // direction and up: unit length, up at a right angle to the direction
    float dir[3] = {d.dir[0], d.dir[1], d.dir[2]}, up[3] = {d.up[0], d.up[1], d.up[2]};
    Normalize(dir);
    if (std::fabs(dir[0]) + std::fabs(dir[1]) + std::fabs(dir[2]) < 0.5f) return err = "the direction is zero", 0;
    float k = up[0] * dir[0] + up[1] * dir[1] + up[2] * dir[2];
    for (int i = 0; i < 3; ++i) up[i] -= k * dir[i];
    if (up[0] * up[0] + up[1] * up[1] + up[2] * up[2] < 1e-6f) {  // up along the direction: any other axis
        float alt[3] = {0, 0, 1};
        if (std::fabs(dir[2]) > 0.9f) alt[2] = 0, alt[1] = 1;
        k = alt[0] * dir[0] + alt[1] * dir[1] + alt[2] * dir[2];
        for (int i = 0; i < 3; ++i) up[i] = alt[i] - k * dir[i];
    }
    Normalize(up);

    alignas(16) uint8_t p[0x100];
    memset(p, 0, sizeof p);
    init(p);
    Put(p, 0x00, diffuse);
    Put(p, 0x08, normal);
    Put(p, 0x10, d.color);
    Put(p, 0x14, d.specularColor);
    Put(p, 0x18, d.specularScale);
    Put(p, 0x1c, d.specularBase);
    Put(p, 0x20, d.specularity);
    const float origin[4] = {d.pos[0], d.pos[1], d.pos[2], 1.0f}, front[4] = {dir[0], dir[1], dir[2], 0.0f},
                upv[4] = {up[0], up[1], up[2], 0.0f};
    memcpy(p + 0x30, origin, 16);
    memcpy(p + 0x40, front, 16);
    memcpy(p + 0x50, upv, 16);
    Put(p, 0x60, d.width);
    Put(p, 0x64, d.height);
    Put(p, 0x68, -d.depth * 0.5f);
    Put(p, 0x6c, d.depth * 0.5f);
    Put(p, 0x70, d.depthFadePower);
    Put(p, 0x74, d.normalFadeBias);
    Put(p, 0x78, d.normalFadeScale);
    Put<uint8_t>(p, 0x7d, d.doubleSided ? 1 : 0);
    Put<uint8_t>(p, 0xc0, d.staticOnly ? 1 : 0);
    Put<int32_t>(p, 0xc4, d.projection ? 1 : 0);
    Put(p, 0xc8, d.lifetime > 0 ? d.lifetime : std::numeric_limits<float>::infinity());
    Put<int32_t>(p, 0xcc, d.priority ? 1 : 0);
    Put(p, 0xd0, d.fadeOut > 0 ? d.fadeOut : 0.0f);
    Put(p, 0xd4, d.fadeIn > 0 ? d.fadeIn : 0.0f);
    Put(p, 0xe0, d.autoHideDistance);
    if (d.raw7c >= 0) Put<uint8_t>(p, 0x7c, static_cast<uint8_t>(d.raw7c));  // meaning open (FINDINGS_DECAL.md)
    if (d.raw7e >= 0) Put<uint8_t>(p, 0x7e, static_cast<uint8_t>(d.raw7e));
    memcpy(p + 0xf0, d.uv, 16);

    using Create_t = void* (*)(void*, const void*);
    void* decal = reinterpret_cast<Create_t>((*reinterpret_cast<void***>(render))[createSlot / 8])(render, p);
    if (!Plausible(decal)) return err = "the renderer made no decal", 0;

    char* cmd = alloc();
    if (!cmd) {
        Release(decal);
        return err = "no render command memory", 0;
    }
    memset(cmd, 0, 0x28);
    if (proxies.empty()) {
        memcpy(cmd, &vtAdd, 8);
        memcpy(cmd + 8, &scene, 8);
        memcpy(cmd + 0x10, &decal, 8);
        cmd[0x18] = d.staticOnly ? 1 : 0;  // the scene's queue byte: only static geometry (the footprints set 1)
    } else {
        void** list = static_cast<void**>(EngineAlloc(proxies.size() * 8, 8));
        if (!list) {
            Release(decal);
            return err = "out of memory", 0;
        }
        memcpy(list, proxies.data(), proxies.size() * 8);
        uint32_t n = static_cast<uint32_t>(proxies.size());
        memcpy(cmd, &vtFor, 8);
        memcpy(cmd + 8, &scene, 8);
        memcpy(cmd + 0x10, &decal, 8);
        memcpy(cmd + 0x18, &list, 8);
        memcpy(cmd + 0x20, &n, 4);
        for (void* px : proxies) AddRef(px);
    }
    AddRef(scene);
    AddRef(decal);
    Commit(cmd);

    Prune();
    Decal e{};
    e.id = g_nextId++;
    if (!g_nextId) g_nextId = 1;
    e.decal = decal;  // the reference the renderer gave us
    e.scene = scene;
    e.owner = entities && entityCount > 0 ? entities[0] : nullptr;
    if (d.lifetime > 0)
        e.ends = GetTickCount64() + static_cast<ULONGLONG>((d.lifetime + (d.fadeOut > 0 ? d.fadeOut : 0) + 5) * 1000);
    g_decals.push_back(e);
    return e.id;
}

bool DecalRemove(uint32_t id) {
    for (size_t i = 0; i < g_decals.size(); ++i)
        if (g_decals[i].id == id) return RemoveAt(i, RenderScene(ActiveWorld()));
    return false;
}

int DecalClear(void* entity) {
    void* scene = RenderScene(ActiveWorld());
    int n = 0;
    for (size_t i = g_decals.size(); i-- > 0;)
        if (!entity || g_decals[i].owner == entity) {
            RemoveAt(i, scene);
            ++n;
        }
    return n;
}

int DecalCount() {
    Prune();
    return static_cast<int>(g_decals.size());
}

// The decal's copy of its parameters: the renderer reads colour and gloss from there when it draws (written from the
// main thread while the render thread may read: one value is either old or new).
bool DecalSetColor(uint32_t id, uint32_t color, uint32_t specularColor, float specularity, float specularScale,
                   float specularBase) {
    for (const Decal& d : g_decals)
        if (d.id == id) {
            uint8_t* b = static_cast<uint8_t*>(d.decal) + 0x40;
            Put(b, 0x10, color);
            Put(b, 0x14, specularColor);
            Put(b, 0x18, specularScale);
            Put(b, 0x1c, specularBase);
            Put(b, 0x20, specularity);
            return true;
        }
    return false;
}

// How many dynamic decals one mesh takes (the engine: 2). The limit is the immediate of the target's check (an imm8,
// so 1 to 127); the render thread reads it when a decal lands, one byte written whole. Returns the limit before, or
// -1 when the check was not found in this build. n <= 0 only reads.
int DecalPerMesh(int n) {
    uint8_t* at = reinterpret_cast<uint8_t*>(Address("DecalTarget::LimitCheck"));
    if (!at) return -1;
    uint8_t* imm = at + 2;
    int before = *imm;
    if (n <= 0) return before;
    if (n > 127) n = 127;
    DWORD old = 0;
    if (!VirtualProtect(imm, 1, PAGE_EXECUTE_READWRITE, &old)) return -1;
    *reinterpret_cast<volatile uint8_t*>(imm) = static_cast<uint8_t>(n);
    VirtualProtect(imm, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), imm, 1);
    Log("decal: a mesh takes %d dynamic decals (was %d)", n, before);
    return before;
}

// A texture by its depot path, loaded once and held for the session (the game's LoadResource; a few pictures)
void* DecalTexture(const char* path, std::string& err) {
    static std::vector<std::pair<std::string, void*>> cache;
    std::string key = path ? path : "";
    for (char& c : key) c = c == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (key.empty()) return err = "no texture path", nullptr;
    // after a save was loaded (another world) a cached texture can be gone and another one lie at its address with
    // its block still pointing back (the decals would come out as squares of other pictures). A hit counts only when the
    // object there still has this path.
    auto pathOf = [](void* obj) {
        TW3SE_Value r{};
        std::string p;
        if (ApiValueCall(obj, "GetPath", nullptr, 0, &r, 1) >= 1 && r.type == TW3SE_STRING && r.v.s) p = r.v.s;
        for (char& c : p) c = c == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return p;
    };
    for (auto& [k, obj] : cache) {
        void* block = nullptr;
        void* back = nullptr;
        if (k == key && SafeRead(static_cast<char*>(obj) + 8, &block, 8) && Plausible(block) &&
            SafeRead(static_cast<char*>(block) + 8, &back, 8) && back == obj && pathOf(obj) == key)
            return obj;
    }
    TW3SE_Value args[2] = {};
    args[0].type = TW3SE_STRING;
    args[0].v.s = key.c_str();
    args[1].type = TW3SE_BOOL;
    args[1].v.i = 1;
    TW3SE_Value r{};
    if (ApiValueCall(nullptr, "LoadResource", args, 2, &r, 1) < 1 || r.type != TW3SE_OBJECT || !Plausible(r.v.object))
        return err = "texture not found: " + key, nullptr;
    void* obj = r.v.object;
    void* block = nullptr;
    if (SafeRead(static_cast<char*>(obj) + 8, &block, 8) && Plausible(block))
        InterlockedIncrement(static_cast<volatile LONG*>(block));  // held: the cache keeps it
    for (auto& e : cache)
        if (e.first == key) return e.second = obj;
    cache.emplace_back(key, obj);
    return obj;
}

void DecalsGameLoaded() {
    for (const Decal& d : g_decals) Let(d.decal, nullptr);  // the old world's scene went with them
    g_decals.clear();
}

void* BitmapRenderTexture(void* texture) { return RenderTexture(texture); }

}  // namespace tw3se
