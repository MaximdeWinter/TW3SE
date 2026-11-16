// TW3SE core - Delta Mush on a skinning hook slot.
//
// The patched skinning vertex shaders (tools/skin_patch.py, MUSH_REGISTER) write each vertex's skinned position (this
// frame's, before the capsules) to u15 space7 at b12's z + its vertex id, and add the offset at the same place in t15.
// The slot's mush area: 65536 places from slot * 65536 on; a chunk's vertices start at (its stream 0 - the mesh's) / 16.
// Each frame, ahead of the game's lists (blendshape.cpp's submit), two passes make the offsets from the frame before's
// positions q: smooth (s = sum of w * q over the vertex's smoothing neighbours) and offsets (the rest detail d, kept in
// a frame of two smoothed neighbours, put back on s: offset = mask * (s + frame * d - q)). One frame late.
//
// The rest data comes from the mesh itself: its vertex and index buffers are read back once (MeshBuffers), then a
// worker builds per vertex: the smoothing weights (`iterations` steps of a diffusion over the triangles, vertices at one
// place joined, the 12 largest kept), two frame neighbours, the rest detail in their frame, the mask (1 unless given).
#include "core.h"

#include <d3d12.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tw3se {

void BlendQueueRawCb(void* res, uint64_t off, uint64_t size, std::function<void(const uint8_t*, uint64_t)> done);

namespace {

constexpr uint32_t kSlotRoom = 65536, kSlotsMax = 9, kTotal = kSlotRoom * kSlotsMax;
constexpr uint32_t kRec = 128;  // bytes a vertex record: gi, 12 neighbours, 12 weights, a, b, d.xyz, mask, -
constexpr int kNbr = 12;

const char kMushCS[] = R"(
cbuffer C : register(b0) { uint count; uint p1; uint p2; uint p3; };
ByteAddressBuffer rest : register(t0);
ByteAddressBuffer q : register(t1);
RWByteAddressBuffer s : register(u0);
RWByteAddressBuffer o : register(u1);
[numthreads(64, 1, 1)] void smooth(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    uint r = id.x * 128, gi = rest.Load(r);
    float3 acc = 0;
    float ok = 1;
    for (uint k = 0; k < 12; ++k) {
        float w = asfloat(rest.Load(r + 52 + k * 4));
        if (w == 0) continue;
        float4 v = asfloat(q.Load4(rest.Load(r + 4 + k * 4) * 16));
        ok = min(ok, v.w);
        acc += w * v.xyz;
    }
    s.Store4(gi * 16, asuint(float4(acc, ok)));
}
[numthreads(64, 1, 1)] void offsets(uint3 id : SV_DispatchThreadID) {
    if (id.x >= count) return;
    uint r = id.x * 128, gi = rest.Load(r);
    float mask = asfloat(rest.Load(r + 120));
    float4 si = asfloat(s.Load4(gi * 16)), sa = asfloat(s.Load4(rest.Load(r + 100) * 16)),
           sb = asfloat(s.Load4(rest.Load(r + 104) * 16)), qi = asfloat(q.Load4(gi * 16));
    if (mask <= 0 || si.w < 0.5 || sa.w < 0.5 || sb.w < 0.5 || qi.w < 0.5) { o.Store4(gi * 16, uint4(0, 0, 0, 0)); return; }
    float3 t = sa.xyz - si.xyz, b = sb.xyz - si.xyz;
    if (dot(t, t) < 1e-14) { o.Store4(gi * 16, uint4(0, 0, 0, 0)); return; }
    t = normalize(t);
    float3 n = cross(t, b);
    if (dot(n, n) < 1e-14) { o.Store4(gi * 16, uint4(0, 0, 0, 0)); return; }
    n = normalize(n);
    float3 bt = cross(n, t), d = asfloat(rest.Load3(r + 108));
    float3 f = si.xyz + d.x * t + d.y * bt + d.z * n, off = (f - qi.xyz) * mask;
    float l = length(off);
    if (!(l < 1e9)) off = 0;                    // (not a number)
    else if (l > 0.05) off *= 0.05 / l;         // at most 5 cm
    o.Store4(gi * 16, asuint(float4(off, 0)));
}
)";

