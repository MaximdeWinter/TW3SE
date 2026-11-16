// Vertex deformation, step 1: measuring. Where a mesh's vertices are on the GPU when the game draws it.
//
// The plan: a compute pass of TW3SE's writes base + sum(weight * delta) into the mesh's vertex buffer before the game
// draws, so every one of the game's skinning shaders skins the moved vertices. For that it needs, per mesh: the buffer
// resource, the range of the mesh's vertices in it, the vertex layout, the heap type.
//
// This step only watches: buffers made by the device (GPU address range -> resource, heap type), the vertex and index
// buffers bound on each command list, and the draw whose index count matches the probe (w3._deform_probe(n)); that
// draw's bindings go to the log. The draw functions are hooked in their code, not in the vtable: a layer between the
// game and D3D12 (Streamline) may call the functions it read from the vtable when the list was made (a vtable
// patch sees none of the game's draws). Each distinct function gets its own hook and trampoline (up to four per slot).
#include "core.h"

#include <d3d12.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tw3se {
void RenderSkinNoteVB(void* cl, UINT start, UINT n, const void* views);  // render.cpp: the skinning hook
void RenderSkinBeforeDraw(void* cl);
namespace {

using CreateCommitted_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                      const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                      const D3D12_CLEAR_VALUE*, REFIID, void**);
using CreatePlaced_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
                                                   D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using SetVB_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
using SetIB_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
using DrawIdx_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using Draw_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using ExecIndirect_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT,
                                                ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);

CreateCommitted_t oCommitted = nullptr;
CreatePlaced_t oPlaced = nullptr;
std::atomic<uint64_t> g_nDrawIdx{0}, g_nDraw{0}, g_nIndirect{0}, g_nSetVB{0};

struct Buf {
    uint64_t size;
    int heap;   // D3D12_HEAP_TYPE
    int state;  // the initial state
    ID3D12Resource* res;
    std::string made;  // the game's call chain that made it (offsets in witcher3.exe)
};
std::mutex g_mx;
std::map<uint64_t, Buf> g_bufs;  // GPU address -> buffer (address ranges)
struct Bound {
    D3D12_VERTEX_BUFFER_VIEW vb[8];
    D3D12_INDEX_BUFFER_VIEW ib;
    std::string vbFrom;  // probe 2: the game's call chain that bound stream 0
};

// The return addresses inside witcher3.exe on the current stack, as " rva rva ...", skipping `skip` frames.
std::string ExeChain(int skip, uintptr_t* first = nullptr) {
    void* frames[10] = {};
    USHORT n = CaptureStackBackTrace(skip, 10, frames, nullptr);
    uintptr_t base = ImageBase();
    std::string chain;
    for (USHORT i = 0; i < n; ++i) {
        uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
        if (a < base || a > base + 0x8000000) continue;
        if (first && !*first) *first = a - base;
        char b[24];
        snprintf(b, sizeof b, " %llx", static_cast<unsigned long long>(a - base));
        chain += b;
    }
    return chain;
}
std::unordered_map<void*, Bound> g_bound;
std::unordered_map<uint32_t, uint32_t> g_counts;  // index count -> draws (probe 1)
std::atomic<uint32_t> g_probe{0};
std::atomic<int> g_reports{0};

// Who makes the default-heap buffers (the game's call sites, as offsets in witcher3.exe), logged once per site.
std::unordered_map<uintptr_t, int> g_sites;
void NoteCaller(uint64_t size, const std::string& chain, uintptr_t first) {
    if (!first || g_sites[first]++ > 0 || g_sites.size() > 40) return;
    Log("deform: default-heap buffer of %llu bytes made from witcher3.exe%s", static_cast<unsigned long long>(size),
        chain.c_str());
}

