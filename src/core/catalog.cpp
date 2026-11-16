// TW3SE core - the RTTI catalog: every type and function the engine has registered, in one file.
//
// CRTTISystem keeps two hashmaps with the same node layout (docs/engine/scripting.md sections 1, 4, 8): types
// (bucket count +0x10, bucket array +0x30) and global functions (+0x40 / +0x60); a node holds the key name id at
// +0x10, the value at +0x08 and the next node at +0x18. A CClass carries its parent (+0x10), its own properties
// (+0x30 / +0x38) and its own functions (+0x48 / +0x50). Walking all buckets of both maps and, for each class, its
// properties and functions makes every registered engine class visible at once - no single lookups per feature.
//
// Measured with the catalog itself and checked against the game's scripts (tools/catalog.py check):
//   CFunction+0x20  the return value as a CProperty named "__return" (null for no return value)
//   CFunction+0xe8  the function of a parent class this one overrides (its owner class at +0x08)
//   CEnum+0x20/+0x28  name -> value nodes of 24 bytes {name id, int32 value, ...} and their count; unused nodes
//                     hold no valid name (351 of 352 script enums match exactly)
//   IRTTIType slot 2 = size (equal to CClass+0x60 for all 6664 classes), slot 1 = name, slot 4 = kind
//
// `catalog` writes tw3se\catalog\rtti.json. Every read goes through SafeCopy (an access violation becomes a
// skipped field, not a crash); the only calls into the engine are the IRTTIType getters at vtable slots 1-4
// (name, size, alignment, kind). tools/catalog.py searches the file.
#include "core.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tw3se {
namespace {

// The two hashmaps' fields: retail values, or the REDkit editor's from its table.
uint32_t TypesCount() { return FieldOr("CRTTISystem.classCapacity", 0x10); }
uint32_t TypesBuckets() { return FieldOr("CRTTISystem.classBuckets", 0x30); }
uint32_t FuncsCount() { return FieldOr("CRTTISystem.globalFuncCapacity", 0x40); }
uint32_t FuncsBuckets() { return FieldOr("CRTTISystem.globalFuncBuckets", 0x60); }

using GetRtti_t = void* (*)();

bool SafeCopy(void* dst, const void* src, size_t n) {
    if (!MemReadable(src, n)) return false;  // the REDkit editor's crash handler sees a fault before our __except
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T> T Get(const void* base, size_t off, T def = T()) {
    T v;
    if (!base || !SafeCopy(&v, static_cast<const char*>(base) + off, sizeof v)) return def;
    return v;
}

bool Plausible(const void* p) {
    uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v > 0x10000 && v < 0x7fffffffffffULL && (v & 7) == 0;
}

// IRTTIType slot 4: the kind (2 = class). -1 when the call faults.
int SafeKind(void* type) {
    __try {
        void** vt = *reinterpret_cast<void***>(type);
        return reinterpret_cast<int (*)(void*)>(vt[4])(type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// IRTTIType slot 2 (size) or 3 (alignment). UINT32_MAX when the call faults.
uint32_t SafeU32Slot(void* type, int slot) {
    __try {
        void** vt = *reinterpret_cast<void***>(type);
        return reinterpret_cast<uint32_t (*)(void*)>(vt[slot])(type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return UINT32_MAX;
    }
}

// IRTTIType slot 1: the type's name id. Whether the getter returns a reference or fills a hidden return slot, a
// spare slot is passed in rdx and the returned pointer read: both conventions end in a pointer to the id.
uint32_t SafeNameId(void* type) {
    __try {
        void** vt = *reinterpret_cast<void***>(type);
        uint32_t spare = UINT32_MAX;
        const uint32_t* p = reinterpret_cast<const uint32_t* (*)(void*, uint32_t*)>(vt[1])(type, &spare);
        return p ? *p : UINT32_MAX;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return UINT32_MAX;
    }
}

struct MapEntry {
    uint32_t id;
    void* value;
};

// All entries of one CRTTISystem hashmap. A cap on buckets and chain length keeps a wrong offset from walking
// garbage forever.
std::vector<MapEntry> WalkMap(void* rtti, size_t countOff, size_t bucketsOff) {
    std::vector<MapEntry> out;
    uint32_t n = Get<uint32_t>(rtti, countOff);
    void** arr = Get<void**>(rtti, bucketsOff);
    if (!n || n > (1u << 22) || !Plausible(arr)) return out;
    for (uint32_t b = 0; b < n; ++b) {
        void* node = Get<void*>(arr, b * sizeof(void*));
        for (int steps = 0; node && steps < 100000; ++steps) {
            if (!Plausible(node)) break;
            out.push_back({Get<uint32_t>(node, 0x10, UINT32_MAX), Get<void*>(node, 0x08)});
            node = Get<void*>(node, 0x18);
        }
    }
    return out;
}

std::string Esc(const char* s) {
    std::string o;
    if (!s) return o;
    for (; *s; ++s) {
        unsigned char c = static_cast<unsigned char>(*s);
        if (c == '"' || c == '\\') o += '\\', o += static_cast<char>(c);
        else if (c < 0x20) {
            char b[8];
            sprintf_s(b, "\\u%04x", c);
            o += b;
        } else o += static_cast<char>(c);
    }
    return o;
}

std::string NameOf(uint32_t id) {
    if (id == UINT32_MAX) return "?";
    const char* s = GetCNameString(id);
    return s ? s : "#" + std::to_string(id);
}

std::string Quote(const std::string& s) { return "\"" + Esc(s.c_str()) + "\""; }

class Catalog {
public:
    std::unordered_map<void*, std::string> typeNames;  // every type pointer seen, by name
    uint32_t unnamedTypes = 0, sizeChecks = 0, sizeMismatches = 0;
    uint32_t classes = 0, props = 0, methods = 0, enumValues = 0;

    std::string TypeName(void* type) {
        if (!type) return "";
        auto it = typeNames.find(type);
        if (it != typeNames.end()) return it->second;
        if (!Plausible(type)) return "?";
        std::string n = NameOf(SafeNameId(type));  // a type not in the map (none seen on 5.01)
        if (n == "?") ++unnamedTypes;
        typeNames[type] = n;
        return n;
    }

    // One CProperty (class field or function parameter): name +0x10, type +0x08, offset +0x20, flags +0x24.
    std::string Prop(void* p, bool withOffset) {
        std::string s = "{\"name\":" + Quote(NameOf(Get<uint32_t>(p, 0x10, UINT32_MAX))) +
                        ",\"type\":" + Quote(TypeName(Get<void*>(p, 0x08)));
        if (withOffset) s += ",\"offset\":" + std::to_string(Get<uint32_t>(p, 0x20));
        return s + ",\"flags\":" + std::to_string(Get<uint32_t>(p, 0x24)) + "}";
    }

    // One CFunction: name +0x10, flags +0x18, return +0x20, parameters +0x28 / +0x30, opcode +0xe0,
    // overridden function +0xe8.
    std::string Function(void* fn) {
        std::string s = "{\"name\":" + Quote(NameOf(Get<uint32_t>(fn, 0x10, UINT32_MAX)));
        s += ",\"flags\":" + std::to_string(Get<uint32_t>(fn, 0x18));
        s += ",\"opcode\":" + std::to_string(Get<uint32_t>(fn, 0xe0));
        void* ret = Get<void*>(fn, 0x20);
        if (Plausible(ret)) s += ",\"ret\":" + Quote(TypeName(Get<void*>(ret, 0x08)));
        void* base = Get<void*>(fn, FieldOr("CFunction.OverriddenFunction", 0xe8));  // REDkit editor: +0xf0
        if (Plausible(base) && Plausible(Get<void*>(base, 0x08)))
            s += ",\"overrides\":" + Quote(TypeName(Get<void*>(base, 0x08)));
        void** params = Get<void**>(fn, 0x28);
        uint32_t np = Get<uint32_t>(fn, 0x30);
        s += ",\"params\":[";
        if (Plausible(params) && np < 64)
            for (uint32_t i = 0; i < np; ++i) {
                void* p = Get<void*>(params, i * sizeof(void*));
                if (!Plausible(p)) continue;
                if (s.back() != '[') s += ',';
                s += Prop(p, false);
            }
        return s + "]}";
    }

    // A pointer array (CClass own properties or functions): pointer at +ptrOff, count at +ptrOff + 8.
    template <class F> std::string List(void* cls, size_t ptrOff, uint32_t* counter, F item) {
        std::string s = "[";
        void** arr = Get<void**>(cls, ptrOff);
        uint32_t n = Get<uint32_t>(cls, ptrOff + 8);
        if (!Plausible(arr) || n > 8192) return s + "]";
        for (uint32_t i = 0; i < n; ++i) {
            void* e = Get<void*>(arr, i * sizeof(void*));
            if (!Plausible(e)) continue;
            if (s.size() > 1) s += ',';
            s += item(e);
            ++*counter;
        }
        return s + "]";
    }

    // An enum's values: nodes of 24 bytes {name id, value} at +0x20, count at +0x28. Nodes without a valid name
    // are unused and skipped.
    std::string EnumValues(void* t) {
        char* arr = Get<char*>(t, 0x20);
        uint32_t n = Get<uint32_t>(t, 0x28);
        if (!Plausible(arr) || n > 65536) return "";
        std::map<int32_t, std::vector<std::string>> byValue;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t id = Get<uint32_t>(arr, i * 24u);
            const char* name = id ? GetCNameString(id) : nullptr;
            if (name && *name) byValue[Get<int32_t>(arr, i * 24u + 4)].push_back(name);
        }
        std::string s;
        for (auto& [value, names] : byValue)
            for (auto& name : names) {
                s += (s.empty() ? "" : ",") + ("[" + Quote(name) + "," + std::to_string(value) + "]");
                ++enumValues;
            }
        return ",\"values\":[" + s + "]";
    }

    std::string Type(const MapEntry& e, std::map<int, uint32_t>& kinds) {
        void* t = e.value;
        int kind = SafeKind(t);
        ++kinds[kind];
        uint32_t size = SafeU32Slot(t, 2), align = SafeU32Slot(t, 3);
        std::string s = "{\"name\":" + Quote(NameOf(e.id)) + ",\"kind\":" + std::to_string(kind);
        if (size != UINT32_MAX) s += ",\"size\":" + std::to_string(size);
        if (align != UINT32_MAX) s += ",\"align\":" + std::to_string(align);
        if (kind == 2) {
            ++classes;
            ++sizeChecks;
            if (size != Get<uint32_t>(t, 0x60, UINT32_MAX)) ++sizeMismatches;
            void* parent = Get<void*>(t, 0x10);
            if (Plausible(parent)) s += ",\"parent\":" + Quote(TypeName(parent));
            s += ",\"props\":" + List(t, 0x30, &props, [&](void* p) { return Prop(p, true); });
            s += ",\"methods\":" + List(t, 0x48, &methods, [&](void* f) { return Function(f); });
        }
        if (kind == 1) s += EnumValues(t);  // bitfields (kind 9) store names elsewhere: EnumValues gives none (measured)
        return s + "}";
    }
};

bool WriteText(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &w, nullptr) && w == text.size();
    CloseHandle(h);
    return ok;
}

void CmdCatalog(const char*, char* reply, uint32_t size) {
    auto getRtti = Fn<GetRtti_t>("SRTTI::Get");
    void* rtti = getRtti ? getRtti() : nullptr;
    if (!rtti) {
        strncpy_s(reply, size, "catalog: RTTI system not available", _TRUNCATE);
        return;
    }
    ULONGLONG t0 = GetTickCount64();
    std::vector<MapEntry> types = WalkMap(rtti, TypesCount(), TypesBuckets());
    std::vector<MapEntry> funcs = WalkMap(rtti, FuncsCount(), FuncsBuckets());

    Catalog c;
    for (const MapEntry& e : types)
        if (Plausible(e.value)) c.typeNames[e.value] = NameOf(e.id);

    std::map<int, uint32_t> kinds;
    char build[16];
    sprintf_s(build, "%08x", GameBuild());
    std::string out = "{\"build\":\"" + std::string(build) + "\",\"tw3se\":\"" + std::to_string(TW3SE_VERSION_MAJOR) +
                      "." + std::to_string(TW3SE_VERSION_MINOR) + "." + std::to_string(TW3SE_VERSION_PATCH) +
                      "\",\n\"types\":[\n";
    bool first = true;
    for (const MapEntry& e : types) {
        if (!Plausible(e.value)) continue;
        out += (first ? "" : ",\n") + c.Type(e, kinds);
        first = false;
    }
    out += "\n],\n\"functions\":[\n";
    first = true;
    uint32_t globals = 0;
    for (const MapEntry& e : funcs) {
        if (!Plausible(e.value)) continue;
        out += (first ? "" : ",\n") + c.Function(e.value);
        first = false;
        ++globals;
    }
    out += "\n]}\n";

    std::string kindList;
    for (auto& [k, n] : kinds) kindList += (kindList.empty() ? "" : " ") + std::to_string(k) + ":" + std::to_string(n);
    std::wstring dir = Dir() + L"\\catalog";
    CreateDirectoryW(dir.c_str(), nullptr);
    bool ok = WriteText(dir + L"\\rtti.json", out);
    _snprintf_s(reply, size, _TRUNCATE,
                "catalog: %zu types (%u classes, %u props, %u methods, %u enum values), %u global functions, "
                "kinds {%s}, unnamed %u, size check %u/%u, %.1f MB in %llu ms%s",
                types.size(), c.classes, c.props, c.methods, c.enumValues, globals, kindList.c_str(), c.unnamedTypes,
                c.sizeChecks - c.sizeMismatches, c.sizeChecks, out.size() / 1048576.0, GetTickCount64() - t0,
                ok ? " -> catalog\\rtti.json" : " - FILE NOT WRITTEN");
    Log("%s", reply);
}

using FindType_t = void* (*)(void* rtti, const void* name);  // CRTTISystem::FindType(rtti, CName*)

// The enum nodes (+0x20, 24 bytes {name id, value}, count +0x28), calling `visit(name, value)` until it returns true.
template <class F> bool ForEachEnumValue(void* t, F visit) {
    if (SafeKind(t) == 9) {  // a bitfield: one name id per bit right in the type, from +0x0c (measured on
                             // EEntityStaticFlags and EDrawableFlags; an unnamed bit has id 0), value 1 << bit
        uint32_t bits = RttiSize(t) ? RttiSize(t) * 8 : 32;
        if (bits > 64) bits = 64;
        for (uint32_t b = 0; b < bits; ++b) {
            uint32_t id = Get<uint32_t>(t, 0x0c + b * 4u);
            const char* name = id ? GetCNameString(id) : nullptr;
            if (name && *name && visit(name, static_cast<int32_t>(b < 32 ? (1u << b) : 0u))) return true;
        }
        return false;
    }
    char* arr = Get<char*>(t, 0x20);
    uint32_t n = Get<uint32_t>(t, 0x28);
    if (!Plausible(arr) || n > 65536) return false;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t id = Get<uint32_t>(arr, i * 24u);
        const char* name = id ? GetCNameString(id) : nullptr;
        if (name && *name && visit(name, Get<int32_t>(arr, i * 24u + 4))) return true;
    }
    return false;
}

}  // namespace

bool SafeRead(const void* src, void* dst, size_t n) { return src && SafeCopy(dst, src, n); }

// Whether [p, p+n) is committed, readable and no guard page. In the game a fault in a guarded read only becomes
// `false`; in the REDkit editor its crash handler (a vectored handler, it runs before any __except) writes a dump
// and ends the process (enginedump). So every read of a foreign pointer asks first. The last region found
// is kept per thread: walks over one object cost one VirtualQuery.
namespace {
// The readable regions of the process, sorted and merged (VirtualQuery over the whole address space). A lookup is a
// binary search; an address the map does not cover is asked of Windows alone (its region, one VirtualQuery). One
// VirtualQuery per read made a Lua type lookup take tenths of a second and froze the editor.
// The whole walk takes 110-125 ms in the game: done on the main thread for every address the map did not know (at most
// every 100 ms), it cost a hitch every one to three seconds while the pose hook checked its garbage pointers.
// So only the first ask walks there; a thread of its own walks again every 5 s, for the regions freed meanwhile.
// A region freed after the map was read faults on the read: the editor guard hands that fault to TW3SE's __except.
SRWLOCK g_mapLock = SRWLOCK_INIT;
std::vector<std::pair<uintptr_t, uintptr_t>> g_map;
ULONGLONG g_mapAt = 0;

void RebuildMap() {
    std::vector<std::pair<uintptr_t, uintptr_t>> m;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    uintptr_t a = 0x10000;
    MEMORY_BASIC_INFORMATION mbi{};
    while (a < 0x7ffffffeffffULL && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) {
        uintptr_t beg = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = beg + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & readable) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            if (!m.empty() && m.back().second == beg) m.back().second = end;
            else m.push_back({beg, end});
        }
        if (end <= a) break;
        a = end;
    }
    AcquireSRWLockExclusive(&g_mapLock);
    g_map.swap(m);
    g_mapAt = GetTickCount64();
    ReleaseSRWLockExclusive(&g_mapLock);
}

DWORD WINAPI MapThread(void*) {
    for (;;) {
        Sleep(5000);
        RebuildMap();
    }
}

// The region around one address, asked of Windows (one VirtualQuery) and put into the map when it is readable. An
// address the map does not know is most often memory the process took after the map was read (the heap grows after
// a script reload; counted as unreadable, such memory would fail Lua calls with "the struct could not be read" on a
// value Lua had just copied into its own memory).
bool AddRegionOf(uintptr_t a, uintptr_t* regionEnd) {
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || !(mbi.Protect & readable) || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    uintptr_t beg = reinterpret_cast<uintptr_t>(mbi.BaseAddress), end = beg + mbi.RegionSize;
    *regionEnd = end;
    AcquireSRWLockExclusive(&g_mapLock);
    auto it = std::lower_bound(g_map.begin(), g_map.end(), std::make_pair(beg, beg));
    it = g_map.insert(it, {beg, end});
    std::vector<std::pair<uintptr_t, uintptr_t>> merged;  // keep it sorted and without overlaps
    merged.reserve(g_map.size());
    for (const auto& r : g_map) {
        if (!merged.empty() && r.first <= merged.back().second) merged.back().second = (std::max)(merged.back().second, r.second);
        else merged.push_back(r);
    }
    g_map.swap(merged);
    ReleaseSRWLockExclusive(&g_mapLock);
    return true;
}

int InMap(uintptr_t a, size_t n) {  // 1 readable, 0 not, -1 the map is empty
    AcquireSRWLockShared(&g_mapLock);
    int r = g_map.empty() ? -1 : 0;
    size_t lo = 0, hi = g_map.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (a < g_map[mid].first) hi = mid;
        else if (a >= g_map[mid].second) lo = mid + 1;
        else {
            r = a + n <= g_map[mid].second ? 1 : 0;
            break;
        }
    }
    ReleaseSRWLockShared(&g_mapLock);
    return r;
}
}  // namespace

