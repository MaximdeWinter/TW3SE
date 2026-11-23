// TW3SE core - named engine addresses.
//
// The table (addresses.inc), made from the known build of witcher3.exe:
//   FUNC   - a function: its RVA in the known build and a byte signature (?? = relative operand)
//   FUNC_OPT - like FUNC, but TW3SE runs without it: on a build that lacks it, only the feature that needs it
//              stays off (most rows; FUNC is what TW3SE cannot start without)
//   GLOBAL - a global variable, found through an instruction in a FUNC that loads it RIP-relative
//   FIELD  - a structure offset, read from a displacement inside a FUNC
//   CALL   - a short function, found through a direct call to it in a FUNC
// In the known build each RVA is checked against its signature; in any other build the signature is searched in
// .text. GLOBAL, FIELD and CALL carry an anchor (the bytes from their instruction on), searched in their function
// when another build moved the instruction. An address that cannot be found stays 0 and everything that needs it
// stays off.
#include "core.h"
#include <cstring>
#include <map>
#include <vector>

namespace tw3se {
namespace {

constexpr uint32_t kKnownBuild = 0x6abd8695;  // witcher3.exe 5.0 (Steam), the build addresses.inc was made from
constexpr uint32_t kEditorBuild = 0x6ac574c5;  // REDkit editor.exe (addresses_redkit_editor.inc was made from)

// The first bytes of a function searched for a row's anchor (tools/sigs.py ANCHOR_WINDOW)
constexpr size_t kAnchorWindow = 0x2000;

// optional: a reference/analysis address (FUNC_OPT) that may be absent on a build without disabling the extender.
struct FuncRow { const char* name; uint32_t rva; const char* sig; bool optional; };
struct GlobalRow { const char* name; const char* fn; uint32_t insnOffset, dispOffset, insnLength; const char* anchor; };
struct FieldRow { const char* name; const char* fn; uint32_t dispOffset, knownValue; const char* anchor; uint32_t anchorDisp; };
struct CallRow { const char* name; const char* fn; uint32_t callOffset; const char* anchor; };

#define FUNC(n, r, s) {n, r, s, false},
#define FUNC_OPT(n, r, s) {n, r, s, true},
#define CALL(n, f, o, a)
#define GLOBAL(n, f, i, d, l, a)
#define FIELD(n, f, d, v, a, ad)
const FuncRow kFuncs[] = {
#include "addresses.inc"
};
#undef FUNC
#undef FUNC_OPT
#undef CALL
#undef GLOBAL
#undef FIELD
#define FUNC(n, r, s)
#define FUNC_OPT(n, r, s)
#define CALL(n, f, o, a)
#define GLOBAL(n, f, i, d, l, a) {n, f, i, d, l, a},
#define FIELD(n, f, d, v, a, ad)
const GlobalRow kGlobals[] = {
#include "addresses.inc"
};
#undef FUNC
#undef FUNC_OPT
#undef CALL
#undef GLOBAL
#undef FIELD
#define FUNC(n, r, s)
#define FUNC_OPT(n, r, s)
#define CALL(n, f, o, a)
#define GLOBAL(n, f, i, d, l, a)
#define FIELD(n, f, d, v, a, ad) {n, f, d, v, a, ad},
const FieldRow kFields[] = {
#include "addresses.inc"
};
#undef FUNC
#undef FUNC_OPT
#undef CALL
#undef GLOBAL
#undef FIELD
#define FUNC(n, r, s)
#define FUNC_OPT(n, r, s)
#define CALL(n, f, o, a) {n, f, o, a},
#define GLOBAL(n, f, i, d, l, a)
#define FIELD(n, f, d, v, a, ad)
const CallRow kCalls[] = {
#include "addresses.inc"
};
#undef FUNC
#undef FUNC_OPT
#undef CALL
#undef GLOBAL
#undef FIELD

// REDkit editor.exe: a second, partial table that grows stage by stage (docs/PLAN_REDKIT_VOLL.md). Built from
// addresses_redkit_editor.inc with the same macros.
#define FUNC(n, r, s) {n, r, s, false},
#define FUNC_OPT(n, r, s) {n, r, s, true},
#define CALL(n, f, o, a)
#define GLOBAL(n, f, i, d, l, a)
#define FIELD(n, f, d, v, a, ad)
const FuncRow kFuncsEd[] = {
#include "addresses_redkit_editor.inc"
};
#undef FUNC
#undef FUNC_OPT
#undef CALL
#undef GLOBAL
#undef FIELD
#define FUNC(n, r, s)
#define FUNC_OPT(n, r, s)
#define CALL(n, f, o, a)
#define GLOBAL(n, f, i, d, l, a) {n, f, i, d, l, a},
#define FIELD(n, f, d, v, a, ad)
const GlobalRow kGlobalsEd[] = {
#include "addresses_redkit_editor.inc"
};
#undef FUNC
#undef FUNC_OPT
#undef CALL
#undef GLOBAL
#undef FIELD
#define FUNC(n, r, s)
#define FUNC_OPT(n, r, s)
#define CALL(n, f, o, a) {n, f, o, a},
#define GLOBAL(n, f, i, d, l, a)
#define FIELD(n, f, d, v, a, ad)
const CallRow kCallsEd[] = {
#include "addresses_redkit_editor.inc"
};
#undef FUNC
#undef FUNC_OPT
#undef CALL
#undef GLOBAL
#undef FIELD
#define FUNC(n, r, s)
#define FUNC_OPT(n, r, s)
#define CALL(n, f, o, a)
#define GLOBAL(n, f, i, d, l, a)
#define FIELD(n, f, d, v, a, ad) {n, f, d, v, a, ad},
const FieldRow kFieldsEd[] = {
#include "addresses_redkit_editor.inc"
};
#undef FUNC
#undef FUNC_OPT
#undef CALL
#undef GLOBAL
#undef FIELD

std::map<std::string, uintptr_t> g_addr;
std::string g_optMissing;  // the optional rows not found, for the log line

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool> fixed;
};

Pattern Parse(const char* sig) {
    Pattern p;
    for (const char* s = sig; *s;) {
        if (*s == ' ') { ++s; continue; }
        if (s[0] == '?') {
            p.bytes.push_back(0);
            p.fixed.push_back(false);
            s += (s[1] == '?') ? 2 : 1;
        } else {
            p.bytes.push_back(static_cast<uint8_t>(strtoul(std::string(s, 2).c_str(), nullptr, 16)));
            p.fixed.push_back(true);
            s += 2;
        }
    }
    return p;
}

bool MatchAt(const uint8_t* at, const Pattern& p) {
    for (size_t i = 0; i < p.bytes.size(); ++i)
        if (p.fixed[i] && at[i] != p.bytes[i]) return false;
    return true;
}

void TextSection(const uint8_t*& begin, size_t& size) {
    auto base = reinterpret_cast<const uint8_t*>(ImageBase());
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!memcmp(sec->Name, ".text", 6)) {
            begin = base + sec->VirtualAddress;
            size = sec->Misc.VirtualSize;
            return;
        }
    }
    begin = nullptr;
    size = 0;
}

