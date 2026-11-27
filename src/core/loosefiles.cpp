// Loose files: every file under tw3se\files\ (by its depot path, e.g.
// tw3se\files\characters\npc_entities\main_npc\triss.w2ent) replaces the game's file of that path.
//
// At the start, before the game scans its mods, TW3SE packs them into a bundle of a mod of its own,
// Mods\mod00000_TW3SE_Files\content\ (blob0.bundle and metadata.store), the way wcc_lite does. The name sorts before
// every other mod, so its files come first. The files go in as they are (uncompressed); the bundle is built again
// only when a file was added, changed or removed (a list of paths, sizes and times beside it). The two formats as
// wcc_lite writes them (measured on its output):
//
//   bundle: header 0x20 ('POTATO70', u32 size rounded up to 16, u32 0, u32 table size, u16 5, u16 data start,
//           8 zero bytes); a table of 0x130 per file (path 0x100, hash 16 zero bytes, u64 offset, u32 size,
//           u32 stored size, u32 crc32, u32 compression); the data in the reverse order of the table, each file at
//           a 16-byte boundary.
//   metadata.store: '\x03VTM', u32 version 7, u32 largest stored size, u32 largest size, a string table and seven
//           arrays (file infos, entries, bundles, buffers, directories, file inits, hashes), counts as CR2W VLQs.
#include "core.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace tw3se {
namespace {

const wchar_t* kModName = L"mod00000_TW3SE_Files";

struct LooseFile {
    std::string path;  // depot path, lower case, backslashes
    std::wstring full;
    uint64_t size = 0;
    uint64_t time = 0;
    // filled while packing
    uint64_t offset = 0;
    uint32_t crc = 0;
};

uint32_t g_crcTable[256];
void CrcInit() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crcTable[i] = c;
    }
}
uint32_t Crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = g_crcTable[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
uint64_t Fnv64(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char ch : s) {
        h ^= ch;
        h *= 0x100000001b3ull;
    }
    return h;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c < 128) ? static_cast<char>(tolower(c)) : '?';
    return s;
}

void Walk(const std::wstring& root, const std::wstring& rel, std::vector<LooseFile>& out, std::string& bad) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + rel + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            Walk(root, rel + name + L"\\", out, bad);
            continue;
        }
        LooseFile f;
        f.full = root + rel + name;
        f.path = Narrow(rel + name);
        f.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        f.time = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        if (f.path.find('?') != std::string::npos || f.path.size() >= 0x100) {
            bad += " " + f.path;  // a bundle holds ASCII paths below 256 characters
            continue;
        }
        out.push_back(std::move(f));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void PutVlq(std::vector<uint8_t>& b, int64_t value) {  // CR2W's: 6 bits, 0x40 more, 0x80 negative; then 7 bits
    bool neg = value < 0;
    uint64_t v = static_cast<uint64_t>(neg ? -value : value);
    uint8_t first = static_cast<uint8_t>(v & 0x3F);
    v >>= 6;
    b.push_back(first | (neg ? 0x80 : 0) | (v ? 0x40 : 0));
    while (v) {
        uint8_t x = static_cast<uint8_t>(v & 0x7F);
        v >>= 7;
        b.push_back(x | (v ? 0x80 : 0));
    }
}
template <class T> void Put(std::vector<uint8_t>& b, T v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof v);
}

