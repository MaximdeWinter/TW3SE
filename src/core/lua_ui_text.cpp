// TW3SE core - text as Qt draws it (for a Qt app's UI in the game): Segoe UI rasterised by DirectWrite the way
// Qt 6 does on Windows (ClearType; up to 16 px GDI-classic rendering and widths, from 17 px natural symmetric with
// GDI-natural widths), kerning from the font, blended per colour channel by the host's text shader (dual-source
// blending, gamma like Qt's). Fonts are made on the player's machine from the installed Segoe UI: no font file goes
// with TW3SE.
//
//   w3.ui.draw.text(x, y, colour, s [, size, bold, wrap, background])  - y is the top of the line, as in Qt
//   (bold: true or "bold" for Segoe UI bold, "mono" for Consolas - a QPlainTextEdit's log, 11 to 13 px)
//   w3.ui.text_size(s [, size, bold])                                    - Qt's horizontalAdvance and line height
#define NOMINMAX
#include <windows.h>
#include <dwrite_1.h>
#include <dwrite_2.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "lua.h"
#include "lauxlib.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "lua_ui.h"

#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "advapi32.lib")

namespace tw3se {
namespace {

struct Glyph {
    int x = 0, y = 0, w = 0, h = 0;  // in the atlas
    int gx = 0, gy = 0;              // its grayscale picture (Qt's in a translucent window)
    int ox = 0, oy = 0;              // the bitmap's top left from the pen on the baseline
    int adv = 0;
    float rsb = 0;  // the design right side bearing in px (negative: the letter reaches past its advance)
    int rsbDesign = 0, upem = 2048;  // the same in design units of its font (Qt's line breaking counts with it)
    UINT16 index = 0;
};
struct CtFont {
    float px = 0;  // (a point size's pixels may have a fraction: 8.5 pt are 11.33 px, as Qt gives DirectWrite)
    int style = 0;  // 0 Segoe UI, 1 Segoe UI bold, 2 Consolas
    IDWriteFontFace* face = nullptr;
    std::vector<IDWriteFontFace*> fallback;  // Windows' font link list for Segoe UI, for what Segoe UI lacks
    IDWriteFontFace1* face1 = nullptr;
    IDWriteTextFormat* format = nullptr;
    int upem = 2048;
    int ascent = 0, descent = 0;
    int minRsb = 0;  // the 'hhea' table's minimum right side bearing (design units): QFontEngine::minRightBearing
    std::unordered_map<uint32_t, Glyph> glyphs;
    std::unordered_map<uint64_t, int> kern;  // (left << 32 | right) -> px
};

IDWriteFactory* g_dw = nullptr;
std::vector<CtFont> g_ct;
std::vector<unsigned char> g_atlas;  // RGBA: rgb the ClearType coverage
int g_aw = 2048, g_ah = 0;
unsigned long long g_tex = 0;
// 0 not asked, 1 the thread builds them, 2 built (the texture not made yet), 3 ready. Until 3 only the building
// thread touches the fonts and the atlas; from 3 on only the main thread.
std::atomic<int> g_state{0};
unsigned long long g_buildMs = 0;
float g_gamma = 1.2f;
float g_grayGamma = 1.0f;  // the gamma for grayscale glyphs (measured on Qt's bar: 1)
int g_grayVariant = 2;     // 0: qGray of the ClearType coverage, 1: DirectWrite's grayscale analysis, 2: the
                           // average of the three (Qt's naive blend on a translucent pixel, qt_alphargbblit_argb32)
// Qt's letter widths: DirectWrite's GDI-classic ones up to 16 px, the GDI-natural ones from 17 px ("s" bold 17 px: 8,
// classic 7). Measured for every letter 33..126 at 9..28 px, normal and bold (measured against Qt).
constexpr int kNaturalFrom = 17;
int g_gammaMode = 1;  // 0 plain blending, 1 Qt's gamma (pow g), 2 pow 1/g: the lab compares them with Qt
int g_shelfX = 1, g_shelfY = 1, g_shelfH = 0;

const uint32_t kRanges[][2] = {{0x20, 0x7E},     {0xA0, 0x17F},    {0x400, 0x45F},   {0x490, 0x491},  {0x2010, 0x2027}, {0x2030, 0x203A},
                               {0x20AC, 0x20AC}, {0x2190, 0x2199}, {0x2212, 0x2212}, {0x25A0, 0x25FF},
                               {0x2605, 0x2606}, {0x2713, 0x2718}, {0x2190, 0x21FF},
                               // (zero width: the space, the joiners, the marks; U+200B breaks paths)
                               {0x200B, 0x200F}, {0x2060, 0x2064}, {0xFEFF, 0xFEFF}};

bool Place(int w, int h, int* x, int* y) {
    if (g_shelfX + w + 1 > g_aw) g_shelfX = 1, g_shelfY += g_shelfH + 1, g_shelfH = 0;
    *x = g_shelfX;
    *y = g_shelfY;
    g_shelfX += w + 1;
    g_shelfH = std::max(g_shelfH, h);
    int need = g_shelfY + g_shelfH + 1;
    if (need > g_ah) {
        g_ah = std::max(need, g_ah * 2 > 0 ? std::min(g_ah * 2, 8192) : 512);
        if (g_ah < need) g_ah = need;
        g_atlas.resize(static_cast<size_t>(g_aw) * g_ah * 4, 0);
    }
    return true;
}

void Raster(CtFont& f, uint32_t cp) {
    UINT32 c = cp;
    UINT16 gi = 0;
    IDWriteFontFace* face = f.face;
    if (FAILED(face->GetGlyphIndices(&c, 1, &gi))) return;
    for (IDWriteFontFace* fb : f.fallback) {  // not in Segoe UI: the first linked font that has it (as Qt)
        if (gi) break;
        UINT16 fg = 0;
        if (SUCCEEDED(fb->GetGlyphIndices(&c, 1, &fg)) && fg) face = fb, gi = fg;
    }
    if (gi == 0) return;
    DWRITE_FONT_METRICS fm = {};
    face->GetMetrics(&fm);
    int upem = fm.designUnitsPerEm ? fm.designUnitsPerEm : f.upem;
    Glyph g;
    g.index = gi;
    DWRITE_GLYPH_METRICS gm = {};
    face->GetGdiCompatibleGlyphMetrics(static_cast<float>(f.px), 1.0f, nullptr, f.px >= kNaturalFrom, &gi, 1, &gm, FALSE);
    g.adv = static_cast<int>(std::lround(static_cast<double>(gm.advanceWidth) * f.px / upem));
    DWRITE_GLYPH_METRICS dm = {};
    if (SUCCEEDED(face->GetDesignGlyphMetrics(&gi, 1, &dm, FALSE))) {
        g.rsb = static_cast<float>(static_cast<double>(dm.rightSideBearing) * f.px / upem);
        g.rsbDesign = dm.rightSideBearing;
        g.upem = upem;
    }
    FLOAT advance = 0;
    DWRITE_GLYPH_OFFSET off = {};
    DWRITE_GLYPH_RUN run = {};
    run.fontFace = face;
    run.fontEmSize = static_cast<float>(f.px);
    run.glyphCount = 1;
    run.glyphIndices = &gi;
    run.glyphAdvances = &advance;
    run.glyphOffsets = &off;
    IDWriteGlyphRunAnalysis* an = nullptr;
    // from 17 px Qt renders natural symmetric (lab, text_big: 8 of 1 350 000 pixels; GDI natural 44 000)
    bool big = f.px >= kNaturalFrom;
    if (SUCCEEDED(g_dw->CreateGlyphRunAnalysis(&run, 1.0f, nullptr,
                                               big ? DWRITE_RENDERING_MODE_CLEARTYPE_NATURAL_SYMMETRIC
                                                   : DWRITE_RENDERING_MODE_CLEARTYPE_GDI_CLASSIC,
                                               big ? DWRITE_MEASURING_MODE_NATURAL : DWRITE_MEASURING_MODE_GDI_CLASSIC,
                                               0.0f, 0.0f, &an))) {
        RECT r = {};
        if (SUCCEEDED(an->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &r)) && r.right > r.left && r.bottom > r.top) {
            g.w = r.right - r.left;
            g.h = r.bottom - r.top;
            g.ox = r.left;
            g.oy = r.top;
            std::vector<BYTE> rgb(static_cast<size_t>(g.w) * g.h * 3);
            if (SUCCEEDED(an->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &r, rgb.data(), static_cast<UINT32>(rgb.size())))) {
                Place(g.w, g.h, &g.x, &g.y);
                for (int yy = 0; yy < g.h; ++yy)
                    for (int xx = 0; xx < g.w; ++xx) {
                        const BYTE* s = &rgb[(static_cast<size_t>(yy) * g.w + xx) * 3];
                        unsigned char* d = &g_atlas[(static_cast<size_t>(g.y + yy) * g_aw + g.x + xx) * 4];
                        d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = std::max({s[0], s[1], s[2]});
                    }
                Place(g.w, g.h, &g.gx, &g.gy);  // the grayscale picture beside it
                std::vector<BYTE> grey;  // DirectWrite's own grayscale coverage, in the same bounds
                if (g_grayVariant == 1) {
                    IDWriteFactory2* f2 = nullptr;
                    if (SUCCEEDED(g_dw->QueryInterface(__uuidof(IDWriteFactory2), reinterpret_cast<void**>(&f2)))) {
                        IDWriteGlyphRunAnalysis* ga = nullptr;
                        if (SUCCEEDED(f2->CreateGlyphRunAnalysis(&run, nullptr, DWRITE_RENDERING_MODE_GDI_CLASSIC,
                                                                 DWRITE_MEASURING_MODE_GDI_CLASSIC, DWRITE_GRID_FIT_MODE_DEFAULT,
                                                                 DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0.0f, 0.0f, &ga))) {
                            // the grayscale analysis gives one byte a pixel (ALIASED_1x1 is its texture type)
                            std::vector<BYTE> one(static_cast<size_t>(g.w) * g.h, 0);
                            if (SUCCEEDED(ga->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &r, one.data(),
                                                                 static_cast<UINT32>(one.size())))) {
                                grey.resize(one.size() * 3);
                                for (size_t i = 0; i < one.size(); ++i) grey[i * 3] = grey[i * 3 + 1] = grey[i * 3 + 2] = one[i];
                                if (getenv("LAB_GRAY_DEBUG") && cp == 'F') {
                                    int nz = 0;
                                    for (BYTE b : one) nz += b ? 1 : 0;
                                    printf("gray F px %g: %d of %zu set\n", f.px, nz, one.size());
                                }
                            }
                            ga->Release();
                        }
                        f2->Release();
                    }
                }
                const std::vector<BYTE>& src = grey.empty() ? rgb : grey;
                for (int yy = 0; yy < g.h; ++yy)
                    for (int xx = 0; xx < g.w; ++xx) {
                        const BYTE* s = &src[(static_cast<size_t>(yy) * g.w + xx) * 3];
                        int a;
                        if (g_grayVariant == 2) {
                            a = (s[0] + s[1] + s[2]) / 3;   // Qt: where the pixel under it is not opaque
                        } else {
                            int inv = ((255 - s[0]) * 11 + (255 - s[1]) * 16 + (255 - s[2]) * 5) / 32;   // qGray of 255 - c
                            int pg = static_cast<int>(std::lround(std::pow(inv / 255.0, g_grayGamma) * 2047));
                            a = 255 - static_cast<int>(pg * 255.0 / 2047.0);
                        }
                        unsigned char* d = &g_atlas[(static_cast<size_t>(g.gy + yy) * g_aw + g.gx + xx) * 4];
                        d[0] = d[1] = d[2] = 255, d[3] = static_cast<unsigned char>(std::max(0, std::min(255, a)));
                    }
            } else {
                g.w = g.h = 0;
            }
        }
        an->Release();
    }
    f.glyphs[cp] = g;
}

