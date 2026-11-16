// TW3SE core - D3D12 present hook, the foundation for an on-screen overlay (custom HUD / debug UI).
//
// The game renders through a DXGI swap chain; TW3SE draws each frame from a hook on its Present, the way ReShade,
// Special K and the ImGui backends do: a throwaway D3D12 device and a 1x1 dummy swap chain give Present / Present1
// from their vtable. Those functions live in the system dxgi.dll, shared by the game and the dummy, so the hooked
// address is the one the game calls too, on every game patch.
//
// Plugins register a per-present callback here (interface v8); it hands over the live swap chain, so a renderer
// gets the back buffer and device from it.
#include "core.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <atomic>
#include <mutex>
#include <thread>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "gdi32.lib")  // GDI: rasterize a font into the glyph-atlas texture

namespace tw3se {
// ui.cpp: the in-game UI draws into the overlay's command list
void UiRender(ID3D12Device* dev, ID3D12CommandQueue* queue, int frames, DXGI_FORMAT fmt, ID3D12DescriptorHeap* heap,
              D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu, ID3D12GraphicsCommandList* cl);
bool UiWantsDraw();
void UiRenderShutdown();

namespace {

using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT);
using Present1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ExecCmdLists_t = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ResizeBuffers1_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                     const UINT*, IUnknown* const*);

Present_t g_origPresent = nullptr;
Present1_t g_origPresent1 = nullptr;
ExecCmdLists_t g_origExec = nullptr;
ResizeBuffers_t g_origResize = nullptr;
using CreateSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                      IDXGISwapChain**);
using CreateSwapChainForHwnd_t = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                            const DXGI_SWAP_CHAIN_DESC1*,
                                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                            IDXGISwapChain1**);
CreateSwapChain_t g_origCreateSc = nullptr;
CreateSwapChainForHwnd_t g_origCreateScHwnd = nullptr;
void** g_factoryVtable = nullptr;  // the DXGI factory's (GetSwapChainVTable): its CreateSwapChain slots get hooked
bool g_ovInitFlag();              // g_ovInit, defined with the renderer below
ResizeBuffers1_t g_origResize1 = nullptr;
void* g_resize1Slot = nullptr;  // IDXGISwapChain3::ResizeBuffers1 of the dummy swap chain (GetSwapChainVTable)

void OverlayShutdown(bool waitGpu);  // defined with the renderer below; ResizeBuffers hook passes false
// The overlay's life: made and drawn on the thread that presents, dropped on the one that resizes or makes a new
// swap chain. With DLSS Frame Generation a present ran beside a ResizeBuffers - it started the UI's backend
// again while the resize was dropping it (ImGui: "Already initialized a renderer backend!") and faulted on the freed
// heap; the overlay stayed off from then on. A present that finds it taken draws no overlay this once.
std::recursive_mutex g_ovLifeMx;

// The game's PRESENTATION command queue. A present-time draw must be submitted on it. We cannot get it from
// the swap chain, so we hook ID3D12CommandQueue::ExecuteCommandLists (shared across all queues via the runtime
// vtable, same patch-proof trick as the present hook). Under Streamline there are multiple DIRECT queues -
// Streamline runs its own internal queues (Reflex/frame pacing) even with FG off, and submitting to one of
// those is refused with ACCESS_DENIED. So the hook tracks the last DIRECT queue to submit each frame and latches
// it at the first Present: the queue that submits right before Present is the presentation queue.
ID3D12CommandQueue* g_gameQueue = nullptr;       // latched presentation queue (used by the renderer)
ID3D12CommandQueue* g_lastDirectExec = nullptr;  // most recent DIRECT queue to call ExecuteCommandLists
// The queue a swap chain the game made while we watch presents from (its creation queue, or the first present queue
// of ResizeBuffers1), held with a reference. Latched in place of g_lastDirectExec: XeSS FG's swap chain sits
// on XeSS's own queue while the last DIRECT submit before Present is still the game's - the overlay drew on the
// game's queue and the frame never finished (the game hung at the first switch).
ID3D12CommandQueue* g_scQueue = nullptr;
void SetSwapChainQueue(IUnknown* q, const char* from) {
    ID3D12CommandQueue* cq = nullptr;
    if (!q || FAILED(q->QueryInterface(IID_PPV_ARGS(&cq))) || !cq) return;
    if (cq->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        cq->Release();
        return;
    }
    if (g_scQueue) g_scQueue->Release();
    g_scQueue = cq;  // (the reference QueryInterface gave)
    Log("overlay: the swap chain presents from queue %p (%s)", cq, from);
}
unsigned long long g_firstPresentTick = 0;       // when the present hook first fired (for a draw warm-up)

struct PresentCb {
    TW3SE_PresentCallback cb;
    void* context;
    bool dead;
};
std::vector<PresentCb> g_presentCbs;

// Runs the present callbacks, SEH-guarded (a faulting one is dropped). `swapChain` is the live game swap chain.
void FirePresent(void* swapChain);

// Both hooks funnel here. Logs once so we can confirm in game which path the game uses, then fires callbacks.
// Only the game's own swap chain counts: the hooks sit on the vtable every DXGI swap chain in the process shares,
// and a plugin's OpenGL (cj_qt's offscreen surface; NVIDIA presents GL through a DXGI swap chain of its own) presents
// too; its other size would make the renderer rebuild on it and fail. The game's window is the one the first
// present goes to.
HWND g_gameWnd = nullptr;  // the window the game's first present went to
bool IsGameSwapChain(void* swapChain) {
    HWND& gameWnd = g_gameWnd;
    HWND hwnd = nullptr;
    if (!swapChain || FAILED(static_cast<IDXGISwapChain1*>(swapChain)->GetHwnd(&hwnd)) || !hwnd) return gameWnd == nullptr;
    if (!gameWnd) {
        gameWnd = hwnd;
        return true;
    }
    if (hwnd == gameWnd) return true;
    static HWND logged = nullptr;
    if (logged != hwnd) {
        logged = hwnd;
        Log("overlay: a present to another window (%p) - not the game's, left alone", hwnd);
    }
    return false;
}

// For a menu that never comes up (ui.cpp; on an AMD card the present hook went live and the renderer never
// said a word): how many presents reached the hook, how many reached the overlay, and where the overlay last stopped.
std::atomic<unsigned> g_presentsGame{0}, g_presentsOther{0}, g_ovCalls{0};
std::atomic<const char*> g_ovWhy{"no present yet"};

void OnPresentCommon(void* swapChain, const char* which) {
    if (!IsGameSwapChain(swapChain)) {
        ++g_presentsOther;
        return;
    }
    ++g_presentsGame;
    static bool first = true;
    if (first) {
        first = false;
        g_firstPresentTick = GetTickCount64();
        Log("overlay: present hook live (%s)", which);
    }
    // Latch the presentation queue: the last DIRECT queue that submitted before this Present (not the first one
    // we ever saw, which under Streamline may be an internal privileged queue that refuses our submissions).
    if (!g_gameQueue && g_scQueue) {
        g_gameQueue = g_scQueue;
        Log("overlay: command queue captured (%p) - the swap chain's own", g_gameQueue);
    } else if (!g_gameQueue && g_lastDirectExec) {
        g_gameQueue = g_lastDirectExec;
        Log("overlay: command queue captured (%p) - last DIRECT submit before present", g_gameQueue);
    }
    FirePresent(swapChain);
    RenderOnPresent();
}

HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain3* sc, UINT sync, UINT flags) {
    OnPresentCommon(sc, "Present");
    HRESULT hr = g_origPresent(sc, sync, flags);
    DredOnPresent(sc, hr);
    return hr;
}
HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain3* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    OnPresentCommon(sc, "Present1");
    HRESULT hr = g_origPresent1(sc, sync, flags, p);
    DredOnPresent(sc, hr);
    return hr;
}

// Remembers the most recent DIRECT queue to submit (until the presentation queue is latched at Present), then
// always calls through unchanged. Logs each distinct DIRECT queue once, to see how many Streamline runs.
void STDMETHODCALLTYPE ExecHook(ID3D12CommandQueue* self, UINT n, ID3D12CommandList* const* lists) {
    if (!g_gameQueue && self) {
        D3D12_COMMAND_QUEUE_DESC d = self->GetDesc();
        if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            if (self != g_lastDirectExec) Log("overlay: DIRECT queue submits (%p)", self);
            g_lastDirectExec = self;
        }
    }
    DeformNoteSubmitted(reinterpret_cast<void* const*>(lists), n);  // the lists the game really draws with: their draw functions get hooked
    RenderNoteSubmitted(reinterpret_cast<void* const*>(lists), n);  // the render API's list hooks (once a plugin uses it)
    RenderBeforeSubmit(self, g_gameQueue);  // plugins' work of the frame, ahead of the game's first submission
    DeformBeforeSubmit(self, n);  // vertex deformation work, ahead of the game's lists on the same queue
    g_origExec(self, n, lists);
}