// metadata.store for one bundle, as wcc_lite writes it (the files in the order of their data)
std::vector<uint8_t> MetadataStore(const std::vector<LooseFile>& files, uint32_t dataStart, uint32_t bundleSize) {
    std::vector<const LooseFile*> byData;
    for (const LooseFile& f : files) byData.push_back(&f);
    std::sort(byData.begin(), byData.end(), [](const LooseFile* a, const LooseFile* b) { return a->offset < b->offset; });
    std::vector<uint8_t> strings(1, 0);
    auto intern = [&](const std::string& s) {
        uint32_t off = static_cast<uint32_t>(strings.size());
        strings.insert(strings.end(), s.begin(), s.end());
        strings.push_back(0);
        return off;
    };
    uint32_t bundleName = intern("blob0.bundle");
    std::vector<uint32_t> pathOff;
    for (const LooseFile* f : byData) pathOff.push_back(intern(f->path));
    uint32_t root = intern("");
    std::vector<std::pair<int32_t, int32_t>> dirs{{static_cast<int32_t>(root), 0}};
    std::map<std::pair<int32_t, std::string>, int32_t> children;
    struct Init { int32_t file, dir, leaf; };
    std::vector<Init> inits;
    for (size_t i = 0; i < byData.size(); ++i) {
        const std::string& p = byData[i]->path;
        int32_t cur = 0;
        size_t start = 0, slash;
        while ((slash = p.find('\\', start)) != std::string::npos) {
            std::string comp = p.substr(start, slash - start);
            auto key = std::make_pair(cur, comp);
            auto it = children.find(key);
            if (it == children.end()) {
                int32_t id = static_cast<int32_t>(dirs.size());
                children[key] = id;
                dirs.push_back({static_cast<int32_t>(intern(comp)), cur});
                cur = id;
            } else {
                cur = it->second;
            }
            start = slash + 1;
        }
        inits.push_back({static_cast<int32_t>(i + 1), cur, static_cast<int32_t>(pathOff[i] + start)});
    }
    uint32_t maxSize = 0;
    for (const LooseFile* f : byData) maxSize = (std::max)(maxSize, static_cast<uint32_t>(f->size));
    std::vector<uint8_t> b = {0x03, 'V', 'T', 'M'};
    Put<uint32_t>(b, 7);
    Put<uint32_t>(b, maxSize);  // largest stored size (stored as they are)
    Put<uint32_t>(b, maxSize);
    PutVlq(b, static_cast<int64_t>(strings.size()));
    b.insert(b.end(), strings.begin(), strings.end());
    // file infos: name, path hash, stored size, size, first entry, buffer id, has buffer, compression. A file with
    // buffers of its own (a mesh: <path>.1.buffer, .2.buffer ... beside it) points at them in the buffers below - the
    // game finds a mesh's vertices only through them (27.11.: a mesh in tw3se\files drew nothing without; measured on
    // a mod bundle wcc_lite made: the mesh's buffer id is its first buffer's place in the list, "has buffer" 1)
    std::map<std::string, uint32_t> fileOf;
    for (size_t i = 0; i < byData.size(); ++i) fileOf[byData[i]->path] = static_cast<uint32_t>(i + 1);
    std::vector<uint32_t> buffers;
    PutVlq(b, static_cast<int64_t>(byData.size() + 1));
    for (int k = 0; k < 8; ++k) Put<uint32_t>(b, 0);
    for (size_t i = 0; i < byData.size(); ++i) {
        uint32_t s = static_cast<uint32_t>(byData[i]->size);
        uint32_t bufferId = 0, hasBuffer = 0;
        for (int n = 1;; ++n) {
            auto f = fileOf.find(byData[i]->path + "." + std::to_string(n) + ".buffer");
            if (f == fileOf.end()) break;
            if (n == 1) bufferId = static_cast<uint32_t>(buffers.size()), hasBuffer = 1;
            buffers.push_back(f->second);
        }
        for (uint32_t v : {pathOff[i], 0u, s, s, static_cast<uint32_t>(i + 1), bufferId, hasBuffer, 0u}) Put<uint32_t>(b, v);
    }
    // entries: u64 offset, stored size, next entry, file, bundle
    PutVlq(b, static_cast<int64_t>(byData.size() + 1));
    Put<uint64_t>(b, 0);
    for (int k = 0; k < 4; ++k) Put<uint32_t>(b, 0);
    for (size_t i = 0; i < byData.size(); ++i) {
        Put<uint64_t>(b, byData[i]->offset);
        for (uint32_t v : {static_cast<uint32_t>(byData[i]->size), 0u, static_cast<uint32_t>(i + 1), 1u}) Put<uint32_t>(b, v);
    }
    // bundles: u64 data block size, data block offset, name, first entry, entries
    PutVlq(b, 2);
    Put<uint64_t>(b, 0);
    for (int k = 0; k < 4; ++k) Put<uint32_t>(b, 0);
    Put<uint64_t>(b, bundleSize - dataStart);
    for (uint32_t v : {dataStart, bundleName, byData.empty() ? 0u : 1u, static_cast<uint32_t>(byData.size())}) Put<uint32_t>(b, v);
    if (buffers.empty()) {
        b.push_back(0x80);  // buffers: none (wcc_lite writes the empty count as a negative zero)
    } else {
        PutVlq(b, static_cast<int64_t>(buffers.size()));
        for (uint32_t f : buffers) Put<uint32_t>(b, f);
    }
    PutVlq(b, static_cast<int64_t>(dirs.size()));
    for (auto& d : dirs) {
        Put<int32_t>(b, d.first);
        Put<int32_t>(b, d.second);
    }
    PutVlq(b, static_cast<int64_t>(inits.size()));
    for (auto& in : inits) {
        Put<int32_t>(b, in.file);
        Put<int32_t>(b, in.dir);
        Put<int32_t>(b, in.leaf);
    }
    // hashes: fnv64 of the path and the file (high half 0 for the first file, -1 for the others), sorted unsigned
    std::vector<std::pair<uint64_t, int64_t>> hashes;
    for (size_t i = 0; i < byData.size(); ++i) {
        uint32_t hi = i == 0 ? 0 : 0xFFFFFFFFu;
        int64_t file = static_cast<int64_t>((static_cast<uint64_t>(hi) << 32) | static_cast<uint32_t>(i + 1));
        hashes.push_back({Fnv64(byData[i]->path), file});
    }
    std::sort(hashes.begin(), hashes.end());
    PutVlq(b, static_cast<int64_t>(hashes.size()));
    for (auto& h : hashes) {
        Put<uint64_t>(b, h.first);
        Put<int64_t>(b, h.second);
    }
    return b;
}

