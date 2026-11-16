// Skinning shader hook (contact zones that push the skin): the game's skinning vertex shaders, patched offline by
// tools/skin_build.py, read two constant buffers of TW3SE's after the skinned position:
//   b12, space7: a root constant, the slot of the mesh drawn (0 = none: the shader leaves the vertex as it is)
//   b13, space7: a table: rows 0..8 the capsule counts of slot s (x: this frame, y: the frame before), then per slot,
//                capsule and time three rows (a.xyz + radius, b.xyz + strength, falloff + mode; model space of the
//                slot's mesh, metres). Out (mode >= 0): a skinned vertex's distance to the capsule's segment becomes
//                the smooth maximum of it and the radius over the falloff (falloff 0: out to the surface); in
//                (mode < 0): a vertex outside, within the falloff of the surface, is drawn to it. Times the strength;
//                the previous position (G-buffer shaders, for the motion vectors) gets the frame before's capsules.
//                skinhook.txt says the layout the shaders were made for (# layout capsules 32 rows 3).
//   b12's y:     where the drawn chunk's weights start in t14 (~0: none).
//   t14, space7: the weights, one float a vertex (a zone mask, stiffness: the push times it); element 0 is 1, what a
//                chunk without weights reads. The vertex is SV_VertexID: the game binds stream 0 at the chunk.
// Ray tracing: the game skins each figure once more in a compute shader for the ray tracing geometry (root signature
// 1950dac2999359a8; tools/skin_patch.py inject rt). It gets b13 and t14 too, bound right after the game sets that
// signature on a list; no slot: the shader finds its chunk in a list at the table's end (rows kRecRow0..: offset of
// the chunk's stream 0 in the vertex resource, vertex count, slot, where its weights start; row 0's z: how many).
// All three are appended to the game's main root signature at the start (config skin_hook = on; the shaders' ids in
// tw3se\shaders\skinhook.txt). render.cpp's command list hooks bind the table when a patched pipeline is set and,
// before each draw with one, set the slot of the vertex buffer bound at stream 0: a registered mesh's vertex data
// (its range in the game's buffer pool, blendshape.cpp) gives its slot, any other draw slot 0.
#include "core.h"

#include <d3d12.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tw3se {

bool ShadersEnsureHooks();
int ShadersAddRootParam(uint64_t rsId, const D3D12_ROOT_PARAMETER1* p);
int ShadersRootParamIndex(void* rs, int handle);
int ShadersOnPipeline(TW3SE_PipelineCallback cb, void* ctx);
bool MeshVertexRange(uintptr_t rm, uint64_t* va, uint64_t* size);
bool MeshChunk(uintptr_t rm, uint32_t chunk, uint32_t* off0, uint32_t* count, uint32_t* chunks);
bool MeshVertexBase(uintptr_t rm, uint64_t* base);
void RenderWantDrawHooks();