struct MushSlot {
    uintptr_t rm = 0;
    int iterations = 0;
    bool ready = false, building = false;
    uint32_t records = 0;
    ID3D12Resource* rest = nullptr;  // upload heap, kRec a record
    uint8_t* restMap = nullptr;
    std::map<uint32_t, std::vector<float>> masks;  // chunk -> a mask per vertex (until built; then written in place)
    std::vector<uint32_t> chunkFirst;               // per chunk: its first record
    std::vector<uint32_t> chunkCount;
    uint64_t serial = 0;  // a newer enable drops an older build
};

std::mutex g_mx;
MushSlot g_slots[kSlotsMax];
ID3D12Resource *g_q = nullptr, *g_o = nullptr, *g_s = nullptr;
ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_pso[2] = {};
bool g_failed = false;
uint64_t g_serial = 0;
uint32_t g_parity = 0;               // the half of q this frame's vertex shaders write
uint64_t g_frame = 0, g_lastRun = ~0ull;  // frames started; the frame the passes last ran in

ID3D12Resource* Buffer(ID3D12Device* dev, uint64_t size, D3D12_HEAP_TYPE heap, bool uav) {
    D3D12_HEAP_PROPERTIES hp = {heap};
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource* r = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                 heap == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON,
                                 nullptr, IID_PPV_ARGS(&r));
    return r;
}

bool InitPasses(ID3D12Device* dev) {
    if (g_pso[1]) return true;
    if (g_failed) return false;
    g_failed = true;
    using Compile_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT,
                                       ID3DBlob**, ID3DBlob**);
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? reinterpret_cast<Compile_t>(GetProcAddress(dc, "D3DCompile")) : nullptr;
    if (!compile) return false;
    D3D12_ROOT_PARAMETER p[5] = {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[0].Constants.Num32BitValues = 4;
    p[1].ParameterType = p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[2].Descriptor.ShaderRegister = 1;
    p[3].ParameterType = p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    p[4].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rd = {5, p};
    ID3DBlob *sig = nullptr, *err = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_root))))
        return false;
    const char* entry[2] = {"smooth", "offsets"};
    for (int i = 0; i < 2; ++i) {
        ID3DBlob* cs = nullptr;
        if (FAILED(compile(kMushCS, sizeof kMushCS - 1, "mush", nullptr, nullptr, entry[i], "cs_5_0", 0, 0, &cs, &err))) {
            Log("mush: shader: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_root;
        pd.CS = {cs->GetBufferPointer(), cs->GetBufferSize()};
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso[i])))) return false;
    }
    g_failed = false;
    return true;
}

struct Chunk {
    uint32_t off0, count, ioff, icount;
};

