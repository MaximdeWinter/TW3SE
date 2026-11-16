// TW3SE core - `enginedump`: one large snapshot of the running engine for the analysis side (one
// run answers many questions at once). The same command runs in the game and in the REDkit editor, so the two
// dumps can be compared field by field. Everything goes to tw3se\out\dump\ as text:
//
//   globals.txt  every 8-byte value in the exe's writable sections that points to an object with MSVC RTTI from
//                the exe (the variable's RVA, the object's C++ type), and every object that lives in those sections
//                itself (its first qword is a vtable of the exe)
//   vtables.txt  for every type met: its vtable RVA and all slots as RVAs (until a slot leaves the exe's code)
//   objects.txt  raw dumps of the player, its components, theGame, the RTTI system and the objects they point to
//                whose type name matches kFollow; every qword annotated (typed pointer, CName, float, int)
//   threads.txt  every thread of the process with its start address (RVA in the exe, else module+offset)
//
// Reads only. Every read of a foreign pointer is guarded; nothing in the engine is called except the RTTI getters
// TW3SE uses everywhere (class of a reflected object).
#include "core.h"
#include <tlhelp32.h>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace tw3se {
namespace {

uintptr_t g_imgBase = 0, g_imgEnd = 0, g_textBeg = 0, g_textEnd = 0;

void ImageRanges() {
    g_imgBase = ImageBase();
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_imgBase +
                                                          reinterpret_cast<const IMAGE_DOS_HEADER*>(g_imgBase)->e_lfanew);
    g_imgEnd = g_imgBase + nt->OptionalHeader.SizeOfImage;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
        if (!memcmp(sec->Name, ".text", 6)) {
            g_textBeg = g_imgBase + sec->VirtualAddress;
            g_textEnd = g_textBeg + sec->Misc.VirtualSize;
        }
}

bool InImage(uintptr_t v) { return v >= g_imgBase && v < g_imgEnd; }
bool InText(uintptr_t v) { return v >= g_textBeg && v < g_textEnd; }

// The readable memory of the process, read once per dump (VirtualQuery over the whole address space: a few thousand
// regions in milliseconds). One syscall per checked pointer made the first editor dump take 6.5 minutes. A region
// freed while the dump runs is caught by the guard (editorprobe.cpp keeps it in front of the editor's handler).
std::vector<std::pair<uintptr_t, uintptr_t>> g_readable;  // [begin, end), sorted, merged

void SnapshotMemory() {
    g_readable.clear();
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    uintptr_t a = 0x10000;
    MEMORY_BASIC_INFORMATION mbi{};
    while (a < 0x7ffffffeffffULL && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) {
        uintptr_t beg = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = beg + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & readable) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            if (!g_readable.empty() && g_readable.back().second == beg) g_readable.back().second = end;
            else g_readable.push_back({beg, end});
        }
        if (end <= a) break;
        a = end;
    }
}

bool Readable(uintptr_t at, size_t n) {
    size_t lo = 0, hi = g_readable.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (at < g_readable[mid].first) hi = mid;
        else if (at >= g_readable[mid].second) lo = mid + 1;
        else return at + n <= g_readable[mid].second;
    }
    return false;
}

