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
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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

// --- pinned textures (w3.texture.write, 26.11.): the levels of a DDS of the texture's own block format, copied block
// for block into the texture's resident level (render texture +0x50, 32 x 32) and its streamed one (+0x54: the size
// the streaming has it at, 2048 x 2048 of a 4096 face on 5.02), and again whenever the streaming gives it a new
// resource. Measured on 5.02: the pool is the same as on 5.01 (entries +0x7dfc0, 0x140 each, the resource at +0x48).
namespace {

constexpr uint32_t kPinIds[2] = {kRtId, 0x54};
constexpr int kAgainFrames = 30;  // a new streamed resource is written once more after that many frames (its upload
                                  // from the game's copy queue may land after ours)

// The block format of a DXGI format: 1 BC1, 2 BC2, 3 BC3, 4 BC4, 5 BC5, 6 BC6H, 7 BC7, 8 RGBA8, 9 BGRA8; 0 other.
int Family(DXGI_FORMAT f) {
    if (f >= 70 && f <= 72) return 1;
    if (f >= 73 && f <= 75) return 2;
    if (f >= 76 && f <= 78) return 3;
    if (f >= 79 && f <= 81) return 4;
    if (f >= 82 && f <= 84) return 5;
    if (f >= 94 && f <= 96) return 6;
    if (f >= 97 && f <= 99) return 7;
    if (f >= 27 && f <= 32) return 8;
    if (f == 87 || f == 90 || f == 91) return 9;
    return 0;
}
const char* FamilyName(int fam) {
    static const char* n[] = {"other", "BC1", "BC2", "BC3", "BC4", "BC5", "BC6H", "BC7", "RGBA8", "BGRA8"};
    return fam >= 0 && fam <= 9 ? n[fam] : "other";
}
// The bytes of one level: rows of blocks (or pixels) and the bytes of a row, packed.
uint64_t LevelBytes(int fam, uint32_t w, uint32_t h, uint32_t* rows, uint32_t* rowBytes) {
    if (fam == 8 || fam == 9) {
        *rows = h, *rowBytes = 4 * w;
    } else {
        uint32_t bs = (fam == 1 || fam == 4) ? 8 : 16;
        *rows = (std::max)(1u, (h + 3) / 4), *rowBytes = (std::max)(1u, (w + 3) / 4) * bs;
    }
    return static_cast<uint64_t>(*rows) * *rowBytes;
}

struct Pin {
    void* rt = nullptr;
    std::string path;
    int fam = 0;
    uint32_t w = 0, h = 0;                   // the DDS's top level
    std::vector<std::vector<uint8_t>> mips;  // the DDS's levels, packed
    ID3D12Resource* last[2] = {};
    int again[2] = {};
    bool due[2] = {};
    bool warned[2] = {};
    uint64_t gen = 0;                        // a new one at each w3.texture.write (stamps start anew on it)
    bool part[2] = {};                       // only rect is due (stamps), not the whole texture
    uint32_t rect[2][4] = {};                // x0, y0, x1, y1 of the top level
    bool held = false;                       // the pin holds a reference on its render texture
};
std::map<void*, Pin> g_pins;  // render texture -> pin, under g_jobMx
// A pin holds its render texture (IRenderResource::AddRef), as a blend shape set holds its mesh: it knew the texture
// by address only, and another world frees the textures of the old one - the render thread read every pin's id from
// freed memory at each present and the game fell (27.11., tw3se.dll in TexWritesPending, or later in the game's own
// code). A pin of the same path on another render texture (the texture loaded anew) replaces the old one. The
// references go back from the main thread (to the render thread, ReleaseOnRenderThread), held here until then.
std::vector<void*> g_rtRelease;  // under g_jobMx
bool HoldRt(void* rt) {
    auto add = reinterpret_cast<void (*)(void*)>(Address("IRenderResource::AddRef"));
    if (!add || !Address("IRenderResource::Release")) return false;
    __try {
        add(rt);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Under g_jobMx: the other pins of `path` go (their references to g_rtRelease); whether rt's own pin holds it.
bool DropOtherPins(void* rt, const std::string& path) {
    bool held = false;
    for (auto it = g_pins.begin(); it != g_pins.end();) {
        if (it->first == rt) {
            held = it->second.held;
            ++it;
        } else if (it->second.path == path) {
            if (it->second.held) g_rtRelease.push_back(it->first);
            it = g_pins.erase(it);
        } else {
            ++it;
        }
    }
    return held;
}
// Main thread: the references of pins that went.
void ReleaseDroppedRts() {
    std::vector<void*> list;
    {
        std::lock_guard<std::mutex> l(g_jobMx);
        list.swap(g_rtRelease);
    }
    // (the texture's own Release, on the render thread: IRenderResource::Release skips the unlink from the renderer's
    // list of render resources - blendshape.cpp, ReleaseRenderMesh)
    for (void* rt : list)
        if (!ReleaseOnRenderThread(rt)) Log("texwrite: giving render texture %p back faulted (caught)", rt);
}
uint64_t g_pinWrites = 0, g_pinGen = 0, g_pinBytes = 0;

// A DDS file's levels: "" or why not.
std::string ParseDds(const std::vector<uint8_t>& b, Pin* p) {
    auto u32 = [&](size_t o) { uint32_t v = 0; memcpy(&v, b.data() + o, 4); return v; };
    if (b.size() < 128 || memcmp(b.data(), "DDS ", 4) != 0 || u32(4) != 124) return "not a DDS file";
    uint32_t h = u32(12), w = u32(16), mips = (std::max)(1u, u32(28)), pfFlags = u32(80), four = u32(84);
    size_t at = 128;
    int fam = 0;
    if (pfFlags & 4) {  // DDPF_FOURCC
        if (four == 0x31545844) fam = 1;                         // DXT1
        else if (four == 0x33545844) fam = 2;                    // DXT3
        else if (four == 0x35545844) fam = 3;                    // DXT5
        else if (four == 0x31495441 || four == 0x55344342) fam = 4;  // ATI1, BC4U
        else if (four == 0x32495441 || four == 0x55354342) fam = 5;  // ATI2, BC5U
        else if (four == 0x30315844) {                           // DX10
            if (b.size() < 148) return "a DDS with a cut header";
            fam = Family(static_cast<DXGI_FORMAT>(u32(128)));
            at = 148;
        }
    } else if (u32(88) == 32) {  // 32 bits a pixel: by the red mask
        fam = u32(92) == 0xff ? 8 : (u32(92) == 0xff0000 ? 9 : 0);
    }
    if (!fam) return "a DDS format TW3SE does not copy (BC1-BC7, RGBA8 or BGRA8)";
    if (!w || !h || w > 16384 || h > 16384) return "a DDS of a size outside 1 to 16384";
    p->fam = fam, p->w = w, p->h = h;
    uint32_t lw = w, lh = h;
    for (uint32_t m = 0; m < mips; ++m) {
        uint32_t rows = 0, rowBytes = 0;
        uint64_t n = LevelBytes(fam, lw, lh, &rows, &rowBytes);
        if (at + n > b.size()) return "the DDS is shorter than its levels";
        p->mips.emplace_back(b.begin() + at, b.begin() + at + n);
        at += n;
        lw = (std::max)(1u, lw / 2), lh = (std::max)(1u, lh / 2);
    }
    return "";
}

// The texture of one of the render texture's ids: the resource (borrowed) and its description, null if none.
ID3D12Resource* ResAt(void* rt, uint32_t off, D3D12_RESOURCE_DESC* d) {
    uintptr_t dd = GpuDeviceData(), res = 0;
    uint32_t id = 0;
    if (!dd || !rt || !SafeRead(static_cast<char*>(rt) + off, &id, 4) || !id || id >= 0x100000 ||
        !SafeRead(reinterpret_cast<void*>(dd + kTexEntries + (id - 1ull) * kTexStride + kTexResource), &res, 8) || !res)
        return nullptr;
    auto* r = reinterpret_cast<ID3D12Resource*>(res);
    if (!DescOf(r, d) || d->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return nullptr;
    return r;
}

// Render thread: a pin's slot written into its resource (the levels that fit its size), between barriers.
bool PinRecord(Pin& p, int s, ID3D12Resource* res, const D3D12_RESOURCE_DESC& d, ID3D12GraphicsCommandList* l,
               ID3D12Device* dev, std::vector<IUnknown*>* keep, const uint32_t* rect) {
    uint32_t k = 0;  // the DDS level the resource's top level is
    while (k < p.mips.size() && ((std::max)(1u, p.w >> k) != d.Width || (std::max)(1u, p.h >> k) != d.Height)) ++k;
    if (Family(d.Format) != p.fam || k >= p.mips.size() || k + d.MipLevels > p.mips.size() || d.DepthOrArraySize != 1) {
        if (!p.warned[s]) {
            p.warned[s] = true;
            Log("texwrite: %s: its %s level is %llu x %u, %u mips, %s - the DDS (%u x %u, %zu mips, %s) does not fit it",
                p.path.c_str(), s ? "streamed" : "resident", static_cast<unsigned long long>(d.Width), d.Height,
                d.MipLevels, FamilyName(Family(d.Format)), p.w, p.h, p.mips.size(), FamilyName(p.fam));
        }
        return false;
    }
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(d.MipLevels);
    std::vector<UINT> rows(d.MipLevels);
    std::vector<UINT64> rowBytes(d.MipLevels);
    UINT64 total = 0;
    dev->GetCopyableFootprints(&d, 0, d.MipLevels, 0, fp.data(), rows.data(), rowBytes.data(), &total);
    // only a rectangle (a stamp moved): each level's part of it, whole 4 x 4 blocks; the small levels whole
    struct Box {
        uint32_t x0, y0, x1, y1;
        bool whole;
    };
    std::vector<Box> box(d.MipLevels);
    if (rect && p.fam == 1) {
        total = 0;
        for (uint32_t m = 0; m < d.MipLevels; ++m) {
            uint32_t lv = k + m, lw = (std::max)(1u, p.w >> lv), lh = (std::max)(1u, p.h >> lv);
            Box& bx = box[m];
            if (lw < 8 || lh < 8) {
                bx = {0, 0, lw, lh, true};
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT one;
                UINT nr;
                UINT64 rb, sz;
                dev->GetCopyableFootprints(&d, m, 1, (total + 511) & ~511ull, &one, &nr, &rb, &sz);
                fp[m] = one, rows[m] = nr, rowBytes[m] = rb;
                total = one.Offset + sz;
                continue;
            }
            bx.x0 = (rect[0] >> lv) & ~3u, bx.y0 = (rect[1] >> lv) & ~3u;
            bx.x1 = (std::min)(lw, (((rect[2] + (1u << lv) - 1) >> lv) + 3) & ~3u);
            bx.y1 = (std::min)(lh, (((rect[3] + (1u << lv) - 1) >> lv) + 3) & ~3u);
            bx.whole = false;
            if (bx.x1 <= bx.x0 || bx.y1 <= bx.y0) bx = {0, 0, 4, 4, false};
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f = fp[m];
            f.Offset = (total + 511) & ~511ull;
            f.Footprint.Format = d.Format;
            f.Footprint.Width = bx.x1 - bx.x0, f.Footprint.Height = bx.y1 - bx.y0, f.Footprint.Depth = 1;
            rowBytes[m] = (bx.x1 - bx.x0) / 4 * 8ull;
            f.Footprint.RowPitch = static_cast<UINT>((rowBytes[m] + 255) & ~255ull);
            rows[m] = (bx.y1 - bx.y0) / 4;
            total = f.Offset + static_cast<UINT64>(f.Footprint.RowPitch) * rows[m];
        }
    } else {
        rect = nullptr;
    }
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
        Log("texwrite: %s: upload buffer failed (%llu bytes)", p.path.c_str(), static_cast<unsigned long long>(total));
        return false;
    }
    for (uint32_t m = 0; m < d.MipLevels; ++m) {
        const std::vector<uint8_t>& src = p.mips[k + m];
        if (rect && !box[m].whole) {
            uint32_t lw = (std::max)(1u, p.w >> (k + m));
            size_t pitch = ((lw + 3) / 4) * 8ull;
            for (UINT r = 0; r < rows[m]; ++r)
                memcpy(map + fp[m].Offset + static_cast<UINT64>(r) * fp[m].Footprint.RowPitch,
                       src.data() + (box[m].y0 / 4 + r) * pitch + box[m].x0 / 4 * 8ull, static_cast<size_t>(rowBytes[m]));
            continue;
        }
        size_t pitch = src.size() / (std::max)(1u, rows[m]);
        for (UINT r = 0; r < rows[m]; ++r)
            memcpy(map + fp[m].Offset + static_cast<UINT64>(r) * fp[m].Footprint.RowPitch, src.data() + r * pitch,
                   (std::min)(static_cast<size_t>(rowBytes[m]), pitch));
    }
    up->Unmap(0, nullptr);
    g_pinBytes += total;
    res->AddRef();
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = TexState();
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    l->ResourceBarrier(1, &b);
    for (uint32_t m = 0; m < d.MipLevels; ++m) {
        D3D12_TEXTURE_COPY_LOCATION dst = {res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        dst.SubresourceIndex = m;
        D3D12_TEXTURE_COPY_LOCATION srcLoc = {up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        srcLoc.PlacedFootprint = fp[m];
        bool part = rect && !box[m].whole;
        l->CopyTextureRegion(&dst, part ? box[m].x0 : 0, part ? box[m].y0 : 0, 0, &srcLoc, nullptr);
    }
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l->ResourceBarrier(1, &b);
    keep->push_back(up);
    keep->push_back(res);
    ++g_pinWrites;
    return true;
}

}  // namespace

// w3.texture.write: the DDS pinned on the texture (main thread). Its size has to be the texture's full size.
bool TexturePin(const char* path, const std::wstring& dds, std::string* err) {
    if (!OnMainThread()) {
        *err = "main thread only";
        return false;
    }
    std::vector<uint8_t> bytes;
    if (FILE* f = _wfopen(dds.c_str(), L"rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0) bytes.resize(static_cast<size_t>(n));
        if (n <= 0 || fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
        fclose(f);
    }
    if (bytes.empty()) {
        *err = "cannot read the DDS file";
        return false;
    }
    void* tex = DecalTexture(path, *err);
    if (!tex) return false;
    void* rt = BitmapRenderTexture(tex);
    if (!rt) {
        *err = "the texture has no render texture";
        return false;
    }
    Pin p;
    p.rt = rt;
    p.path = path;
    *err = ParseDds(bytes, &p);
    if (!err->empty()) return false;
    uint16_t full[2] = {};  // the full size: u16 width, u16 height at +0x9c (5.02: 0x1000, 0x1000 of a 4096 face)
    if (SafeRead(static_cast<char*>(rt) + 0x9c, full, 4) && full[0] && (full[0] != p.w || full[1] != p.h)) {
        *err = "the DDS is " + std::to_string(p.w) + " x " + std::to_string(p.h) + ", the texture " +
               std::to_string(full[0]) + " x " + std::to_string(full[1]);
        return false;
    }
    InstallOverlay(true);  // its ExecuteCommandLists hook records the copies (installed on first use)
    {
        std::lock_guard<std::mutex> l(g_jobMx);
        p.gen = ++g_pinGen;  // (a new DDS: its stamps are composed anew on it)
        p.held = DropOtherPins(rt, p.path) || HoldRt(rt);
        g_pins[rt] = std::move(p);
    }
    ReleaseDroppedRts();
    return true;
}

// w3.texture.release: the texture is no longer written; its pixels stay until the streaming loads it again.
bool TextureUnpin(const char* path) {
    std::string err;
    void* tex = DecalTexture(path, err);
    void* rt = tex ? BitmapRenderTexture(tex) : nullptr;
    bool had = false;
    {
        std::lock_guard<std::mutex> l(g_jobMx);
        auto it = rt ? g_pins.find(rt) : g_pins.end();
        if (it != g_pins.end()) {
            if (it->second.held) g_rtRelease.push_back(rt);
            g_pins.erase(it);
            had = true;
        }
    }
    ReleaseDroppedRts();
    return had;
}

// --- stamps on a pinned BC1 texture (w3.texture.stamp, 27.11., the character editor's tattoos and scars) ------------
// A stamp is a projector on a body: centre, normal, up, size (m), turn, opacity and a motif (RGBA: rgb the colour
// ratio * 128 - the skin's colour multiplied by it -, a the cover). Which texel lies where on the body comes from a
// position map (TWPM: per texel of the texture's UV the rest-pose place and normal, made offline from the meshes). Each
// call composes every given stamp anew on the pinned DDS's top level, only where the stamps were or are now: those
// 4 x 4 blocks are decoded from the DDS as pinned first, the stamps applied, encoded again (BC1), and the smaller
// levels redone over the same area; the pin then writes the texture again.
namespace {

struct PosMap {
    uint32_t w = 0, h = 0;
    std::vector<int16_t> pos;    // 3 per texel, quarter millimetres
    std::vector<int8_t> nrm;     // 3 per texel, -127..127; 0,0,0: no texel there
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;  // 2 cm cells -> texels
};
std::map<std::wstring, std::unique_ptr<PosMap>> g_posmaps;
constexpr float kCell = 0.02f;

uint64_t CellKey(int x, int y, int z) {
    return (static_cast<uint64_t>(x + 0x8000) << 32) | (static_cast<uint64_t>(y + 0x8000) << 16) |
           static_cast<uint64_t>(z + 0x8000);
}

PosMap* LoadPosMap(const std::wstring& file, std::string* err) {
    auto it = g_posmaps.find(file);
    if (it != g_posmaps.end()) return it->second.get();
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) {
        *err = "cannot read the position map";
        return nullptr;
    }
    char magic[4] = {};
    uint32_t wh[2] = {};
    auto pm = std::make_unique<PosMap>();
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "TWPM", 4) == 0 && fread(wh, 4, 2, f) == 2 && wh[0] &&
              wh[1] && wh[0] <= 8192 && wh[1] <= 8192;
    if (ok) {
        size_t n = static_cast<size_t>(wh[0]) * wh[1];
        pm->w = wh[0], pm->h = wh[1];
        pm->pos.resize(3 * n);
        pm->nrm.resize(3 * n);
        ok = fread(pm->pos.data(), 2, 3 * n, f) == 3 * n && fread(pm->nrm.data(), 1, 3 * n, f) == 3 * n;
    }
    fclose(f);
    if (!ok) {
        *err = "not a position map (TWPM)";
        return nullptr;
    }
    size_t n = static_cast<size_t>(pm->w) * pm->h;
    for (uint32_t i = 0; i < n; ++i) {
        const int8_t* nn = &pm->nrm[3ull * i];
        if (!nn[0] && !nn[1] && !nn[2]) continue;
        const int16_t* p = &pm->pos[3ull * i];
        int cx = static_cast<int>(std::floor(p[0] / 4000.0f / kCell)), cy = static_cast<int>(std::floor(p[1] / 4000.0f / kCell)),
            cz = static_cast<int>(std::floor(p[2] / 4000.0f / kCell));
        pm->grid[CellKey(cx, cy, cz)].push_back(i);
    }
    PosMap* out = pm.get();
    g_posmaps[file] = std::move(pm);
    return out;
}

struct Motif {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
};
std::map<std::wstring, std::unique_ptr<Motif>> g_motifs;

Motif* LoadMotif(const std::wstring& file, std::string* err) {  // TWMO: u32 w, u32 h, RGBA8 rows
    auto it = g_motifs.find(file);
    if (it != g_motifs.end()) return it->second.get();
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) {
        *err = "cannot read the motif";
        return nullptr;
    }
    char magic[4] = {};
    uint32_t wh[2] = {};
    auto m = std::make_unique<Motif>();
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "TWMO", 4) == 0 && fread(wh, 4, 2, f) == 2 && wh[0] &&
              wh[1] && wh[0] <= 4096 && wh[1] <= 4096;
    if (ok) {
        m->w = wh[0], m->h = wh[1];
        m->rgba.resize(4ull * m->w * m->h);
        ok = fread(m->rgba.data(), 1, m->rgba.size(), f) == m->rgba.size();
    }
    fclose(f);
    if (!ok) {
        *err = "not a motif (TWMO)";
        return nullptr;
    }
    Motif* out = m.get();
    g_motifs[file] = std::move(m);
    return out;
}

void DecodeBC1(const uint8_t* b, uint8_t out[16][4]) {
    uint16_t c0, c1;
    memcpy(&c0, b, 2);
    memcpy(&c1, b + 2, 2);
    int pal[4][3];
    From565(c0, pal[0]);
    From565(c1, pal[1]);
    for (int k = 0; k < 3; ++k) {
        if (c0 > c1) {
            pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
            pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
        } else {
            pal[2][k] = (pal[0][k] + pal[1][k]) / 2;
            pal[3][k] = 0;
        }
    }
    uint32_t bits;
    memcpy(&bits, b + 4, 4);
    for (int i = 0; i < 16; ++i) {
        int k = (bits >> (2 * i)) & 3;
        out[i][0] = static_cast<uint8_t>(pal[k][0]), out[i][1] = static_cast<uint8_t>(pal[k][1]);
        out[i][2] = static_cast<uint8_t>(pal[k][2]), out[i][3] = 255;
    }
}

// The stamps' worker (27.11.: moving a stamp in the editor lagged badly): w3.texture.stamp hands its layers over and
// returns at once; a thread of its own composes them, the newest request per texture winning (a held slider sends one
// a frame). Per layer it keeps the texels it covers and where in the motif each lies, so a change composes only the
// 4 x 4 blocks of the layers that changed (where they were and are now), on a few threads; the pin then uploads only
// the rectangle that changed. The game's own thread never waits for a stamp.

// a small pool: fn(begin, end) over [0, n) in chunks of `grain`, on its threads and the caller's
class StampPool {
public:
    explicit StampPool(int n) {
        for (int i = 1; i < n; ++i) std::thread([this] { Run(); }).detach();
    }
    void For(size_t n, const std::function<void(size_t, size_t)>& fn, size_t grain) {
        if (n == 0) return;
        {
            std::lock_guard<std::mutex> l(mx_);
            fn_.store(&fn);
            n_ = n;
            grain_ = grain > 0 ? grain : 1;
            next_.store(0);
            ++round_;
        }
        cv_.notify_all();
        Work();
        std::unique_lock<std::mutex> l(mx_);
        done_.wait(l, [&] { return busy_ == 0; });
        fn_.store(nullptr);
    }

private:
    void Work() {
        for (;;) {
            size_t g = grain_.load(), b = next_.fetch_add(g), n = n_.load();
            const std::function<void(size_t, size_t)>* fn = fn_.load();
            if (b >= n || !fn) return;
            (*fn)(b, (std::min)(n, b + g));
        }
    }
    void Run() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> l(mx_);
                cv_.wait(l, [&] { return round_ != seen; });
                seen = round_;
                ++busy_;
            }
            Work();
            {
                std::lock_guard<std::mutex> l(mx_);
                --busy_;
            }
            done_.notify_all();
        }
    }
    std::mutex mx_;
    std::condition_variable cv_, done_;
    std::atomic<const std::function<void(size_t, size_t)>*> fn_{nullptr};
    std::atomic<size_t> next_{0};
    std::atomic<size_t> n_{0}, grain_{1};
    uint64_t round_ = 0;
    int busy_ = 0;
};

