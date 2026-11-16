// TW3SE core - foliage (docs/FINDINGS_FOLIAGE.md, measured in the game). A tree of the
// foliage is a render instance: a CDynamicFoliageComponent makes one when it attaches, and nothing moves it
// afterwards - the engine has no move command for them (a planted tree stays where it was made). The
// foliage render command dispatcher's send functions are plain functions; these natives call them:
//
//   TW3SE_FoliagePlant(tree, x, y, z, yaw, scale, half, height) : bool   a tree drawn there (no entity, no collision)
//   TW3SE_FoliageRemoveAt(tree, x, y, z, radius) : bool                  planted trees of that kind there taken out
//
// What the engine passes, measured with a read-only hook on its own calls (the hook is gone again):
//   1st  the CFoliageRenderCommandDispatcher - not optional (null faulted). It lives in the world's
//        CFoliageScene at +0x68 (found by searching the scene for the pointer the engine passed).
//   2nd  the tree resource (CSRTBaseTree), its render-side reference (+0x90) resolved - create resolves it itself,
//        a remove does not (an unresolved one faulted): funcFoliageResolveResourceReference does it, idempotent.
//   create: an array descriptor {data, count}, each placement 24 bytes - position, scale, the turn about the up
//           axis as a direction (cos, sin; the engine's own carry e.g. 1.000 -0.531 0.847) - and a world-space box
//           (w = 2 at both corners).
//   remove: the centre (the entity's position) and a radius (the engine uses 0.1 m).
// Seen in the game: a removed tree goes, a created one stands (no collision: no entity). A remove right after its
// create misses the tree - both only queue a record, a later step applies them (round 4): the editor's preview takes
// a tree out once it has stood 0.3 s.
#include "core.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>

