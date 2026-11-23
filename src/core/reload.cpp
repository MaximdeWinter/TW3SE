// Game files changed while the game runs.
//
// Every file of the game is a CDiskFile in the depot; GDepot's vtable slot 10 finds one by its path (const char*,
// what DepotCacheLookup calls). A file from a bundle is a CBundleDiskFile: its vtable slot 3 makes the reader the
// loader reads the file through (the bundle manager's reader for the file id at +0x40). TW3SE puts its own function
// in that slot: for a file with new contents it hands out a CMemoryFileReader over those bytes (constructor
// 0x26f6400: data +0x18, size +0x20, offset +0x28; 0x30 bytes from the engine's allocator, which its destructor frees
// into), every other file gets the bundle's reader as before. The bytes stay in memory for the session: a reader made
// earlier may still read them.
#include "core.h"

#include <windows.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tw3se {
namespace {

using CreateReader_t = void* (*)(void* diskFile);
using FindFile_t = void* (*)(void* depot, const char* path);
using MemReaderCtor_t = void* (*)(void* self, const void* data, size_t size);

CreateReader_t oCreateReader = nullptr;
using CreateAsync_t = void* (*)(void* diskFile, uint8_t priority, void** out);
CreateAsync_t oCreateAsync = nullptr;  // the bundle's async reader (CBundleDiskFile slot 5)
std::mutex g_mx;
std::unordered_map<void*, std::shared_ptr<std::vector<uint8_t>>> g_new;  // CDiskFile -> its new contents
std::vector<std::shared_ptr<std::vector<uint8_t>>> g_keep;              // every version handed out
uint64_t g_readers = 0;

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD r = 0;
    bool ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &r, nullptr) && r == out.size());
    CloseHandle(h);
    return ok;
}

// The MSVC type name of a polymorphic object (vtable[-1] -> complete object locator -> type descriptor), guarded.
std::string TypeName(void* obj) {
    char name[64] = {};
    uintptr_t vt = 0, col = 0;
    uint32_t tdRva = 0;
    if (!SafeRead(obj, &vt, 8) || !SafeRead(reinterpret_cast<void*>(vt - 8), &col, 8) ||
        !SafeRead(reinterpret_cast<void*>(col + 12), &tdRva, 4) ||
        !SafeRead(reinterpret_cast<void*>(ImageBase() + tdRva + 0x10), name, sizeof name - 1))
        return "";
    return name;
}

void* CreateReaderHook(void* df) {
    std::shared_ptr<std::vector<uint8_t>> bytes;
    {
        std::lock_guard<std::mutex> l(g_mx);
        auto it = g_new.find(df);
        if (it != g_new.end()) bytes = it->second;
    }
    if (!bytes) return oCreateReader(df);
    auto ctor = reinterpret_cast<MemReaderCtor_t>(Address("CMemoryFileReader::CMemoryFileReader"));
    void* r = ctor ? EngineAlloc(0x30, 16) : nullptr;
    if (!r) return oCreateReader(df);
    ctor(r, bytes->data(), bytes->size());
    ++g_readers;
    return r;
}

// The loader reads through the async file (slot 5). A file with new contents gets the base class's one instead (a
// CAsyncFile_Fallback), which asks slot 3 for its reader: the memory reader above.
void* CreateAsyncHook(void* df, uint8_t priority, void** out) {
    bool mine;
    {
        std::lock_guard<std::mutex> l(g_mx);
        mine = g_new.count(df) != 0;
    }
    auto base = reinterpret_cast<CreateAsync_t>(Address("CDiskFile::CreateAsyncFile"));
    if (mine && base) return base(df, priority, out);
    return oCreateAsync(df, priority, out);
}