struct StampHit {
    uint32_t texel;   // of the texture's top level: y * w + x
    float mx, my;     // where in the motif (pixels)
};
struct StampLayerCache {
    TexStampLayer s;
    Motif* m = nullptr;
    std::vector<StampHit> hits;
    std::vector<uint32_t> blocks;  // its 4 x 4 blocks of the top level
};
struct StampState {
    uint64_t gen = 0;                          // the pin's generation it was made from
    uint32_t w = 0, h = 0;
    std::vector<std::vector<uint8_t>> orig;    // the pinned DDS's levels (BC1), to start each compose from
    std::vector<std::vector<uint8_t>> mips;    // the levels as composed now (the pin's own copy is kept the same)
    std::vector<uint8_t> top;                  // the top level as RGBA now
    std::vector<StampLayerCache> layers;
};
struct StampJob {
    std::string path;
    std::wstring posmap;
    std::vector<TexStampLayer> layers;
};
std::map<void*, StampState> g_stamps;        // the worker's own
std::mutex g_stampMx;                         // the requests and the worker's numbers
std::condition_variable g_stampCv;
std::map<void*, StampJob> g_stampWant;        // render texture -> the newest request
bool g_stampRunning = false;
std::string g_stampErr;
uint64_t g_stampDone = 0, g_stampLastUs = 0, g_stampDropped = 0;

