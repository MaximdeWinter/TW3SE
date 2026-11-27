// TW3SE core - the merged geometry of the world (CSectorData). Buildings, walls, rocks, their collisions, decals and
// lights are cooked out of their entities into one packed block per layer (CLayer+0x98; resources at +0x10, the
// object list at +0x28: 24-byte entries {type, flags, radius, offset, position}, type 1 mesh, 2 collision, 3 decal)
// and streamed in as the player comes near. Measured: objects moved away before their layer attaches never
// stream in (the main square's buildings gone, walked through). Removing single buildings this way, switched live
// by quest facts, is the next step; until then this file holds the switch for the precomputed visibility.
#include "core.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tw3se {
namespace {

bool Plausible(const void* p) {
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v > 0x10000 && v < 0x7fffffffffffULL && (v & 7) == 0;
}

bool IsType(void* obj, const char* want) {  // the object's MSVC RTTI type name is `want`
    char name[64] = {};
    uintptr_t vt = 0, col = 0;
    uint32_t tdRva = 0;
    if (!Plausible(obj) || !SafeRead(obj, &vt, 8) || vt < ImageBase() || !SafeRead(reinterpret_cast<void*>(vt - 8), &col, 8) ||
        !SafeRead(reinterpret_cast<void*>(col + 12), &tdRva, 4) ||
        !SafeRead(reinterpret_cast<void*>(ImageBase() + tdRva + 0x10), name, sizeof name - 1))
        return false;
    return strcmp(name, want) == 0;
}

bool WriteGuarded(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (1) {
        return false;
    }
}

// umbra [on|off]: the precomputed visibility (Umbra, the [Rendering] UseUmbraOcclusion setting). It is baked with
// the world and keeps hiding what stands behind geometry that is no longer drawn; off, everything in view is drawn,
// at a clear cost in frame time. For looking into a problem, not for play.
void CmdUmbra(const char* arg, char* reply, uint32_t size) {
    auto flag = reinterpret_cast<volatile uint8_t*>(Address("GUseUmbraOcclusion"));
    if (!flag) {
        strncpy_s(reply, size, "error the Umbra switch is not known in this build", _TRUNCATE);
        return;
    }
    if (arg && _stricmp(arg, "off") == 0) *flag = 0;
    else if (arg && _stricmp(arg, "on") == 0) *flag = 1;
    _snprintf_s(reply, size, _TRUNCATE, "umbra %s", *flag ? "on" : "off");
}

}  // namespace

// The objects of a CSectorData block (+0x28 {entries, u32 count}): 24 bytes each, {u8 type, u8 flags, u16 radius,
// u64 offset into the packed data (unaligned, +4), 3 floats position (+12)} (docs/engine/world.md).
int SectorObjects(void* sector, std::vector<SectorObject>* out) {
    char* list = nullptr;
    uint32_t n = 0;
    if (!SafeRead(static_cast<char*>(sector) + 0x28, &list, 8) || !SafeRead(static_cast<char*>(sector) + 0x30, &n, 4) ||
        !list || n > 1000000)
        return -1;
    if (out) {
        out->resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t e[24];
            if (!SafeRead(list + i * 24, e, 24)) return -1;
            (*out)[i].type = e[0];
            (*out)[i].flags = e[1];
            memcpy(&(*out)[i].radius, e + 2, 2);
            memcpy((*out)[i].pos, e + 12, 12);
        }
    }
    return static_cast<int>(n);
}
// The world's merged sector data: one CSectorDataMerged with the merged meshes of every layer of the world (the
// houses, walls and rocks of a city; the layers' own blocks hold a quest's or a DLC's). Not an RTTI object of the
// engine's: found by its MSVC type two pointers from the world (5.01: CWorld+0x138 points at the sector streaming,
// the qword before what it points at is the block). The largest block found wins.
void* WorldSectorData(void* world) {
    static void* cachedWorld = nullptr;
    static void* cached = nullptr;
    if (world == cachedWorld && cached) return cached;
    void* best = nullptr;
    uint32_t bestCount = 0;
    for (size_t off = 0; off < 0x800; off += 8) {
        char* p = nullptr;
        if (!SafeRead(static_cast<char*>(world) + off, &p, 8) || !Plausible(p)) continue;
        for (int k = -4; k < 12; ++k) {
            void* q = nullptr;
            if (!SafeRead(p + k * 8, &q, 8) || !IsType(q, ".?AVCSectorDataMerged@@")) continue;
            uint32_t n = 0;
            if (SafeRead(static_cast<char*>(q) + 0x30, &n, 4) && n > bestCount && n < 10000000) best = q, bestCount = n;
        }
    }
    if (best) cachedWorld = world, cached = best;
    return best;
}