// The rest data of a mesh from its vertex and index bytes (worker thread). Returns the records, chunkFirst/Count.
std::vector<uint8_t> Build(int slot, const std::vector<uint8_t>& vb, const std::vector<uint8_t>& ib,
                           const std::vector<Chunk>& chunks, const float box[6], int iterations,
                           const std::map<uint32_t, std::vector<float>>& masks, std::vector<uint32_t>* first,
                           std::vector<uint32_t>* counts) {
    const uint32_t area = static_cast<uint32_t>(slot) * kSlotRoom;
    std::vector<uint8_t> out;
    for (size_t ci = 0; ci < chunks.size(); ++ci) {
        const Chunk& c = chunks[ci];
        first->push_back(static_cast<uint32_t>(out.size() / kRec));
        counts->push_back(c.count);
        uint32_t g0 = c.off0 / 16;
        if (g0 + c.count > kSlotRoom || c.off0 + 16ull * c.count > vb.size()) {
            out.resize(out.size() + static_cast<size_t>(kRec) * c.count, 0);  // (left out: mask 0)
            continue;
        }
        std::vector<float> p(3ull * c.count);
        std::unordered_map<uint64_t, uint32_t> place;
        std::vector<uint32_t> rep(c.count);
        for (uint32_t i = 0; i < c.count; ++i) {
            uint16_t u[3];
            memcpy(u, vb.data() + c.off0 + 16ull * i, 6);
            for (int k = 0; k < 3; ++k) p[3 * i + k] = u[k] / 65535.0f * box[k] + box[3 + k];
            uint64_t key = static_cast<uint64_t>(u[0]) | static_cast<uint64_t>(u[1]) << 16 | static_cast<uint64_t>(u[2]) << 32;
            rep[i] = place.emplace(key, i).first->second;
        }
        std::vector<std::vector<uint32_t>> nb(c.count);
        auto link = [&](uint32_t a, uint32_t b) {
            if (a == b) return;
            if (std::find(nb[a].begin(), nb[a].end(), b) == nb[a].end()) nb[a].push_back(b);
        };
        for (uint32_t t = 0; t + 2 < c.icount && c.ioff + 2ull * (t + 3) <= ib.size(); t += 3) {
            uint16_t v[3];
            memcpy(v, ib.data() + c.ioff + 2ull * t, 6);
            if (v[0] >= c.count || v[1] >= c.count || v[2] >= c.count) continue;
            uint32_t r[3] = {rep[v[0]], rep[v[1]], rep[v[2]]};
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) link(r[a], r[b]);
        }
        // smoothing rows: `iterations` steps of x <- 0.5 x + 0.5 * mean over the neighbours, from the vertex alone
        std::vector<std::vector<std::pair<uint32_t, float>>> row(c.count);
        std::vector<float> sm(3ull * c.count, 0);
        for (uint32_t i = 0; i < c.count; ++i) {
            if (rep[i] != i) continue;
            std::unordered_map<uint32_t, float> x{{i, 1.0f}};
            for (int it = 0; it < iterations; ++it) {
                std::unordered_map<uint32_t, float> y;
                for (auto& [j, w] : x) {
                    y[j] += 0.5f * w;
                    if (nb[j].empty()) {
                        y[j] += 0.5f * w;
                        continue;
                    }
                    float part = 0.5f * w / nb[j].size();
                    for (uint32_t k : nb[j]) y[k] += part;
                }
                x.swap(y);
            }
            std::vector<std::pair<uint32_t, float>> v(x.begin(), x.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
            if (v.size() > kNbr) v.resize(kNbr);
            float sum = 0;
            for (auto& e : v) sum += e.second;
            for (auto& e : v) e.second /= sum > 0 ? sum : 1;
            row[i] = v;
            for (auto& [j, w] : v)
                for (int k = 0; k < 3; ++k) sm[3 * i + k] += w * p[3 * j + k];
        }
        const std::vector<float>* mask = nullptr;
        auto mi = masks.find(static_cast<uint32_t>(ci));
        if (mi != masks.end() && mi->second.size() == c.count) mask = &mi->second;
        size_t at = out.size();
        out.resize(at + static_cast<size_t>(kRec) * c.count, 0);
        for (uint32_t i = 0; i < c.count; ++i) {
            uint32_t r = rep[i], rec[32] = {};
            rec[0] = area + g0 + i;
            float m = mask ? (*mask)[i] : 1.0f;
            const auto& rw = row[r];
            for (size_t k = 0; k < rw.size(); ++k) {
                rec[1 + k] = area + g0 + rw[k].first;
                memcpy(&rec[13 + k], &rw[k].second, 4);
            }
            // the frame: two neighbours of the vertex (smoothed), the rest detail in it
            if (nb[r].size() >= 2) {
                uint32_t a = nb[r][0], b = nb[r][1];
                const float* si = &sm[3ull * r];
                float t[3], bb[3], n[3], bt[3], d[3];
                for (int k = 0; k < 3; ++k) t[k] = sm[3ull * a + k] - si[k], bb[k] = sm[3ull * b + k] - si[k];
                float tl = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
                n[0] = t[1] * bb[2] - t[2] * bb[1], n[1] = t[2] * bb[0] - t[0] * bb[2], n[2] = t[0] * bb[1] - t[1] * bb[0];
                float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (tl > 1e-7f && nl > 1e-12f) {
                    for (int k = 0; k < 3; ++k) t[k] /= tl, n[k] /= nl;
                    bt[0] = n[1] * t[2] - n[2] * t[1], bt[1] = n[2] * t[0] - n[0] * t[2], bt[2] = n[0] * t[1] - n[1] * t[0];
                    for (int k = 0; k < 3; ++k) d[k] = p[3ull * i + k] - si[k];
                    float dd[3] = {d[0] * t[0] + d[1] * t[1] + d[2] * t[2], d[0] * bt[0] + d[1] * bt[1] + d[2] * bt[2],
                                   d[0] * n[0] + d[1] * n[1] + d[2] * n[2]};
                    rec[25] = area + g0 + a;
                    rec[26] = area + g0 + b;
                    memcpy(&rec[27], dd, 12);
                    memcpy(&rec[30], &m, 4);
                }
            }
            memcpy(out.data() + at + static_cast<size_t>(kRec) * i, rec, kRec);
        }
    }
    return out;
}

}  // namespace