bool ReadQ(uintptr_t at, uintptr_t* out) {
    if (!Readable(at, 8)) return false;  // the editor's crash handler comes first: never fault on purpose
    __try {
        *out = *reinterpret_cast<const uintptr_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The MSVC type name of the vtable `vt` (a vtable of the exe), or null.
const char* VtableTypeName(uintptr_t vt) {
    if (!InImage(vt)) return nullptr;
    uintptr_t colAt = 0;
    if (!ReadQ(vt - 8, &colAt) || !InImage(colAt) || !Readable(colAt, 24)) return nullptr;
    __try {
        const uint32_t* col = reinterpret_cast<const uint32_t*>(colAt);
        if (!Readable(g_imgBase + col[3] + 0x10, 4)) return nullptr;
        if (col[0] != 1 || reinterpret_cast<uintptr_t>(col) - col[5] != g_imgBase) return nullptr;  // x64 COL
        const char* name = reinterpret_cast<const char*>(g_imgBase + col[3] + 0x10);
        return (name[0] == '.' && name[1] == '?') ? name : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The type of the object at `p` (its first qword is a vtable of the exe), or null.
const char* ObjectTypeName(uintptr_t p, uintptr_t* vtOut = nullptr) {
    if (p < 0x10000 || p > 0x7fffffffffffULL || (p & 7)) return nullptr;
    uintptr_t vt = 0;
    if (!ReadQ(p, &vt) || !InImage(vt)) return nullptr;
    if (vtOut) *vtOut = vt;
    return VtableTypeName(vt);
}

std::string Rva(uintptr_t v) {
    char b[32];
    _snprintf_s(b, _TRUNCATE, "%llx", static_cast<unsigned long long>(v - g_imgBase));
    return b;
}

struct Dumper {
    std::wstring dir;
    std::map<std::string, uintptr_t> vtables;  // type -> vtable
    std::set<uintptr_t> dumped;
    FILE* objects = nullptr;
    int objectCount = 0;
    std::vector<std::pair<std::string, uintptr_t>> globalsToFollow;  // globals whose type matches kFollow

    FILE* Open(const wchar_t* name) {
        FILE* f = nullptr;
        _wfopen_s(&f, (dir + L"\\" + name).c_str(), L"w");
        return f;
    }

    void NoteType(uintptr_t obj) {
        uintptr_t vt = 0;
        if (const char* t = ObjectTypeName(obj, &vt)) vtables.emplace(t, vt);
    }

    int Globals() {
        FILE* f = Open(L"globals.txt");
        if (!f) return 0;
        fprintf(f, "# exe build %08x; RVA of the variable, kind, C++ type\n", GameBuild());
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            g_imgBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(g_imgBase)->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        int found = 0;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            uintptr_t beg = g_imgBase + sec->VirtualAddress, end = beg + (sec->Misc.VirtualSize & ~7u);
            for (uintptr_t at = beg; at < end; at += 8) {
                uintptr_t v = 0;
                if (!ReadQ(at, &v) || !v) continue;
                if (InImage(v)) {
                    if (const char* t = VtableTypeName(v)) {  // an object living in the section itself
                        fprintf(f, "%s static %s\n", Rva(at).c_str(), t);
                        vtables.emplace(t, v);
                        ++found;
                    }
                    continue;
                }
                uintptr_t vt = 0;
                if (const char* t = ObjectTypeName(v, &vt)) {
                    fprintf(f, "%s ptr %s %p\n", Rva(at).c_str(), t, reinterpret_cast<void*>(v));
                    vtables.emplace(t, vt);
                    ++found;
                    for (const char* k : kFollow)
                        if (strstr(t, k) && strncmp(t, ".?AV?$TTypedClass", 17) != 0) {
                            globalsToFollow.push_back({"global " + Rva(at), v});
                            break;
                        }
                }
            }
        }
        fclose(f);
        return found;
    }

    int Vtables() {
        FILE* f = Open(L"vtables.txt");
        if (!f) return 0;
        fprintf(f, "# type, vtable RVA, then the slots: index rva (until a slot leaves the exe's code, at most 600)\n");
        for (auto& [type, vt] : vtables) {
            fprintf(f, "%s vtable %s\n", type.c_str(), Rva(vt).c_str());
            for (int i = 0; i < 600; ++i) {
                uintptr_t s = 0;
                if (!ReadQ(vt + i * 8, &s) || !InText(s)) break;
                fprintf(f, "  %d %s\n", i, Rva(s).c_str());
            }
        }
        fclose(f);
        return static_cast<int>(vtables.size());
    }

    // Annotates one qword: a typed pointer, a pointer into the exe, a CName, a float, an int.
    std::string Explain(uintptr_t v) {
        char b[160];
        if (!v) return "0";
        // type names run past 200 characters (TTypedClass<...>): strings, never a fixed buffer (sprintf_s into
        // 160 bytes ends the editor with an invalid-parameter fast fail)
        if (const char* t = ObjectTypeName(v)) return std::string("-> ") + t;
        if (InImage(v)) {
            if (const char* t = VtableTypeName(v)) {
                vtables.emplace(t, v);  // an embedded sub-object (a base or member with its own vtable)
                return std::string("vtable ") + t;
            }
            return "exe+" + Rva(v);
        }
        std::string s;
        uint32_t lo = static_cast<uint32_t>(v), hi = static_cast<uint32_t>(v >> 32);
        if (lo && lo < 0x400000)
            if (const char* n = GetCNameString(lo); n && *n) s += std::string("name? '") + n + "' ";
        float flo, fhi;
        memcpy(&flo, &lo, 4);
        memcpy(&fhi, &hi, 4);
        auto plausible = [](float x) { return x == x && ((x > 1e-3f && x < 1e6f) || (x < -1e-3f && x > -1e6f)); };
        if (plausible(flo) || plausible(fhi)) {
            _snprintf_s(b, _TRUNCATE, "f(%g, %g) ", flo, fhi);
            s += b;
        }
        _snprintf_s(b, _TRUNCATE, "i(%u, %u)", lo, hi);
        return s + b;
    }

    // Dumps `size` bytes of the object at p, then follows pointers to objects of interesting types (depth-limited).
    void Object(const char* label, uintptr_t p, size_t size, int depth) {
        if (!p || dumped.count(p) || objectCount > 3000) return;
        dumped.insert(p);
        ++objectCount;
        NoteType(p);
        const char* t = ObjectTypeName(p);
        fprintf(objects, "\n== %s at %p, type %s, %zx bytes\n", label, reinterpret_cast<void*>(p), t ? t : "?", size);
        std::vector<std::pair<std::string, uintptr_t>> follow;
        for (size_t off = 0; off < size; off += 8) {
            uintptr_t v = 0;
            if (!ReadQ(p + off, &v)) {
                fprintf(objects, "  +%04zx unreadable - stop\n", off);
                break;
            }
            std::string e = Explain(v);
            fprintf(objects, "  +%04zx %016llx %s\n", off, static_cast<unsigned long long>(v), e.c_str());
            if (const char* ct = ObjectTypeName(v); ct && strncmp(ct, ".?AV?$TTypedClass", 17) != 0) {
                char lab[256];
                _snprintf_s(lab, _TRUNCATE, "%s +%zx", label, off);
                if (depth > 0) {
                    for (const char* k : kFollow)
                        if (strstr(ct, k)) {
                            follow.push_back({lab, v});
                            break;
                        }
                } else {
                    for (const char* k : kMust)  // met past the depth: dumped in a second pass
                        if (strstr(ct, k)) {
                            must.push_back({lab, v});
                            break;
                        }
                }
            }
        }
        for (auto& [lab, v] : follow) Object(lab.c_str(), v, FollowSize(v), depth - 1);
    }

    static constexpr const char* kFollow[] = {"World", "Layer", "Foliage", "ContentManager", "DLCManager", "Animated",
                                              "Skeleton", "Mimic", "Behavior", "Engine", "RemoteConnection",
                                              "QuestsSystem", "ScriptingSystem", "Depot", "Camera", "Physics",
                                              "SRT", "Game"};
    // types the analysis side asked for: dumped even when they turn up past the follow depth
    static constexpr const char* kMust[] = {"CGameWorld", "CFoliageScene", "SRTBaseTree", "CFoliage", "CDLCManager",
                                            "CContentManager", "CAnimatedComponent", "CMovingPhysicalAgentComponent",
                                            "CQuest"};  // every quest type: threads, the system, graphs and their blocks
    std::vector<std::pair<std::string, uintptr_t>> must;

    // A followed object may be any C++ type: no virtual call on it (GetObjectClass), a size by its type name.
    static size_t FollowSize(uintptr_t p) {
        const char* t = ObjectTypeName(p);
        if (t && strstr(t, "Animated")) return 0x1200;
        if (t && strstr(t, "World")) return 0x1000;
        return 0x800;
    }

    // A reflected object's size from its class (only for objects known to be reflected: the player, its components,
    // theGame), else the fallback.
    static size_t SizeOf(uintptr_t p, size_t fallback) {
        __try {
            void* cls = GetObjectClass(reinterpret_cast<void*>(p));
            if (cls) {
                uint32_t s = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(cls) + 0x60);
                if (s >= 0x10 && s <= 0x20000) return s;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return fallback;
    }

    void Objects() {
        objects = Open(L"objects.txt");
        if (!objects) return;
        fprintf(objects, "# exe build %08x; offset, raw qword, meaning (-> typed pointer, name?, f(lo,hi), i(lo,hi))\n",
                GameBuild());
        auto player = reinterpret_cast<uintptr_t>(GetThePlayer());
        auto game = reinterpret_cast<uintptr_t>(GetTheGame());
        if (player) {
            Object("player", player, SizeOf(player, 0x1000), 2);
            uintptr_t list = 0, countRaw = 0;
            const uint32_t lo = FieldOr("CEntity.components", 0xf0), co = FieldOr("CEntity.componentCount", 0xf8);
            if (ReadQ(player + lo, &list) && ReadQ(player + co, &countRaw)) {
                uint32_t count = static_cast<uint32_t>(countRaw);
                for (uint32_t i = 0; i < count && i < 128; ++i) {
                    uintptr_t c = 0;
                    if (!ReadQ(list + i * 8, &c)) break;
                    char lab[64];
                    _snprintf_s(lab, _TRUNCATE, "player component %u", i);
                    size_t sz = SizeOf(c, 0x800);
                    const char* ct = ObjectTypeName(c);
                    if (ct && strstr(ct, "Animated") && sz < 0x1200) sz = 0x1200;  // past the pose fields
                    Object(lab, c, sz, 1);
                }
            }
        }
        if (game) Object("theGame", game, SizeOf(game, 0x16200), 2);
        for (auto& [lab, v] : globalsToFollow) Object(lab.c_str(), v, FollowSize(v), 0);
        if (auto rttiGet = reinterpret_cast<void* (*)()>(Address("SRTTI::Get")))
            Object("CRTTISystem", reinterpret_cast<uintptr_t>(rttiGet()), 0x200, 0);
        for (size_t i = 0; i < must.size() && i < 200; ++i) {  // grows while it runs: depth 1 finds more
            auto [lab, v] = must[i];
            Object(("must: " + lab).c_str(), v, FollowSize(v), 1);
        }
        fclose(objects);
        objects = nullptr;
    }

    int Threads() {
        FILE* f = Open(L"threads.txt");
        if (!f) return 0;
        using Query_t = LONG(WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);
        auto query = reinterpret_cast<Query_t>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        int n = 0;
        THREADENTRY32 te{sizeof te};
        fprintf(f, "# thread id, start address (exe RVA or module+offset), priority\n");
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
            uintptr_t start = 0;
            if (HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID)) {
                if (query) query(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, nullptr);
                CloseHandle(h);
            }
            char where[MAX_PATH + 32] = "?";
            if (InImage(start)) {
                _snprintf_s(where, _TRUNCATE, "exe+%s", Rva(start).c_str());
            } else {
                HMODULE m = nullptr;
                wchar_t mod[MAX_PATH] = L"";
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(start), &m) && m) {
                    GetModuleFileNameW(m, mod, MAX_PATH);
                    const wchar_t* base = wcsrchr(mod, L'\\');
                    _snprintf_s(where, _TRUNCATE, "%ls+%llx", base ? base + 1 : mod,
                              static_cast<unsigned long long>(start - reinterpret_cast<uintptr_t>(m)));
                }
            }
            fprintf(f, "%lu %s %ld\n", te.th32ThreadID, where, te.tpBasePri);
            ++n;
        }
        CloseHandle(snap);
        fclose(f);
        return n;
    }
};