// The meshes of a block: a packed mesh is 12 floats (its three axes as rows, then its position), u32, u32, the
// index of its resource, u32; a resource is 32 bytes, {6 floats box in the mesh's own space, u64 path hash}
// (FNV-1a 64 over the depot path). Measured 06.11. on vip_door_broken.w2l and the world's block. With `center`,
// only the meshes whose box in the world lies within `radius` of it; with `only` (an index from 0), that one alone.
int SectorMeshes(void* sector, std::vector<SectorMesh>* out, const float* center, float radius, int64_t only) {
    char *res = nullptr, *data = nullptr, *list = nullptr;
    uint32_t nres = 0, ndata = 0, n = 0;
    char* s = static_cast<char*>(sector);
    if (!SafeRead(s + 0x10, &res, 8) || !SafeRead(s + 0x18, &nres, 4) || !SafeRead(s + 0x1c, &data, 8) ||
        !SafeRead(s + 0x24, &ndata, 4) || !SafeRead(s + 0x28, &list, 8) || !SafeRead(s + 0x30, &n, 4) || !list ||
        !data || !res || n > 1000000 || nres > 1000000)
        return -1;
    out->clear();
    for (uint32_t i = only >= 0 ? static_cast<uint32_t>(only) : 0; i < n && (only < 0 || i == only); ++i) {
        uint8_t e[24];
        if (!SafeRead(list + static_cast<size_t>(i) * 24, e, 24) || e[0] != 1) continue;
        // {u32 offset, u32 group}: the world's block numbers the group (from 1) each object came from, a layer's has 0
        uint32_t off = 0, group = 0;
        memcpy(&off, e + 4, 4);
        memcpy(&group, e + 8, 4);
        if (static_cast<uint64_t>(off) + 64 > ndata) continue;
        if (center) {  // the pivot first: no mesh of the game reaches 500 m from its own
            float pos[3];
            memcpy(pos, e + 12, 12);
            float dx = pos[0] - center[0], dy = pos[1] - center[1];
            if (dx * dx + dy * dy > (radius + 500) * (radius + 500)) continue;
        }
        SectorMesh m{};
        m.index = i;
        m.group = group;
        uint32_t tail[4];
        if (!SafeRead(data + off, m.matrix, 48) || !SafeRead(data + off + 48, tail, 16) || tail[2] >= nres) continue;
        m.resource = tail[2];
        uint8_t r[32];
        if (!SafeRead(res + static_cast<size_t>(m.resource) * 32, r, 32)) continue;
        memcpy(m.box, r, 24);
        memcpy(&m.hash, r + 24, 8);
        for (int k = 0; k < 3; ++k) m.min[k] = 1e30f, m.max[k] = -1e30f;
        for (int c = 0; c < 8; ++c) {
            float l[3] = {m.box[(c & 1) ? 3 : 0], m.box[(c & 2) ? 4 : 1], m.box[(c & 4) ? 5 : 2]};
            for (int k = 0; k < 3; ++k) {
                float w = m.matrix[9 + k] + l[0] * m.matrix[k] + l[1] * m.matrix[3 + k] + l[2] * m.matrix[6 + k];
                if (w < m.min[k]) m.min[k] = w;
                if (w > m.max[k]) m.max[k] = w;
            }
        }
        if (center) {
            float d2 = 0;
            for (int k = 0; k < 3; ++k) {
                float d = center[k] < m.min[k] ? m.min[k] - center[k] : center[k] > m.max[k] ? center[k] - m.max[k] : 0;
                d2 += d * d;
            }
            if (d2 > radius * radius) continue;
        }
        uint8_t next = 0;
        m.collision = i + 1 < n && SafeRead(list + static_cast<size_t>(i + 1) * 24, &next, 1) && next == 2;
        out->push_back(m);
    }
    return static_cast<int>(out->size());
}
namespace {
std::mutex g_meshPathMx;
std::unordered_map<uint64_t, std::string> g_meshPaths;
bool g_meshPathsRead = false;

uint64_t Fnv64(const char* s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; ++s) h = (h ^ static_cast<uint8_t>(tolower(static_cast<unsigned char>(*s)))) * 0x100000001b3ull;
    return h;
}