namespace {

constexpr uint64_t kMainRootSignature = 0xe6a20b1c84a3896bull;  // the game's main one (5.01, 5.01b)
constexpr uint64_t kRtRootSignature = 0x1950dac2999359a8ull;    // the ray tracing skinning's (5.01b)
constexpr int kSlots = 8;
constexpr int kCaps = 32;             // capsules per slot (tools/skin_patch.py CAPS)
constexpr int kCapRows = 3;           // rows per capsule and time (tools/skin_patch.py CAP_ROWS)
constexpr int kRing = 3;              // parts of the table buffer in flight
constexpr uint64_t kRow = 16;
constexpr uint64_t kCapRow0 = kSlots + 1;
constexpr uint64_t kRecRow0 = kCapRow0 + (kSlots + 1) * kCaps * kCapRows * 2;  // tools/skin_patch.py TABLE_ROWS
constexpr uint32_t kRecs = 64;                                       // RT_RECS
constexpr uint64_t kRows = kRecRow0 + kRecs;                         // RT_TABLE_ROWS
constexpr uint64_t kPart = (kRows * kRow + 255) / 256 * 256;       // a constant buffer view's alignment
constexpr uint32_t kWeightCapacity = 1u << 20;  // floats in t14 (tools/skin_patch.py WEIGHT_CAPACITY)

std::atomic<bool> g_on{false};
int g_hTable = 0, g_hSlot = 0, g_hWeights = 0, g_hMushQ = 0, g_hMushO = 0, g_hOverlay = 0;
constexpr uint32_t kOverlayMax = 512;  // texels a side, a slot
ID3D12Resource* g_ov = nullptr;       // upload heap: RGBA8, kOverlayMax^2 a slot (slot s at (s - 1) * that), the
uint32_t* g_ovMap = nullptr;          // colour planes of all slots, then their material planes
uint32_t g_ovSize[kSlots + 1] = {};   // a slot's overlay side (0: none)
bool g_ovMat[kSlots + 1] = {};        // a slot's material plane is set
int g_ovPlanes = 1;                   // the planes the shaders read (skinhook.txt: overlay 2)  // our root parameters' handles (shaders.cpp)
int g_hRtTable = 0, g_hRtWeights = 0;             // the same on the ray tracing skinning's signature
int g_hRtCapture = 0;                             // u15: the capture of the skinned positions (ray tracing)
constexpr uint32_t kCaptureVerts = 1u << 18;      // float4 a vertex
ID3D12Resource* g_cap = nullptr;                  // default heap, UAV
struct Capture {
    int slot = 0;          // armed for this slot (0: none)
    uint32_t base = 0, limit = 0;
    int frames = 0;        // tables written since armed
    std::wstring path;
    std::vector<uint8_t> head;
} g_capture;
std::unordered_set<uint64_t> g_vsIds;  // the patched vertex shaders (skinhook.txt)
std::shared_mutex g_mx;
std::unordered_set<void*> g_psos;      // pipelines with a patched vertex shader
std::unordered_map<void*, uint64_t> g_psoPs;  // their pixel shaders (0: none, a depth pass)
std::unordered_set<uint64_t> g_slotPsSeen;    // logged once: slot and pixel shader
std::unordered_set<uint64_t> g_missIds;          // skinning vertex shaders without a patch (SkinHookNoteVs)
std::unordered_map<void*, uint64_t> g_missPsos;  // pipelines with one of them -> its id
struct Weights {
    uint32_t chunk = 0, off0 = 0;  // the chunk, where its stream 0 starts in the mesh's vertex data
    uint32_t base = 0, room = 0;   // its place in t14 (room: floats there)
    std::vector<float> w;
    bool sent = false;             // in t14 (WriteTable): until then the chunk's draws read element 0
};
struct Slot {
    uintptr_t rm = 0;
    std::vector<Weights> weights;
    std::vector<uint32_t> rec;  // per chunk: offset of stream 0 in the vertex resource, vertex count (ray tracing)
    uint64_t lo = 0, hi = 0;
    bool lost = false;          // the mesh's vertex data moved (the game loaded the mesh anew): the slot acts no more
    float cap[kCaps][12] = {};  // this frame's: a.xyz, radius, b.xyz, strength, falloff, mode, 0, 0
    int count = 0;
    float last[kCaps][12] = {};  // what the frame before had (written with the table)
    int lastCount = 0;
};
Slot g_slots[kSlots + 1];  // [0] stays empty
ID3D12Resource* g_buf = nullptr;
uint8_t* g_map = nullptr;
std::atomic<uint32_t> g_part{0};
ID3D12Resource* g_wbuf = nullptr;
float* g_wmap = nullptr;
uint32_t g_wNext = 1;  // the next free float in t14 (g_mx)

ID3D12Resource* UploadBuffer(ID3D12Device* dev, uint64_t size, void** map) {
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* b = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&b))))
        return nullptr;
    if (FAILED(b->Map(0, nullptr, map))) {
        b->Release();
        return nullptr;
    }
    memset(*map, 0, size);
    return b;
}

void MakeBuffer(ID3D12PipelineState* pso) {
    ID3D12Device* dev = nullptr;
    pso->GetDevice(IID_PPV_ARGS(&dev));
    if (!dev) return;
    void* m = nullptr;
    void* w = nullptr;
    ID3D12Resource* b = UploadBuffer(dev, kPart * kRing, &m);
    ID3D12Resource* wb = b ? UploadBuffer(dev, 4ull * kWeightCapacity, &w) : nullptr;
    if (wb) {
        g_wmap = static_cast<float*>(w);
        g_wmap[0] = 1.0f;  // what a chunk without weights reads
        g_wbuf = wb;
        g_map = static_cast<uint8_t*>(m);
        g_buf = b;
        void* om = nullptr;
        g_ov = UploadBuffer(dev, 2 * 4ull * kOverlayMax * kOverlayMax * kSlots, &om);
        g_ovMap = static_cast<uint32_t*>(om);
        D3D12_HEAP_PROPERTIES dh = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC cd = {};
        cd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        cd.Width = 16ull * kCaptureVerts;
        cd.Height = cd.DepthOrArraySize = cd.MipLevels = 1;
        cd.SampleDesc.Count = 1;
        cd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        cd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        dev->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &cd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                     IID_PPV_ARGS(&g_cap));
    } else if (b) {
        b->Release();
    }
    dev->Release();
}

void OnPipeline(const TW3SE_PipelineInfo* info, void*) {
    if (!g_vsIds.count(info->compute ? info->cs : info->vs)) {
        std::unique_lock<std::shared_mutex> l(g_mx);
        if (!info->compute && g_missIds.count(info->vs)) g_missPsos[info->pipelineState] = info->vs;
        return;
    }
    std::unique_lock<std::shared_mutex> l(g_mx);
    if (!info->compute) g_psos.insert(info->pipelineState);  // (a compute one finds its chunk itself)
    if (!info->compute) g_psoPs[info->pipelineState] = info->ps;
    if (!g_buf) MakeBuffer(static_cast<ID3D12PipelineState*>(info->pipelineState));
}

