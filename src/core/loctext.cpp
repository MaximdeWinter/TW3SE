// TW3SE core - localized texts replaced by id. Every text the game shows by its string id
// (dialogue lines and choices in scenes, UI texts, item names, GetLocStringById) comes from one function of the
// localization manager: funcGetLocStringById calls it as GetLocalizedText(manager, &out, id, true), 18 callers,
// among them the story-scene code (read in the native's handler). Hooked on the first replacement: after the
// original, a replaced id gets its own text in `out`. Any thread may ask for a text, so the table has a lock.
#include "core.h"
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace tw3se {
namespace {
struct Replacement {
    std::string text;
    std::string owner;  // the Lua mod that set it ("" = the console or a plugin)
};
std::shared_mutex g_mx;
std::unordered_map<uint32_t, Replacement> g_texts;
std::atomic<int> g_count{0};

using GetText_t = void* (*)(void* manager, void* out, uint32_t id, bool flag);
GetText_t oGetText = nullptr;

void* GetTextHook(void* manager, void* out, uint32_t id, bool flag) {
    void* r = oGetText(manager, out, id, flag);
    if (g_count.load(std::memory_order_relaxed) && out) {
        std::string text;
        bool found = false;
        {
            std::shared_lock<std::shared_mutex> l(g_mx);
            auto it = g_texts.find(id);
            if (it != g_texts.end()) {
                text = it->second.text;
                found = true;
            }
        }
        if (found) WriteStringField(out, text.data(), text.size());
    }
    return r;
}

bool EnsureHook() {
    static int state = 0;  // 0 not tried, 1 in, -1 failed
    if (state) return state > 0;
    void* target = reinterpret_cast<void*>(Address("CLocalizationManager::GetLocalizedText"));
    bool ok = target && Hook(target, reinterpret_cast<void*>(&GetTextHook), reinterpret_cast<void**>(&oGetText));
    Log("loctext: text hook %s", ok ? "in" : "NOT in (the address is missing)");
    state = ok ? 1 : -1;
    return ok;
}
}  // namespace

bool LocTextSet(uint32_t id, const char* text, const std::string& owner) {
    if (!id || !text || !EnsureHook()) return false;
    std::unique_lock<std::shared_mutex> l(g_mx);
    g_texts[id] = {text, owner};
    g_count.store(static_cast<int>(g_texts.size()));
    return true;
}

bool LocTextClear(uint32_t id) {
    std::unique_lock<std::shared_mutex> l(g_mx);
    bool had = g_texts.erase(id) != 0;
    g_count.store(static_cast<int>(g_texts.size()));
    return had;
}

int LocTextDropOwner(const std::string& owner) {
    std::unique_lock<std::shared_mutex> l(g_mx);
    int n = 0;
    for (auto it = g_texts.begin(); it != g_texts.end();) {
        if (it->second.owner == owner) {
            it = g_texts.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    g_count.store(static_cast<int>(g_texts.size()));
    return n;
}

int LocTextCount() { return g_count.load(); }

}  // namespace tw3se
