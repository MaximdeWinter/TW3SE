// TW3SE core - DLSS Frame Generation and the overlay (Streamline, deps\streamline, MIT).
//
// With DLSS-G the game's present goes through Streamline, which makes frames between two of the game's. It tells
// the scene from the UI by the buffers the game tags (slSetTagForFrame): the picture without UI (kBufferTypeHUDLess
// Color) and the UI alone (kBufferTypeUIAlpha, R8 on 5.02). The overlay's present hook sits on the native present of
// Streamline's pacer queue, after frame generation (measured 27.11.): it draws on every frame shown, generated ones
// too, and is in no tagged buffer.
//
// This file watches what the game tags, on the way through: every buffer type once into tw3se.log with its format,
// size, state and lifecycle, the frame generation options as they change, and counts for `overlaystats`.
#include "core.h"

#include <d3d12.h>

#include <map>
#include <mutex>
#include <string>

#include "../../deps/streamline/include/sl.h"
#include "../../deps/streamline/include/sl_dlss_g.h"
#include "../../deps/streamline/include/sl_pcl.h"

namespace tw3se {
namespace {

using TagForFrame_t = sl::Result (*)(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*,
                                     uint32_t, sl::CommandBuffer*);
using Tag_t = sl::Result (*)(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
TagForFrame_t oTagForFrame = nullptr;
Tag_t oTag = nullptr;

using SetOptions_t = sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSGOptions&);
using GetState_t = sl::Result (*)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*);
using Marker_t = sl::Result (*)(sl::PCLMarker, const sl::FrameToken&);
Marker_t oMarker = nullptr;
SetOptions_t oSetOptions = nullptr;
GetState_t oGetState = nullptr;

std::mutex g_mx;
std::map<uint32_t, uint64_t> g_seen;  // buffer type -> how often tagged
std::map<uint32_t, bool> g_seenRes;   // buffer type -> logged with a resource
std::map<uint32_t, uint64_t> g_markers;  // PCL marker -> how often
std::map<uint32_t, DWORD> g_markerThread;
bool g_installed = false;
// frame generation as the game sets it, and what Streamline says it presented
uint32_t g_fgMode = 0xffffffff, g_fgFrames = 0, g_fgFlags = 0, g_fgRecompose = 0, g_fgStatus = 0, g_fgMax = 0;
uint64_t g_fgStates = 0, g_fgPresented = 0;

const char* TypeName(uint32_t t) {
    switch (t) {
        case sl::kBufferTypeDepth: return "depth";
        case sl::kBufferTypeMotionVectors: return "motion vectors";
        case sl::kBufferTypeHUDLessColor: return "HUD-less colour";
        case sl::kBufferTypeScalingInputColor: return "scaling input colour";
        case sl::kBufferTypeScalingOutputColor: return "scaling output colour";
        case sl::kBufferTypeExposure: return "exposure";
        case sl::kBufferTypeUIColorAndAlpha: return "UI colour and alpha";
        case sl::kBufferTypeUIAlpha: return "UI alpha";
        case sl::kBufferTypeBackbuffer: return "back buffer";
        default: return "other";
    }
}

bool DescGuarded(void* native, D3D12_RESOURCE_DESC* d) {
    __try {
        *d = static_cast<ID3D12Resource*>(native)->GetDesc();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Note(const char* via, uint32_t viewport, const sl::ResourceTag* tags, uint32_t n) {
    if (!tags || n > 64) return;
    std::lock_guard<std::mutex> l(g_mx);
    for (uint32_t i = 0; i < n; ++i) {
        const sl::ResourceTag& t = tags[i];
        bool first = ++g_seen[t.type] == 1;
        bool res = t.resource && t.resource->native;
        if (!first && (!res || g_seenRes[t.type])) continue;
        if (res) g_seenRes[t.type] = true;
        D3D12_RESOURCE_DESC d = {};
        bool desc = t.resource && t.resource->native && DescGuarded(t.resource->native, &d);
        Log("slui: the game tags %s (%u) through %s: viewport %u, %s%llux%u format %d, state 0x%x, lifecycle %d, "
            "extent %u,%u %ux%u",
            TypeName(t.type), t.type, via, viewport, t.resource ? "" : "no resource, ",
            static_cast<unsigned long long>(d.Width), d.Height, desc ? static_cast<int>(d.Format) : -1,
            t.resource ? t.resource->state : 0, static_cast<int>(t.lifecycle), t.extent.left, t.extent.top,
            t.extent.width, t.extent.height);
    }
}

sl::Result TagForFrameHook(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* tags,
                           uint32_t n, sl::CommandBuffer* cmd) {
    Note("slSetTagForFrame", static_cast<uint32_t>(viewport), tags, n);
    return oTagForFrame(frame, viewport, tags, n, cmd);
}
sl::Result TagHook(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t n, sl::CommandBuffer* cmd) {
    Note("slSetTag", static_cast<uint32_t>(viewport), tags, n);
    return oTag(viewport, tags, n, cmd);
}

sl::Result MarkerHook(sl::PCLMarker m, const sl::FrameToken& frame) {
    {
        std::lock_guard<std::mutex> l(g_mx);
        uint32_t k = static_cast<uint32_t>(m);
        if (++g_markers[k] == 1) {
            g_markerThread[k] = GetCurrentThreadId();
            Log("slui: the game sets latency marker %u (thread %lu)", k, GetCurrentThreadId());
        }
    }
    return oMarker(m, frame);
}

sl::Result SetOptionsHook(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& o) {
    {
        std::lock_guard<std::mutex> l(g_mx);
        uint32_t recompose = o.structVersion >= sl::kStructVersion4 ? static_cast<uint32_t>(o.enableUserInterfaceRecomposition) : 0;
        if (static_cast<uint32_t>(o.mode) != g_fgMode || o.numFramesToGenerate != g_fgFrames ||
            static_cast<uint32_t>(o.flags) != g_fgFlags || recompose != g_fgRecompose) {
            g_fgMode = static_cast<uint32_t>(o.mode), g_fgFrames = o.numFramesToGenerate;
            g_fgFlags = static_cast<uint32_t>(o.flags), g_fgRecompose = recompose;
            Log("slui: the game sets frame generation: viewport %u, mode %u, %u frame(s) generated, flags 0x%x, "
                "UI recomposition %u, back buffers %u, formats colour %u hud-less %u UI %u (options v%zu)",
                static_cast<uint32_t>(viewport), g_fgMode, g_fgFrames, g_fgFlags, recompose, o.numBackBuffers,
                o.colorBufferFormat, o.hudLessBufferFormat, o.uiBufferFormat, o.structVersion);
        }
    }
    return oSetOptions(viewport, o);
}
sl::Result GetStateHook(const sl::ViewportHandle& viewport, sl::DLSSGState& st, const sl::DLSSGOptions* o) {
    sl::Result r = oGetState(viewport, st, o);
    std::lock_guard<std::mutex> l(g_mx);
    ++g_fgStates;
    g_fgPresented += st.numFramesActuallyPresented;
    g_fgStatus = static_cast<uint32_t>(st.status);
    if (st.structVersion >= sl::kStructVersion2) g_fgMax = st.numFramesToGenerateMax;
    return r;
}

}  // namespace

// Once Streamline's interposer is in the process (the overlay's start): its tag functions hooked, to watch them.
void InstallStreamlineWatch() {
    if (g_installed) return;
    HMODULE sl = GetModuleHandleW(L"sl.interposer.dll");
    if (!sl) return;
    g_installed = true;
    void* a = reinterpret_cast<void*>(GetProcAddress(sl, "slSetTagForFrame"));
    void* b = reinterpret_cast<void*>(GetProcAddress(sl, "slSetTag"));
    bool ha = a && Hook(a, reinterpret_cast<void*>(&TagForFrameHook), reinterpret_cast<void**>(&oTagForFrame));
    bool hb = b && Hook(b, reinterpret_cast<void*>(&TagHook), reinterpret_cast<void**>(&oTag));
    // DLSS-G's options and state: the game fetched them at its start (slGetFeatureFunction), so the functions
    // themselves get hooked
    using GetFeatureFunction_t = sl::Result (*)(sl::Feature, const char*, void*&);
    auto gff = reinterpret_cast<GetFeatureFunction_t>(GetProcAddress(sl, "slGetFeatureFunction"));
    void* so = nullptr;
    void* gs = nullptr;
    void* mk = nullptr;
    if (gff) {
        gff(sl::kFeaturePCL, "slPCLSetMarker", mk);
        gff(sl::kFeatureDLSS_G, "slDLSSGSetOptions", so);
        gff(sl::kFeatureDLSS_G, "slDLSSGGetState", gs);
    }
    bool hc = so && Hook(so, reinterpret_cast<void*>(&SetOptionsHook), reinterpret_cast<void**>(&oSetOptions));
    bool hd = gs && Hook(gs, reinterpret_cast<void*>(&GetStateHook), reinterpret_cast<void**>(&oGetState));
    bool he = mk && Hook(mk, reinterpret_cast<void*>(&MarkerHook), reinterpret_cast<void**>(&oMarker));
    Log("slui: Streamline watched (slSetTagForFrame %s, slSetTag %s, slDLSSGSetOptions %s, slDLSSGGetState %s, "
        "slPCLSetMarker %s; present hook on thread %lu)", ha ? "hooked" : "-", hb ? "hooked" : "-", hc ? "hooked" : "-",
        hd ? "hooked" : "-", he ? "hooked" : "-", GetCurrentThreadId());
}

// overlaystats: what the game tagged so far, by type
std::string StreamlineTagStats() {
    std::lock_guard<std::mutex> l(g_mx);
    if (!g_installed) return "Streamline: not in the game";
    std::string s = g_seen.empty() ? "Streamline: no tags seen" : "Streamline tags:";
    for (auto& e : g_seen) s += " " + std::string(TypeName(e.first)) + "(" + std::to_string(e.first) + ") x" + std::to_string(e.second);
    char fg[200];
    snprintf(fg, sizeof fg, " | frame generation: mode %d, %u generated (max %u), UI recomposition %u, status 0x%x, "
             "%llu presented over %llu state calls", g_fgMode == 0xffffffff ? -1 : static_cast<int>(g_fgMode), g_fgFrames,
             g_fgMax, g_fgRecompose, g_fgStatus, static_cast<unsigned long long>(g_fgPresented),
             static_cast<unsigned long long>(g_fgStates));
    s += fg;
    s += " | markers:";
    for (auto& e : g_markers) s += " " + std::to_string(e.first) + " x" + std::to_string(e.second);
    return s;
}

}  // namespace tw3se
