// TW3SE core - pixels written into a loaded texture (interface 26, TextureWrite), docs/FINDINGS_TEXWRITE.md.
//
// Where the pixels are (build 6adfbd7a): a CBitmapTexture's render texture holds at +0x50 the id of its resident
// texture in the game's texture pool: entry(id) = device data + 0x7dfc0 + (id - 1) * 0x140, the ID3D12Resource at
// +0x48. A texture of a streamed group is resident at 32 x 32 only; one cooked in GUIWithAlpha is resident in full.
//
// How: TextureWrite (main thread) finds the resource, checks its size and format and hands the pixels to a worker
// thread, which makes the missing mips (box filter) and encodes them (BC3, or copies RGBA / BGRA). The finished job
// waits for the next submit of the game's queue (blendshape.cpp's SubmitWork): TexRecord finds the resource again
// (the id can change), copies the job into an upload buffer and records the copy into the texture, between two
// barriers from and back to the texture's state, ahead of the game's lists. A newer write of a texture drops an older
// one that still waits.
//
// TexProbe (w3._tex_probe(depot path, out file)): the render texture's first 0x200 bytes, the pool entries of its ids
// and every D3D12 texture whose pointer lies in the device data, with its offset there, size, format and mips.
#include "core.h"

#include <d3d12.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tw3se {

void* BitmapRenderTexture(void* texture);
uintptr_t GpuDeviceData();
void* OverlayBackBuffer();