// The kerning of a pair as Qt sets it (HarfBuzz with the font's GPOS kerning, rounded to whole pixels): the width of
// the two letters in DirectWrite's GDI-compatible layout, less their two advances. Asked once per pair, then kept.
int Kern(CtFont& f, uint32_t a, uint32_t b, const Glyph& ga, const Glyph& gb) {
    uint64_t key = (static_cast<uint64_t>(a) << 32) | b;
    auto it = f.kern.find(key);
    if (it != f.kern.end()) return it->second;
    int px = 0;
    if (!f.format)
        g_dw->CreateTextFormat(f.style == 2 ? L"Consolas" : L"Segoe UI", nullptr,
                               f.style == 1 ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(f.px), L"en-us",
                               &f.format);
    wchar_t pair[4] = {};
    int n = 0;
    for (uint32_t cp : {a, b}) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            pair[n++] = static_cast<wchar_t>(0xD800 + (cp >> 10));
            pair[n++] = static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
        } else {
            pair[n++] = static_cast<wchar_t>(cp);
        }
    }
    // the kerning unrounded: the ideal layout's width of the pair less the widths of its two letters
    auto width = [&](const wchar_t* t, int len) {
        IDWriteTextLayout* lay = nullptr;
        double w = 0;
        if (f.format && SUCCEEDED(g_dw->CreateTextLayout(t, len, f.format, 10000.0f, 1000.0f, &lay))) {
            DWRITE_TEXT_METRICS m = {};
            if (SUCCEEDED(lay->GetMetrics(&m))) w = m.widthIncludingTrailingWhitespace;
            lay->Release();
        }
        return w;
    };
    int na = (a >= 0x10000) ? 2 : 1;
    double k = width(pair, n) - width(pair, na) - width(pair + na, n - na);
    px = static_cast<int>(std::floor(k + 0.5));
    (void)ga;
    (void)gb;
    if (px < -f.px || px > f.px) px = 0;
    f.kern[key] = px;
    return px;
}