// The game resizes/recreates its swap-chain buffers (resolution change, fullscreen toggle, or during load).
// We hold references to the old buffers; release everything first (so ResizeBuffers can succeed), then let it
// through. The overlay rebuilds from the new buffers on the next present. This is what keeps a cached back
// buffer from going stale under us (which otherwise takes the device down when we draw to it).
// A resize that failed: the references left on each back buffer (one held fails the call, see Unwrap).
void LogResizeFailure(IDXGISwapChain3* sc, HRESULT hr) {
    DXGI_SWAP_CHAIN_DESC1 d;
    char line[256] = "";
    int n = 0;
    if (SUCCEEDED(sc->GetDesc1(&d))) {
        for (UINT i = 0; i < d.BufferCount && n < 200; ++i) {
            ID3D12Resource* r = nullptr;
            if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&r))) || !r) continue;
            n += snprintf(line + n, sizeof(line) - n, " %lu", r->Release());
        }
    }
    Log("overlay: resizing the swap chain failed 0x%08lx, back buffer references:%s", hr, line);
}

HRESULT STDMETHODCALLTYPE ResizeBuffersHook(IDXGISwapChain3* sc, UINT bufCount, UINT w, UINT h, DXGI_FORMAT fmt,
                                            UINT flags) {
    Log("overlay: ResizeBuffers(%u x %u, count %u) - dropping cached buffers", w, h, bufCount);
    std::lock_guard<std::recursive_mutex> life(g_ovLifeMx);  // (until the buffers are new: no overlay drawn meanwhile)
    OverlayShutdown(false);
    HRESULT hr = g_origResize(sc, bufCount, w, h, fmt, flags);
    if (FAILED(hr)) LogResizeFailure(sc, hr);
    return hr;
}

// The same for IDXGISwapChain3::ResizeBuffers1 (a buffer per GPU node; the game uses ResizeBuffers, others may not).
HRESULT STDMETHODCALLTYPE ResizeBuffers1Hook(IDXGISwapChain3* sc, UINT bufCount, UINT w, UINT h, DXGI_FORMAT fmt,
                                             UINT flags, const UINT* nodeMasks, IUnknown* const* queues) {
    Log("overlay: ResizeBuffers1(%u x %u, count %u, first present queue %p) - dropping cached buffers", w, h, bufCount,
        queues ? static_cast<void*>(queues[0]) : nullptr);
    std::lock_guard<std::recursive_mutex> life(g_ovLifeMx);
    OverlayShutdown(false);
    HRESULT hr = g_origResize1(sc, bufCount, w, h, fmt, flags, nodeMasks, queues);
    if (FAILED(hr)) LogResizeFailure(sc, hr);
    if (SUCCEEDED(hr) && queues && queues[0] && IsGameSwapChain(sc)) {  // (its buffers present from these from now on)
        SetSwapChainQueue(queues[0], "ResizeBuffers1");
        g_gameQueue = nullptr;
    }
    return hr;
}

// The game makes a new swap chain (FSR Frame Generation switched on or off: FidelityFX brings its own)
// instead of resizing the old one. The overlay's references on the old one's back buffers keep it alive, and a
// window takes only one: the new one failed and the game read a null swap chain (witcher3.exe+0x1ec5860). Our
// buffers go first, as in ResizeBuffers; the overlay is made anew on the next present.
extern bool g_ovDisabled;
// After the game's new swap chain (FSR Frame Generation switched while playing): the overlay made anew on it
// (`overlay_new_swapchain = rebuild`, the default) or off for the rest of the session (`= off`). Measured with
// plugins that draw: rebuild held FSR FG (world 20, main menu 10), upscaler
// switches with FG on (8) and DLSS FG (10); with XeSS FG the game hung at the first switch (off: 10 held). A game
// started with frame generation on makes the overlay on FidelityFX's swap chain from the start and draws (measured).
void OverlayAfterNewSwapChain() {
    // the queue the game presents with is latched again before the next present (measured: the overlay made
    // anew on FidelityFX's swap chain with the old one faulted in the driver, nvwgf2umx.dll+0x7d99a3)
    g_gameQueue = nullptr;
    g_lastDirectExec = nullptr;
    static std::string mode = ConfigGet("overlay_new_swapchain", "rebuild");
    if (mode == "off" && !g_ovDisabled) {
        g_ovDisabled = true;
        Log("overlay: off until the game starts again (it made a new swap chain: frame generation switched; "
            "overlay_new_swapchain = off)");
    }
}
HRESULT STDMETHODCALLTYPE CreateSwapChainHook(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d,
                                              IDXGISwapChain** out) {
    const bool ours = g_ovInitFlag() && d && d->OutputWindow == g_gameWnd;  // (decided before the shutdown clears it)
    std::unique_lock<std::recursive_mutex> life(g_ovLifeMx, std::defer_lock);
    if (ours) {  // (another window's: not ours to drop for)
        life.lock();
        Log("overlay: a new swap chain (%u x %u, on queue %p) - dropping cached buffers", d ? d->BufferDesc.Width : 0,
            d ? d->BufferDesc.Height : 0, static_cast<void*>(dev));
        OverlayShutdown(false);
        OverlayAfterNewSwapChain();
    }
    HRESULT hr = g_origCreateSc(f, dev, d, out);
    if (FAILED(hr)) Log("overlay: the game's new swap chain failed 0x%08lx", hr);
    else if (ours) SetSwapChainQueue(dev, "CreateSwapChain");
    return hr;
}
HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndHook(IDXGIFactory2* f, IUnknown* dev, HWND hwnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* d,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o,
                                                     IDXGISwapChain1** out) {
    const bool ours = g_ovInitFlag() && hwnd == g_gameWnd;  // (decided before the shutdown clears it)
    std::unique_lock<std::recursive_mutex> life(g_ovLifeMx, std::defer_lock);
    if (ours) {  // (another window's: not ours to drop for)
        life.lock();
        Log("overlay: a new swap chain (%u x %u, on queue %p) - dropping cached buffers", d ? d->Width : 0,
            d ? d->Height : 0, static_cast<void*>(dev));
        OverlayShutdown(false);
        OverlayAfterNewSwapChain();
    }
    HRESULT hr = g_origCreateScHwnd(f, dev, hwnd, d, fs, o, out);
    if (FAILED(hr)) Log("overlay: the game's new swap chain failed 0x%08lx", hr);
    else if (ours) SetSwapChainQueue(dev, "CreateSwapChainForHwnd");
    return hr;
}

// Creates a throwaway device + 1x1 swap chain on a hidden window, returns the swap chain's vtable (caller
// reads the Present slots) and the command queue's vtable (caller reads ExecuteCommandLists), then releases
// everything. The vtables are the runtime's, shared with the game, so hooking a slot catches the game's call.
bool GetSwapChainVTable(void*** outVtable, void*** outQueueVtable) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TW3SE_DummyWnd";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) { UnregisterClassW(wc.lpszClassName, wc.hInstance); return false; }

    bool ok = false;
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGIFactory4* factory = nullptr;
    IDXGISwapChain1* swap = nullptr;
    do {
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) break;
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) break;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) break;
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = 1;
        sd.Height = 1;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &swap))) break;
        *outVtable = *reinterpret_cast<void***>(swap);        // the swap chain's vtable
        IDXGISwapChain3* swap3 = nullptr;
        if (SUCCEEDED(swap->QueryInterface(IID_PPV_ARGS(&swap3))) && swap3) {
            g_resize1Slot = (*reinterpret_cast<void***>(swap3))[39];  // IDXGISwapChain3::ResizeBuffers1
            swap3->Release();
        }
        *outQueueVtable = *reinterpret_cast<void***>(queue);  // the command queue's vtable
        g_factoryVtable = *reinterpret_cast<void***>(factory);  // the factory's (CreateSwapChain 10, ...ForHwnd 15)
        ok = true;
    } while (false);

    if (swap) swap->Release();
    if (factory) factory->Release();
    if (queue) queue->Release();
    if (device) device->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

}  // namespace

// Registers a per-present callback. Called every frame from the render thread with the live swap chain.
void RegisterPresent(TW3SE_PresentCallback callback, void* context) {
    if (!callback) return;
    g_presentCbs.push_back({callback, context, false});
    InstallOverlay(true);  // something needs the present hook now
}

namespace {
// where a present callback faulted: the instruction (module + offset), read/write and the address it touched
char g_presentFault[200] = "";
int PresentFaultFilter(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep ? ep->ExceptionRecord : nullptr;
    if (!r) return EXCEPTION_EXECUTE_HANDLER;
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(r->ExceptionAddress), &m) && m) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(m, full, MAX_PATH)) {
            const char* b = strrchr(full, '\\');
            strncpy_s(name, b ? b + 1 : full, _TRUNCATE);
        }
    }
    uintptr_t off = reinterpret_cast<uintptr_t>(r->ExceptionAddress) - reinterpret_cast<uintptr_t>(m);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        _snprintf_s(g_presentFault, _TRUNCATE, " at %s+0x%llx, %s 0x%llx", name, static_cast<unsigned long long>(off),
                    r->ExceptionInformation[0] == 1 ? "write to" : r->ExceptionInformation[0] == 8 ? "execute" : "read of",
                    static_cast<unsigned long long>(r->ExceptionInformation[1]));
    else
        _snprintf_s(g_presentFault, _TRUNCATE, " at %s+0x%llx", name, static_cast<unsigned long long>(off));
    return EXCEPTION_EXECUTE_HANDLER;
}