namespace {

constexpr uintptr_t kTexEntries = 0x7dfc0, kTexStride = 0x140, kTexResource = 0x48, kRtId = 0x50;

bool DescOf(ID3D12Resource* r, D3D12_RESOURCE_DESC* d) {
    __try {
        *d = r->GetDesc();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The resident texture of a render texture: its resource (borrowed) and description, null if there is none.
ID3D12Resource* Resident(void* rt, D3D12_RESOURCE_DESC* d) {
    uintptr_t dd = GpuDeviceData(), res = 0;
    uint32_t id = 0;
    if (!dd || !rt || !SafeRead(static_cast<char*>(rt) + kRtId, &id, 4) || !id || id >= 0x100000 ||
        !SafeRead(reinterpret_cast<void*>(dd + kTexEntries + (id - 1ull) * kTexStride + kTexResource), &res, 8) || !res)
        return nullptr;
    auto* r = reinterpret_cast<ID3D12Resource*>(res);
    if (!DescOf(r, d) || d->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return nullptr;
    return r;
}

enum class Kind { BC3, RGBA, BGRA, None };
Kind KindOf(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_BC3_UNORM: case DXGI_FORMAT_BC3_UNORM_SRGB: case DXGI_FORMAT_BC3_TYPELESS: return Kind::BC3;
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return Kind::RGBA;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return Kind::BGRA;
    default: return Kind::None;
    }
}

// --- BC3: per 4 x 4 block 8 bytes alpha (two ends, 3-bit indices) and 8 bytes colour (two 565 ends, 2-bit indices)
uint16_t To565(const int* c) { return static_cast<uint16_t>(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 | ((c[2] * 31 + 127) / 255)); }
void From565(uint16_t v, int* c) {
    c[0] = ((v >> 11) & 31) * 255 / 31;
    c[1] = ((v >> 5) & 63) * 255 / 63;
    c[2] = (v & 31) * 255 / 31;
}

void EncodeBlock(const uint8_t px[16][4], uint8_t* out) {
    // alpha: the block's lowest and highest, 8 steps between (a0 > a1)
    int amin = 255, amax = 0;
    for (int i = 0; i < 16; ++i) amin = (std::min)(amin, int(px[i][3])), amax = (std::max)(amax, int(px[i][3]));
    out[0] = static_cast<uint8_t>(amax);
    out[1] = static_cast<uint8_t>(amin);
    uint64_t abits = 0;
    if (amax > amin) {
        int pal[8] = {amax, amin};
        for (int k = 1; k < 7; ++k) pal[k + 1] = ((7 - k) * amax + k * amin) / 7;
        for (int i = 0; i < 16; ++i) {
            int best = 0, bd = 1 << 30;
            for (int k = 0; k < 8; ++k) {
                int dd = std::abs(pal[k] - px[i][3]);
                if (dd < bd) bd = dd, best = k;
            }
            abits |= static_cast<uint64_t>(best) << (3 * i);
        }
    }
    for (int k = 0; k < 6; ++k) out[2 + k] = static_cast<uint8_t>(abits >> (8 * k));
    // colour: the box of the block's colours, pulled in by a sixteenth, the ends as 565, 4 steps (c0 > c1)
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int i = 0; i < 16; ++i)
        for (int k = 0; k < 3; ++k) lo[k] = (std::min)(lo[k], int(px[i][k])), hi[k] = (std::max)(hi[k], int(px[i][k]));
    for (int k = 0; k < 3; ++k) {
        int inset = (hi[k] - lo[k]) / 16;
        lo[k] += inset, hi[k] -= inset;
    }
    uint16_t c0 = To565(hi), c1 = To565(lo);
    uint32_t cbits = 0;
    if (c0 < c1) std::swap(c0, c1);
    if (c0 != c1) {
        int pal[4][3];
        From565(c0, pal[0]);
        From565(c1, pal[1]);
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
            pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
        }
        for (int i = 0; i < 16; ++i) {
            int best = 0, bd = 1 << 30;
            for (int k = 0; k < 4; ++k) {
                int dr = pal[k][0] - px[i][0], dg = pal[k][1] - px[i][1], db = pal[k][2] - px[i][2];
                int dd = dr * dr + dg * dg + db * db;
                if (dd < bd) bd = dd, best = k;
            }
            cbits |= static_cast<uint32_t>(best) << (2 * i);
        }
    }
    memcpy(out + 8, &c0, 2);
    memcpy(out + 10, &c1, 2);
    memcpy(out + 12, &cbits, 4);
}

// One level of RGBA (w x h) in the texture's format, rows packed (BC3: rows of 4 x 4 blocks).
std::vector<uint8_t> EncodeLevel(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, Kind kind) {
    if (kind == Kind::RGBA) return rgba;
    if (kind == Kind::BGRA) {
        std::vector<uint8_t> o(rgba);
        for (size_t i = 0; i + 3 < o.size(); i += 4) std::swap(o[i], o[i + 2]);
        return o;
    }
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    std::vector<uint8_t> o(16ull * bw * bh);
    uint8_t px[16][4];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            for (int i = 0; i < 16; ++i) {
                uint32_t x = (std::min)(bx * 4 + (i & 3), w - 1), y = (std::min)(by * 4 + (i >> 2), h - 1);
                memcpy(px[i], &rgba[4ull * (y * w + x)], 4);
            }
            EncodeBlock(px, &o[16ull * (by * bw + bx)]);
        }
    return o;
}

std::vector<uint8_t> HalfSize(const std::vector<uint8_t>& src, uint32_t w, uint32_t h) {
    uint32_t nw = (std::max)(1u, w / 2), nh = (std::max)(1u, h / 2);
    std::vector<uint8_t> o(4ull * nw * nh);
    for (uint32_t y = 0; y < nh; ++y)
        for (uint32_t x = 0; x < nw; ++x)
            for (int k = 0; k < 4; ++k) {
                uint32_t x0 = (std::min)(2 * x, w - 1), x1 = (std::min)(2 * x + 1, w - 1);
                uint32_t y0 = (std::min)(2 * y, h - 1), y1 = (std::min)(2 * y + 1, h - 1);
                int s = src[4ull * (y0 * w + x0) + k] + src[4ull * (y0 * w + x1) + k] + src[4ull * (y1 * w + x0) + k] +
                        src[4ull * (y1 * w + x1) + k];
                o[4ull * (y * nw + x) + k] = static_cast<uint8_t>((s + 2) / 4);
            }
    return o;
}

