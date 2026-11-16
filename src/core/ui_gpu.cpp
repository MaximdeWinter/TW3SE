// TW3SE core - pictures and Qt's text for the in-game UI on D3D12.
//
// Pictures (w3.ui.image_load) and the text atlas (lua_ui_text.cpp) are made on the main thread as pixels; their id is
// a tag plus a slot of the overlay's SRV heap (overlay.cpp, slots kUiFirstSlot..). The render thread uploads what is
// new inside the overlay's command list, before the UI draws, and puts the slot's GPU handle into the frame's draw
// commands. A heap made anew (a resize) uploads everything again from the kept pixels.
//
// The text shader: ClearType coverage per channel with dual-source blending, gamma-correct towards the background the
// text stands on (as Qt blends it). UiTextCallback switches to it for the draws after it; ImGui's ResetRenderState
// switches back.
#define NOMINMAX
#include "core.h"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "lua_ui.h"

#pragma comment(lib, "d3dcompiler")
#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "ole32")

namespace tw3se {
namespace {

const unsigned long long kTag = 1ull << 62;
const UINT kUiFirstSlot = 64;    // the overlay's slots below are its own and the plugins' (overlay.cpp)
const UINT kUiLastSlot = 2047;   // kSrvSlots - 1 in overlay.cpp

struct Tex {
    std::vector<unsigned char> px;  // RGBA, kept for a new heap
    int w = 0, h = 0;
    ID3D12Resource* res = nullptr;
    ID3D12Resource* upload = nullptr;
    int uploadFrames = 0;
    int gen = -1;      // the heap generation it is uploaded for
    bool dead = false;
};
std::mutex g_mx;
std::map<UINT, Tex> g_tex;  // slot -> texture
UINT g_nextSlot = kUiFirstSlot;
std::vector<ID3D12Resource*> g_graveyard;  // released a few frames after free (the GPU may still read them)
int g_graveFrames = 0;

// render thread
ID3D12RootSignature* g_textRoot = nullptr;
ID3D12PipelineState* g_textPso = nullptr;
ID3D12PipelineState* g_invertPso = nullptr;  // the text pipeline's root signature, out = 1 - dst
ID3D12GraphicsCommandList* g_cl = nullptr;
float g_mvp[16] = {};
D3D12_GPU_DESCRIPTOR_HANDLE g_gpu0 = {};
UINT g_stride = 0;
int g_gen = 0;

UINT NewSlot() {  // under g_mx
    for (UINT n = 0; n <= kUiLastSlot - kUiFirstSlot; ++n) {
        UINT s = g_nextSlot;
        g_nextSlot = g_nextSlot >= kUiLastSlot ? kUiFirstSlot : g_nextSlot + 1;
        if (!g_tex.count(s)) return s;
    }
    return 0;
}

const char* kVs =
    "cbuffer vertexBuffer : register(b0) { float4x4 ProjectionMatrix; };"
    "struct VS_INPUT { float2 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };"
    "struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };"
    "PS_INPUT main(VS_INPUT input) { PS_INPUT o; o.pos = mul(ProjectionMatrix, float4(input.pos.xy, 0.f, 1.f));"
    " o.col = input.col; o.uv = input.uv; return o; }";
const char* kPs =
    "cbuffer P : register(b1) { float4 colour; float4 bg; float4 gamma; };"
    "struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };"
    "SamplerState sampler0 : register(s0); Texture2D texture0 : register(t0);"
    "struct OUT { float4 c0 : SV_Target0; float4 c1 : SV_Target1; };"
    "OUT main(PS_INPUT i) {"
    " float3 sh = texture0.Sample(sampler0, i.uv).rgb; float3 cov = sh * i.col.a; float3 s = i.col.rgb, b = bg.rgb, e = cov;"
    " if (gamma.y > 0.5) { float g = gamma.y > 1.5 ? 1.0 / gamma.x : gamma.x;"
    "  float3 sa = s * i.col.a + b * (1 - i.col.a);"
    "  float3 r = pow(pow(sa, g) * sh + pow(b, g) * (1 - sh), 1.0 / g); float3 d = s - b;"
    "  e = float3(abs(d.x) > 1e-4 ? saturate((r.x - b.x) / d.x) : cov.x, abs(d.y) > 1e-4 ? saturate((r.y - b.y) / d.y) : cov.y,"
    "             abs(d.z) > 1e-4 ? saturate((r.z - b.z) / d.z) : cov.z); }"
    " OUT o; o.c0 = float4(s, 1); o.c1 = float4(e, 1); return o; }";

bool MakeTextPipeline(ID3D12Device* dev, DXGI_FORMAT fmt) {
    if (g_textPso) return true;
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER p[3] = {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // as ImGui's: the projection for the VS
    p[0].Constants.Num32BitValues = 16;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;  // as ImGui's: the texture (set per draw)
    p[1].DescriptorTable.NumDescriptorRanges = 1;
    p[1].DescriptorTable.pDescriptorRanges = &range;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;  // colour, background, gamma
    p[2].Constants.ShaderRegister = 1;
    p[2].Constants.Num32BitValues = 12;
    p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC smp = {};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rd = {};
    rd.NumParameters = 3;
    rd.pParameters = p;
    rd.NumStaticSamplers = 1;
    rd.pStaticSamplers = &smp;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* sig = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, nullptr))) return false;
    HRESULT hr = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_textRoot));
    sig->Release();
    if (FAILED(hr)) return false;
    ID3DBlob *vs = nullptr, *ps = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(kVs, strlen(kVs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs, nullptr)) ||
        FAILED(D3DCompile(kPs, strlen(kPs), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps, &err))) {
        Log("ui: text shader does not compile: %s", err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        if (vs) vs->Release();
        if (err) err->Release();
        return false;
    }
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(ImDrawVert, pos), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, (UINT)offsetof(ImDrawVert, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, (UINT)offsetof(ImDrawVert, col), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_textRoot;
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.InputLayout = {layout, 3};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = fmt;
    pd.SampleDesc.Count = 1;
    pd.SampleMask = UINT_MAX;
    D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D12_BLEND_SRC1_COLOR;
    b.DestBlend = D3D12_BLEND_INV_SRC1_COLOR;
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ONE;
    b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE;
    hr = dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_textPso));
    b.SrcBlend = D3D12_BLEND_INV_DEST_COLOR;  // the invert pipeline: white times (1 - dst), nothing of dst
    b.DestBlend = D3D12_BLEND_ZERO;
    b.SrcBlendAlpha = D3D12_BLEND_ZERO;
    b.DestBlendAlpha = D3D12_BLEND_ONE;
    if (SUCCEEDED(hr)) dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_invertPso));
    vs->Release();
    ps->Release();
    if (FAILED(hr)) Log("ui: text pipeline failed (0x%08lx)", static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

void TextCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    if (!g_cl || !g_textPso || !cmd->UserCallbackData || cmd->UserCallbackDataSize < 48) return;
    g_cl->SetGraphicsRootSignature(g_textRoot);
    g_cl->SetPipelineState(g_textPso);
    g_cl->SetGraphicsRoot32BitConstants(0, 16, g_mvp, 0);
    g_cl->SetGraphicsRoot32BitConstants(2, 12, cmd->UserCallbackData, 0);
}