bool MemReadable(const void* p, size_t n) {
    auto a = reinterpret_cast<uintptr_t>(p);
    // A garbage length (a count read from the wrong offset) is refused at once. No read TW3SE makes is this large.
    if (n > (64u << 20)) return false;
    if (a < 0x10000 || a + n < a || a > 0x7ffffffeffffULL) return false;
    int r = InMap(a, n);
    if (r == -1) {  // the first ask: the whole map, and the thread that keeps it fresh
        static std::once_flag once;
        std::call_once(once, [] {
            RebuildMap();
            if (HANDLE h = CreateThread(nullptr, 0, &MapThread, nullptr, 0, nullptr)) CloseHandle(h);
        });
        r = InMap(a, n);
    }
    if (r == 1) return true;
    // new memory (or none): Windows asked about its regions, each put into the map - up to the read's end, a read may
    // cross from one region into the next
    uintptr_t at = a;
    for (int i = 0; i < 64; ++i) {
        uintptr_t end = 0;
        if (!AddRegionOf(at, &end)) return false;
        if (end >= a + n) break;
        at = end;
    }
    return InMap(a, n) == 1;
}

void RttiTypes(void (*visit)(const char* name, void* type, void* ctx), void* ctx) {
    auto getRtti = Fn<GetRtti_t>("SRTTI::Get");
    void* rtti = getRtti ? getRtti() : nullptr;
    if (!rtti) return;
    for (const MapEntry& e : WalkMap(rtti, TypesCount(), TypesBuckets())) {
        const char* n = GetCNameString(e.id);
        if (n && Plausible(e.value)) visit(n, e.value, ctx);
    }
}