struct Job {
    void* rt = nullptr;            // the render texture (its CBitmapTexture is held by DecalTexture's cache)
    std::string path;
    uint32_t w = 0, h = 0, mips = 0;  // mips: the resource's
    Kind kind = Kind::None;
    std::vector<std::vector<uint8_t>> levels;  // RGBA as given (worker: all mips); after encoding: the texture's bytes
    uint64_t seq = 0;
};

std::mutex g_jobMx;
std::condition_variable g_jobCv;
std::map<void*, std::unique_ptr<Job>> g_waiting;  // given, not yet encoded (newest per texture)
std::map<void*, std::unique_ptr<Job>> g_ready;    // encoded, for the next submit (newest per texture)
uint64_t g_seq = 0, g_written = 0, g_dropped = 0, g_encodeUs = 0;
bool g_worker = false;

void Worker() {
    for (;;) {
        std::unique_ptr<Job> j;
        {
            std::unique_lock<std::mutex> l(g_jobMx);
            g_jobCv.wait(l, [] { return !g_waiting.empty(); });
            j = std::move(g_waiting.begin()->second);
            g_waiting.erase(g_waiting.begin());
        }
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        uint32_t w = j->w, h = j->h, pw = w, ph = h;  // this level's size, the level above's
        std::vector<std::vector<uint8_t>> out;
        for (uint32_t m = 0; m < j->mips; ++m) {
            if (m >= j->levels.size()) j->levels.push_back(HalfSize(j->levels[m - 1], pw, ph));
            if (j->levels[m].size() != 4ull * w * h) break;  // (never: the levels are made to this size)
            out.push_back(EncodeLevel(j->levels[m], w, h, j->kind));
            pw = w, ph = h;
            w = (std::max)(1u, w / 2), h = (std::max)(1u, h / 2);
        }
        if (out.size() != j->mips) {
            Log("texwrite: %s: a level of the wrong size (dropped)", j->path.c_str());
            continue;
        }
        j->levels = std::move(out);
        QueryPerformanceCounter(&t1);
        std::lock_guard<std::mutex> l(g_jobMx);
        g_encodeUs += static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
        auto it = g_ready.find(j->rt);
        if (it != g_ready.end() && it->second->seq > j->seq) continue;  // a newer one was faster
        if (it != g_ready.end()) ++g_dropped;
        g_ready[j->rt] = std::move(j);
    }
}

D3D12_RESOURCE_STATES TexState() {
    static D3D12_RESOURCE_STATES s = static_cast<D3D12_RESOURCE_STATES>(
        strtoul(ConfigGet("texwrite_state", "0xc0").c_str(), nullptr, 0));  // pixel + non-pixel shader resource
    return s;
}

}  // namespace