namespace tw3se {
namespace {

struct Placement {
    float pos[3];
    float scale;
    float dir[2];  // the turn about the up axis: cos, sin of the yaw
};
struct PlacementArray {
    const Placement* data;
    uint32_t count;
    uint32_t pad;
};

using Create_t = void (*)(void* dispatcher, void* resource, const PlacementArray* placements, const float* bounds);
using RemoveRadius_t = void (*)(void* dispatcher, void* resource, const float* center, float radius);
using Resolve_t = void* (*)(void* resource, void* out);

std::set<void*> g_resolved;  // tree resources resolved once (a resolve takes a reference: once each is enough)

const char* ClassOf(void* obj) {
    void* cls = obj ? GetObjectClass(obj) : nullptr;
    return cls ? GetCNameString(*reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(cls) + 0x2c)) : nullptr;
}

bool IsClassGuarded(void* obj, const char* name) {
    __try {
        const char* c = obj ? ClassOf(obj) : nullptr;
        return c && strcmp(c, name) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The C++ type name of an object with a vtable (MSVC RTTI: vtable[-1] -> complete object locator -> type
// descriptor, its name at +0x10), compared under SEH.
bool IsRttiGuarded(void* obj, const char* name) {
    __try {
        void** vtbl = *reinterpret_cast<void***>(obj);
        const uint32_t* col = reinterpret_cast<const uint32_t*>(vtbl[-1]);
        uintptr_t base = reinterpret_cast<uintptr_t>(col) - col[5];          // pSelf: the locator's own RVA
        return strcmp(reinterpret_cast<const char*>(base + col[3] + 0x10), name) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* ClassGuarded(void* obj) {
    __try {
        return GetObjectClass(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// An object field (property `name` of `obj`) that holds the object or a handle block: whichever reads as `cls`.
void* FieldObject(void* obj, const char* name, const char* cls) {
    void* c = obj ? ClassGuarded(obj) : nullptr;
    void* prop = c ? FindProperty(c, name) : nullptr;
    void* field = prop ? PropertyPtr(obj, prop) : nullptr;
    if (!field) return nullptr;
    void* v = nullptr;
    __try {
        v = *reinterpret_cast<void**>(field);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (v && IsClassGuarded(v, cls)) return v;
    void* h = v ? ResolveHandle(v) : nullptr;
    return h && IsClassGuarded(h, cls) ? h : nullptr;
}

// The dispatcher of the loaded world (its CFoliageScene +0x68), checked by its type - or null.
void* Dispatcher() {
    void* scene = FieldObject(CurrentWorld(), "foliageScene", "CFoliageScene");
    if (!scene) return nullptr;
    void* d = nullptr;
    __try {
        d = *reinterpret_cast<void**>(reinterpret_cast<char*>(scene) + 0x68);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return d && IsRttiGuarded(d, ".?AVCFoliageRenderCommandDispatcher@@") ? d : nullptr;
}

// The render reference comes back in out (with a reference taken). Retail keeps it at +0x90, the editor at +0xa8,
// so the result is checked here instead of the field.
bool ResolveGuarded(Resolve_t fn, void* tree) {
    uint8_t out[16] = {0};
    __try {
        fn(tree, out);
        return *reinterpret_cast<void**>(out) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The tree resource ready for the send functions: a CSRTBaseTree with its render reference resolved, or null.
void* ReadyTree(void* tree) {
    if (!tree || !IsClassGuarded(tree, "CSRTBaseTree")) return nullptr;
    if (g_resolved.count(tree)) return tree;
    auto fn = reinterpret_cast<Resolve_t>(Address("funcFoliageResolveResourceReference"));
    if (!fn || !ResolveGuarded(fn, tree)) {
        Log("foliage: the tree resource %p could not be made ready", tree);
        return nullptr;
    }
    g_resolved.insert(tree);
    return tree;
}

bool CreateGuarded(Create_t fn, void* d, void* tree, const PlacementArray* arr, const float* bounds) {
    __try {
        fn(d, tree, arr, bounds);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RemoveGuarded(RemoveRadius_t fn, void* d, void* tree, const float* center, float radius) {
    __try {
        fn(d, tree, center, radius);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A tree drawn at pos (yaw degrees, scale) -> sent.
bool Plant(void* tree, const float pos[3], float yaw, float scale, float half, float height) {
    auto fn = reinterpret_cast<Create_t>(Address("funcFoliageSendCreateSpeedTreeDynamicInstances"));
    void* d = Dispatcher();
    void* ready = ReadyTree(tree);
    if (!fn || !d || !ready || scale <= 0 || half <= 0 || height <= 0) {
        Log("foliage: plant not sent (fn=%p dispatcher=%p tree=%p)", reinterpret_cast<void*>(fn), d, tree);
        return false;
    }
    float a = yaw * 3.14159265f / 180.0f;
    Placement p{{pos[0], pos[1], pos[2]}, scale, {cosf(a), sinf(a)}};
    PlacementArray arr{&p, 1, 0};
    float box[8] = {pos[0] - half, pos[1] - half, pos[2] - 1.0f, 2.0f, pos[0] + half, pos[1] + half,
                    pos[2] + height, 2.0f};
    bool ok = CreateGuarded(fn, d, ready, &arr, box);
    if (!ok) Log("foliage: plant faulted (tree %p at %.2f %.2f %.2f)", ready, pos[0], pos[1], pos[2]);
    return ok;
}

// Planted trees of this kind within radius of centre taken out -> sent.
bool Remove(void* tree, const float centre[3], float radius) {
    auto fn = reinterpret_cast<RemoveRadius_t>(Address("funcFoliageSendRemoveSpeedTreeDynamicInstancesRadius"));
    void* d = Dispatcher();
    void* ready = ReadyTree(tree);
    if (!fn || !d || !ready || radius <= 0 || radius > 50) {
        Log("foliage: remove not sent (fn=%p dispatcher=%p tree=%p r=%.2f)", reinterpret_cast<void*>(fn), d, tree,
            radius);
        return false;
    }
    float c[4] = {centre[0], centre[1], centre[2], 1.0f};
    bool ok = RemoveGuarded(fn, d, ready, c, radius);
    if (!ok) Log("foliage: remove faulted (tree %p at %.2f %.2f %.2f)", ready, c[0], c[1], c[2]);
    return ok;
}

float ParamFloat(void* frame) {
    uint8_t b[16] = {0};
    ReadScriptParam(frame, b);
    float f = 0;
    memcpy(&f, b, sizeof f);
    return f;
}

void* ParamObject(void* frame) {
    uint8_t b[16] = {0};
    ReadScriptParam(frame, b);
    return ResolveHandle(*reinterpret_cast<void**>(b));
}

void Return(void* result, bool ok) {
    if (result) *reinterpret_cast<bool*>(result) = ok;
}

// TW3SE_FoliagePlant(tree : CResource, x, y, z, yaw, scale, half, height : float) : bool
void Tw3FoliagePlant(void*, void* frame, void* result) {
    void* tree = ParamObject(frame);
    float v[7];
    for (float& f : v) f = ParamFloat(frame);
    EndScriptParams(frame);
    Return(result, Plant(tree, v, v[3], v[4], v[5], v[6]));
}

// TW3SE_FoliageRemoveAt(tree : CResource, x, y, z, radius : float) : bool
void Tw3FoliageRemoveAt(void*, void* frame, void* result) {
    void* tree = ParamObject(frame);
    float v[4];
    for (float& f : v) f = ParamFloat(frame);
    EndScriptParams(frame);
    Return(result, Remove(tree, v, v[3]));
}

}  // namespace

void InstallFoliage() {
    QueueNative("TW3SE_FoliagePlant", &Tw3FoliagePlant, 8);
    QueueNative("TW3SE_FoliageRemoveAt", &Tw3FoliageRemoveAt, 5);
}

}  // namespace tw3se