// The table of this frame into the next part of the buffer (the parts the GPU may still read stay as they are).
void WriteTable() {
    if (!g_map) return;
    uint32_t part = (g_part.load(std::memory_order_relaxed) + 1) % kRing;
    uint8_t* dst = g_map + part * kPart;
    uint32_t n = 0;  // chunks in the ray tracing list
    {
        std::unique_lock<std::shared_mutex> l(g_mx);  // (the frame before's capsules are kept here)
        for (int s = 0; s <= kSlots; ++s) {
            Slot& sl = g_slots[s];
            if (sl.rm && !sl.lost) {  // still the vertex data it was given with? else no draw is this slot's any more
                uint64_t va = 0, size = 0;
                if (!MeshVertexRange(sl.rm, &va, &size) || va != sl.lo || va + size != sl.hi) {
                    sl.lost = true;
                    sl.lo = sl.hi = 0;
                    sl.count = 0;
                    Log("skinhook: slot %d: the mesh is gone or was loaded anew - add it again", s);
                }
            }
            uint32_t counts[4] = {static_cast<uint32_t>(sl.count), static_cast<uint32_t>(sl.lastCount),
                                  s ? (s - 1) * kOverlayMax * kOverlayMax : 0u, s && sl.rm && !sl.lost ? g_ovSize[s] | (g_ovMat[s] && g_ovPlanes >= 2 ? 1u << 16 : 0u) : 0u};
            memcpy(dst + s * kRow, counts, kRow);
            for (int c = 0; c < kCaps; ++c)
                for (int t = 0; t < 2; ++t) {
                    const float* v = t == 0 ? sl.cap[c] : sl.last[c];
                    uint64_t row = kCapRow0 + static_cast<uint64_t>(s) * kCaps * kCapRows * 2 + c * kCapRows * 2 +
                                   t * kCapRows;
                    memcpy(dst + row * kRow, v, kCapRows * kRow);
                }
            memcpy(sl.last, sl.cap, sizeof sl.cap);
            sl.lastCount = sl.count;
            for (size_t c = 0; s && sl.rm && !sl.lost && c * 2 + 1 < sl.rec.size() && n < kRecs; ++c) {
                uint32_t w = ~0u;
                for (const Weights& x : sl.weights)
                    if (x.chunk == c && x.sent && x.off0 != ~0u && !x.w.empty()) w = x.base;
                uint32_t r[4] = {sl.rec[c * 2], sl.rec[c * 2 + 1], static_cast<uint32_t>(s), w};
                memcpy(dst + (kRecRow0 + n++) * kRow, r, kRow);
            }
            // weights given since: into t14 (a draw of the frame before may still read the old ones there: a change
            // of weights can show half in one frame)
            for (Weights& w : sl.weights)
                if (!w.sent && g_wmap) {
                    memcpy(g_wmap + w.base, w.w.data(), 4 * w.w.size());
                    w.sent = true;
                }
        }
    }
    memset(dst + (kRecRow0 + n) * kRow, 0, (kRecs - n) * kRow);
    memcpy(dst + 8, &n, 4);  // row 0's z
    // row 0's x, y, w: the capture (slot, base, room); after 6 tables it ends and the buffer is read back
    bool readBack = false;
    Capture done;
    {
        std::unique_lock<std::shared_mutex> l(g_mx);
        uint32_t c[2] = {g_capture.slot ? static_cast<uint32_t>(g_capture.slot) : 0u, g_capture.base};
        memcpy(dst, c, 8);
        memcpy(dst + 12, &g_capture.limit, 4);
        if (g_capture.slot && ++g_capture.frames > 6) {
            done = g_capture;
            g_capture = Capture();
            readBack = true;
        }
    }
    g_part.store(part, std::memory_order_release);
    if (readBack && g_cap) {
        BlendQueueRaw(g_cap, 0, 16ull * done.limit, done.head, done.path);
        Log("skinhook: capture of slot %d read back (%u vertices)", done.slot, done.limit);
    }
}

}  // namespace