bool MushActive() {
    std::lock_guard<std::mutex> l(g_mx);
    for (const MushSlot& s : g_slots)
        if (s.ready) return true;
    return false;
}

// Each frame's start (skinhook.cpp's table): the other half of q from now on.
void MushFrameStart() {
    std::lock_guard<std::mutex> l(g_mx);
    g_parity ^= 1;
    ++g_frame;
}

// b12's z for a draw of a slot's chunk (~0: no mush on that slot)
uint32_t MushAreaOf(int slot, uint64_t vb0, uint64_t lo) {
    if (slot < 1 || slot >= static_cast<int>(kSlotsMax) || vb0 < lo) return ~0u;
    std::lock_guard<std::mutex> l(g_mx);
    if (!g_slots[slot].ready) return ~0u;
    uint64_t g = (vb0 - lo) / 16;
    return g < kSlotRoom ? static_cast<uint32_t>(slot) * kSlotRoom + static_cast<uint32_t>(g) : ~0u;
}
// b12's w: where this frame's half of q starts (added to the q index only)
uint32_t MushQHalf() {
    std::lock_guard<std::mutex> l(g_mx);
    return g_parity * kTotal;
}
uint64_t MushQVa() { return g_q ? g_q->GetGPUVirtualAddress() : 0; }
uint64_t MushOVa() { return g_o ? g_o->GetGPUVirtualAddress() : 0; }