void ReadBundleMeshes(const std::wstring& file) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    uint8_t head[0x20] = {};
    DWORD got = 0;
    uint32_t size = 0;
    if (ReadFile(h, head, sizeof head, &got, nullptr) && got == sizeof head && memcmp(head, "POTATO70", 8) == 0) {
        memcpy(&size, head + 0x10, 4);
        if (size % 0x130 == 0 && size <= (64u << 20)) {
            std::vector<char> toc(size);
            if (size && ReadFile(h, toc.data(), size, &got, nullptr) && got == size)
                for (uint32_t o = 0; o + 0x130 <= size; o += 0x130) {
                    char* p = toc.data() + o;
                    p[0xff] = 0;
                    size_t n = strlen(p);
                    if (n > 7 && _stricmp(p + n - 7, ".w2mesh") == 0) g_meshPaths.emplace(Fnv64(p), p);
                }
        }
    }
    CloseHandle(h);
}

void ReadBundlesUnder(const std::wstring& dir, int depth) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 5) ReadBundlesUnder(dir + L"\\" + name, depth + 1);
        } else if (name.size() > 7 && _wcsicmp(name.c_str() + name.size() - 7, L".bundle") == 0) {
            ReadBundleMeshes(dir + L"\\" + name);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}
}  // namespace

// The depot path of a mesh by its hash (FNV-1a 64 over the lower-case path), or "". The first call reads the
// tables of every bundle under the game's content, dlc and Mods folders (the .w2mesh paths only).
std::string MeshPathOfHash(uint64_t hash) {
    std::lock_guard<std::mutex> lk(g_meshPathMx);
    if (!g_meshPathsRead) {
        g_meshPathsRead = true;
        ULONGLONG t0 = GetTickCount64();
        std::wstring game = Dir() + L"\\..\\..\\..";
        for (const wchar_t* sub : {L"content", L"dlc", L"Mods"}) ReadBundlesUnder(game + L"\\" + sub, 0);
        Log("sector: %zu mesh paths from the bundles' tables in %llu ms", g_meshPaths.size(), GetTickCount64() - t0);
    }
    auto it = g_meshPaths.find(hash);
    return it == g_meshPaths.end() ? std::string() : it->second;
}

// The render proxies of objects of the world's block that are streamed in. The block's streamer is
// [[CWorld+0x130]+0x48] ([[S+0x68]] leads back to the block); it keeps its instances from S+0x16510, 16 bytes each
// {u32 object, instance}, their count at S+0x16500; a mesh's instance (CSectorDataObjectMesh) has its proxy at +0x18
// (docs/engine/world.md, Sektor-Streaming). ids: object numbers from 0; out gets the proxy or null for each.
// -> how many have a proxy, -1 when the world has no streamer
int SectorProxies(void* world, const uint32_t* ids, size_t n, void** out) {
    for (size_t i = 0; i < n; ++i) out[i] = nullptr;
    void* block = WorldSectorData(world);
    char *p = nullptr, *s = nullptr, *back = nullptr;
    void* b = nullptr;
    uint32_t count = 0;
    if (!block || !SafeRead(static_cast<char*>(world) + 0x130, &p, 8) || !Plausible(p) || !SafeRead(p + 0x48, &s, 8) ||
        !Plausible(s) || !SafeRead(s + 0x68, &back, 8) || !Plausible(back) || !SafeRead(back, &b, 8) || b != block ||
        !SafeRead(s + 0x16500, &count, 4) || count > 0x19000)
        return -1;
    std::unordered_map<uint32_t, size_t> want;
    for (size_t i = 0; i < n; ++i) want.emplace(ids[i], i);
    std::vector<uint8_t> inst(static_cast<size_t>(count) * 16);
    if (count && !SafeRead(s + 0x16510, inst.data(), inst.size())) return -1;
    int found = 0;
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t id;
        char* obj;
        memcpy(&id, &inst[k * 16], 4);
        memcpy(&obj, &inst[k * 16 + 8], 8);
        auto it = want.find(id);
        void* proxy = nullptr;
        if (it == want.end() || !Plausible(obj) || !SafeRead(obj + 0x18, &proxy, 8) || !Plausible(proxy)) continue;
        out[it->second] = proxy;
        ++found;
    }
    return found;
}

bool SectorSetPos(void* sector, uint32_t index, const float* pos) {
    char* list = nullptr;
    uint32_t n = 0;
    if (!SafeRead(static_cast<char*>(sector) + 0x28, &list, 8) || !SafeRead(static_cast<char*>(sector) + 0x30, &n, 4) ||
        !list || index >= n)
        return false;
    return WriteGuarded(list + index * 24 + 12, pos, 12);
}