// interface 26: the pixels of a loaded texture from the next frame on (see the top of the file). Main thread.
bool TextureWrite(const char* path, const void* rgba8, int w, int h, int mips, std::string* err) {
    if (!path || !rgba8 || w <= 0 || h <= 0 || w > 4096 || h > 4096 || mips < 1) {
        *err = "no path or pixels, or a size outside 1 to 4096";
        return false;
    }
    if (!OnMainThread()) {
        *err = "main thread only (RunOnMainThread)";
        return false;
    }
    void* tex = DecalTexture(path, *err);
    if (!tex) return false;
    void* rt = BitmapRenderTexture(tex);
    D3D12_RESOURCE_DESC d = {};
    if (!Resident(rt, &d)) {
        *err = "the texture has no resident picture";
        return false;
    }
    if (d.Width != static_cast<uint64_t>(w) || d.Height != static_cast<uint32_t>(h)) {
        *err = "the texture is resident at " + std::to_string(d.Width) + " x " + std::to_string(d.Height) + ", not " +
               std::to_string(w) + " x " + std::to_string(h) + " (a streamed texture group? GUIWithAlpha stays resident)";
        return false;
    }
    Kind kind = KindOf(d.Format);
    if (kind == Kind::None || d.DepthOrArraySize != 1) {
        *err = "the texture's format is not BC3, RGBA8 or BGRA8 (DXGI " + std::to_string(d.Format) + ")";
        return false;
    }
    auto j = std::make_unique<Job>();
    j->rt = rt;
    j->path = path;
    j->w = w, j->h = h, j->mips = d.MipLevels;
    j->kind = kind;
    const uint8_t* p = static_cast<const uint8_t*>(rgba8);
    uint32_t lw = w, lh = h;
    for (int m = 0; m < mips && m < d.MipLevels; ++m) {
        j->levels.emplace_back(p, p + 4ull * lw * lh);
        p += 4ull * lw * lh;
        lw = (std::max)(1u, lw / 2), lh = (std::max)(1u, lh / 2);
    }
    InstallOverlay(true);  // its ExecuteCommandLists hook copies the pixels in (installed on first use)
    std::lock_guard<std::mutex> l(g_jobMx);
    j->seq = ++g_seq;
    if (g_waiting.count(rt)) ++g_dropped;
    g_waiting[rt] = std::move(j);
    if (!g_worker) {
        g_worker = true;
        std::thread(&Worker).detach();
    }
    g_jobCv.notify_one();
    return true;
}

bool TexWritesPending() {
    std::lock_guard<std::mutex> l(g_jobMx);
    return !g_ready.empty();
}

// On the render thread inside blendshape.cpp's SubmitWork (under its lock): each finished job copied into its
// texture. Objects the list uses go to `keep` (released once the list is done).
bool TexRecord(void* list, void* device, std::vector<IUnknown*>* keep) {
    auto* l = static_cast<ID3D12GraphicsCommandList*>(list);
    auto* dev = static_cast<ID3D12Device*>(device);
    std::map<void*, std::unique_ptr<Job>> jobs;
    {
        std::lock_guard<std::mutex> lk(g_jobMx);
        jobs.swap(g_ready);
    }
    bool any = false;
    for (auto& e : jobs) {
        Job& j = *e.second;
        D3D12_RESOURCE_DESC d = {};
        ID3D12Resource* res = Resident(j.rt, &d);
        if (!res || d.Width != j.w || d.Height != j.h || d.MipLevels != j.mips || KindOf(d.Format) != j.kind) {
            Log("texwrite: %s changed before its write (dropped)", j.path.c_str());
            continue;
        }
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(j.mips);
        std::vector<UINT> rows(j.mips);
        std::vector<UINT64> rowBytes(j.mips);
        UINT64 total = 0;
        dev->GetCopyableFootprints(&d, 0, j.mips, 0, fp.data(), rows.data(), rowBytes.data(), &total);
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* up = nullptr;
        uint8_t* map = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&up))) ||
            FAILED(up->Map(0, nullptr, reinterpret_cast<void**>(&map)))) {
            if (up) up->Release();
            Log("texwrite: %s: upload buffer failed (%llu bytes)", j.path.c_str(), static_cast<unsigned long long>(total));
            continue;
        }
        for (uint32_t m = 0; m < j.mips; ++m) {
            const std::vector<uint8_t>& src = j.levels[m];
            size_t pitch = src.size() / (std::max)(1u, rows[m]);
            for (UINT r = 0; r < rows[m]; ++r)
                memcpy(map + fp[m].Offset + static_cast<UINT64>(r) * fp[m].Footprint.RowPitch, src.data() + r * pitch,
                       (std::min)(static_cast<size_t>(rowBytes[m]), pitch));
        }
        up->Unmap(0, nullptr);
        res->AddRef();
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = TexState();
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        l->ResourceBarrier(1, &b);
        for (uint32_t m = 0; m < j.mips; ++m) {
            D3D12_TEXTURE_COPY_LOCATION dst = {res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            dst.SubresourceIndex = m;
            D3D12_TEXTURE_COPY_LOCATION srcLoc = {up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            srcLoc.PlacedFootprint = fp[m];
            l->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
        }
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        l->ResourceBarrier(1, &b);
        keep->push_back(up);
        keep->push_back(res);
        any = true;
        std::lock_guard<std::mutex> lk(g_jobMx);
        ++g_written;
    }
    return any;
}

