// Shaders of the game: dump and replace.
//
// Every pipeline the game creates passes its shaders' bytecode to the D3D12 device (CreateGraphicsPipelineState,
// CreateComputePipelineState, CreatePipelineState with a stream). TW3SE hooks those slots of the runtime's device
// vtable (read from a throwaway device, the same vtable the game's device uses) and gives each shader an id: the
// FNV-1a 64 of its bytes, as 16 hex digits. A file tw3se\shaders\<id>.cso replaces that shader in every pipeline that
// uses it (compiled DXIL, signed by the validator, for the same root signature). With `shader_dump = on` in
// tw3se.config every shader goes once to tw3se\shaders\dump\<id>.<stage>.cso, to find the one to replace.
//
// Off unless tw3se\shaders exists or shader_dump is on: the hooks cost nothing then.
#include "core.h"

#include <d3d12.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "d3d12.lib")

namespace tw3se {
namespace {

using CreateGfx_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateCompute_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateStream_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using CreateLibrary_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const void*, SIZE_T, REFIID, void**);
CreateGfx_t oGfx = nullptr;
CreateCompute_t oCompute = nullptr;
CreateStream_t oStream = nullptr;
CreateLibrary_t oLibrary = nullptr;

std::mutex g_mx;
bool g_dump = false;
std::wstring g_dir, g_dumpDir;
std::unordered_set<uint64_t> g_dumped;
std::unordered_map<uint64_t, std::vector<uint8_t>> g_replacements;  // id -> bytecode (empty: looked, no file)
std::unordered_map<uint64_t, std::vector<uint8_t>> g_memReplacements;  // id -> bytecode from a plugin (ShaderReplace)
std::atomic<uint64_t> g_pipelines{0}, g_streams{0}, g_libraries{0}, g_replaced{0}, g_shaders{0};

uint64_t Fnv64(const void* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

std::wstring Hex(uint64_t id) {
    wchar_t b[20];
    swprintf_s(b, L"%016llx", static_cast<unsigned long long>(id));
    return b;
}

void WriteFileBytes(const std::wstring& path, const void* data, size_t n) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;  // (written by an earlier run: kept)
    DWORD w = 0;
    WriteFile(h, data, static_cast<DWORD>(n), &w, nullptr);
    CloseHandle(h);
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD r = 0;
    bool ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &r, nullptr) && r == out.size());
    CloseHandle(h);
    return ok && !out.empty();
}

