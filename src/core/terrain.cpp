// A colour layer over the terrain. The renderer has CRenderCommand_SetTerrainCustomOverlay; the
// game itself never builds one (only its destructors are in the exe, an editor leftover), but its execute is there:
// with data it makes a width x height RGBA texture on the terrain proxy (CRenderProxy_Terrain +0x4720) and uploads
// the pixels (pitch width * 4); with none it takes the layer away. The command, 0x28 bytes in the renderer's command
// ring: vtable, +0x08 the terrain proxy (ref counted at +0x08), +0x10 the pixels {data, u32 bytes} (freed by the
// command), +0x1c width, +0x20 height. Committed with its vtable slot 1, like every render command.
#include "core.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace tw3se {

// The vtable of a class by its RTTI name: type descriptor (name - 0x10) -> complete object locator -> vtable.
// decal.cpp uses it too.
void* VtableOfType(const char* typeName) {
    uintptr_t base = ImageBase();
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* first = IMAGE_FIRST_SECTION(nt);
    size_t len = strlen(typeName) + 1;
    uintptr_t name = 0;
    for (int s = 0; s < nt->FileHeader.NumberOfSections && !name; ++s) {
        IMAGE_SECTION_HEADER& sec = first[s];
        if (memcmp(sec.Name, ".data", 5) != 0) continue;  // type descriptors live in .data
        const char* lo = reinterpret_cast<const char*>(base + sec.VirtualAddress);
        for (size_t i = 0; i + len < sec.Misc.VirtualSize; ++i)
            if (lo[i] == typeName[0] && memcmp(lo + i, typeName, len) == 0) {
                name = reinterpret_cast<uintptr_t>(lo + i);
                break;
            }
    }
    if (!name) return nullptr;
    uint32_t td = static_cast<uint32_t>(name - 0x10 - base);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
        IMAGE_SECTION_HEADER& sec = first[s];
        if (memcmp(sec.Name, ".rdata", 6) != 0) continue;
        uintptr_t lo = base + sec.VirtualAddress, hi = lo + sec.Misc.VirtualSize;
        for (uintptr_t col = lo; col + 24 <= hi; col += 4) {
            const uint32_t* c = reinterpret_cast<const uint32_t*>(col);
            if (c[0] != 1 || c[1] != 0 || c[3] != td || c[5] != static_cast<uint32_t>(col - base)) continue;
            for (uintptr_t at = lo; at + 8 <= hi; at += 8)  // the vtable follows the pointer to its locator
                if (*reinterpret_cast<const uintptr_t*>(at) == col) return reinterpret_cast<void*>(at + 8);
        }
    }
    return nullptr;
}

std::string TerrainOverlay(void* clipMap, uint32_t width, uint32_t height, const uint8_t* rgba) {
    static void* vt = VtableOfType(".?AVCRenderCommand_SetTerrainCustomOverlay@@");
    auto alloc = reinterpret_cast<char* (*)()>(Address("RenderCommandAlloc28"));
    if (!vt || !alloc) return "the terrain overlay command was not found in this build";
    void* proxy = clipMap ? ObjectFieldOfType(clipMap, 0x1c0, ".?AVCRenderProxy_Terrain@@") : nullptr;
    if (!proxy) return "no terrain proxy (no terrain in this world, or not drawn yet)";
    if (rgba && (!width || !height || width > 4096 || height > 4096)) return "width and height from 1 to 4096";
    if (!rgba) {  // off: the engine's own clear command (0x10 bytes: vtable, the proxy with a reference; docs/engine/terrain.md 4)
        static void* clearVt = VtableOfType(".?AVCRenderCommand_ClearTerrainCustomOverlay@@");
        if (!clearVt) return "the terrain overlay's clear command was not found in this build";
        char* cmd = alloc();
        if (!cmd) return "no render command memory";
        memset(cmd, 0, 0x28);
        memcpy(cmd, &clearVt, 8);
        memcpy(cmd + 8, &proxy, 8);
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(static_cast<char*>(proxy) + 8));
        reinterpret_cast<void (*)(void*)>((*reinterpret_cast<void***>(cmd))[1])(cmd);
        return "";
    }
    uint32_t count = width * height;  // the command holds the number of pixels, not of bytes
    auto pixels = static_cast<uint8_t*>(EngineAlloc(count * 4, 4));
    if (!pixels) return "out of memory";
    memcpy(pixels, rgba, count * 4);
    char* cmd = alloc();
    if (!cmd) return "no render command memory";
    memset(cmd, 0, 0x28);
    memcpy(cmd, &vt, 8);
    memcpy(cmd + 8, &proxy, 8);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(static_cast<char*>(proxy) + 8));
    memcpy(cmd + 0x10, &pixels, 8);
    memcpy(cmd + 0x18, &count, 4);
    memcpy(cmd + 0x1c, &width, 4);
    memcpy(cmd + 0x20, &height, 4);
    using Commit_t = void (*)(void*);
    reinterpret_cast<Commit_t>((*reinterpret_cast<void***>(cmd))[1])(cmd);
    return "";
}

