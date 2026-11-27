// Blend shapes (live mesh deformation): by Tenour.
// Blend shapes (vertex deformation): a mesh's vertices moved by weighted deltas on the GPU, before the game
// draws, so every one of the game's shaders (skinning, shadows, every pass) draws the moved vertices.
//
// Where a mesh's vertices are (measured with deform.cpp):
//   CRenderMesh +0xa0: u32 vertex buffer id, u32 index buffer id; +0xa8 chunk array, +0xb0 u32 chunk count;
//   +0xe0 quantization scale (3 floats), +0xf0 offset (3 floats): position = unorm16 / 65535 * scale + offset.
//   chunk (0x30 bytes): +0x04 u32 offset of stream 0 (position unorm16 x4, bone indices, bone weights: 16 bytes per
//   vertex), +0x08 stream 1 (uv), +0x0c stream 2 (normal R10G10B10A2, tangent: 8 bytes), +0x20 u32 vertex count,
//   +0x24 u32 index count, +0x2c u32 LOD mask.
//   buffer pool: dd = *(exe + pool), entry(id) = dd + (id - 1) * 0x240 + entries (both found in the game's code at
//   the first use, PoolKnown): +0x00 u32 size, +0x10 GPU address, +0x18 ID3D12Resource*.
//   The renderer reads the box at each draw (z scale doubled = the head twice as tall), so a set grows the
//   box to hold every reachable position and quantizes every vertex anew. CRenderMesh is shared by all instances of
//   the mesh: a change shows on each of them.
//
// How: on load, a chunk's stream 0 is copied once into a buffer of TW3SE's (orig) and its deltas go to the GPU. When
// weights change, a compute shader writes orig + sum(weight * delta), quantized into the grown box, to a second buffer
// (work), which is copied into the game's buffer: on the game's DIRECT queue, ahead of the game's own lists
// (overlay.cpp's ExecuteCommandLists hook). Bone indices and weights stay as they were. Unload copies orig back.
//
// Delta file (TWDF, little endian):
//   char[4] "TWDF", u32 version (1 or 2), u32 shape count K, u32 chunk count C; version 2: u32 flags (bit 0: normal
//   deltas follow each chunk's position deltas);
//   K names: u16 length, UTF-8 bytes;
//   C chunks: u32 chunk index, u32 vertex count n, float32 deltas[K][n][3] (model space, metres); with bit 0 then
//   float32 normal deltas[K][n][3] (added to the unit normal, the sum normalized; the tangent is turned to stay at a
//   right angle to it, its handedness kept).
// Base dump (TWDB, from w3.deform.dump): char[4] "TWDB", u32 version (1), u32 chunk count C;
//   C chunks: u32 chunk index, u32 LOD mask, u32 vertex count n, float32 positions[n][3], float32 normals[n][3].
#include "core.h"

#include <d3d12.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tw3se {
namespace {

constexpr uintptr_t kPoolStride = 0x240;

// The buffer pool's global and the offset of its entries, from the game's code that reads an entry:
//   lea r8, [rax + rax*8]; mov rax, [rip + pool]; shl r8, 6; mov ecx, [r8 + rax + entries]
// 5.01: pool 5a58ef8 (at 510711), build 6adfbd7a: 5a56ef8 (at 511771), entries 5924d8 in both. They are searched
// for in each build (fixed numbers read garbage on another build). Every match has to give the same numbers. 0 when
// none is found.
uintptr_t g_poolPtr = 0, g_poolEntries = 0;
bool PoolKnown() {
    static std::once_flag once;
    std::call_once(once, [] {
        const uint8_t* text = nullptr;
        size_t size = 0;
        ExeText(&text, &size);
        uintptr_t ptr = 0, entries = 0;
        int matches = 0;
        bool agree = true;
        for (size_t i = 0; text && i + 23 < size; ++i) {
            const uint8_t* a = text + i;
            if (a[0] != 0x4c || a[1] != 0x8d || a[2] != 0x04 || a[3] != 0xc0 || a[4] != 0x48 || a[5] != 0x8b ||
                a[6] != 0x05 || a[11] != 0x49 || a[12] != 0xc1 || a[13] != 0xe0 || a[14] != 0x06 ||
                (a[15] != 0x41 && a[15] != 0x42) || a[16] != 0x8b || a[17] != 0x8c || a[18] != 0x00)
                continue;
            int32_t disp = 0;
            uint32_t off = 0;
            memcpy(&disp, a + 7, 4);
            memcpy(&off, a + 19, 4);
            uintptr_t p = reinterpret_cast<uintptr_t>(a + 11) + disp - ImageBase();
            if (matches++ && (p != ptr || off != entries)) agree = false;
            ptr = p;
            entries = off;
        }
        if (matches && agree) {
            g_poolPtr = ptr;
            g_poolEntries = entries;
            Log("blendshape: buffer pool at witcher3.exe+0x%llx, entries at +0x%llx (%d places)",
                static_cast<unsigned long long>(ptr), static_cast<unsigned long long>(entries), matches);
        } else {
            Log("blendshape: the game's buffer pool was not found (%d places%s): blend shapes are off on this build",
                matches, agree ? "" : ", they disagree");
        }
    });
    return g_poolPtr != 0;
}
constexpr uint32_t kChunkSize = 0x30;
constexpr int kSlots = 3;                   // command lists in flight
constexpr uint64_t kWeightBytes = 0x10000;  // per slot: (shape, weight) pairs of all sets

// orig and work: stream 0 (16 bytes per vertex), then (with normals) stream 2 (8 bytes: normal, tangent, both
// R10G10B10A2 unorm, the 2 bits kept). deltas: positions [K][n][3], then (with normals) normals [K][n][3].
const char kBlendCS[] = R"(
cbuffer C : register(b0) { uint count; uint active; uint normals; uint shapes; float4 s0; float4 o0; float4 s1; float4 o1; };
ByteAddressBuffer orig : register(t0);
ByteAddressBuffer deltas : register(t1);
ByteAddressBuffer weights : register(t2);
RWByteAddressBuffer work : register(u0);
float3 Unpack(uint v) { return float3(v & 1023, (v >> 10) & 1023, (v >> 20) & 1023) / 1023.0 * 2 - 1; }
uint Pack(float3 n, uint old) {
    uint3 q = uint3(saturate(n * 0.5 + 0.5) * 1023.0 + 0.5);
    return q.x | (q.y << 10) | (q.z << 20) | (old & 0xc0000000);
}
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= count) return;
    uint4 v = orig.Load4(i * 16);
    float3 p = float3(v.x & 0xffff, v.x >> 16, v.y & 0xffff) / 65535.0 * s0.xyz + o0.xyz;
    for (uint a = 0; a < active; ++a) {
        uint2 kw = weights.Load2(a * 8);
        p += asfloat(kw.y) * asfloat(deltas.Load3((kw.x * count + i) * 12));
    }
    float3 q = clamp((p - o1.xyz) / max(s1.xyz, 1e-9) * 65535.0, 0, 65535);
    uint3 w = uint3(q + 0.5);
    work.Store4(i * 16, uint4(w.x | (w.y << 16), w.z | (v.y & 0xffff0000), v.z, v.w));
    if (normals == 0) return;
    uint2 nt = orig.Load2(count * 16 + i * 8);
    float3 n = Unpack(nt.x), t = Unpack(nt.y), n0 = n;
    for (uint b = 0; b < active; ++b) {
        uint2 kw = weights.Load2(b * 8);
        n += asfloat(kw.y) * asfloat(deltas.Load3(((shapes + kw.x) * count + i) * 12));
    }
    n = dot(n, n) > 1e-12 ? normalize(n) : n0;
    float3 tt = t - n * dot(n, t);
    t = dot(tt, tt) > 1e-12 ? normalize(tt) : t;
    work.Store2(count * 16 + i * 8, uint2(Pack(n, nt.x), Pack(t, nt.y)));
}
)";

// A live mesh's chunk: positions (and normals) from the plugin's array (src: positions [n][3], then normals [n][3]),
// or with flags bit 0 clear the chunk's own positions read in its box (s0, o0) and stored in the live box (s1, o1).
// Bit 1: normals given. Bone indices, weights and the 4th position value stay as in orig.
const char kLiveCS[] = R"(
cbuffer C : register(b0) { uint count; uint flags; uint pad0; uint pad1; float4 s0; float4 o0; float4 s1; float4 o1; };
ByteAddressBuffer orig : register(t0);
ByteAddressBuffer src : register(t1);
RWByteAddressBuffer work : register(u0);
float3 Unpack(uint v) { return float3(v & 1023, (v >> 10) & 1023, (v >> 20) & 1023) / 1023.0 * 2 - 1; }
uint Pack(float3 n, uint old) {
    uint3 q = uint3(saturate(n * 0.5 + 0.5) * 1023.0 + 0.5);
    return q.x | (q.y << 10) | (q.z << 20) | (old & 0xc0000000);
}
[numthreads(64, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= count) return;
    uint4 v = orig.Load4(i * 16);
    float3 p = (flags & 1) ? asfloat(src.Load3(i * 12))
                           : float3(v.x & 0xffff, v.x >> 16, v.y & 0xffff) / 65535.0 * s0.xyz + o0.xyz;
    float3 q = clamp((p - o1.xyz) / max(s1.xyz, 1e-9) * 65535.0, 0, 65535);
    uint3 w = uint3(q + 0.5);
    work.Store4(i * 16, uint4(w.x | (w.y << 16), w.z | (v.y & 0xffff0000), v.z, v.w));
    if ((flags & 2) == 0) return;
    uint2 nt = orig.Load2(count * 16 + i * 8);
    float3 n = asfloat(src.Load3(count * 12 + i * 12)), t = Unpack(nt.y);
    n = dot(n, n) > 1e-12 ? normalize(n) : Unpack(nt.x);
    float3 tt = t - n * dot(n, t);
    t = dot(tt, tt) > 1e-12 ? normalize(tt) : t;
    work.Store2(count * 16 + i * 8, uint2(Pack(n, nt.x), Pack(t, nt.y)));
}
)";
constexpr uint32_t kLiveMaxVertices = 65536;  // a chunk of a live mesh

// Normals of a moved chunk from its triangles (DeformNormals). Per triangle its unit normal at rest (orig, box s0/o0)
// and moved (work, box s1/o1), added up per vertex in fixed point (acc: int3 rest, int3 moved). A vertex counts as its
// weld representative (weld: the first vertex at the same rest position), so the copies at a UV seam add into one.
// Per vertex the authored normal and tangent (orig stream 2) are turned by the rotation that takes the rest sum onto
// the moved one: hard edges and baked normals stay, the bend comes in. idx: the chunk's u16 indices (triangle list).
const char kNormCS[] = R"(
cbuffer C : register(b0) { uint count; uint tris; uint pad0; uint pad1; float4 s0; float4 o0; float4 s1; float4 o1; };
ByteAddressBuffer orig : register(t0);
ByteAddressBuffer idx : register(t1);
ByteAddressBuffer weld : register(t2);
RWByteAddressBuffer work : register(u0);
RWByteAddressBuffer acc : register(u1);
float3 Unpack(uint v) { return float3(v & 1023, (v >> 10) & 1023, (v >> 20) & 1023) / 1023.0 * 2 - 1; }
uint Pack(float3 n, uint old) {
    uint3 q = uint3(saturate(n * 0.5 + 0.5) * 1023.0 + 0.5);
    return q.x | (q.y << 10) | (q.z << 20) | (old & 0xc0000000);
}
float3 P0(uint i) { uint2 v = orig.Load2(i * 16); return float3(v.x & 0xffff, v.x >> 16, v.y & 0xffff) / 65535.0 * s0.xyz + o0.xyz; }
float3 P1(uint i) { uint2 v = work.Load2(i * 16); return float3(v.x & 0xffff, v.x >> 16, v.y & 0xffff) / 65535.0 * s1.xyz + o1.xyz; }
uint Index(uint k) { uint w = idx.Load((k * 2) & ~3u); return (k & 1) ? (w >> 16) : (w & 0xffff); }
float3 Unit(float3 v) { float l = dot(v, v); return l > 1e-24 ? v * rsqrt(l) : float3(0, 0, 0); }
void Add(uint at, float3 n) {
    int3 q = int3(n * 65536.0);
    uint o;
    acc.InterlockedAdd(at, asuint(q.x), o);
    acc.InterlockedAdd(at + 4, asuint(q.y), o);
    acc.InterlockedAdd(at + 8, asuint(q.z), o);
}
[numthreads(64, 1, 1)] void clear(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    acc.Store3(id.x * 24, uint3(0, 0, 0));
    acc.Store3(id.x * 24 + 12, uint3(0, 0, 0));
}
[numthreads(64, 1, 1)] void faces(uint3 id : SV_DispatchThreadID) {
    if (id.x >= tris) return;
    uint a = Index(id.x * 3), b = Index(id.x * 3 + 1), c = Index(id.x * 3 + 2);
    if (a >= count || b >= count || c >= count) return;
    float3 n0 = Unit(cross(P0(b) - P0(a), P0(c) - P0(a)));
    float3 n1 = Unit(cross(P1(b) - P1(a), P1(c) - P1(a)));
    uint w[3] = { weld.Load(a * 4), weld.Load(b * 4), weld.Load(c * 4) };
    for (uint k = 0; k < 3; ++k) { Add(w[k] * 24, n0); Add(w[k] * 24 + 12, n1); }
}
float3 Turn(float3 a, float3 b, float3 v) {  // v turned by the rotation that takes unit a onto unit b
    float c = dot(a, b);
    if (c < -0.999) return v;
    float3 k = cross(a, b);
    return v * c + cross(k, v) + k * (dot(k, v) / (1 + c));
}
[numthreads(64, 1, 1)] void verts(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= count) return;
    uint r = weld.Load(i * 4);
    float3 a = Unit(float3(asint(acc.Load3(r * 24)))), b = Unit(float3(asint(acc.Load3(r * 24 + 12))));
    uint2 nt = orig.Load2(count * 16 + i * 8);
    if (dot(a, a) < 0.5 || dot(b, b) < 0.5) { work.Store2(count * 16 + i * 8, nt); return; }
    float3 n = Unit(Turn(a, b, Unpack(nt.x))), t = Unit(Turn(a, b, Unpack(nt.y)));
    work.Store2(count * 16 + i * 8, uint2(Pack(n, nt.x), Pack(t, nt.y)));
}
)";