void NoteBuffer(ID3D12Resource* r, const D3D12_RESOURCE_DESC* d, int heap, int state) {
    if (!r || !d || d->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    uint64_t va = r->GetGPUVirtualAddress();
    uintptr_t first = 0;
    std::string chain = heap == D3D12_HEAP_TYPE_DEFAULT ? ExeChain(3, &first) : std::string();
    std::lock_guard<std::mutex> l(g_mx);
    g_bufs[va] = {d->Width, heap, state, r, chain};
    if (heap == D3D12_HEAP_TYPE_DEFAULT) NoteCaller(d->Width, chain, first);
}

HRESULT STDMETHODCALLTYPE CommittedHook(ID3D12Device* dev, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                        const D3D12_RESOURCE_DESC* d, D3D12_RESOURCE_STATES st,
                                        const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    HRESULT hr = oCommitted(dev, hp, hf, d, st, cv, riid, out);
    if (SUCCEEDED(hr) && out && *out) NoteBuffer(static_cast<ID3D12Resource*>(*out), d, hp ? hp->Type : -1, st);
    return hr;
}

HRESULT STDMETHODCALLTYPE PlacedHook(ID3D12Device* dev, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC* d,
                                     D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** out) {
    HRESULT hr = oPlaced(dev, heap, off, d, st, cv, riid, out);
    if (SUCCEEDED(hr) && out && *out)
        NoteBuffer(static_cast<ID3D12Resource*>(*out), d, heap ? heap->GetDesc().Properties.Type : -1, st);
    return hr;
}

std::string Where(uint64_t va) {  // the buffer holding a GPU address: its size, heap type, offset (lock held)
    auto it = g_bufs.upper_bound(va);
    if (it == g_bufs.begin()) return "no buffer";
    --it;
    if (va >= it->first + it->second.size) return "no buffer";
    char b[160];
    snprintf(b, sizeof b, "resource %p size %llu heap %d state 0x%x, at +%llu", static_cast<void*>(it->second.res),
             static_cast<unsigned long long>(it->second.size), it->second.heap, it->second.state,
             static_cast<unsigned long long>(va - it->first));
    // (the resource is only printed, never called: the game may have released it since; a debug name query
    // on such a pointer crashes, and the engine names no buffers anyway)
    std::string out = b;
    if (!it->second.made.empty()) out += ", made from" + it->second.made;
    return out;
}

constexpr int kVariants = 4;
Draw_t g_trDraw[kVariants];
DrawIdx_t g_trDrawIdx[kVariants];
SetIB_t g_trSetIB[kVariants];
SetVB_t g_trSetVB[kVariants];
ExecIndirect_t g_trIndirect[kVariants];

template <int K>
void STDMETHODCALLTYPE DrawHook(ID3D12GraphicsCommandList* cl, UINT v, UINT i, UINT sv, UINT si) {
    g_nDraw.fetch_add(1, std::memory_order_relaxed);
    RenderSkinBeforeDraw(cl);
    g_trDraw[K](cl, v, i, sv, si);
}

template <int K>
void STDMETHODCALLTYPE IndirectHook(ID3D12GraphicsCommandList* cl, ID3D12CommandSignature* sig, UINT max,
                                    ID3D12Resource* args, UINT64 argOff, ID3D12Resource* cnt, UINT64 cntOff) {
    g_nIndirect.fetch_add(1, std::memory_order_relaxed);
    g_trIndirect[K](cl, sig, max, args, argOff, cnt, cntOff);
}

// Vertex ranges a new blend set waits for (blendshape.cpp): a draw binding one marks it seen. Lock-free, a few slots.
struct VbWatch {
    std::atomic<uint64_t> lo{0}, hi{0};
    std::atomic<bool> seen{false};
};
constexpr int kVbWatch = 32;
VbWatch g_vbWatch[kVbWatch];
std::atomic<int> g_vbWatching{0};

void NoteWatchedVB(UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) {
    for (UINT i = 0; i < n; ++i) {
        uint64_t a = v[i].BufferLocation;
        if (!a) continue;
        for (auto& w : g_vbWatch) {
            uint64_t lo = w.lo.load(std::memory_order_relaxed);
            if (lo && a >= lo && a < w.hi.load(std::memory_order_relaxed)) w.seen.store(true, std::memory_order_relaxed);
        }
    }
}

template <int K>
void STDMETHODCALLTYPE SetVBHook(ID3D12GraphicsCommandList* cl, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) {
    g_nSetVB.fetch_add(1, std::memory_order_relaxed);
    if (v && g_vbWatching.load(std::memory_order_relaxed)) NoteWatchedVB(n, v);
    RenderSkinNoteVB(cl, start, n, v);
    uint32_t probe = g_probe.load(std::memory_order_relaxed);
    if (probe && v && start + n <= 8) {
        // probe 2, until its 12 reports are out: who binds a skinned mesh (stream 0, stride 16)
        std::string from;
        if (probe == 2 && start == 0 && n && v[0].StrideInBytes == 16 && g_reports.load() < 12) from = ExeChain(2);
        std::lock_guard<std::mutex> l(g_mx);
        Bound& b = g_bound[cl];
        for (UINT i = 0; i < n; ++i) b.vb[start + i] = v[i];
        if (start == 0 && n) b.vbFrom = from;
    }
    g_trSetVB[K](cl, start, n, v);
}

template <int K>
void STDMETHODCALLTYPE SetIBHook(ID3D12GraphicsCommandList* cl, const D3D12_INDEX_BUFFER_VIEW* v) {
    if (g_probe.load(std::memory_order_relaxed) && v) {
        std::lock_guard<std::mutex> l(g_mx);
        g_bound[cl].ib = *v;
    }
    g_trSetIB[K](cl, v);
}

void ReportDraw(ID3D12GraphicsCommandList* cl, UINT count, UINT inst, UINT startIdx, INT baseVtx) {
    std::string r;
    {
        std::lock_guard<std::mutex> l(g_mx);
        const Bound& b = g_bound[cl];
        char h[480];
        snprintf(h, sizeof h, "deform probe: draw %u indices x%u, start index %u, base vertex %d, index buffer %llx "
                 "size %u fmt %d (%s)\n", count, inst, startIdx, baseVtx,
                 static_cast<unsigned long long>(b.ib.BufferLocation), b.ib.SizeInBytes, b.ib.Format,
                 Where(b.ib.BufferLocation).c_str());
        r = h;
        for (int i = 0; i < 8; ++i) {
            if (!b.vb[i].BufferLocation) continue;
            char v[480];
            snprintf(v, sizeof v, "  stream %d: %llx size %u stride %u (%s)\n", i,
                     static_cast<unsigned long long>(b.vb[i].BufferLocation), b.vb[i].SizeInBytes,
                     b.vb[i].StrideInBytes, Where(b.vb[i].BufferLocation).c_str());
            r += v;
        }
        if (!b.vbFrom.empty()) r += "  stream 0 bound from witcher3.exe" + b.vbFrom + "\n";
    }
    Log("%s", r.c_str());
}

template <int K>
void STDMETHODCALLTYPE DrawIdxHook(ID3D12GraphicsCommandList* cl, UINT count, UINT inst, UINT startIdx, INT baseVtx,
                                   UINT startInst) {
    g_nDrawIdx.fetch_add(1, std::memory_order_relaxed);
    uint32_t probe = g_probe.load(std::memory_order_relaxed);
    if (probe == 1) {
        std::lock_guard<std::mutex> l(g_mx);
        ++g_counts[count];
    }
    if (probe == 2) {  // the first 12 distinct skinned meshes drawn (stream 0 with stride 16), one line each
        uint64_t vb0 = 0;
        UINT stride = 0;
        {
            std::lock_guard<std::mutex> l(g_mx);
            const Bound& b = g_bound[cl];
            vb0 = b.vb[0].BufferLocation;
            stride = b.vb[0].StrideInBytes;
        }
        static std::unordered_map<uint64_t, int> seen;
        static std::mutex seenMx;
        bool report = false;
        if (stride == 16 && vb0) {
            std::lock_guard<std::mutex> l(seenMx);
            report = seen.size() < 12 && seen[vb0 ^ (static_cast<uint64_t>(count) << 48)]++ == 0;
        }
        if (report) {
            g_reports.fetch_add(1);
            ReportDraw(cl, count, inst, startIdx, baseVtx);
        }
    } else if (probe > 2 && count == probe && g_reports.fetch_add(1) < 6) {
        ReportDraw(cl, count, inst, startIdx, baseVtx);
    }
    RenderSkinBeforeDraw(cl);
    g_trDrawIdx[K](cl, count, inst, startIdx, baseVtx, startInst);
}

// One slot of a vtable: its function gets the next free variant of the hook, unless it has one already.
template <class Tr>
void HookSlot(void* fn, std::unordered_map<void*, int>& done, int& used, void* const (&hooks)[kVariants], Tr (&tr)[kVariants],
              const char* what) {
    if (!fn || done.count(fn)) return;
    if (used >= kVariants) {
        Log("deform: a fifth %s function - not hooked", what);
        return;
    }
    int k = used++;
    if (Hook(fn, hooks[k], reinterpret_cast<void**>(&tr[k]))) done[fn] = k;
    else Log("deform: %s function %p not hooked", what, fn);
}

void* const kDraw[kVariants] = {reinterpret_cast<void*>(&DrawHook<0>), reinterpret_cast<void*>(&DrawHook<1>),
                                reinterpret_cast<void*>(&DrawHook<2>), reinterpret_cast<void*>(&DrawHook<3>)};
void* const kDrawIdx[kVariants] = {reinterpret_cast<void*>(&DrawIdxHook<0>), reinterpret_cast<void*>(&DrawIdxHook<1>),
                                   reinterpret_cast<void*>(&DrawIdxHook<2>), reinterpret_cast<void*>(&DrawIdxHook<3>)};
void* const kSetIB[kVariants] = {reinterpret_cast<void*>(&SetIBHook<0>), reinterpret_cast<void*>(&SetIBHook<1>),
                                 reinterpret_cast<void*>(&SetIBHook<2>), reinterpret_cast<void*>(&SetIBHook<3>)};
void* const kSetVB[kVariants] = {reinterpret_cast<void*>(&SetVBHook<0>), reinterpret_cast<void*>(&SetVBHook<1>),
                                 reinterpret_cast<void*>(&SetVBHook<2>), reinterpret_cast<void*>(&SetVBHook<3>)};
void* const kIndirect[kVariants] = {reinterpret_cast<void*>(&IndirectHook<0>), reinterpret_cast<void*>(&IndirectHook<1>),
                                    reinterpret_cast<void*>(&IndirectHook<2>), reinterpret_cast<void*>(&IndirectHook<3>)};

std::mutex g_vtMx;
std::unordered_map<void*, int> g_doneVt, g_doneDraw, g_doneDrawIdx, g_doneSetIB, g_doneSetVB, g_doneIndirect;
int g_usedDraw = 0, g_usedDrawIdx = 0, g_usedSetIB = 0, g_usedSetVB = 0, g_usedIndirect = 0;

void HookListVtable(void* list) {
    void** vt = *static_cast<void***>(list);
    std::lock_guard<std::mutex> l(g_vtMx);
    if (g_doneVt.count(vt)) return;
    g_doneVt[vt] = 1;
    // 12 DrawInstanced, 13 DrawIndexedInstanced, 43 IASetIndexBuffer, 44 IASetVertexBuffers, 59 ExecuteIndirect
    HookSlot(vt[12], g_doneDraw, g_usedDraw, kDraw, g_trDraw, "DrawInstanced");
    HookSlot(vt[13], g_doneDrawIdx, g_usedDrawIdx, kDrawIdx, g_trDrawIdx, "DrawIndexedInstanced");
    HookSlot(vt[43], g_doneSetIB, g_usedSetIB, kSetIB, g_trSetIB, "IASetIndexBuffer");
    HookSlot(vt[44], g_doneSetVB, g_usedSetVB, kSetVB, g_trSetVB, "IASetVertexBuffers");
    HookSlot(vt[59], g_doneIndirect, g_usedIndirect, kIndirect, g_trIndirect, "ExecuteIndirect");
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(vt[13]), &m))
        GetModuleFileNameA(m, name, MAX_PATH);
    const char* base = strrchr(name, '\\');
    Log("deform: command list vtable %p (DrawIndexedInstanced in %s+0x%llx); functions hooked: %d indexed draw variant(s)",
        static_cast<void*>(vt), base ? base + 1 : name,
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(vt[13]) - reinterpret_cast<uintptr_t>(m)), g_usedDrawIdx);
}