bool FirePresentGuarded(TW3SE_PresentCallback cb, void* sc, void* ctx, DWORD* code) {
    __try {
        cb(sc, ctx);
        return true;
    } __except (PresentFaultFilter(GetExceptionInformation())) {
        *code = GetExceptionCode();
        return false;
    }
}

void FirePresent(void* swapChain) {
    size_t n = g_presentCbs.size();
    for (size_t i = 0; i < n; ++i) {
        if (g_presentCbs[i].dead) continue;
        DWORD code = 0;
        if (!FirePresentGuarded(g_presentCbs[i].cb, swapChain, g_presentCbs[i].context, &code)) {
            g_presentCbs[i].dead = true;
            Log("overlay: present callback %p dropped: exception 0x%08x%s", g_presentCbs[i].cb, code, g_presentFault);
        }
    }
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------
// On-screen overlay renderer, plain D3D12. The game reaches DXGI/D3D12 only through sl.interposer.dll (NVIDIA
// Streamline / DLSS Frame Generation) and holds Streamline's proxy objects from factory and device creation on.
// A D3D11On12 layer on top fails: its state tracking collides with Streamline's, and the back-buffer acquire is
// refused with DXGI_ERROR_ACCESS_DENIED. The overlay draws the way the game and Discord's overlay do: its own
// command allocator and list, an RTV on the back buffer of the swap chain, submitted on the game's presentation
// queue. Streamline runs several DIRECT queues of its own (Reflex, frame pacing, also with FG off) that refuse
// other submissions; the presentation queue is the one that submits right before Present (OnPresentCommon).
// Text is a GDI glyph atlas drawn as one quad per glyph (OverlayInitText/OverlayDrawText).
namespace {

bool g_ovEnabled = false;   // toggled by the console command; nothing happens until set
bool g_ovDraw = false;      // `overlaytest draw`: submit the actual D3D12 draw (the coexistence proof)
bool g_ovDisabled = false;  // latched on init/draw failure so a broken pipeline is never retried
bool g_ovInit = false;
UINT g_ovW = 0, g_ovH = 0, g_ovCount = 0;
DXGI_FORMAT g_ovFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
// uishot: the back buffer after everything was drawn, into a readback buffer, saved once the GPU is done
std::mutex g_shotMx;
std::string g_shotPath;            // asked for (main thread) -> taken by the render thread
std::string g_shotTaking;          // the path of the copy on its way
ID3D12Resource* g_shotBuf = nullptr;
UINT64 g_shotFence = 0;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_shotFp = {};
std::atomic<int> g_shotDone{0};    // shots saved (the command's answer waits for it)
void ShotCollect();
// shot burst (measuring, w3._shot_burst): a box of the back buffer in each of the next n frames, one raw file per
// frame (u32 width, u32 height, u32 frame number, then BGRA rows), to see in which frame a change shows
struct BurstShot {
    ID3D12Resource* buf;
    UINT64 fence;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    std::string path;
    uint32_t frame;
};
std::vector<BurstShot> g_burst;    // on their way back from the GPU (g_shotMx)
std::string g_burstDir;
int g_burstLeft = 0;
uint32_t g_burstNo = 0, g_presentNo = 0;
D3D12_BOX g_burstBox = {};
std::atomic<int> g_burstDone{0};
void BurstCollect();

// Streamline unwrap: the game reaches DXGI/D3D12 only through sl.interposer.dll, so the device, queue and back
// buffers we hold are Streamline PROXY objects - drawing on them is refused with ACCESS_DENIED. The public
// Streamline export slGetNativeInterface(proxy, &native) returns the real underlying D3D12 object; we unwrap
// everything to native before any of our own D3D12 work. If the export is absent (non-Streamline game), unwrap
// returns the object unchanged.
using slGetNative_t = int (*)(void* proxy, void** native);  // sl::Result (0 = eOk)
slGetNative_t g_slGetNative = nullptr;

int g_slAddsRef = -1;  // what the last object measured showed (slGetNativeInterface adds a reference: 1, none: 0)

template <class T>
T* Unwrap(T* proxy) {
    if (!proxy || !g_slGetNative) return proxy;
    void* native = nullptr;
    if (g_slGetNative(proxy, &native) != 0 || !native) return proxy;  // not a proxy, or unwrap unavailable: as-is
    // slGetNativeInterface may add a reference to what it returns (left on the back buffers, the game's ResizeBuffers
    // fails with DXGI_ERROR_INVALID_CALL and ends the game at a resolution change) - but not for every object: on
    // FidelityFX's frame-generation swap chain (FSR FG) it added none, and giving one back took the game's
    // own and ended it. So each unwrap measures its object: a second call, the count before and after (two
    // readings that agree; another thread's AddRef or Release between them gives another difference and does not
    // count). Every reference the calls added is given back: the native object stays borrowed.
    T* n = static_cast<T*>(native);
    int adds = 0, same = 0, calls = 0;
    for (int i = 0; i < 5 && adds < 2 && same < 2; ++i) {
        n->AddRef();
        ULONG c1 = n->Release();
        void* again = nullptr;
        if (g_slGetNative(proxy, &again) != 0 || again != native) break;
        ++calls;
        n->AddRef();
        ULONG c2 = n->Release();
        if (c2 == c1 + 1) ++adds;
        else if (c2 == c1) ++same;
    }
    int add = adds >= 2 ? 1 : same >= 2 ? 0 : g_slAddsRef;  // (undecided: what the last decided object showed)
    if (add >= 0) g_slAddsRef = add;
    if (add == 1)
        for (int k = 0; k <= calls; ++k) n->Release();  // the first call's reference and the measuring calls'
    static int logged = 0;
    if (logged < 12) {
        ++logged;
        Log("overlay renderer: unwrap %p->%p: slGetNativeInterface %s (%d of %d readings +1, %d the same)", proxy, n,
            add == 1 ? "adds a reference" : add == 0 ? "adds no reference" : "undecided (none given back)", adds,
            calls, same);
    }
    return n;
}

ID3D12Device* g_devProxy = nullptr;  // owns a ref (from GetDevice); released in shutdown
ID3D12Device* g_dev = nullptr;       // native (unwrapped) - used for all creation; borrowed, not released
ID3D12CommandQueue* g_queue = nullptr;  // native (unwrapped) queue we submit on; borrowed, not released
IDXGISwapChain3* g_scNative = nullptr;  // native (unwrapped) swap chain - GetBuffer/GetCurrentBackBufferIndex on this
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
UINT g_rtvStride = 0;
std::vector<ID3D12Resource*> g_bbProxy;                 // back buffers from GetBuffer (own a ref, release these)
std::vector<ID3D12Resource*> g_backbuf;                 // native (unwrapped) back buffers - used; borrowed
std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> g_rtv;         // one RTV per back buffer (created on the native buffer)
std::vector<ID3D12CommandAllocator*> g_alloc;           // one per back buffer (so we never reset one in flight)
std::vector<UINT64> g_allocFence;                       // fence value each allocator's last submit waits on
ID3D12GraphicsCommandList* g_cl = nullptr;
ID3D12Fence* g_fence = nullptr;
HANDLE g_fenceEvent = nullptr;
UINT64 g_fenceCtr = 0;

// One shared pipeline for every overlay primitive. A per-draw `mode` root constant selects: 0 = solid colour
// (DrawRect), 1 = R8 coverage * colour (DrawText, glyph atlas), 2 = RGBA texture * colour (DrawImage). Geometry
// is a quad from SV_VertexID + per-draw root constants (dest NDC rect, atlas/texture UV rect, colour). Textures
// live in one shader-visible SRV heap: slot 0 = the glyph atlas, slots 1.. = images loaded by plugins.
ID3D12Resource* g_atlas = nullptr;
ID3D12Resource* g_atlasUpload = nullptr;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
UINT g_srvStride = 0;
ID3D12RootSignature* g_ovRoot = nullptr;
ID3D12PipelineState* g_ovPso = nullptr;
UINT g_cellW = 0, g_cellH = 0, g_atlasW = 0, g_atlasH = 0;
const UINT kGlyphFirst = 32, kGlyphLast = 126, kGridCols = 16;
const UINT kSrvSlots = 2048;  // slot 0 atlas, slot 63 the in-game UI's font (ui.cpp), 1..62 plugin images,
                              // 64.. the UI's pictures and text atlas (ui_gpu.cpp)
const UINT kUiFontSlot = 63;
const UINT kViewSlot = 62;  // the game's picture for w3.view.rect

// w3.view.rect (the game as a viewport between an editor's panels): each frame the back
// buffer is copied, cleared black, and the copy drawn into the rectangle with its own aspect, before the overlay's
// and the in-game UI's drawing, which stay full screen.
std::mutex g_viewMx;
bool g_viewOn = false;
float g_viewX = 0, g_viewY = 0, g_viewW = 0, g_viewH = 0;  // asked for, screen pixels
ID3D12Resource* g_viewTex = nullptr;
D3D12_RESOURCE_STATES g_viewState = D3D12_RESOURCE_STATE_COPY_DEST;

// Immediate-mode draw: plugins register an OnOverlayDraw callback and, inside it, call the draw primitives.
// Recording is only valid during that callback (the command list is open and bound then).
struct OverlayDrawEntry { TW3SE_OverlayDrawCallback cb; void* ctx; bool dead; };
std::vector<OverlayDrawEntry> g_overlayDrawCbs;
bool g_recording = false;

template <class T>
void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

void OverlayWaitGpu() {  // block until everything we submitted is done (before releasing resources)
    if (g_queue && g_fence && g_fenceEvent) {
        UINT64 v = ++g_fenceCtr;
        if (SUCCEEDED(g_queue->Signal(g_fence, v)) && g_fence->GetCompletedValue() < v) {
            g_fence->SetEventOnCompletion(v, g_fenceEvent);
            WaitForSingleObject(g_fenceEvent, 2000);
        }
    }
}

// Waits for the overlay's last submitted list without a new Signal (the ResizeBuffers hook: signalling the game's
// queue mid-resize is unsafe, but the lists we submitted may still run when we release their allocators)
void OverlayWaitSubmitted() {
    if (g_fence && g_fenceEvent && g_fenceCtr && g_fence->GetCompletedValue() < g_fenceCtr) {
        g_fence->SetEventOnCompletion(g_fenceCtr, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 2000);
    }
}

void* g_ovSc = nullptr;  // the swap chain the overlay was made on (a present on another one: made anew)
bool g_ovInitFlag() { return g_ovInit; }

void OverlayShutdown(bool waitGpu = true) {
    if (waitGpu) OverlayWaitGpu();  // skipped from the ResizeBuffers hook: the app already synchronized, and
                                    // signalling the game queue mid-resize is unsafe
    else OverlayWaitSubmitted();
    UiRenderShutdown();  // the UI's backend lives in our heap and on our device: it starts again with them
    SafeRelease(g_viewTex);  // made again in the back buffer's new size
    for (auto& b : g_bbProxy) SafeRelease(b);  // only the refs we own (GetBuffer); natives are borrowed
    for (auto& a : g_alloc) SafeRelease(a);
    g_bbProxy.clear();
    g_backbuf.clear();
    g_alloc.clear();
    g_rtv.clear();
    g_allocFence.clear();
    SafeRelease(g_ovPso);
    SafeRelease(g_ovRoot);
    SafeRelease(g_srvHeap);
    SafeRelease(g_atlasUpload);
    SafeRelease(g_atlas);
    SafeRelease(g_cl);
    SafeRelease(g_rtvHeap);
    SafeRelease(g_fence);
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    SafeRelease(g_devProxy);  // owns a ref; the unwrapped native g_dev is borrowed - just forget it
    g_dev = nullptr;
    g_queue = nullptr;
    g_scNative = nullptr;
    g_fenceCtr = 0;
    g_ovInit = false;
    g_ovSc = nullptr;
    g_ovW = g_ovH = g_ovCount = 0;
}

// One shader for every overlay primitive. VS builds a quad from SV_VertexID, placed by root constants (dst =
// NDC rect, uv = texture rect). PS branches on `mode`: 0 = solid colour, 1 = R8 coverage*colour (text),
// 2 = RGBA texture*colour (image).
static const char* kOverlayHlsl =
    "cbuffer K : register(b0) { float4 dst; float4 uv; float4 col; float mode; };"
    "Texture2D gTex : register(t0); SamplerState gSmp : register(s0);"
    "struct V { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };"
    "V VSMain(uint id : SV_VertexID) {"
    "  float2 c = float2((id>>1)&1, id&1);"
    "  V o; o.pos = float4(dst.x + c.x*dst.z, dst.y - c.y*dst.w, 0, 1);"
    "  o.uv = float2(uv.x + c.x*uv.z, uv.y + c.y*uv.w); return o;"
    "}"
    "float4 PSMain(V i) : SV_Target {"
    "  if (mode < 0.5) return col;"
    "  float4 t = gTex.Sample(gSmp, i.uv);"
    "  if (mode < 1.5) return float4(col.rgb, col.a * t.r);"
    "  if (mode < 2.5) return t * col;"
    "  return float4(t.rgb, 1);"
    "}";

// Rasterize ASCII 32..126 into an R8 coverage atlas with GDI (antialiased, grayscale). Monospace (Consolas),
// so one fixed cell per glyph. Fills `out` (g_atlasW*g_atlasH bytes) and sets the cell/atlas dimensions.
bool BuildGlyphAtlas(std::vector<uint8_t>& out) {
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT font = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    SIZE sz = {};
    wchar_t mm = L'M';
    GetTextExtentPoint32W(dc, &mm, 1, &sz);
    g_cellW = static_cast<UINT>(sz.cx) + 2;
    g_cellH = static_cast<UINT>(tm.tmHeight) + 2;
    UINT count = kGlyphLast - kGlyphFirst + 1;
    UINT rows = (count + kGridCols - 1) / kGridCols;
    g_atlasW = g_cellW * kGridCols;
    g_atlasH = g_cellH * rows;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = static_cast<LONG>(g_atlasW);
    bi.bmiHeader.biHeight = -static_cast<LONG>(g_atlasH);  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp) {
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        RECT full = {0, 0, static_cast<LONG>(g_atlasW), static_cast<LONG>(g_atlasH)};
        SetBkColor(dc, RGB(0, 0, 0));
        SetBkMode(dc, OPAQUE);
        SetTextColor(dc, RGB(255, 255, 255));
        ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &full, L"", 0, nullptr);  // clear to black
        for (UINT ci = 0; ci < count; ++ci) {
            wchar_t ch = static_cast<wchar_t>(kGlyphFirst + ci);
            UINT col = ci % kGridCols, row = ci / kGridCols;
            ExtTextOutW(dc, static_cast<int>(col * g_cellW + 1), static_cast<int>(row * g_cellH + 1), 0, nullptr,
                        &ch, 1, nullptr);
        }
        GdiFlush();
        out.resize(static_cast<size_t>(g_atlasW) * g_atlasH);
        const uint32_t* px = static_cast<const uint32_t*>(bits);
        for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(px[i] & 0xFF);  // blue = coverage
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        ok = true;
    }
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    return ok;
}