// iterations 0: off. Reads the mesh back, builds on a worker; ready a few frames later.
bool MushEnable(int slot, uintptr_t rm, int iterations, std::string* err) {
    if (slot < 1 || slot >= static_cast<int>(kSlotsMax)) return *err = "bad slot", false;
    std::unique_lock<std::mutex> l(g_mx);
    MushSlot& ms = g_slots[slot];
    ms.ready = false;
    ms.serial = ++g_serial;
    if (iterations <= 0) {
        ms.iterations = 0;
        return true;  // (the vertex shaders get z = ~0 from now on; the upload buffer stays for a later enable)
    }
    void *vbRes = nullptr, *ibRes = nullptr;
    uint64_t vbOff = 0, ibOff = 0;
    uint32_t vbSize = 0, ibSize = 0;
    if (!MeshBuffers(rm, &vbRes, &vbOff, &vbSize, &ibRes, &ibOff, &ibSize)) return *err = "the mesh's buffers were not found", false;
    float box[8];
    uint64_t arr = 0;
    uint32_t n = 0;
    if (!SafeRead(reinterpret_cast<void*>(rm + 0xe0), box, sizeof box) || !SafeRead(reinterpret_cast<void*>(rm + 0xa8), &arr, 8) ||
        !SafeRead(reinterpret_cast<void*>(rm + 0xb0), &n, 4) || !n || n > 256)
        return *err = "not a loaded render mesh", false;
    float b6[6] = {box[0], box[1], box[2], box[4], box[5], box[6]};
    std::vector<Chunk> chunks(n);
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t c[11] = {};
        if (!SafeRead(reinterpret_cast<void*>(arr + i * 0x30 + 4), c, sizeof c)) return *err = "chunk unreadable", false;
        chunks[i] = {c[0], c[7], c[6], c[8]};  // +0x04 stream 0, +0x20 vertices, +0x1c index offset, +0x24 indices
    }
    ms.rm = rm;
    ms.iterations = iterations;
    ms.building = true;
    const uint64_t serial = ms.serial;
    const auto masks = ms.masks;
    l.unlock();  // (the readbacks take blendshape.cpp's lock; its submit takes this one)

    // both buffers read back at once; the callbacks run in blendshape.cpp's submit under its lock and only copy; the
    // second one to arrive starts the build on a worker
    struct Got {
        std::mutex mx;
        std::vector<uint8_t> vb, ib;
        int n = 0;
    };
    auto got = std::make_shared<Got>();
    ID3D12Device* dev = nullptr;
    static_cast<ID3D12Resource*>(vbRes)->GetDevice(IID_PPV_ARGS(&dev));
    auto build = [=]() {
        std::thread([=]() {
            std::vector<uint32_t> first, counts;
            std::vector<uint8_t> rec = Build(slot, got->vb, got->ib, chunks, b6, iterations, masks, &first, &counts);
            ID3D12Resource* up = dev ? Buffer(dev, rec.size() ? rec.size() : kRec, D3D12_HEAP_TYPE_UPLOAD, false) : nullptr;
            uint8_t* map = nullptr;
            if (up && SUCCEEDED(up->Map(0, nullptr, reinterpret_cast<void**>(&map)))) memcpy(map, rec.data(), rec.size());
            std::lock_guard<std::mutex> l2(g_mx);
            if (!g_q && dev) {
                g_q = Buffer(dev, 32ull * kTotal, D3D12_HEAP_TYPE_DEFAULT, true);  // two halves
                g_o = Buffer(dev, 16ull * kTotal, D3D12_HEAP_TYPE_DEFAULT, true);
                g_s = Buffer(dev, 16ull * kTotal, D3D12_HEAP_TYPE_DEFAULT, true);
            }
            if (dev) dev->Release();
            MushSlot& m3 = g_slots[slot];
            if (m3.serial != serial || !up || !map || !g_q || !g_o || !g_s) {
                if (up) up->Release();
                Log("mush: slot %d: built, but dropped (a newer call, or buffers failed)", slot);
                return;
            }
            if (m3.rest) m3.rest->Release();  // (no list of ours still reads it: the slot was off meanwhile)
            m3.rest = up;
            m3.restMap = map;
            m3.records = static_cast<uint32_t>(rec.size() / kRec);
            m3.chunkFirst = first;
            m3.chunkCount = counts;
            m3.building = false;
            m3.ready = true;
            Log("mush: slot %d ready: %u vertices in %zu chunks, %d iterations", slot, m3.records, first.size(), iterations);
        }).detach();
    };
    BlendQueueRawCb(vbRes, vbOff, vbSize, [=](const uint8_t* m, uint64_t size) {
        std::lock_guard<std::mutex> g(got->mx);
        got->vb.assign(m, m + size);
        if (++got->n == 2) build();
    });
    BlendQueueRawCb(ibRes, ibOff, ibSize, [=](const uint8_t* m, uint64_t size) {
        std::lock_guard<std::mutex> g(got->mx);
        got->ib.assign(m, m + size);
        if (++got->n == 2) build();
    });
    return true;
}

