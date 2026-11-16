// TW3SE core - the merged geometry of the world (CSectorData). Buildings, walls, rocks, their collisions, decals and
// lights are cooked out of their entities into one packed block per layer (CLayer+0x98; resources at +0x10, the
// object list at +0x28: 24-byte entries {type, flags, radius, offset, position}, type 1 mesh, 2 collision, 3 decal)
// and streamed in as the player comes near. Measured: objects moved away before their layer attaches never
// stream in (the main square's buildings gone, walked through). Removing single buildings this way, switched live
// by quest facts, is the next step; until then this file holds the switch for the precomputed visibility.
#include "core.h"
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
// only the meshes whose box in the world lies within `radius` of it.
int SectorMeshes(void* sector, std::vector<SectorMesh>* out, const float* center, float radius) {
    char *res = nullptr, *data = nullptr, *list = nullptr;
    uint32_t nres = 0, ndata = 0, n = 0;
    char* s = static_cast<char*>(sector);
    if (!SafeRead(s + 0x10, &res, 8) || !SafeRead(s + 0x18, &nres, 4) || !SafeRead(s + 0x1c, &data, 8) ||
        !SafeRead(s + 0x24, &ndata, 4) || !SafeRead(s + 0x28, &list, 8) || !SafeRead(s + 0x30, &n, 4) || !list ||
        !data || !res || n > 1000000 || nres > 1000000)
        return -1;
    out->clear();
    for (uint32_t i = 0; i < n; ++i) {
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

bool SectorSetPos(void* sector, uint32_t index, const float* pos) {
    char* list = nullptr;
    uint32_t n = 0;
    if (!SafeRead(static_cast<char*>(sector) + 0x28, &list, 8) || !SafeRead(static_cast<char*>(sector) + 0x30, &n, 4) ||
        !list || index >= n)
        return false;
    return WriteGuarded(list + index * 24 + 12, pos, 12);
}

void InstallSectorEdit() {
    RegisterCommand("umbra", &CmdUmbra, "umbra [on|off]: the precomputed visibility, for looking into a problem");
}

}  // namespace tw3se