// Builds the shared draw pipeline: the glyph atlas as an R8 texture in SRV slot 0 (copy on our queue,
// fence-wait), a 64-slot shader-visible SRV heap, the root signature (13 root constants + SRV table + static
// sampler) and one alpha-blended PSO used for every primitive.
bool OverlayInitPipelines() {
    std::vector<uint8_t> atlas;
    if (!BuildGlyphAtlas(atlas) || atlas.empty()) return false;

    D3D12_HEAP_PROPERTIES hpDef = {};
    hpDef.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = g_atlasW;
    td.Height = g_atlasH;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    if (FAILED(g_dev->CreateCommittedResource(&hpDef, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&g_atlas))))
        return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT numRows = 0;
    UINT64 rowBytes = 0, total = 0;
    g_dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &numRows, &rowBytes, &total);

    D3D12_HEAP_PROPERTIES hpUp = {};
    hpUp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_dev->CreateCommittedResource(&hpUp, D3D12_HEAP_FLAG_NONE, &bd,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&g_atlasUpload))))
        return false;
    uint8_t* map = nullptr;
    D3D12_RANGE none = {0, 0};
    if (FAILED(g_atlasUpload->Map(0, &none, reinterpret_cast<void**>(&map)))) return false;
    for (UINT y = 0; y < g_atlasH; ++y)
        memcpy(map + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch,
               atlas.data() + static_cast<size_t>(y) * g_atlasW, g_atlasW);
    g_atlasUpload->Unmap(0, nullptr);

    g_cl->Reset(g_alloc[0], nullptr);
    D3D12_TEXTURE_COPY_LOCATION dstL = {};
    dstL.pResource = g_atlas;
    dstL.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstL.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION srcL = {};
    srcL.pResource = g_atlasUpload;
    srcL.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcL.PlacedFootprint = fp;
    g_cl->CopyTextureRegion(&dstL, 0, 0, 0, &srcL, nullptr);
    D3D12_RESOURCE_BARRIER tb = {};
    tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    tb.Transition.pResource = g_atlas;
    tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    tb.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g_cl->ResourceBarrier(1, &tb);
    g_cl->Close();
    ID3D12CommandList* lists[] = {g_cl};
    g_queue->ExecuteCommandLists(1, lists);
    UINT64 fv = ++g_fenceCtr;
    g_queue->Signal(g_fence, fv);
    if (g_fence->GetCompletedValue() < fv) {
        g_fence->SetEventOnCompletion(fv, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 2000);
    }

    D3D12_DESCRIPTOR_HEAP_DESC shd = {};
    shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    shd.NumDescriptors = kSrvSlots;
    shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&g_srvHeap)))) return false;
    g_srvStride = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(g_atlas, &sv, g_srvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 13;  // dst(4) + uv(4) + colour(4) + mode(1)
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &range;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC smp = {};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rd = {};
    rd.NumParameters = 2;
    rd.pParameters = rp;
    rd.NumStaticSamplers = 1;
    rd.pStaticSamplers = &smp;
    ID3DBlob* rsBlob = nullptr;
    ID3DBlob* rsErr = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr))) {
        SafeRelease(rsErr);
        return false;
    }
    HRESULT hr = g_dev->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                            IID_PPV_ARGS(&g_ovRoot));
    SafeRelease(rsBlob);
    SafeRelease(rsErr);
    if (FAILED(hr)) return false;

    ID3DBlob* vs = nullptr;
    ID3DBlob* ps = nullptr;
    ID3DBlob* err = nullptr;
    UINT cf = D3DCOMPILE_OPTIMIZATION_LEVEL1;
    if (FAILED(D3DCompile(kOverlayHlsl, strlen(kOverlayHlsl), "txt", nullptr, nullptr, "VSMain", "vs_5_0", cf, 0, &vs,
                          &err))) {
        SafeRelease(err);
        return false;
    }
    SafeRelease(err);
    if (FAILED(D3DCompile(kOverlayHlsl, strlen(kOverlayHlsl), "txt", nullptr, nullptr, "PSMain", "ps_5_0", cf, 0, &ps,
                          &err))) {
        SafeRelease(err);
        SafeRelease(vs);
        return false;
    }
    SafeRelease(err);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_ovRoot;
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = g_ovFmt;
    pd.SampleDesc.Count = 1;
    hr = g_dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&g_ovPso));
    SafeRelease(vs);
    SafeRelease(ps);
    if (FAILED(hr)) return false;
    Log("overlay renderer: draw pipeline ready (atlas %ux%u, cell %ux%u)", g_atlasW, g_atlasH, g_cellW, g_cellH);
    return true;
}

