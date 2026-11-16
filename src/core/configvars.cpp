// The engine's own configuration variables: 1725 statics like Rendering/UseUmbraOcclusion or
// Rendering/RT/PathTracer/..., each a Config::TConfigVar<T, Validation> object in the game's data: vtable,
// group string (+0x08), name string (+0x10), flags (+0x18), a pointer to the variable the engine reads (+0x20),
// the default (+0x28). T comes from the RTTI name (_N bool, H int, I unsigned, M float). The engine reads the bound
// variable where it uses the setting, so a write counts from its next use.
#include "core.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace tw3se {
namespace {

bool SafeWrite(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string TypeOfObj(void* obj) {
    char name[160] = {};
    uintptr_t vt = 0, col = 0;
    uint32_t tdRva = 0;
    if (!SafeRead(obj, &vt, 8) || vt < ImageBase() || !SafeRead(reinterpret_cast<void*>(vt - 8), &col, 8) ||
        col < ImageBase() || !SafeRead(reinterpret_cast<void*>(col + 12), &tdRva, 4) ||
        !SafeRead(reinterpret_cast<void*>(ImageBase() + tdRva + 0x10), name, sizeof name - 1))
        return "";
    return name;
}

bool ImageText(uintptr_t p, std::string& out) {  // a short ASCII string inside the image
    if (p < ImageBase()) return false;
    char b[96] = {};
    if (!SafeRead(reinterpret_cast<void*>(p), b, sizeof b - 1)) return false;
    size_t n = strnlen(b, sizeof b - 1);
    if (!n || n >= sizeof b - 1) return false;
    for (size_t i = 0; i < n; ++i)
        if (b[i] < 32 || b[i] > 126) return false;
    out.assign(b, n);
    return true;
}

struct Var {
    std::string group, name;
    char type;   // 'b' bool, 'i' int, 'u' unsigned, 'f' float
    void* value; // the bound variable
};
std::vector<Var> g_vars;
std::map<std::string, size_t> g_index;  // "group/name" in lower case
bool g_scanned = false;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

void Scan() {
    if (g_scanned) return;
    g_scanned = true;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(ImageBase());
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(ImageBase() + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    // in the file the objects carry TConfigVarBinding's vtable, their constructors give them TConfigVar's
    // (measured); both share the slots, Get reads [this+0x20]
    const char* kPrefixes[2] = {".?AV?$TConfigVarBinding@", ".?AV?$TConfigVar@"};
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (memcmp(sec->Name, ".data", 5) != 0) continue;
        uintptr_t lo = ImageBase() + sec->VirtualAddress, hi = lo + sec->Misc.VirtualSize;
        std::map<uintptr_t, char> vtType;  // vtable -> type (0: not a binding)
        for (uintptr_t at = lo; at + 0x30 <= hi; at += 8) {
            uintptr_t q[5];
            if (!SafeRead(reinterpret_cast<void*>(at), q, sizeof q)) continue;
            if (q[0] < ImageBase() || q[0] >= lo) continue;  // a vtable lies in .rdata, before .data
            auto it = vtType.find(q[0]);
            char t;
            if (it != vtType.end()) {
                t = it->second;
            } else {
                std::string n = TypeOfObj(reinterpret_cast<void*>(at));
                t = 0;
                for (const char* prefix : kPrefixes) {
                    if (n.compare(0, strlen(prefix), prefix) != 0) continue;
                    std::string arg = n.substr(strlen(prefix), 2);
                    t = arg == "_N" ? 'b' : arg[0] == 'H' ? 'i' : arg[0] == 'I' ? 'u' : arg[0] == 'M' ? 'f' : 0;
                }
                vtType[q[0]] = t;
            }
            if (!t) continue;
            Var v;
            v.type = t;
            v.value = reinterpret_cast<void*>(q[4]);
            if (!ImageText(q[1], v.group) || !ImageText(q[2], v.name) || !v.value) continue;
            uint32_t probe;
            if (!SafeRead(v.value, &probe, 4)) continue;
            g_index[Lower(v.group + "/" + v.name)] = g_vars.size();
            g_vars.push_back(v);
        }
    }
    Log("config: %zu engine configuration variables", g_vars.size());
}

}  // namespace

// The variable's value as text, "" with the reason in err when there is none. A set writes `value` first.
bool EngineConfig(const char* group, const char* name, const char* value, std::string& out, std::string& err) {
    Scan();
    auto it = g_index.find(Lower(std::string(group) + "/" + name));
    if (it == g_index.end()) return err = std::string("no engine variable ") + group + "/" + name, false;
    Var& v = g_vars[it->second];
    if (value) {
        bool ok = true;
        switch (v.type) {
            case 'b': {
                std::string s = Lower(value);
                uint8_t b = s == "true" || s == "1" || s == "on";
                ok = SafeWrite(v.value, &b, 1);
                break;
            }
            case 'i': {
                int32_t i = atoi(value);
                ok = SafeWrite(v.value, &i, 4);
                break;
            }
            case 'u': {
                uint32_t u = static_cast<uint32_t>(strtoul(value, nullptr, 10));
                ok = SafeWrite(v.value, &u, 4);
                break;
            }
            default: {
                float f = static_cast<float>(atof(value));
                ok = SafeWrite(v.value, &f, 4);
            }
        }
        if (!ok) return err = "the variable cannot be written", false;
    }
    char b[64];
    switch (v.type) {
        case 'b': {
            uint8_t x = 0;
            SafeRead(v.value, &x, 1);
            out = x ? "true" : "false";
            break;
        }
        case 'i': {
            int32_t x = 0;
            SafeRead(v.value, &x, 4);
            snprintf(b, sizeof b, "%d", x);
            out = b;
            break;
        }
        case 'u': {
            uint32_t x = 0;
            SafeRead(v.value, &x, 4);
            snprintf(b, sizeof b, "%u", x);
            out = b;
            break;
        }
        default: {
            float x = 0;
            SafeRead(v.value, &x, 4);
            snprintf(b, sizeof b, "%g", x);
            out = b;
        }
    }
    return true;
}

// "group/name<TAB>type" of every variable whose path holds `filter` (lower case match)
std::vector<std::string> EngineConfigList(const char* filter) {
    Scan();
    std::vector<std::string> out;
    std::string f = Lower(filter ? filter : "");
    for (const Var& v : g_vars) {
        std::string path = v.group + "/" + v.name;
        if (!f.empty() && Lower(path).find(f) == std::string::npos) continue;
        out.push_back(path + "\t" + (v.type == 'b' ? "bool" : v.type == 'i' ? "int" : v.type == 'u' ? "uint" : "float"));
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace tw3se
