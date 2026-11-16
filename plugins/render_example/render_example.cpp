// TW3SE plugin example: data of your own in the game's shaders (TW3SE 1.2, interface 21), the kind of thing a ReShade
// add-on does. It appends a root constant buffer (b13, space7) to the game's main root signature, binds its buffer
// whenever the game sets one of the pipelines it marked, and writes the buffer once per frame.
//
// A shader of yours reads it: declare `cbuffer MyData : register(b13, space7) { float4 myValue; };` and give it to
// TW3SE with ShaderReplace (or as tw3se\shaders\<id>.cso). shader_dump = on in tw3se.config writes the game's shaders
// and dump\pipelines.txt, which names each pipeline's shaders and root signature (rs=...).
#include "tw3se/tw3se.hpp"
#include <d3d12.h>
#include <cstring>

static const TW3SE_Interface* g_api = nullptr;
static const uint64_t kMainRootSignature = 0xe6a20b1c84a3896bull;  // the game's main one (patch 5.01)
static int g_param = 0;                                            // our parameter's handle
static ID3D12Resource* g_buffer = nullptr;
static float* g_data = nullptr;                                    // the buffer, mapped (upload heap)

// The game set a marked pipeline on a command list: bind our buffer at our index of its root signature.
static void Bind(void* list, void*, void* rootSignature, void*, void*) {
    int i = g_api->RootParamIndex(rootSignature, g_param);
    if (i >= 0 && g_buffer)
        static_cast<ID3D12GraphicsCommandList*>(list)->SetGraphicsRootConstantBufferView(
            static_cast<UINT>(i), g_buffer->GetGPUVirtualAddress());
}

// Every pipeline the game creates: mark the ones on the main root signature (pick yours by info->ps / info->vs).
static void OnPipeline(const TW3SE_PipelineInfo* info, void*) {
    if (info->compute || info->rootSignatureId != kMainRootSignature) return;
    if (!g_buffer) {
        ID3D12Device* dev = nullptr;
        static_cast<ID3D12PipelineState*>(info->pipelineState)->GetDevice(IID_PPV_ARGS(&dev));
        if (!dev) return;
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 256;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&g_buffer))))
            g_buffer->Map(0, nullptr, reinterpret_cast<void**>(&g_data));
        dev->Release();
    }
    g_api->MarkPipeline(info->pipelineState, &Bind, nullptr, nullptr);
}

// Once per frame, before the game draws: new values into the buffer (a compute pass of yours would go here too,
// with ExecuteCommandLists on this queue). One buffer is enough for values that change slowly; for values per
// frame keep one per frame in flight.
static void OnSubmit(void*, void*) {
    if (!g_data) return;
    static float t = 0.0f;
    t += 1.0f / 60.0f;
    const float value[4] = {t, 0.0f, 0.0f, 1.0f};
    memcpy(g_data, value, sizeof value);
}

TW3SE_PLUGIN("RenderExample", "YourName", 0x00010000, /*minInterface*/ 21)

TW3SE_EXPORT int TW3SE_PluginLoad(const TW3SE_Interface* api) {
    g_api = api;
    tw3se::init(api);
    D3D12_ROOT_PARAMETER1 p = {};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p.Descriptor.ShaderRegister = 13;
    p.Descriptor.RegisterSpace = 7;
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    g_param = api->RootSignatureAddParam(kMainRootSignature, &p);  // before the game creates the signature
    api->RegisterOnPipeline(&OnPipeline, nullptr);
    api->RegisterOnSubmit(&OnSubmit, nullptr);
    return 1;
}