CtFont* Find(float px, int style) {
    CtFont* best = nullptr;
    float bestd = 1e30f;
    for (CtFont& f : g_ct) {
        if ((f.style == 2) != (style == 2)) continue;  // (Consolas only for Consolas)
        float d = std::fabs(f.px - px) * 2 + (f.style != style ? 1 : 0);
        if (d < bestd) bestd = d, best = &f;
    }
    return best;
}

// UTF-8 -> code points
uint32_t Next(const char*& s, const char* e) {
    unsigned char c = static_cast<unsigned char>(*s++);
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3F >> n);
    while (n-- > 0 && s < e) cp = (cp << 6) | (static_cast<unsigned char>(*s++) & 0x3F);
    return cp;
}
const Glyph* GlyphOf(CtFont& f, uint32_t cp) {
    auto it = f.glyphs.find(cp);
    if (it != f.glyphs.end()) return &it->second;
    it = f.glyphs.find('?');
    return it == f.glyphs.end() ? nullptr : &it->second;
}

// The pen positions of a line, as Qt sets them (whole pixels: advances plus kerning). -> its width.
int Layout(CtFont& f, const char* s, const char* e, std::vector<std::pair<const Glyph*, int>>* out) {
    int x = 0;
    const Glyph* prev = nullptr;
    uint32_t prevCp = 0;
    while (s < e) {
        uint32_t cp = Next(s, e);
        if (cp == '\n' || cp == '\r') continue;
        const Glyph* g = GlyphOf(f, cp);
        if (!g) continue;
        if (prev) x += Kern(f, prevCp, cp, *prev, *g);
        if (out) out->emplace_back(g, x);
        x += g->adv;
        prev = g;
        prevCp = cp;
    }
    return x;
}

