// Byte patches (w3.patch): bytes written into the game's code or data, the bytes they replace kept, so a patch can
// be taken back (and is, when the Lua mod that made it is taken out). The place is an address, an offset in
// witcher3.exe, an engine function by name (EngineResolve) or a signature that matches once in the code.
#include "core.h"

#include <windows.h>

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace tw3se {
namespace {

struct BytePatch {
    uintptr_t at = 0;
    std::vector<uint8_t> old, now;
    std::string owner;
};
std::mutex g_mx;
std::map<int, BytePatch> g_patches;
int g_next = 1;

bool WriteBytes(uintptr_t at, const uint8_t* bytes, size_t n) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(reinterpret_cast<void*>(at), bytes, n);
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(at), n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
    return true;
}

bool IsSignature(const std::string& s) {
    if (s.find(' ') == std::string::npos && s.find('?') == std::string::npos) return false;
    for (char c : s)
        if (!isxdigit(static_cast<unsigned char>(c)) && c != ' ' && c != '?') return false;
    return true;
}

}  // namespace

// The address of a place: an integer (below 0x10000000 an offset in witcher3.exe), an engine name, or a signature
// that matches exactly once. 0 and *err if there is none.
uintptr_t PatchPlace(const std::string* name, uintptr_t value, std::string* err) {
    if (!name) return value < 0x10000000 ? ImageBase() + value : value;
    if (IsSignature(*name)) {
        std::vector<uintptr_t> hits = SignatureScan(name->c_str(), 2);
        if (hits.size() != 1) {
            *err = hits.empty() ? "the signature matches nowhere" : "the signature matches more than once";
            return 0;
        }
        return hits[0];
    }
    return EngineResolve(*name, err);
}

int PatchBytes(uintptr_t at, const std::vector<uint8_t>& bytes, const std::string& owner, std::string* err) {
    if (!at || bytes.empty() || bytes.size() > 4096) {
        *err = "no place, or no bytes (or more than 4096)";
        return 0;
    }
    BytePatch p;
    p.at = at;
    p.now = bytes;
    p.old.resize(bytes.size());
    p.owner = owner;
    if (!SafeRead(reinterpret_cast<void*>(at), p.old.data(), p.old.size())) {
        *err = "the place cannot be read";
        return 0;
    }
    std::lock_guard<std::mutex> l(g_mx);
    for (const auto& kv : g_patches) {
        const BytePatch& o = kv.second;
        if (at < o.at + o.now.size() && o.at < at + bytes.size()) {
            *err = "another patch covers these bytes (" + std::to_string(kv.first) + ")";
            return 0;
        }
    }
    if (!WriteBytes(at, bytes.data(), bytes.size())) {
        *err = "the place cannot be made writable";
        return 0;
    }
    int id = g_next++;
    g_patches[id] = std::move(p);
    Log("patch %d: %zu bytes at witcher3.exe+0x%llx (%s)", id, bytes.size(),
        static_cast<unsigned long long>(at - ImageBase()), owner.c_str());
    return id;
}

bool PatchRestore(int id) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_patches.find(id);
    if (it == g_patches.end()) return false;
    const BytePatch& p = it->second;
    std::vector<uint8_t> cur(p.now.size());
    if (SafeRead(reinterpret_cast<void*>(p.at), cur.data(), cur.size()) && cur != p.now)
        Log("patch %d: its bytes were changed since (another mod?) - the old ones go back anyway", id);
    WriteBytes(p.at, p.old.data(), p.old.size());
    g_patches.erase(it);
    return true;
}

int PatchRestoreOwner(const std::string& owner) {
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> l(g_mx);
        for (const auto& kv : g_patches)
            if (kv.second.owner == owner) ids.push_back(kv.first);
    }
    for (int id : ids) PatchRestore(id);
    return static_cast<int>(ids.size());
}

}  // namespace tw3se