using CreateList_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
                                                 ID3D12PipelineState*, REFIID, void**);
CreateList_t oCreateList = nullptr;
HRESULT STDMETHODCALLTYPE CreateListHook(ID3D12Device* dev, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                         ID3D12CommandAllocator* alloc, ID3D12PipelineState* pso, REFIID riid, void** out) {
    HRESULT hr = oCreateList(dev, mask, type, alloc, pso, riid, out);
    if (SUCCEEDED(hr) && out && *out && type != D3D12_COMMAND_LIST_TYPE_COPY) HookListVtable(*out);
    return hr;
}

using CreateList1_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS,
                                                  REFIID, void**);
CreateList1_t oCreateList1 = nullptr;
HRESULT STDMETHODCALLTYPE CreateList1Hook(ID3D12Device* dev, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                          D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void** out) {
    HRESULT hr = oCreateList1(dev, mask, type, flags, riid, out);
    if (SUCCEEDED(hr) && out && *out && type != D3D12_COMMAND_LIST_TYPE_COPY) HookListVtable(*out);
    return hr;
}

}  // namespace

// Every list the game submits passes here (the overlay's ExecuteCommandLists hook): a vtable not seen yet gets its
// draw functions hooked (hooks at list creation see none of the game's indexed draws).
void DeformNoteSubmitted(void* const* lists, UINT n) {
    if (!lists) return;
    for (UINT i = 0; i < n; ++i) {
        auto* l = static_cast<ID3D12CommandList*>(lists[i]);
        if (!l) continue;
        void* vt = *reinterpret_cast<void**>(l);
        {
            std::lock_guard<std::mutex> g(g_vtMx);
            if (g_doneVt.count(vt)) continue;
        }
        if (l->GetType() == D3D12_COMMAND_LIST_TYPE_COPY) continue;
        HookListVtable(l);
    }
}