int RttiKind(void* type) { return Plausible(type) ? SafeKind(type) : -1; }

uint32_t RttiSize(void* type) {
    uint32_t s = Plausible(type) ? SafeU32Slot(type, 2) : UINT32_MAX;
    return s == UINT32_MAX ? 0 : s;
}

std::string RttiName(void* type) {
    if (!Plausible(type)) return "";
    uint32_t id = SafeNameId(type);
    const char* s = id == UINT32_MAX ? nullptr : GetCNameString(id);
    return s ? s : "";
}

void* RttiFindType(const char* name) {
    auto getRtti = Fn<GetRtti_t>("SRTTI::Get");
    auto findType = Fn<FindType_t>("CRTTISystem::FindType");
    uint32_t id = name ? InternName(name) : 0;
    if (!getRtti || !findType || !id) return nullptr;
    return findType(getRtti(), &id);  // a CName is its id; its address serves as CName*
}

void RttiFunctions(void (*visit)(void* fn, void* ctx), void* ctx) {
    auto getRtti = Fn<GetRtti_t>("SRTTI::Get");
    void* rtti = getRtti ? getRtti() : nullptr;
    if (!rtti) return;
    for (const MapEntry& e : WalkMap(rtti, FuncsCount(), FuncsBuckets()))
        if (Plausible(e.value)) visit(e.value, ctx);
    for (const MapEntry& e : WalkMap(rtti, TypesCount(), TypesBuckets())) {
        if (!Plausible(e.value) || SafeKind(e.value) != 2) continue;
        void** arr = Get<void**>(e.value, 0x48);
        uint32_t n = Get<uint32_t>(e.value, 0x50);
        if (!Plausible(arr) || n > 8192) continue;
        for (uint32_t i = 0; i < n; ++i) {
            void* f = Get<void*>(arr, i * sizeof(void*));
            if (Plausible(f)) visit(f, ctx);
        }
    }
}

void RttiEnumNodes(void* enumType, void (*visit)(const char* name, int32_t value, void* ctx), void* ctx) {
    ForEachEnumValue(enumType, [&](const char* name, int32_t v) {
        visit(name, v, ctx);
        return false;
    });
}

bool RttiEnumName(void* enumType, int32_t value, std::string& out) {
    return ForEachEnumValue(enumType, [&](const char* name, int32_t v) {
        if (v != value) return false;
        out = name;
        return true;
    });
}

bool RttiEnumValue(void* enumType, const char* name, int32_t* out) {
    return name && ForEachEnumValue(enumType, [&](const char* n, int32_t v) {
        if (_stricmp(n, name) != 0) return false;
        *out = v;
        return true;
    });
}

void InstallCatalog() {
    RegisterCommand("catalog", &CmdCatalog, "catalog: every registered type and function -> tw3se\\catalog\\rtti.json");
}

}  // namespace tw3se