bool HookReader(void* df, std::string& err) {
    if (oCreateReader) return true;
    if (TypeName(df) != ".?AVCBundleDiskFile@@") {
        err = "the file is not from a bundle (" + TypeName(df) + ")";
        return false;
    }
    if (!Address("CDiskFile::CreateAsyncFile") || !Address("CMemoryFileReader::CMemoryFileReader")) {
        err = "the file reader functions were not found in this build";
        return false;
    }
    void** vt = *static_cast<void***>(df);
    DWORD old = 0;
    if (!VirtualProtect(&vt[3], 0x18, PAGE_READWRITE, &old)) {
        err = "the vtable of CBundleDiskFile is not writable";
        return false;
    }
    oCreateReader = reinterpret_cast<CreateReader_t>(vt[3]);
    oCreateAsync = reinterpret_cast<CreateAsync_t>(vt[5]);
    vt[3] = reinterpret_cast<void*>(&CreateReaderHook);
    vt[5] = reinterpret_cast<void*>(&CreateAsyncHook);
    VirtualProtect(&vt[3], 0x18, old, &old);
    Log("reload: the readers of bundled files hooked");
    return true;
}

}  // namespace

void* FindDiskFile(const char* path) {
    auto pDepot = reinterpret_cast<void**>(Address("GDepot"));
    void* depot = pDepot ? *pDepot : nullptr;
    if (!depot) return nullptr;
    void** vt = nullptr;
    if (!SafeRead(depot, &vt, 8) || !vt) return nullptr;
    void* fn = nullptr;
    if (!SafeRead(&vt[10], &fn, 8) || !fn) return nullptr;
    return reinterpret_cast<FindFile_t>(fn)(depot, path);
}

// New contents for a game file from now on (its next load reads them). Returns "" or the reason. drop: the loaded
// resource is forgotten at once, so the next use loads anew - only for files nothing in the world uses (a template
// for the next spawn). A layer of the world keeps its resource until the engine frees it (a save load): taken away
// under it, the unload read a null resource (witcher3+0x26ecc00, the main thread).
std::string SetFileContents(const char* path, std::vector<uint8_t> bytes, bool drop) {
    std::string err;
    void* df = FindDiskFile(path);
    if (!df) return std::string("no game file ") + path;
    if (!HookReader(df, err)) return err;
    auto b = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    {
        std::lock_guard<std::mutex> l(g_mx);
        g_new[df] = b;
        g_keep.push_back(b);
    }
    // +0x10 is the loaded resource (null: not loaded; measured on a CEntityTemplate whose handle block is at its
    // +0x08). Set to null, the file loads anew at its next use; the old resource lives on for those that hold it.
    if (!drop) return "";
    void* none = nullptr;
    void* loaded = nullptr;
    if (SafeRead(static_cast<char*>(df) + 0x10, &loaded, 8) && loaded) {
        DWORD old = 0;
        if (VirtualProtect(static_cast<char*>(df) + 0x10, 8, PAGE_READWRITE, &old))
            *reinterpret_cast<void**>(static_cast<char*>(df) + 0x10) = none;
    }
    return "";
}

// The game's own contents again (a loose file taken away): the file forgets the new contents and its resource.
std::string ClearFileContents(const char* path, bool drop) {
    void* df = FindDiskFile(path);
    if (!df) return std::string("no game file ") + path;
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (!g_new.erase(df)) return "";  // (never had new contents)
    }
    if (!drop) return "";  // (SetFileContents: what the world uses stays until the engine frees it)
    void* none = nullptr;
    void* loaded = nullptr;
    if (SafeRead(static_cast<char*>(df) + 0x10, &loaded, 8) && loaded)
        *reinterpret_cast<void**>(static_cast<char*>(df) + 0x10) = none;
    return "";
}

std::string ReloadLooseFile(const char* path) {
    std::vector<uint8_t> bytes;
    std::string p = path;
    for (char& c : p) c = c == '/' ? '\\' : c;
    std::wstring full = Dir() + L"\\files\\" + std::wstring(p.begin(), p.end());
    if (!ReadAll(full, bytes)) return "no file tw3se\\files\\" + p;
    if (!FindDiskFile(p.c_str())) {
        // a file the game does not have: a new one in the depot, read from memory (its bundle bytes never are)
        std::string err = NewDiskFile(p.c_str());
        if (err.rfind("no folder", 0) == 0) return "new, in a folder the game does not have - in the game from the next start";
        if (!err.empty()) return err;
    }
    return SetFileContents(p.c_str(), std::move(bytes), true);
}