// A jump over a function's first bytes: another mod's hook (jmp rel32, jmp [rip+x], mov rax + jmp rax).
bool HookedAt(const uint8_t* at) {
    return at[0] == 0xE9 || (at[0] == 0xFF && at[1] == 0x25) ||
           (at[0] == 0x48 && at[1] == 0xB8 && at[10] == 0xFF && at[11] == 0xE0);
}

// The only match of `p` in .text, or 0 if none or more than one.
uintptr_t Scan(const Pattern& p) {
    const uint8_t* text;
    size_t size;
    TextSection(text, size);
    if (!text || p.bytes.empty() || !p.fixed[0]) return 0;
    uintptr_t found = 0;
    const uint8_t first = p.bytes[0];
    const uint8_t* end = text + size - p.bytes.size();
    for (const uint8_t* at = text; at < end;) {
        at = static_cast<const uint8_t*>(memchr(at, first, end - at));
        if (!at) break;
        if (MatchAt(at, p)) {
            if (found) return 0;  // not unique in this build
            found = reinterpret_cast<uintptr_t>(at);
        }
        ++at;
    }
    return found;
}

// A function another mod hooked before TW3SE, on a build without known RVAs: its first bytes in memory are the other
// mod's jump, so the signature is searched in witcher3.exe on disk, which has them as the game shipped (a chained
// dinput8.dll that hooks RestoreSession would make the signature miss). Taken only when the place in memory is hooked indeed.
uintptr_t ScanOnDisk(const Pattern& p) {
    if (p.bytes.empty() || !p.fixed[0]) return 0;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    uintptr_t found = 0;
    HANDLE map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const uint8_t* view = map ? static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0)) : nullptr;
    if (view) {
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(view + reinterpret_cast<const IMAGE_DOS_HEADER*>(view)->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (memcmp(sec->Name, ".text", 6) != 0 || sec->SizeOfRawData < p.bytes.size()) continue;
            const uint8_t* lo = view + sec->PointerToRawData;
            const uint8_t* end = lo + sec->SizeOfRawData - p.bytes.size();
            size_t hits = 0;
            uintptr_t rva = 0;
            for (const uint8_t* at = lo; at < end && hits < 2;) {
                at = static_cast<const uint8_t*>(memchr(at, p.bytes[0], end - at));
                if (!at) break;
                if (MatchAt(at, p)) {
                    ++hits;
                    rva = sec->VirtualAddress + static_cast<uintptr_t>(at - lo);
                }
                ++at;
            }
            if (hits == 1 && HookedAt(reinterpret_cast<const uint8_t*>(ImageBase() + rva))) found = ImageBase() + rva;
        }
        UnmapViewOfFile(view);
    }
    if (map) CloseHandle(map);
    CloseHandle(file);
    return found;
}