bool WriteAll(const std::wstring& path, const std::vector<uint8_t>& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &w, nullptr) && w == data.size();
    CloseHandle(h);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD r = 0;
    bool ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &r, nullptr) && r == out.size());
    CloseHandle(h);
    return ok;
}

std::wstring GameRoot() {  // Dir() is <game>\bin\x64_dx12\tw3se
    std::wstring d = Dir();
    for (int i = 0; i < 3; ++i) {
        size_t s = d.find_last_of(L'\\');
        if (s == std::wstring::npos) return L"";
        d.resize(s);
    }
    return d;
}

// --- textures: a DDS file under the depot path of a game texture (tw3se\files\...\body_d.dds for ...\body_d.xbm)
// replaces its pixels. The pixels of a texture are in the game's texture.cache files, found by a key the .xbm holds;
// a mod's texture.cache comes before the game's. TW3SE writes one with an entry under the game entry's key (layout
// in tools\texcache.py, rebuilt byte for byte from the game's caches there). The DDS has to have the format, size and
// mip count of the game's texture.
#pragma pack(push, 1)
struct TexEntry {
    uint32_t key;
    int32_t nameOffset;
    uint32_t page, stored, size, align;
    uint16_t width, height, mips, slices;
    int32_t firstPiece, pieces;
    int64_t time;
    uint8_t format, format2, cube, pad;
};
struct TexFooter {
    uint64_t crc;
    uint32_t pages, count, namesSize, tableCount, ident, version;
};
#pragma pack(pop)
static_assert(sizeof(TexEntry) == 52 && sizeof(TexFooter) == 32, "texture.cache layout");

uint32_t BlockBytes(uint8_t format) {  // per 4x4 block; 0 for the uncompressed RGBA one
    switch (format) {
        case 7: case 14: return 8;
        case 8: case 13: case 10: case 15: return 16;
        default: return 0;
    }
}
uint32_t MipBytes(uint8_t format, uint32_t w, uint32_t h) {
    uint32_t b = BlockBytes(format);
    return b ? ((w + 3) / 4 > 0 ? (w + 3) / 4 : 1) * ((h + 3) / 4 > 0 ? (h + 3) / 4 : 1) * b : w * h * 4;
}