bool SameGeometry(const TexStampLayer& a, const TexStampLayer& b) {
    return a.motif == b.motif && !memcmp(a.centre, b.centre, sizeof a.centre) &&
           !memcmp(a.normal, b.normal, sizeof a.normal) && !memcmp(a.up, b.up, sizeof a.up) &&
           !memcmp(a.right, b.right, sizeof a.right) && a.size == b.size && a.depth == b.depth &&
           a.facing == b.facing && a.flip == b.flip;
}
bool SamePaint(const TexStampLayer& a, const TexStampLayer& b) {
    return a.opacity == b.opacity && a.tint == b.tint && !memcmp(a.colour, b.colour, sizeof a.colour);
}

StampPool& Pool() {
    static StampPool pool((std::max)(2, (std::min)(8, static_cast<int>(std::thread::hardware_concurrency()) / 4)));
    return pool;
}

// the texels a layer covers, and where in its motif each lies
void GatherStamp(const PosMap& pm, uint32_t W, uint32_t H, StampLayerCache& lc) {
    const TexStampLayer& s = lc.s;
    const Motif& m = *lc.m;
    lc.hits.clear();
    lc.blocks.clear();
    const float halfCell = kCell * 0.8661f;
    const float sx = static_cast<float>(pm.w) / W, sy = static_cast<float>(pm.h) / H;
    const bool same = pm.w == W && pm.h == H;
    float asp = static_cast<float>(m.w) / m.h;
    float su = asp >= 1 ? 1.0f : asp, sv = asp >= 1 ? 1.0f / asp : 1.0f;
    float reach = s.size * 0.75f + s.depth;
    int c0[3], c1[3];
    for (int k = 0; k < 3; ++k) {
        c0[k] = static_cast<int>(std::floor((s.centre[k] - reach) / kCell));
        c1[k] = static_cast<int>(std::floor((s.centre[k] + reach) / kCell));
    }
    // one texel's test: in the stamp's flat box, not turned away, inside the motif -> its place in the motif
    auto test = [&](const float* p3, const float* n3, float* mx, float* my) {
        float d[3] = {p3[0] - s.centre[0], p3[1] - s.centre[1], p3[2] - s.centre[2]};
        float z = d[0] * s.normal[0] + d[1] * s.normal[1] + d[2] * s.normal[2];
        if (std::fabs(z) > s.depth) return false;
        if (n3[0] * s.normal[0] + n3[1] * s.normal[1] + n3[2] * s.normal[2] < s.facing) return false;
        float u = (d[0] * s.right[0] + d[1] * s.right[1] + d[2] * s.right[2]) / s.size;
        float v = -(d[0] * s.up[0] + d[1] * s.up[1] + d[2] * s.up[2]) / s.size;
        if (s.flip) u = -u;
        if (std::fabs(u) >= su / 2 || std::fabs(v) >= sv / 2) return false;
        *mx = (u / su + 0.5f) * (m.w - 1), *my = (v / sv + 0.5f) * (m.h - 1);
        return true;
    };
    auto bilinear = [&](float fx, float fy, float* out, float* nrm) {
        fx = (std::max)(0.0f, (std::min)(fx, pm.w - 1.001f));
        fy = (std::max)(0.0f, (std::min)(fy, pm.h - 1.001f));
        uint32_t x0 = static_cast<uint32_t>(fx), y0 = static_cast<uint32_t>(fy);
        float ax = fx - x0, ay = fy - y0, wsum = 0;
        float o[3] = {}, n[3] = {};
        for (int c = 0; c < 4; ++c) {
            uint32_t xx = x0 + (c & 1), yy = y0 + (c >> 1);
            const int8_t* nn = &pm.nrm[3ull * (yy * pm.w + xx)];
            if (!nn[0] && !nn[1] && !nn[2]) continue;
            float wgt = ((c & 1) ? ax : 1 - ax) * ((c >> 1) ? ay : 1 - ay);
            const int16_t* pp = &pm.pos[3ull * (yy * pm.w + xx)];
            for (int k = 0; k < 3; ++k) o[k] += wgt * pp[k] / 4000.0f, n[k] += wgt * nn[k] / 127.0f;
            wsum += wgt;
        }
        if (wsum <= 0) return false;
        for (int k = 0; k < 3; ++k) out[k] = o[k] / wsum, nrm[k] = n[k];
        return true;
    };
    // over the cells in slices across x, each slice's hits on its own, joined after
    int nx = c1[0] - c0[0] + 1;
    std::vector<std::vector<StampHit>> parts(static_cast<size_t>(nx));
    Pool().For(static_cast<size_t>(nx), [&](size_t b, size_t e) {
        for (size_t i = b; i < e; ++i) {
            int x = c0[0] + static_cast<int>(i);
            std::vector<StampHit>& out = parts[i];
            for (int y = c0[1]; y <= c1[1]; ++y)
                for (int z = c0[2]; z <= c1[2]; ++z) {
                    float d[3] = {(x + 0.5f) * kCell - s.centre[0], (y + 0.5f) * kCell - s.centre[1],
                                  (z + 0.5f) * kCell - s.centre[2]};
                    float dn = d[0] * s.normal[0] + d[1] * s.normal[1] + d[2] * s.normal[2];
                    float du = d[0] * s.right[0] + d[1] * s.right[1] + d[2] * s.right[2];
                    float dv = d[0] * s.up[0] + d[1] * s.up[1] + d[2] * s.up[2];
                    if (std::fabs(dn) > s.depth + halfCell || std::fabs(du) > s.size * 0.5f + halfCell ||
                        std::fabs(dv) > s.size * 0.5f + halfCell)
                        continue;
                    auto g = pm.grid.find(CellKey(x, y, z));
                    if (g == pm.grid.end()) continue;
                    for (uint32_t t : g->second) {
                        const int8_t* nn = &pm.nrm[3ull * t];
                        if (nn[0] * s.normal[0] + nn[1] * s.normal[1] + nn[2] * s.normal[2] < (s.facing - 0.05f) * 127)
                            continue;
                        if (same) {  // (the texture as large as the map: its texel is the map's, no blending)
                            const int16_t* pp = &pm.pos[3ull * t];
                            float p3[3] = {pp[0] / 4000.0f, pp[1] / 4000.0f, pp[2] / 4000.0f};
                            float n3[3] = {nn[0] / 127.0f, nn[1] / 127.0f, nn[2] / 127.0f};
                            float mx, my;
                            if (test(p3, n3, &mx, &my)) out.push_back({t, mx, my});
                            continue;
                        }
                        uint32_t tx = t % pm.w, ty = t / pm.w;
                        uint32_t x0 = static_cast<uint32_t>(tx / sx), y0 = static_cast<uint32_t>(ty / sy);
                        uint32_t x1 = (std::min)(W, static_cast<uint32_t>((tx + 1) / sx)),
                                 y1 = (std::min)(H, static_cast<uint32_t>((ty + 1) / sy));
                        for (uint32_t yy = y0; yy < (std::max)(y1, y0 + 1); ++yy)
                            for (uint32_t xx = x0; xx < (std::max)(x1, x0 + 1); ++xx) {
                                float p3[3], n3[3], mx, my;
                                if (!bilinear((xx + 0.5f) * sx - 0.5f, (yy + 0.5f) * sy - 0.5f, p3, n3)) continue;
                                if (test(p3, n3, &mx, &my)) out.push_back({yy * W + xx, mx, my});
                            }
                    }
                }
        }
    }, 1);
    size_t total = 0;
    for (auto& v : parts) total += v.size();
    lc.hits.reserve(total);
    for (auto& v : parts) lc.hits.insert(lc.hits.end(), v.begin(), v.end());
    uint32_t bw = (W + 3) / 4, bh = (H + 3) / 4;
    std::vector<uint8_t> mark(static_cast<size_t>(bw) * bh, 0);
    for (const StampHit& hh : lc.hits) mark[((hh.texel / W) / 4) * bw + (hh.texel % W) / 4] = 1;
    for (uint32_t b = 0; b < mark.size(); ++b)
        if (mark[b]) lc.blocks.push_back(b);
}