// Where a row's anchor sits in its function: the known offset when the anchor is there, else its only match in the
// function's first kAnchorWindow bytes (patch 5.01: one instruction more in funcLogChannel moved a call and a
// global by 9 bytes; read at the old offset the global was garbage and TW3SE would have written through it).
// -1: not there or not unique - the row stays 0.
int64_t AnchorOffset(const char* name, uintptr_t fn, uint32_t known, const char* anchor) {
    Pattern p = Parse(anchor);
    const uint8_t* text;
    size_t size;
    TextSection(text, size);
    const auto base = reinterpret_cast<const uint8_t*>(fn);
    if (!text || p.bytes.empty() || base < text || base >= text + size) return -1;
    const size_t room = static_cast<size_t>(text + size - base);
    const size_t window = room < kAnchorWindow ? room : kAnchorWindow;
    if (window < p.bytes.size()) return -1;
    if (known + p.bytes.size() <= window && MatchAt(base + known, p)) return known;
    int64_t found = -1;
    for (size_t k = 0; k + p.bytes.size() <= window; ++k) {
        if (!MatchAt(base + k, p)) continue;
        if (found >= 0) {
            Log("address %s: its anchor is not unique in this build", name);
            return -1;
        }
        found = static_cast<int64_t>(k);
    }
    if (found < 0) Log("address %s: its anchor is NOT FOUND in this build", name);
    else Log("address %s: moved %+lld bytes in its function", name, static_cast<long long>(found) - known);
    return found;
}