void CmdEngineDump(const char* arg, char* reply, uint32_t size) {
    ImageRanges();
    SnapshotMemory();
    Dumper d;
    const bool quick = arg && strcmp(arg, "quick") == 0;  // no globals scan (minutes in the editor): objects only
    d.dir = Dir() + (quick ? L"\\out\\dump_quick" : L"\\out\\dump");
    CreateDirectoryW((Dir() + L"\\out").c_str(), nullptr);
    CreateDirectoryW(d.dir.c_str(), nullptr);
    ULONGLONG t0 = GetTickCount64();
    int globals = quick ? 0 : d.Globals();
    d.Objects();
    int threads = d.Threads();
    int types = d.Vtables();  // last: the objects above add their types
    _snprintf_s(reply, size, _TRUNCATE, "enginedump: %d globals, %d objects, %d types with vtables, %d threads in %llu ms -> out\\dump",
                globals, d.objectCount, types, threads, static_cast<unsigned long long>(GetTickCount64() - t0));
    Log("%s", reply);
}

// Annotated raw dump of [p, p+size) into out\dump\<file>, and the first 0x40 bytes behind every pointer in it that
// leads outside the exe (bytecode, arrays, sub-objects). For the analysis side: one live look at a structure.
bool DumpRegion(const std::wstring& file, uintptr_t p, size_t size, const char* title) {
    ImageRanges();
    SnapshotMemory();
    Dumper d;
    d.dir = Dir() + L"\\out\\dump";
    CreateDirectoryW((Dir() + L"\\out").c_str(), nullptr);
    CreateDirectoryW(d.dir.c_str(), nullptr);
    FILE* f = d.Open(file.c_str());
    if (!f) return false;
    const char* t = ObjectTypeName(p);
    fprintf(f, "# exe build %08x, image base %p; %s at %p, type %s, %zx bytes\n", GameBuild(),
            reinterpret_cast<void*>(g_imgBase), title, reinterpret_cast<void*>(p), t ? t : "?", size);
    std::vector<std::pair<size_t, uintptr_t>> behind;
    for (size_t off = 0; off < size; off += 8) {
        uintptr_t v = 0;
        if (!ReadQ(p + off, &v)) {
            fprintf(f, "  +%04zx unreadable - stop\n", off);
            break;
        }
        fprintf(f, "  +%04zx %016llx %s\n", off, static_cast<unsigned long long>(v), d.Explain(v).c_str());
        if (v && !InImage(v) && Readable(v, 0x40)) behind.push_back({off, v});
    }
    for (auto& [off, v] : behind) {
        fprintf(f, "\n-- behind +%04zx (%p):", off, reinterpret_cast<void*>(v));
        for (size_t i = 0; i < 0x40; i += 8) {
            uintptr_t q = 0;
            if (!ReadQ(v + i, &q)) break;
            fprintf(f, "%s%016llx", i % 32 ? " " : "\n  ", static_cast<unsigned long long>(q));
        }
        fprintf(f, "\n");
    }
    fclose(f);
    return true;
}