void InstallSkinHook() {
    if (!ConfigBool("skin_hook", false)) return;
    std::wstring path = Dir() + L"\\shaders\\skinhook.txt";
    FILE* f = _wfopen(path.c_str(), L"r");
    if (!f) {
        Log("skinhook: skin_hook = on, but tw3se\\shaders\\skinhook.txt is missing (tools/skin_build.py writes it)");
        return;
    }
    char line[128];
    int caps = 8, rows = 2, mush = 0, planes = 1;  // a list without the layout line: shaders of the first layout
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') {
            sscanf_s(line, "# layout capsules %d rows %d mush %d overlay %d", &caps, &rows, &mush, &planes);
            continue;
        }
        uint64_t id = 0;
        if (sscanf_s(line, "%llx", &id) == 1 && id) g_vsIds.insert(id);
    }
    fclose(f);
    g_ovPlanes = planes;
    if (caps != kCaps || rows != kCapRows) {
        Log("skinhook: the shaders in tw3se\\shaders were made for %d capsules of %d rows, this TW3SE has %d of %d: "
            "the skinning hook stays off (tools/skin_build.py makes them anew)", caps, rows, kCaps, kCapRows);
        g_vsIds.clear();
        return;
    }
    if (g_vsIds.empty()) {
        Log("skinhook: skinhook.txt lists no shader");
        return;
    }
    ShadersEnsureHooks();
    D3D12_ROOT_PARAMETER1 table = {};
    table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    table.Descriptor.ShaderRegister = 13;
    table.Descriptor.RegisterSpace = 7;
    table.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_PARAMETER1 slot = {};
    slot.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    slot.Constants.ShaderRegister = 12;
    slot.Constants.RegisterSpace = 7;
    slot.Constants.Num32BitValues = 4;  // the slot, where the chunk's weights start, its place in the mush area,
                                        // this frame's half of the mush positions
    slot.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_PARAMETER1 weights = {};
    weights.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    weights.Descriptor.ShaderRegister = 14;
    weights.Descriptor.RegisterSpace = 7;
    weights.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;  // (written at each frame's start)
    weights.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    g_hTable = ShadersAddRootParam(kMainRootSignature, &table);
    g_hSlot = ShadersAddRootParam(kMainRootSignature, &slot);
    g_hWeights = ShadersAddRootParam(kMainRootSignature, &weights);
    D3D12_ROOT_PARAMETER1 mq = {}, mo = {};  // Delta Mush (mush.cpp): skinned positions out (u15), offsets in (t15)
    mq.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    mo.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    mq.Descriptor.ShaderRegister = mo.Descriptor.ShaderRegister = 15;
    mq.Descriptor.RegisterSpace = mo.Descriptor.RegisterSpace = 7;
    mq.Descriptor.Flags = mo.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    mq.ShaderVisibility = mo.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    g_hMushQ = ShadersAddRootParam(kMainRootSignature, &mq);
    g_hMushO = ShadersAddRootParam(kMainRootSignature, &mo);
    D3D12_ROOT_PARAMETER1 ov = mo;  // the skin overlays (t16): the material pixel shaders read them
    ov.Descriptor.ShaderRegister = 16;
    g_hOverlay = ShadersAddRootParam(kMainRootSignature, &ov);
    g_hRtTable = ShadersAddRootParam(kRtRootSignature, &table);
    g_hRtWeights = ShadersAddRootParam(kRtRootSignature, &weights);
    D3D12_ROOT_PARAMETER1 capture = {};
    capture.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    capture.Descriptor.ShaderRegister = 15;
    capture.Descriptor.RegisterSpace = 7;
    capture.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    capture.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    g_hRtCapture = ShadersAddRootParam(kRtRootSignature, &capture);
    if (!g_hTable || !g_hSlot || !g_hWeights) {
        Log("skinhook: the root parameters were not taken");
        return;
    }
    ShadersOnPipeline(&OnPipeline, nullptr);
    RenderWantDrawHooks();
    g_on = true;
    Log("skinhook: on for %zu skinning shaders (b12/b13/t14, space7 on the main root signature)", g_vsIds.size());
}

bool SkinHookOn() { return g_on.load(std::memory_order_relaxed); }

// A vertex shader the game makes a pipeline with: one that skins (its input signature names BLENDINDICES) and has no
// patch in skinhook.txt is written to tw3se\shaders\skinhook_missing\<id>.vs.cso and logged once - a figure drawn
// with it is not pushed. tools/skin_build.py makes the missing ones from that folder.
std::atomic<int> g_missing{0};
void SkinHookNoteVs(uint64_t id, const void* bc, size_t n) {
    if (!SkinHookOn() || g_vsIds.count(id) || n < 12) return;
    static const char kBlend[] = "BLENDINDICES";
    const char* p = static_cast<const char*>(bc);
    bool skins = false;
    for (size_t i = 0; i + sizeof kBlend - 1 <= n && !skins; ++i)
        skins = p[i] == 'B' && memcmp(p + i, kBlend, sizeof kBlend - 1) == 0;
    if (!skins) return;
    static std::mutex mx;
    static std::unordered_set<uint64_t> seen;
    {
        std::lock_guard<std::mutex> l(mx);
        if (!seen.insert(id).second) return;
    }
    {
        std::unique_lock<std::shared_mutex> l(g_mx);
        g_missIds.insert(id);
    }
    ++g_missing;
    std::wstring dir = Dir() + L"\\shaders\\skinhook_missing";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[40];
    swprintf(name, 40, L"\\%016llx.vs.cso", static_cast<unsigned long long>(id));
    FILE* f = _wfopen((dir + name).c_str(), L"wb");
    if (f) {
        fwrite(bc, 1, n, f);
        fclose(f);
    }
    Log("skinhook: skinning shader %016llx has no patch (saved to tw3se\\shaders\\skinhook_missing; tools/skin_build.py "
        "makes it)", static_cast<unsigned long long>(id));
}