// Resolve one table set into g_addr; returns how many required (non-optional) entries were not found. rvaValid =
// the current module is the build these RVAs were made from (so a row can be taken at its RVA; otherwise scanned).
int ResolveTables(const FuncRow* funcs, size_t nf, const CallRow* calls, size_t nc, const GlobalRow* globals,
                  size_t ng, const FieldRow* fields, size_t nfl, bool rvaValid) {
    int missing = 0;
    // a CALL, GLOBAL or FIELD read from a FUNC_OPT is optional too: only the feature that needs it stays off
    auto optionalFn = [&](const char* fn) {
        for (size_t i = 0; i < nf; ++i)
            if (strcmp(funcs[i].name, fn) == 0) return funcs[i].optional;
        return false;
    };
    for (size_t i = 0; i < nf; ++i) {
        const FuncRow& f = funcs[i];
        Pattern p = Parse(f.sig);
        uintptr_t a = 0;
        const uint8_t* at = reinterpret_cast<const uint8_t*>(ImageBase() + f.rva);
        if (rvaValid && MatchAt(at, p)) {
            a = ImageBase() + f.rva;
        } else if (rvaValid && HookedAt(at)) {
            // another mod hooked it before TW3SE (a jump over its first bytes): on the known build the place is
            // certain, our hook goes after theirs
            a = ImageBase() + f.rva;
            Log("address %s: hooked by another mod before TW3SE - kept, the hooks chain", f.name);
        } else {
            a = Scan(p);
            if (a) {
                Log("address %s: found by signature", f.name);
            } else if ((a = ScanOnDisk(p)) != 0) {
                Log("address %s: hooked by another mod before TW3SE - found in witcher3.exe on disk, the hooks chain", f.name);
            } else {
                Log("address %s: NOT FOUND", f.name);
            }
        }
        if (!a && !f.optional) ++missing;  // an optional address may be absent: only its feature stays off
        if (!a && f.optional) g_optMissing += std::string(g_optMissing.empty() ? "" : ", ") + f.name;
        g_addr[f.name] = a;
    }
    for (size_t i = 0; i < nc; ++i) {
        const CallRow& c = calls[i];
        uintptr_t fn = Address(c.fn), a = 0;
        const int64_t at = fn ? AnchorOffset(c.name, fn, c.callOffset, c.anchor) : -1;
        if (at >= 0 && *reinterpret_cast<const uint8_t*>(fn + at) == 0xE8)
            a = fn + at + 5 + *reinterpret_cast<const int32_t*>(fn + at + 1);
        if (!a && !optionalFn(c.fn)) ++missing;
        if (!a && optionalFn(c.fn)) g_optMissing += std::string(g_optMissing.empty() ? "" : ", ") + c.name;
        g_addr[c.name] = a;
    }
    for (size_t i = 0; i < ng; ++i) {
        const GlobalRow& g = globals[i];
        uintptr_t fn = Address(g.fn), a = 0;
        const int64_t at = fn ? AnchorOffset(g.name, fn, g.insnOffset, g.anchor) : -1;
        if (at >= 0) {
            uintptr_t insn = fn + at;
            a = insn + g.insnLength + *reinterpret_cast<const int32_t*>(insn + g.dispOffset);
        }
        if (!a && !optionalFn(g.fn)) ++missing;
        if (!a && optionalFn(g.fn)) g_optMissing += std::string(g_optMissing.empty() ? "" : ", ") + g.name;
        g_addr[g.name] = a;
    }
    for (size_t i = 0; i < nfl; ++i) {
        const FieldRow& f = fields[i];
        uintptr_t fn = Address(f.fn), v = 0;
        const int64_t at = fn ? AnchorOffset(f.name, fn, f.dispOffset - f.anchorDisp, f.anchor) : -1;
        // The displacement is as wide as the run of wildcards at anchorDisp: 1 byte (disp8, e.g. [rcx+18h]) or 4.
        Pattern p = Parse(f.anchor);
        size_t width = 0;
        while (f.anchorDisp + width < p.fixed.size() && !p.fixed[f.anchorDisp + width]) ++width;
        if (at >= 0) {
            const auto* d = reinterpret_cast<const uint8_t*>(fn + at + f.anchorDisp);
            v = width == 1 ? static_cast<uint32_t>(static_cast<int8_t>(*d))
                           : static_cast<uint32_t>(*reinterpret_cast<const int32_t*>(d));
        }
        if (rvaValid && v != f.knownValue) {
            // the table was made from this very build: its value is the right one (a disp8 field read as 4 bytes
            // gives garbage a feature would use as an offset)
            Log("field %s: read 0x%llx, the table says 0x%x - the table's value is used", f.name,
                (unsigned long long)v, f.knownValue);
            v = f.knownValue;
        }
        if (!v && !optionalFn(f.fn)) ++missing;
        if (!v && optionalFn(f.fn)) g_optMissing += std::string(g_optMissing.empty() ? "" : ", ") + f.name;
        g_addr[f.name] = v;
    }
    return missing;
}

}  // namespace

bool IsRedkitEditor() { return GameBuild() == kEditorBuild; }

// Memory::AllocDefault has two ABIs. Retail witcher3.exe returns {pointer, size} through a hidden first argument:
// (out, size, align). The REDkit editor build takes (size, align, memory class) and returns the pointer in rax;
// its own native registration calls it as (0xf8, 0x10, 0x18) for every CFunction. Every TW3SE allocation goes
// through here so no caller can use the wrong one.
void* EngineAlloc(size_t size, uint32_t align, uint16_t memClass) {
    uintptr_t a = Address("Memory::AllocDefault");
    if (!a || !size) return nullptr;
    if (IsRedkitEditor())
        return reinterpret_cast<void* (*)(uint32_t, size_t, uint16_t)>(a)(static_cast<uint32_t>(size), align, memClass);
    struct { void* ptr; size_t size; } r{};
    reinterpret_cast<void (*)(void*, size_t, uint32_t)>(a)(&r, size, align);
    return r.ptr;
}