void BlendQueueRaw(void* res, uint64_t off, uint64_t size, const std::vector<uint8_t>& head, const std::wstring& path);
// Measuring (w3._skin_mush_dump): the slot's q and o areas (float4 a place, TWIB files <path>_q.bin, <path>_o.bin) and
// its records (<path>_r.bin, raw).
std::string MushDump(int slot, const std::wstring& path) {
    std::lock_guard<std::mutex> l(g_mx);
    if (slot < 1 || slot >= static_cast<int>(kSlotsMax) || !g_slots[slot].ready || !g_q) return "the slot has no mush";
    const MushSlot& ms = g_slots[slot];
    std::vector<uint8_t> head(4);
    uint32_t area = static_cast<uint32_t>(slot) * kSlotRoom;
    memcpy(head.data(), &area, 4);
    BlendQueueRaw(g_q, 16ull * (area + kTotal * (g_parity ^ 1)), 16ull * kSlotRoom, head, path + L"_q.bin");
    BlendQueueRaw(g_o, 16ull * area, 16ull * kSlotRoom, head, path + L"_o.bin");
    FILE* f = _wfopen((path + L"_r.bin").c_str(), L"wb");
    if (f) {
        fwrite(ms.restMap, kRec, ms.records, f);
        fclose(f);
    }
    return "queued (" + std::to_string(ms.records) + " records)";
}

// A mask for one chunk (one float a vertex, 0..1); at once when the slot is built, else at its build.
bool MushMask(int slot, int chunk, const float* mask, int count, std::string* err) {
    if (slot < 1 || slot >= static_cast<int>(kSlotsMax) || chunk < 0 || count < 0 || (count && !mask))
        return *err = "bad slot, chunk or count", false;
    std::lock_guard<std::mutex> l(g_mx);
    MushSlot& ms = g_slots[slot];
    ms.masks[static_cast<uint32_t>(chunk)].assign(mask, mask + count);
    if (ms.ready && static_cast<size_t>(chunk) < ms.chunkFirst.size()) {
        if (static_cast<uint32_t>(count) != ms.chunkCount[chunk]) return *err = "the count is not the chunk's", false;
        for (int i = 0; i < count; ++i)
            memcpy(ms.restMap + static_cast<size_t>(kRec) * (ms.chunkFirst[chunk] + i) + 120, &mask[i], 4);
    }
    return true;
}

// The passes of every ready slot, in blendshape.cpp's list (render thread).
void MushRecord(void* list, void* device, std::vector<IUnknown*>* keep) {
    (void)keep;
    auto* l = static_cast<ID3D12GraphicsCommandList*>(list);
    auto* dev = static_cast<ID3D12Device*>(device);
    std::lock_guard<std::mutex> lk(g_mx);
    bool any = false;
    for (const MushSlot& s : g_slots) any |= s.ready;
    if (!any || !g_q || g_frame == g_lastRun || !InitPasses(dev)) return;
    g_lastRun = g_frame;  // once a frame, from the half the frame before wrote
    const uint64_t qva = g_q->GetGPUVirtualAddress() + 16ull * kTotal * (g_parity ^ 1);
    D3D12_RESOURCE_BARRIER uav = {};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = g_s;
    l->SetComputeRootSignature(g_root);
    for (const MushSlot& s : g_slots) {
        if (!s.ready || !s.records) continue;
        uint32_t k[4] = {s.records, 0, 0, 0};
        l->SetComputeRoot32BitConstants(0, 4, k, 0);
        l->SetComputeRootShaderResourceView(1, s.rest->GetGPUVirtualAddress());
        l->SetComputeRootShaderResourceView(2, qva);
        l->SetComputeRootUnorderedAccessView(3, g_s->GetGPUVirtualAddress());
        l->SetComputeRootUnorderedAccessView(4, g_o->GetGPUVirtualAddress());
        l->SetPipelineState(g_pso[0]);
        l->Dispatch((s.records + 63) / 64, 1, 1);
        l->ResourceBarrier(1, &uav);
        l->SetPipelineState(g_pso[1]);
        l->Dispatch((s.records + 63) / 64, 1, 1);
        l->ResourceBarrier(1, &uav);
    }
}

}  // namespace tw3se