// One shader of a pipeline: counted, dumped once, and swapped when a replacement file exists.
void Visit(D3D12_SHADER_BYTECODE& bc, const wchar_t* stage) {
    if (!bc.pShaderBytecode || !bc.BytecodeLength) return;
    uint64_t id = Fnv64(bc.pShaderBytecode, bc.BytecodeLength);
    g_shaders.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> l(g_mx);
    if (g_dump && g_dumped.insert(id).second)
        WriteFileBytes(g_dumpDir + Hex(id) + L"." + stage + L".cso", bc.pShaderBytecode, bc.BytecodeLength);
    auto mem = g_memReplacements.find(id);  // a plugin's replacement (ShaderReplace) comes first
    if (mem != g_memReplacements.end()) {
        bc.pShaderBytecode = mem->second.data();
        bc.BytecodeLength = mem->second.size();
        g_replaced.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto it = g_replacements.find(id);
    if (it == g_replacements.end()) {
        std::vector<uint8_t> bytes;
        if (!ReadFileBytes(g_dir + Hex(id) + L".cso", bytes)) bytes.clear();
        else Log("shaders: %016llx (%ls) replaced from tw3se\\shaders", static_cast<unsigned long long>(id), stage);
        it = g_replacements.emplace(id, std::move(bytes)).first;
    }
    if (!it->second.empty()) {
        bc.pShaderBytecode = it->second.data();
        bc.BytecodeLength = it->second.size();
        g_replaced.fetch_add(1, std::memory_order_relaxed);
    }
}

std::unordered_set<std::string> g_pairs;  // the shader combinations written to dump\pipelines.txt

// Root signatures (dump mode): the game makes them from serialized blobs (no shader carries one); each gets an id
// like a shader, dump\rs_<id>.bin, and pipelines.txt names the one a pipeline uses.
using CreateRootSig_t = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
CreateRootSig_t oRootSig = nullptr;
std::unordered_map<void*, uint64_t> g_rootSigIds;

// Root parameters plugins append to a root signature of the game (RootSignatureAddParam, 1.2): the game creates the
// signature from its blob; TW3SE appends the parameters and creates the extended one in its place. Every pipeline and
// every SetGraphicsRootSignature of the game then uses the extended signature (a superset: the game's parameters keep
// their indices, its shaders ignore the new ones).
struct AddedParam {
    uint64_t rsId;
    D3D12_ROOT_PARAMETER1 p;
    std::vector<D3D12_DESCRIPTOR_RANGE1> ranges;  // a table's ranges, kept here
};
std::vector<AddedParam> g_added;                                          // the handle is the index + 1
std::unordered_map<void*, std::vector<std::pair<int, UINT>>> g_extended;  // extended signature -> (handle, index)

UINT CostOf(const D3D12_ROOT_PARAMETER1& p) {
    return p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE   ? 1
           : p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ? p.Constants.Num32BitValues
                                                                          : 2;
}

// Under g_mx. Null when nothing was added (or the signature could not be extended).
ID3DBlob* Extend(const void* blob, SIZE_T n, uint64_t id, std::vector<std::pair<int, UINT>>& placed) {
    ID3D12VersionedRootSignatureDeserializer* des = nullptr;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(blob, n, IID_PPV_ARGS(&des))) || !des) return nullptr;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
    ID3DBlob* out = nullptr;
    if (SUCCEEDED(des->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd)) && vd) {
        const D3D12_ROOT_SIGNATURE_DESC1& d = vd->Desc_1_1;
        std::vector<D3D12_ROOT_PARAMETER1> params(d.pParameters, d.pParameters + d.NumParameters);
        UINT cost = 0;
        for (const auto& p : params) cost += CostOf(p);
        for (size_t h = 0; h < g_added.size(); ++h) {
            if (g_added[h].rsId != id) continue;
            D3D12_ROOT_PARAMETER1 p = g_added[h].p;
            if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                p.DescriptorTable.NumDescriptorRanges = static_cast<UINT>(g_added[h].ranges.size());
                p.DescriptorTable.pDescriptorRanges = g_added[h].ranges.data();
            }
            if (cost + CostOf(p) > 64) {
                Log("render: root signature %016llx is full (%u of 64 DWORDs) - a plugin's parameter left out",
                    static_cast<unsigned long long>(id), cost);
                continue;
            }
            cost += CostOf(p);
            placed.push_back({static_cast<int>(h + 1), static_cast<UINT>(params.size())});
            params.push_back(p);
        }
        if (!placed.empty()) {
            D3D12_VERSIONED_ROOT_SIGNATURE_DESC nd = {};
            nd.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
            nd.Desc_1_1 = d;
            nd.Desc_1_1.NumParameters = static_cast<UINT>(params.size());
            nd.Desc_1_1.pParameters = params.data();
            ID3DBlob* err = nullptr;
            if (FAILED(D3D12SerializeVersionedRootSignature(&nd, &out, &err))) {
                Log("render: root signature %016llx could not be extended: %s", static_cast<unsigned long long>(id),
                    err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                out = nullptr;
                placed.clear();
            }
            if (err) err->Release();
        }
    }
    des->Release();
    return out;
}

HRESULT STDMETHODCALLTYPE RootSigHook(ID3D12Device* dev, UINT node, const void* blob, SIZE_T n, REFIID riid, void** out) {
    uint64_t id = blob && n ? Fnv64(blob, n) : 0;
    ID3DBlob* ext = nullptr;
    std::vector<std::pair<int, UINT>> placed;
    {
        std::lock_guard<std::mutex> l(g_mx);
        bool wanted = false;
        for (const auto& a : g_added) wanted = wanted || a.rsId == id;
        if (wanted) ext = Extend(blob, n, id, placed);
    }
    HRESULT hr = ext ? oRootSig(dev, node, ext->GetBufferPointer(), ext->GetBufferSize(), riid, out) : E_FAIL;
    if (ext && FAILED(hr)) {
        Log("render: the extended root signature %016llx was refused (0x%08lx) - the game's own",
            static_cast<unsigned long long>(id), hr);
        placed.clear();
    }
    if (ext) ext->Release();
    if (FAILED(hr)) hr = oRootSig(dev, node, blob, n, riid, out);
    if (SUCCEEDED(hr) && out && *out && blob && n) {
        std::lock_guard<std::mutex> l(g_mx);
        g_rootSigIds[*out] = id;
        if (!placed.empty()) {
            g_extended[*out] = placed;
            Log("render: root signature %016llx extended by %zu parameter(s) of plugins",
                static_cast<unsigned long long>(id), placed.size());
        }
        if (g_dump && g_dumped.insert(id).second) WriteFileBytes(g_dumpDir + L"rs_" + Hex(id) + L".bin", blob, n);
    }
    return hr;
}