uint64_t ReloadReaders() { return g_readers; }

// A second path for a game file (the mesh copy per figure, docs/FINDINGS_G6.md): a new CBundleDiskFile with the
// source's file id in the folder of the new path. It reads the source's bytes from its bundle and finds the source's
// .buffer files (CBundleDiskFile slot 7 maps a buffer index through the file id), but it keeps its own loaded resource
// (+0x10): loading the new path gives a resource of its own. Built like CBundleDiskFile's own clone (slot 11:
// CDiskFile::CDiskFile, then vtable and file id at +0x40, 0x48 bytes) and entered like CDirectory::CreateNewFile
// (slot 7) does: CDirectory slot 5 puts it into the folder's file list (+0x08) and gives it its global index (+0x24).
// The folder's file list is found by a hash of the name alone (h = h * 31 + c), so a name with the hash of a file
// already there is refused.
namespace {
std::unordered_map<void*, void*> g_alias;  // alias CDiskFile -> source CDiskFile
using DirWalk_t = void* (*)(void* dir, const char* path, const char** fileName);
using DirFind_t = void* (*)(void* dir, const char* name, int32_t len);
using DirAdd_t = void (*)(void* dir, void* file, uint64_t sortedInsert);
using DiskFileCtor_t = void* (*)(void* self, void* dir, const void* name);
}  // namespace

std::string FileAlias(const char* src, const char* dst) {
    std::string d = dst;
    for (char& c : d) c = c == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
    void* sf = FindDiskFile(src);
    if (!sf) return std::string("no game file ") + src;
    if (TypeName(sf) != ".?AVCBundleDiskFile@@") return "the file is not from a bundle (" + TypeName(sf) + ")";
    if (void* have = FindDiskFile(d.c_str())) {
        std::lock_guard<std::mutex> l(g_mx);
        auto it = g_alias.find(have);
        if (it != g_alias.end() && it->second == sf) return "";  // made before
        return "the path is taken: " + d;
    }
    auto ctor = reinterpret_cast<DiskFileCtor_t>(Address("CDiskFile::CDiskFile"));
    auto pDepot = reinterpret_cast<void**>(Address("GDepot"));
    void* depot = pDepot ? *pDepot : nullptr;
    void* root = nullptr;
    if (!ctor || !depot || !SafeRead(static_cast<char*>(depot) + 8, &root, 8) || !root)
        return "the depot functions were not found in this build";
    void** rvt = *static_cast<void***>(root);
    const char* name = nullptr;
    void* dir = reinterpret_cast<DirWalk_t>(rvt[11])(root, d.c_str(), &name);  // the last folder, the name after it
    if (!dir || !name || !*name) return "no folder for " + d + " (the folder has to be in the depot already)";
    void** dvt = *static_cast<void***>(dir);
    if (reinterpret_cast<DirFind_t>(dvt[17])(dir, name, -1))
        return std::string("the name has the hash of another file in the folder, choose another name than ") + name;
    char* f = static_cast<char*>(EngineAlloc(0x48, 16));
    if (!f) return "out of memory";
    memset(f, 0, 0x48);
    struct {
        const char* data;
        uint32_t size;  // with the terminating 0
    } as{name, static_cast<uint32_t>(strlen(name) + 1)};
    ctor(f, dir, &as);
    memcpy(f, sf, 8);                                     // the vtable of CBundleDiskFile
    memcpy(f + 0x40, static_cast<char*>(sf) + 0x40, 4);  // the file id
    reinterpret_cast<DirAdd_t>(dvt[5])(dir, f, 0);
    if (FindDiskFile(d.c_str()) != f) return "the folder did not take the file " + d;
    {
        std::lock_guard<std::mutex> l(g_mx);
        g_alias[f] = sf;
    }
    Log("reload: %s is an alias of %s", d.c_str(), src);
    return "";
}