// Hiding triangles (w3.deform.hide): the triangles of a chunk marked in a bit field are drawn as nothing - their three
// indices made the same (a triangle without area) in the game's index buffer of the mesh. kHideCS writes the chunk's
// indices from a copy of the original (orig) and the bits (triangle t: bit t % 8 of byte t / 8) into work, which is
// copied over the chunk's indices; clearing copies orig back.
const char kHideCS[] = R"(
cbuffer C : register(b0) { uint words; uint indices; uint pad0; uint pad1; float4 pad2[4]; };
ByteAddressBuffer orig : register(t0);
ByteAddressBuffer bits : register(t1);
RWByteAddressBuffer work : register(u0);
uint Index(uint k) { uint w = orig.Load((k * 2) & ~3u); return (k & 1) ? (w >> 16) : (w & 0xffff); }
uint Out(uint k) {
    if (k >= indices) return 0;
    uint t = k / 3;
    return ((bits.Load((t >> 5) * 4) >> (t & 31)) & 1) ? Index(t * 3) : Index(k);
}
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= words) return;
    work.Store(id.x * 4, Out(id.x * 2) | (Out(id.x * 2 + 1) << 16));
}
)";

struct Box {
    float s[3], o[3];
};

struct Chunk {
    uint32_t index = 0, count = 0;
    uint64_t off0 = 0, off2 = 0;  // stream 0 and stream 2 in the game's resource
    std::vector<float> data;  // the deltas until they are on the GPU (positions, then normals)
    ID3D12Resource* orig = nullptr;
    ID3D12Resource* deltas = nullptr;
    ID3D12Resource* work = nullptr;
    // a live mesh: the newest write (positions [n][3], with `liveNormals` then normals [n][3]), kept after it went
    // to the GPU (a new box stores it anew); `upload` holds kSlots copies of it, one per command list in flight
    std::vector<float> live;
    bool liveNormals = false;
    bool liveNew = false;  // written since the last list
    ID3D12Resource* upload = nullptr;
    uint8_t* upMap = nullptr;
    // normals from the triangles (DeformNormals): the chunk's indices in the game's index buffer, their copy, the weld
    // map (upload heap: identity until the rest positions are read back), the sums, the readback of the rest positions
    uint64_t ioff = 0;
    uint32_t icount = 0;
    ID3D12Resource* idx = nullptr;
    ID3D12Resource* weld = nullptr;
    uint32_t* weldMap = nullptr;
    ID3D12Resource* acc = nullptr;
    ID3D12Resource* rb = nullptr;
    uint64_t rbFence = 0;  // the list that filled rb; 0: none yet
    bool welded = false;
    bool normalsWritten = false;  // stream 2 in the game's buffer holds normals made here (back to orig when off)
};

struct LiveWriteEarly {  // a write before the copy's render mesh was there
    std::vector<float> v;
    uint32_t count = 0;
    bool normals = false;
};

struct Set {
    int id = 0;
    uintptr_t rm = 0;
    uint32_t vbId = 0;
    ID3D12Resource* res = nullptr;  // the game's vertex buffer, held (AddRef) while the set lives
    Box box0, box1;                 // the mesh's box, and the grown one
    std::vector<std::string> names;
    std::vector<float> weights;
    std::vector<Chunk> chunks;
    bool normals = false;  // the file has normal deltas: stream 2 is written too
    bool ready = false;   // orig and deltas on the GPU
    bool dirty = true;    // weights changed since the last write
    bool unload = false;  // copy the original back, then go
    bool lost = false;    // the mesh is gone or was loaded anew (its buffer changed)
    bool heldRm = false;  // TW3SE holds a reference on the render mesh (IRenderResource::AddRef)
    int watch = -1;       // DeformWatchRange slot: the game has drawn the mesh (its vertices are on the GPU)
    int waited = 0;       // frames the set waited for that
    int sinceSeen = -1;   // frames since the draw was seen
    // a live mesh (LiveNew): its positions come from the plugin, box1 is the box they are stored in. rm stays 0
    // until the component's copy has its render mesh (LiveAttach).
    bool live = false;
    bool boxSet = false;  // LiveBounds was called (else box1 = the mesh's box)
    bool boxNew = false;  // box1 changed: every chunk is stored anew in the next list
    std::map<uint32_t, LiveWriteEarly> early;
    // DeformNormals: 1 = normals and tangents follow the moved triangles (in place of the file's or plugin's normals)
    int normalsMode = 0;
    ID3D12Resource* ibRes = nullptr;  // the game's index buffer (held while the set lives), 0 if not found
};

struct Dump {
    uintptr_t rm;
    std::wstring path;
    Box box;
    // raw (w3._bs_index_dump, measuring): the render mesh's ids and chunk table, then the bytes of one pool buffer
    bool raw = false;
    std::vector<uint8_t> head;
    uint64_t rawOff = 0, rawSize = 0;
    std::function<void(const uint8_t*, uint64_t)> done;  // raw: called with the bytes in place of a file
    struct Part {
        uint32_t index, mask, count;
        uint64_t off0, off2, at;  // in the game's resource; `at` = position in the readback buffer
    };
    std::vector<Part> parts;
    ID3D12Resource* res = nullptr;
    ID3D12Resource* readback = nullptr;
    uint64_t fence = 0;  // 0 = not yet submitted
};

std::mutex g_mx;
std::map<int, std::unique_ptr<Set>> g_sets;
std::set<int> g_resetIds;  // sets BlendResetAll ended: "lost" until their owner unloads them
std::vector<std::unique_ptr<Dump>> g_dumps;

// w3.deform.hide: per render mesh, the chunks whose triangles are hidden (kHideCS)
struct HideChunk {
    uint32_t index = 0, icount = 0;
    uint64_t ioff = 0;          // the chunk's indices in the game's index buffer resource
    std::vector<uint8_t> bits;  // a bit field waiting to go to the GPU (whole 32-bit words)
    bool fresh = false;         // `bits` is new
    bool any = false;           // the bit field hides a triangle (else the original indices go back)
    ID3D12Resource* orig = nullptr;  // the chunk's original indices
    ID3D12Resource* work = nullptr;
    ID3D12Resource* mask = nullptr;  // upload heap: the bit field
    bool written = false;            // the game's buffer holds indices of ours
    bool newOrig = false;            // orig was made in the list being recorded (NON_PIXEL_SHADER_RESOURCE)
};
struct Hide {
    int id = 0;
    uintptr_t rm = 0;
    uint32_t ibId = 0;
    ID3D12Resource* ibRes = nullptr;  // the game's index buffer, held while the hide lives
    uint64_t ibBase = 0;
    uint32_t ibSize = 0;
    std::map<uint32_t, HideChunk> chunks;
    bool ready = false;   // each chunk's original indices are copied
    bool dirty = false;   // a bit field changed
    bool unload = false;  // the original indices back, then go
    bool lost = false;    // the mesh is gone or was loaded anew
    bool heldRm = false;
    int watch = -1, waited = 0, sinceSeen = -1;  // as Set: the game's draw of the mesh is awaited
};
std::map<int, std::unique_ptr<Hide>> g_hides;
ID3D12PipelineState* g_hidePso = nullptr;
int g_nextId = 1;

ID3D12Device* g_dev = nullptr;
ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_pso = nullptr;
ID3D12PipelineState* g_livePso = nullptr;
ID3D12RootSignature* g_normRoot = nullptr;  // kNormCS: constants, 3 SRVs, 2 UAVs
ID3D12PipelineState* g_normPso[3] = {};       // clear, faces, verts
ID3D12CommandAllocator* g_alloc[kSlots] = {};
ID3D12GraphicsCommandList* g_list[kSlots] = {};
uint64_t g_slotFence[kSlots] = {};
ID3D12Fence* g_fence = nullptr;
uint64_t g_fenceValue = 0;
ID3D12CommandQueue* g_queue = nullptr;  // the game's DIRECT queue of the last submit (BlendResetAll writes through it)
ID3D12Resource* g_weightBuf = nullptr;  // upload heap, kSlots * kWeightBytes, mapped
uint8_t* g_weightMap = nullptr;
int g_slot = 0;
bool g_failed = false;
thread_local bool t_inSubmit = false;
// timing of DeformBeforeSubmit (w3._bs_stats): calls, microseconds in all, the longest, in StillThere
uint64_t g_tCalls = 0, g_tTotal = 0, g_tMax = 0, g_tCheck = 0, g_tChecks = 0;

uint64_t Micros() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart * 1000000 / f.QuadPart);
}
std::vector<std::pair<uint64_t, IUnknown*>> g_retire;  // released when the fence passes the value
std::vector<std::pair<uint64_t, uintptr_t>> g_retireRm;  // render meshes: handed to the main thread then

// `inList`: used by the list being recorded (released after its signal); else by lists already signalled.
void Retire(IUnknown* u, bool inList = false) {
    if (u) g_retire.push_back({g_fenceValue + (inList ? 1 : 0), u});
}

// A set holds its render mesh (docs/FINDINGS_DEFORM_LIFETIME.md). It knew the mesh by address only: a figure
// with an own copy was destroyed, the game freed the render mesh, StillThere read the old values from the freed memory
// and SetBox wrote the box into it at each weight change (a crash minutes later). With
// a reference of its own the render mesh lives until the set has ended and the GPU is done with it.
bool HoldRenderMesh(uintptr_t rm) {
    auto add = reinterpret_cast<void (*)(void*)>(Address("IRenderResource::AddRef"));
    if (!add || !Address("IRenderResource::Release")) return false;
    __try {
        add(reinterpret_cast<void*>(rm));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// On the main thread: the last reference runs the game's destructor, which must not run inside the render thread's
// submit hook under g_mx.
uint32_t RenderMeshRefs(void* rm) {
    uint32_t n = 0;
    SafeRead(static_cast<char*>(rm) + 8, &n, 4);
    return n;
}
bool ReleaseGuarded(void (*rel)(void*), void* rm) {
    __try {
        rel(rm);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void ReleaseRenderMesh(void* rm) {
    auto rel = reinterpret_cast<void (*)(void*)>(Address("IRenderResource::Release"));
    if (!rel) return;
    uint32_t before = RenderMeshRefs(rm);
    bool last = before == 1;  // the game's destructor runs: the memory is gone after
    if (!ReleaseGuarded(rel, rm)) Log("blendshape: giving render mesh %p back faulted (caught)", rm);
    else Log("blendshape: render mesh %p given back (%u references before%s)", rm, before, last ? ", it goes now" : "");
}
// After the lists already signalled (FreeSet runs before a lost set is recorded, or after the unload list's signal).
void RetireRenderMesh(uintptr_t rm) { g_retireRm.push_back({g_fenceValue, rm}); }

ID3D12Resource* MakeBuffer(uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                           D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_HEAP_PROPERTIES hp = {heap};
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size ? size : 4;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    ID3D12Resource* r = nullptr;
    return SUCCEEDED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)))
               ? r : nullptr;
}

// The device objects, made on the first work (on the render thread, from the game's queue).
bool InitWriter(ID3D12CommandQueue* q) {
    if (g_pso) return true;
    if (g_failed) return false;
    g_failed = true;  // until everything below exists
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&g_dev)))) return false;
    using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                       ID3DBlob**, ID3DBlob**);
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!compile) { Log("blendshape: d3dcompiler_47.dll not found"); return false; }
    ID3DBlob *cs = nullptr, *err = nullptr;
    if (FAILED(compile(kBlendCS, sizeof kBlendCS - 1, "blend", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err))) {
        Log("blendshape: shader: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        return false;
    }
    D3D12_ROOT_PARAMETER p[5] = {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 20;
    for (int i = 0; i < 3; ++i) {
        p[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[1 + i].Descriptor.ShaderRegister = i;
    }
    p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rd = {5, p};
    ID3DBlob* sig = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(g_dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_root)))) {
        Log("blendshape: root signature failed");
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_root;
    pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
    if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso)))) {
        Log("blendshape: compute pipeline failed");
        return false;
    }
    ID3DBlob* lcs = nullptr;
    if (FAILED(compile(kLiveCS, sizeof kLiveCS - 1, "live", nullptr, nullptr, "main", "cs_5_0", 0, 0, &lcs, &err))) {
        Log("blendshape: live mesh shader: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        g_pso = nullptr;
        return false;
    }
    pd.CS = {lcs->GetBufferPointer(), lcs->GetBufferSize()};
    if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_livePso)))) {
        Log("blendshape: live mesh pipeline failed");
        g_pso = nullptr;
        return false;
    }
    // hiding triangles (kHideCS, the blend root signature): without it w3.deform.hide reports the failure
    ID3DBlob* hcs = nullptr;
    if (SUCCEEDED(compile(kHideCS, sizeof kHideCS - 1, "hide", nullptr, nullptr, "main", "cs_5_0", 0, 0, &hcs, &err))) {
        pd.CS = {hcs->GetBufferPointer(), hcs->GetBufferSize()};
        if (FAILED(g_dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_hidePso)))) g_hidePso = nullptr;
    } else {
        Log("blendshape: hide shader: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
    }
    if (!g_hidePso) Log("blendshape: hiding triangles is off (its shader failed)");
    // the normals (kNormCS): its own root signature (two UAVs); without it the sets work, DeformNormals stays off
    D3D12_ROOT_PARAMETER np[6] = {};
    np[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    np[0].Constants.Num32BitValues = 20;
    for (int i = 0; i < 3; ++i) {
        np[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        np[1 + i].Descriptor.ShaderRegister = i;
    }
    for (int i = 0; i < 2; ++i) {
        np[4 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        np[4 + i].Descriptor.ShaderRegister = i;
    }
    D3D12_ROOT_SIGNATURE_DESC nrd = {6, np};
    ID3DBlob* nsig = nullptr;
    if (SUCCEEDED(D3D12SerializeRootSignature(&nrd, D3D_ROOT_SIGNATURE_VERSION_1, &nsig, &err)) &&
        SUCCEEDED(g_dev->CreateRootSignature(0, nsig->GetBufferPointer(), nsig->GetBufferSize(), IID_PPV_ARGS(&g_normRoot)))) {
        const char* entry[3] = {"clear", "faces", "verts"};
        for (int i = 0; i < 3 && g_normRoot; ++i) {
            ID3DBlob* b = nullptr;
            D3D12_COMPUTE_PIPELINE_STATE_DESC npd = {};
            npd.pRootSignature = g_normRoot;
            if (FAILED(compile(kNormCS, sizeof kNormCS - 1, "normals", nullptr, nullptr, entry[i], "cs_5_0", 0, 0, &b, &err))) {
                Log("blendshape: normals shader: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                g_normRoot = nullptr;
                break;
            }
            npd.CS = {b->GetBufferPointer(), b->GetBufferSize()};
            if (FAILED(g_dev->CreateComputePipelineState(&npd, IID_PPV_ARGS(&g_normPso[i])))) g_normRoot = nullptr;
        }
    }
    if (!g_normRoot) Log("blendshape: the normals pass is off (its shader or root signature failed)");
    for (int i = 0; i < kSlots; ++i) {
        if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i]))) ||
            FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[i], nullptr,
                                            IID_PPV_ARGS(&g_list[i])))) {
            Log("blendshape: command lists failed");
            g_pso = nullptr;
            return false;
        }
        g_list[i]->Close();
        g_list[i]->SetName(L"TW3SE blend shapes");  // a GPU crash report (DRED) names it
    }
    g_weightBuf = MakeBuffer(kSlots * kWeightBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))) || !g_weightBuf ||
        FAILED(g_weightBuf->Map(0, nullptr, reinterpret_cast<void**>(&g_weightMap)))) {
        Log("blendshape: fence or weight buffer failed");
        g_pso = nullptr;
        return false;
    }
    g_failed = false;
    Log("blendshape: ready (compute shader, own command lists on the game's queue)");
    return true;
}

