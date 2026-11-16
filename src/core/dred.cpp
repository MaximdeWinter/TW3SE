// D3D12 DRED (config `dred = on`, off by default): when the graphics card drops the game's device (a GPU crash, a
// driver reset, "device removed"), Windows keeps what the GPU did last. TW3SE turns it on at the start, before the
// game makes its device, and writes it to tw3se\out\dred.txt when a present fails with the device gone:
//   - the reason the driver gives
//   - each command list the GPU had not finished, its last finished operation and the ones around it (TW3SE's own
//     lists are named "TW3SE ...", so a list of TW3SE among them shows)
//   - the GPU address it faulted on, and which live or just freed allocations lay there
// The breadcrumbs cost a little GPU time; for debugging a crash only.
#include "core.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace tw3se {
namespace {

bool g_on = false;
std::atomic<bool> g_written{false};
// The game's device, with a reference of its own, taken at a present (on the thread that presents, the swap chain
// alive then). The swap chain itself was kept, borrowed; FSR's frame generation made a new one and freed the
// old, the poll thread asked the freed one for its device, faulted, the fault came here again through the vectored
// handler and faulted again, until the stack was gone (3 s after the switch, at tw3se.dll+0x4e1fb).
std::atomic<IDXGISwapChain*> g_sc{nullptr};  // the swap chain the device was taken from (only compared, never called)
std::atomic<ID3D12Device*> g_dev{nullptr};

const char* OpName(D3D12_AUTO_BREADCRUMB_OP op) {
    static const char* names[] = {"SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced",
                                  "ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion", "CopyResource",
                                  "CopyTiles", "ResolveSubresource", "ClearRenderTargetView", "ClearUnorderedAccessView",
                                  "ClearDepthStencilView", "ResourceBarrier", "ExecuteBundle", "Present",
                                  "ResolveQueryData", "BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames",
                                  "AtomicCopyBufferUint", "AtomicCopyBufferUint64", "ResolveSubresourceRegion",
                                  "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession", "DecodeFrame2",
                                  "ProcessFrames1", "BuildRaytracingAccelerationStructure",
                                  "EmitRaytracingAccelerationStructurePostbuildInfo", "CopyRaytracingAccelerationStructure",
                                  "DispatchRays", "InitializeMetaCommand", "ExecuteMetaCommand", "EstimateMotion",
                                  "ResolveMotionVectorHeap", "SetPipelineState1", "InitializeExtensionCommand",
                                  "ExecuteExtensionCommand", "DispatchMesh"};
    return static_cast<unsigned>(op) < sizeof names / sizeof *names ? names[op] : "?";
}

std::string Narrow(const wchar_t* w) {
    if (!w) return {};
    char b[256];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr);
    return n > 0 ? std::string(b) : std::string();
}

std::string AllocList(const D3D12_DRED_ALLOCATION_NODE1* n, int max) {
    std::string r;
    for (int i = 0; n && i < max; n = n->pNext, ++i) {
        std::string name = n->ObjectNameW ? Narrow(n->ObjectNameW) : n->ObjectNameA ? n->ObjectNameA : "";
        char b[200];
        snprintf(b, sizeof b, "    type %d%s%s\r\n", static_cast<int>(n->AllocationType), name.empty() ? "" : ", ", name.c_str());
        r += b;
    }
    return r.empty() ? "    none\r\n" : r;
}

void WriteDred(ID3D12Device* dev, HRESULT presentHr) {
    std::string r = "TW3SE DRED report: the graphics card dropped the game's device.\r\n\r\n";
    char b[512];
    if (FAILED(presentHr))
        snprintf(b, sizeof b, "Seen at a present (0x%08lx). The device's reason: 0x%08lx\r\n", static_cast<unsigned long>(presentHr),
                 static_cast<unsigned long>(dev->GetDeviceRemovedReason()));
    else
        snprintf(b, sizeof b, "Seen by TW3SE's check of the device (every 0.5 s, and at a fault). The device's reason: 0x%08lx\r\n",
                 static_cast<unsigned long>(dev->GetDeviceRemovedReason()));
    r += b;
    ID3D12DeviceRemovedExtendedData1* dred = nullptr;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dred)))) {
        r += "DRED data not available (it was not on when the device was made)\r\n";
    } else {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc{};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&bc))) {
            r += "\r\n[command lists the GPU had not finished]\r\n";
            int lists = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE1* n = bc.pHeadAutoBreadcrumbNode; n && lists < 40; n = n->pNext) {
                UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
                if (done >= n->BreadcrumbCount) continue;  // finished
                ++lists;
                std::string list = n->pCommandListDebugNameW ? Narrow(n->pCommandListDebugNameW) : n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "";
                std::string queue = n->pCommandQueueDebugNameW ? Narrow(n->pCommandQueueDebugNameW) : n->pCommandQueueDebugNameA ? n->pCommandQueueDebugNameA : "";
                snprintf(b, sizeof b, "list %p%s%s on queue %p%s%s: %u of %u operations done\r\n", static_cast<const void*>(n->pCommandList),
                         list.empty() ? "" : " \"", list.empty() ? "" : (list + "\"").c_str(), static_cast<const void*>(n->pCommandQueue),
                         queue.empty() ? "" : " \"", queue.empty() ? "" : (queue + "\"").c_str(), done, n->BreadcrumbCount);
                r += b;
                UINT from = done > 4 ? done - 4 : 0, to = done + 3 < n->BreadcrumbCount ? done + 3 : n->BreadcrumbCount;
                for (UINT i = from; i < to; ++i) {
                    snprintf(b, sizeof b, "    %s %u %s\r\n", i < done ? "done" : i == done ? "THIS ->" : "next", i,
                             OpName(n->pCommandHistory[i]));
                    r += b;
                }
            }
            if (!lists) r += "none (every list had finished)\r\n";
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT1 pf{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pf))) {
            snprintf(b, sizeof b, "\r\n[the GPU faulted on address 0x%llx]\r\n", static_cast<unsigned long long>(pf.PageFaultVA));
            r += b;
            if (pf.PageFaultVA) {
                r += "allocations that lie there now:\r\n" + AllocList(pf.pHeadExistingAllocationNode, 16);
                r += "allocations freed there a moment before (the GPU used memory that was given back):\r\n" +
                     AllocList(pf.pHeadRecentFreedAllocationNode, 16);
            }
        }
        dred->Release();
    }
    std::wstring out = Dir() + L"\\out\\dred.txt";
    HANDLE h = CreateFileW(out.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, r.data(), static_cast<DWORD>(r.size()), &w, nullptr);
        CloseHandle(h);
    }
    Log("dred: the device is gone (reason 0x%08lx) - tw3se\\out\\dred.txt written", static_cast<unsigned long>(dev->GetDeviceRemovedReason()));
}

