// TW3SE core - the outlines of the Witcher senses on any object: an entity's meshes and the world's merged meshes.
//
// In focus mode the game outlines every render proxy whose light channel byte (CRenderProxy+0x121) has bit 0x04 (the
// game's FMV_Interactive: doors, chests; yellow) or bit 0x08 (FMV_Clue, and an actor's EGEF_FocusModeHighlight; red).
// The game sets them through CRenderCommand_SetProxyLightChannels {proxy, value, mask}, from
// CDrawableComponent::SetLightChannels (component+0x164, copied into each new proxy) and from the focus mode
// controller for each gameplay entity. The colours are the four groups CFocusModeEffectPostFX keeps at +0x160 (count
// +0x168; 32 bytes each, the colour premultiplied by its alpha at +0x10), read from
// gameplay\globals\gameplayfx_focusmode_groups.csv: 0 the red one, 1 the yellow one, 2 and 3 the same for the
// colour-blind setting. Group 0 draws the proxies with bit 0x08, group 1 those with 0x04 (measured 24.11. in the game,
// 5.01: a candelabrum with 0x08 red, an object of the world with 0x04 yellow). The effect lives at
// [[GRender]+0x408]+0x18.
//
// A colour of TW3SE's own: the effect draws its mask (R11G11B10) with two full screen passes, each under a stencil
// test of one bit: bit 0x08 writes the constant 0x14 = (0, 1, 0, 0), bit 0x04 (1, 0, 0, 0), times a fade by distance.
// The read mask is in the pipeline (5.01: stencil state 0x47, read mask = reference, EQUAL), and the engine binds no
// other pipeline for another reference (24.11.: a third pass under 0x47 kept the second pass's, tested 0x04 and wrote
// all channels: doors and chests lost their yellow). So the mask pipeline is copied here once per stencil bit, its
// read mask that bit, only B written; after the second pass one more pass goes in with the copy of g_extraBit bound,
// and the pass's own pipeline back after it. B is the one channel left: the mask has no alpha. The composite (a
// patched shader, tools/focus_patch.py: tw3se\shaders\ec02b0473a2af8a4.cso for 5.01) outlines B with the colour of
// the constant 0x1e, set here before its draw.
#include "core.h"

#include <d3d12.h>
#include <intrin.h>

#include <atomic>
#include <cstring>
#include <string>