// --- the height of the terrain at run time (docs/engine/terrain.md 1-3). A tile's height map of mip 0
// (CTerrainTile::GetLevelSyncHM) is what the clipmap sends the renderer when the camera brings that part into view;
// the collision is a set of PhysX heightfields per tile, swapped for new ones (PhysX3Common_x64.dll exports their
// functions by name). Both last until the tile builds them again (the player leaves the area and comes back).
namespace {

struct Clip {
    void* clip = nullptr;
    uint32_t tileRes = 0, tiles = 0;
    float size = 0, low = 0, high = 0, cornerX = 0, cornerY = 0;
    void** grid = nullptr;
};

template <class T> T Get(const void* base, size_t off) {
    T v{};
    SafeRead(static_cast<const char*>(base) + off, &v, sizeof v);
    return v;
}

uint32_t PropOff(const char* cls, const char* name, uint32_t fallback) {  // a reflected field's offset
    void* c = FindClassByName(cls);
    void* p = c ? FindProperty(c, name) : nullptr;
    return p ? Get<uint32_t>(p, 0x20) : fallback;
}

bool ReadClip(void* clip, Clip& c, std::string& err) {
    c.clip = clip;
    c.tileRes = Get<uint32_t>(clip, PropOff("CClipMap", "tileRes", 0x64));
    c.size = Get<float>(clip, PropOff("CClipMap", "terrainSize", 0x6c));
    c.low = Get<float>(clip, PropOff("CClipMap", "lowestElevation", 0x70));
    c.high = Get<float>(clip, PropOff("CClipMap", "highestElevation", 0x74));
    uint32_t corner = PropOff("CClipMap", "terrainCorner", 0x90);
    c.cornerX = Get<float>(clip, corner);
    c.cornerY = Get<float>(clip, corner + 4);
    c.tiles = Get<uint32_t>(clip, PropOff("CClipMap", "numTilesPerEdge", 0x104));
    c.grid = Get<void**>(clip, FieldOr("CClipMap.terrainGrid", 0xb8));
    if (!c.tileRes || c.tileRes > 8192 || !c.tiles || c.tiles > 1024 || c.size <= 0 || c.high <= c.low || !c.grid) {
        err = "the terrain's numbers do not look right (res " + std::to_string(c.tileRes) + ", tiles " +
              std::to_string(c.tiles) + ")";
        return false;
    }
    return true;
}

void* Tile(const Clip& c, int tx, int ty) {
    if (tx < 0 || ty < 0 || tx >= static_cast<int>(c.tiles) || ty >= static_cast<int>(c.tiles)) return nullptr;
    void* t = Get<void*>(c.grid, (static_cast<size_t>(ty) * c.tiles + tx) * sizeof(void*));
    return t && MemReadable(t, 0x100) ? t : nullptr;
}

using GetHM_t = uint16_t* (*)(void* tile, uint32_t mip);
uint16_t* HeightMapGuarded(GetHM_t f, void* tile) {
    __try {
        return f(tile, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

}  // namespace

// The terrain height at a world point from the tile's height map, and the raw value. spacing 0: texels tileSize/res
// apart, 1: tileSize/(res - 1) (which one the game uses is measured with a ray, docs/engine/terrain.md 1).
std::string TerrainProbe(void* clip, float wx, float wy, int spacing, float* height, uint32_t* raw) {
    Clip c;
    std::string err;
    if (!ReadClip(clip, c, err)) return err;
    auto getHM = reinterpret_cast<GetHM_t>(Address("CTerrainTile::GetLevelSyncHM"));
    if (!getHM) return "the tile height maps are not known for this game build";
    float tileSize = c.size / c.tiles;
    int tx = static_cast<int>((wx - c.cornerX) / tileSize), ty = static_cast<int>((wy - c.cornerY) / tileSize);
    void* tile = Tile(c, tx, ty);
    if (!tile) return "no tile at that point";
    uint16_t* hm = HeightMapGuarded(getHM, tile);
    if (!hm) return "the tile's height map is not there";
    float step = tileSize / (spacing ? c.tileRes - 1 : c.tileRes);
    int i = static_cast<int>((wx - c.cornerX - tx * tileSize) / step + 0.5f);
    int j = static_cast<int>((wy - c.cornerY - ty * tileSize) / step + 0.5f);
    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (i >= static_cast<int>(c.tileRes)) i = c.tileRes - 1;
    if (j >= static_cast<int>(c.tileRes)) j = c.tileRes - 1;
    uint16_t v = Get<uint16_t>(hm, (static_cast<size_t>(j) * c.tileRes + i) * 2);
    *raw = v;
    *height = c.low + v / 65535.0f * (c.high - c.low);
    return "";
}

namespace {

// PhysX 3.3 (statically linked in witcher3.exe): vtable slots of the objects, docs/engine/terrain.md 3. The heightfield
// functions come from PhysX3Common_x64.dll by their exported names.
struct PxTransform {
    float q[4];  // x y z w
    float p[3];
};
struct PxHFGeometry {  // PxHeightFieldGeometry, 0x20 bytes
    uint32_t type;
    uint32_t pad;
    void* hf;
    float heightScale, rowScale, columnScale;
    uint32_t flags;
};
struct PxHFDesc {  // PxHeightFieldDesc, 0x30 bytes
    uint32_t rows, columns, format, pad0;
    uint64_t stride;
    void* samples;
    float thickness, edge;
    uint16_t flags;
    uint8_t pad1[6];
};
static_assert(sizeof(PxHFGeometry) == 0x20 && sizeof(PxHFDesc) == 0x30, "PhysX 3.3 layouts");

struct HfApi {
    uint32_t (*rows)(void*) = nullptr;
    uint32_t (*cols)(void*) = nullptr;
    uint32_t (*save)(void*, void*, uint32_t) = nullptr;
    float (*thickness)(void*) = nullptr;
    float (*edge)(void*) = nullptr;
    uint16_t* (*flags)(void*, uint16_t*) = nullptr;
    void (*release)(void*) = nullptr;
    void* (*create)(void*, const PxHFDesc&) = nullptr;
    bool ok = false;
};
HfApi g_hf;
void LoadHfApi() {
    if (g_hf.ok) return;
    HMODULE m = GetModuleHandleW(L"PhysX3Common_x64.dll");
    if (!m) return;
    auto get = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(m, n)); };
    g_hf.rows = reinterpret_cast<uint32_t (*)(void*)>(get("?getNbRows@HeightField@Gu@physx@@UEBAIXZ"));
    g_hf.cols = reinterpret_cast<uint32_t (*)(void*)>(get("?getNbColumns@HeightField@Gu@physx@@UEBAIXZ"));
    g_hf.save = reinterpret_cast<uint32_t (*)(void*, void*, uint32_t)>(get("?saveCells@HeightField@Gu@physx@@UEBAIPEAXI@Z"));
    g_hf.thickness = reinterpret_cast<float (*)(void*)>(get("?getThickness@HeightField@Gu@physx@@UEBAMXZ"));
    g_hf.edge = reinterpret_cast<float (*)(void*)>(get("?getConvexEdgeThreshold@HeightField@Gu@physx@@UEBAMXZ"));
    g_hf.flags = reinterpret_cast<uint16_t* (*)(void*, uint16_t*)>(
        get("?getFlags@HeightField@Gu@physx@@UEBA?AV?$PxFlags@W4Enum@PxHeightFieldFlag@physx@@G@3@XZ"));
    g_hf.release = reinterpret_cast<void (*)(void*)>(get("?release@HeightField@Gu@physx@@UEAAXXZ"));
    g_hf.create = reinterpret_cast<void* (*)(void*, const PxHFDesc&)>(
        get("?createHeightField@GuMeshFactory@physx@@QEAAPEAVPxHeightField@2@AEBVPxHeightFieldDesc@2@@Z"));
    g_hf.ok = g_hf.rows && g_hf.cols && g_hf.save && g_hf.thickness && g_hf.edge && g_hf.flags && g_hf.release && g_hf.create;
}

template <class F> F Slot(void* obj, int slot) { return reinterpret_cast<F>((*reinterpret_cast<void***>(obj))[slot]); }

bool PxTypeIs(void* obj, const char* name) {  // vtable slot 1: getConcreteTypeName
    __try {
        const char* n = Slot<const char* (*)(void*)>(obj, 1)(obj);
        return n && strcmp(n, name) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The shapes of a static actor that are heightfields (at most `max`), its pose. No C++ objects (SEH).
int HfShapesGuarded(void* actor, void** shapes, int max, PxTransform* actorPose) {
    __try {
        Slot<PxTransform* (*)(void*, PxTransform*)>(actor, 20)(actor, actorPose);
        uint32_t n = Slot<uint32_t (*)(void*)>(actor, 25)(actor);
        if (n > static_cast<uint32_t>(max)) n = max;
        uint32_t got = Slot<uint32_t (*)(void*, void**, uint32_t, uint32_t)>(actor, 26)(actor, shapes, n, 0);
        int k = 0;
        for (uint32_t i = 0; i < got; ++i)
            if (Slot<int (*)(void*)>(shapes[i], 5)(shapes[i]) == 6) shapes[k++] = shapes[i];
        return k;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool ShapeGeometryGuarded(void* shape, PxHFGeometry* g, PxTransform* local) {
    __try {
        if (!Slot<bool (*)(void*, PxHFGeometry*)>(shape, 14)(shape, g)) return false;
        Slot<PxTransform* (*)(void*, PxTransform*)>(shape, 17)(shape, local);
        return g->hf != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// rows, columns, thickness, edge, flags of a heightfield; its samples into `cells` (rows * columns * 4 bytes).
bool HfReadGuarded(void* hf, uint32_t* rows, uint32_t* cols, float* thick, float* edge, uint16_t* flags, void* cells,
                   uint32_t capacity) {
    __try {
        *rows = g_hf.rows(hf);
        *cols = g_hf.cols(hf);
        if (!*rows || !*cols || static_cast<uint64_t>(*rows) * *cols * 4 > capacity) return false;
        *thick = g_hf.thickness(hf);
        *edge = g_hf.edge(hf);
        g_hf.flags(hf, flags);
        return g_hf.save(hf, cells, *rows * *cols * 4) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool HfSwapGuarded(void* shape, PxHFGeometry* g, const PxHFDesc* desc) {  // a new heightfield into the shape
    __try {
        void* factory = *reinterpret_cast<void**>(static_cast<char*>(g->hf) + 0x88);
        void* fresh = factory ? g_hf.create(factory, *desc) : nullptr;
        if (!fresh) return false;
        g->hf = fresh;
        Slot<void (*)(void*, const void*)>(shape, 6)(shape, g);  // the shape takes its own reference
        g_hf.release(fresh);                                        // ours
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Rotate(const float* q, const float* v, float* out) {  // v by the quaternion q (x y z w)
    float t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0])};
    out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}
void ToWorld(const PxTransform& a, const PxTransform& s, const float* local, float* world) {
    float r1[3], r2[3];
    Rotate(s.q, local, r1);
    for (int k = 0; k < 3; ++k) r1[k] += s.p[k];
    Rotate(a.q, r1, r2);
    for (int k = 0; k < 3; ++k) world[k] = r2[k] + a.p[k];
}

bool PutU16(uint16_t* at, uint16_t v) {
    __try {
        *at = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float Falloff(float d, float radius) {  // 1 in the middle, 0 at the edge, smooth
    if (d >= radius) return 0;
    float x = d / radius;
    return (1 - x * x) * (1 - x * x);
}

}  // namespace

// Raises (delta > 0) or lowers the terrain within `radius` metres of (wx, wy), smoothly to the edge: the tiles' height
// maps (what the picture is built from when the camera brings that part into view) and their collision heightfields
// (at once). Main thread. Returns "" and the counts, or the reason.
std::string TerrainRaise(void* clip, float wx, float wy, float radius, float delta, int* texels, int* pieces) {
    *texels = 0;
    *pieces = 0;
    Clip c;
    std::string err;
    if (!ReadClip(clip, c, err)) return err;
    if (!(radius > 0) || radius > 200 || !(delta == delta) || delta > 100 || delta < -100) return "a radius up to 200 m and a change up to 100 m";
    auto getHM = reinterpret_cast<GetHM_t>(Address("CTerrainTile::GetLevelSyncHM"));
    LoadHfApi();
    float tileSize = c.size / c.tiles;
    int tx0 = static_cast<int>((wx - radius - c.cornerX) / tileSize), tx1 = static_cast<int>((wx + radius - c.cornerX) / tileSize);
    int ty0 = static_cast<int>((wy - radius - c.cornerY) / tileSize), ty1 = static_cast<int>((wy + radius - c.cornerY) / tileSize);
    float step = tileSize / c.tileRes;
    float perUnit = 65535.0f / (c.high - c.low);
    uint32_t wrapperOff = FieldOr("CTerrainTile.collisionWrapper", 0xa0);
    std::vector<uint32_t> cells;
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx) {
            void* tile = Tile(c, tx, ty);
            if (!tile) continue;
            if (getHM) {  // the picture: the tile's height map of mip 0
                if (uint16_t* hm = HeightMapGuarded(getHM, tile)) {
                    float ox = c.cornerX + tx * tileSize, oy = c.cornerY + ty * tileSize;
                    for (uint32_t j = 0; j < c.tileRes; ++j)
                        for (uint32_t i = 0; i < c.tileRes; ++i) {
                            float px = ox + i * step, py = oy + j * step;
                            float w = Falloff(std::sqrt((px - wx) * (px - wx) + (py - wy) * (py - wy)), radius);
                            if (w <= 0) continue;
                            uint16_t* v = hm + static_cast<size_t>(j) * c.tileRes + i;
                            float nv = *v + delta * w * perUnit;
                            uint16_t nu = static_cast<uint16_t>(nv < 0 ? 0 : nv > 65535 ? 65535 : nv + 0.5f);
                            if (PutU16(v, nu)) ++*texels;
                        }
                }
            }
            // The collision stays as it is for now: the game ended itself about 25 s after a heightfield swap.
            // Something of the engine likely holds the old heightfield without a reference of its own,
            // and PhysX3Common exports no way to add one. tw3se.config terrain_collision = on tries it anyway.
            if (!g_hf.ok || !ConfigBool("terrain_collision", false)) continue;
            void* wrapper = Get<void*>(tile, wrapperOff);  // the collision: static actors, heightfield shapes
            if (!wrapper || !MemReadable(wrapper, 0x60)) continue;
            char* bodies = Get<char*>(wrapper, 0x50);
            uint16_t nb = Get<uint16_t>(wrapper, 0x58);
            for (uint16_t b = 0; bodies && b < nb && b < 256; ++b) {
                void* actor = Get<void*>(bodies + static_cast<size_t>(b) * 0x38, 0x10);
                if (!actor || !MemReadable(actor, 8) || !PxTypeIs(actor, "PxRigidStatic")) continue;
                void* shapes[16] = {};
                PxTransform pa{};
                int ns = HfShapesGuarded(actor, shapes, 16, &pa);
                for (int s = 0; s < ns; ++s) {
                    PxHFGeometry g{};
                    PxTransform ps{};
                    if (!ShapeGeometryGuarded(shapes[s], &g, &ps) || !(g.heightScale > 0)) continue;
                    uint32_t rows = 0, cols = 0;
                    float thick = 0, edge = 0;
                    uint16_t flags = 0;
                    cells.assign(1 << 20, 0);  // 4 MB: up to 1M samples (a piece is far smaller)
                    if (!HfReadGuarded(g.hf, &rows, &cols, &thick, &edge, &flags, cells.data(), static_cast<uint32_t>(cells.size() * 4)))
                        continue;
                    float up[3], yAxis[3] = {0, 1, 0}, tmp[3];  // the shape's height axis in the world
                    Rotate(ps.q, yAxis, tmp);
                    Rotate(pa.q, tmp, up);
                    if (up[2] < 0.5f) continue;  // not a ground heightfield
                    bool changed = false;
                    for (uint32_t r = 0; r < rows; ++r)
                        for (uint32_t col = 0; col < cols; ++col) {
                            uint32_t& cell = cells[static_cast<size_t>(r) * cols + col];
                            int16_t h = static_cast<int16_t>(cell & 0xffff);
                            float local[3] = {r * g.rowScale, h * g.heightScale, col * g.columnScale}, world[3];
                            ToWorld(pa, ps, local, world);
                            float w = Falloff(std::sqrt((world[0] - wx) * (world[0] - wx) + (world[1] - wy) * (world[1] - wy)), radius);
                            if (w <= 0) continue;
                            float nh = h + delta * w / (g.heightScale * up[2]);
                            int16_t ni = static_cast<int16_t>(nh < -32768 ? -32768 : nh > 32767 ? 32767 : nh + (nh < 0 ? -0.5f : 0.5f));
                            cell = (cell & 0xffff0000u) | static_cast<uint16_t>(ni);
                            changed = true;
                        }
                    if (!changed) continue;
                    PxHFDesc d{};
                    d.rows = rows;
                    d.columns = cols;
                    d.format = 1;
                    d.stride = 4;
                    d.samples = cells.data();
                    d.thickness = thick;
                    d.edge = edge;
                    d.flags = flags;
                    if (HfSwapGuarded(shapes[s], &g, &d)) ++*pieces;
                }
            }
        }
    return "";
}

}  // namespace tw3se
