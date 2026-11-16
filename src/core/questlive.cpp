// TW3SE core - the quest blocks the game activates, live, for an editor's quest graph.
//
// docs/FINDINGS_STORY_SPRUNG.md (S1c): CQuestThread::ActivateBlock(thread, block, arg) sets the
// block's active bit in the thread's data, calls the block's own OnActivate (each of the 59 block classes), adds
// it to the thread's active blocks and tells the thread's listeners. Hooked here: after the original, the block's
// GUID - the "guid" property every quest block has in its file (what saves remember blocks by) - read through
// RTTI (properties.cpp: no fixed offset), and one line for the editor:
//
//     qb|on|<32 hex digits: the GUID's 16 bytes as in the file>
//
// Off until asked for: `questlive on` (console / link); `questlive off`; `questlive` says the state and how many
// blocks were seen.
#include "core.h"
#include <unordered_map>
#include <mutex>

namespace tw3se {
namespace {

using ActivateBlock_t = void* (*)(void* thread, void* block, void* arg, void* a4);
ActivateBlock_t oActivateBlock = nullptr;
volatile bool g_on = false;
volatile uint32_t g_seen = 0, g_failed = 0;
std::mutex g_mx;
std::unordered_map<void*, void*> g_guidProp;    // CClass* -> its "guid" CProperty* (null: it has none)

// The 16 bytes of the block's GUID; SEH for a pointer that is not what it should be (never crash the game).
bool ReadGuid(void* field, uint8_t out[16]) {
    __try {
        memcpy(out, field, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* GuidProperty(void* block) {
    void* cls = GetObjectClass(block);
    if (!cls) return nullptr;
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_guidProp.find(cls);
    if (it != g_guidProp.end()) return it->second;
    void* prop = FindProperty(cls, "guid");
    g_guidProp[cls] = prop;
    return prop;
}

void Report(void* block) {
    void* prop = GuidProperty(block);
    void* field = prop ? PropertyPtr(block, prop) : nullptr;
    uint8_t g[16];
    if (!field || !ReadGuid(field, g)) {
        ++g_failed;
        return;
    }
    char line[64] = "qb|on|";
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        line[6 + 2 * i] = hex[g[i] >> 4];
        line[7 + 2 * i] = hex[g[i] & 15];
    }
    line[38] = 0;
    ++g_seen;
    LinkEmit(line);
}

bool ReportGuarded(void* block) {
    __try {
        Report(block);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* ActivateBlockHook(void* thread, void* block, void* arg, void* a4) {
    void* r = oActivateBlock(thread, block, arg, a4);
    if (g_on && block && !ReportGuarded(block)) ++g_failed;
    return r;
}

void CmdQuestLive(const char* args, char* reply, uint32_t size) {
    if (args && !strncmp(args, "on", 2)) g_on = true;
    else if (args && !strncmp(args, "off", 3)) g_on = false;
    _snprintf_s(reply, size, _TRUNCATE, "questlive %s hooked=%s seen=%u failed=%u", g_on ? "on" : "off",
                oActivateBlock ? "yes" : "no", g_seen, g_failed);
}

}  // namespace

void InstallQuestLive() {
    HookNamed("CQuestThread::ActivateBlock", reinterpret_cast<void*>(&ActivateBlockHook),
              reinterpret_cast<void**>(&oActivateBlock));
    RegisterCommand("questlive", &CmdQuestLive, "questlive [on|off]: quest blocks activated -> qb|on|<guid> lines");
    Log("questlive: %s", oActivateBlock ? "hooked" : "not available in this build");
}

}  // namespace tw3se