// one request composed: -> "" or why not
std::string ComposeStamp(void* rt, const StampJob& job) {
    std::string err;
    PosMap* pm = LoadPosMap(job.posmap, &err);
    if (!pm) return err;
    std::vector<Motif*> motifs;
    for (const TexStampLayer& s : job.layers) {
        Motif* m = LoadMotif(s.motif, &err);
        if (!m) return err;
        motifs.push_back(m);
    }
    StampState& st = g_stamps[rt];
    uint64_t gen = 0;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> lk(g_jobMx);
        auto pit = g_pins.find(rt);
        if (pit == g_pins.end()) return "the texture is not pinned (w3.texture.write first)";
        Pin& p = pit->second;
        if (p.fam != 1) return "stamps go on BC1 textures only";
        gen = p.gen;
        // (a new pin - another DDS, another tone - starts from that one)
        if (st.orig.empty() || st.gen != gen) {
            st = StampState();
            st.gen = gen, st.w = p.w, st.h = p.h;
            st.orig = p.mips;
            st.mips = p.mips;
            fresh = true;
        }
    }
    const uint32_t W = st.w, H = st.h, bw = (W + 3) / 4, bh = (H + 3) / 4;
    if (fresh) {  // the top level decoded once
        st.top.assign(4ull * W * H, 0);
        Pool().For(bh, [&](size_t b, size_t e) {
            uint8_t px[16][4];
            for (size_t by = b; by < e; ++by)
                for (uint32_t bx = 0; bx < bw; ++bx) {
                    DecodeBC1(&st.orig[0][8ull * (by * bw + bx)], px);
                    for (int i = 0; i < 16; ++i) {
                        uint32_t x = bx * 4 + (i & 3), y = static_cast<uint32_t>(by) * 4 + (i >> 2);
                        if (x < W && y < H) memcpy(&st.top[4ull * (y * W + x)], px[i], 4);
                    }
                }
        }, 16);
    }
    // the layers: kept where nothing of their place changed, gathered anew otherwise; the blocks to compose again
    // are those of every layer that changed in any way, where it was and where it is now
    std::vector<uint8_t> dirty(static_cast<size_t>(bw) * bh, 0);
    std::vector<StampLayerCache> now(job.layers.size());
    for (size_t i = 0; i < job.layers.size(); ++i) {
        StampLayerCache& lc = now[i];
        lc.s = job.layers[i];
        lc.m = motifs[i];
        const StampLayerCache* old = i < st.layers.size() ? &st.layers[i] : nullptr;
        if (old && old->m == lc.m && SameGeometry(old->s, lc.s)) {
            lc.hits = old->hits;
            lc.blocks = old->blocks;
            if (SamePaint(old->s, lc.s)) continue;
        } else {
            GatherStamp(*pm, W, H, lc);
            if (old)
                for (uint32_t b : old->blocks) dirty[b] = 1;
        }
        for (uint32_t b : lc.blocks) dirty[b] = 1;
    }
    for (size_t i = job.layers.size(); i < st.layers.size(); ++i)
        for (uint32_t b : st.layers[i].blocks) dirty[b] = 1;
    st.layers = std::move(now);
    std::vector<uint32_t> redo;
    uint32_t rx0 = bw, ry0 = bh, rx1 = 0, ry1 = 0;
    for (uint32_t b = 0; b < dirty.size(); ++b)
        if (dirty[b]) {
            redo.push_back(b);
            uint32_t bx = b % bw, by = b / bw;
            rx0 = (std::min)(rx0, bx), ry0 = (std::min)(ry0, by), rx1 = (std::max)(rx1, bx + 1), ry1 = (std::max)(ry1, by + 1);
        }
    if (redo.empty()) return "";
    // back to the DDS's own pixels there
    Pool().For(redo.size(), [&](size_t b0, size_t e) {
        uint8_t px[16][4];
        for (size_t k = b0; k < e; ++k) {
            uint32_t b = redo[k];
            DecodeBC1(&st.orig[0][8ull * b], px);
            uint32_t bx = b % bw, by = b / bw;
            for (int i = 0; i < 16; ++i) {
                uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < W && y < H) memcpy(&st.top[4ull * (y * W + x)], px[i], 4);
            }
        }
    }, 256);
    // each layer on its texels there (in order: later ones on top; a layer's texels are its own, so it runs on all
    // threads at once)
    for (const StampLayerCache& lc : st.layers) {
        const TexStampLayer& s = lc.s;
        const Motif& m = *lc.m;
        Pool().For(lc.hits.size(), [&](size_t b0, size_t e) {
            for (size_t k = b0; k < e; ++k) {
                const StampHit& hh = lc.hits[k];
                uint32_t x = hh.texel % W, y = hh.texel / W;
                if (!dirty[(y / 4) * bw + x / 4]) continue;
                uint32_t ix = static_cast<uint32_t>(hh.mx), iy = static_cast<uint32_t>(hh.my);
                uint32_t ix1 = (std::min)(ix + 1, m.w - 1), iy1 = (std::min)(iy + 1, m.h - 1);
                float fx = hh.mx - ix, fy = hh.my - iy;
                const uint8_t* r0 = &m.rgba[4ull * (iy * m.w)];
                const uint8_t* r1 = &m.rgba[4ull * (iy1 * m.w)];
                float a = ((r0[4 * ix + 3] * (1 - fx) + r0[4 * ix1 + 3] * fx) * (1 - fy) +
                           (r1[4 * ix + 3] * (1 - fx) + r1[4 * ix1 + 3] * fx) * fy) / 255.0f * s.opacity;
                if (a <= 0.002f) continue;
                uint8_t* t = &st.top[4ull * hh.texel];
                // ink: its own colour under her skin - shaded by the skin's brightness (its pores and shading stay, on
                // dark skin a light ink shows dimmer); a scar: the skin times its colour ratio
                float shade = 0.0f;
                if (s.tint) {
                    float lum = (0.30f * t[0] + 0.59f * t[1] + 0.11f * t[2]) / 255.0f;
                    shade = (std::max)(0.45f, (std::min)(1.1f, 0.35f + lum));
                }
                for (int c = 0; c < 3; ++c) {
                    float v2;
                    if (s.tint) {
                        v2 = t[c] * (1 - a) + 255.0f * s.colour[c] * shade * a;
                    } else {
                        float ratio = ((r0[4 * ix + c] * (1 - fx) + r0[4 * ix1 + c] * fx) * (1 - fy) +
                                       (r1[4 * ix + c] * (1 - fx) + r1[4 * ix1 + c] * fx) * fy) / 128.0f;
                        v2 = t[c] * (1 - a) + t[c] * ratio * a;
                    }
                    t[c] = static_cast<uint8_t>((std::max)(0.0f, (std::min)(255.0f, v2 + 0.5f)));
                }
            }
        }, 8192);
    }
    // encoded again: the redone blocks of the top level, and the levels below over the same area
    Pool().For(redo.size(), [&](size_t b0, size_t e) {
        uint8_t px[16][4], enc[16];
        for (size_t k = b0; k < e; ++k) {
            uint32_t b = redo[k], bx = b % bw, by = b / bw;
            for (int i = 0; i < 16; ++i) {
                uint32_t x = (std::min)(bx * 4 + (i & 3), W - 1), y = (std::min)(by * 4 + (i >> 2), H - 1);
                memcpy(px[i], &st.top[4ull * (y * W + x)], 4);
            }
            EncodeBlock(px, enc);
            memcpy(&st.mips[0][8ull * b], enc + 8, 8);
        }
    }, 256);
    for (size_t lv = 1; lv < st.mips.size(); ++lv) {
        uint32_t lw = (std::max)(1u, W >> lv), lh = (std::max)(1u, H >> lv), lbw = (lw + 3) / 4, lbh = (lh + 3) / 4;
        uint32_t f = 1u << lv;
        uint32_t x0 = rx0 * 4 / f / 4, y0 = ry0 * 4 / f / 4;
        uint32_t x1 = (std::min)(lbw, (rx1 * 4 + f * 4 - 1) / f / 4), y1 = (std::min)(lbh, (ry1 * 4 + f * 4 - 1) / f / 4);
        if (x1 <= x0 || y1 <= y0) continue;
        Pool().For(static_cast<size_t>(y1 - y0), [&](size_t b0, size_t e) {
            uint8_t px[16][4], enc[16];
            for (size_t r = b0; r < e; ++r) {
                uint32_t lby = y0 + static_cast<uint32_t>(r);
                for (uint32_t lbx = x0; lbx < x1; ++lbx) {
                    for (int i = 0; i < 16; ++i) {
                        uint32_t x = (std::min)(lbx * 4 + (i & 3), lw - 1), y = (std::min)(lby * 4 + (i >> 2), lh - 1);
                        uint32_t acc[3] = {}, n = 0;
                        uint32_t step = (std::max)(1u, f / 4);
                        for (uint32_t yy = y * f; yy < (std::min)(H, (y + 1) * f); yy += step)
                            for (uint32_t xx = x * f; xx < (std::min)(W, (x + 1) * f); xx += step) {
                                const uint8_t* t = &st.top[4ull * (yy * W + xx)];
                                acc[0] += t[0], acc[1] += t[1], acc[2] += t[2], ++n;
                            }
                        for (int k = 0; k < 3; ++k) px[i][k] = static_cast<uint8_t>(acc[k] / (std::max)(1u, n));
                        px[i][3] = 255;
                    }
                    EncodeBlock(px, enc);
                    memcpy(&st.mips[lv][8ull * (lby * lbw + lbx)], enc + 8, 8);
                }
            }
        }, 4);
    }
    // into the pin (the same levels there), the changed rectangle for its upload
    std::lock_guard<std::mutex> lk(g_jobMx);
    auto pit = g_pins.find(rt);
    if (pit == g_pins.end() || pit->second.gen != gen) {
        g_stamps.erase(rt);  // (pinned anew meanwhile: the next request starts from that)
        return "";
    }
    Pin& p = pit->second;
    for (size_t lv = 0; lv < st.mips.size() && lv < p.mips.size(); ++lv) {
        uint32_t lw = (std::max)(1u, W >> lv), lh = (std::max)(1u, H >> lv), lbw = (lw + 3) / 4, lbh = (lh + 3) / 4;
        uint32_t f = 1u << lv;
        uint32_t y0 = ry0 * 4 / f / 4, y1 = (std::min)(lbh, (ry1 * 4 + f * 4 - 1) / f / 4);
        uint32_t x0 = rx0 * 4 / f / 4, x1 = (std::min)(lbw, (rx1 * 4 + f * 4 - 1) / f / 4);
        for (uint32_t by = y0; by < y1; ++by)
            memcpy(&p.mips[lv][8ull * (by * lbw + x0)], &st.mips[lv][8ull * (by * lbw + x0)], 8ull * (x1 - x0));
    }
    uint32_t rc[4] = {rx0 * 4, ry0 * 4, (std::min)(W, rx1 * 4), (std::min)(H, ry1 * 4)};
    for (int s = 0; s < 2; ++s) {
        if (p.due[s] && !p.part[s]) continue;  // (a whole write is due anyway)
        if (p.part[s]) {
            p.rect[s][0] = (std::min)(p.rect[s][0], rc[0]), p.rect[s][1] = (std::min)(p.rect[s][1], rc[1]);
            p.rect[s][2] = (std::max)(p.rect[s][2], rc[2]), p.rect[s][3] = (std::max)(p.rect[s][3], rc[3]);
        } else {
            memcpy(p.rect[s], rc, sizeof rc);
            p.part[s] = true;
        }
        p.due[s] = true;
    }
    return "";
}