// A draw of a slot's mesh with a patched pipeline: its pixel shader, logged once a slot.
void SkinHookNoteDraw(void* pso, uint32_t slot) {
    if (!slot) return;
    std::unique_lock<std::shared_mutex> l(g_mx);
    auto it = g_psoPs.find(pso);
    uint64_t ps = it == g_psoPs.end() ? 0 : it->second;
    if (!g_slotPsSeen.insert(ps ^ (static_cast<uint64_t>(slot) << 60)).second) return;
    Log("skinhook: slot %u drawn with pixel shader %016llx", slot, static_cast<unsigned long long>(ps));
}

bool SkinHookPso(void* pso) {
    std::shared_lock<std::shared_mutex> l(g_mx);
    return g_psos.count(pso) != 0;
}

// A pipeline with a skinning shader that has no patch: that shader's id (0 for any other pipeline).
uint64_t SkinHookMissVs(void* pso) {
    std::shared_lock<std::shared_mutex> l(g_mx);
    auto it = g_missPsos.find(pso);
    return it == g_missPsos.end() ? 0 : it->second;
}

uint32_t SkinHookSlotOf(uint64_t va, uint32_t* weights, uint32_t* mush);

// A draw with such a pipeline: when it draws a slot's mesh, that pass shows the mesh without the push. Logged once a
// shader (render.cpp, before the draw).
void SkinHookMissedDraw(uint64_t vs, uint64_t vb0) {
    uint32_t w = 0;
    uint32_t mu = 0;
    uint32_t slot = SkinHookSlotOf(vb0, &w, &mu);
    if (!slot) return;
    static std::mutex mx;
    static std::unordered_set<uint64_t> told;
    {
        std::lock_guard<std::mutex> l(mx);
        if (!told.insert(vs).second) return;
    }
    Log("skinhook: slot %u's mesh is drawn with skinning shader %016llx, which has no patch: that pass shows it without "
        "the push", slot, static_cast<unsigned long long>(vs));
}

bool SkinHookIndices(void* rs, int* table, int* slot, int* weights, int* mushQ, int* mushO, int* overlay) {
    *overlay = g_hOverlay ? ShadersRootParamIndex(rs, g_hOverlay) : -1;
    *table = ShadersRootParamIndex(rs, g_hTable);
    *slot = ShadersRootParamIndex(rs, g_hSlot);
    *weights = ShadersRootParamIndex(rs, g_hWeights);
    *mushQ = g_hMushQ ? ShadersRootParamIndex(rs, g_hMushQ) : -1;
    *mushO = g_hMushO ? ShadersRootParamIndex(rs, g_hMushO) : -1;
    return *table >= 0 && *slot >= 0 && *weights >= 0;
}

// The slot of the vertex buffer bound at stream 0 (0: none) and where the drawn chunk's weights start (~0: none).
std::atomic<uint64_t> g_slotDraws{0};  // draws given a slot (w3.skinhook.stats)
std::atomic<uint64_t> g_askDraws{0}, g_lastVa{0};  // draws with a patched pipeline asked for, the last stream 0
uint32_t MushAreaOf(int slot, uint64_t vb0, uint64_t lo);
uint32_t SkinHookSlotOf(uint64_t va, uint32_t* weights, uint32_t* mush) {
    *weights = ~0u;
    *mush = ~0u;
    g_askDraws.fetch_add(1, std::memory_order_relaxed);
    g_lastVa.store(va, std::memory_order_relaxed);
    if (!va) return 0;
    std::shared_lock<std::shared_mutex> l(g_mx);
    for (int s = 1; s <= kSlots; ++s) {
        const Slot& sl = g_slots[s];
        if (!sl.hi || va < sl.lo || va >= sl.hi) continue;
        for (const Weights& w : sl.weights)
            if (w.sent && sl.lo + w.off0 == va) *weights = w.base;
        g_slotDraws.fetch_add(1, std::memory_order_relaxed);
        *mush = MushAreaOf(s, va, sl.lo);
        return static_cast<uint32_t>(s);
    }
    return 0;
}

uint64_t SkinHookTableVa() {
    return g_buf ? g_buf->GetGPUVirtualAddress() + g_part.load(std::memory_order_acquire) * kPart : 0;
}
uint64_t SkinHookWeightsVa() { return g_wbuf ? g_wbuf->GetGPUVirtualAddress() : 0; }