struct TextParams {  // the host's text shader reads it (UiTextCallback)
    float colour[4];
    float background[4];
    float gamma[4];
};

}  // namespace

// The families Windows links to Segoe UI (HKLM ... FontLink\SystemLink, "FILE,Family[,scale]" each), in order,
// once each; Segoe UI Symbol last if the list does not name it.
std::vector<std::wstring> LinkedFamilies() {
    std::vector<std::wstring> out;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\FontLink\\SystemLink", 0, KEY_READ,
                      &k) == ERROR_SUCCESS) {
        DWORD type = 0, size = 0;
        if (RegQueryValueExW(k, L"Segoe UI", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_MULTI_SZ) {
            std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, L'\0');
            RegQueryValueExW(k, L"Segoe UI", nullptr, nullptr, reinterpret_cast<BYTE*>(buf.data()), &size);
            for (const wchar_t* e = buf.data(); *e; e += wcslen(e) + 1) {
                std::wstring entry = e;
                size_t a = entry.find(L',');
                if (a == std::wstring::npos) continue;
                size_t b = entry.find(L',', a + 1);
                std::wstring fam = entry.substr(a + 1, b == std::wstring::npos ? std::wstring::npos : b - a - 1);
                if (std::find(out.begin(), out.end(), fam) == out.end()) out.push_back(fam);
            }
        }
        RegCloseKey(k);
    }
    if (std::find(out.begin(), out.end(), L"Segoe UI Symbol") == out.end()) out.push_back(L"Segoe UI Symbol");
    return out;
}