// --- an eye texture mixed from layers (27.11., the character editor's eyes: per eye the white and its veins, the
// iris inside and outside, the pupil, round or a cat's slit). The layers (TWEY: u32 w, h, channels, then the channels
// interleaved, u8; player_woman\eye_layers.twey from eye_layers.py): 0-2 the white without veins, 3 veins as many as
// a usual eye's, 4 bloodshot veins, 5 the iris's cover, 6 across the iris (0 at the pupil, 1 at its rim), 7 the
// round iris's structure (/ 2), 8 the round pupil, 9 the cat iris's structure, 10 the cat's slit. The mix goes on the
// stamps' thread (the newest per texture wins) and pins the texture with the result (BC1, all levels), as
// w3.texture.write does with a DDS.
struct EyeLayers {
    uint32_t w = 0, h = 0, n = 0;
    std::vector<uint8_t> data;
};
std::map<std::wstring, std::unique_ptr<EyeLayers>> g_eyeLayers;  // the worker's own
struct EyeJob {
    std::string path;
    std::wstring layers;
    TexEyeLook look;
};
std::map<void*, EyeJob> g_eyeWant;  // under g_stampMx

EyeLayers* LoadEyeLayers(const std::wstring& file, std::string* err) {
    auto it = g_eyeLayers.find(file);
    if (it != g_eyeLayers.end()) return it->second.get();
    FILE* f = _wfopen(file.c_str(), L"rb");
    if (!f) {
        *err = "cannot read the eye layers";
        return nullptr;
    }
    char magic[4] = {};
    uint32_t hd[3] = {};
    auto e = std::make_unique<EyeLayers>();
    bool ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "TWEY", 4) == 0 && fread(hd, 4, 3, f) == 3 && hd[0] &&
              hd[1] && hd[0] <= 4096 && hd[1] <= 4096 && hd[2] >= 11 && hd[2] <= 32;
    if (ok) {
        e->w = hd[0], e->h = hd[1], e->n = hd[2];
        e->data.resize(static_cast<size_t>(e->w) * e->h * e->n);
        ok = fread(e->data.data(), 1, e->data.size(), f) == e->data.size();
    }
    fclose(f);
    if (!ok) {
        *err = "not eye layers (TWEY)";
        return nullptr;
    }
    EyeLayers* out = e.get();
    g_eyeLayers[file] = std::move(e);
    return out;
}