std::string TexStats() {
    std::lock_guard<std::mutex> l(g_jobMx);
    char b[200];
    snprintf(b, sizeof b, "%llu written, %llu dropped for a newer one, %llu us encoding in all, %zu waiting, %zu ready",
             static_cast<unsigned long long>(g_written), static_cast<unsigned long long>(g_dropped),
             static_cast<unsigned long long>(g_encodeUs), g_waiting.size(), g_ready.size());
    return b;
}

// w3._tex_readback(depot path, file): the resident texture's top level as the GPU holds it, raw bytes (BC3 blocks or
// pixels, rows packed) into the file. Its own queue and list, waited for; for tests only.
std::string TexReadback(const char* path, const std::wstring& out) {
    std::string err;
    void* tex = DecalTexture(path, err);
    if (!tex) return err;
    D3D12_RESOURCE_DESC d = {};
    ID3D12Resource* res = Resident(BitmapRenderTexture(tex), &d);
    if (!res) return "no resident texture";
    ID3D12Device* dev = nullptr;
    if (FAILED(res->GetDevice(IID_PPV_ARGS(&dev)))) return "no device";
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &total);
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* a = nullptr;
    ID3D12GraphicsCommandList* l = nullptr;
    ID3D12Fence* f = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    std::string r = "failed";
    if (SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&rb))) &&
        SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) &&
        SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))) &&
        SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l))) &&
        SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)))) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = TexState();
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        l->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION src = {res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION dst = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = fp;
        l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        l->ResourceBarrier(1, &b);
        l->Close();
        ID3D12CommandList* lists[] = {l};
        q->ExecuteCommandLists(1, lists);
        q->Signal(f, 1);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        f->SetEventOnCompletion(1, ev);
        bool done = WaitForSingleObject(ev, 2000) == WAIT_OBJECT_0;
        CloseHandle(ev);
        uint8_t* m = nullptr;
        FILE* o = _wfopen(out.c_str(), L"wb");
        if (done && o && SUCCEEDED(rb->Map(0, nullptr, reinterpret_cast<void**>(&m)))) {
            for (UINT y = 0; y < rows; ++y) fwrite(m + fp.Offset + static_cast<UINT64>(y) * fp.Footprint.RowPitch, 1, rowBytes, o);
            rb->Unmap(0, nullptr);
            r = "written: " + std::to_string(d.Width) + " x " + std::to_string(d.Height) + ", format " +
                std::to_string(d.Format) + ", " + std::to_string(rows) + " rows of " + std::to_string(rowBytes) + " bytes";
        } else {
            r = done ? "cannot write the file" : "the copy did not finish in 2 s";
        }
        if (o) fclose(o);
    }
    for (IUnknown* u : std::initializer_list<IUnknown*>{f, l, a, q, rb, dev})
        if (u) u->Release();
    return r;
}

// A test picture (w3._tex_test): rings that move with `t`, alpha falling off from the middle, RGBA, top level only.
std::vector<uint8_t> TexTestPicture(int w, int h, float t) {
    std::vector<uint8_t> p(4ull * w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float dx = (x - w / 2.0f) / w, dy = (y - h / 2.0f) / h, r = std::sqrt(dx * dx + dy * dy);
            uint8_t* q = &p[4ull * (y * w + x)];
            float ring = 0.5f + 0.5f * std::sin(r * 60.0f - t * 6.0f);
            q[0] = static_cast<uint8_t>(255 * ring);
            q[1] = static_cast<uint8_t>(64 + 128 * (x * 4 / w % 2));
            q[2] = static_cast<uint8_t>(255 * (1 - ring));
            q[3] = static_cast<uint8_t>(r < 0.5f ? 255 * (1 - 2 * r) : 0);
        }
    return p;
}