// Plugins told of every pipeline the game creates (RegisterOnPipeline, 1.2)
struct PipelineListener {
    TW3SE_PipelineCallback cb;
    void* ctx;
};
std::vector<PipelineListener> g_pipeListeners;

bool CallPipeGuarded(TW3SE_PipelineCallback cb, const TW3SE_PipelineInfo* info, void* ctx) {
    __try {
        cb(info, ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t IdOf(const D3D12_SHADER_BYTECODE& bc) {
    return bc.pShaderBytecode && bc.BytecodeLength ? Fnv64(bc.pShaderBytecode, bc.BytecodeLength) : 0;
}

void TellPipeline(TW3SE_PipelineInfo& info, void* rs) {
    std::vector<PipelineListener> list;
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (g_pipeListeners.empty()) return;
        list = g_pipeListeners;
        auto it = g_rootSigIds.find(rs);
        info.rootSignatureId = it != g_rootSigIds.end() ? it->second : 0;
    }
    info.rootSignature = rs;
    for (const auto& p : list)
        if (!CallPipeGuarded(p.cb, &info, p.ctx)) Log("render: a plugin's pipeline callback faulted");
}

// In dump mode: which shaders make one pipeline (a pixel shader that goes with a skinning vertex shader ...).
void NotePipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
    if (!g_dump) return;
    std::string line;
    auto add = [&](const char* stage, const D3D12_SHADER_BYTECODE& bc) {
        if (!bc.pShaderBytecode || !bc.BytecodeLength) return;
        char b[40];
        snprintf(b, sizeof b, "%s%s=%016llx", line.empty() ? "" : " ", stage,
                 static_cast<unsigned long long>(Fnv64(bc.pShaderBytecode, bc.BytecodeLength)));
        line += b;
    };
    add("vs", d.VS);
    add("ps", d.PS);
    add("ds", d.DS);
    add("hs", d.HS);
    add("gs", d.GS);
    std::lock_guard<std::mutex> l(g_mx);
    auto rs = g_rootSigIds.find(d.pRootSignature);
    if (rs != g_rootSigIds.end()) {
        char b[40];
        snprintf(b, sizeof b, " rs=%016llx", static_cast<unsigned long long>(rs->second));
        line += b;
    }
    if (d.InputLayout.NumElements && d.InputLayout.pInputElementDescs) {  // il=SEMANTIC<index>:format:slot:offset,...
        line += " il=";
        for (UINT i = 0; i < d.InputLayout.NumElements && i < 24; ++i) {
            const D3D12_INPUT_ELEMENT_DESC& e = d.InputLayout.pInputElementDescs[i];
            char b[80];
            snprintf(b, sizeof b, "%s%s%u:%d:%u:%u", i ? "," : "", e.SemanticName ? e.SemanticName : "?", e.SemanticIndex,
                     e.Format, e.InputSlot, e.AlignedByteOffset);
            line += b;
        }
    }
    if (d.StreamOutput.NumEntries) {  // so=<entries>:<strides>
        char b[48];
        snprintf(b, sizeof b, " so=%u:%u", d.StreamOutput.NumEntries,
                 d.StreamOutput.NumStrides && d.StreamOutput.pBufferStrides ? d.StreamOutput.pBufferStrides[0] : 0);
        line += b;
    }
    if (!g_pairs.insert(line).second) return;
    line += "\r\n";
    HANDLE h = CreateFileW((g_dumpDir + L"pipelines.txt").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &w, nullptr);
    CloseHandle(h);
}

// In dump mode: a compute pipeline's shader and root signature (the skinning for ray tracing is compute)
void NoteCompute(const D3D12_COMPUTE_PIPELINE_STATE_DESC& d) {
    if (!g_dump || !d.CS.pShaderBytecode || !d.CS.BytecodeLength) return;
    char b[80];
    snprintf(b, sizeof b, "cs=%016llx", static_cast<unsigned long long>(Fnv64(d.CS.pShaderBytecode, d.CS.BytecodeLength)));
    std::string line = b;
    std::lock_guard<std::mutex> l(g_mx);
    auto rs = g_rootSigIds.find(d.pRootSignature);
    if (rs != g_rootSigIds.end()) {
        snprintf(b, sizeof b, " rs=%016llx", static_cast<unsigned long long>(rs->second));
        line += b;
    }
    if (!g_pairs.insert(line).second) return;
    line += "\r\n";
    HANDLE h = CreateFileW((g_dumpDir + L"pipelines.txt").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &w, nullptr);
    CloseHandle(h);
}

HRESULT STDMETHODCALLTYPE GfxHook(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    g_pipelines.fetch_add(1, std::memory_order_relaxed);
    if (!desc) return oGfx(dev, desc, riid, out);
    NotePipeline(*desc);  // (the game's own ids, before a replacement)
    if (desc->VS.pShaderBytecode && desc->VS.BytecodeLength)  // the skinning hook: a skinning shader it has no patch for
        SkinHookNoteVs(Fnv64(desc->VS.pShaderBytecode, desc->VS.BytecodeLength), desc->VS.pShaderBytecode,
                       desc->VS.BytecodeLength);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *desc;
    uint64_t before = g_replaced.load();
    Visit(d.VS, L"vs");
    Visit(d.PS, L"ps");
    Visit(d.DS, L"ds");
    Visit(d.HS, L"hs");
    Visit(d.GS, L"gs");
    HRESULT hr = oGfx(dev, &d, riid, out);
    if (FAILED(hr) && g_replaced.load() != before) {  // a replacement the device refused: the game's own shaders
        Log("shaders: a pipeline with a replaced shader was refused (0x%08lx) - it keeps the game's shaders", hr);
        hr = oGfx(dev, desc, riid, out);
    }
    if (SUCCEEDED(hr) && out && *out) {
        TW3SE_PipelineInfo info = {};
        info.pipelineState = *out;
        info.vs = IdOf(desc->VS);
        info.ps = IdOf(desc->PS);
        info.ds = IdOf(desc->DS);
        info.hs = IdOf(desc->HS);
        info.gs = IdOf(desc->GS);
        TellPipeline(info, desc->pRootSignature);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE ComputeHook(ID3D12Device* dev, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out) {
    g_pipelines.fetch_add(1, std::memory_order_relaxed);
    if (!desc) return oCompute(dev, desc, riid, out);
    NoteCompute(*desc);
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = *desc;
    uint64_t before = g_replaced.load();
    Visit(d.CS, L"cs");
    HRESULT hr = oCompute(dev, &d, riid, out);
    if (FAILED(hr) && g_replaced.load() != before) {
        Log("shaders: a compute pipeline with a replaced shader was refused (0x%08lx) - it keeps the game's", hr);
        hr = oCompute(dev, desc, riid, out);
    }
    if (SUCCEEDED(hr) && out && *out) {
        TW3SE_PipelineInfo info = {};
        info.pipelineState = *out;
        info.cs = IdOf(desc->CS);
        info.compute = 1;
        TellPipeline(info, desc->pRootSignature);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE StreamHook(ID3D12Device* dev, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out) {
    g_streams.fetch_add(1, std::memory_order_relaxed);  // (counted only: is this path used at all?)
    return oStream(dev, desc, riid, out);
}

HRESULT STDMETHODCALLTYPE LibraryHook(ID3D12Device* dev, const void* blob, SIZE_T n, REFIID riid, void** out) {
    g_libraries.fetch_add(1, std::memory_order_relaxed);
    return oLibrary(dev, blob, n, riid, out);
}

void CmdShaders(const char*, char* reply, uint32_t size) {
    char b[300];
    snprintf(b, size < sizeof b ? size : sizeof b,
             "shaders: %llu pipelines (create), %llu by stream, %llu pipeline libraries; %llu shaders seen, %llu "
             "replaced; dump %s",
             static_cast<unsigned long long>(g_pipelines.load()), static_cast<unsigned long long>(g_streams.load()),
             static_cast<unsigned long long>(g_libraries.load()), static_cast<unsigned long long>(g_shaders.load()),
             static_cast<unsigned long long>(g_replaced.load()), g_dump ? "on" : "off");
    strncpy_s(reply, size, b, _TRUNCATE);
}


bool g_hooked = false;

// Under g_mx: the device's pipeline and root signature functions (a throwaway device's vtable is the game's)
bool HookDevice() {
    if (g_hooked) return true;
    ID3D12Device* device = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) || !device) {
        Log("shaders: no D3D12 device for the vtable - off");
        return false;
    }
    void** vt = *reinterpret_cast<void***>(device);
    // ID3D12Device: 10 CreateGraphicsPipelineState, 11 CreateComputePipelineState; ID3D12Device1: 44
    // CreatePipelineLibrary; ID3D12Device2: 47 CreatePipelineState
    bool ok = Hook(vt[10], reinterpret_cast<void*>(&GfxHook), reinterpret_cast<void**>(&oGfx)) &&
              Hook(vt[11], reinterpret_cast<void*>(&ComputeHook), reinterpret_cast<void**>(&oCompute));
    Hook(vt[16], reinterpret_cast<void*>(&RootSigHook), reinterpret_cast<void**>(&oRootSig));  // 16 CreateRootSignature
    ID3D12Device2* dev2 = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dev2))) && dev2) {
        void** vt2 = *reinterpret_cast<void***>(dev2);
        Hook(vt2[44], reinterpret_cast<void*>(&LibraryHook), reinterpret_cast<void**>(&oLibrary));
        Hook(vt2[47], reinterpret_cast<void*>(&StreamHook), reinterpret_cast<void**>(&oStream));
        dev2->Release();
    }
    device->Release();
    RegisterCommand("shaders", &CmdShaders, "shaders: pipelines and shaders seen, replaced (tw3se\\shaders)");
    Log("shaders: %s (dump %s)", ok ? "pipeline creation hooked" : "NOT hooked", g_dump ? "on" : "off");
    g_hooked = ok;
    return ok;
}

}  // namespace