namespace {
// The fonts and the atlas, without the texture. -> fonts made.
int BuildTextFonts() {
    if (g_dw) return static_cast<int>(g_ct.size());
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_dw))))
        return 0;
    UINT contrast = 0;
    if (SystemParametersInfoW(SPI_GETFONTSMOOTHINGCONTRAST, 0, &contrast, 0) && contrast >= 1000 && contrast <= 2200)
        g_gamma = contrast / 1000.0f;
    IDWriteFontCollection* coll = nullptr;
    g_dw->GetSystemFontCollection(&coll);
    UINT32 fam = 0;
    BOOL exists = FALSE;
    if (!coll || FAILED(coll->FindFamilyName(L"Segoe UI", &fam, &exists)) || !exists) return 0;
    IDWriteFontFamily* family = nullptr;
    coll->GetFontFamily(fam, &family);
    g_atlas.assign(static_cast<size_t>(g_aw) * 512 * 4, 0);
    g_ah = 512;
    for (bool bold : {false, true}) {
        IDWriteFont* font = nullptr;
        family->GetFirstMatchingFont(bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                     DWRITE_FONT_STYLE_NORMAL, &font);
        if (!font) continue;
        IDWriteFontFace* face = nullptr;
        font->CreateFontFace(&face);
        font->Release();
        if (!face) continue;
        std::vector<IDWriteFontFace*> linked;  // Windows' font link list for Segoe UI, in its order
        for (const std::wstring& name : LinkedFamilies()) {
            UINT32 lf = 0;
            BOOL lex = FALSE;
            if (FAILED(coll->FindFamilyName(name.c_str(), &lf, &lex)) || !lex) continue;
            IDWriteFontFamily* fam2 = nullptr;
            coll->GetFontFamily(lf, &fam2);
            IDWriteFont* lfont = nullptr;  // bold: DirectWrite simulates it where the family has none (as Qt draws it)
            if (fam2) fam2->GetFirstMatchingFont(bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                                 DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &lfont);
            IDWriteFontFace* lface = nullptr;
            if (lfont) lfont->CreateFontFace(&lface), lfont->Release();
            if (fam2) fam2->Release();
            if (lface) linked.push_back(lface);
        }
        // (11.33: 8.5 pt, the Asset Browser's tiles - Qt's pixel size of a point size, rounded to 1/100 px). And the
        // sizes between, for text_scaled (a zoomed view; drawn smaller from the next size it would be blurred):
        // 4 to 8.75 px in quarters, 9.5 to 27.5 in halves, 25 and 27
        std::vector<float> sizes = {9.f, 10.f, 11.f, 11.33f, 12.f, 13.f, 14.f, 15.f, 16.f, 17.f, 18.f, 19.f, 20.f, 21.f,
                                    22.f, 23.f, 24.f, 26.f, 28.f, 25.f, 27.f};
        for (float q = 4.f; q < 8.9f; q += 0.25f) sizes.push_back(q);
        for (float q = 9.5f; q < 28.f; q += 1.f) sizes.push_back(q);
        for (float px : sizes) {
            CtFont f;
            f.px = px;
            f.style = bold ? 1 : 0;
            f.face = face;
            f.fallback = linked;
            face->QueryInterface(__uuidof(IDWriteFontFace1), reinterpret_cast<void**>(&f.face1));
            DWRITE_FONT_METRICS m = {};
            face->GetMetrics(&m);  // Qt rounds the design ascent and descent (the GDI-compatible ones are 1 px short at 11 and 12 px)
            f.upem = m.designUnitsPerEm;
            f.ascent = static_cast<int>(std::lround(static_cast<double>(m.ascent) * px / f.upem));
            f.descent = static_cast<int>(std::lround(static_cast<double>(m.descent) * px / f.upem));
            {
                const void* data = nullptr;
                UINT32 size = 0;
                void* ctx = nullptr;
                BOOL exists = FALSE;
                if (SUCCEEDED(face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('h', 'h', 'e', 'a'), &data, &size, &ctx, &exists))) {
                    if (exists && size >= 14) {
                        const unsigned char* t = static_cast<const unsigned char*>(data);
                        f.minRsb = static_cast<int16_t>((t[12] << 8) | t[13]);
                    }
                    face->ReleaseFontTable(ctx);
                }
            }
            for (const auto& r : kRanges)
                for (uint32_t cp = r[0]; cp <= r[1]; ++cp) Raster(f, cp);
            g_ct.push_back(std::move(f));
        }
    }
    family->Release();
    // Consolas for a QPlainTextEdit's log (the build window): 11 to 13 px, normal only, no linked fonts
    UINT32 cfam = 0;
    BOOL cex = FALSE;
    IDWriteFontFamily* mono = nullptr;
    if (SUCCEEDED(coll->FindFamilyName(L"Consolas", &cfam, &cex)) && cex) coll->GetFontFamily(cfam, &mono);
    IDWriteFont* mfont = nullptr;
    if (mono) mono->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &mfont);
    IDWriteFontFace* mface = nullptr;
    if (mfont) mfont->CreateFontFace(&mface), mfont->Release();
    if (mono) mono->Release();
    for (float px : {11.f, 12.f, 13.f}) {
        if (!mface) break;
        CtFont f;
        f.px = px;
        f.style = 2;
        f.face = mface;
        mface->QueryInterface(__uuidof(IDWriteFontFace1), reinterpret_cast<void**>(&f.face1));
        DWRITE_FONT_METRICS m = {};
        mface->GetMetrics(&m);
        f.upem = m.designUnitsPerEm;
        f.ascent = static_cast<int>(std::lround(static_cast<double>(m.ascent) * px / f.upem));
        f.descent = static_cast<int>(std::lround(static_cast<double>(m.descent) * px / f.upem));
        {
            const void* data = nullptr;
            UINT32 size = 0;
            void* ctx = nullptr;
            BOOL exists = FALSE;
            if (SUCCEEDED(mface->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('h', 'h', 'e', 'a'), &data, &size, &ctx, &exists))) {
                if (exists && size >= 14) {
                    const unsigned char* t = static_cast<const unsigned char*>(data);
                    f.minRsb = static_cast<int16_t>((t[12] << 8) | t[13]);
                }
                mface->ReleaseFontTable(ctx);
            }
        }
        for (const auto& r : kRanges)
            for (uint32_t cp = r[0]; cp <= r[1]; ++cp) Raster(f, cp);
        g_ct.push_back(std::move(f));
    }
    coll->Release();
    return static_cast<int>(g_ct.size());
}