std::string TexProbe(const char* path, const std::wstring& out) {
    std::string err;
    void* tex = DecalTexture(path, err);
    if (!tex) return err;
    void* rt = BitmapRenderTexture(tex);
    uintptr_t dd = GpuDeviceData();
    void* bb = OverlayBackBuffer();
    uintptr_t vt = 0;
    if (!rt) return "no render texture";
    if (!dd) return "no device data";
    if (!bb || !SafeRead(bb, &vt, 8)) return "no back buffer seen yet";
    FILE* f = _wfopen(out.c_str(), L"w");
    if (!f) return "cannot write the file";
    fprintf(f, "texture %p render %p dd %p vtable %p\n", tex, rt, reinterpret_cast<void*>(dd), reinterpret_cast<void*>(vt));
    for (int i = 0; i < 0x200; i += 8) {
        uint64_t q = 0;
        SafeRead(static_cast<char*>(rt) + i, &q, 8);
        fprintf(f, "rt +%03x %016llx\n", i, static_cast<unsigned long long>(q));
    }
    // the texture pool (build 6adfbd7a): entry(id) = dd + 0x7dfc0 + (id - 1) * 0x140, the resource at +0x48;
    // the ids at +0x50 (resident) and +0x5c (streamed, a guess)
    for (int off : {0x50, 0x5c}) {
        uint32_t id = 0;
        uintptr_t res = 0;
        D3D12_RESOURCE_DESC d = {};
        SafeRead(static_cast<char*>(rt) + off, &id, 4);
        bool ok = id && id < 0x100000 && SafeRead(reinterpret_cast<void*>(dd + 0x7dfc0 + (id - 1ull) * 0x140 + 0x48), &res, 8) &&
                  res && DescOf(reinterpret_cast<ID3D12Resource*>(res), &d);
        fprintf(f, "id+%02x %u resource %p %llux%u mips %u fmt %d%s\n", off, id, reinterpret_cast<void*>(res),
                static_cast<unsigned long long>(d.Width), d.Height, d.MipLevels, d.Format, ok ? "" : " (none)");
    }
    // the device data: committed regions from dd on, up to 512 MB
    int found = 0;
    uintptr_t at = dd, end = dd + 0x20000000;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mi = {};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &mi, sizeof mi)) break;
        uintptr_t lo = reinterpret_cast<uintptr_t>(mi.BaseAddress), hi = lo + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mi.Protect & 0xee)) {
            static uint64_t page[512];
            for (uintptr_t a = (std::max)(lo, at); a + 8 <= hi; a += 8) {
                if (((a & 0xfff) == 0 || a == (std::max)(lo, at)) &&
                    !SafeRead(reinterpret_cast<void*>(a & ~0xfffull), page, sizeof page)) {
                    a = (a & ~0xfffull) + 0x1000 - 8;
                    continue;
                }
                uint64_t q = page[(a & 0xfff) / 8];
                if (q < 0x10000 || q > 0x7fffffffffffULL || (q & 7)) continue;
                uintptr_t v = 0;
                if (!SafeRead(reinterpret_cast<void*>(q), &v, 8) || v != vt) continue;
                D3D12_RESOURCE_DESC d;
                if (!DescOf(reinterpret_cast<ID3D12Resource*>(q), &d)) continue;
                if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) continue;
                fprintf(f, "res +%llx %p dim %d %llux%u depth %u mips %u fmt %d\n",
                        static_cast<unsigned long long>(a - dd), reinterpret_cast<void*>(q), d.Dimension,
                        static_cast<unsigned long long>(d.Width), d.Height, d.DepthOrArraySize, d.MipLevels, d.Format);
                ++found;
            }
        }
        at = hi;
    }
    fclose(f);
    return "written, " + std::to_string(found) + " textures";
}

}  // namespace tw3se