// --- merged meshes of the world's block out of the world at once (w3.world.unstream). The streaming update runs in
// two halves: the first on CSectorDataStreamingThread decides what streams in and out (it adds instances at S+0x16510
// and reads the allowed bits at S+0xf20, one per object, the highest bit of a qword first), the second on the main
// thread in SectorStreaming::Finish calls the instances' slot 1 (stream) and slot 2 (unstream); the world's tick
// starts the next first half right after it. An object switched off loses its allowed bit at once: the streamer lets
// it go within about a second and does not bring it back while the bit is clear. Right after the next second half its
// mesh and collision instances are unstreamed through their own slot 2, with the context the streamer builds for it
// (0x2671620 in 5.01: S+0x50, the render scene S+0x38, the physics world S+0x40, S+0x48, S+0x60, then u8 1 = no
// fade): the render proxy leaves the scene that frame, the body its physics tile. When the streamer unstreams the
// instance itself later, slot 2 finds it empty (mesh +0x18 null, collision +0x18 -1) and returns.
namespace {
constexpr uint32_t kAllowBits = 0xf20, kInstCount = 0x16500, kInsts = 0x16510, kHasInst = 0x1a6510;
constexpr uint32_t kMaxObjects = (kInstCount - kAllowBits) * 8;
using Finish_t = void (*)(void*);
Finish_t oFinish = nullptr;
std::mutex g_unMx;
char* g_unS = nullptr;              // the streamer the lists belong to (a new world has a new one)
std::vector<uint32_t> g_off;        // switched off (object numbers from 0, a mesh and its collision)
std::vector<uint32_t> g_pending;    // switched off, their instances still to unstream
std::vector<std::pair<uint32_t, int>> g_back;  // wanted back: the bit goes on once the streamer dropped the instance
std::atomic<bool> g_unAny{false};

char* StreamerOf(void* world) {  // [[CWorld+0x130]+0x48], checked by its way back to the world's block
    void* block = WorldSectorData(world);
    char *p = nullptr, *s = nullptr, *back = nullptr;
    void* b = nullptr;
    uint32_t count = 0;
    if (!block || !SafeRead(static_cast<char*>(world) + 0x130, &p, 8) || !Plausible(p) || !SafeRead(p + 0x48, &s, 8) ||
        !Plausible(s) || !SafeRead(s + 0x68, &back, 8) || !Plausible(back) || !SafeRead(back, &b, 8) || b != block ||
        !SafeRead(s + kInstCount, &count, 4) || count > 0x19000)
        return nullptr;
    return s;
}

bool Bit(char* s, uint32_t base, uint32_t id) {
    uint64_t q = 0;
    return SafeRead(s + base + (id >> 6) * 8, &q, 8) && ((q >> (63 - (id & 63))) & 1);
}

bool SetAllowBit(char* s, uint32_t id, bool on) {
    if (id >= kMaxObjects) return false;
    auto* q = reinterpret_cast<volatile LONG64*>(s + kAllowBits + (id >> 6) * 8);
    LONG64 mask = static_cast<LONG64>(1ull << (63 - (id & 63)));
    __try {
        if (on)
            InterlockedOr64(q, mask);
        else
            InterlockedAnd64(q, ~mask);
        return true;
    } __except (1) {
        return false;
    }
}

bool CallUnstream(void* inst, void* ctx) {  // the instance's slot 2 (instance, context, u8) -> false on a fault
    __try {
        auto fn = (*reinterpret_cast<int (***)(void*, void*, uint8_t)>(inst))[2];
        fn(inst, ctx, 0);
        return true;
    } __except (1) {
        return false;
    }
}

// The instances of `ids` the streamer has, unstreamed (main thread, between the two halves) -> how many
int UnstreamInstances(char* s, const std::vector<uint32_t>& ids) {
    uint32_t count = 0;
    if (ids.empty() || !SafeRead(s + kInstCount, &count, 4) || count > 0x19000 || !count) return 0;
    std::vector<uint8_t> inst(static_cast<size_t>(count) * 16);
    alignas(8) uint8_t ctx[0x30] = {};
    static const uint32_t from[5] = {0x50, 0x38, 0x40, 0x48, 0x60};
    for (int k = 0; k < 5; ++k)
        if (!SafeRead(s + from[k], ctx + k * 8, 8)) return 0;
    ctx[0x28] = 1;  // at once, no fade
    if (!SafeRead(s + kInsts, inst.data(), inst.size())) return 0;
    int done = 0;
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t id;
        void* obj;
        memcpy(&id, &inst[k * 16], 4);
        memcpy(&obj, &inst[k * 16 + 8], 8);
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) continue;
        if (!IsType(obj, ".?AVCSectorDataObjectMesh@@") && !IsType(obj, ".?AVCSectorDataObjectCollision@@")) continue;
        if (CallUnstream(obj, ctx))
            ++done;
        else
            Log("sector: unstreaming object %u faulted", id);
    }
    return done;
}