// --- Immediate-mode primitives. Valid only while g_recording (inside an OnOverlayDraw callback); the shared
// root sig / PSO / descriptor heap / topology are already bound by OverlayDraw. Colour is 0xRRGGBBAA. ---
void PushQuad(float x, float y, float w, float h, float u0, float v0, float uw, float vh, float r, float g,
              float b, float a, float mode, UINT srvSlot) {
    float k[13] = {x / g_ovW * 2.0f - 1.0f, 1.0f - y / g_ovH * 2.0f, w / g_ovW * 2.0f, h / g_ovH * 2.0f,
                   u0, v0, uw, vh, r, g, b, a, mode};
    D3D12_GPU_DESCRIPTOR_HANDLE gh = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += static_cast<UINT64>(srvSlot) * g_srvStride;
    g_cl->SetGraphicsRootDescriptorTable(1, gh);
    g_cl->SetGraphicsRoot32BitConstants(0, 13, k, 0);
    g_cl->DrawInstanced(4, 1, 0, 0);
}
inline void Unpack(uint32_t c, float& r, float& g, float& b, float& a) {
    r = ((c >> 24) & 0xff) / 255.0f;
    g = ((c >> 16) & 0xff) / 255.0f;
    b = ((c >> 8) & 0xff) / 255.0f;
    a = (c & 0xff) / 255.0f;
}
void ApiDrawRect(float x, float y, float w, float h, uint32_t rgba) {
    if (!g_recording || !g_cl) return;
    float r, g, b, a;
    Unpack(rgba, r, g, b, a);
    PushQuad(x, y, w, h, 0, 0, 0, 0, r, g, b, a, 0.0f, 0);
}
void ApiDrawText(float x, float y, uint32_t rgba, const char* text) {
    if (!g_recording || !g_cl || !text) return;
    float r, g, b, a;
    Unpack(rgba, r, g, b, a);
    for (const char* p = text; *p; ++p) {
        unsigned char ch = static_cast<unsigned char>(*p);
        if (ch < kGlyphFirst || ch > kGlyphLast) { x += g_cellW; continue; }
        UINT gi = ch - kGlyphFirst, col = gi % kGridCols, row = gi / kGridCols;
        PushQuad(x, y, static_cast<float>(g_cellW), static_cast<float>(g_cellH),
                 static_cast<float>(col * g_cellW) / g_atlasW, static_cast<float>(row * g_cellH) / g_atlasH,
                 static_cast<float>(g_cellW) / g_atlasW, static_cast<float>(g_cellH) / g_atlasH, r, g, b, a,
                 1.0f, 0);
        x += g_cellW;
    }
}

bool OverlayInit(IDXGISwapChain3* sc) {
    if (!g_gameQueue) return false;
    DXGI_SWAP_CHAIN_DESC1 d;
    if (FAILED(sc->GetDesc1(&d))) return false;
    g_ovW = d.Width;
    g_ovH = d.Height;
    g_ovCount = d.BufferCount;
    g_ovFmt = d.Format;

    // Resolve the Streamline unwrap export (once), then unwrap device + queue to their native D3D12 objects.
    if (!g_slGetNative) {
        if (HMODULE sl = GetModuleHandleW(L"sl.interposer.dll"))
            g_slGetNative = reinterpret_cast<slGetNative_t>(GetProcAddress(sl, "slGetNativeInterface"));
    }
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_devProxy)))) return false;
    g_dev = Unwrap(g_devProxy);
    g_queue = Unwrap(g_gameQueue);
    g_scNative = Unwrap(sc);  // the swap chain is the likely Streamline proxy: unwrap it and
    if (!g_scNative) g_scNative = sc;  // call GetBuffer / GetCurrentBackBufferIndex on the native one
    if (!g_dev || !g_queue) return false;
    Log("overlay renderer: streamline unwrap %s (dev %p->%p queue %p->%p swapchain %p->%p)",
        g_slGetNative ? "on" : "off", g_devProxy, g_dev, g_gameQueue, g_queue, sc, g_scNative);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = g_ovCount;
    if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap)))) return false;
    g_rtvStride = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    g_bbProxy.resize(g_ovCount, nullptr);
    g_backbuf.resize(g_ovCount, nullptr);
    g_rtv.resize(g_ovCount);
    g_alloc.resize(g_ovCount, nullptr);
    g_allocFence.resize(g_ovCount, 0);
    for (UINT i = 0; i < g_ovCount; ++i) {
        if (FAILED(g_scNative->GetBuffer(i, IID_PPV_ARGS(&g_bbProxy[i])))) return false;
        g_backbuf[i] = Unwrap(g_bbProxy[i]);  // native back buffer: RTV + barriers go on this one
        if (!g_backbuf[i]) return false;
        if (i == 0)
            Log("overlay renderer: backbuffer[0] %p->%p", g_bbProxy[0], g_backbuf[0]);
        g_rtv[i].ptr = base.ptr + static_cast<SIZE_T>(i) * g_rtvStride;
        g_dev->CreateRenderTargetView(g_backbuf[i], nullptr, g_rtv[i]);
        if (FAILED(g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i]))))
            return false;
    }
    if (FAILED(g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr,
                                        IID_PPV_ARGS(&g_cl))))
        return false;
    g_cl->Close();  // created recording; close so the first per-frame Reset is valid
    if (FAILED(g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) return false;

    if (!OverlayInitPipelines()) return false;  // glyph atlas + shared root signature / PSO
    return true;
}