// The texture of what the thread built (main thread). -> the fonts are ready.
bool TextReady() {
    int s = g_state.load(std::memory_order_acquire);
    if (s == 3) return true;
    if (s != 2) return false;
    g_tex = UiCreateTexture(g_aw, g_ah, g_atlas.data());
    std::vector<unsigned char>().swap(g_atlas);  // the texture keeps its own copy, and nothing is rasterised later
    g_state.store(3, std::memory_order_release);
    if (g_uiLog) g_uiLog("ui: text fonts ready (%d, atlas %dx%d, built in %llu ms on their own thread)", static_cast<int>(g_ct.size()),
        g_aw, g_ah, g_buildMs);
    return true;
}
}  // namespace

// Made once, at once (the host calls it after it can make textures). -> fonts made.
int UiMakeTextFonts() {
    if (g_state.load() == 0) {
        g_state = 1;
        BuildTextFonts();
        g_state = 2;
    }
    while (g_state.load() == 1) Sleep(10);  // the thread of UiMakeTextFontsLater is at it
    TextReady();
    return static_cast<int>(g_ct.size());
}

// The same on a thread of its own: rasterising every size takes seconds, and the main thread would stand still
// for them. The text functions say "no font" until the first call on the main thread after the build.
void UiMakeTextFontsLater() {
    int expect = 0;
    if (!g_state.compare_exchange_strong(expect, 1)) return;
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        ULONGLONG t0 = GetTickCount64();
        __try {
            BuildTextFonts();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_ct.clear();
        }
        g_buildMs = GetTickCount64() - t0;
        g_state.store(2, std::memory_order_release);
    }).detach();
}