void InstallShaders() {
    g_dir = Dir() + L"\\shaders\\";
    g_dump = ConfigGet("shader_dump", "off") == "on";
    bool folder = GetFileAttributesW(g_dir.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!folder && !g_dump) return;
    if (g_dump) {
        CreateDirectoryW(g_dir.c_str(), nullptr);
        g_dumpDir = g_dir + L"dump\\";
        CreateDirectoryW(g_dumpDir.c_str(), nullptr);
    }
    std::lock_guard<std::mutex> l(g_mx);
    HookDevice();
}

// --- the render API of plugins (render.cpp, 1.2): the same hooks, also without tw3se\shaders or shader_dump ---
bool ShadersEnsureHooks() {
    std::lock_guard<std::mutex> l(g_mx);
    return HookDevice();
}
int ShadersReplaceMemory(uint64_t id, const void* data, size_t size) {
    if (!data || !size || size > (16u << 20)) return 0;  // a shader is far smaller than 16 MB
    std::vector<uint8_t> bytes(size);
    if (!SafeRead(data, bytes.data(), size)) return 0;  // the plugin's memory: a pointer to nothing is refused
    std::lock_guard<std::mutex> l(g_mx);
    g_memReplacements[id] = std::move(bytes);
    return 1;
}
int ShadersAddRootParam(uint64_t rsId, const D3D12_ROOT_PARAMETER1* p) {
    D3D12_ROOT_PARAMETER1 copy;
    if (!p || !SafeRead(p, &copy, sizeof copy) || copy.ParameterType > D3D12_ROOT_PARAMETER_TYPE_UAV) return 0;
    AddedParam a{rsId, copy, {}};
    if (copy.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        // the ranges come from the plugin's memory: a count past 32 or a pointer to nothing is refused
        UINT n = copy.DescriptorTable.NumDescriptorRanges;
        if (n > 32 || (n && !copy.DescriptorTable.pDescriptorRanges)) return 0;
        a.ranges.resize(n);
        if (n && !SafeRead(copy.DescriptorTable.pDescriptorRanges, a.ranges.data(), n * sizeof(a.ranges[0]))) return 0;
    }
    std::lock_guard<std::mutex> l(g_mx);
    g_added.push_back(std::move(a));
    return static_cast<int>(g_added.size());
}
int ShadersRootParamIndex(void* rs, int handle) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_extended.find(rs);
    if (it == g_extended.end()) return -1;
    for (const auto& [h, idx] : it->second)
        if (h == handle) return static_cast<int>(idx);
    return -1;
}
bool ShadersIsExtended(void* rs) {
    std::lock_guard<std::mutex> l(g_mx);
    return g_extended.count(rs) != 0;
}
int ShadersOnPipeline(TW3SE_PipelineCallback cb, void* ctx) {
    if (!cb) return 0;
    std::lock_guard<std::mutex> l(g_mx);
    g_pipeListeners.push_back({cb, ctx});
    return 1;
}
int ShadersDropIn(uintptr_t lo, uintptr_t hi) {
    std::lock_guard<std::mutex> l(g_mx);
    int n = 0;
    for (size_t i = g_pipeListeners.size(); i-- > 0;) {
        auto a = reinterpret_cast<uintptr_t>(g_pipeListeners[i].cb);
        if (a >= lo && a < hi) {
            g_pipeListeners.erase(g_pipeListeners.begin() + static_cast<long>(i));
            ++n;
        }
    }
    return n;
}

}  // namespace tw3se