// A file the game does not have, added while it runs: an alias of a small bundled file, its contents from memory.
std::string NewDiskFile(const char* path) {
    static const char* const kAny[] = {"gameplay\\items\\def_item_junk.xml", "gameplay\\items\\def_item_misc.xml"};
    std::string err;
    for (const char* src : kAny) {
        if (!FindDiskFile(src)) continue;
        err = FileAlias(src, path);
        if (err.empty()) Log("reload: %s is new in the depot", path);
        return err;
    }
    return "no bundled file to start a new one from";
}

namespace {

// The watch: tw3se\files\ once a second; a file written after the start (or after its last reload) loads anew.
std::unordered_map<std::string, uint64_t> g_seen;  // depot path -> last write time
bool g_watchPrimed = false;
float g_watchClock = 0;

void Scan(const std::wstring& root, const std::wstring& rel, std::vector<std::pair<std::string, uint64_t>>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + rel + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            Scan(root, rel + name + L"\\", out);
            continue;
        }
        std::wstring w = rel + name;
        std::string p;
        for (wchar_t c : w) p += c < 128 ? static_cast<char>(tolower(c)) : '?';
        out.push_back({p, (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int ReloadChanged(std::string* report) {
    std::vector<std::pair<std::string, uint64_t>> now;
    Scan(Dir() + L"\\files\\", L"", now);
    int n = 0;
    for (auto& f : now) {
        auto it = g_seen.find(f.first);
        bool changed = g_watchPrimed && (it == g_seen.end() || it->second != f.second);
        g_seen[f.first] = f.second;
        if (!changed || f.first.find('?') != std::string::npos) continue;
        std::string ext = f.first.size() > 4 ? f.first.substr(f.first.size() - 4) : "";
        if (ext == ".dds" || ext == ".wem" || ext == ".bnk") {  // texture.cache and soundspc.cache
            Log("reload: %s goes into a cache - it changes at the next start of the game", f.first.c_str());
            if (report) *report += f.first + " (at the next start)\n";
            continue;
        }
        std::string err = ReloadLooseFile(f.first.c_str());
        Log("reload: %s %s", f.first.c_str(), err.empty() ? "loads anew at its next use" : err.c_str());
        if (report) *report += f.first + (err.empty() ? "" : " (" + err + ")") + "\n";
        if (err.empty()) ++n;
    }
    // files taken away: the game's own contents again
    std::unordered_map<std::string, bool> present;
    for (auto& f : now) present[f.first] = true;
    for (auto it = g_seen.begin(); it != g_seen.end();) {
        if (present.count(it->first)) {
            ++it;
            continue;
        }
        std::string err = ClearFileContents(it->first.c_str(), true);
        Log("reload: %s taken away - %s", it->first.c_str(), err.empty() ? "the game's own contents again" : err.c_str());
        if (report) *report += it->first + " (taken away)\n";
        it = g_seen.erase(it);
    }
    g_watchPrimed = true;
    return n;
}

void WatchTick(float dt, void*) {
    g_watchClock += dt;
    if (g_watchClock < 1.0f) return;
    g_watchClock = 0;
    ReloadChanged(nullptr);
}

void CmdFiles(const char*, char* reply, uint32_t size) {
    std::string r;
    int n = ReloadChanged(&r);
    r = "files: " + std::to_string(n) + " file(s) load anew; " + std::to_string(g_readers) + " read from memory so far\n" + r;
    strncpy_s(reply, size, r.c_str(), _TRUNCATE);
}

}  // namespace

void InstallReload() {
    RegisterCommand("files", &CmdFiles, "files: the files changed in tw3se\\files load anew (the watch does it each second)");
    if (!ConfigBool("files_watch", true)) return;
    ReloadChanged(nullptr);  // what is there at the start is in the packed mod already
    RegisterTick(&WatchTick, nullptr);
}

}  // namespace tw3se
