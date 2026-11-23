// TW3SE core - engine functions by name, for w3.engine_call, w3.engine_hook, w3.engine_addr and plugins
// (EngineAddress). A name is one of:
//   a name of TW3SE's address list            "CDrawableComponent::RefreshRenderProxies"
//   a class's vtable slot                     "CEntity::vf140" (the class's main vtable), "CEntity::vf0@10" (the
//                                             vtable at offset 0x10 of the object, for a second base)
//   an offset in witcher3.exe                 "0x2263570" or "witcher3.exe+0x2263570"
//   the function that loads a text            "text:PxRigidBodyExt::setMassAndUpdateInertia: No mass" (the text's
//                                             start; for functions without a name that print a warning)
// A slot is found by the class's name in the exe's MSVC RTTI (type descriptor ".?AVCEntity@@" in .data, the complete
// object locator that names it and the offset in .rdata, the vtable after the qword pointing at the locator), so it
// holds across patches as long as the class keeps its slot order. tools/enginemap.py lists classes and slots.
#include "core.h"

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tw3se {
namespace {

struct Section {
    uintptr_t a = 0, b = 0;
};
bool g_sectionsRead = false;
Section g_text, g_rdata, g_data;

void ReadSections() {
    if (g_sectionsRead) return;
    g_sectionsRead = true;
    auto base = reinterpret_cast<const uint8_t*>(ImageBase());
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char n[9] = {};
        memcpy(n, sec->Name, 8);
        Section s{ImageBase() + sec->VirtualAddress, ImageBase() + sec->VirtualAddress + sec->Misc.VirtualSize};
        if (!strcmp(n, ".text")) g_text = s;
        else if (!strcmp(n, ".rdata")) g_rdata = s;
        else if (!strcmp(n, ".data")) g_data = s;
    }
}

bool InText(uintptr_t p) { return p >= g_text.a && p < g_text.b; }

// "physx::NpScene" -> "NpScene@physx" (MSVC writes the innermost name first)
std::string Mangled(const std::string& cls) {
    std::string out, rest = cls;
    for (;;) {
        size_t k = rest.rfind("::");
        if (k == std::string::npos) {
            out += rest;
            break;
        }
        out += rest.substr(k + 2) + "@";
        rest = rest.substr(0, k);
    }
    return out;
}

const uint8_t* FindBytes(uintptr_t a, uintptr_t b, const char* pat, size_t n) {
    auto p = reinterpret_cast<const uint8_t*>(a);
    auto end = reinterpret_cast<const uint8_t*>(b) - n;
    for (; p <= end; ++p)
        if (*p == static_cast<uint8_t>(pat[0]) && !memcmp(p, pat, n)) return p;
    return nullptr;
}

std::mutex g_mx;
std::unordered_map<std::string, uintptr_t> g_vtables;  // "class@offset" -> vtable (0: not there)

// pats: ".?AV<name>@@\0" and ".?AU<name>@@\0" (each with its length); no C++ objects here (it runs under __try)
uintptr_t FindVtable(const char* patV, size_t nV, const char* patU, size_t nU, uint32_t offset) {
    if (!g_data.a || !g_rdata.a) return 0;
    const uint8_t* p = FindBytes(g_data.a, g_data.b, patV, nV);
    if (!p) p = FindBytes(g_data.a, g_data.b, patU, nU);
    if (!p) return 0;
    uintptr_t td = reinterpret_cast<uintptr_t>(p) - 0x10;  // the type descriptor: vtable, spare, then the name
    uint32_t tdRva = static_cast<uint32_t>(td - ImageBase());
    // the complete object locator: signature 1, offset, cdOffset, type descriptor, hierarchy, itself (all RVAs)
    uintptr_t col = 0;
    for (uintptr_t q = (g_rdata.a + 3) & ~uintptr_t(3); q + 24 <= g_rdata.b; q += 4) {
        auto u = reinterpret_cast<const uint32_t*>(q);
        if (u[3] == tdRva && u[0] == 1 && u[1] == offset && u[5] == static_cast<uint32_t>(q - ImageBase())) {
            col = q;
            break;
        }
    }
    if (!col) return 0;
    for (uintptr_t q = (g_rdata.a + 7) & ~uintptr_t(7); q + 16 <= g_rdata.b; q += 8)
        if (*reinterpret_cast<const uintptr_t*>(q) == col && InText(*reinterpret_cast<const uintptr_t*>(q + 8)))
            return q + 8;
    return 0;
}

uintptr_t FindVtableGuarded(const char* patV, size_t nV, const char* patU, size_t nU, uint32_t offset) {
    __try {
        return FindVtable(patV, nV, patU, nU, offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uintptr_t VtableOf(const std::string& cls, uint32_t offset) {
    std::string key = cls + "@" + std::to_string(offset);
    {
        std::lock_guard<std::mutex> l(g_mx);
        auto it = g_vtables.find(key);
        if (it != g_vtables.end()) return it->second;
    }
    ReadSections();
    std::string v = ".?AV" + Mangled(cls) + "@@", u = ".?AU" + Mangled(cls) + "@@";
    uintptr_t vt = FindVtableGuarded(v.c_str(), v.size() + 1, u.c_str(), u.size() + 1, offset);  // (with the \0)
    std::lock_guard<std::mutex> l(g_mx);
    g_vtables[key] = vt;
    return vt;
}

// "text:<start of a text>": the function that loads that text (a lea rip+disp to it in .text), found by its
// .pdata entry, a cold part's parent through its chained unwind info. No C++ objects (it runs under __try).
uintptr_t FunctionOfText(const char* text, size_t len) {
    const uint8_t* s = FindBytes(g_rdata.a, g_rdata.b, text, len);
    if (!s || (s > reinterpret_cast<const uint8_t*>(g_rdata.a) && s[-1] != 0)) return 0;  // (the text's start)
    auto lo = reinterpret_cast<const uint8_t*>(g_text.a);
    size_t n = g_text.b - g_text.a;
    for (size_t i = 0; i + 7 <= n; ++i) {
        if ((lo[i] != 0x48 && lo[i] != 0x4c) || lo[i + 1] != 0x8d || (lo[i + 2] & 0xc7) != 0x05) continue;
        int32_t disp;
        memcpy(&disp, lo + i + 3, 4);
        if (lo + i + 7 + disp != s) continue;
        DWORD64 base = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(lo + i), &base, nullptr);
        for (int hops = 0; rf && hops < 8; ++hops) {
            const uint8_t* ui = reinterpret_cast<const uint8_t*>(base + rf->UnwindData);
            if (!((ui[0] >> 3) & 4)) break;  // UNW_FLAG_CHAININFO: the parent's entry after the code array
            rf = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t*>(ui + 4 + ((ui[2] + 1) & ~1) * 2));
        }
        return rf ? static_cast<uintptr_t>(base + rf->BeginAddress) : 0;
    }
    return 0;
}
uintptr_t FunctionOfTextGuarded(const char* text, size_t len) {
    __try {
        return FunctionOfText(text, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

}  // namespace

// The address of an engine function by one of the names above; 0 and *err when there is none.
uintptr_t EngineResolve(const std::string& name, std::string* err) {
    ReadSections();
    if (uintptr_t a = Address(name.c_str())) return a;
    if (name.compare(0, 5, "text:") == 0 && name.size() > 9) {
        std::string t = name.substr(5);
        if (uintptr_t a = FunctionOfTextGuarded(t.c_str(), t.size())) return a;
        if (err) *err = "no function loads the text \"" + t + "\"";
        return 0;
    }
    std::string s = name;
    const char* exe = "witcher3.exe+";
    if (s.compare(0, strlen(exe), exe) == 0) s = s.substr(strlen(exe));
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char* end = nullptr;
        unsigned long long rva = strtoull(s.c_str() + 2, &end, 16);
        if (end && !*end) {
            uintptr_t a = ImageBase() + static_cast<uintptr_t>(rva);
            if (InText(a)) return a;
            if (err) *err = name + " lies outside witcher3.exe's code";
            return 0;
        }
    }
    size_t k = s.rfind("::vf");
    if (k != std::string::npos && k > 0) {
        std::string cls = s.substr(0, k);
        char* end = nullptr;
        long slot = strtol(s.c_str() + k + 4, &end, 10);
        uint32_t offset = 0;
        if (end && *end == '@') offset = static_cast<uint32_t>(strtoul(end + 1, &end, 16));
        if (end && !*end && (slot < 0 || slot >= 4096)) {
            if (err) *err = cls + " has no slot " + std::to_string(slot);
            return 0;
        }
        if (end && !*end) {
            uintptr_t vt = VtableOf(cls, offset);
            if (!vt) {
                if (err) *err = "no vtable of " + cls + (offset ? " at offset " + std::to_string(offset) : "") +
                                " in witcher3.exe";
                return 0;
            }
            uintptr_t fn = 0;
            if (!SafeRead(reinterpret_cast<const uintptr_t*>(vt) + slot, &fn, 8) || !InText(fn)) {
                if (err) *err = cls + " has no slot " + std::to_string(slot);
                return 0;
            }
            return fn;
        }
    }
    if (err) *err = name + " is not in TW3SE's address list, and no Class::vfN or 0x offset";
    return 0;
}

// The vtable of a class (offset 0x10 etc. for a second base), 0 if the exe has none by that name.
uintptr_t EngineVtable(const std::string& cls, uint32_t offset) { return VtableOf(cls, offset); }

}  // namespace tw3se