int DeformWatchRange(uint64_t lo, uint64_t hi) {
    if (!lo || hi <= lo) return -1;
    for (int i = 0; i < kVbWatch; ++i) {
        uint64_t zero = 0;
        if (g_vbWatch[i].lo.compare_exchange_strong(zero, ~0ull)) {  // taken; the range is set before it counts
            g_vbWatch[i].seen.store(false);
            g_vbWatch[i].hi.store(hi);
            g_vbWatch[i].lo.store(lo);
            g_vbWatching.fetch_add(1);
            return i;
        }
    }
    return -1;
}
bool DeformWatchSeen(int slot) { return slot >= 0 && slot < kVbWatch && g_vbWatch[slot].seen.load(); }
void DeformWatchEnd(int slot) {
    if (slot < 0 || slot >= kVbWatch) return;
    g_vbWatch[slot].hi.store(0);
    g_vbWatch[slot].lo.store(0);
    g_vbWatching.fetch_sub(1);
}
bool DeformDrawHooksLive() { return g_nSetVB.load(std::memory_order_relaxed) > 0; }

void DeformProbe(uint32_t indexCount) {
    g_reports = 0;
    g_probe = indexCount;
}

size_t DeformBuffersSeen() {
    std::lock_guard<std::mutex> l(g_mx);
    return g_bufs.size();
}