void UiSetTextGamma(float gamma, int mode) {
    if (gamma > 0) g_gamma = gamma;
    g_gammaMode = mode;
}
void UiSetGrayGamma(float gamma, int variant) {  // before UiMakeTextFonts (the lab's comparison)
    if (gamma > 0) g_grayGamma = gamma;
    if (variant >= 0) g_grayVariant = variant;
}

// Qt's metrics of a font: ascent, descent, line height (ascent + descent).
bool UiTextMetrics(float px, int bold, int* ascent, int* descent) {
    if (!TextReady()) return false;
    CtFont* f = Find(px, bold);
    if (!f) return false;
    *ascent = f->ascent;
    *descent = f->descent;
    return true;
}

// The pen position before each letter and after the last (whole pixels, kerning in), and the byte where each starts:
// what a text field needs for its cursor and selection. -> count of positions.
int UiTextOffsets(const char* s, unsigned long long len, float px, int bold, std::vector<int>* xs, std::vector<int>* bytes) {
    CtFont* f = TextReady() ? Find(px, bold) : nullptr;
    xs->clear();
    bytes->clear();
    if (!f) return 0;
    const char* p = s;
    const char* e = s + len;
    int x = 0;
    const Glyph* prev = nullptr;
    uint32_t prevCp = 0;
    while (p < e) {
        const char* start = p;
        uint32_t cp = Next(p, e);
        const Glyph* g = GlyphOf(*f, cp);
        if (prev && g) x += Kern(*f, prevCp, cp, *prev, *g);
        xs->push_back(x);
        bytes->push_back(static_cast<int>(start - s));
        if (g) x += g->adv;
        prev = g;
        prevCp = cp;
    }
    xs->push_back(x);
    bytes->push_back(static_cast<int>(len));
    return static_cast<int>(xs->size());
}

// Each letter's right bearing as Qt's DirectWrite font engine has it (boundingBox: the design one, in 1/64 px cut
// towards zero - QFixed::fromReal), and the font's least (hhea: QFontEngine::minRightBearing), for the line breaker.
int UiTextBearings(const char* s, unsigned long long len, float px, int bold, std::vector<int>* rb, int* minRb) {
    CtFont* f = TextReady() ? Find(px, bold) : nullptr;
    rb->clear();
    *minRb = 0;
    if (!f) return 0;
    const double size = static_cast<double>(f->px);
    *minRb = static_cast<int>(static_cast<double>(f->minRsb) * (size / f->upem) * 64.0);
    const char* p = s;
    const char* e = s + len;
    while (p < e) {
        uint32_t cp = Next(p, e);
        const Glyph* g = GlyphOf(*f, cp);
        rb->push_back(g ? static_cast<int>((static_cast<double>(g->rsbDesign) / static_cast<double>(g->upem)) * size * 64.0) : 0);
    }
    return static_cast<int>(rb->size());
}

// The width Qt's size hints use (QFontMetrics::size): the advance, and the last letter's overhang past it rounded up.
int UiTextHintWidth(const char* s, unsigned long long len, float px, int bold) {
    CtFont* f = TextReady() ? Find(px, bold) : nullptr;
    if (!f) return 0;
    std::vector<std::pair<const Glyph*, int>> line;
    int w = Layout(*f, s, s + len, &line);
    if (!line.empty() && line.back().first->rsb < -1.0f / 64) w += static_cast<int>(std::ceil(-line.back().first->rsb - 1.0f / 64));  // Qt counts in 1/64 px
    return w;
}

int UiTextWidth(const char* s, unsigned long long len, float px, int bold) {
    CtFont* f = TextReady() ? Find(px, bold) : nullptr;
    return f ? Layout(*f, s, s + len, nullptr) : 0;
}