// the mix -> "" or why not
std::string ComposeEye(void* rt, const EyeJob& job) {
    std::string err;
    EyeLayers* L = LoadEyeLayers(job.layers, &err);
    if (!L) return err;
    const TexEyeLook& k = job.look;
    const uint32_t W = L->w, H = L->h, N = L->n;
    std::vector<uint8_t> top(4ull * W * H);
    // (the white's colour against the layers' own: (240, 236, 232) is "as drawn")
    const float ref[3] = {240 / 255.0f, 236 / 255.0f, 232 / 255.0f};
    const float base1 = (std::min)(1.0f, k.veins / 0.35f);
    const float heavy = (std::max)(0.0f, (std::min)(1.0f, (k.veins - 0.35f) / 0.65f));
    const float red = 0.35f * (std::max)(0.0f, (std::min)(1.0f, (k.veins - 0.5f) / 0.5f));
    Pool().For(H, [&](size_t y0, size_t y1) {
        for (size_t y = y0; y < y1; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const uint8_t* l = &L->data[(y * W + x) * N];
                float base[3], px[3];
                for (int c = 0; c < 3; ++c) base[c] = l[c] / 255.0f * (k.white[c] / ref[c]);
                float lum = 0.30f * base[0] + 0.59f * base[1] + 0.11f * base[2];
                float v = (std::min)(1.0f, l[3] / 255.0f * base1 + l[4] / 255.0f * heavy);
                for (int c = 0; c < 3; ++c) {
                    base[c] = base[c] * (1 - red) + base[c] * k.vein[c] * 1.6f * red;
                    base[c] = base[c] * (1 - v) + k.vein[c] * (0.55f + 0.6f * lum) * v;
                }
                float t = l[6] / 255.0f, m = l[5] / 255.0f;
                float detail = (k.cat ? l[9] : l[7]) / 255.0f * 2;
                float pm = (k.cat ? l[10] : l[8]) / 255.0f;
                for (int c = 0; c < 3; ++c) {
                    float iris = (k.inner[c] * (1 - t) + k.outer[c] * t) * detail;
                    px[c] = base[c] * (1 - m) + iris * m;
                    px[c] = px[c] * (1 - pm) + k.pupil[c] * (0.7f + 0.3f * detail) * pm;
                }
                uint8_t* o = &top[4ull * (y * W + x)];
                for (int c = 0; c < 3; ++c)
                    o[c] = static_cast<uint8_t>((std::max)(0.0f, (std::min)(255.0f, px[c] * 255.0f + 0.5f)));
                o[3] = 255;
            }
    }, 16);
    // BC1, every level (each from the one above, a 2 x 2 box)
    std::vector<std::vector<uint8_t>> mips;
    std::vector<uint8_t> cur = std::move(top), next;
    uint32_t lw = W, lh = H;
    for (;;) {
        uint32_t bw = (lw + 3) / 4, bh = (lh + 3) / 4;
        std::vector<uint8_t> bc(8ull * bw * bh);
        Pool().For(bh, [&](size_t b0, size_t b1) {
            uint8_t px[16][4], enc[16];
            for (size_t by = b0; by < b1; ++by)
                for (uint32_t bx = 0; bx < bw; ++bx) {
                    for (int i = 0; i < 16; ++i) {
                        uint32_t x = (std::min)(bx * 4 + (i & 3), lw - 1);
                        uint32_t y = (std::min)(static_cast<uint32_t>(by) * 4 + (i >> 2), lh - 1);
                        memcpy(px[i], &cur[4ull * (y * lw + x)], 4);
                    }
                    EncodeBlock(px, enc);
                    memcpy(&bc[8ull * (by * bw + bx)], enc + 8, 8);
                }
        }, 8);
        mips.push_back(std::move(bc));
        if (lw == 1 && lh == 1) break;
        uint32_t nw = (std::max)(1u, lw / 2), nh = (std::max)(1u, lh / 2);
        next.assign(4ull * nw * nh, 0);
        for (uint32_t y = 0; y < nh; ++y)
            for (uint32_t x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    uint32_t x0 = (std::min)(2 * x, lw - 1), x1 = (std::min)(2 * x + 1, lw - 1);
                    uint32_t y0 = (std::min)(2 * y, lh - 1), y1 = (std::min)(2 * y + 1, lh - 1);
                    uint32_t s = cur[4ull * (y0 * lw + x0) + c] + cur[4ull * (y0 * lw + x1) + c] +
                                 cur[4ull * (y1 * lw + x0) + c] + cur[4ull * (y1 * lw + x1) + c];
                    next[4ull * (y * nw + x) + c] = static_cast<uint8_t>((s + 2) / 4);
                }
        cur.swap(next);
        lw = nw, lh = nh;
    }
    // pinned with it: a pin of its own, or the pin's levels replaced (written whole, both slots)
    std::lock_guard<std::mutex> lk(g_jobMx);
    auto pit = g_pins.find(rt);
    if (pit == g_pins.end()) {
        Pin p;
        p.rt = rt;
        p.path = job.path;
        p.fam = 1;
        p.w = W, p.h = H;
        p.mips = std::move(mips);
        p.gen = ++g_pinGen;
        // (on the stamps' thread: the reference is taken here, given back on the main thread)
        p.held = DropOtherPins(rt, p.path) || HoldRt(rt);
        g_pins[rt] = std::move(p);
    } else {
        Pin& p = pit->second;
        if (p.fam != 1 || p.w != W || p.h != H) return "the texture is pinned with another size or format";
        p.mips = std::move(mips);
        p.gen = ++g_pinGen;
        p.due[0] = p.due[1] = true;
        p.part[0] = p.part[1] = false;
    }
    return "";
}

