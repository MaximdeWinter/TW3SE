// Test of the skinning shader hook (tools/skin_build.py ... offset): the patched skinning shaders read an
// offset from b13, space7 and add it to the skinned position. This plugin appends that constant buffer to the game's
// main root signature, binds it on every pipeline of that signature and writes the offset once per frame:
// 30 cm up (model space), so every skinned figure floats. The file skinhook_test.txt next to the DLL (three numbers)
// sets another offset.
#include "tw3se/tw3se.hpp"
#include <d3d12.h>
#include <cstdio>
#include <cstring>

static const TW3SE_Interface* g_api = nullptr;
static const uint64_t kMainRootSignature = 0xe6a20b1c84a3896bull;  // 5.01 and 5.01b
static int g_param = 0;
static ID3D12Resource* g_buffer = nullptr;
static float* g_data = nullptr;
static float g_offset[4] = {0.0f, 0.0f, 0.3f, 0.0f};

static void Bind(void* list, void*, void* rootSignature, void*, void*) {
    int i = g_api->RootParamIndex(rootSignature, g_param);
    if (i >= 0 && g_buffer)
        static_cast<ID3D12GraphicsCommandList*>(list)->SetGraphicsRootConstantBufferView(
            static_cast<UINT>(i), g_buffer->GetGPUVirtualAddress());
}

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
                                                   nullptr, IID_PPV_ARGS(&g_buffer)))) {
            g_buffer->Map(0, nullptr, reinterpret_cast<void**>(&g_data));
            memcpy(g_data, g_offset, sizeof g_offset);
        }
        dev->Release();
    }
    g_api->MarkPipeline(info->pipelineState, &Bind, nullptr, nullptr);
}

static void OnSubmit(void*, void*) {
    if (g_data) memcpy(g_data, g_offset, sizeof g_offset);
}

TW3SE_PLUGIN("SkinHookTest", "TW3SE tests", 0x00010000, /*minInterface*/ 21)

TW3SE_EXPORT int TW3SE_PluginLoad(const TW3SE_Interface* api) {
    g_api = api;
    tw3se::init(api);
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&OnSubmit), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) {
        strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "skinhook_test.txt");
        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") == 0 && f) {
            fscanf_s(f, "%f %f %f", &g_offset[0], &g_offset[1], &g_offset[2]);
            fclose(f);
        }
    }
    D3D12_ROOT_PARAMETER1 p = {};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p.Descriptor.ShaderRegister = 13;
    p.Descriptor.RegisterSpace = 7;
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    g_param = api->RootSignatureAddParam(kMainRootSignature, &p);
    api->RegisterOnPipeline(&OnPipeline, nullptr);
    api->RegisterOnSubmit(&OnSubmit, nullptr);
    api->Log("SkinHookTest", "offset %.3f %.3f %.3f, root parameter %d", g_offset[0], g_offset[1], g_offset[2], g_param);
    return 1;
}