void InvertCallback(const ImDrawList*, const ImDrawCmd*) {
    if (!g_cl || !g_invertPso) return;
    static const float params[12] = {1, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0};  // white, no background: the coverage as is
    g_cl->SetGraphicsRootSignature(g_textRoot);
    g_cl->SetPipelineState(g_invertPso);
    g_cl->SetGraphicsRoot32BitConstants(0, 16, g_mvp, 0);
    g_cl->SetGraphicsRoot32BitConstants(2, 12, params, 0);
}

bool Upload(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE cpu0, UINT slot, Tex& t) {
    if (t.res) g_graveyard.push_back(t.res), t.res = nullptr;
    if (t.upload) g_graveyard.push_back(t.upload), t.upload = nullptr;
    D3D12_HEAP_PROPERTIES def = {};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = t.w;
    td.Height = t.h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    if (FAILED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&t.res))))
        return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    D3D12_HEAP_PROPERTIES up = {};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&t.upload))))
        return false;
    unsigned char* map = nullptr;
    D3D12_RANGE none = {0, 0};
    if (FAILED(t.upload->Map(0, &none, reinterpret_cast<void**>(&map)))) return false;
    for (int y = 0; y < t.h; ++y)
        memcpy(map + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch, t.px.data() + static_cast<size_t>(y) * t.w * 4,
               static_cast<size_t>(t.w) * 4);
    t.upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = t.res;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = t.upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = t.res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cl->ResourceBarrier(1, &b);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE h = cpu0;
    h.ptr += static_cast<SIZE_T>(slot) * g_stride;
    dev->CreateShaderResourceView(t.res, &sv, h);
    t.uploadFrames = 4;
    t.gen = g_gen;
    return true;
}

}  // namespace

// --- main thread ---
unsigned long long UiCreateTexture(int w, int h, const void* rgba) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || !rgba) return 0;
    std::lock_guard<std::mutex> lk(g_mx);
    UINT slot = NewSlot();
    if (!slot) {
        Log("ui: no free picture slot (%u in use)", kUiLastSlot - kUiFirstSlot + 1);
        return 0;
    }
    Tex& t = g_tex[slot];
    t.w = w;
    t.h = h;
    t.px.assign(static_cast<const unsigned char*>(rgba), static_cast<const unsigned char*>(rgba) + static_cast<size_t>(w) * h * 4);
    return kTag | slot;
}

