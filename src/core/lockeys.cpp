// TW3SE core - texts for localisation keys of TW3SE's own: the option pages of Lua mods in the game's
// Options > Mods and the names of w3.items. The game asks GetLocStringByKeyExt for every label of its menus, item
// tooltips and more; a Lua hook on it ran Lua for each of those calls. Here the native's table entry points at
// KeyHandler: it reads the key argument once (as funcLogChannel reads its string and frees it with Memory::Free)
// and, for a key in the table, writes the text as the result. Any other key puts the cursor back and runs the
// game's own handler, which reads the argument again. Lua fills the table through w3._loctext.
// The engine's own lookup by key (GetStringByStringKey) is hooked as well: Flash asks it for a text written as
// [[key]] (the loot feed's item names), and the script natives ask it when KeyHandler passed a key on.
#include "core.h"

#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace tw3se {
namespace {

struct Frame {  // ScriptFrame (scripting.cpp): the context at +0x00, the bytecode cursor at +0x30
    void* context;
    uint8_t pad[0x28];
    uint8_t* code;
};
static_assert(offsetof(Frame, code) == 0x30, "the cursor of a script frame is at +0x30");

using Native = void (*)(void* self, void* frame, void* result);
using OpHandler = void (*)(void* context, void* frame, void* out);
using Free_t = void (*)(void*);

Native g_orig = nullptr;
OpHandler* g_ops = nullptr;  // GScriptNativeTable: the handler of each opcode
Free_t g_free = nullptr;
std::shared_mutex g_mx;
std::unordered_map<std::string, std::string> g_keys;
std::atomic<int> g_count{0};
std::atomic<uint64_t> g_calls{0}, g_hits{0};
void** g_slot = nullptr;  // the native's table entry while KeyHandler is in it
std::atomic<uint64_t> g_engineHits{0};

bool Lookup(const char* key, std::string* text) {
    if (!key) return false;
    std::shared_lock<std::shared_mutex> l(g_mx);
    auto it = g_keys.find(key);
    if (it == g_keys.end()) return false;
    *text = it->second;
    return true;
}

// GetStringByStringKey(manager, &out, &key): the game's answer first (it makes `out`, which the caller may pass
// uninitialised), then TW3SE's text over it for a key of TW3SE's
struct EngineString {
    char* data;
    uint32_t length;
};
using ByKey_t = void* (*)(void* manager, void* out, const EngineString* key);
ByKey_t g_origByKey = nullptr;
void* ByKeyHook(void* manager, void* out, const EngineString* key) {
    void* r = g_origByKey(manager, out, key);
    if (g_count.load(std::memory_order_relaxed) && out && key) {
        std::string text;
        if (Lookup(key->data, &text)) {
            WriteStringField(out, text.data(), text.size());
            g_engineHits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return r;
}

void EnsureEngineHook() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    void* target = reinterpret_cast<void*>(Address("CLocalizationManager::GetStringByStringKey"));
    bool ok = target && Hook(target, reinterpret_cast<void*>(&ByKeyHook), reinterpret_cast<void**>(&g_origByKey));
    Log("lockeys: the engine's lookup by key %s", ok ? "hooked (Flash's [[key]] texts)" : "NOT hooked (the address is missing)");
}

void KeyHandler(void* self, void* frame, void* result) {
    auto f = static_cast<Frame*>(frame);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_count.load(std::memory_order_relaxed)) {
        g_orig(self, frame, result);
        return;
    }
    uint8_t* before = f->code;
    uint8_t op = *f->code;
    uint8_t buf[16] = {};  // a String: char* data, u32 length
    f->code += 1;
    if (op && g_ops[op]) g_ops[op](f->context, frame, buf);
    char* key = *reinterpret_cast<char**>(buf);
    std::string text;
    bool hit = false;
    if (key) hit = Lookup(key, &text);
    if (key && g_free) g_free(key);
    if (!hit) {
        f->code = before;  // the game's handler reads the argument itself
        g_orig(self, frame, result);
        return;
    }
    g_hits.fetch_add(1, std::memory_order_relaxed);
    f->code += 1;  // the end-of-arguments marker
    if (result) WriteStringField(result, text.data(), text.size());
}

bool EnsureHandler() {
    static int state = 0;  // 0 not tried, 1 in, -1 failed
    if (state) return state > 0;
    state = -1;
    void* fn = FindGlobalFn("GetLocStringByKeyExt");
    g_ops = reinterpret_cast<OpHandler*>(Address("GScriptNativeTable"));
    g_free = reinterpret_cast<Free_t>(Address("Memory::Free"));
    if (!fn || !g_ops || !g_free) {
        Log("lockeys: GetLocStringByKeyExt, the opcode table or Memory::Free missing - the keys stay unanswered");
        return false;
    }
    uint32_t flags = 0, opcode = 0;
    memcpy(&flags, static_cast<char*>(fn) + 0x18, 4);
    memcpy(&opcode, static_cast<char*>(fn) + 0xe0, 4);
    if (!(flags & 1) || !(flags & 2) || !opcode) {  // a global native with its own table entry
        Log("lockeys: GetLocStringByKeyExt is no global native here (flags %08x) - the keys stay unanswered", flags);
        return false;
    }
    void** slot = reinterpret_cast<void**>(&g_ops[opcode]);
    void* orig = *slot;
    if (!orig) return false;
    g_orig = reinterpret_cast<Native>(orig);
    void* mine = reinterpret_cast<void*>(&KeyHandler);
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = mine;
    VirtualProtect(slot, sizeof(void*), old, &old);
    g_slot = slot;
    Log("lockeys: GetLocStringByKeyExt answers TW3SE's keys in C (opcode %u)", opcode);
    state = 1;
    return true;
}

// `lockeys off` puts the game's own handler back (the A/B check), `lockeys on` KeyHandler again
void CmdLocKeys(const char* args, char* reply, uint32_t size) {
    std::string a = args ? args : "";
    if ((a == "off" || a == "on") && g_slot && g_orig) {
        void* want = a == "off" ? reinterpret_cast<void*>(g_orig) : reinterpret_cast<void*>(&KeyHandler);
        DWORD old = 0;
        if (VirtualProtect(g_slot, sizeof(void*), PAGE_READWRITE, &old)) {
            *g_slot = want;
            VirtualProtect(g_slot, sizeof(void*), old, &old);
        }
        Log("lockeys: %s", a == "off" ? "the game's own handler again" : "KeyHandler again");
    }
    bool in = g_slot && *g_slot == reinterpret_cast<void*>(&KeyHandler);
    snprintf(reply, size, "lockeys: %s, %d key(s), %llu call(s), %llu answered, %llu answered in the engine's lookup",
             in ? "on" : "off", g_count.load(), static_cast<unsigned long long>(g_calls.load()),
             static_cast<unsigned long long>(g_hits.load()), static_cast<unsigned long long>(g_engineHits.load()));
}

}  // namespace

bool LocKeySet(const char* key, const char* text) {
    static bool cmd = false;
    if (!cmd) {
        cmd = true;
        RegisterCommand("lockeys", &CmdLocKeys, "lockeys [off|on]: TW3SE's localisation keys and how often the game asked (off: the game's own handler)");
    }
    if (!key || !*key || !text) return false;
    EnsureEngineHook();
    if (!EnsureHandler()) return false;
    std::unique_lock<std::shared_mutex> l(g_mx);
    g_keys[key] = text;
    g_count.store(static_cast<int>(g_keys.size()));
    return true;
}

}  // namespace tw3se