// The ray tracing skinning's signature: where our table and weights go (false for any other signature).
bool SkinHookComputeIndices(void* rs, int* table, int* weights, int* capture) {
    if (!g_hRtTable) return false;
    *table = ShadersRootParamIndex(rs, g_hRtTable);
    *weights = ShadersRootParamIndex(rs, g_hRtWeights);
    *capture = g_hRtCapture ? ShadersRootParamIndex(rs, g_hRtCapture) : -1;
    return *table >= 0 && *weights >= 0;
}
uint64_t SkinHookCaptureVa() { return g_cap ? g_cap->GetGPUVirtualAddress() : 0; }
uint64_t SkinHookOverlayVa() { return g_ov ? g_ov->GetGPUVirtualAddress() : 0; }

// A plane of a slot's skin overlay, `size` x `size` RGBA8 texels at the mesh's UV. Plane 0, the colour: red first,
// alpha = how much covers the skin, mixed into the albedo of the skin's material pixel shaders. Plane 1, the
// material: R wet (the game's own rain look), G matte, B height (its slope tilts the normal). nullptr: the plane
// clear. A new size clears the other plane; size 0: the overlay off. Any thread.
bool SkinHookOverlayPlane(int slot, int plane, const void* rgba8, int size, std::string* err) {
    if (slot < 1 || slot > kSlots || size < 0 || size > static_cast<int>(kOverlayMax))
        return *err = "bad slot, or a size outside 0 to 512", false;
    std::unique_lock<std::shared_mutex> l(g_mx);
    if (!g_ovMap) return *err = "the skinning hook is off", false;
    uint32_t* p[2] = {g_ovMap + (slot - 1) * kOverlayMax * kOverlayMax,
                      g_ovMap + (kSlots + slot - 1) * kOverlayMax * kOverlayMax};
    size_t n = 4ull * size * size;
    if (size && static_cast<uint32_t>(size) != g_ovSize[slot]) {
        memset(p[plane ^ 1], 0, n);
        if (plane == 0) g_ovMat[slot] = false;
    }
    if (size && rgba8) memcpy(p[plane], rgba8, n);
    else if (size) memset(p[plane], 0, n);
    if (plane == 1) g_ovMat[slot] = size && rgba8;
    if (!size) g_ovMat[slot] = false;
    g_ovSize[slot] = static_cast<uint32_t>(size);
    return true;
}
bool SkinHookOverlay(int slot, const void* rgba8, int size, std::string* err) {
    if (size && !rgba8) return *err = "no picture", false;
    return SkinHookOverlayPlane(slot, 0, rgba8, size, err);
}

// Delta Mush on a slot's mesh (mush.cpp): iterations 0 = off.
bool MushEnable(int slot, uintptr_t rm, int iterations, std::string* err);
bool MushMask(int slot, int chunk, const float* mask, int count, std::string* err);
bool SkinHookDeltaMush(int slot, int iterations, std::string* err) {
    uintptr_t rm = 0;
    {
        std::shared_lock<std::shared_mutex> l(g_mx);
        if (slot < 1 || slot > kSlots || !g_slots[slot].rm || g_slots[slot].lost) return *err = "no mesh in the slot", false;
        rm = g_slots[slot].rm;
    }
    if (!g_hMushQ) return *err = "the skinning hook is off", false;
    return MushEnable(slot, rm, iterations, err);
}
bool SkinHookMushMask(int slot, int chunk, const float* mask, int count, std::string* err) {
    return MushMask(slot, chunk, mask, count, err);
}

// The capture of a slot's skinned positions (ray tracing skinning, u15): armed for 6 frames, then read back into
// `path`: TWIB head (u32 base, u32 vertices, u32 chunks, per chunk u32 stream-0 offset in the resource and u32
// vertex count), then float4 a vertex at (chunk's offset - base) / 16 + its number (w = 1: written).
bool SkinHookCapture(int slot, const std::wstring& path, std::string* err) {
    if (slot < 1 || slot > kSlots || !g_cap || !g_hRtCapture) {
        *err = "no such slot, or no capture (the skinning hook is off)";
        return false;
    }
    std::unique_lock<std::shared_mutex> l(g_mx);
    Slot& sl = g_slots[slot];
    uint64_t base = 0, va = 0, size = 0;
    if (!sl.rm || sl.lost || !MeshVertexBase(sl.rm, &base) || !MeshVertexRange(sl.rm, &va, &size)) {
        *err = "the slot has no mesh";
        return false;
    }
    if (g_capture.slot) {
        *err = "a capture runs";
        return false;
    }
    g_capture.slot = slot;
    g_capture.base = static_cast<uint32_t>(base);
    g_capture.limit = static_cast<uint32_t>((std::min)(size / 16, static_cast<uint64_t>(kCaptureVerts)));
    g_capture.frames = 0;
    g_capture.path = path;
    uint32_t h[3] = {g_capture.base, g_capture.limit, static_cast<uint32_t>(sl.rec.size() / 2)};
    g_capture.head.assign(reinterpret_cast<uint8_t*>(h), reinterpret_cast<uint8_t*>(h) + sizeof h);
    g_capture.head.insert(g_capture.head.end(), reinterpret_cast<const uint8_t*>(sl.rec.data()),
                          reinterpret_cast<const uint8_t*>(sl.rec.data() + sl.rec.size()));
    return true;
}