unsigned long long UiLoadImage(const char* path, int* w, int* h) {
    *w = *h = 0;
    static IWICImagingFactory* wic = nullptr;
    if (!wic) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // the game's main thread may have COM already (then this fails, and that is fine)
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return 0;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
    std::wstring wp(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path, -1, wp.data(), n);
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* conv = nullptr;
    unsigned long long id = 0;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(wp.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        UINT iw = 0, ih = 0;
        conv->GetSize(&iw, &ih);
        std::vector<unsigned char> px(static_cast<size_t>(iw) * ih * 4);
        if (iw && ih && SUCCEEDED(conv->CopyPixels(nullptr, iw * 4, static_cast<UINT>(px.size()), px.data()))) {
            id = UiCreateTexture(static_cast<int>(iw), static_cast<int>(ih), px.data());
            if (id) *w = static_cast<int>(iw), *h = static_cast<int>(ih);
        }
    }
    if (conv) conv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    return id;
}

void UiFreeImage(unsigned long long id) {
    if ((id & kTag) == 0) return;
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_tex.find(static_cast<UINT>(id & 0xFFFF));
    if (it != g_tex.end()) it->second.dead = true;
}

ImDrawCallback UiTextCallback() { return &TextCallback; }
ImDrawCallback UiInvertCallback() { return &InvertCallback; }

// Copies a draw list for the render thread with its callbacks' data (ImDrawList::CloneOutput leaves that out: the
// data would point into the main thread's list, which the next frame overwrites).
ImDrawList* UiCloneDrawList(const ImDrawList* l) {
    ImDrawList* c = l->CloneOutput();
    c->_CallbacksDataBuf = l->_CallbacksDataBuf;
    for (ImDrawCmd& cmd : c->CmdBuffer)
        if (cmd.UserCallback && cmd.UserCallbackDataOffset != -1 && cmd.UserCallbackDataSize > 0)
            cmd.UserCallbackData = c->_CallbacksDataBuf.Data + cmd.UserCallbackDataOffset;
    return c;
}

// --- render thread (ui.cpp's UiRender, inside the overlay's command list) ---
// Uploads what is new, frees what went, and puts the GPU handles into the frame's draw commands.
void UiGpuPrepare(ID3D12Device* dev, ID3D12DescriptorHeap* heap, ID3D12GraphicsCommandList* cl, DXGI_FORMAT fmt, int gen,
                  ImDrawData* dd) {
    g_cl = cl;
    if (gen != g_gen) g_gen = gen;
    g_stride = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_gpu0 = heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = heap->GetCPUDescriptorHandleForHeapStart();
    MakeTextPipeline(dev, fmt);
    if (g_graveFrames > 0 && --g_graveFrames == 0) {
        for (ID3D12Resource* r : g_graveyard) r->Release();
        g_graveyard.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (auto it = g_tex.begin(); it != g_tex.end();) {
            Tex& t = it->second;
            if (t.dead) {
                if (t.res) g_graveyard.push_back(t.res);
                if (t.upload) g_graveyard.push_back(t.upload);
                g_graveFrames = 4;
                it = g_tex.erase(it);
                continue;
            }
            if (t.gen != g_gen) Upload(dev, cl, cpu0, it->first, t);
            else if (t.upload && t.uploadFrames > 0 && --t.uploadFrames == 0) g_graveyard.push_back(t.upload), t.upload = nullptr, g_graveFrames = 4;
            ++it;
        }
        if (dd)
            for (ImDrawList* l : dd->CmdLists)
                for (ImDrawCmd& c : l->CmdBuffer) {
                    unsigned long long id = static_cast<unsigned long long>(c.TextureId);
                    if ((id & kTag) == 0) continue;
                    auto it = g_tex.find(static_cast<UINT>(id & 0xFFFF));
                    if (it == g_tex.end() || it->second.gen != g_gen) {
                        c.ElemCount = 0;  // not there (yet): drawn on a later frame
                        continue;
                    }
                    c.TextureId = static_cast<ImTextureID>(g_gpu0.ptr + static_cast<UINT64>(it->first) * g_stride);
                }
    }
    if (dd) {  // ImGui's projection, for the text shader's vertex stage
        float L = dd->DisplayPos.x, R = dd->DisplayPos.x + dd->DisplaySize.x, T = dd->DisplayPos.y,
              B = dd->DisplayPos.y + dd->DisplaySize.y;
        float m[16] = {2.0f / (R - L), 0, 0, 0, 0, 2.0f / (T - B), 0, 0, 0, 0, 0.5f, 0, (R + L) / (L - R), (T + B) / (B - T), 0.5f, 1.0f};
        memcpy(g_mvp, m, sizeof m);
    }
}

// The overlay's device objects go (a resize): the pipeline with them; the pictures upload again into the new heap.
void UiGpuShutdown() {
    if (g_textPso) g_textPso->Release(), g_textPso = nullptr;
    if (g_invertPso) g_invertPso->Release(), g_invertPso = nullptr;
    if (g_textRoot) g_textRoot->Release(), g_textRoot = nullptr;
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto& [slot, t] : g_tex) {
        if (t.res) g_graveyard.push_back(t.res), t.res = nullptr;
        if (t.upload) g_graveyard.push_back(t.upload), t.upload = nullptr;
        t.gen = -1;
    }
    for (ID3D12Resource* r : g_graveyard) r->Release();  // the overlay waited for its GPU work before this
    g_graveyard.clear();
    g_cl = nullptr;
}

}  // namespace tw3se