bool ResolveAddresses() {
    if (IsRedkitEditor()) {
        // REDkit editor: a partial table that grows stage by stage (docs/PLAN_REDKIT_VOLL.md). TW3SE runs when the
        // native-registration chain resolves; every other feature checks its own addresses in main.cpp.
        int missing = ResolveTables(kFuncsEd, std::size(kFuncsEd), kCallsEd, std::size(kCallsEd), kGlobalsEd,
                                    std::size(kGlobalsEd), kFieldsEd, std::size(kFieldsEd), true);
        Log("addresses (REDkit editor): %zu functions, %zu calls, %zu globals, %zu fields, %d missing (build %08x)",
            std::size(kFuncsEd), std::size(kCallsEd), std::size(kGlobalsEd), std::size(kFieldsEd), missing,
            GameBuild());
        // the scripting singleton the retail code reads as GScriptGlobals is the editor's native-table
        // singleton slot - one variable, two names.
        if (!Address("GScriptGlobals")) g_addr["GScriptGlobals"] = Address("GScriptNativeTable_ed_Singleton");
        // eb: FindType_ed has the retail form FindType(CRTTISystem*, const CName*) -> IRTTIType* (it looks in an
        // editor-only table first, then in the class table the retail code walks).
        if (!Address("CRTTISystem::FindType")) g_addr["CRTTISystem::FindType"] = Address("FindType_ed");
        static const char* const kChain[] = {"RegisterCoreScriptNatives", "CFunction::CFunction",
                                             "CRTTISystem::RegisterGlobalFunction", "Memory::AllocDefault",
                                             "CNamesPool::AddEntry", "CNamesPool::Get", "SRTTI::Get",
                                             "GScriptNativeTable"};
        for (const char* n : kChain)
            if (!Address(n)) {
                Log("addresses (REDkit editor): %s missing - natives cannot be registered", n);
                return false;
            }
        return true;
    }
    const bool known = GameBuild() == kKnownBuild;
    int missing = ResolveTables(kFuncs, std::size(kFuncs), kCalls, std::size(kCalls), kGlobals, std::size(kGlobals),
                                kFields, std::size(kFields), known);
    // (the build named here is the one TW3SE was made on; any other build is read by signature, which is the normal
    // case after a game patch. 23.11.: "unknown build" read to a player like the wrong game version)
    Log("addresses: %zu functions, %zu calls, %zu globals, %zu fields, %d missing (build %08x, %s)", std::size(kFuncs),
        std::size(kCalls), std::size(kGlobals), std::size(kFields), missing, GameBuild(),
        known ? "the reference build" : "found by signature");
    if (!g_optMissing.empty()) Log("addresses: not in this build, the features that need them stay off: %s", g_optMissing.c_str());
    return missing == 0;
}

void ExeText(const uint8_t** begin, size_t* size) { TextSection(*begin, *size); }

uintptr_t Address(const char* name) {
    auto it = g_addr.find(name);
    return it == g_addr.end() ? 0 : it->second;
}

// The named table address at or below `a` (for the watchdog's stacks): "name+0x1c", or "" when none is within 64 KB.
std::string NearestAddressName(uintptr_t a) {
    const char* best = nullptr;
    uintptr_t bv = 0;
    for (const auto& [n, v] : g_addr)
        if (v >= ImageBase() && v <= a && v > bv) bv = v, best = n.c_str();
    if (!best || a - bv > 0x10000) return "";
    char b[200];
    snprintf(b, sizeof b, "%s+0x%llx", best, static_cast<unsigned long long>(a - bv));
    return b;
}

// Every resolved name with its address (crashreport.cpp names a fault's function by them).
void ForEachAddress(void (*visit)(const char* name, uintptr_t a, void* ctx), void* ctx) {
    for (const auto& [n, v] : g_addr) visit(n.c_str(), v, ctx);
}

uint32_t FieldOr(const char* name, uint32_t retail) {
    uintptr_t v = Address(name);
    return v ? static_cast<uint32_t>(v) : retail;
}

}  // namespace tw3se
