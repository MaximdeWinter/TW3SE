// TW3SE core - the render API of plugins (TW3SE 1.2): what mods like Dynamic Grass took ReShade's add-on API
// for. Pipelines when the game creates them, shaders replaced from memory, parameters appended to a root signature of
// the game (shaders.cpp), and a callback on the command list whenever the game sets a pipeline a plugin marked: there
// the plugin binds its own data (SetGraphicsRootConstantBufferView ... at the index RootParamIndex gives).
//
// The game's command lists have their own vtables (deform.cpp: a list made here has others). Each vtable gets
// its SetPipelineState, SetGraphicsRootSignature and Reset hooked when a list of it is submitted the first time
// (overlay.cpp's ExecuteCommandLists hook), up to four different implementations of each.
#include "core.h"

#include <d3d12.h>

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace tw3se {

bool ShadersEnsureHooks();
int ShadersReplaceMemory(uint64_t id, const void* data, size_t size);
int ShadersAddRootParam(uint64_t rsId, const D3D12_ROOT_PARAMETER1* p);
int ShadersRootParamIndex(void* rs, int handle);
bool ShadersIsExtended(void* rs);
int ShadersOnPipeline(TW3SE_PipelineCallback cb, void* ctx);
int ShadersDropIn(uintptr_t lo, uintptr_t hi);
uint64_t RenderSubmitFrames();
// skinhook.cpp: the skinning shader hook (slot of the mesh drawn, its table)
bool SkinHookOn();
bool SkinHookPso(void* pso);
bool SkinHookIndices(void* rs, int* table, int* slot, int* weights, int* mushQ, int* mushO, int* overlay);
uint64_t SkinHookOverlayVa();
uint32_t SkinHookSlotOf(uint64_t va, uint32_t* weights, uint32_t* mush);
uint64_t MushQVa();
uint64_t MushOVa();
void SkinHookNoteDraw(void* pso, uint32_t slot);
uint32_t MushQHalf();
uint64_t SkinHookTableVa();
uint64_t SkinHookWeightsVa();
uint64_t SkinHookSlotVa(const uint32_t v[4]);
bool SkinHookComputeIndices(void* rs, int* table, int* weights, int* capture);
uint64_t SkinHookCaptureVa();
uint64_t SkinHookMissVs(void* pso);
void SkinHookMissedDraw(uint64_t vs, uint64_t vb0);
void SkinHookFrameStart();

namespace {

struct Mark {
    TW3SE_BindCallback cb;
    void* cookie;
    void* ctx;
};
std::shared_mutex g_markMx;
std::unordered_map<void*, Mark> g_marks;  // pipeline state -> the plugin's callback
std::atomic<bool> g_any{false};
std::atomic<uint64_t> g_fired{0};

struct ListState {
    void* pso = nullptr;
    void* rs = nullptr;
    // the skinning hook (skinhook.cpp): the vertex buffer at stream 0, whether the pipeline set has a patched
    // shader, our parameters' indices in the signature set, what is bound / set of ours now
    uint64_t vb0 = 0;
    bool skin = false, skinBound = false;
    uint64_t skinMiss = 0;  // the pipeline's skinning shader without a patch (its id)
    int skinTable = -1, skinSlot = -1, skinWeights = -1, skinMushQ = -1, skinMushO = -1, skinOverlay = -1;
    uint32_t skinSet = ~0u, skinBase = ~0u, skinMush = ~0u;
};
thread_local std::unordered_map<void*, ListState> t_lists;  // a list is recorded on one thread at a time

bool FireGuarded(TW3SE_BindCallback cb, void* cl, void* pso, void* rs, void* cookie, void* ctx) {
    __try {
        cb(cl, pso, rs, cookie, ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The plugin binds only on a signature TW3SE extended: on another one its parameter index would not exist.
void Fire(void* cl, const ListState& st) {
    if (!st.pso || !st.rs || !ShadersIsExtended(st.rs)) return;
    Mark m;
    {
        std::shared_lock<std::shared_mutex> l(g_markMx);
        auto it = g_marks.find(st.pso);
        if (it == g_marks.end()) return;
        m = it->second;
    }
    g_fired.fetch_add(1, std::memory_order_relaxed);
    if (!FireGuarded(m.cb, cl, st.pso, st.rs, m.cookie, m.ctx)) {
        Log("render: a plugin's bind callback faulted - its pipeline is unmarked");
        std::unique_lock<std::shared_mutex> l(g_markMx);
        g_marks.erase(st.pso);
    }
}

constexpr int kVariants = 4;
using SetPso_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using SetRs_t = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
SetPso_t g_trPso[kVariants];
SetRs_t g_trRs[kVariants];
SetRs_t g_trCrs[kVariants];
Reset_t g_trReset[kVariants];

// 19.11.: the trace of the pipelines a few frames set (w3._pso_trace): how often each, written to a file at the end
std::atomic<int> g_traceFrames{0};
std::mutex g_traceMx;
std::unordered_map<void*, uint64_t> g_traceCounts;
std::wstring g_tracePath;
void TraceCount(void* pso) {
    if (g_traceFrames.load(std::memory_order_relaxed) <= 0 || !pso) return;
    std::lock_guard<std::mutex> l(g_traceMx);
    ++g_traceCounts[pso];
}

// 24.11.: the pipeline one call of the engine sets on this thread (highlight.cpp: which one draws the focus outlines)
thread_local bool t_psoCapture = false;
thread_local void* t_psoCaptured = nullptr;
thread_local void* t_psoCapturedList = nullptr;  // and the list it went on

template <int K>
void STDMETHODCALLTYPE SetPsoHook(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso) {
    g_trPso[K](cl, pso);
    TraceCount(pso);
    if (t_psoCapture) {
        t_psoCaptured = pso;
        t_psoCapturedList = cl;
    }
    bool skin = SkinHookOn();
    if (!g_any.load(std::memory_order_relaxed) && !skin) return;
    ListState& st = t_lists[cl];
    st.pso = pso;
    if (skin) {
        st.skin = SkinHookPso(pso);
        st.skinMiss = st.skin ? 0 : SkinHookMissVs(pso);
    }
    if (g_any.load(std::memory_order_relaxed)) Fire(cl, st);
}
template <int K>
void STDMETHODCALLTYPE SetRsHook(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* rs) {
    g_trRs[K](cl, rs);  // (a new signature drops the bindings: the plugin binds again)
    bool skin = SkinHookOn();
    if (!g_any.load(std::memory_order_relaxed) && !skin) return;
    ListState& st = t_lists[cl];
    st.rs = rs;
    if (skin) {
        if (!SkinHookIndices(rs, &st.skinTable, &st.skinSlot, &st.skinWeights, &st.skinMushQ, &st.skinMushO,
                             &st.skinOverlay))
            st.skinTable = st.skinSlot = st.skinWeights = -1;
        st.skinBound = false;
        st.skinSet = st.skinBase = st.skinMush = ~0u;
    }
    if (g_any.load(std::memory_order_relaxed)) Fire(cl, st);
}
// The ray tracing skinning's signature (skinhook.cpp): our table and weights bound at once (they stay until the
// list sets another signature).
template <int K>
void STDMETHODCALLTYPE SetCrsHook(ID3D12GraphicsCommandList* cl, ID3D12RootSignature* rs) {
    g_trCrs[K](cl, rs);
    int t = -1, w = -1, c = -1;
    if (!SkinHookOn() || !SkinHookComputeIndices(rs, &t, &w, &c)) return;
    if (uint64_t cv = SkinHookCaptureVa(); c >= 0 && cv) cl->SetComputeRootUnorderedAccessView(static_cast<UINT>(c), cv);
    uint64_t tv = SkinHookTableVa(), wv = SkinHookWeightsVa();
    if (tv && wv) {
        cl->SetComputeRootConstantBufferView(static_cast<UINT>(t), tv);
        cl->SetComputeRootShaderResourceView(static_cast<UINT>(w), wv);
    }
}
template <int K>
HRESULT STDMETHODCALLTYPE ResetHook(ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* a, ID3D12PipelineState* pso) {
    HRESULT hr = g_trReset[K](cl, a, pso);
    if (g_any.load(std::memory_order_relaxed) || SkinHookOn()) {
        ListState st;
        st.pso = pso;
        if (SkinHookOn()) {
            st.skin = pso && SkinHookPso(pso);
            st.skinMiss = pso && !st.skin ? SkinHookMissVs(pso) : 0;
        }
        t_lists[cl] = st;
    }
    return hr;
}

// The skinning hook before a draw with a patched pipeline: the table and the weights bound, the slot of the vertex
// buffer at stream 0 and where its chunk's weights start set (only when they change; a new signature drops all). deform.cpp's draw hooks call it (RenderSkinBeforeDraw).
std::atomic<uint64_t> g_skinDraws{0}, g_skinNoTable{0}, g_skinNoList{0};
void SkinBeforeDraw(ID3D12GraphicsCommandList* cl) {
    auto it = t_lists.find(cl);
    if (it == t_lists.end()) {
        g_skinNoList.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ListState& st = it->second;
    if (st.skinMiss) SkinHookMissedDraw(st.skinMiss, st.vb0);
    if (st.skin) g_skinDraws.fetch_add(1, std::memory_order_relaxed);
    if (st.skin && st.skinTable < 0) g_skinNoTable.fetch_add(1, std::memory_order_relaxed);
    if (!st.skin || st.skinTable < 0) return;
    if (!st.skinBound) {
        if (uint64_t va = SkinHookTableVa()) cl->SetGraphicsRootConstantBufferView(static_cast<UINT>(st.skinTable), va);
        if (uint64_t va = SkinHookWeightsVa()) cl->SetGraphicsRootShaderResourceView(static_cast<UINT>(st.skinWeights), va);
        uint64_t qv = MushQVa(), ov = MushOVa();
        if (st.skinMushQ >= 0 && qv) cl->SetGraphicsRootUnorderedAccessView(static_cast<UINT>(st.skinMushQ), qv);
        if (st.skinMushO >= 0 && ov) cl->SetGraphicsRootShaderResourceView(static_cast<UINT>(st.skinMushO), ov);
        if (uint64_t sv = SkinHookOverlayVa(); st.skinOverlay >= 0 && sv)
            cl->SetGraphicsRootShaderResourceView(static_cast<UINT>(st.skinOverlay), sv);
        st.skinBound = true;
    }
    uint32_t base = ~0u;
    uint32_t mush = ~0u;
    uint32_t slot = SkinHookSlotOf(st.vb0, &base, &mush);
    if (slot) SkinHookNoteDraw(st.pso, slot);
    if (!MushQVa() || !MushOVa() || st.skinMushQ < 0 || st.skinMushO < 0) mush = ~0u;  // (nothing bound to write to)
    if (mush != ~0u) st.skinMush = ~0u - 1;  // (the half changes each frame: set every draw of a mush slot)
    if (slot != st.skinSet || base != st.skinBase || mush != st.skinMush) {
        uint32_t v[4] = {slot, base, mush, mush == ~0u ? 0u : MushQHalf()};
        uint64_t sv = SkinHookSlotVa(v);
        if (!sv) return;
        cl->SetGraphicsRootConstantBufferView(static_cast<UINT>(st.skinSlot), sv);
        st.skinSet = slot;
        st.skinBase = base;
        st.skinMush = mush;
    }
}

void* const kPso[kVariants] = {reinterpret_cast<void*>(&SetPsoHook<0>), reinterpret_cast<void*>(&SetPsoHook<1>), reinterpret_cast<void*>(&SetPsoHook<2>), reinterpret_cast<void*>(&SetPsoHook<3>)};
void* const kRs[kVariants] = {reinterpret_cast<void*>(&SetRsHook<0>), reinterpret_cast<void*>(&SetRsHook<1>), reinterpret_cast<void*>(&SetRsHook<2>), reinterpret_cast<void*>(&SetRsHook<3>)};
void* const kCrs[kVariants] = {reinterpret_cast<void*>(&SetCrsHook<0>), reinterpret_cast<void*>(&SetCrsHook<1>), reinterpret_cast<void*>(&SetCrsHook<2>), reinterpret_cast<void*>(&SetCrsHook<3>)};
void* const kReset[kVariants] = {reinterpret_cast<void*>(&ResetHook<0>), reinterpret_cast<void*>(&ResetHook<1>), reinterpret_cast<void*>(&ResetHook<2>), reinterpret_cast<void*>(&ResetHook<3>)};

std::mutex g_vtMx;
std::unordered_map<void*, int> g_doneVt, g_donePso, g_doneRs, g_doneCrs, g_doneReset;
int g_usedPso = 0, g_usedRs = 0, g_usedCrs = 0, g_usedReset = 0;
bool g_listHooksWanted = false;

template <class Tr>
void HookSlot(void* fn, std::unordered_map<void*, int>& done, int& used, void* const (&hooks)[kVariants],
              Tr (&tr)[kVariants], const char* what) {
    if (!fn || done.count(fn)) return;
    if (used >= kVariants) {
        Log("render: %s function number %d - not hooked (room for %d)", what, used + 1, kVariants);
        return;
    }
    int k = used++;
    if (Hook(fn, hooks[k], reinterpret_cast<void**>(&tr[k]))) done[fn] = k;
    else Log("render: %s function %p not hooked", what, fn);
}

void HookListVtable(void* list) {
    void** vt = *static_cast<void***>(list);
    std::lock_guard<std::mutex> l(g_vtMx);
    if (g_doneVt.count(vt)) return;
    g_doneVt[vt] = 1;
    // ID3D12GraphicsCommandList: 10 Reset, 25 SetPipelineState, 29 SetComputeRootSignature, 30 SetGraphicsRootSignature
    HookSlot(vt[10], g_doneReset, g_usedReset, kReset, g_trReset, "Reset");
    HookSlot(vt[25], g_donePso, g_usedPso, kPso, g_trPso, "SetPipelineState");
    if (SkinHookOn()) HookSlot(vt[29], g_doneCrs, g_usedCrs, kCrs, g_trCrs, "SetComputeRootSignature");
    HookSlot(vt[30], g_doneRs, g_usedRs, kRs, g_trRs, "SetGraphicsRootSignature");
    Log("render: command list vtable %p hooked (%d implementation(s) of SetPipelineState)", static_cast<void*>(vt), g_usedPso);
}

// The overlay's hooks (its ExecuteCommandLists hook shows the game's lists) go in at the first tick: installed while
// the game starts, they end the game after the swap chain's first ResizeBuffers (see overlay.cpp)
void OverlayAtFirstTick(float, void*) {
    static bool done = false;
    if (done) return;
    done = true;
    InstallOverlay(true);
}

void Want() {  // the first use of the render API: device hooks now, list hooks once the game runs
    static std::once_flag once;
    std::call_once(once, [] {
        ShadersEnsureHooks();
        g_listHooksWanted = true;
        RegisterTick(&OverlayAtFirstTick, nullptr);
        RegisterCommand("render", [](const char*, char* reply, uint32_t size) {
            size_t marks;
            {
                std::shared_lock<std::shared_mutex> l(g_markMx);
                marks = g_marks.size();
            }
            snprintf(reply, size, "render: %zu marked pipeline(s), %llu bind callback(s) so far, %d list vtable(s), "
                     "%llu frame(s) of submit callbacks", marks, static_cast<unsigned long long>(g_fired.load()),
                     static_cast<int>(g_doneVt.size()), static_cast<unsigned long long>(RenderSubmitFrames()));
        }, "render: the render API of plugins (marked pipelines, bind callbacks)");
    });
}

}  // namespace

// overlay.cpp's ExecuteCommandLists hook: the game's lists, their vtables hooked once the render API is used
void RenderNoteSubmitted(void* const* lists, UINT n) {
    if (!g_listHooksWanted || !lists) return;
    for (UINT i = 0; i < n; ++i) {
        auto* l = static_cast<ID3D12CommandList*>(lists[i]);
        if (!l) continue;
        void* vt = *reinterpret_cast<void**>(l);
        {
            std::lock_guard<std::mutex> g(g_vtMx);
            if (g_doneVt.count(vt)) continue;
        }
        if (l->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT && l->GetType() != D3D12_COMMAND_LIST_TYPE_BUNDLE) continue;
        HookListVtable(l);
    }
}

// The skinning hook (skinhook.cpp): the list hooks. The vertex buffers and the draws come from deform.cpp's hooks
// (they are on every list the game submits; a second hook on the same driver function is refused).
void RenderWantDrawHooks() { Want(); }
void RenderPsoCapture(bool on) {
    t_psoCapture = on;
    if (on) t_psoCaptured = t_psoCapturedList = nullptr;
}
void* RenderPsoCaptured() { return t_psoCaptured; }
void* RenderPsoCapturedList() { return t_psoCapturedList; }
std::string RenderSkinDiag() {
    char b[160];
    snprintf(b, sizeof b, "; draws: patched pipeline %llu, of them without the table's index %llu, list unknown %llu",
             static_cast<unsigned long long>(g_skinDraws.load()), static_cast<unsigned long long>(g_skinNoTable.load()),
             static_cast<unsigned long long>(g_skinNoList.load()));
    return b;
}
void RenderSkinNoteVB(void* cl, UINT start, UINT n, const void* views) {
    if (start != 0 || !n || !views || !SkinHookOn()) return;
    auto it = t_lists.find(cl);
    if (it != t_lists.end()) it->second.vb0 = static_cast<const D3D12_VERTEX_BUFFER_VIEW*>(views)[0].BufferLocation;
}
void RenderSkinBeforeDraw(void* cl) {
    if (SkinHookOn()) SkinBeforeDraw(static_cast<ID3D12GraphicsCommandList*>(cl));
}

int RenderOnPipeline(TW3SE_PipelineCallback cb, void* ctx) {
    Want();
    return ShadersOnPipeline(cb, ctx);
}
int RenderShaderReplace(uint64_t id, const void* data, size_t size) {
    Want();
    return ShadersReplaceMemory(id, data, size);
}
int RenderAddRootParam(uint64_t rsId, const void* param) {
    Want();
    return ShadersAddRootParam(rsId, static_cast<const D3D12_ROOT_PARAMETER1*>(param));
}
int RenderRootParamIndex(void* rs, int handle) { return ShadersRootParamIndex(rs, handle); }
int RenderMarkPipeline(void* pso, TW3SE_BindCallback cb, void* cookie, void* ctx) {
    if (!pso) return 0;
    Want();
    std::unique_lock<std::shared_mutex> l(g_markMx);
    if (!cb) g_marks.erase(pso);
    else g_marks[pso] = Mark{cb, cookie, ctx};
    g_any = !g_marks.empty();
    return 1;
}
// Plugins' work of the frame on the game's graphics queue (RegisterOnSubmit)
namespace {
struct SubmitListener {
    TW3SE_SubmitCallback cb;
    void* ctx;
};
std::mutex g_subMx;
std::vector<SubmitListener> g_submit;
std::atomic<bool> g_frameStart{false};
std::atomic<uint64_t> g_submits{0};

bool SubmitGuarded(TW3SE_SubmitCallback cb, void* q, void* ctx) {
    __try {
        cb(q, ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

int RenderOnSubmit(TW3SE_SubmitCallback cb, void* ctx) {
    if (!cb) return 0;
    Want();
    std::lock_guard<std::mutex> l(g_subMx);
    g_submit.push_back({cb, ctx});
    return 1;
}
std::string ShadersPsoNote(void* pso);
void TraceWrite() {
    std::unordered_map<void*, uint64_t> counts;
    std::wstring path;
    {
        std::lock_guard<std::mutex> l(g_traceMx);
        counts.swap(g_traceCounts);
        path = g_tracePath;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") || !f) return;
    for (const auto& c : counts)
        fprintf(f, "%llu %p %s\n", static_cast<unsigned long long>(c.second), c.first, ShadersPsoNote(c.first).c_str());
    fclose(f);
    Log("render: pipeline trace of %zu pipelines written", counts.size());
}
void RenderOnPresent() {
    g_frameStart = true;
    if (g_traceFrames.load(std::memory_order_relaxed) > 0 && g_traceFrames.fetch_sub(1) == 1) TraceWrite();
}
// 19.11.: the pipelines the next `frames` frames set, to `path` (count, pipeline, its note a line)
bool RenderPsoTrace(int frames, const std::wstring& path) {
    if (frames <= 0 || g_traceFrames.load() > 0) return false;
    Want();
    {
        std::lock_guard<std::mutex> l(g_traceMx);
        g_traceCounts.clear();
        g_tracePath = path;
    }
    g_traceFrames = frames;
    return true;
}
uint64_t RenderSubmitFrames() { return g_submits.load(); }
void RenderBeforeSubmit(void* queue, void* gameQueue) {
    // the queue the game presents with (latched at its first Present): Streamline's own queues refuse other work
    if (!queue || queue != gameQueue || !g_frameStart.load(std::memory_order_relaxed)) return;
    bool skin = SkinHookOn();
    std::vector<SubmitListener> list;
    {
        std::lock_guard<std::mutex> l(g_subMx);
        if (g_submit.empty() && !skin) return;
        list = g_submit;
    }
    if (!g_frameStart.exchange(false)) return;  // (a plugin's own submission comes here again: once per frame)
    if (skin) SkinHookFrameStart();  // the skinning hook's table of this frame
    if (list.empty()) return;
    g_submits.fetch_add(1, std::memory_order_relaxed);
    for (const auto& s : list)
        if (!SubmitGuarded(s.cb, queue, s.ctx)) Log("render: a plugin's submit callback faulted");
}

int DropRenderIn(uintptr_t lo, uintptr_t hi) {
    int n = ShadersDropIn(lo, hi);
    {
        std::lock_guard<std::mutex> g(g_subMx);
        for (size_t i = g_submit.size(); i-- > 0;) {
            auto a = reinterpret_cast<uintptr_t>(g_submit[i].cb);
            if (a >= lo && a < hi) {
                g_submit.erase(g_submit.begin() + static_cast<long>(i));
                ++n;
            }
        }
    }
    std::unique_lock<std::shared_mutex> l(g_markMx);
    for (auto it = g_marks.begin(); it != g_marks.end();) {
        auto a = reinterpret_cast<uintptr_t>(it->second.cb);
        if (a >= lo && a < hi) {
            it = g_marks.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    g_any = !g_marks.empty();
    return n;
}

}  // namespace tw3se