// fndump <GlobalFunction>: the CFunction (0x120 bytes) and what its fields point to -> out\dump\fn_<name>.txt
void CmdFnDump(const char* arg, char* reply, uint32_t size) {
    std::string name = arg ? arg : "";
    void* fn = name.empty() ? nullptr : FindGlobalFn(name.c_str());
    if (!fn) {
        _snprintf_s(reply, size, _TRUNCATE, "fndump: no global function '%s'", name.c_str());
        return;
    }
    std::wstring file = L"fn_" + std::wstring(name.begin(), name.end()) + L".txt";
    bool ok = DumpRegion(file, reinterpret_cast<uintptr_t>(fn), 0x120, ("CFunction " + name).c_str());
    _snprintf_s(reply, size, _TRUNCATE, "fndump %s: %p -> out\\dump\\fn_%s.txt%s", name.c_str(), fn, name.c_str(),
                ok ? "" : " (not written)");
}

// peek <hex address> [bytes]: an annotated dump of any readable memory -> out\dump\peek_<address>.txt
void CmdPeek(const char* arg, char* reply, uint32_t size) {
    unsigned long long a = 0, n = 0x100;
    if (!arg || sscanf_s(arg, "%llx %llx", &a, &n) < 1 || !a) {
        strncpy_s(reply, size, "peek <hex address> [hex bytes]", _TRUNCATE);
        return;
    }
    if (n > 0x4000) n = 0x4000;
    char file[64];
    _snprintf_s(file, _TRUNCATE, "peek_%llx.txt", a);
    std::string fs(file);
    bool ok = DumpRegion(std::wstring(fs.begin(), fs.end()), static_cast<uintptr_t>(a), static_cast<size_t>(n), "peek");
    _snprintf_s(reply, size, _TRUNCATE, "peek %llx (%llx bytes) -> out\\dump\\%s%s", a, n, file, ok ? "" : " (not written)");
}

}  // namespace

void InstallDump() {
    RegisterCommand("fndump", &CmdFnDump, "fndump <GlobalFunction>: the CFunction and what it points to -> out\\dump");
    RegisterCommand("peek", &CmdPeek, "peek <hex address> [hex bytes]: annotated memory dump -> out\\dump");
    RegisterCommand("enginedump", &CmdEngineDump,
                    "enginedump [quick]: globals, vtables, object dumps, threads -> tw3se\\out\\dump (quick: no globals)");
}

}  // namespace tw3se