// The calls so far, and with probe 1 the index counts of indexed draws.
std::string DeformStats() {
    char b[200];
    snprintf(b, sizeof b, "DrawIndexedInstanced %llu, DrawInstanced %llu, ExecuteIndirect %llu, IASetVertexBuffers %llu; "
             "counts:", static_cast<unsigned long long>(g_nDrawIdx.load()), static_cast<unsigned long long>(g_nDraw.load()),
             static_cast<unsigned long long>(g_nIndirect.load()), static_cast<unsigned long long>(g_nSetVB.load()));
    std::string out = b;
    std::lock_guard<std::mutex> l(g_mx);
    std::vector<std::pair<uint32_t, uint32_t>> v(g_counts.begin(), g_counts.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& c) { return a.first < c.first; });
    for (auto& e : v) out += " " + std::to_string(e.first) + "x" + std::to_string(e.second);
    return out;
}

void InstallDeform() {
    if (!ConfigBool("deform", false)) return;  // measuring only for now: on with `deform = on`
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) || !dev) return;
    void** dvt = *reinterpret_cast<void***>(dev);
    // device: 12 CreateCommandList, 27 CreateCommittedResource, 29 CreatePlacedResource; ID3D12Device4: 51
    // CreateCommandList1
    bool ok = Hook(dvt[12], reinterpret_cast<void*>(&CreateListHook), reinterpret_cast<void**>(&oCreateList)) &&
              Hook(dvt[27], reinterpret_cast<void*>(&CommittedHook), reinterpret_cast<void**>(&oCommitted)) &&
              Hook(dvt[29], reinterpret_cast<void*>(&PlacedHook), reinterpret_cast<void**>(&oPlaced));
    ID3D12Device4* dev4 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dev4))) && dev4) {
        Hook((*reinterpret_cast<void***>(dev4))[51], reinterpret_cast<void*>(&CreateList1Hook),
             reinterpret_cast<void**>(&oCreateList1));
        dev4->Release();
    }
    dev->Release();
    InstallOverlay(true);  // its ExecuteCommandLists hook hands every submitted list to DeformNoteSubmitted
    Log("deform: %s", ok ? "buffer hooks on, draw hooks at each command list vtable the game makes (measuring)"
                         : "hooks NOT installed");
}

}  // namespace tw3se