bool OverlayCbGuarded(TW3SE_OverlayDrawCallback cb, void* ctx, DWORD* code) {
    __try {
        cb(ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

// The game's picture into the rectangle of w3.view.rect: copy, clear, draw the copy. The back buffer is a render
// target here and is one again afterwards.
void ViewFit(float& x, float& y, float& w, float& h) {  // the rectangle at the picture's own aspect, centred
    float a = static_cast<float>(g_ovW) / static_cast<float>(g_ovH);
    if (w / h > a) {
        float nw = h * a;
        x += (w - nw) / 2, w = nw;
    } else {
        float nh = w / a;
        y += (h - nh) / 2, h = nh;
    }
}
void ViewDraw(UINT idx) {
    float x, y, w, h;
    {
        std::lock_guard<std::mutex> lk(g_viewMx);
        if (!g_viewOn) return;
        x = g_viewX, y = g_viewY, w = g_viewW, h = g_viewH;
    }
    if (w < 2 || h < 2 || !g_ovW || !g_ovH) return;
    D3D12_RESOURCE_DESC bd = g_backbuf[idx]->GetDesc();
    if (!g_viewTex) {
        D3D12_RESOURCE_DESC td = bd;
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&g_viewTex)))) {
            Log("overlay: view texture could not be made (%llux%u, format %d)", bd.Width, bd.Height, bd.Format);
            std::lock_guard<std::mutex> lk(g_viewMx);
            g_viewOn = false;
            return;
        }
        g_viewState = D3D12_RESOURCE_STATE_COPY_DEST;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = g_ovFmt;  // the format the swap chain renders in (the buffer itself may be typeless)
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE ch = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        ch.ptr += static_cast<SIZE_T>(kViewSlot) * g_srvStride;
        g_dev->CreateShaderResourceView(g_viewTex, &sv, ch);
        Log("overlay: view texture %llux%u (format %d)", bd.Width, bd.Height, g_ovFmt);
    }
    D3D12_RESOURCE_BARRIER bb[2] = {};
    bb[0].Type = bb[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bb[0].Transition.pResource = g_backbuf[idx];
    bb[0].Transition.Subresource = bb[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bb[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bb[1].Transition.pResource = g_viewTex;
    bb[1].Transition.StateBefore = g_viewState;
    bb[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_cl->ResourceBarrier(g_viewState == D3D12_RESOURCE_STATE_COPY_DEST ? 1 : 2, bb);
    g_cl->CopyResource(g_viewTex, g_backbuf[idx]);
    bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bb[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    bb[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g_cl->ResourceBarrier(2, bb);
    g_viewState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const float black[4] = {0, 0, 0, 1};
    g_cl->ClearRenderTargetView(g_rtv[idx], black, 0, nullptr);
    ViewFit(x, y, w, h);
    PushQuad(x, y, w, h, 0, 0, 1, 1, 1, 1, 1, 1, 3.0f, kViewSlot);
}

void OverlayDraw(IDXGISwapChain3* /*scProxy*/) {
    ShotCollect();
    BurstCollect();
    UINT idx = g_scNative->GetCurrentBackBufferIndex();  // index on the native swap chain, matching our buffers
    if (idx >= g_ovCount || !g_backbuf[idx]) return;
    static bool firstDraw = true;
    if (firstDraw) { firstDraw = false; Log("overlay: first draw (buffer %u)", idx); }
    // Make sure the allocator for this back buffer is no longer in use on the GPU before resetting it.
    if (g_fence->GetCompletedValue() < g_allocFence[idx]) {
        g_fence->SetEventOnCompletion(g_allocFence[idx], g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 2000);
    }
    g_alloc[idx]->Reset();
    g_cl->Reset(g_alloc[idx], g_ovPso);

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = g_backbuf[idx];
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_cl->ResourceBarrier(1, &b);

    g_cl->OMSetRenderTargets(1, &g_rtv[idx], FALSE, nullptr);
    D3D12_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(g_ovW), static_cast<float>(g_ovH), 0.0f, 1.0f};
    D3D12_RECT sr = {0, 0, static_cast<LONG>(g_ovW), static_cast<LONG>(g_ovH)};
    g_cl->RSSetViewports(1, &vp);
    g_cl->RSSetScissorRects(1, &sr);
    ID3D12DescriptorHeap* heaps[] = {g_srvHeap};
    g_cl->SetDescriptorHeaps(1, heaps);
    g_cl->SetGraphicsRootSignature(g_ovRoot);
    g_cl->SetPipelineState(g_ovPso);
    g_cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    g_recording = true;  // primitives are valid now; each PushQuad binds its own SRV slot + constants
    ViewDraw(idx);
    for (auto& e : g_overlayDrawCbs) {
        if (e.dead) continue;
        DWORD code = 0;
        if (!OverlayCbGuarded(e.cb, e.ctx, &code)) {
            e.dead = true;
            Log("overlay: draw callback %p dropped: exception 0x%08x", e.cb, code);
        }
    }
    if (g_ovDraw) ApiDrawText(12.0f, 10.0f, 0xFFFFFFFFu, "TW3SE overlay");  // dev watermark (`overlaytest draw`)
    g_recording = false;
    {  // the in-game UI last, on top (its backend binds its own pipeline; our heap stays bound)
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(kUiFontSlot) * g_srvStride;
        gpu.ptr += static_cast<UINT64>(kUiFontSlot) * g_srvStride;
        UiRender(g_dev, g_queue, static_cast<int>(g_ovCount), g_ovFmt, g_srvHeap, cpu, gpu, g_cl);
    }

    bool shot = false;
    {
        std::lock_guard<std::mutex> lk(g_shotMx);
        if (!g_shotPath.empty() && !g_shotBuf) {
            D3D12_RESOURCE_DESC td = g_backbuf[idx]->GetDesc();
            UINT rows = 0;
            UINT64 rowBytes = 0, total = 0;
            g_dev->GetCopyableFootprints(&td, 0, 1, 0, &g_shotFp, &rows, &rowBytes, &total);
            D3D12_HEAP_PROPERTIES rb = {};
            rb.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(g_dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                         nullptr, IID_PPV_ARGS(&g_shotBuf)))) {
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                g_cl->ResourceBarrier(1, &b);
                D3D12_TEXTURE_COPY_LOCATION src = {};
                src.pResource = g_backbuf[idx];
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = g_shotBuf;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = g_shotFp;
                g_cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                shot = true;
                g_shotTaking.swap(g_shotPath);
                g_shotPath.clear();
            }
        }
    }
    bool burst = false;
    ++g_presentNo;
    {
        std::lock_guard<std::mutex> lk(g_shotMx);
        if (g_burstLeft > 0) {
            D3D12_RESOURCE_DESC td = g_backbuf[idx]->GetDesc();
            D3D12_BOX box = g_burstBox;
            if (box.right <= box.left) {  // no size given: the middle 60 % of the picture
                UINT w = static_cast<UINT>(td.Width), h = td.Height;
                box = {w / 5, h / 5, 0, w * 4 / 5, h * 4 / 5, 1};
            }
            box.right = (std::min)(box.right, static_cast<UINT>(td.Width));
            box.bottom = (std::min)(box.bottom, td.Height);
            if (box.right > box.left && box.bottom > box.top) {
                td.Width = box.right - box.left;
                td.Height = box.bottom - box.top;
                BurstShot s = {};
                UINT rows = 0;
                UINT64 rowBytes = 0, total = 0;
                g_dev->GetCopyableFootprints(&td, 0, 1, 0, &s.fp, &rows, &rowBytes, &total);
                D3D12_HEAP_PROPERTIES rb = {};
                rb.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC bd = {};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = total;
                bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
                bd.SampleDesc.Count = 1;
                bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                if (SUCCEEDED(g_dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd,
                                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.buf)))) {
                    if (!shot) {
                        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                        g_cl->ResourceBarrier(1, &b);
                    }
                    D3D12_TEXTURE_COPY_LOCATION src = {};
                    src.pResource = g_backbuf[idx];
                    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    D3D12_TEXTURE_COPY_LOCATION dst = {};
                    dst.pResource = s.buf;
                    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint = s.fp;
                    g_cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
                    char name[32];
                    snprintf(name, sizeof name, "\\%04u.raw", g_burstNo++);
                    s.path = g_burstDir + name;
                    s.frame = g_presentNo;
                    g_burst.push_back(s);
                    burst = true;
                }
            }
            --g_burstLeft;
        }
    }
    b.Transition.StateBefore = (shot || burst) ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_cl->ResourceBarrier(1, &b);
    g_cl->Close();

    ID3D12CommandList* lists[] = {g_cl};
    g_queue->ExecuteCommandLists(1, lists);
    g_allocFence[idx] = ++g_fenceCtr;
    g_queue->Signal(g_fence, g_allocFence[idx]);
    if (shot) g_shotFence = g_allocFence[idx];
    if (burst) {
        std::lock_guard<std::mutex> lk(g_shotMx);
        g_burst.back().fence = g_allocFence[idx];
    }
}

// Burst copies the GPU finished: written raw (BGRA) on a thread of their own.
void BurstCollect() {
    std::vector<BurstShot> done;
    {
        std::lock_guard<std::mutex> lk(g_shotMx);
        UINT64 completed = g_fence->GetCompletedValue();
        for (size_t i = 0; i < g_burst.size();) {
            if (g_burst[i].fence && g_burst[i].fence <= completed) {
                done.push_back(g_burst[i]);
                g_burst.erase(g_burst.begin() + i);
            } else {
                ++i;
            }
        }
    }
    for (BurstShot& s : done) {
        UINT w = s.fp.Footprint.Width, h = s.fp.Footprint.Height, pitch = s.fp.Footprint.RowPitch;
        DXGI_FORMAT fmt = s.fp.Footprint.Format;
        std::vector<unsigned char> px(12 + static_cast<size_t>(w) * h * 4);
        memcpy(px.data(), &w, 4);
        memcpy(px.data() + 4, &h, 4);
        memcpy(px.data() + 8, &s.frame, 4);
        unsigned char* map = nullptr;
        D3D12_RANGE all = {0, static_cast<SIZE_T>(pitch) * h};
        if (SUCCEEDED(s.buf->Map(0, &all, reinterpret_cast<void**>(&map)))) {
            for (UINT y = 0; y < h; ++y) {
                const unsigned char* row = map + s.fp.Offset + static_cast<size_t>(y) * pitch;
                unsigned char* o = px.data() + 12 + static_cast<size_t>(y) * w * 4;
                for (UINT x = 0; x < w; ++x, o += 4) {
                    if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
                        uint32_t v;
                        memcpy(&v, row + x * 4, 4);
                        o[2] = static_cast<unsigned char>(((v >> 0) & 1023) >> 2);
                        o[1] = static_cast<unsigned char>(((v >> 10) & 1023) >> 2);
                        o[0] = static_cast<unsigned char>(((v >> 20) & 1023) >> 2);
                    } else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                        memcpy(o, row + x * 4, 4);
                    } else {
                        o[0] = row[x * 4 + 2], o[1] = row[x * 4 + 1], o[2] = row[x * 4];
                    }
                    o[3] = 255;
                }
            }
            D3D12_RANGE none = {0, 0};
            s.buf->Unmap(0, &none);
        }
        s.buf->Release();
        std::thread([path = s.path, px = std::move(px)]() {
            FILE* f = fopen(path.c_str(), "wb");
            if (f) {
                fwrite(px.data(), 1, px.size(), f);
                fclose(f);
            }
            ++g_burstDone;
        }).detach();
    }
}