void StampWorker() {
    for (;;) {
        void* rt;
        StampJob job;
        EyeJob eye;
        bool isEye = false;
        {
            std::unique_lock<std::mutex> l(g_stampMx);
            g_stampCv.wait(l, [] { return !g_stampWant.empty() || !g_eyeWant.empty(); });
            if (!g_eyeWant.empty()) {
                auto it = g_eyeWant.begin();
                rt = it->first;
                eye = std::move(it->second);
                g_eyeWant.erase(it);
                isEye = true;
            } else {
                auto it = g_stampWant.begin();
                rt = it->first;
                job = std::move(it->second);
                g_stampWant.erase(it);
            }
        }
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        std::string err = isEye ? ComposeEye(rt, eye) : ComposeStamp(rt, job);
        if (isEye) job.path = eye.path;
        QueryPerformanceCounter(&t1);
        std::lock_guard<std::mutex> l(g_stampMx);
        ++g_stampDone;
        g_stampLastUs = static_cast<uint64_t>((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
        if (!err.empty()) {
            g_stampErr = job.path + ": " + err;
            Log("texwrite: stamp on %s: %s", job.path.c_str(), err.c_str());
        }
    }
}

}  // namespace

// w3.texture.stamp (main thread): the pinned texture's stamps composed anew - handed to the worker, back at once. A
// pin replaced by w3.texture.write (another skin tone) starts without stamps: call it again after.
bool TextureStamp(const char* path, const std::wstring& posmap, const std::vector<TexStampLayer>& layers,
                  std::string* err) {
    std::string e;
    void* tex = DecalTexture(path, e);
    void* rt = tex ? BitmapRenderTexture(tex) : nullptr;
    if (!rt) {
        *err = e.empty() ? "the texture has no render texture" : e;
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(g_jobMx);
        if (g_pins.find(rt) == g_pins.end()) {
            *err = "the texture is not pinned (w3.texture.write first)";
            return false;
        }
    }
    std::lock_guard<std::mutex> l(g_stampMx);
    if (g_stampWant.count(rt)) ++g_stampDropped;
    StampJob& j = g_stampWant[rt];
    j.path = path;
    j.posmap = posmap;
    j.layers = layers;
    if (!g_stampRunning) {
        g_stampRunning = true;
        std::thread(StampWorker).detach();
    }
    g_stampCv.notify_one();
    return true;
}

// w3.texture.eye (main thread): handed to the stamps' thread, back at once
bool TextureEye(const char* path, const std::wstring& layers, const TexEyeLook& look, std::string* err) {
    std::string e;
    void* tex = DecalTexture(path, e);
    void* rt = tex ? BitmapRenderTexture(tex) : nullptr;
    if (!rt) {
        *err = e.empty() ? "the texture has no render texture" : e;
        return false;
    }
    InstallOverlay(true);  // its ExecuteCommandLists hook records the copies (installed on first use)
    ReleaseDroppedRts();
    std::lock_guard<std::mutex> l(g_stampMx);
    if (g_eyeWant.count(rt)) ++g_stampDropped;
    EyeJob& j = g_eyeWant[rt];
    j.path = path;
    j.layers = layers;
    j.look = look;
    if (!g_stampRunning) {
        g_stampRunning = true;
        std::thread(StampWorker).detach();
    }
    g_stampCv.notify_one();
    return true;
}

std::string TexStampStats() {
    std::lock_guard<std::mutex> l(g_stampMx);
    char b[400];
    snprintf(b, sizeof b, "stamps and eyes: %llu composed, %llu replaced by a newer one, last %llu us, %zu waiting%s%s",
             static_cast<unsigned long long>(g_stampDone), static_cast<unsigned long long>(g_stampDropped),
             static_cast<unsigned long long>(g_stampLastUs), g_stampWant.size() + g_eyeWant.size(),
             g_stampErr.empty() ? "" : "; ",
             g_stampErr.c_str());
    return b;
}

// Render thread, at each present: a finished job, or a pinned texture with a new resource or a second write due.
bool TexWritesPending() {
    std::lock_guard<std::mutex> l(g_jobMx);
    bool due = !g_ready.empty();
    for (auto& e : g_pins) {
        Pin& p = e.second;
        for (int s = 0; s < 2; ++s) {
            D3D12_RESOURCE_DESC d = {};
            ID3D12Resource* r = ResAt(p.rt, kPinIds[s], &d);
            if (r && r != p.last[s]) due = true;
            if (p.again[s] > 0 && --p.again[s] == 0) p.due[s] = true, p.part[s] = false;
            if (p.due[s]) due = true;
        }
    }
    return due;
}

// On the render thread inside blendshape.cpp's SubmitWork (under its lock): each finished job copied into its
// texture. Objects the list uses go to `keep` (released once the list is done).
bool TexRecord(void* list, void* device, std::vector<IUnknown*>* keep) {
    auto* l = static_cast<ID3D12GraphicsCommandList*>(list);
    auto* dev = static_cast<ID3D12Device*>(device);
    std::map<void*, std::unique_ptr<Job>> jobs;
    bool any = false;
    {
        std::lock_guard<std::mutex> lk(g_jobMx);
        jobs.swap(g_ready);
        for (auto& e : g_pins) {  // pinned textures: a new resource, or the second write after one
            Pin& p = e.second;
            for (int s = 0; s < 2; ++s) {
                D3D12_RESOURCE_DESC d = {};
                ID3D12Resource* r = ResAt(p.rt, kPinIds[s], &d);
                if (!r || (r == p.last[s] && !p.due[s])) continue;
                bool fresh = r != p.last[s];
                p.last[s] = r;
                p.due[s] = false;
                if (fresh) p.again[s] = kAgainFrames, p.warned[s] = false;
                const uint32_t* rc = (!fresh && p.part[s]) ? p.rect[s] : nullptr;
                p.part[s] = false;
                if (PinRecord(p, s, r, d, l, dev, keep, rc)) any = true;
            }
        }
    }
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
    char b[260];
    {
        std::lock_guard<std::mutex> l(g_jobMx);
        snprintf(b, sizeof b, "%llu written, %llu dropped for a newer one, %llu us encoding in all, %zu waiting, %zu "
                 "ready; %zu pinned, %llu pin writes (%llu KB); ",
                 static_cast<unsigned long long>(g_written), static_cast<unsigned long long>(g_dropped),
                 static_cast<unsigned long long>(g_encodeUs), g_waiting.size(), g_ready.size(), g_pins.size(),
                 static_cast<unsigned long long>(g_pinWrites), static_cast<unsigned long long>(g_pinBytes / 1024));
    }
    return b + TexStampStats();
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