namespace tw3se {
void RenderPsoCapture(bool on);  // render.cpp
void* RenderPsoCaptured();
void* RenderPsoCapturedList();
void RenderWantDrawHooks();
std::string ShadersPsoNote(void* pso);  // shaders.cpp

namespace {

using Quad_t = void (*)(void*, const float*, const float*, float);
using SetConstants_t = void (*)(uint32_t, const float*, uint32_t);
Quad_t oQuad = nullptr;
SetConstants_t g_setConst = nullptr;
void* g_afterMask = nullptr;       // the return address of the effect's second mask pass
void* g_afterComposite = nullptr;  // and of its composite
// The stencil bit of TW3SE's objects (w3._focus_extra chooses another). 24.11. in the game (Passiflora): 0x10 on
// nothing of the game's; 0x02 on Geralt (characters), 0x04 doors and chests, 0x08 clues
std::atomic<uint8_t> g_extraBit{0x10};
float g_extraColor[4] = {};             // premultiplied; alpha 0 = off
volatile bool g_extraOn = false;

constexpr uint64_t kMaskPs = 0x7af35661cf90da2dULL;  // the mask's pixel shader (5.01)
std::atomic<ID3D12PipelineState*> g_maskCopy[8] = {};  // by bit number

// 24.11.: the pipelines the three quads set, once (w3._focus_probe; render.cpp's SetPipelineState hook)
void* g_afterFirst = nullptr;
std::atomic<int> g_probe{0};
void* g_probePso[4] = {};  // [3]: one the engine set in the extra pass (it should set none)

int BitNumber(uint8_t bit) {
    for (int k = 0; k < 8; ++k)
        if (bit == (1u << k)) return k;
    return -1;
}

// After the second mask pass: its pipeline (pso) and list are known from its SetPipelineState
std::atomic<uint64_t> g_extraRuns{0}, g_extraNoList{0}, g_extraNoCopy{0};  // (w3._focus_probe)

// The stencil reference stays the engine's: set to the bit, the pass drew nothing (24.11., in the game: the copy
// for 0x04 with the reference 0x04 left the doors without B, with the engine's reference they had it)
void ExtraPass(void* list, void* pso, bool probe, void* unused, const float* lo, const float* hi, float z) {
    int k = BitNumber(g_extraBit.load());
    ID3D12PipelineState* copy = k >= 0 ? g_maskCopy[k].load() : nullptr;
    if (!list || !pso) ++g_extraNoList;
    if (!copy) ++g_extraNoCopy;
    if (!list || !pso || !copy) return;
    ++g_extraRuns;
    auto* cl = static_cast<ID3D12GraphicsCommandList*>(list);
    float channel[4] = {0, 0, 1, 0};
    g_setConst(0x14, channel, 1);
    cl->SetPipelineState(copy);
    if (probe) RenderPsoCapture(true);
    oQuad(unused, lo, hi, z);
    if (probe) {
        g_probePso[3] = RenderPsoCaptured();
        RenderPsoCapture(false);
    }
    cl->SetPipelineState(static_cast<ID3D12PipelineState*>(pso));
}

// The stencil census (w3._focus_census): which bits the frame's pixels carry. A full screen pass per bit with a copy
// of the mask pipeline that writes nothing, inside an occlusion query: the samples that pass its stencil test. [8]
// has the read mask 0 (every sample). The results go to a readback buffer of kSlots frames and are read when their
// slot comes round again (the GPU is done with a frame by then).
constexpr int kCounts = 9, kSlots = 8;
std::atomic<ID3D12PipelineState*> g_countCopy[kCounts] = {};
ID3D12QueryHeap* g_queries = nullptr;
ID3D12Resource* g_readback = nullptr;
const uint64_t* g_readMapped = nullptr;
std::atomic<bool> g_census{false};
uint64_t g_censusFrames = 0;
std::atomic<uint64_t> g_censusCounts[kCounts] = {};

void CensusPass(void* list, void* pso, void* unused, const float* lo, const float* hi, float z) {
    if (!list || !pso || !g_queries || !g_readMapped) return;
    for (auto& p : g_countCopy)
        if (!p.load()) return;
    auto* cl = static_cast<ID3D12GraphicsCommandList*>(list);
    UINT slot = static_cast<UINT>(g_censusFrames % kSlots);
    if (g_censusFrames >= kSlots)
        for (int k = 0; k < kCounts; ++k) g_censusCounts[k] = g_readMapped[slot * kCounts + k];
    for (int k = 0; k < kCounts; ++k) {
        cl->SetPipelineState(g_countCopy[k].load());
        cl->BeginQuery(g_queries, D3D12_QUERY_TYPE_OCCLUSION, slot * kCounts + k);
        oQuad(unused, lo, hi, z);
        cl->EndQuery(g_queries, D3D12_QUERY_TYPE_OCCLUSION, slot * kCounts + k);
    }
    cl->ResolveQueryData(g_queries, D3D12_QUERY_TYPE_OCCLUSION, slot * kCounts, kCounts, g_readback,
                         static_cast<UINT64>(slot) * kCounts * 8);
    cl->SetPipelineState(static_cast<ID3D12PipelineState*>(pso));
    ++g_censusFrames;
}

void QuadHook(void* unused, const float* lo, const float* hi, float z) {
    void* ret = _ReturnAddress();
    bool on = g_extraOn;
    if (ret == g_afterComposite && on) {
        float rows[8] = {};  // rows 10 and 11 (11: a second colour the mask has no channel for)
        memcpy(rows, g_extraColor, sizeof g_extraColor);
        g_setConst(0x1e, rows, 2);
    }
    int k = ret == g_afterFirst ? 0 : ret == g_afterMask ? 1 : ret == g_afterComposite ? 2 : -1;
    bool probe = k >= 0 && g_probe.load(std::memory_order_relaxed) > 0;
    bool census = k == 1 && g_census.load(std::memory_order_relaxed);
    bool capture = probe || (k == 1 && on) || census;
    if (capture) RenderPsoCapture(true);
    oQuad(unused, lo, hi, z);
    void *pso = nullptr, *list = nullptr;
    if (capture) {
        pso = RenderPsoCaptured();
        list = RenderPsoCapturedList();
        RenderPsoCapture(false);
    }
    if (probe) {
        g_probePso[k] = pso;
        if (k == 2) g_probe = 0;
    }
    if (k == 1 && on) ExtraPass(list, pso, probe, unused, lo, hi, z);
    if (census) CensusPass(list, pso, unused, lo, hi, z);
}

// The three quads of the effect's draw: the calls to the quad in its first 0x1000 bytes (5.01: 2 mask passes, then
// the composite)
bool FindQuadCalls(uintptr_t apply, uintptr_t quad) {
    void* found[4] = {};
    int n = 0;
    for (uintptr_t p = apply; p < apply + 0x1000 && n < 4; ++p) {
        uint8_t op = 0;
        int32_t rel = 0;
        if (!SafeRead(reinterpret_cast<void*>(p), &op, 1) || op != 0xE8 ||
            !SafeRead(reinterpret_cast<void*>(p + 1), &rel, 4) || p + 5 + rel != quad)
            continue;
        found[n++] = reinterpret_cast<void*>(p + 5);
    }
    if (n != 3) {
        Log("highlight: the focus effect has %d quads, not 3 - the extra colours are off", n);
        return false;
    }
    g_afterFirst = found[0];
    g_afterMask = found[1];
    g_afterComposite = found[2];
    return true;
}

bool Plausible(const void* p) {
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v > 0x10000 && v < 0x7fffffffffffULL && (v & 7) == 0;
}

// the byte and the render thread's own change of it (the command's read-modify-write) may meet: an atomic change here
bool ChangeByte(volatile char* b, uint8_t set, uint8_t clear, uint8_t* before) {
    __try {
        char was = *b;
        for (;;) {
            char now = static_cast<char>((static_cast<uint8_t>(was) & ~clear) | set);
            char seen = _InterlockedCompareExchange8(b, now, was);
            if (seen == was) break;
            was = seen;
        }
        if (before) *before = static_cast<uint8_t>(was);
        return true;
    } __except (1) {
        return false;
    }
}

char* FocusEffect() {
    void** renderVar = reinterpret_cast<void**>(Address("GRender"));
    char *render = nullptr, *gfx = nullptr, *fx = nullptr;
    if (!renderVar || !SafeRead(renderVar, &render, 8) || !Plausible(render) || !SafeRead(render + 0x408, &gfx, 8) ||
        !Plausible(gfx) || !SafeRead(gfx + 0x18, &fx, 8) || !Plausible(fx))
        return nullptr;
    static void* known = nullptr;  // the type is looked at once per object
    if (fx != known) {
        if (ObjectTypeName(fx) != ".?AVCFocusModeEffectPostFX@@") return nullptr;
        known = fx;
    }
    return fx;
}

}  // namespace

// Sets and clears bits of a render proxy's light channels; before gets the byte as it was
bool ProxyChannels(void* proxy, uint8_t set, uint8_t clear, uint8_t* before) {
    if (!Plausible(proxy) || ObjectTypeName(proxy).rfind(".?AVCRenderProxy_", 0) != 0) return false;
    return ChangeByte(static_cast<volatile char*>(proxy) + 0x121, set, clear, before);
}

// A colour group of the focus mode: rgba gets its colour (premultiplied, as the effect keeps it); with set, that
// colour first
bool FocusGroupColor(int group, float* rgba, const float* set) {
    char* fx = FocusEffect();
    char* groups = nullptr;
    uint32_t n = 0;
    if (!fx || group < 0 || !SafeRead(fx + 0x160, &groups, 8) || !Plausible(groups) || !SafeRead(fx + 0x168, &n, 4) ||
        static_cast<uint32_t>(group) >= n || n > 16)
        return false;
    float* c = reinterpret_cast<float*>(groups + group * 32 + 0x10);
    if (set) {
        __try {
            memcpy(c, set, 16);
        } __except (1) {
            return false;
        }
    }
    return SafeRead(c, rgba, 16);
}

// TW3SE's colour (slot 0; the mask has no channel for a slot 1): rgba (premultiplied) or null for off. -> the stencil
// bit its objects carry, 0 when the colour is not there (the quad hook or the patched composite missing)
uint8_t FocusExtraColor(int slot, const float* rgba, uint8_t bit) {
    if (slot != 0 || !oQuad || (bit && BitNumber(bit) < 0)) return 0;
    if (bit) g_extraBit = bit;
    for (int k = 0; k < 4; ++k) g_extraColor[k] = rgba ? rgba[k] : 0.f;
    if (rgba) RenderWantDrawHooks();  // (the pass finds the mask's list by the SetPipelineState hook)
    g_extraOn = g_extraColor[3] > 0;
    return g_extraBit;
}

// The mask pipeline of the second pass, copied for each stencil bit when the game makes it (shaders.cpp's hook). The
// copies are kept: a graphics rebuild makes the pipeline anew, and the old copies may still be on the GPU.
void HighlightNotePipeline(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, uint64_t ps) {
    static thread_local bool inside = false;  // (the copies pass the same hook)
    if (ps != kMaskPs || inside || !dev || !desc.DepthStencilState.StencilEnable ||
        desc.DepthStencilState.StencilReadMask != 0x04)
        return;
    inside = true;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC c = desc;
    c.CachedPSO = {};  // (a cached pipeline of the game's is made from the game's description)
    for (auto& t : c.BlendState.RenderTarget) t.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_BLUE;
    int made = 0;
    for (int k = 0; k < 8; ++k) {
        c.DepthStencilState.StencilReadMask = static_cast<UINT8>(1u << k);
        ID3D12PipelineState* p = nullptr;
        if (FAILED(dev->CreateGraphicsPipelineState(&c, IID_PPV_ARGS(&p))) || !p) continue;
        g_maskCopy[k] = p;
        ++made;
    }
    // the census: copies that write nothing, the query heap and the readback buffer
    for (auto& t : c.BlendState.RenderTarget) t.RenderTargetWriteMask = 0;
    for (int k = 0; k < kCounts; ++k) {
        c.DepthStencilState.StencilReadMask = static_cast<UINT8>(k < 8 ? 1u << k : 0);
        ID3D12PipelineState* p = nullptr;
        if (SUCCEEDED(dev->CreateGraphicsPipelineState(&c, IID_PPV_ARGS(&p))) && p) g_countCopy[k] = p;
    }
    if (!g_queries) {
        D3D12_QUERY_HEAP_DESC qd = {D3D12_QUERY_HEAP_TYPE_OCCLUSION, kCounts * kSlots, 0};
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = kCounts * kSlots * 8;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12QueryHeap* q = nullptr;
        ID3D12Resource* r = nullptr;
        void* mapped = nullptr;
        if (SUCCEEDED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&q))) &&
            SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&r))) &&
            SUCCEEDED(r->Map(0, nullptr, &mapped))) {
            g_readback = r;
            g_readMapped = static_cast<const uint64_t*>(mapped);
            g_queries = q;
        } else {
            if (q) q->Release();
            if (r) r->Release();
            Log("highlight: the stencil census has no query heap");
        }
    }
    inside = false;
    Log("highlight: the focus mask's pipeline copied for %d of 8 stencil bits", made);
}