// A copy the GPU finished: to 8-bit BGRA, saved as PNG on a thread of its own (the render thread goes on).
void ShotCollect() {
    std::lock_guard<std::mutex> lk(g_shotMx);
    if (!g_shotBuf || g_fence->GetCompletedValue() < g_shotFence) return;
    UINT w = g_shotFp.Footprint.Width, h = g_shotFp.Footprint.Height, pitch = g_shotFp.Footprint.RowPitch;
    DXGI_FORMAT fmt = g_shotFp.Footprint.Format;
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
    unsigned char* map = nullptr;
    D3D12_RANGE all = {0, static_cast<SIZE_T>(pitch) * h};
    if (SUCCEEDED(g_shotBuf->Map(0, &all, reinterpret_cast<void**>(&map)))) {
        for (UINT y = 0; y < h; ++y) {
            const unsigned char* row = map + g_shotFp.Offset + static_cast<size_t>(y) * pitch;
            unsigned char* o = px.data() + static_cast<size_t>(y) * w * 4;
            for (UINT x = 0; x < w; ++x, o += 4) {
                if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
                    memcpy(o, row + x * 4, 4);
                } else if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
                    uint32_t v;
                    memcpy(&v, row + x * 4, 4);
                    o[2] = static_cast<unsigned char>(((v >> 0) & 1023) >> 2);
                    o[1] = static_cast<unsigned char>(((v >> 10) & 1023) >> 2);
                    o[0] = static_cast<unsigned char>(((v >> 20) & 1023) >> 2);
                } else {  // R8G8B8A8 (and the rest read as it)
                    o[0] = row[x * 4 + 2], o[1] = row[x * 4 + 1], o[2] = row[x * 4];
                }
                o[3] = 255;
            }
        }
        D3D12_RANGE none = {0, 0};
        g_shotBuf->Unmap(0, &none);
    }
    g_shotBuf->Release();
    g_shotBuf = nullptr;
    std::string path = g_shotTaking;
    g_shotTaking.clear();
    std::thread([path, w, h, px = std::move(px)]() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* wic = nullptr;
        bool ok = false;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
            int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
            std::wstring wp(n > 0 ? n - 1 : 0, L'\0');
            if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wp.data(), n);
            IWICStream* st = nullptr;
            IWICBitmapEncoder* enc = nullptr;
            IWICBitmapFrameEncode* fr = nullptr;
            if (SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(wp.c_str(), GENERIC_WRITE)) &&
                SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
                SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h))) {
                WICPixelFormatGUID f = GUID_WICPixelFormat32bppBGRA;
                fr->SetPixelFormat(&f);
                ok = SUCCEEDED(fr->WritePixels(h, w * 4, static_cast<UINT>(px.size()), const_cast<BYTE*>(px.data()))) &&
                     SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
            }
            if (fr) fr->Release();
            if (enc) enc->Release();
            if (st) st->Release();
            wic->Release();
        }
        Log("uishot: %s %s (%ux%u)", ok ? "saved" : "could not save", path.c_str(), w, h);
        ++g_shotDone;
    }).detach();
}

// The internal present callback (registered like any other). Runs on the render thread inside the SEH guard.
void OverlayPresentCb(void* swapChain, void* /*ctx*/) {
    // Draw when a plugin registered a draw callback, or the dev watermark switch is on. `overlaytest on` only
    // initializes the pipeline (proof), without drawing.
    ++g_ovCalls;
    std::unique_lock<std::recursive_mutex> life(g_ovLifeMx, std::try_to_lock);  // (a resize on another thread)
    if (!life.owns_lock()) {
        g_ovWhy = "a resize of the swap chain held it";
        return;
    }
    bool shotWaiting = false;
    {
        std::lock_guard<std::mutex> lk(g_shotMx);
        shotWaiting = !g_shotPath.empty() || g_shotBuf != nullptr ||  // asked for, or on its way back from the GPU
                      g_burstLeft > 0 || !g_burst.empty();
    }
    bool wantDraw = g_ovDraw || !g_overlayDrawCbs.empty() || UiWantsDraw() || shotWaiting;
    if (g_ovDisabled) {
        g_ovWhy = "off after a failure (see the overlay lines before)";
        return;
    }
    if ((!g_ovEnabled && !wantDraw) || !swapChain) {
        g_ovWhy = "nothing to draw";
        return;
    }
    // Warm-up: don't touch the swap chain until the game is past its start-up ResizeBuffers churn (it recreates
    // its buffers during load). Drawing across that resize takes the device down; waiting a few seconds lets it
    // settle so we init on a stable swap chain.
    if (g_firstPresentTick && GetTickCount64() - g_firstPresentTick < 4000) {
        g_ovWhy = "waits 4 s after the first present";
        return;
    }
    auto sc = static_cast<IDXGISwapChain3*>(swapChain);
    DXGI_SWAP_CHAIN_DESC1 d;
    if (g_ovInit && SUCCEEDED(sc->GetDesc1(&d)) && (d.Width != g_ovW || d.Height != g_ovH))
        OverlayShutdown();  // resolution changed: rebuild the targets next frame
    if (g_ovInit && swapChain != g_ovSc) {
        Log("overlay: presents on another swap chain (%p, was %p) - made anew", swapChain, g_ovSc);
        OverlayShutdown();
    }
    if (!g_ovInit) {
        g_ovWhy = "in its start (OverlayInit did not return)";
        static int starts = 0;
        if (starts++ < 3) Log("overlay renderer: starts on swap chain %p, queue %p", swapChain, g_gameQueue);
        if (!OverlayInit(sc)) {
            OverlayShutdown();
            g_ovDisabled = true;
            Log("overlay renderer: init failed - disabled");
            return;
        }
        g_ovInit = true;
        g_ovSc = swapChain;
        Log("overlay renderer: initialized (%ux%u, %u buffers, fmt %d)", g_ovW, g_ovH, g_ovCount,
            static_cast<int>(g_ovFmt));
    }
    // `overlaytest draw` submits the real D3D12 text draw (works under Streamline once we submit on the true
    // presentation queue). Kept as a dev switch. A device-removal is turned into a logged reason instead of a
    // blind crash; a hard access violation is caught by the SEH present guard.
    g_ovWhy = wantDraw ? "draws" : "ready, nothing to draw";
    if (wantDraw) {
        OverlayDraw(sc);
        if (g_dev) {
            HRESULT rr = g_dev->GetDeviceRemovedReason();
            if (rr != S_OK) {
                Log("overlay draw: device removed 0x%08x - disabling overlay", rr);
                g_ovDraw = false;
                g_ovDisabled = true;
                OverlayShutdown();
            }
        }
    }
}

void CmdUiShot(const char* args, char* reply, uint32_t size) {
    std::string path = args ? args : "";
    while (!path.empty() && path.back() == ' ') path.pop_back();
    if (path.empty()) {
        strncpy_s(reply, size, "usage: uishot <file.png>", _TRUNCATE);
        return;
    }
    if (!g_ovInit) {
        strncpy_s(reply, size, "uishot: the overlay draws nothing yet (open the TW3SE Menu or a Lua window first)", _TRUNCATE);
        return;
    }
    int before = g_shotDone.load();
    {
        std::lock_guard<std::mutex> lk(g_shotMx);
        g_shotPath = path;
    }
    for (int i = 0; i < 100 && g_shotDone.load() == before; ++i) Sleep(20);  // a few frames and the encoding
    snprintf(reply, size, g_shotDone.load() != before ? "uishot: %s" : "uishot: asked for %s (see tw3se.log)", path.c_str());
}

void CmdOverlayTest(const char* arg, char* reply, uint32_t size) {
    std::string a = arg ? arg : "";
    if (a != "on" && a != "draw" && a != "off") {
        strncpy_s(reply, size, "usage: overlaytest on|draw|off", _TRUNCATE);
        return;
    }
    if (a != "off") InstallOverlay(true);
    if (a == "off") {
        g_ovEnabled = false;
        g_ovDraw = false;
        strncpy_s(reply, size, "overlay: off", _TRUNCATE);
        return;
    }
    if (a == "draw") {  // experimental: attempt the real draw (may take the device down - supervised only)
        g_ovEnabled = true;
        g_ovDraw = true;
        g_ovDisabled = false;
        strncpy_s(reply, size, "overlay: draw ON (text watermark top-left)", _TRUNCATE);
        return;
    }
    g_ovEnabled = true;   // plain `on`: initialize the pipeline only, no draw (safe)
    g_ovDraw = false;
    g_ovDisabled = false;
    strncpy_s(reply, size,
              g_ovInit ? "overlay: on (pipeline ready; `overlaytest draw` to attempt drawing)"
                       : "overlay: on (initializes on the next frame; `draw` to attempt drawing)",
              _TRUNCATE);
}

}  // namespace