void CmdDredTest(const char* args, char* reply, uint32_t size) {
    if (!g_on) {
        snprintf(reply, size, "dred is off (dred = on in tw3se.config)");
        return;
    }
    if (!args || strcmp(args, "remove")) {
        snprintf(reply, size, "dredtest remove: drops the game's device on purpose (the game ends; to check dred.txt)");
        return;
    }
    ID3D12Device5* d5 = nullptr;
    if (ID3D12Device* dev = g_dev.load()) dev->QueryInterface(IID_PPV_ARGS(&d5));
    if (!d5) {
        snprintf(reply, size, "no D3D12 device known yet");
        return;
    }
    d5->RemoveDevice();
    d5->Release();
    snprintf(reply, size, "device removed");
}

// The game's device: gone? Then the report (once). From the poll thread or a fault.
void CheckDevice(HRESULT why) {
    ID3D12Device* dev = g_dev.load();
    if (!g_on || !dev || g_written.load()) return;
    if (FAILED(dev->GetDeviceRemovedReason()) && !g_written.exchange(true)) WriteDred(dev, why);
}

DWORD WINAPI PollThread(void*) {
    for (;;) {
        Sleep(500);
        CheckDevice(S_OK);
        if (g_written.load()) return 0;
    }
}

}  // namespace

// A fault outside a guard: a game whose device is gone often crashes in the driver before it presents again
void DredOnFault() { CheckDevice(S_OK); }

// Before the game makes its device: auto breadcrumbs, their context and page fault data on
void InstallDred() {
    RegisterCommand("dredtest", &CmdDredTest, "dredtest remove: drop the D3D12 device on purpose (dred = on), to see dred.txt");
    if (!ConfigBool("dred", false)) return;
    HMODULE d3d = LoadLibraryW(L"d3d12.dll");
    using GetDebug_t = HRESULT(WINAPI*)(REFIID, void**);
    auto get = d3d ? reinterpret_cast<GetDebug_t>(GetProcAddress(d3d, "D3D12GetDebugInterface")) : nullptr;
    ID3D12DeviceRemovedExtendedDataSettings1* s = nullptr;
    if (!get || FAILED(get(IID_PPV_ARGS(&s)))) {
        Log("dred: not available on this Windows (D3D12GetDebugInterface)");
        return;
    }
    s->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    s->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    s->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    s->Release();
    g_on = true;
    InstallOverlay(true);  // its present hook sees a present fail
    if (HANDLE t = CreateThread(nullptr, 0, &PollThread, nullptr, 0, nullptr)) CloseHandle(t);  // and a device gone without a present
    Log("dred: on (a GPU crash writes tw3se\\out\\dred.txt; costs a little GPU time)");
}

// A present that failed: with the device gone, the DRED report (once)
void DredOnPresent(void* swapChain, long hr) {
    auto* sc = static_cast<IDXGISwapChain*>(swapChain);
    if (g_on && sc && g_sc.exchange(sc) != sc) {  // a swap chain not seen before: its device
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
            if (g_dev.load() == dev) dev->Release();
            else g_dev = dev;  // (a device before it keeps its reference: another thread may still ask it)
        }
    }
    if (!g_on || (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET && hr != DXGI_ERROR_DEVICE_HUNG) ||
        g_written.exchange(true))
        return;
    ID3D12Device* dev = nullptr;
    if (sc && SUCCEEDED(sc->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
        WriteDred(dev, hr);
        dev->Release();
    }
}

}  // namespace tw3se