// Text at any size, a zoomed view's (a node graph): the sizes above are made with DirectWrite
// one by one; any other is the next bigger one's grayscale letters, drawn smaller (or the biggest one's, larger) -
// a little softer than letters made at that size, never a size that jumps while the view zooms.
bool UiDrawTextScaled(ImDrawList* dl, float x, float y, unsigned int colour, const char* s, unsigned long long len,
                      float px, int bold) {
    if (!TextReady() || !g_tex || px <= 0) return false;
    CtFont* f = nullptr;
    for (CtFont& c : g_ct) {  // the nearest size made (same style): drawn at most a few percent smaller or larger
        if (c.style != (bold ? 1 : 0)) continue;
        if (!f || std::fabs(c.px - px) < std::fabs(f->px - px)) f = &c;
    }
    if (!f) return false;
    float k = px / f->px;
    x = std::floor(x + 0.5f);  // (whole pixels: the letters stay sharp)
    y = std::floor(y + 0.5f);
    std::vector<std::pair<const Glyph*, int>> line;
    Layout(*f, s, s + len, &line);
    ImU32 col = IM_COL32((colour >> 24) & 255, (colour >> 16) & 255, (colour >> 8) & 255, colour & 255);
    ImTextureID tex = static_cast<ImTextureID>(g_tex);
    float iw = 1.0f / g_aw, ih = 1.0f / g_ah;
    float by = y + std::floor(f->ascent * k + 0.5f);
    for (const auto& [g, gx] : line) {
        if (!g->w) continue;
        float x0 = x + std::floor((gx + g->ox) * k + 0.5f), y0 = by + std::floor(g->oy * k + 0.5f);
        dl->AddImage(tex, ImVec2(x0, y0), ImVec2(x0 + g->w * k, y0 + g->h * k), ImVec2(g->gx * iw, g->gy * ih),
                     ImVec2((g->gx + g->w) * iw, (g->gy + g->h) * ih), col);
    }
    return true;
}

// Draws a line of text with its top at y (Qt's text rectangle), in colour over the background colour bg (the text
// shader blends towards it like Qt's gamma-correct blending; 0 = whatever is there, plain blending).
bool UiDrawText(ImDrawList* dl, float x, float y, unsigned int colour, unsigned int bg, const char* s, unsigned long long len,
                float px, int bold, bool gray) {
    CtFont* f = TextReady() ? Find(px, bold) : nullptr;
    if (!f || !g_tex) return false;
    std::vector<std::pair<const Glyph*, int>> line;
    Layout(*f, s, s + len, &line);
    if (gray) {  // a translucent window's text: the grayscale pictures with plain alpha (ImGui's own blend)
        float bx = std::floor(x + 0.5f), by = std::floor(y + 0.5f) + f->ascent;
        ImU32 col = IM_COL32((colour >> 24) & 255, (colour >> 16) & 255, (colour >> 8) & 255, colour & 255);
        ImTextureID tex = static_cast<ImTextureID>(g_tex);
        float iw = 1.0f / g_aw, ih = 1.0f / g_ah;
        for (const auto& [g, gx] : line) {
            if (!g->w) continue;
            float x0 = bx + gx + g->ox, y0 = by + g->oy;
            dl->AddImage(tex, ImVec2(x0, y0), ImVec2(x0 + g->w, y0 + g->h), ImVec2(g->gx * iw, g->gy * ih),
                         ImVec2((g->gx + g->w) * iw, (g->gy + g->h) * ih), col);
        }
        return true;
    }
    auto unpack = [](uint32_t c, float* o) {
        o[0] = ((c >> 24) & 255) / 255.0f, o[1] = ((c >> 16) & 255) / 255.0f, o[2] = ((c >> 8) & 255) / 255.0f,
        o[3] = (c & 255) / 255.0f;
    };
    TextParams p = {};
    unpack(colour, p.colour);
    unpack(bg, p.background);
    p.gamma[0] = bg ? g_gamma : 1.0f;
    p.gamma[1] = bg ? static_cast<float>(g_gammaMode) : 0.0f;
    dl->AddCallback(UiTextCallback(), &p, sizeof p);
    float bx = std::floor(x + 0.5f), by = std::floor(y + 0.5f) + f->ascent;
    ImU32 col = IM_COL32((colour >> 24) & 255, (colour >> 16) & 255, (colour >> 8) & 255, colour & 255);
    ImTextureID tex = static_cast<ImTextureID>(g_tex);
    float iw = 1.0f / g_aw, ih = 1.0f / g_ah;
    for (const auto& [g, gx] : line) {
        if (!g->w) continue;
        float x0 = bx + gx + g->ox, y0 = by + g->oy;
        dl->AddImage(tex, ImVec2(x0, y0), ImVec2(x0 + g->w, y0 + g->h), ImVec2(g->x * iw, g->y * ih),
                     ImVec2((g->x + g->w) * iw, (g->y + g->h) * ih), col);
    }
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    return true;
}

}  // namespace tw3se