void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    l->ResourceBarrier(1, &b);
}

// The resource's GPU address and size, 0 when `r` is no live resource (its calls guarded: the pool entry may hold
// something else on another build).
bool ResourceRange(ID3D12Resource* r, uint64_t* va, uint64_t* width) {
    __try {
        *va = r->GetGPUVirtualAddress();
        D3D12_RESOURCE_DESC d = r->GetDesc();
        *width = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? d.Width : 0;
        return *va && *width;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Whether `p` points at an object whose vtable lies in a loaded module (a COM object of the driver or a layer).
bool LooksLikeObject(const void* p) {
    uintptr_t vt = 0, fn = 0;
    if (!SafeRead(p, &vt, 8) || !vt || !SafeRead(reinterpret_cast<void*>(vt), &fn, 8) || !fn) return false;
    HMODULE m1 = nullptr, m2 = nullptr;
    const DWORD f = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    return GetModuleHandleExW(f, reinterpret_cast<LPCWSTR>(vt), &m1) &&
           GetModuleHandleExW(f, reinterpret_cast<LPCWSTR>(fn), &m2);
}

// The vertex buffer of a CRenderMesh: its id, the resource and the resource offset of the pool entry's data. `why`
// gets the reason and the values read when it is not there.
bool MeshBuffer(uintptr_t rm, uint32_t* id, ID3D12Resource** res, uint64_t* base, uint32_t* size,
                std::string* why = nullptr) {
    uint64_t ids = 0, dd = 0, va = 0, r = 0, rva = 0, width = 0;
    char b[300];
    auto fail = [&](const char* what) {
        snprintf(b, sizeof b, "%s (render mesh %p, buffer id %u, pool %p, entry va 0x%llx, resource %p, its va 0x%llx, "
                 "%llu bytes, entry size %u)", what, reinterpret_cast<void*>(rm), static_cast<uint32_t>(ids),
                 reinterpret_cast<void*>(dd), static_cast<unsigned long long>(va), reinterpret_cast<void*>(r),
                 static_cast<unsigned long long>(rva), static_cast<unsigned long long>(width), *size);
        if (why) *why = b;
        return false;
    };
    *size = 0;
    if (!SafeRead(reinterpret_cast<void*>(rm + 0xa0), &ids, 8) || !static_cast<uint32_t>(ids))
        return fail("no vertex buffer id");
    if (!PoolKnown()) return fail("the game's buffer pool was not found on this build");
    if (!SafeRead(reinterpret_cast<void*>(ImageBase() + g_poolPtr), &dd, 8) || !dd) return fail("no buffer pool");
    uintptr_t e = dd + (static_cast<uint32_t>(ids) - 1) * kPoolStride + g_poolEntries;
    if (!SafeRead(reinterpret_cast<void*>(e), size, 4) || !SafeRead(reinterpret_cast<void*>(e + 0x10), &va, 8) ||
        !SafeRead(reinterpret_cast<void*>(e + 0x18), &r, 8) || !r)
        return fail("no pool entry");
    if (!LooksLikeObject(reinterpret_cast<void*>(r)) || !ResourceRange(reinterpret_cast<ID3D12Resource*>(r), &rva, &width))
        return fail("the pool entry holds no buffer");
    if (va < rva || va + *size > rva + width) return fail("the pool entry's data lies outside its buffer");
    *id = static_cast<uint32_t>(ids);
    *res = reinterpret_cast<ID3D12Resource*>(r);
    if (base) *base = va - rva;
    return true;
}

// Is the set's mesh still the one it was loaded on (same buffer id, same resource in the pool)?
bool StillThere(const Set& s) {
    uint64_t ids = 0, dd = 0, r = 0;
    if (!SafeRead(reinterpret_cast<void*>(s.rm + 0xa0), &ids, 8) || static_cast<uint32_t>(ids) != s.vbId) return false;
    if (!PoolKnown() || !SafeRead(reinterpret_cast<void*>(ImageBase() + g_poolPtr), &dd, 8) || !dd) return false;
    uintptr_t e = dd + (s.vbId - 1) * kPoolStride + g_poolEntries;
    return SafeRead(reinterpret_cast<void*>(e + 0x18), &r, 8) && reinterpret_cast<ID3D12Resource*>(r) == s.res;
}

void SetBox(uintptr_t rm, const Box& b) {
    float v[8];
    if (!SafeRead(reinterpret_cast<void*>(rm + 0xe0), v, sizeof v)) return;
    for (int k = 0; k < 3; ++k) {
        v[k] = b.s[k];
        v[4 + k] = b.o[k];
    }
    memcpy(reinterpret_cast<void*>(rm + 0xe0), v, sizeof v);
}

bool ReadBox(uintptr_t rm, Box* b) {
    float v[8];
    if (!SafeRead(reinterpret_cast<void*>(rm + 0xe0), v, sizeof v)) return false;
    for (int k = 0; k < 3; ++k) {
        b->s[k] = v[k];
        b->o[k] = v[4 + k];
    }
    return true;
}

void FreeSet(Set& s) {
    if (s.watch >= 0) {
        DeformWatchEnd(s.watch);
        s.watch = -1;
    }
    if (s.heldRm) RetireRenderMesh(s.rm);
    s.heldRm = false;
    for (Chunk& c : s.chunks) {
        Retire(c.orig);
        Retire(c.deltas);
        Retire(c.work);
        Retire(c.upload);
        Retire(c.idx);
        Retire(c.weld);
        Retire(c.acc);
        Retire(c.rb);
        c.orig = c.deltas = c.work = c.upload = c.idx = c.weld = c.acc = c.rb = nullptr;
        c.upMap = nullptr;
        c.weldMap = nullptr;
    }
    Retire(s.res);
    Retire(s.ibRes);
    s.res = s.ibRes = nullptr;
}

// The game's index buffer of a render mesh (held) and each chunk's indices in it (Chunk ioff, icount): chunk +0x1c
// offset in bytes, +0x24 count, u16 triangle list (docs/FINDINGS_NACHT_1511.md). Without it DeformNormals stays off.
void FindIndices(uintptr_t rm, Set& s) {
    uint32_t ib = 0, size = 0;
    uint64_t dd = 0, va = 0, r = 0, rva = 0, width = 0, arr = 0;
    if (!SafeRead(reinterpret_cast<void*>(rm + 0xa4), &ib, 4) || !ib || !PoolKnown() ||
        !SafeRead(reinterpret_cast<void*>(ImageBase() + g_poolPtr), &dd, 8) || !dd ||
        !SafeRead(reinterpret_cast<void*>(rm + 0xa8), &arr, 8))
        return;
    uintptr_t e = dd + (ib - 1ull) * kPoolStride + g_poolEntries;
    if (!SafeRead(reinterpret_cast<void*>(e), &size, 4) || !SafeRead(reinterpret_cast<void*>(e + 0x10), &va, 8) ||
        !SafeRead(reinterpret_cast<void*>(e + 0x18), &r, 8) || !r || !LooksLikeObject(reinterpret_cast<void*>(r)) ||
        !ResourceRange(reinterpret_cast<ID3D12Resource*>(r), &rva, &width) || va < rva || va + size > rva + width)
        return;
    for (Chunk& c : s.chunks) {
        uint32_t off = 0, n = 0;
        if (!SafeRead(reinterpret_cast<void*>(arr + c.index * kChunkSize + 0x1c), &off, 4) ||
            !SafeRead(reinterpret_cast<void*>(arr + c.index * kChunkSize + 0x24), &n, 4) || off + 2ull * n > size ||
            n % 3 || c.count > 65535)
            n = 0;
        c.ioff = va - rva + off;
        c.icount = n;
    }
    s.ibRes = reinterpret_cast<ID3D12Resource*>(r);
    s.ibRes->AddRef();
}

// The weld map of a chunk once its rest positions are read back: each vertex to the first one at the same quantized
// position (the copies of a vertex at a UV seam). Under g_mx.
void Weld(Chunk& c) {
    uint8_t* m = nullptr;
    if (!c.rb || !c.weldMap || FAILED(c.rb->Map(0, nullptr, reinterpret_cast<void**>(&m)))) return;
    std::map<uint64_t, uint32_t> first;
    uint32_t joined = 0;
    for (uint32_t i = 0; i < c.count; ++i) {
        uint64_t key = 0;
        memcpy(&key, m + 16ull * i, 6);
        auto it = first.emplace(key, i).first;
        c.weldMap[i] = it->second;
        if (it->second != i) ++joined;
    }
    c.rb->Unmap(0, nullptr);
    c.welded = true;
    Log("blendshape: chunk %u: %u of %u vertices share a place with another (UV seams), their normals are one", c.index,
        joined, c.count);
}

// Normals from the moved triangles (kNormCS) into work's stream 2, after the chunk's positions went into work (work is
// in UNORDERED_ACCESS). False when the chunk cannot (no indices, buffers failed): work's stream 2 is left as it was.
bool RecordNormals(ID3D12GraphicsCommandList* l, Set& s, Chunk& c, bool fresh) {
    if (!s.ibRes && s.rm) FindIndices(s.rm, s);  // (a copy's index buffer comes a little after its vertices)
    if (!g_normRoot || !s.ibRes || !c.icount) return false;
    if (!c.idx) {
        uint64_t ib = (2ull * c.icount + 3) & ~3ull;
        c.idx = MakeBuffer(ib, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        c.acc = MakeBuffer(24ull * c.count, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        c.weld = MakeBuffer(4ull * c.count, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        c.rb = MakeBuffer(16ull * c.count, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!c.idx || !c.acc || !c.weld || !c.rb || FAILED(c.weld->Map(0, nullptr, reinterpret_cast<void**>(&c.weldMap)))) {
            Log("blendshape: set %d chunk %u: normals buffers failed", s.id, c.index);
            c.icount = 0;
            return false;
        }
        for (uint32_t i = 0; i < c.count; ++i) c.weldMap[i] = i;
        l->CopyBufferRegion(c.idx, 0, s.ibRes, c.ioff, 2ull * c.icount);
        Barrier(l, c.idx, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // the rest positions for the weld map (Weld at a later submit, once this list is done)
        if (fresh) Barrier(l, c.orig, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyBufferRegion(c.rb, 0, c.orig, 0, 16ull * c.count);
        if (fresh) Barrier(l, c.orig, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        c.rbFence = ~0ull;  // set to this list's fence after its submit
    }
    D3D12_RESOURCE_BARRIER uav[2] = {};
    uav[0].Type = uav[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav[0].UAV.pResource = c.work;
    uav[1].UAV.pResource = c.acc;
    float k[20] = {};
    uint32_t tris = c.icount / 3;
    memcpy(&k[0], &c.count, 4);
    memcpy(&k[1], &tris, 4);
    for (int a = 0; a < 3; ++a) {
        k[4 + a] = s.box0.s[a];
        k[8 + a] = s.box0.o[a];
        k[12 + a] = s.box1.s[a];
        k[16 + a] = s.box1.o[a];
    }
    l->ResourceBarrier(1, &uav[0]);  // the positions written into work
    l->SetComputeRootSignature(g_normRoot);
    l->SetComputeRoot32BitConstants(0, 20, k, 0);
    l->SetComputeRootShaderResourceView(1, c.orig->GetGPUVirtualAddress());
    l->SetComputeRootShaderResourceView(2, c.idx->GetGPUVirtualAddress());
    l->SetComputeRootShaderResourceView(3, c.weld->GetGPUVirtualAddress());
    l->SetComputeRootUnorderedAccessView(4, c.work->GetGPUVirtualAddress());
    l->SetComputeRootUnorderedAccessView(5, c.acc->GetGPUVirtualAddress());
    l->SetPipelineState(g_normPso[0]);
    l->Dispatch((c.count + 63) / 64, 1, 1);
    l->ResourceBarrier(1, &uav[1]);
    l->SetPipelineState(g_normPso[1]);
    l->Dispatch((tris + 63) / 64, 1, 1);
    l->ResourceBarrier(1, &uav[1]);
    l->SetPipelineState(g_normPso[2]);
    l->Dispatch((c.count + 63) / 64, 1, 1);
    return true;
}

std::vector<uint8_t> ReadFile(const std::wstring& path) {
    std::vector<uint8_t> out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(static_cast<size_t>(n));
        if (fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    fclose(f);
    return out;
}

// Records the work of one set: first its orig and deltas, then (unless unloading) the blend, or the original back.
// States: buffers start every list in COMMON (made in it, and they decay to it after each ExecuteCommandLists), so
// first uses are implicit promotions (copy, shader read, UAV); explicit barriers only within the list.
// A new set reads the mesh's original vertices only once the game has drawn the mesh and two more frames passed: a
// mesh loaded a moment before (a body copied for a scene) had its vertices still on the way to the GPU on an AMD
// card, the set took that for the original and the body stood in spikes (tester 2, RX 9070). Without the
// draw hooks, or after 300 frames, it reads as before. False: not yet (nothing recorded).
bool ReadyToRead(Set& s) {
    if (s.watch < 0) return true;
    if (DeformWatchSeen(s.watch) && ++s.sinceSeen >= 2) {
        DeformWatchEnd(s.watch);
        s.watch = -1;
        Log("blendshape: set %d: the game drew its mesh, the set reads it now (%d frames after the load)", s.id,
            s.waited + s.sinceSeen);
        return true;
    }
    if (!DeformWatchSeen(s.watch)) ++s.waited;
    if (s.waited >= 300 || !DeformDrawHooksLive()) {
        if (s.waited >= 300) Log("blendshape: set %d: no draw of its mesh seen in 300 frames - reading it now", s.id);
        DeformWatchEnd(s.watch);
        s.watch = -1;
        return true;
    }
    return false;
}

// A live mesh: once the game has drawn the copy, each chunk's streams 0 and 2 are copied to orig (bone indices and
// weights, tangents) and an upload buffer is made; then each chunk with a new write (all of them after a new box) goes
// through kLiveCS into work and from there into the game's buffer. orig and the upload are kept for the next frames.
void RecordLive(ID3D12GraphicsCommandList* l, Set& s, int slot) {
    if (!s.ready && !ReadyToRead(s)) {
        if (s.unload) s.lost = true;
        s.dirty = true;
        return;
    }
    bool fresh = !s.ready;
    if (!s.ready) {
        for (Chunk& c : s.chunks) {
            uint64_t bytes = 24ull * c.count;
            c.orig = MakeBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
            c.work = MakeBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            c.upload = MakeBuffer(kSlots * bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!c.orig || !c.work || !c.upload || FAILED(c.upload->Map(0, nullptr, reinterpret_cast<void**>(&c.upMap)))) {
                Log("blendshape: live mesh %d: GPU buffers failed (%u vertices in chunk %u)", s.id, c.count, c.index);
                s.lost = true;
                return;
            }
            l->CopyBufferRegion(c.orig, 0, s.res, c.off0, 16ull * c.count);
            l->CopyBufferRegion(c.orig, 16ull * c.count, s.res, c.off2, 8ull * c.count);
            Barrier(l, c.orig, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        s.ready = true;
        s.boxNew = true;  // the chunks nobody writes are stored in the live box too
        Log("blendshape: live mesh %d ready (%zu chunks)", s.id, s.chunks.size());
    }
    if (s.unload) {
        for (Chunk& c : s.chunks) {
            if (fresh) Barrier(l, c.orig, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            l->CopyBufferRegion(s.res, c.off0, c.orig, 0, 16ull * c.count);
            l->CopyBufferRegion(s.res, c.off2, c.orig, 16ull * c.count, 8ull * c.count);
        }
        return;
    }
    for (Chunk& c : s.chunks) {
        if (!c.liveNew && !s.boxNew) continue;
        l->SetComputeRootSignature(g_root);
        l->SetPipelineState(g_livePso);
        const bool pos = !c.live.empty(), nrm = pos && c.liveNormals;
        const uint64_t at = slot * 24ull * c.count;
        if (pos) memcpy(c.upMap + at, c.live.data(), (nrm ? 24ull : 12ull) * c.count);
        float k[20] = {};
        uint32_t flags = (pos ? 1u : 0u) | (nrm ? 2u : 0u);
        memcpy(&k[0], &c.count, 4);
        memcpy(&k[1], &flags, 4);
        for (int a = 0; a < 3; ++a) {
            k[4 + a] = s.box0.s[a];
            k[8 + a] = s.box0.o[a];
            k[12 + a] = s.box1.s[a];
            k[16 + a] = s.box1.o[a];
        }
        const D3D12_GPU_VIRTUAL_ADDRESS ova = c.orig->GetGPUVirtualAddress();
        l->SetComputeRoot32BitConstants(0, 20, k, 0);
        l->SetComputeRootShaderResourceView(1, ova);
        l->SetComputeRootShaderResourceView(2, pos ? c.upload->GetGPUVirtualAddress() + at : ova);
        l->SetComputeRootShaderResourceView(3, ova);
        l->SetComputeRootUnorderedAccessView(4, c.work->GetGPUVirtualAddress());
        l->Dispatch((c.count + 63) / 64, 1, 1);
        const bool nn = s.normalsMode && RecordNormals(l, s, c, fresh);
        Barrier(l, c.work, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyBufferRegion(s.res, c.off0, c.work, 0, 16ull * c.count);
        if (nrm || nn) l->CopyBufferRegion(s.res, c.off2, c.work, 16ull * c.count, 8ull * c.count);
        else if (c.normalsWritten) l->CopyBufferRegion(s.res, c.off2, c.orig, 16ull * c.count, 8ull * c.count);
        c.normalsWritten = nn;
        c.liveNew = false;
    }
    s.boxNew = false;
}

void RecordSet(ID3D12GraphicsCommandList* l, Set& s, uint64_t& weightAt, int slot) {
    if (s.live) {
        RecordLive(l, s, slot);
        return;
    }
    if (!s.ready && !ReadyToRead(s)) {
        if (s.unload) s.lost = true;  // unloaded before it wrote anything: it ends without a write
        s.dirty = true;              // the blend comes when it is ready
        return;
    }
    bool fresh = !s.ready;  // orig and deltas were written in this list: they are in NON_PIXEL_SHADER_RESOURCE
    if (!s.ready) {
        for (Chunk& c : s.chunks) {
            uint64_t bytes = 24ull * c.count, dbytes = c.data.size() * sizeof(float);  // stream 0 + stream 2 always
            c.orig = MakeBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
            c.deltas = MakeBuffer(dbytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
            c.work = MakeBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            ID3D12Resource* staging = MakeBuffer(dbytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            void* map = nullptr;
            if (!c.orig || !c.deltas || !c.work || !staging || FAILED(staging->Map(0, nullptr, &map))) {
                Log("blendshape: set %d: GPU buffers failed (%llu bytes of deltas)", s.id,
                    static_cast<unsigned long long>(dbytes));
                if (staging) staging->Release();
                s.lost = true;
                return;
            }
            if (dbytes) memcpy(map, c.data.data(), dbytes);
            staging->Unmap(0, nullptr);
            l->CopyBufferRegion(c.deltas, 0, staging, 0, dbytes);
            Retire(staging, true);
            // the game's buffer: COMMON while not in use, promoted to the copy states implicitly
            l->CopyBufferRegion(c.orig, 0, s.res, c.off0, 16ull * c.count);
            l->CopyBufferRegion(c.orig, 16ull * c.count, s.res, c.off2, 8ull * c.count);
            Barrier(l, c.orig, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(l, c.deltas, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            std::vector<float>().swap(c.data);
        }
        s.ready = true;
    }
    if (s.unload) {
        for (Chunk& c : s.chunks) {
            if (fresh) Barrier(l, c.orig, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            l->CopyBufferRegion(s.res, c.off0, c.orig, 0, 16ull * c.count);
            l->CopyBufferRegion(s.res, c.off2, c.orig, 16ull * c.count, 8ull * c.count);
        }
        return;
    }
    // the weights that are not 0, as (shape, weight) pairs in this slot's part of the upload buffer
    uint32_t active = 0;
    uint64_t at = weightAt;
    for (uint32_t k = 0; k < s.weights.size(); ++k) {
        if (s.weights[k] == 0 || at + 8 > kWeightBytes) continue;
        uint32_t pair[2] = {k, 0};
        memcpy(&pair[1], &s.weights[k], 4);
        memcpy(g_weightMap + slot * kWeightBytes + at, pair, 8);
        at += 8;
        ++active;
    }
    D3D12_GPU_VIRTUAL_ADDRESS wva = g_weightBuf->GetGPUVirtualAddress() + slot * kWeightBytes + weightAt;
    weightAt = at;
    for (Chunk& c : s.chunks) {
        l->SetComputeRootSignature(g_root);
        l->SetPipelineState(g_pso);
        float k[20] = {};
        uint32_t normals = s.normals ? 1 : 0, shapes = static_cast<uint32_t>(s.weights.size());
        memcpy(&k[0], &c.count, 4);
        memcpy(&k[1], &active, 4);
        memcpy(&k[2], &normals, 4);
        memcpy(&k[3], &shapes, 4);
        for (int a = 0; a < 3; ++a) {
            k[4 + a] = s.box0.s[a];
            k[8 + a] = s.box0.o[a];
            k[12 + a] = s.box1.s[a];
            k[16 + a] = s.box1.o[a];
        }
        l->SetComputeRoot32BitConstants(0, 20, k, 0);
        l->SetComputeRootShaderResourceView(1, c.orig->GetGPUVirtualAddress());
        l->SetComputeRootShaderResourceView(2, c.deltas->GetGPUVirtualAddress());
        l->SetComputeRootShaderResourceView(3, wva);
        l->SetComputeRootUnorderedAccessView(4, c.work->GetGPUVirtualAddress());
        l->Dispatch((c.count + 63) / 64, 1, 1);
        const bool nn = s.normalsMode && RecordNormals(l, s, c, fresh);
        Barrier(l, c.work, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyBufferRegion(s.res, c.off0, c.work, 0, 16ull * c.count);
        if (s.normals || nn) l->CopyBufferRegion(s.res, c.off2, c.work, 16ull * c.count, 8ull * c.count);
        else if (c.normalsWritten) l->CopyBufferRegion(s.res, c.off2, c.orig, 16ull * c.count, 8ull * c.count);
        c.normalsWritten = nn;
    }
}

void RecordDump(ID3D12GraphicsCommandList* l, Dump& d) {
    if (d.raw) {
        d.readback = MakeBuffer(d.rawSize, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        if (d.readback) l->CopyBufferRegion(d.readback, 0, d.res, d.rawOff, d.rawSize);
        return;
    }
    uint64_t total = 0;
    for (auto& p : d.parts) {
        p.at = total;
        total += 24ull * p.count;
    }
    d.readback = MakeBuffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!d.readback) return;
    for (auto& p : d.parts) {
        l->CopyBufferRegion(d.readback, p.at, d.res, p.off0, 16ull * p.count);
        l->CopyBufferRegion(d.readback, p.at + 16ull * p.count, d.res, p.off2, 8ull * p.count);
    }
}

// A dump whose copy is done: positions and normals decoded, written as TWDB.
void FinishDump(Dump& d) {
    uint8_t* m = nullptr;
    if (!d.readback || FAILED(d.readback->Map(0, nullptr, reinterpret_cast<void**>(&m)))) {
        Log("blendshape: dump failed (no readback)");
        return;
    }
    if (d.raw && d.done) {
        d.done(m, d.rawSize);
        d.readback->Unmap(0, nullptr);
        return;
    }
    if (d.raw) {  // "TWIB", u32 head bytes, head, u64 bytes, bytes
        const size_t slash = d.path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            std::error_code ec;
            std::filesystem::create_directories(d.path.substr(0, slash), ec);
        }
        FILE* f = _wfopen(d.path.c_str(), L"wb");
        if (f) {
            uint32_t h[2] = {0x42495754, static_cast<uint32_t>(d.head.size())};
            fwrite(h, 4, 2, f);
            fwrite(d.head.data(), 1, d.head.size(), f);
            fwrite(&d.rawSize, 8, 1, f);
            fwrite(m, 1, d.rawSize, f);
            fclose(f);
            Log("blendshape: index dump written (%llu bytes)", static_cast<unsigned long long>(d.rawSize));
        }
        d.readback->Unmap(0, nullptr);
        return;
    }
    // its folder first (a dump can go into a folder that is not there yet)
    const size_t slash = d.path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::error_code ec;
        std::filesystem::create_directories(d.path.substr(0, slash), ec);
    }
    FILE* f = _wfopen(d.path.c_str(), L"wb");
    if (f) {
        uint32_t head[3] = {0x42445754, 1, static_cast<uint32_t>(d.parts.size())};  // "TWDB"
        fwrite(head, 4, 3, f);
        for (auto& p : d.parts) {
            uint32_t ph[3] = {p.index, p.mask, p.count};
            fwrite(ph, 4, 3, f);
            std::vector<float> pos(3ull * p.count), nrm(3ull * p.count);
            for (uint32_t i = 0; i < p.count; ++i) {
                uint32_t v[2], n;
                memcpy(v, m + p.at + 16ull * i, 8);
                memcpy(&n, m + p.at + 16ull * p.count + 8ull * i, 4);
                uint32_t u[3] = {v[0] & 0xffff, v[0] >> 16, v[1] & 0xffff};
                uint32_t q[3] = {n & 1023, (n >> 10) & 1023, (n >> 20) & 1023};
                for (int k = 0; k < 3; ++k) {
                    pos[3 * i + k] = u[k] / 65535.0f * d.box.s[k] + d.box.o[k];
                    nrm[3 * i + k] = q[k] / 1023.0f * 2 - 1;
                }
            }
            fwrite(pos.data(), 4, pos.size(), f);
            fwrite(nrm.data(), 4, nrm.size(), f);
        }
        fclose(f);
        Log("blendshape: dump written (%zu chunks)", d.parts.size());
    } else {
        Log("blendshape: dump: cannot write the file");
    }
    d.readback->Unmap(0, nullptr);
}

// The index buffer of a render mesh: its pool id, the resource and where the mesh's indices start in it.
bool MeshIndices(uintptr_t rm, uint32_t* id, ID3D12Resource** res, uint64_t* base, uint32_t* size) {
    uint32_t ib = 0;
    uint64_t dd = 0, va = 0, r = 0, rva = 0, width = 0;
    if (!SafeRead(reinterpret_cast<void*>(rm + 0xa4), &ib, 4) || !ib || !PoolKnown() ||
        !SafeRead(reinterpret_cast<void*>(ImageBase() + g_poolPtr), &dd, 8) || !dd)
        return false;
    uintptr_t e = dd + (ib - 1ull) * kPoolStride + g_poolEntries;
    if (!SafeRead(reinterpret_cast<void*>(e), size, 4) || !SafeRead(reinterpret_cast<void*>(e + 0x10), &va, 8) ||
        !SafeRead(reinterpret_cast<void*>(e + 0x18), &r, 8) || !r || !LooksLikeObject(reinterpret_cast<void*>(r)) ||
        !ResourceRange(reinterpret_cast<ID3D12Resource*>(r), &rva, &width) || va < rva || va + *size > rva + width)
        return false;
    *id = ib;
    *res = reinterpret_cast<ID3D12Resource*>(r);
    *base = va - rva;
    return true;
}

bool HideStillThere(const Hide& h) {
    uint32_t id = 0, size = 0;
    ID3D12Resource* r = nullptr;
    uint64_t base = 0;
    return MeshIndices(h.rm, &id, &r, &base, &size) && id == h.ibId && r == h.ibRes && base == h.ibBase;
}

void FreeHide(Hide& h) {
    for (auto& e : h.chunks) {
        Retire(e.second.orig);
        Retire(e.second.work);
        Retire(e.second.mask);
        e.second.orig = e.second.work = e.second.mask = nullptr;
    }
    if (h.watch >= 0) DeformWatchEnd(h.watch);
    h.watch = -1;
    Retire(h.ibRes);
    h.ibRes = nullptr;
    if (h.heldRm) RetireRenderMesh(h.rm);
    h.heldRm = false;
}

// Records a hide's work: once the game has drawn the mesh, each chunk's original indices into orig; then each chunk
// with a new bit field through kHideCS into the game's buffer (or orig back when it hides nothing); unloading writes
// every chunk's orig back.
void RecordHide(ID3D12GraphicsCommandList* l, Hide& h) {
    if (!h.ready) {
        if (h.watch >= 0) {
            if (DeformWatchSeen(h.watch) && ++h.sinceSeen >= 2) {
                DeformWatchEnd(h.watch);
                h.watch = -1;
            } else {
                if (!DeformWatchSeen(h.watch)) ++h.waited;
                if (h.waited < 300 && DeformDrawHooksLive()) {
                    if (h.unload) h.lost = true;  // nothing written yet: it ends as it is
                    return;
                }
                DeformWatchEnd(h.watch);
                h.watch = -1;
            }
        }
    }
    for (auto& e : h.chunks) {  // the original indices of chunks met for the first time
        HideChunk& c = e.second;
        if (c.orig) continue;
        uint64_t bytes = (2ull * c.icount + 3) & ~3ull;
        c.orig = MakeBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        c.work = MakeBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!c.orig || !c.work) {
            Log("blendshape: hide %d: GPU buffers failed (chunk %u)", h.id, c.index);
            h.lost = true;
            return;
        }
        l->CopyBufferRegion(c.orig, 0, h.ibRes, c.ioff, 2ull * c.icount);
        Barrier(l, c.orig, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        c.newOrig = true;
    }
    if (!h.ready) Log("blendshape: hide %d ready (%zu chunks)", h.id, h.chunks.size());
    h.ready = true;
    for (auto& e : h.chunks) {
        HideChunk& c = e.second;
        if (h.unload || (c.fresh && !c.any)) {  // the original back
            if (!c.written) { c.fresh = false; continue; }
            if (c.newOrig) Barrier(l, c.orig, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            l->CopyBufferRegion(h.ibRes, c.ioff, c.orig, 0, 2ull * c.icount);
            if (c.newOrig) Barrier(l, c.orig, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            c.written = false;
            c.fresh = false;
            continue;
        }
        if (!c.fresh) continue;
        ID3D12Resource* mask = MakeBuffer(c.bits.size(), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* map = nullptr;
        if (!mask || FAILED(mask->Map(0, nullptr, &map))) {
            if (mask) mask->Release();
            Log("blendshape: hide %d: bit field buffer failed", h.id);
            continue;
        }
        memcpy(map, c.bits.data(), c.bits.size());
        mask->Unmap(0, nullptr);
        Retire(c.mask, true);  // the last one may still be read by a list in flight
        c.mask = mask;
        uint32_t words = (c.icount + 1) / 2;
        float k[20] = {};
        memcpy(&k[0], &words, 4);
        memcpy(&k[1], &c.icount, 4);
        l->SetComputeRootSignature(g_root);
        l->SetPipelineState(g_hidePso);
        l->SetComputeRoot32BitConstants(0, 20, k, 0);
        l->SetComputeRootShaderResourceView(1, c.orig->GetGPUVirtualAddress());
        l->SetComputeRootShaderResourceView(2, c.mask->GetGPUVirtualAddress());
        l->SetComputeRootShaderResourceView(3, c.orig->GetGPUVirtualAddress());
        l->SetComputeRootUnorderedAccessView(4, c.work->GetGPUVirtualAddress());
        l->Dispatch((words + 63) / 64, 1, 1);
        Barrier(l, c.work, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyBufferRegion(h.ibRes, c.ioff, c.work, 0, 2ull * c.icount);
        c.written = true;
        c.fresh = false;
    }
    for (auto& e : h.chunks) e.second.newOrig = false;  // the next list finds them in COMMON
}

}  // namespace

int BlendLoad(uintptr_t rm, const std::wstring& path, float range, std::string* err) {
    std::vector<uint8_t> f = ReadFile(path);
    size_t at = 0;
    auto u32 = [&](uint32_t* v) {
        if (at + 4 > f.size()) return false;
        memcpy(v, f.data() + at, 4);
        at += 4;
        return true;
    };
    uint32_t magic = 0, version = 0, shapes = 0, nChunks = 0, flags = 0;
    if (f.empty()) { *err = "cannot read the file"; return 0; }
    if (!u32(&magic) || magic != 0x46445754 || !u32(&version) || (version != 1 && version != 2) || !u32(&shapes) ||
        !u32(&nChunks) || (version == 2 && !u32(&flags))) {
        *err = "not a TWDF file (version 1 or 2)";
        return 0;
    }
    auto set = std::make_unique<Set>();
    set->normals = (flags & 1) != 0;
    for (uint32_t k = 0; k < shapes; ++k) {
        if (at + 2 > f.size()) { *err = "file ends in the names"; return 0; }
        uint16_t n = 0;
        memcpy(&n, f.data() + at, 2);
        at += 2;
        if (at + n > f.size()) { *err = "file ends in the names"; return 0; }
        set->names.emplace_back(reinterpret_cast<const char*>(f.data() + at), n);
        at += n;
    }
    set->weights.assign(shapes, 0.0f);
    uint32_t size = 0;
    uint64_t base = 0;
    uint64_t arr = 0;
    uint32_t meshChunks = 0;
    std::string why;
    if (!MeshBuffer(rm, &set->vbId, &set->res, &base, &size, &why)) {
        Log("blendshape: load: %s", why.c_str());
        *err = "the mesh's vertex buffer was not found: " + why;
        return 0;
    }
    if (!ReadBox(rm, &set->box0) || !SafeRead(reinterpret_cast<void*>(rm + 0xa8), &arr, 8) ||
        !SafeRead(reinterpret_cast<void*>(rm + 0xb0), &meshChunks, 4)) {
        *err = "not a loaded render mesh";
        return 0;
    }
    {
        std::lock_guard<std::mutex> l(g_mx);
        for (auto& e : g_sets)
            if (e.second->rm == rm && !e.second->unload) {
                *err = "the mesh has a set already (unload it first)";
                return 0;
            }
    }
    float grow[3] = {0, 0, 0};  // how far a vertex can move per axis: range * sum of |delta| over the shapes
    for (uint32_t i = 0; i < nChunks; ++i) {
        Chunk c;
        if (!u32(&c.index) || !u32(&c.count)) { *err = "file ends in a chunk header"; return 0; }
        uint32_t off0 = 0, off2 = 0, count = 0;
        uintptr_t ca = arr + c.index * kChunkSize;
        if (c.index >= meshChunks || !SafeRead(reinterpret_cast<void*>(ca + 0x04), &off0, 4) ||
            !SafeRead(reinterpret_cast<void*>(ca + 0x0c), &off2, 4) ||
            !SafeRead(reinterpret_cast<void*>(ca + 0x20), &count, 4)) {
            *err = "chunk " + std::to_string(c.index) + " is not in the mesh (" + std::to_string(meshChunks) + " chunks)";
            return 0;
        }
        if (count != c.count) {
            *err = "chunk " + std::to_string(c.index) + " has " + std::to_string(count) + " vertices, the file " +
                   std::to_string(c.count);
            return 0;
        }
        if (off0 + 16ull * count > size || (set->normals && off2 + 8ull * count > size)) {
            *err = "chunk outside the buffer";
            return 0;
        }
        size_t floats = 3ull * shapes * count, all = set->normals ? 2 * floats : floats;
        if (at + all * 4 > f.size()) { *err = "file ends in the deltas"; return 0; }
        c.data.resize(all);
        memcpy(c.data.data(), f.data() + at, all * 4);
        at += all * 4;
        c.off0 = base + off0;
        c.off2 = base + off2;
        std::vector<float> sum(3ull * count, 0.0f);
        for (uint32_t k = 0; k < shapes; ++k)
            for (size_t j = 0; j < 3ull * count; ++j) sum[j] += std::fabs(c.data[3ull * k * count + j]);
        for (size_t j = 0; j < sum.size(); ++j)
            if (sum[j] * range > grow[j % 3]) grow[j % 3] = sum[j] * range;
        set->chunks.push_back(std::move(c));
    }
    set->box1 = set->box0;
    for (int k = 0; k < 3; ++k) {
        set->box1.s[k] += 2 * grow[k];
        set->box1.o[k] -= grow[k];
    }
    set->rm = rm;
    FindIndices(rm, *set);  // DeformNormals
    set->res->AddRef();
    // the game's draw of this mesh is awaited before the set reads its vertices (ReadyToRead)
    set->watch = DeformWatchRange(set->res->GetGPUVirtualAddress() + base, set->res->GetGPUVirtualAddress() + base + size);
    set->heldRm = HoldRenderMesh(rm);
    if (!set->heldRm) Log("blendshape: the render mesh's reference count is not known for this build - the set does not hold it");
    InstallOverlay(true);  // its ExecuteCommandLists hook runs the work (installed on first use)
    std::lock_guard<std::mutex> l(g_mx);
    set->id = g_nextId++;
    int id = set->id;
    Log("blendshape: set %d loaded: %u shapes%s, %u chunks, box grown by %.3f %.3f %.3f m", id, shapes,
        (flags & 1) ? " with normals" : "", nChunks, grow[0], grow[1], grow[2]);
    g_sets[id] = std::move(set);
    return id;
}

bool BlendWeight(int id, int shape, float w) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_sets.find(id);
    if (it == g_sets.end() || shape < 0 || shape >= static_cast<int>(it->second->weights.size())) return false;
    if (it->second->weights[shape] != w) {
        it->second->weights[shape] = w;
        it->second->dirty = true;
    }
    return true;
}

int BlendWeights(int id, const float* w, int n) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_sets.find(id);
    if (it == g_sets.end() || !w || n < 0) return 0;
    Set& s = *it->second;
    int k = (std::min)(n, static_cast<int>(s.weights.size()));
    for (int i = 0; i < k; ++i)
        if (s.weights[i] != w[i]) {
            s.weights[i] = w[i];
            s.dirty = true;
        }
    return k;
}

int BlendShapeIndex(int id, const std::string& name) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_sets.find(id);
    if (it == g_sets.end()) return -1;
    for (size_t k = 0; k < it->second->names.size(); ++k)
        if (it->second->names[k] == name) return static_cast<int>(k);
    return -1;
}

std::vector<std::string> BlendNames(int id) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_sets.find(id);
    return it == g_sets.end() ? std::vector<std::string>() : it->second->names;
}

std::string BlendState(int id) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_sets.find(id);
    if (it == g_sets.end()) return g_resetIds.count(id) ? "lost" : "none";
    if (it->second->lost) return "lost";
    return it->second->ready ? "ready" : "loading";
}

bool BlendUnload(int id) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_sets.find(id);
    if (it == g_sets.end()) return g_resetIds.erase(id) > 0;
    it->second->unload = true;
    return true;
}

// The render mesh goes away (a copy given back): its sets end without writing the original back (after unload
// and w3.deform.release, the write-back of the next submit would go into a range the game may have handed on).
// How many sets that ended.
int BlendForget(uintptr_t rm) {
    std::lock_guard<std::mutex> l(g_mx);
    int n = 0;
    for (auto& e : g_sets)
        if (e.second->rm == rm) {
            if (e.second->live && !e.second->unload) g_resetIds.insert(e.first);  // "lost" until LiveClose
            e.second->lost = true;
            e.second->unload = true;
            ++n;
        }
    return n;
}

// w3.deform.hide (kHideCS): a hide per render mesh. HideOpen gives the mesh's hide (a new one if it has none).
int HideOpen(uintptr_t rm, std::string* err) {
    if (!g_hidePso && g_pso) {
        *err = "hiding triangles is off (its shader failed, see tw3se.log)";
        return 0;
    }
    {
        std::lock_guard<std::mutex> l(g_mx);
        for (auto& e : g_hides)
            if (e.second->rm == rm && !e.second->unload && !e.second->lost) return e.first;
    }
    auto h = std::make_unique<Hide>();
    if (!MeshIndices(rm, &h->ibId, &h->ibRes, &h->ibBase, &h->ibSize)) {
        *err = "the mesh's index buffer was not found (is the mesh loaded?)";
        return 0;
    }
    h->rm = rm;
    h->ibRes->AddRef();
    uint32_t vbId = 0, vbSize = 0;
    ID3D12Resource* vb = nullptr;
    uint64_t vbBase = 0;
    if (MeshBuffer(rm, &vbId, &vb, &vbBase, &vbSize))
        h->watch = DeformWatchRange(vb->GetGPUVirtualAddress() + vbBase, vb->GetGPUVirtualAddress() + vbBase + vbSize);
    h->heldRm = HoldRenderMesh(rm);
    InstallOverlay(true);
    std::lock_guard<std::mutex> l(g_mx);
    h->id = g_nextId++;
    int id = h->id;
    Log("blendshape: hide %d opened (render mesh %p)", id, reinterpret_cast<void*>(rm));
    g_hides[id] = std::move(h);
    return id;
}

// A chunk's bit field (triangle t: bit t % 8 of byte t / 8, 1 = hidden); an empty field shows the chunk whole.
bool HideBits(int id, uint32_t chunk, const uint8_t* bits, size_t n, std::string* err) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_hides.find(id);
    if (it == g_hides.end() || it->second->unload) {
        *err = "no such hide";
        return false;
    }
    Hide& h = *it->second;
    uint64_t arr = 0;
    uint32_t meshChunks = 0, off = 0, icount = 0;
    if (!SafeRead(reinterpret_cast<void*>(h.rm + 0xa8), &arr, 8) ||
        !SafeRead(reinterpret_cast<void*>(h.rm + 0xb0), &meshChunks, 4) || chunk >= meshChunks ||
        !SafeRead(reinterpret_cast<void*>(arr + chunk * kChunkSize + 0x1c), &off, 4) ||
        !SafeRead(reinterpret_cast<void*>(arr + chunk * kChunkSize + 0x24), &icount, 4) || icount % 3 ||
        off + 2ull * icount > h.ibSize) {
        *err = "chunk " + std::to_string(chunk) + " is not in the mesh";
        return false;
    }
    uint32_t tris = icount / 3;
    if (n > (tris + 7) / 8) {
        *err = "the bit field is longer than the chunk's " + std::to_string(tris) + " triangles";
        return false;
    }
    HideChunk& c = h.chunks[chunk];
    c.index = chunk;
    c.icount = icount;
    c.ioff = h.ibBase + off;
    c.bits.assign(((tris + 31) / 32) * 4, 0);
    if (n) memcpy(c.bits.data(), bits, n);
    c.any = false;
    for (uint8_t b : c.bits) c.any = c.any || b;
    c.fresh = true;
    h.dirty = true;
    return true;
}

bool HideClose(int id) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_hides.find(id);
    if (it == g_hides.end()) return false;
    it->second->unload = true;
    return true;
}

std::string HideState(int id) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_hides.find(id);
    if (it == g_hides.end()) return "none";
    const Hide& h = *it->second;
    return h.lost ? "lost" : (h.ready && !h.dirty) ? "ready" : "loading";
}

int HideForget(uintptr_t rm) {  // the render mesh goes away: its hide ends without writing the indices back
    std::lock_guard<std::mutex> l(g_mx);
    int n = 0;
    for (auto& e : g_hides)
        if (e.second->rm == rm) {
            e.second->lost = true;
            e.second->unload = true;
            ++n;
        }
    return n;
}

// Live meshes: a set whose positions come from the plugin (interface 26). LiveNew makes it before the component's
// copy has its render mesh (state "loading"); w3.deform.live attaches it with LiveAttach once the copy is loaded.
int LiveNew() {
    InstallOverlay(true);
    auto set = std::make_unique<Set>();
    set->live = true;
    set->normals = true;  // orig holds stream 2 too (RestoreForReset copies it back)
    std::lock_guard<std::mutex> l(g_mx);
    set->id = g_nextId++;
    int id = set->id;
    g_sets[id] = std::move(set);
    return id;
}

bool LiveAttach(int id, uintptr_t rm, std::string* err) {
    uint32_t vbId = 0, size = 0, meshChunks = 0;
    ID3D12Resource* res = nullptr;
    uint64_t base = 0, arr = 0;
    Box box0;
    std::string why;
    if (!MeshBuffer(rm, &vbId, &res, &base, &size, &why)) {
        *err = "the mesh's vertex buffer was not found: " + why;
        return false;
    }
    if (!ReadBox(rm, &box0) || !SafeRead(reinterpret_cast<void*>(rm + 0xa8), &arr, 8) ||
        !SafeRead(reinterpret_cast<void*>(rm + 0xb0), &meshChunks, 4) || !arr || !meshChunks || meshChunks > 256) {
        *err = "not a loaded render mesh";
        return false;
    }
    std::vector<Chunk> chunks(meshChunks);
    for (uint32_t i = 0; i < meshChunks; ++i) {
        uint32_t off0 = 0, off2 = 0, count = 0;
        uintptr_t ca = arr + i * kChunkSize;
        if (!SafeRead(reinterpret_cast<void*>(ca + 0x04), &off0, 4) || !SafeRead(reinterpret_cast<void*>(ca + 0x0c), &off2, 4) ||
            !SafeRead(reinterpret_cast<void*>(ca + 0x20), &count, 4)) {
            *err = "chunk " + std::to_string(i) + " unreadable";
            return false;
        }
        if (off0 + 16ull * count > size || off2 + 8ull * count > size || count > kLiveMaxVertices) {
            *err = "chunk " + std::to_string(i) + " outside the buffer or above " + std::to_string(kLiveMaxVertices) +
                   " vertices";
            return false;
        }
        chunks[i].index = i;
        chunks[i].count = count;
        chunks[i].off0 = base + off0;
        chunks[i].off2 = base + off2;
    }
    {
        std::lock_guard<std::mutex> l(g_mx);
        auto it = g_sets.find(id);
        if (it == g_sets.end() || !it->second->live || it->second->rm || it->second->lost) {
            *err = "the live mesh was closed or is attached already";
            return false;
        }
        for (auto& e : g_sets)
            if (e.second->rm == rm && !e.second->unload) {
                *err = "the mesh has a set already";
                return false;
            }
        Set& s = *it->second;
        s.vbId = vbId;
        s.box0 = box0;
        if (!s.boxSet) s.box1 = box0;
        s.chunks = std::move(chunks);
        for (auto& e : s.early) {
            if (e.first >= s.chunks.size() || e.second.count != s.chunks[e.first].count) {
                Log("blendshape: live mesh %d: an early write to chunk %u (%u vertices) does not fit the mesh, dropped", id,
                    e.first, e.second.count);
                continue;
            }
            Chunk& c = s.chunks[e.first];
            c.live = std::move(e.second.v);
            c.liveNormals = e.second.normals;
            c.liveNew = true;
        }
        s.early.clear();
        s.res = res;
        s.res->AddRef();
        s.watch = DeformWatchRange(res->GetGPUVirtualAddress() + base, res->GetGPUVirtualAddress() + base + size);
        s.heldRm = HoldRenderMesh(rm);
        s.dirty = true;
        FindIndices(rm, s);  // DeformNormals
        s.rm = rm;  // last: the submit hook takes the set from here on
        Log("blendshape: live mesh %d on render mesh %p: %u chunks, box %.3f %.3f %.3f m", id, reinterpret_cast<void*>(rm),
            meshChunks, s.box1.s[0], s.box1.s[1], s.box1.s[2]);
    }
    return true;
}

// DeformNormals (interface 26): 1 = the set's or live mesh's normals and tangents follow its moved triangles (the
// authored ones turned with them), 0 = as before (the file's normal deltas, the plugin's normals, or unchanged).
bool BlendNormals(int id, int mode, std::string* err) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_sets.find(id);
    if (it == g_sets.end()) {
        *err = "no such set or live mesh";
        return false;
    }
    Set& s = *it->second;
    if (s.rm && !s.ibRes && mode) FindIndices(s.rm, s);  // (a copy's index buffer comes a little after its vertices)
    if (s.rm && !s.ibRes && mode) {
        *err = "the mesh's index buffer was not found";
        return false;
    }
    if (s.normalsMode != mode) {
        s.normalsMode = mode;
        s.dirty = true;
        if (s.live) s.boxNew = true;
    }
    return true;
}

bool LiveFail(int id) {  // the copy never loaded (its figure went): the live mesh is lost
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_sets.find(id);
    if (it == g_sets.end() || !it->second->live) return false;
    it->second->lost = true;
    return true;
}

// Ends the live mesh without a write into the copy (the copy goes back to the game after this).
bool LiveClose(int id) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_sets.find(id);
    bool wasLost = g_resetIds.erase(id) > 0;
    if (it == g_sets.end()) return wasLost;
    if (!it->second->live) return false;
    it->second->lost = true;
    it->second->unload = true;
    if (!it->second->rm) {
        FreeSet(*it->second);
        g_sets.erase(it);
    }
    return true;
}

bool LiveInfo(int id, int chunk, int* count, int* chunks) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_sets.find(id);
    if (it == g_sets.end() || !it->second->live || !it->second->rm) return false;
    const Set& s = *it->second;
    if (chunks) *chunks = static_cast<int>(s.chunks.size());
    if (chunk < 0 || chunk >= static_cast<int>(s.chunks.size())) return false;
    if (count) *count = static_cast<int>(s.chunks[chunk].count);
    return true;
}

bool LiveWrite(int id, int chunk, const float* pos, const float* nrm, int count, std::string* err) {
    if (!pos || count <= 0 || static_cast<uint32_t>(count) > kLiveMaxVertices || chunk < 0) {
        *err = "no positions, or a count of 0 or above " + std::to_string(kLiveMaxVertices);
        return false;
    }
    const size_t n = 3ull * count;
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_sets.find(id);
    if (it == g_sets.end() || !it->second->live) {
        *err = g_resetIds.count(id) ? "the live mesh is lost (a graphics rebuild): close it and open it again"
                                    : "no such live mesh";
        return false;
    }
    Set& s = *it->second;
    if (s.lost || s.unload) {
        *err = "the live mesh is lost: close it and open it again";
        return false;
    }
    std::vector<float>* v = nullptr;
    if (!s.rm) {
        LiveWriteEarly& e = s.early[static_cast<uint32_t>(chunk)];
        e.count = static_cast<uint32_t>(count);
        e.normals = nrm != nullptr;
        v = &e.v;
    } else {
        if (chunk >= static_cast<int>(s.chunks.size()) || s.chunks[chunk].count != static_cast<uint32_t>(count)) {
            *err = chunk >= static_cast<int>(s.chunks.size())
                       ? "chunk " + std::to_string(chunk) + " is not in the mesh (" + std::to_string(s.chunks.size()) + " chunks)"
                       : "chunk " + std::to_string(chunk) + " has " + std::to_string(s.chunks[chunk].count) + " vertices, not " +
                             std::to_string(count);
            return false;
        }
        Chunk& c = s.chunks[chunk];
        c.liveNormals = nrm != nullptr;
        c.liveNew = true;
        v = &c.live;
    }
    v->resize(nrm ? 2 * n : n);
    memcpy(v->data(), pos, n * sizeof(float));
    if (nrm) memcpy(v->data() + n, nrm, n * sizeof(float));
    s.dirty = true;
    return true;
}

bool LiveBounds(int id, const float* mn, const float* mx) {
    if (!mn || !mx) return false;
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_sets.find(id);
    if (it == g_sets.end() || !it->second->live || it->second->lost) return false;
    Set& s = *it->second;
    Box b;
    for (int k = 0; k < 3; ++k) {
        b.s[k] = (std::max)(mx[k] - mn[k], 1e-4f);
        b.o[k] = mn[k];
    }
    if (s.boxSet && !memcmp(&b, &s.box1, sizeof b)) return true;
    s.box1 = b;
    s.boxSet = true;
    s.boxNew = true;
    s.dirty = true;
    return true;
}

// Before the game rebuilds its render resources (a graphics option applied through the options, path tracing among
// them): with sets loaded, switching path tracing in the menu crashed the game (the
// engine walked a list with a broken pointer inside CInGameConfigWrapper::SetVarValue), without sets it did not.
// Every set gives its mesh the original back (RestoreForReset) and ends, the GPU finishes what TW3SE sent (at most
// 1 s), and every reference TW3SE holds goes back now, on the main thread, before the rebuild (not at a later
// submit). How many sets ended.
// Waits (at most `ms`) until the GPU has done TW3SE's lists up to `value`. Under g_mx.
void WaitFence(uint64_t value, DWORD ms) {
    if (!g_fence || g_fence->GetCompletedValue() >= value) return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev && SUCCEEDED(g_fence->SetEventOnCompletion(value, ev))) WaitForSingleObject(ev, ms);
    if (ev) CloseHandle(ev);
}

// The render meshes outlive the rebuild (the same meshes and buffers after a switch of path tracing): what a set
// wrote stayed in them, the blend and the grown box. The mod's next set took that for the original and added its
// deltas once more, each switch baked the weights of the moment in (holes in the mesh).
// Each set that wrote copies its original back now, on the main thread through the game's queue (the queue is free
// threaded; t_inSubmit keeps the submit hook out), and its mesh gets the box it had. How many meshes got it.
int RestoreForReset() {
    if (!g_queue || !g_pso) return 0;
    WaitFence(g_fenceValue, 1000);
    const int slot = g_slot;
    if (g_fence->GetCompletedValue() < g_slotFence[slot]) return 0;
    std::vector<std::pair<uintptr_t, Box>> boxes;
    ID3D12GraphicsCommandList* l = g_list[slot];
    g_alloc[slot]->Reset();
    l->Reset(g_alloc[slot], nullptr);
    for (auto& e : g_sets) {
        Set& s = *e.second;
        if (s.lost || !s.ready || !StillThere(s)) continue;
        for (Chunk& c : s.chunks) {
            if (!c.orig) continue;
            l->CopyBufferRegion(s.res, c.off0, c.orig, 0, 16ull * c.count);
            l->CopyBufferRegion(s.res, c.off2, c.orig, 16ull * c.count, 8ull * c.count);
        }
        boxes.push_back({s.rm, s.box0});
    }
    l->Close();
    if (boxes.empty()) return 0;
    ID3D12CommandList* lists[] = {l};
    t_inSubmit = true;
    g_queue->ExecuteCommandLists(1, lists);
    t_inSubmit = false;
    g_queue->Signal(g_fence, ++g_fenceValue);
    g_slotFence[slot] = g_fenceValue;
    g_slot = (slot + 1) % kSlots;
    WaitFence(g_fenceValue, 1000);
    for (auto& b : boxes) SetBox(b.first, b.second);
    return static_cast<int>(boxes.size());
}

int BlendResetAll() {
    std::vector<uintptr_t> rms;
    int n = 0, restored = 0;
    {
        std::lock_guard<std::mutex> l(g_mx);
        restored = RestoreForReset();
        for (auto& e : g_sets) {
            g_resetIds.insert(e.first);
            e.second->lost = true;
            e.second->unload = true;
            FreeSet(*e.second);
            ++n;
        }
        g_sets.clear();
        WaitFence(g_fenceValue, 1000);
        uint64_t done = g_fence ? g_fence->GetCompletedValue() : ~0ull;
        for (size_t i = 0; i < g_retire.size();) {
            if (g_retire[i].first <= done) {
                g_retire[i].second->Release();
                g_retire[i] = g_retire.back();
                g_retire.pop_back();
            } else {
                ++i;
            }
        }
        for (size_t i = 0; i < g_retireRm.size();) {
            if (g_retireRm[i].first <= done) {
                rms.push_back(g_retireRm[i].second);
                g_retireRm[i] = g_retireRm.back();
                g_retireRm.pop_back();
            } else {
                ++i;
            }
        }
    }
    for (uintptr_t rm : rms) ReleaseRenderMesh(reinterpret_cast<void*>(rm));  // the game's destructor: outside g_mx
    if (n || !rms.empty())
        Log("blendshape: %d set(s) ended before the game rebuilds its render resources, %d mesh(es) got their original "
            "vertices and box back", n, restored);
    return n;
}

// The GPU address range of a render mesh's vertex data (the skinning hook knows a mesh's draws by it).
bool MeshVertexRange(uintptr_t rm, uint64_t* va, uint64_t* size) {
    uint32_t id = 0, sz = 0;
    ID3D12Resource* res = nullptr;
    uint64_t base = 0, rva = 0, width = 0;
    if (!PoolKnown() || !MeshBuffer(rm, &id, &res, &base, &sz) || !ResourceRange(res, &rva, &width)) return false;
    *va = rva + base;
    *size = sz;
    return true;
}

// The game's GPU device data (the block the buffer pool lies in), 0 when not found.
uintptr_t GpuDeviceData() {
    uintptr_t dd = 0;
    if (!PoolKnown() || !SafeRead(reinterpret_cast<void*>(ImageBase() + g_poolPtr), &dd, 8)) return 0;
    return dd;
}

// Where a render mesh's vertex data starts in its resource (the ray tracing skinning reads the resource from 0).
bool MeshVertexBase(uintptr_t rm, uint64_t* base) {
    uint32_t id = 0, sz = 0;
    ID3D12Resource* res = nullptr;
    return PoolKnown() && MeshBuffer(rm, &id, &res, base, &sz);
}

// A chunk of a render mesh: where its stream 0 starts in the mesh's vertex data (MeshVertexRange) and its vertices.
bool MeshChunk(uintptr_t rm, uint32_t chunk, uint32_t* off0, uint32_t* count, uint32_t* chunks) {
    uint64_t arr = 0;
    uint32_t n = 0;
    if (!SafeRead(reinterpret_cast<void*>(rm + 0xa8), &arr, 8) || !SafeRead(reinterpret_cast<void*>(rm + 0xb0), &n, 4) ||
        !arr || n > 256)
        return false;
    if (chunks) *chunks = n;
    if (chunk >= n) return false;
    return SafeRead(reinterpret_cast<void*>(arr + chunk * kChunkSize + 0x04), off0, 4) &&
           SafeRead(reinterpret_cast<void*>(arr + chunk * kChunkSize + 0x20), count, 4);
}

std::string BlendDump(uintptr_t rm, const std::wstring& path, bool now) {
    auto d = std::make_unique<Dump>();
    uint32_t id = 0, size = 0, n = 0;
    uint64_t base = 0, arr = 0;
    std::string why;
    if (!MeshBuffer(rm, &id, &d->res, &base, &size, &why)) {
        Log("blendshape: dump: %s", why.c_str());
        return "the mesh's vertex buffer was not found: " + why;
    }
    if (!ReadBox(rm, &d->box) ||
        !SafeRead(reinterpret_cast<void*>(rm + 0xa8), &arr, 8) || !SafeRead(reinterpret_cast<void*>(rm + 0xb0), &n, 4) ||
        !n || n > 256)
        return "not a loaded render mesh";
    std::lock_guard<std::mutex> l(g_mx);
    for (auto& e : g_sets)
        if (e.second->rm == rm && !now) return "the mesh has a set (unload it first: the dump is of the original)";
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t c[11] = {};
        if (!SafeRead(reinterpret_cast<void*>(arr + i * kChunkSize + 4), c, sizeof c)) return "chunk unreadable";
        // c[0] stream 0, c[2] stream 2, c[7] vertex count, c[10] LOD mask (offsets +0x04 .. +0x2c)
        if (c[0] + 16ull * c[7] > size || c[2] + 8ull * c[7] > size) return "chunk outside the buffer";
        d->parts.push_back({i, c[10], c[7], base + c[0], base + c[2], 0});
    }
    d->res->AddRef();
    d->path = path;
    InstallOverlay(true);
    g_dumps.push_back(std::move(d));
    return "queued (" + std::to_string(n) + " chunks)";
}

// Measuring (w3._bs_index_dump): the render mesh's bytes 0x80..0x100 and its chunk table, then the pool buffer of the
// id at +0xa4 (the index buffer, if it lies in the same pool) as raw bytes. Format in FinishDump.
// A buffer region of TW3SE's or the game's (held until the copy is done) raw into a file after `head` (TWIB).
void BlendQueueRaw(void* res, uint64_t off, uint64_t size, const std::vector<uint8_t>& head,
                   const std::wstring& path) {
    auto d = std::make_unique<Dump>();
    d->raw = true;
    d->head = head;
    d->res = static_cast<ID3D12Resource*>(res);
    d->res->AddRef();
    d->rawOff = off;
    d->rawSize = size;
    d->path = path;
    InstallOverlay(true);
    std::lock_guard<std::mutex> l(g_mx);
    g_dumps.push_back(std::move(d));
}

// The same with the bytes handed to `done` (on the render thread, in a submit) in place of a file.
void BlendQueueRawCb(void* res, uint64_t off, uint64_t size, std::function<void(const uint8_t*, uint64_t)> done) {
    auto d = std::make_unique<Dump>();
    d->raw = true;
    d->done = std::move(done);
    d->res = static_cast<ID3D12Resource*>(res);
    d->res->AddRef();
    d->rawOff = off;
    d->rawSize = size;
    InstallOverlay(true);
    std::lock_guard<std::mutex> l(g_mx);
    g_dumps.push_back(std::move(d));
}

// A render mesh's vertex and index buffers in the pool: resource, offset of the mesh's data in it, size.
bool MeshBuffers(uintptr_t rm, void** vbRes, uint64_t* vbOff, uint32_t* vbSize, void** ibRes, uint64_t* ibOff,
                 uint32_t* ibSize) {
    uint32_t id = 0;
    ID3D12Resource* r = nullptr;
    if (!PoolKnown() || !MeshBuffer(rm, &id, &r, vbOff, vbSize)) return false;
    *vbRes = r;
    Set tmp;
    FindIndices(rm, tmp);
    if (!tmp.ibRes) return false;
    uint32_t ib = 0, size = 0;
    uint64_t dd = 0, va = 0, rva = 0, width = 0;
    SafeRead(reinterpret_cast<void*>(rm + 0xa4), &ib, 4);
    SafeRead(reinterpret_cast<void*>(ImageBase() + g_poolPtr), &dd, 8);
    uintptr_t e = dd + (ib - 1ull) * kPoolStride + g_poolEntries;
    SafeRead(reinterpret_cast<void*>(e), &size, 4);
    SafeRead(reinterpret_cast<void*>(e + 0x10), &va, 8);
    ResourceRange(tmp.ibRes, &rva, &width);
    *ibRes = tmp.ibRes;
    tmp.ibRes->Release();  // (FindIndices held it; the pool holds it too)
    *ibOff = va - rva;
    *ibSize = size;
    return true;
}

std::string IndexDump(uintptr_t rm, const std::wstring& path, bool vertices) {
    auto d = std::make_unique<Dump>();
    d->raw = true;
    uint32_t ids[2] = {}, n = 0;
    uint64_t arr = 0, dd = 0, va = 0, r = 0, rva = 0, width = 0;
    uint32_t size = 0;
    if (vertices) ids[1] = 0;
    if (!PoolKnown() || !SafeRead(reinterpret_cast<void*>(rm + 0xa0), ids, 8) || (vertices && (ids[1] = ids[0], false)) ||
        !ids[1] ||
        !SafeRead(reinterpret_cast<void*>(rm + 0xa8), &arr, 8) || !SafeRead(reinterpret_cast<void*>(rm + 0xb0), &n, 4) ||
        !n || n > 256)
        return "not a loaded render mesh with an index buffer id";
    d->head.resize(0x80 + n * kChunkSize);
    if (!SafeRead(reinterpret_cast<void*>(rm + 0x80), d->head.data(), 0x80) ||
        !SafeRead(reinterpret_cast<void*>(arr), d->head.data() + 0x80, n * kChunkSize))
        return "unreadable";
    if (!SafeRead(reinterpret_cast<void*>(ImageBase() + g_poolPtr), &dd, 8) || !dd) return "no pool";
    uintptr_t e = dd + (ids[1] - 1ull) * kPoolStride + g_poolEntries;
    if (!SafeRead(reinterpret_cast<void*>(e), &size, 4) || !SafeRead(reinterpret_cast<void*>(e + 0x10), &va, 8) ||
        !SafeRead(reinterpret_cast<void*>(e + 0x18), &r, 8) || !r || !LooksLikeObject(reinterpret_cast<void*>(r)) ||
        !ResourceRange(reinterpret_cast<ID3D12Resource*>(r), &rva, &width) || va < rva || va + size > rva + width)
        return "the index buffer id " + std::to_string(ids[1]) + " has no buffer in the pool";
    d->res = reinterpret_cast<ID3D12Resource*>(r);
    d->res->AddRef();
    d->rawOff = va - rva;
    d->rawSize = size;
    d->path = path;
    d->rm = rm;
    InstallOverlay(true);
    std::lock_guard<std::mutex> l(g_mx);
    g_dumps.push_back(std::move(d));
    return "queued (index buffer " + std::to_string(ids[1]) + ", " + std::to_string(size) + " bytes, " + std::to_string(n) +
           " chunks)";
}

void SubmitWork(ID3D12CommandQueue* q);

// The game's own DIRECT queue: of the DIRECT queues that are not a frame generation's own, the one that submits the
// most lists (counted each second). With DLSS Frame Generation Streamline submits DIRECT lists on a queue of
// its own ("nv.sl.dlss_g.cmdQ.pacer") beside the game's, at its own pace; the blend shape copies ran there (the work
// went with whichever DIRECT queue submitted first), out of step with the game: the GPU wrote into a buffer the game
// had just given back and the device hung (DXGI_ERROR_DEVICE_HUNG, tw3se\out\dred.txt).
struct QueueCount {
    ID3D12CommandQueue* q;
    bool usable;  // DIRECT, and not named as a pacer's or a presenter's
    uint64_t lists;
};
// under g_queueMx of their own (the submit hook never takes g_mx: see DeformBeforeSubmit)
std::mutex g_queueMx;
std::vector<QueueCount> g_queueCounts;
std::atomic<ID3D12CommandQueue*> g_mainQueue{nullptr};
ULONGLONG g_mainPicked = 0;

bool UsableQueue(ID3D12CommandQueue* q) {
    if (q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    // the name SetName gives (WKPDID_D3DDebugObjectNameW of d3dcommon.h, here without dxguid.lib)
    static const GUID kNameW = {0x4cca5fd8, 0x921f, 0x42c8, {0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41}};
    wchar_t name[128] = {};
    UINT size = sizeof name - sizeof(wchar_t);
    if (FAILED(q->GetPrivateData(kNameW, &size, name))) return true;
    for (wchar_t* c = name; *c; ++c) *c = towlower(*c);
    bool own = wcsstr(name, L"pacer") || wcsstr(name, L"present") || wcsstr(name, L"interpol");
    Log("blendshape: DIRECT queue %p \"%ls\"%s", q, name, own ? " - a frame generation's, not used" : "");
    return !own;
}

bool IsMainQueue(ID3D12CommandQueue* q, unsigned lists) {
    QueueCount* c = nullptr;
    for (auto& e : g_queueCounts)
        if (e.q == q) c = &e;
    if (!c) {
        if (g_queueCounts.size() >= 16) g_queueCounts.clear();  // (queues made anew: a new device or swap chain)
        g_queueCounts.push_back({q, UsableQueue(q), 0});
        c = &g_queueCounts.back();
    }
    c->lists += lists;
    ULONGLONG now = GetTickCount64();
    if (now - g_mainPicked >= 1000) {
        const QueueCount* best = nullptr;
        for (const auto& e : g_queueCounts)
            if (e.usable && (!best || e.lists > best->lists)) best = &e;
        if (best && best->lists >= 10) {
            if (best->q != g_mainQueue)
                Log("blendshape: the game's queue is %p (%llu lists in a second)", best->q,
                    static_cast<unsigned long long>(best->lists));
            g_mainQueue = best->q;
        }
        for (auto& e : g_queueCounts) e.lists = 0;
        g_mainPicked = now;
    }
    return q == g_mainQueue;
}

// (a plugin's live mesh flickered - black spots, the world behind showing through, fast; gone with the game paused):
// the live meshes' copy went ahead of whichever of the game's submits came next, also in the middle of a
// frame - its depth pass drew the body of before, its colour pass the new one. A live mesh writes once a frame now,
// ahead of the game's first submit after a Present, so every pass of a frame sees the same vertices
std::atomic<bool> g_liveFrame{true};

// (with CWSO, Witcher Senses froze the game while a plugin moved a mesh's vertices; measured in the game, thread stacks):
// the work went in the ExecuteCommandLists hook - the game's submit came through CWSO's hook first, which holds its lock
// while it draws its outlines, then Streamline, then ours; our list's Reset went into CWSO's Reset hook and waited for
// that same lock. Inside another mod's submit hook no list of ours is touched any more: the submit hook only counts
// the queues, the work is recorded and submitted from the Present hook - after the frame's lists, before the next
// frame's (which is also when a live mesh has to write).
// (24.11., the same freeze once more, thread stacks): the Present hook held g_mx while its list's Reset went into
// CWSO's Reset hook and waited for CWSO's lock; CWSO, holding it in its submit hook, came on to ours, which waited for
// g_mx. The submit hook counts the queues under a lock of their own now and never waits for g_mx.
void DeformBeforeSubmit(void* queue, unsigned lists) {
    if (t_inSubmit) return;
    std::lock_guard<std::mutex> lock(g_queueMx);
    IsMainQueue(static_cast<ID3D12CommandQueue*>(queue), lists);
}

void DeformOnPresent() {
    g_liveFrame.store(true, std::memory_order_relaxed);
    if (t_inSubmit) return;
    std::lock_guard<std::mutex> lock(g_mx);
    ID3D12CommandQueue* queue = g_mainQueue.load();
    if (!queue) return;
    if (g_sets.empty() && g_hides.empty() && g_dumps.empty() && g_retire.empty() && g_retireRm.empty() && !TexWritesPending() &&
        !MushActive())
        return;
    uint64_t t0 = Micros();
    SubmitWork(queue);
    uint64_t dt = Micros() - t0;
    ++g_tCalls;
    g_tTotal += dt;
    if (dt > g_tMax) g_tMax = dt;
}

std::string BlendStats() {
    std::lock_guard<std::mutex> lock(g_mx);
    char b[200];
    snprintf(b, sizeof b, "%llu calls, %llu us in all, longest %llu us; mesh checks %llu, %llu us", g_tCalls, g_tTotal,
             g_tMax, g_tChecks, g_tCheck);
    g_tCalls = g_tTotal = g_tMax = g_tCheck = g_tChecks = 0;
    return b;
}

void SubmitWork(ID3D12CommandQueue* q) {
    if (q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
    g_queue = q;
    const bool liveNow = g_liveFrame.exchange(false, std::memory_order_relaxed);  // the frame's first submit
    // a live mesh's write (its copy into the game's buffer) waits for that; loading it and giving it back do not
    auto waits = [liveNow](const Set& s) { return s.live && s.ready && !s.unload && !liveNow; };
    if (!InitWriter(q)) {
        for (auto& e : g_sets)
            if (e.second->heldRm) RetireRenderMesh(e.second->rm);
        g_sets.clear();
        g_dumps.clear();
        return;
    }
    uint64_t done = g_fence->GetCompletedValue();
    for (size_t i = 0; i < g_retire.size();) {  // what the GPU no longer uses
        if (g_retire[i].first <= done) {
            g_retire[i].second->Release();
            g_retire[i] = g_retire.back();
            g_retire.pop_back();
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < g_retireRm.size();) {  // render meshes no list of ours uses any more
        if (g_retireRm[i].first <= done) {
            RunOnMainThread(&ReleaseRenderMesh, reinterpret_cast<void*>(g_retireRm[i].second));
            g_retireRm[i] = g_retireRm.back();
            g_retireRm.pop_back();
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < g_dumps.size();) {  // dumps whose copy has finished
        Dump& d = *g_dumps[i];
        if (d.fence && d.fence <= done) {
            FinishDump(d);
            Retire(d.readback);
            Retire(d.res);
            g_dumps.erase(g_dumps.begin() + i);
        } else {
            ++i;
        }
    }
    // weld maps whose rest positions are back (DeformNormals): the normals are made once more with them
    for (auto& e : g_sets) {
        Set& s = *e.second;
        for (Chunk& c : s.chunks)
            if (c.rbFence && c.rbFence != ~0ull && c.rbFence <= done && !c.welded) {
                Weld(c);
                if (s.live) s.boxNew = true;
                s.dirty = true;
            }
    }
    // what needs a list now; a lost mesh is dropped without touching it
    bool work = false;
    for (auto it = g_sets.begin(); it != g_sets.end();) {
        Set& s = *it->second;
        if (!s.rm) {  // a live mesh whose copy is not loaded yet (LiveClose ends it at once)
            ++it;
            continue;
        }
        uint64_t c0 = Micros();
        bool there = s.lost || StillThere(s);
        g_tCheck += Micros() - c0;
        ++g_tChecks;
        if (!there) {
            s.lost = true;
            Log("blendshape: set %d: the mesh is gone or was loaded anew - load the set again", s.id);
        }
        if (s.lost && s.unload) {
            Log("blendshape: set %d ended with its mesh (nothing written back)", s.id);
            FreeSet(s);
            it = g_sets.erase(it);
            continue;
        }
        if (!s.lost && (!s.ready || s.dirty || s.unload) && !waits(s)) work = true;
        ++it;
    }
    for (auto& d : g_dumps)
        if (!d->fence) work = true;
    for (auto it = g_hides.begin(); it != g_hides.end();) {  // w3.deform.hide
        Hide& h = *it->second;
        if (!h.lost && !HideStillThere(h)) {
            h.lost = true;
            Log("blendshape: hide %d: the mesh is gone or was loaded anew - hide again", h.id);
        }
        if (h.lost && h.unload) {
            FreeHide(h);
            it = g_hides.erase(it);
            continue;
        }
        if (!h.lost && g_hidePso && (!h.ready || h.dirty || h.unload)) work = true;
        ++it;
    }
    if (TexWritesPending()) work = true;  // texwrite.cpp: pixels for a texture
    if (MushActive()) work = true;        // mush.cpp: Delta Mush passes each frame
    if (!work) return;
    int slot = g_slot;
    if (done < g_slotFence[slot]) return;  // that slot's list still runs: the next submit
    ID3D12GraphicsCommandList* l = g_list[slot];
    g_alloc[slot]->Reset();
    l->Reset(g_alloc[slot], nullptr);
    uint64_t weightAt = 0;
    std::vector<std::pair<uintptr_t, Box>> boxes;
    for (auto& e : g_sets) {
        Set& s = *e.second;
        if (!s.rm || s.lost || !(!s.ready || s.dirty || s.unload) || waits(s)) continue;
        RecordSet(l, s, weightAt, slot);
        if (s.lost) continue;
        // still waiting for the game's draw: nothing written, the mesh keeps its box (the grown box on vertices still
        // in the old one drew the mesh out of shape, and stayed when the set went before it wrote)
        if (!s.ready) continue;
        s.dirty = false;
        boxes.push_back({s.rm, s.unload ? s.box0 : s.box1});
    }
    for (auto& d : g_dumps)
        if (!d->fence) RecordDump(l, *d);
    for (auto& e : g_hides) {
        Hide& h = *e.second;
        if (h.lost || !g_hidePso || !(!h.ready || h.dirty || h.unload)) continue;
        RecordHide(l, h);
        if (h.ready) h.dirty = false;
    }
    std::vector<IUnknown*> keep;  // texwrite.cpp: the finished writes, their upload buffers and textures
    TexRecord(l, g_dev, &keep);
    MushRecord(l, g_dev, &keep);
    for (IUnknown* u : keep) Retire(u, true);
    l->Close();
    ID3D12CommandList* lists[] = {l};
    t_inSubmit = true;
    q->ExecuteCommandLists(1, lists);
    t_inSubmit = false;
    q->Signal(g_fence, ++g_fenceValue);
    g_slotFence[slot] = g_fenceValue;
    g_slot = (slot + 1) % kSlots;
    for (auto& d : g_dumps)
        if (!d->fence) d->fence = g_fenceValue;
    for (auto& e : g_sets)
        for (Chunk& c : e.second->chunks)
            if (c.rbFence == ~0ull) c.rbFence = g_fenceValue;  // the rest positions for the weld map
    for (auto& b : boxes) SetBox(b.first, b.second);  // the renderer reads the box at each draw
    for (auto it = g_sets.begin(); it != g_sets.end();) {
        if (it->second->unload && it->second->ready) {
            FreeSet(*it->second);
            Log("blendshape: set %d unloaded (original back)", it->second->id);
            it = g_sets.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_hides.begin(); it != g_hides.end();) {
        if (it->second->unload && (it->second->ready || it->second->lost)) {
            FreeHide(*it->second);
            Log("blendshape: hide %d ended (indices back)", it->second->id);
            it = g_hides.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace tw3se