void FinishHook(void* self) {
    oFinish(self);
    if (!g_unAny.load(std::memory_order_relaxed) || !OnMainThread()) return;
    char* s = nullptr;
    if (!SafeRead(static_cast<char*>(self) + 0x48, &s, 8)) return;
    std::lock_guard<std::mutex> lk(g_unMx);
    if (s != g_unS) return;  // another world's streaming
    for (uint32_t id : g_off) SetAllowBit(s, id, false);  // a layer attaching sets the bits of its objects again
    if (!g_pending.empty()) {
        int n = UnstreamInstances(s, g_pending);
        if (n) Log("sector: %d instance(s) of %zu object(s) unstreamed", n, g_pending.size());
        g_pending.clear();
    }
    for (size_t i = 0; i < g_back.size();) {  // back once the streamer has deleted the instance we emptied
        if (Bit(s, kHasInst, g_back[i].first) && --g_back[i].second > 0) {
            ++i;
            continue;
        }
        SetAllowBit(s, g_back[i].first, true);
        g_back.erase(g_back.begin() + i);
    }
    g_unAny.store(!g_off.empty() || !g_back.empty());
}
}  // namespace

// An object of the world's block (a number from 0, a mesh: its collision object after it goes with it) out of the
// world at once and kept out while the world streams (off), or back (on; within a second or two). For this world: a
// load makes a new one. -> the objects switched, -1 when the world has no streamer (another game build)
int SectorUnstream(void* world, uint32_t id, bool on) {
    void* block = WorldSectorData(world);
    char* s = StreamerOf(world);
    if (!s) return -1;
    char* list = nullptr;
    uint32_t n = 0;
    if (!SafeRead(static_cast<char*>(block) + 0x28, &list, 8) || !SafeRead(static_cast<char*>(block) + 0x30, &n, 4) ||
        !list || id >= n || id + 1 >= kMaxObjects)
        return 0;
    uint32_t ids[2] = {id, id};
    int count = 1;
    uint8_t type = 0, next = 0;
    if (SafeRead(list + static_cast<size_t>(id) * 24, &type, 1) && type == 1 && id + 1 < n &&
        SafeRead(list + static_cast<size_t>(id + 1) * 24, &next, 1) && next == 2)
        ids[count++] = id + 1;
    std::lock_guard<std::mutex> lk(g_unMx);
    if (s != g_unS) g_unS = s, g_off.clear(), g_pending.clear(), g_back.clear();
    for (int k = 0; k < count; ++k) {
        uint32_t i = ids[k];
        auto off = std::find(g_off.begin(), g_off.end(), i);
        auto back = std::find_if(g_back.begin(), g_back.end(), [i](const auto& b) { return b.first == i; });
        if (on) {
            if (off == g_off.end()) continue;  // not switched off here
            g_off.erase(off);
            g_pending.erase(std::remove(g_pending.begin(), g_pending.end(), i), g_pending.end());
            if (back == g_back.end()) g_back.emplace_back(i, 600);  // at most 600 updates (about 10 s) waiting
        } else {
            if (back != g_back.end()) g_back.erase(back);
            if (off == g_off.end()) g_off.push_back(i);
            if (std::find(g_pending.begin(), g_pending.end(), i) == g_pending.end()) g_pending.push_back(i);
            SetAllowBit(s, i, false);
        }
    }
    if (!oFinish) {  // without the hook only the bit: the streamer lets the object go within about a second
        g_pending.clear();
        for (auto& b : g_back) SetAllowBit(s, b.first, true);
        g_back.clear();
    }
    g_unAny.store(!g_off.empty() || !g_back.empty() || !g_pending.empty());
    return count;
}

// Is an object of the world's block switched off by SectorUnstream? 1 yes, 0 no, -1 no streamer
int SectorUnstreamed(void* world, uint32_t id) {
    char* s = StreamerOf(world);
    if (!s) return -1;
    std::lock_guard<std::mutex> lk(g_unMx);
    return s == g_unS && std::find(g_off.begin(), g_off.end(), id) != g_off.end() ? 1 : 0;
}

void InstallSectorEdit() {
    RegisterCommand("umbra", &CmdUmbra, "umbra [on|off]: the precomputed visibility, for looking into a problem");
    HookNamed("SectorStreaming::Finish", reinterpret_cast<void*>(&FinishHook), reinterpret_cast<void**>(&oFinish));
    Log("sector: streaming finish %s", oFinish ? "hooked" : "not hooked (w3.world.unstream waits for the streamer)");
}

}  // namespace tw3se