// The stencil census: on 1 / off 0 / as it is -1; out gets the samples per bit (0x01 .. 0x80) and all. -> the frames
// counted so far (0: not counting or not there)
uint64_t FocusCensus(int on, uint64_t out[9]) {
    if (on >= 0) {
        if (on) RenderWantDrawHooks();
        g_census = on > 0 && oQuad;
    }
    for (int k = 0; k < kCounts; ++k) out[k] = g_censusCounts[k].load();
    return g_census ? g_censusFrames : 0;
}

// The pipelines the effect's three quads set the last time (a probe asked for before), and a new probe asked for
std::string FocusProbe() {
    std::string s;
    for (int k = 0; k < 4; ++k) {
        char b[48];
        snprintf(b, sizeof b, "%squad %d %p ", k ? "\n" : "", k, g_probePso[k]);
        s += b;
        if (g_probePso[k]) s += ShadersPsoNote(g_probePso[k]);
        g_probePso[k] = nullptr;
    }
    char b[160];
    snprintf(b, sizeof b, "\nextra pass: %llu drawn, %llu without the mask's list, %llu without a copy (bit %02x)",
             static_cast<unsigned long long>(g_extraRuns.load()), static_cast<unsigned long long>(g_extraNoList.load()),
             static_cast<unsigned long long>(g_extraNoCopy.load()), g_extraBit.load());
    s += b;
    RenderWantDrawHooks();
    g_probe = oQuad ? 1 : 0;
    return s;
}

void InstallHighlight() {
    uintptr_t apply = Address("CFocusModeEffectPostFX::Apply"), quad = Address("RenderFullscreenQuad");
    g_setConst = reinterpret_cast<SetConstants_t>(Address("RenderSetPixelConstants"));
    if (!apply || !quad || !g_setConst || !FindQuadCalls(apply, quad)) return;
    // the composite has to read the extra channel: without the patched shader the colour stays off
    if (GetFileAttributesW((Dir() + L"\\shaders\\ec02b0473a2af8a4.cso").c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("highlight: tw3se\\shaders\\ec02b0473a2af8a4.cso is missing - the extra colours are off");
        return;
    }
    if (!Hook(reinterpret_cast<void*>(quad), reinterpret_cast<void*>(&QuadHook), reinterpret_cast<void**>(&oQuad)))
        oQuad = nullptr;
    Log("highlight: extra colour %s", oQuad ? "on" : "off (the hook failed)");
}

}  // namespace tw3se
