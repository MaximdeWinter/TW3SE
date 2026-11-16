// Test of TW3SE's render API (1.2): a root constant buffer appended to the game's main root signature, bound by the
// plugin whenever the game sets one of its pipelines. The game's shaders do not read it: the picture stays the same,
// and the game must run on (no device removed). `rendertest` answers how often it bound.
// Not part of a release; the pattern for a mod like Dynamic Grass (its own shader reads the buffer).
#include "tw3se/tw3se.hpp"
#include <d3d12.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <windows.h>

static const TW3SE_Interface* g_api = nullptr;
static const uint64_t kMainRootSignature = 0xe6a20b1c84a3896bull;  // 245 of 274 pipelines (dump)
static int g_handle = 0;
static ID3D12Resource* g_buffer = nullptr;
static D3D12_GPU_VIRTUAL_ADDRESS g_va = 0;
static std::atomic<int> g_marked{0}, g_binds{0}, g_bad{0};

static void Bind(void* commandList, void*, void* rootSignature, void*, void*) {
    int index = g_api->RootParamIndex(rootSignature, g_handle);
    if (index < 0 || !g_va) {
        ++g_bad;
        return;
    }
    static_cast<ID3D12GraphicsCommandList*>(commandList)->SetGraphicsRootConstantBufferView(static_cast<UINT>(index), g_va);
    ++g_binds;
}

static void OnPipeline(const TW3SE_PipelineInfo* info, void*) {
    if (info->compute || info->rootSignatureId != kMainRootSignature) return;
    if (!g_buffer) {  // one upload buffer of 256 bytes from the game's device
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
            void* p = nullptr;
            if (SUCCEEDED(g_buffer->Map(0, nullptr, &p)) && p) {
                memset(p, 0, 256);
                const float magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};  // what the replaced shaders of "tint" draw
                memcpy(p, magenta, sizeof magenta);
            }
            g_va = g_buffer->GetGPUVirtualAddress();
        }
        dev->Release();
    }
    g_api->MarkPipeline(info->pipelineState, &Bind, nullptr, nullptr);
    ++g_marked;
}

// Once per frame before the game's first submission: one list of the plugin's own on the game's queue (empty here; a
// mod puts its compute pass in it)
static ID3D12CommandAllocator* g_alloc = nullptr;
static ID3D12GraphicsCommandList* g_list = nullptr;
static std::atomic<int> g_submits{0}, g_wrongQueue{0};
static void OnSubmit(void* queue, void*) {
    auto q = static_cast<ID3D12CommandQueue*>(queue);
    if (q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ++g_wrongQueue;
        return;
    }
    if (!g_list) {
        ID3D12Device* dev = nullptr;
        q->GetDevice(IID_PPV_ARGS(&dev));
        if (!dev) return;
        if (SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc))))
            dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_list));
        dev->Release();
        if (!g_list) return;
        g_list->Close();
    }
    ID3D12CommandList* lists[] = {g_list};
    q->ExecuteCommandLists(1, lists);
    ++g_submits;
}

static void Cmd(const char*, char* reply, uint32_t size) {
    snprintf(reply, size, "rendertest: handle %d, %d pipeline(s) marked, %d bind(s), %d without the parameter, buffer %s, "
             "%d frame(s) submitted, %d on another queue type",
             g_handle, g_marked.load(), g_binds.load(), g_bad.load(), g_va ? "made" : "missing", g_submits.load(),
             g_wrongQueue.load());
}

TW3SE_PLUGIN("RenderTest", "TW3SE tests", 0x00010000, /*minInterface*/ 21)

// tw3se\rendertest_mode.txt: "full" (default), "nomark" (the parameter, no pipeline marked), "noext" (no parameter,
// pipelines marked: the callback never runs), "off" (nothing) - to find which part a problem comes from
static char g_mode[16] = "full";
static void ReadMode() {
    char path[MAX_PATH];
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&ReadMode), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    *slash = 0;  // tw3se\plugins
    slash = strrchr(path, '\\');
    if (!slash) return;
    strcpy_s(slash, MAX_PATH - (slash - path), "\\rendertest_mode.txt");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") == 0 && f) {
        if (fscanf_s(f, "%15s", g_mode, static_cast<unsigned>(sizeof g_mode)) != 1) strcpy_s(g_mode, "full");
        fclose(f);
    }
}

TW3SE_EXPORT int TW3SE_PluginLoad(const TW3SE_Interface* api) {
    g_api = api;
    tw3se::init(api);
    ReadMode();
    api->Log("RenderTest", "mode %s", g_mode);
    tw3se::register_command("rendertest", &Cmd, "rendertest: the render API test's counts");
    if (!strcmp(g_mode, "off")) return 1;
    if (!strcmp(g_mode, "nomark")) {
        D3D12_ROOT_PARAMETER1 q = {};
        q.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        q.Descriptor.ShaderRegister = 13;
        q.Descriptor.RegisterSpace = 7;
        q.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        g_handle = api->RootSignatureAddParam(kMainRootSignature, &q);
        return 1;
    }
    if (!strcmp(g_mode, "noext")) {
        api->RegisterOnPipeline(&OnPipeline, nullptr);
        return 1;
    }
    D3D12_ROOT_PARAMETER1 p = {};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p.Descriptor.ShaderRegister = 13;
    p.Descriptor.RegisterSpace = 7;
    p.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    g_handle = api->RootSignatureAddParam(kMainRootSignature, &p);
    api->RegisterOnPipeline(&OnPipeline, nullptr);
    api->RegisterOnSubmit(&OnSubmit, nullptr);
    if (!strcmp(g_mode, "tint")) {  // tw3se\rendertest\<shader id>.cso: replacements that draw the buffer's colour
        char dir[MAX_PATH];
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&ReadMode), &self);
        GetModuleFileNameA(self, dir, MAX_PATH);
        *strrchr(dir, '\\') = 0;
        *strrchr(dir, '\\') = 0;
        strcat_s(dir, "\\rendertest\\");
        char pattern[MAX_PATH];
        snprintf(pattern, sizeof pattern, "%s*.cso", dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        int n = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char path[MAX_PATH];
                snprintf(path, sizeof path, "%s%s", dir, fd.cFileName);
                FILE* f = nullptr;
                if (fopen_s(&f, path, "rb") != 0 || !f) continue;
                static char buf[1 << 20];
                size_t len = fread(buf, 1, sizeof buf, f);
                fclose(f);
                uint64_t id = _strtoui64(fd.cFileName, nullptr, 16);
                if (len && id && api->ShaderReplace(id, buf, len)) ++n;
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        api->Log("RenderTest", "tint: %d shader(s) replaced from memory", n);
    }
    return 1;
}