// Each chunk's stream 0 in the vertex resource and its vertex count (the ray tracing list), under g_mx.
void ReadChunks(Slot& sl) {
    sl.rec.clear();
    uint64_t base = 0;
    uint32_t off0 = 0, count = 0, chunks = 0;
    if (!MeshVertexBase(sl.rm, &base) || !MeshChunk(sl.rm, 0, &off0, &count, &chunks)) return;
    for (uint32_t c = 0; c < chunks && MeshChunk(sl.rm, c, &off0, &count, nullptr); ++c) {
        sl.rec.push_back(static_cast<uint32_t>(base + off0));
        sl.rec.push_back(count);
    }
}

void MushFrameStart();
void SkinHookFrameStart() {
    if (SkinHookOn()) WriteTable();
    MushFrameStart();
}

// A render mesh gets a slot (1..8), 0 when there is none free or its vertex data is not found.
int SkinHookAdd(uintptr_t rm, std::string* err) {
    uint64_t va = 0, size = 0;
    if (!SkinHookOn()) {
        *err = "the skinning hook is off (skin_hook = on in tw3se.config, and tw3se\\shaders\\skinhook.txt)";
        return 0;
    }
    if (!MeshVertexRange(rm, &va, &size)) {
        *err = "the mesh's vertex data was not found";
        return 0;
    }
    std::unique_lock<std::shared_mutex> l(g_mx);
    for (int s = 1; s <= kSlots; ++s)
        if (g_slots[s].rm == rm && !g_slots[s].lost) return s;
    for (int s = 1; s <= kSlots; ++s)  // a lost slot of the same render mesh is taken again
        if (g_slots[s].rm == rm) {
            g_slots[s].lost = false;
            g_slots[s].lo = va;
            g_slots[s].hi = va + size;
            for (Weights& w : g_slots[s].weights) {  // (where the chunks are now; one gone or grown has none any more)
                uint32_t off0 = 0, count = 0;
                if (MeshChunk(rm, w.chunk, &off0, &count, nullptr) && count == w.w.size()) {
                    w.off0 = off0;
                } else {
                    w.off0 = ~0u;
                }
            }
            ReadChunks(g_slots[s]);
            return s;
        }
    for (int s = 1; s <= kSlots; ++s)
        if (!g_slots[s].rm) {
            g_slots[s] = Slot{};
            g_slots[s].rm = rm;
            g_slots[s].lo = va;
            g_slots[s].hi = va + size;
            ReadChunks(g_slots[s]);
            return s;
        }
    *err = "all 8 slots are taken";
    return 0;
}

bool SkinHookRemove(int slot) {
    if (slot < 1 || slot > kSlots) return false;
    std::unique_lock<std::shared_mutex> l(g_mx);
    bool had = g_slots[slot].rm != 0;
    g_slots[slot] = Slot{};
    bool weights = false;  // t14 starts over once no slot has weights (no draw reads them then)
    for (int s = 1; s <= kSlots; ++s) weights = weights || !g_slots[s].weights.empty();
    if (!weights) g_wNext = 1;
    return had;
}

// Weights for a chunk of the slot's mesh, one a vertex (the push times it: 0 no push, 1 all of it; a zone mask,
// stiffness): `count` has to be the chunk's vertex count; count 0 takes the chunk's weights away (it is pushed
// fully again). They act from the next frame on. False, with why in *err, for a slot without a mesh, a chunk not
// in it, a count not the chunk's, or no room left (1M floats for all slots).
bool SkinHookWeights(int slot, int chunk, const float* weights, int count, std::string* err) {
    if (slot < 1 || slot > kSlots || chunk < 0 || count < 0 || (count && !weights)) {
        *err = "bad slot, chunk or count";
        return false;
    }
    std::unique_lock<std::shared_mutex> l(g_mx);
    Slot& sl = g_slots[slot];
    if (!sl.rm || sl.lost) {
        *err = sl.rm ? "the slot is lost (add the mesh again)" : "the slot has no mesh";
        return false;
    }
    Weights* have = nullptr;
    for (Weights& w : sl.weights)
        if (w.chunk == static_cast<uint32_t>(chunk)) have = &w;
    if (!count) {
        if (have) {  // (its room stays for this chunk)
            have->w.clear();
            have->sent = true;
            have->off0 = ~0u;
        }
        return true;
    }
    uint32_t off0 = 0, n = 0, chunks = 0;
    if (!MeshChunk(sl.rm, static_cast<uint32_t>(chunk), &off0, &n, &chunks)) {
        *err = "chunk " + std::to_string(chunk) + " is not in the mesh (" + std::to_string(chunks) + " chunks)";
        return false;
    }
    if (n != static_cast<uint32_t>(count)) {
        *err = "chunk " + std::to_string(chunk) + " has " + std::to_string(n) + " vertices, not " + std::to_string(count);
        return false;
    }
    if (!have || have->room < n) {
        if (g_wNext + n > kWeightCapacity) {
            *err = "no room for more weights (1M floats for all slots)";
            return false;
        }
        if (!have) {
            sl.weights.emplace_back();
            have = &sl.weights.back();
            have->chunk = static_cast<uint32_t>(chunk);
        }
        have->base = g_wNext;
        have->room = n;
        g_wNext += n;
    }
    have->off0 = off0;
    have->w.assign(weights, weights + count);
    for (float& f : have->w) f = f != f ? 1.0f : f;  // (a NaN would spread)
    have->sent = false;
    return true;
}