// w3.ui.screenshot (lua_ui.h): the same copy as uishot, asked from Lua without waiting for it
bool UiScreenshot(const char* path) {
    // a shot asked for makes the next frames draw (OverlayPresentCb), and the overlay sets itself up for it (in a
    // game where nothing drew yet)
    if (g_ovDisabled || !path || !*path) return false;
    std::lock_guard<std::mutex> lk(g_shotMx);
    g_shotPath = path;
    return true;
}
int UiScreenshotsDone() { return g_shotDone.load(); }
bool UiShotBurst(const char* dir, int frames, int x, int y, int w, int h) {
    if (g_ovDisabled || !dir || !*dir || frames <= 0) return false;
    if (w <= 0 || h <= 0) x = y = w = h = 0;  // the middle 60 % of the picture
    InstallOverlay(true);  // the present hook, in a game where nothing drew yet
    std::lock_guard<std::mutex> lk(g_shotMx);
    g_burstDir = dir;
    g_burstNo = 0;
    g_burstLeft = (std::min)(frames, 600);
    g_burstBox = {static_cast<UINT>(x), static_cast<UINT>(y), 0, static_cast<UINT>(x + w), static_cast<UINT>(y + h), 1};
    return true;
}
int UiShotBurstDone() { return g_burstDone.load(); }

// Public overlay-draw API (plugin interface v10). RegisterOverlayDraw adds a per-frame callback; the Draw*
// helpers are the immediate-mode primitives, only effective while a callback is running (see g_recording).
// A plugin is unloaded: its present and overlay callbacks stop (their code lies in [lo, hi)).
int DropPresentIn(uintptr_t lo, uintptr_t hi) {
    int n = 0;
    for (auto& c : g_presentCbs) {
        uintptr_t f = reinterpret_cast<uintptr_t>(c.cb);
        if (!c.dead && f >= lo && f < hi) c.dead = true, ++n;
    }
    for (auto& c : g_overlayDrawCbs) {
        uintptr_t f = reinterpret_cast<uintptr_t>(c.cb);
        if (!c.dead && f >= lo && f < hi) c.dead = true, ++n;
    }
    return n;
}

void RegisterOverlayDraw(TW3SE_OverlayDrawCallback cb, void* ctx) {
    if (!cb) return;
    g_overlayDrawCbs.push_back({cb, ctx, false});
    InstallOverlay(true);  // a plugin draws: the present hook goes in now
}
void OverlayApiText(float x, float y, uint32_t rgba, const char* text) { ApiDrawText(x, y, rgba, text); }
void OverlayTextSize(const char* text, float* w, float* h) {  // the cells are fixed: 0 before the first frame
    if (w) *w = static_cast<float>(g_cellW) * static_cast<float>(text ? strlen(text) : 0);
    if (h) *h = static_cast<float>(g_cellH);
}
void OverlayApiRect(float x, float y, float w, float h, uint32_t rgba) { ApiDrawRect(x, y, w, h, rgba); }
void OverlayApiScreenSize(float* w, float* h) {
    if (w) *w = static_cast<float>(g_ovW);
    if (h) *h = static_cast<float>(g_ovH);
}

// Installs the present hook via a throwaway swap chain's vtable. Hooks both Present (slot 8) and Present1
// (slot 22); the game uses one of them, and the first call logs which. Safe no-op if D3D12 is unavailable.
// Only when something needs it (the game can end at a resolution change made in its settings, right after this
// hook's ResizeBuffers; so no hook into the swap chain unless a plugin draws or
// presents, `overlaytest` is used, or tw3se.config says `overlay = on`). `overlay = off`: never.
bool InstallOverlay(bool needed) {
    static bool installed = false, command = false, hooked = false;
    if (!command) {  // the console command is there either way: `overlaytest on` installs the hook
        command = true;
        RegisterCommand("overlaytest", &CmdOverlayTest, "overlaytest on|draw|off: on=init only, draw=dev watermark");
        RegisterCommand("uishot", &CmdUiShot, "uishot <file.png>: the game's picture with the overlay and the UI");
    }
    if (installed) return hooked;
    std::string mode = ConfigGet("overlay", "auto");
    if (mode == "off" || mode == "0" || mode == "false" || mode == "no") {
        static bool told = false;
        if (!told) Log("overlay: off (config)");
        told = true;
        return false;
    }
    if (!needed && !(mode == "on" || mode == "1" || mode == "true" || mode == "yes")) {
        Log("overlay: not installed (no plugin draws; `overlay = on` in tw3se.config installs it at start)");
        return false;
    }
    installed = true;
    void** vtable = nullptr;
    void** queueVtable = nullptr;
    if (!GetSwapChainVTable(&vtable, &queueVtable) || !vtable) {
        Log("overlay: could not create a dummy swap chain - present hook not installed");
        return false;
    }
    void* present = vtable[8];    // IDXGISwapChain::Present
    void* present1 = vtable[22];  // IDXGISwapChain1::Present1
    void* resize = vtable[13];    // IDXGISwapChain::ResizeBuffers
    bool any = false;
    if (present && Hook(present, reinterpret_cast<void*>(&PresentHook), reinterpret_cast<void**>(&g_origPresent)))
        any = true;
    if (present1 && Hook(present1, reinterpret_cast<void*>(&Present1Hook), reinterpret_cast<void**>(&g_origPresent1)))
        any = true;
    if (resize)
        Hook(resize, reinterpret_cast<void*>(&ResizeBuffersHook), reinterpret_cast<void**>(&g_origResize));
    if (g_resize1Slot && g_resize1Slot != resize)
        Hook(g_resize1Slot, reinterpret_cast<void*>(&ResizeBuffers1Hook), reinterpret_cast<void**>(&g_origResize1));
    Log("overlay: present hook %s (Present=%p Present1=%p Resize=%p Resize1=%p)", any ? "installed" : "FAILED",
        present, present1, resize, g_resize1Slot);
    // (IDXGIFactory::CreateSwapChain is slot 10, IDXGIFactory2::CreateSwapChainForHwnd slot 15)
    if (g_factoryVtable) {
        bool a = g_factoryVtable[10] && Hook(g_factoryVtable[10], reinterpret_cast<void*>(&CreateSwapChainHook),
                                             reinterpret_cast<void**>(&g_origCreateSc));
        bool b = g_factoryVtable[15] && Hook(g_factoryVtable[15], reinterpret_cast<void*>(&CreateSwapChainForHwndHook),
                                             reinterpret_cast<void**>(&g_origCreateScHwnd));
        Log("overlay: new swap chains %s (CreateSwapChain %s, ForHwnd %s)", a || b ? "watched" : "NOT watched",
            a ? "hooked" : "not", b ? "hooked" : "not");
    }

    // Capture the game's command queue (needed by a D3D12 overlay renderer). ExecuteCommandLists is vtable
    // slot 10 of ID3D12CommandQueue (IUnknown 0-2, ID3D12Object 3-6, GetDevice 7, UpdateTileMappings 8,
    // CopyTileMappings 9, ExecuteCommandLists 10).
    if (queueVtable) {
        void* exec = queueVtable[10];
        if (exec && Hook(exec, reinterpret_cast<void*>(&ExecHook), reinterpret_cast<void**>(&g_origExec)))
            Log("overlay: ExecuteCommandLists hooked (%p) - awaiting first queue submit", exec);
        else
            Log("overlay: could not hook ExecuteCommandLists - overlay renderer will have no queue");
    }

    // The overlay renderer runs as a per-present callback; a plugin's OnOverlayDraw turns it on by itself.
    RegisterPresent(&OverlayPresentCb, nullptr);
    hooked = any;
    return hooked;
}

// w3.view.rect (lua.cpp): on with a rectangle in screen pixels, or off. Any thread.
void OverlayViewRect(bool on, float x, float y, float w, float h) {
    std::lock_guard<std::mutex> lk(g_viewMx);
    g_viewOn = on;
    g_viewX = x, g_viewY = y, g_viewW = w, g_viewH = h;
}
// Where the picture lies now (the rectangle at the picture's aspect) and the screen's size; false when it is off.
bool OverlayViewGet(float* x, float* y, float* w, float* h, float* screenW, float* screenH) {
    *screenW = static_cast<float>(g_ovW), *screenH = static_cast<float>(g_ovH);
    std::lock_guard<std::mutex> lk(g_viewMx);
    if (!g_viewOn || !g_ovW || !g_ovH) return false;
    *x = g_viewX, *y = g_viewY, *w = g_viewW, *h = g_viewH;
    ViewFit(*x, *y, *w, *h);
    return true;
}

void OverlayWhyNoDraw(char* out, size_t size) {
    snprintf(out, size, "present hook %s, %u presents of the game, %u of other windows, %u overlay calls, the overlay: %s",
             g_origPresent || g_origPresent1 ? "in" : "not in", g_presentsGame.load(), g_presentsOther.load(), g_ovCalls.load(), g_ovWhy.load());
}

// The game's device and a back buffer (native, borrowed), null before the overlay has seen a present.
void* OverlayDevice() { return g_dev; }
void* OverlayBackBuffer() { return g_backbuf.empty() ? nullptr : g_backbuf[0]; }

}  // namespace tw3se