// The game's entries of the wanted textures (lower-case names; the first cache that has one wins): every
// texture.cache of the game and its DLC, index only (read from the end), each once.
void FindGameTextures(const std::wstring& root, std::map<std::string, TexEntry>& want,
                      std::map<std::string, bool>& found) {
    std::vector<std::wstring> caches;
    for (const wchar_t* pattern : {L"\\content\\content*", L"\\dlc\\*"}) {
        std::wstring base = root + pattern;
        base.resize(base.find_last_of(L'\\') + 1);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((root + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.cFileName[0] == L'.' || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            std::wstring dir = base + fd.cFileName;
            caches.push_back(dir + (wcsstr(pattern, L"dlc") ? L"\\content\\texture.cache" : L"\\texture.cache"));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (const std::wstring& c : caches) {
        HANDLE h = CreateFileW(c.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER size{}, at{};
        TexFooter f{};
        DWORD r = 0;
        GetFileSizeEx(h, &size);
        at.QuadPart = size.QuadPart - 32;
        bool ok = SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && ReadFile(h, &f, 32, &r, nullptr) && r == 32 &&
                  f.ident == 0x54584348 && f.count < 10000000 && f.namesSize < 0x10000000;
        std::vector<uint8_t> idx;
        if (ok) {
            idx.resize(static_cast<size_t>(f.namesSize) + static_cast<size_t>(f.count) * 52);
            at.QuadPart = size.QuadPart - 32 - static_cast<LONGLONG>(idx.size());
            ok = SetFilePointerEx(h, at, nullptr, FILE_BEGIN) &&
                 ReadFile(h, idx.data(), static_cast<DWORD>(idx.size()), &r, nullptr) && r == idx.size();
        }
        CloseHandle(h);
        if (!ok) continue;
        const char* names = reinterpret_cast<const char*>(idx.data());
        for (uint32_t i = 0; i < f.count; ++i) {
            TexEntry e;
            memcpy(&e, idx.data() + f.namesSize + i * 52, 52);
            if (e.nameOffset < 0 || static_cast<uint32_t>(e.nameOffset) >= f.namesSize) continue;
            const char* n = names + e.nameOffset;
            size_t len = strnlen(n, f.namesSize - e.nameOffset);
            std::string key(n, len);
            for (char& ch : key) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            auto it = want.find(key);
            if (it != want.end() && !found[key]) {
                it->second = e;
                found[key] = true;
            }
        }
    }
}

// zlib with stored blocks: any zlib reader takes it, and it costs nothing to make
void ZStore(std::vector<uint8_t>& out, const uint8_t* p, size_t n) {
    out.push_back(0x78);
    out.push_back(0x01);
    size_t i = 0;
    do {
        size_t part = n - i < 0xffff ? n - i : 0xffff;
        bool last = i + part >= n;
        uint16_t len = static_cast<uint16_t>(part), nlen = static_cast<uint16_t>(~len);
        out.push_back(last ? 1 : 0);
        out.insert(out.end(), reinterpret_cast<uint8_t*>(&len), reinterpret_cast<uint8_t*>(&len) + 2);
        out.insert(out.end(), reinterpret_cast<uint8_t*>(&nlen), reinterpret_cast<uint8_t*>(&nlen) + 2);
        out.insert(out.end(), p + i, p + i + part);
        i += part;
    } while (i < n);
    uint32_t a = 1, b = 0;
    for (size_t k = 0; k < n; ++k) {
        a = (a + p[k]) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(adler >> s));
}

struct DdsInfo {
    uint8_t format = 0;
    uint32_t width = 0, height = 0, mips = 0;
    size_t start = 0;
};
bool ReadDds(const std::vector<uint8_t>& d, DdsInfo& out, std::string& err) {
    if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) return err = "not a DDS file", false;
    uint32_t flags, bits;
    char fourcc[5] = {};
    memcpy(&out.height, &d[12], 4);
    memcpy(&out.width, &d[16], 4);
    memcpy(&out.mips, &d[28], 4);
    if (!out.mips) out.mips = 1;
    memcpy(&flags, &d[80], 4);
    memcpy(fourcc, &d[84], 4);
    memcpy(&bits, &d[88], 4);
    out.start = 128;
    std::string fc = fourcc;
    if (fc == "DXT1") out.format = 7;
    else if (fc == "DXT5") out.format = 8;
    else if (fc == "DXT3") out.format = 13;
    else if (fc == "ATI1" || fc == "BC4U") out.format = 14;
    else if (fc == "ATI2" || fc == "BC5U") out.format = 15;
    else if (fc == "DX10" && d.size() >= 148) {
        uint32_t dxgi;
        memcpy(&dxgi, &d[128], 4);
        out.start = 148;
        out.format = dxgi == 98 || dxgi == 99 ? 10 : dxgi == 71 || dxgi == 72 ? 7 : dxgi == 77 || dxgi == 78 ? 8
                   : dxgi == 74 ? 13 : dxgi == 80 ? 14 : dxgi == 83 ? 15 : 0;
    } else if (!(flags & 4) && bits == 32) {
        out.format = 253;
    }
    if (!out.format) return err = "a DDS format the game does not cache (" + fc + ")", false;
    return true;
}

// The texture cache key of a texture of its own (a DDS under a path the game has no texture for): FNV-1a 32 over its
// .xbm path, lower case. tools\new_texture.py writes the same key into the .xbm.
uint32_t OwnTextureKey(const std::string& xbmPath) {
    uint32_t h = 0x811c9dc5u;
    for (unsigned char ch : xbmPath) {
        h ^= static_cast<unsigned char>(tolower(ch));
        h *= 0x01000193u;
    }
    return h;
}

// One texture's data: its pieces (one per mip whose longer side is above 32, the rest together), each with its
// 9-byte head.
bool TexturePieces(const std::vector<uint8_t>& dds, const TexEntry& like, std::vector<uint8_t>& data,
                   std::vector<uint32_t>& offsets, uint32_t& size, std::string& err) {
    DdsInfo di;
    if (!ReadDds(dds, di, err)) return false;
    if (like.slices != 1) return err = "a texture array or cube map", false;
    if (di.format != like.format || di.width != like.width || di.height != like.height || di.mips != like.mips) {
        char b[200];
        snprintf(b, sizeof b, "the DDS is format %u %ux%u with %u mips, the game's texture format %u %ux%u with %u mips",
                 di.format, di.width, di.height, di.mips, like.format, like.width, like.height, like.mips);
        return err = b, false;
    }
    std::vector<uint32_t> sizes;
    uint32_t tail = 0;
    for (uint32_t m = 0; m < di.mips; ++m) {
        uint32_t w = di.width >> m ? di.width >> m : 1, h = di.height >> m ? di.height >> m : 1;
        uint32_t s = MipBytes(di.format, w, h);
        if (w > 32 || h > 32) sizes.push_back(s);
        else tail += s;
    }
    if (tail) sizes.push_back(tail);
    size_t at = di.start;
    size = 0;
    for (size_t k = 0; k < sizes.size(); ++k) {
        if (at + sizes[k] > dds.size()) return err = "the DDS is shorter than its mips", false;
        if (k) offsets.push_back(static_cast<uint32_t>(data.size()));
        std::vector<uint8_t> z;
        ZStore(z, dds.data() + at, sizes[k]);
        uint32_t zs = static_cast<uint32_t>(z.size());
        uint8_t left = static_cast<uint8_t>(sizes.size() - 1 - k);
        data.insert(data.end(), reinterpret_cast<uint8_t*>(&zs), reinterpret_cast<uint8_t*>(&zs) + 4);
        data.insert(data.end(), reinterpret_cast<uint8_t*>(&sizes[k]), reinterpret_cast<uint8_t*>(&sizes[k]) + 4);
        data.push_back(left);
        data.insert(data.end(), z.begin(), z.end());
        at += sizes[k];
        size += sizes[k];
    }
    return true;
}

// texture.cache for the DDS files; false when none of them could go in
bool BuildTextureCache(const std::wstring& root, const std::vector<LooseFile>& dds, std::vector<uint8_t>& out) {
    std::vector<uint8_t> table, names, entries;
    uint32_t tableCount = 0, count = 0;
    std::map<std::string, TexEntry> game;
    std::map<std::string, bool> found;
    for (const LooseFile& f : dds) game[f.path.substr(0, f.path.size() - 4) + ".xbm"] = TexEntry{};
    FindGameTextures(root, game, found);
    for (const LooseFile& f : dds) {
        std::string name = f.path.substr(0, f.path.size() - 4) + ".xbm";
        TexEntry e = game[name];
        std::vector<uint8_t> raw;
        std::string err;
        std::vector<uint8_t> data;
        std::vector<uint32_t> offsets;
        uint32_t size = 0;
        bool own = false;
        if (!ReadAll(f.full, raw)) {
            err = "cannot read the file";
        } else if (!found[name]) {
            // a texture of its own: no game texture under this path, so the entry comes from the DDS itself, under
            // the key OwnTextureKey(path); the .xbm beside it (a loose file, tools\new_texture.py) holds that key
            DdsInfo di;
            if (ReadDds(raw, di, err)) {
                e = TexEntry{};
                e.key = OwnTextureKey(name);
                e.align = 16;
                e.width = static_cast<uint16_t>(di.width);
                e.height = static_cast<uint16_t>(di.height);
                e.mips = static_cast<uint16_t>(di.mips);
                e.slices = 1;
                e.format = di.format;
                e.format2 = 4;
                own = true;
                TexturePieces(raw, e, data, offsets, size, err);
            }
        } else {
            TexturePieces(raw, e, data, offsets, size, err);
        }
        if (!err.empty()) {
            Log("loose files: %s left out - %s", f.path.c_str(), err.c_str());
            continue;
        }
        out.resize((out.size() + 4095) & ~size_t(4095), 0);
        e.nameOffset = static_cast<int32_t>(names.size());
        e.page = static_cast<uint32_t>(out.size() / 4096);
        e.stored = static_cast<uint32_t>(data.size());
        e.size = size;
        e.firstPiece = static_cast<int32_t>(tableCount);
        e.pieces = static_cast<int32_t>(offsets.size());
        e.time = 0;
        out.insert(out.end(), data.begin(), data.end());
        for (uint32_t o : offsets) table.insert(table.end(), reinterpret_cast<uint8_t*>(&o), reinterpret_cast<uint8_t*>(&o) + 4);
        tableCount += static_cast<uint32_t>(offsets.size());
        names.insert(names.end(), name.begin(), name.end());
        names.push_back(0);
        entries.insert(entries.end(), reinterpret_cast<uint8_t*>(&e), reinterpret_cast<uint8_t*>(&e) + 52);
        ++count;
        if (own)
            Log("loose files: %s is a texture of its own, %s (%ux%u, key %08x)", f.path.c_str(), name.c_str(), e.width,
                e.height, e.key);
        else
            Log("loose files: %s replaces the pixels of %s (%ux%u)", f.path.c_str(), name.c_str(), e.width, e.height);
    }
    if (!count) return false;
    out.resize((out.size() + 4095) & ~size_t(4095), 0);
    uint32_t pages = static_cast<uint32_t>(out.size() / 4096);
    size_t tailStart = out.size();
    out.insert(out.end(), table.begin(), table.end());
    out.insert(out.end(), names.begin(), names.end());
    out.insert(out.end(), entries.begin(), entries.end());
    uint64_t crc = 0xcbf29ce484222325ull;
    for (size_t i = tailStart; i < out.size(); ++i) {
        crc ^= out[i];
        crc *= 0x100000001b3ull;
    }
    TexFooter foot{crc, pages, count, static_cast<uint32_t>(names.size()), tableCount, 0x54584348u, 6};
    out.insert(out.end(), reinterpret_cast<uint8_t*>(&foot), reinterpret_cast<uint8_t*>(&foot) + 32);
    return true;
}

// --- sounds: a Wwise bank (.bnk) or streamed sound (.wem) anywhere under tw3se\files replaces the game's file of
// that name (the game's sound files have no folders). They go into a soundspc.cache of the mod (layout in
// tools\soundcache.py: header, files back to back from 0x40, names, entries; checksum FNV-1a 64 over names and
// entries; rebuilt from the game's own cache there). Two header fields of unknown use stay 0.
bool IsSound(const std::string& p) {
    return p.size() > 4 && (p.compare(p.size() - 4, 4, ".wem") == 0 || p.compare(p.size() - 4, 4, ".bnk") == 0);
}
bool BuildSoundCache(const std::vector<LooseFile>& sounds, std::vector<uint8_t>& out) {
    std::vector<uint8_t> names, entries;
    std::map<std::string, bool> seen;
    out.assign(0x40, 0);
    uint64_t largest = 0;
    uint32_t count = 0;
    for (const LooseFile& f : sounds) {
        std::string name = f.path.substr(f.path.find_last_of('\\') + 1);
        if (seen[name]) {
            Log("loose files: %s left out - another sound has the name %s", f.path.c_str(), name.c_str());
            continue;
        }
        std::vector<uint8_t> raw;
        if (!ReadAll(f.full, raw)) {
            Log("loose files: %s left out - cannot read the file", f.path.c_str());
            continue;
        }
        seen[name] = true;
        uint32_t nameOff = static_cast<uint32_t>(names.size()), zero = 0;
        uint64_t off = out.size(), size = raw.size();
        Put(entries, nameOff);
        Put(entries, zero);
        Put(entries, off);
        Put(entries, size);
        names.insert(names.end(), name.begin(), name.end());
        names.push_back(0);
        out.insert(out.end(), raw.begin(), raw.end());
        largest = (std::max)(largest, size);
        ++count;
        Log("loose files: %s replaces the game's sound %s", f.path.c_str(), name.c_str());
    }
    if (!count) return false;
    uint64_t namesOff = out.size(), info = namesOff + names.size();
    uint64_t crc = 0xcbf29ce484222325ull;
    for (const std::vector<uint8_t>* part : {&names, &entries})
        for (uint8_t b : *part) {
            crc ^= b;
            crc *= 0x100000001b3ull;
        }
    out.insert(out.end(), names.begin(), names.end());
    out.insert(out.end(), entries.begin(), entries.end());
    std::vector<uint8_t> head;
    head.insert(head.end(), {'C', 'S', '3', 'W'});
    Put<uint32_t>(head, 2);
    Put<uint32_t>(head, 0);
    Put<uint32_t>(head, 0);
    Put<uint64_t>(head, info);
    Put<uint32_t>(head, count);
    Put<uint32_t>(head, 0);
    Put<uint64_t>(head, namesOff);
    Put<uint32_t>(head, static_cast<uint32_t>(names.size()));
    Put<uint32_t>(head, 0);
    Put<uint64_t>(head, (largest + 4095) & ~4095ull);
    Put<uint64_t>(head, crc);
    memcpy(out.data(), head.data(), 0x40);
    return true;
}

void RemoveMod(const std::wstring& mod) {
    for (const wchar_t* f : {L"\\content\\blob0.bundle", L"\\content\\metadata.store", L"\\content\\tw3se_files.txt",
                             L"\\content\\texture.cache", L"\\content\\soundspc.cache"})
        DeleteFileW((mod + f).c_str());
    RemoveDirectoryW((mod + L"\\content").c_str());
    RemoveDirectoryW(mod.c_str());
}

}  // namespace

void BuildLooseFiles() {
    std::wstring src = Dir() + L"\\files\\";
    std::wstring root = GameRoot();
    if (root.empty()) return;
    std::wstring mod = root + L"\\Mods\\" + kModName;
    std::vector<LooseFile> files;
    std::string bad;
    // tw3se\files first, then the files\ folder of each mod (modfolders.cpp): a path the one before has stays its
    std::vector<std::pair<std::string, std::wstring>> roots = {{"tw3se\\files", src}};
    if (!SafeMode())  // safe mode (main.cpp): the mods' own loose files stay out, tw3se\files stays in
        for (const ModFolder& m : ModFolders()) roots.push_back({m.name, m.dir + L"\\files\\"});
    std::map<std::string, std::string> from;  // depot path -> where it came from
    std::string doubles;
    for (const auto& [name, dir] : roots) {
        std::vector<LooseFile> found;
        Walk(dir, L"", found, bad);
        for (LooseFile& f : found) {
            auto it = from.find(f.path);
            if (it != from.end()) {
                doubles += "\n  " + f.path + ": " + it->second + " (not " + name + ")";
                continue;
            }
            from[f.path] = name;
            files.push_back(std::move(f));
        }
    }
    if (!bad.empty()) Log("loose files: left out (not ASCII or 256 characters or more):%s", bad.c_str());
    if (!doubles.empty()) Log("loose files: the same file from two places, the first counts:%s", doubles.c_str());
    if (files.empty()) {
        if (GetFileAttributesW(mod.c_str()) != INVALID_FILE_ATTRIBUTES) {
            RemoveMod(mod);
            Log("loose files: tw3se\\files and the mods' files folders are empty - %ls taken away", kModName);
        }
        return;
    }
    std::sort(files.begin(), files.end(), [](const LooseFile& a, const LooseFile& b) { return a.path < b.path; });
    std::string list;
    for (const LooseFile& f : files) {
        char b[64];
        snprintf(b, sizeof b, "\t%llu\t%llu\t", static_cast<unsigned long long>(f.size), static_cast<unsigned long long>(f.time));
        list += f.path + b + from[f.path] + "\n";
    }
    std::vector<uint8_t> old;
    if (ReadAll(mod + L"\\content\\tw3se_files.txt", old) && std::string(old.begin(), old.end()) == list) {
        Log("loose files: %zu file(s), unchanged since the last start", files.size());
        return;
    }
    // DDS files go into the mod's texture.cache, sounds into its soundspc.cache, the rest into its bundle
    std::vector<LooseFile> textures, sounds;
    for (size_t i = files.size(); i-- > 0;) {
        const std::string& p = files[i].path;
        bool dds = p.size() > 4 && p.compare(p.size() - 4, 4, ".dds") == 0;
        if (!dds && !IsSound(p)) continue;
        (dds ? textures : sounds).insert((dds ? textures : sounds).begin(), files[i]);
        files.erase(files.begin() + static_cast<std::ptrdiff_t>(i));
    }
    CreateDirectoryW((root + L"\\Mods").c_str(), nullptr);
    CreateDirectoryW(mod.c_str(), nullptr);
    CreateDirectoryW((mod + L"\\content").c_str(), nullptr);
    std::vector<uint8_t> cache;
    if (!textures.empty() && BuildTextureCache(root, textures, cache)) {
        if (!WriteAll(mod + L"\\content\\texture.cache", cache)) Log("loose files: texture.cache - WRITING FAILED");
    } else {
        DeleteFileW((mod + L"\\content\\texture.cache").c_str());
    }
    std::vector<uint8_t> soundCache;
    if (!sounds.empty() && BuildSoundCache(sounds, soundCache)) {
        if (!WriteAll(mod + L"\\content\\soundspc.cache", soundCache)) Log("loose files: soundspc.cache - WRITING FAILED");
    } else {
        DeleteFileW((mod + L"\\content\\soundspc.cache").c_str());
    }
    if (files.empty()) {  // textures and sounds only
        DeleteFileW((mod + L"\\content\\blob0.bundle").c_str());
        DeleteFileW((mod + L"\\content\\metadata.store").c_str());
        WriteAll(mod + L"\\content\\tw3se_files.txt", std::vector<uint8_t>(list.begin(), list.end()));
        return;
    }
    CrcInit();
    uint32_t toc = static_cast<uint32_t>(0x130 * files.size());
    uint32_t start = 0x20 + toc;
    uint64_t pos = start;
    for (size_t i = files.size(); i-- > 0;) {  // the data in the reverse order of the table
        pos = (pos + 15) & ~15ull;
        files[i].offset = pos;
        pos += files[i].size;
    }
    if (pos >= 0xFFFFFFF0ull) {
        Log("loose files: %llu bytes, more than one bundle holds - nothing packed", static_cast<unsigned long long>(pos));
        return;
    }
    std::vector<uint8_t> bundle(static_cast<size_t>(pos), 0);
    for (LooseFile& f : files) {
        std::vector<uint8_t> raw;
        if (!ReadAll(f.full, raw) || raw.size() != f.size) {
            Log("loose files: %s could not be read - nothing packed", f.path.c_str());
            return;
        }
        if (!raw.empty()) memcpy(bundle.data() + f.offset, raw.data(), raw.size());
        f.crc = Crc32(raw.data(), raw.size());
    }
    uint32_t size = static_cast<uint32_t>(pos);
    memcpy(bundle.data(), "POTATO70", 8);
    uint32_t rounded = (size + 15) & ~15u, zero = 0;
    uint16_t five = 5, st = static_cast<uint16_t>(start & 0xFFFF);  // the low half only (12837 files: 0x8c10)
    memcpy(bundle.data() + 8, &rounded, 4);
    memcpy(bundle.data() + 12, &zero, 4);
    memcpy(bundle.data() + 16, &toc, 4);
    memcpy(bundle.data() + 20, &five, 2);
    memcpy(bundle.data() + 22, &st, 2);
    for (size_t i = 0; i < files.size(); ++i) {
        uint8_t* e = bundle.data() + 0x20 + i * 0x130;
        memcpy(e, files[i].path.data(), files[i].path.size());
        uint64_t off = files[i].offset;
        uint32_t n = static_cast<uint32_t>(files[i].size), comp = 0;
        memcpy(e + 0x110, &off, 8);
        memcpy(e + 0x118, &n, 4);
        memcpy(e + 0x11c, &n, 4);
        memcpy(e + 0x120, &files[i].crc, 4);
        memcpy(e + 0x124, &comp, 4);
    }
    std::vector<uint8_t> store = MetadataStore(files, start, rounded);  // (metastore.py takes the header's size)
    bool ok = WriteAll(mod + L"\\content\\blob0.bundle", bundle) && WriteAll(mod + L"\\content\\metadata.store", store) &&
              WriteAll(mod + L"\\content\\tw3se_files.txt", std::vector<uint8_t>(list.begin(), list.end()));
    Log("loose files: %zu file(s) packed into Mods\\%ls (%u bytes)%s", files.size(), kModName, size,
        ok ? "" : " - WRITING FAILED");
}

}  // namespace tw3se