// The capsules of a slot from now on: `n` of them, 8 floats each (a.xyz, radius, b.xyz, strength), in the model space
// of the slot's mesh; more than 8 are left out. False for a slot without a mesh.
bool SkinHookCapsules(int slot, const float* caps, int n) {
    if (slot < 1 || slot > kSlots || n < 0 || (n && !caps)) return false;
    std::unique_lock<std::shared_mutex> l(g_mx);
    Slot& sl = g_slots[slot];
    if (!sl.rm) return false;
    sl.count = n < kCaps ? n : kCaps;
    memset(sl.cap, 0, sizeof sl.cap);
    for (int c = 0; c < sl.count; ++c) memcpy(sl.cap[c], caps + 8 * c, sizeof(float) * 8);  // falloff 0, out
    return true;
}

// The same with 12 floats a capsule: a.xyz, radius, b.xyz, strength, falloff (metres), mode (>= 0 out, < 0 in), 0, 0.
bool SkinHookCapsulesEx(int slot, const float* caps, int n) {
    if (slot < 1 || slot > kSlots || n < 0 || (n && !caps)) return false;
    std::unique_lock<std::shared_mutex> l(g_mx);
    Slot& sl = g_slots[slot];
    if (!sl.rm) return false;
    sl.count = n < kCaps ? n : kCaps;
    memset(sl.cap, 0, sizeof sl.cap);
    for (int c = 0; c < sl.count; ++c) {
        memcpy(sl.cap[c], caps + 12 * c, sizeof(float) * 10);
        if (sl.cap[c][8] < 0) sl.cap[c][8] = 0;
    }
    return true;
}

// The vertex count of each chunk of the slot's mesh (what SkinHookWeights takes); empty for a slot without one.
std::vector<uint32_t> SkinHookChunks(int slot) {
    std::vector<uint32_t> r;
    if (slot < 1 || slot > kSlots) return r;
    std::shared_lock<std::shared_mutex> l(g_mx);
    uintptr_t rm = g_slots[slot].lost ? 0 : g_slots[slot].rm;
    uint32_t off0 = 0, count = 0, chunks = 0;
    if (!rm || !MeshChunk(rm, 0, &off0, &count, &chunks)) return r;
    for (uint32_t c = 0; c < chunks && MeshChunk(rm, c, &off0, &count, nullptr); ++c) r.push_back(count);
    return r;
}

// 0 no mesh, 2 active, 3 lost (TW3SE_DEFORM_* values)
int SkinHookState(int slot) {
    if (slot < 1 || slot > kSlots) return 0;
    std::shared_lock<std::shared_mutex> l(g_mx);
    if (!g_slots[slot].rm) return 0;
    return g_slots[slot].lost ? 3 : 2;
}

std::string RenderSkinDiag();
std::string SkinHookStats() {
    std::shared_lock<std::shared_mutex> l(g_mx);
    int used = 0;
    for (int s = 1; s <= kSlots; ++s) used += g_slots[s].rm ? 1 : 0;
    char b[400];
    snprintf(b, sizeof b, "skinhook: %s, %zu shaders, %zu pipelines, %d slot(s) taken, %u weights, %d skinning "
             "shader(s) without a patch, %llu draws given a slot of %llu asked, last stream 0 %llx, slot 1 %llx-%llx",
             SkinHookOn() ? "on" : "off", g_vsIds.size(), g_psos.size(), used, g_wNext - 1, g_missing.load(),
             static_cast<unsigned long long>(g_slotDraws.load()), static_cast<unsigned long long>(g_askDraws.load()),
             static_cast<unsigned long long>(g_lastVa.load()), static_cast<unsigned long long>(g_slots[1].lo),
             static_cast<unsigned long long>(g_slots[1].hi));
    return b + RenderSkinDiag();
}

}  // namespace tw3se
