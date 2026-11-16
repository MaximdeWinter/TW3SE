// TW3SE core - world edits: remove, move, set alight or re-skin pre-placed world entities, kept across reloads, no
// bake, optionally only while a quest fact says so. Mechanism: hook CEntity::AttachToWorld, the one function every
// entity passes on its way into the world - the pre-placed ones of a layer (CLayer::AttachToWorld loops over them)
// and the ones created at run time (CDynamicLayer::FinishEntityAttach). At its return the entity and its
// components are attached. (CDynamicLayer::CreateAndAttachEntity sees
// only created entities, never a pre-placed house or crate.)
//
// The match key is the entity's own GUID at CEntity+0xcc - the 16-byte value GetGuidHash reads (6a), the same
// value WitcherScript sees (TW3SE_EntityGuid). A pre-placed entity carries the GUID it has in its layer file (four
// random dwords). An entity created at run time gets a counter (ffffffff ffffffff n 0): not a key that survives,
// so edits are for pre-placed entities.
//
// Where the edits come from (docs/manual/world-edit.md), read at start and on every game load:
//   tw3se\world.txt, dlc\*\tw3se\world.txt, Mods\*\tw3se\world.txt   one edit per line (a quest brings its own)
//   tw3se\world_removed.txt, world_removed_classes.txt, world_moved.txt, world_effect.txt, world_appearance.txt
//                                                                     the first, single-purpose lists
//
// A change runs on the next tick (RunOnMainThread), never inline: destroying or moving mid-attach corrupts
// streaming and crashes. Whether the entity still lives by then is asked of its handle block: every object points
// at its block at +0x08 (measured on the player and the game object), the block holds the object at +0x08 and the
// engine nulls it when the object dies. TW3SE takes a reference on the block (so it stays valid) and never keeps a
// raw pointer it cannot check. A conditional edit stays tracked that way; twice a second the facts are asked again
// and the edit is applied or taken back on entities that are loaded.
#include "core.h"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <unordered_map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace tw3se {
namespace {

struct Guid {
    uint32_t d[4];
};
enum class Act { Remove, Move, Effect, Appearance, Tag, Light, Anim };
struct Entry {
    Act act;
    Guid g;
    std::string name;      // effect / appearance name, the tag of a tag edit
    std::string elseName;  // appearance put back when the condition stops holding ("else <name>")
    std::string mode;      // anim: "" forward once, "back", "loop"
    float pos[3];          // move: target position x/y/z
    float rot[3];          // move: pitch/yaw/roll
    std::string ifFact;    // applies only while FactsQuerySum(ifFact) > 0
    std::string unlessFact;  // ... and while FactsQuerySum(unlessFact) == 0
    bool conditional() const { return !ifFact.empty() || !unlessFact.empty(); }
};

std::mutex g_mx;                            // the lists: the attach hook reads them, a game load replaces them
std::vector<Entry> g_entries;
std::vector<std::string> g_removedClasses;  // remove every entity of these classes
bool g_log = false;                         // config worldedit_log: log attaching entities so an author can find a guid
int g_logged = 0;                           // rate limit for the discovery log

bool GuidMatch(const Guid& g, const uint32_t* c) {
    return g.d[0] == c[0] && g.d[1] == c[1] && g.d[2] == c[2] && g.d[3] == c[3];
}

// An all-zero GUID is not a real object key: many engine-internal entities attach with a zeroed spawn context,
// so a zero entry would match hundreds of them at once. Rejected everywhere.
bool GuidZero(const Guid& g) { return !g.d[0] && !g.d[1] && !g.d[2] && !g.d[3]; }

// --- reading the edits

// One world.txt line: "<action> <d0> <d1> <d2> <d3> [args] [else <name>] [if <fact>] [unless <fact>]", or
// "removeclass <Class>". Returns false for a comment, an empty line or a line it cannot read.
bool ParseLine(const std::string& line, Entry& e, std::string& removeClass) {
    std::istringstream in(line);
    std::vector<std::string> t;
    for (std::string w; in >> w;) t.push_back(w);
    if (t.empty() || t[0][0] == '#') return false;
    if (t[0] == "removeclass") {
        if (t.size() < 2) return false;
        removeClass = t[1];
        return true;
    }
    if (t[0] == "remove") e.act = Act::Remove;
    else if (t[0] == "move") e.act = Act::Move;
    else if (t[0] == "effect") e.act = Act::Effect;
    else if (t[0] == "appearance") e.act = Act::Appearance;
    else if (t[0] == "tag") e.act = Act::Tag;
    else if (t[0] == "light") e.act = Act::Light;
    else if (t[0] == "anim") e.act = Act::Anim;
    else return false;
    if (t.size() < 5) return false;
    for (int i = 0; i < 4; ++i) e.g.d[i] = static_cast<uint32_t>(strtoul(t[1 + i].c_str(), nullptr, 16));
    if (GuidZero(e.g)) return false;
    size_t k = 5;
    if (e.act == Act::Move) {
        if (t.size() < k + 6) return false;
        for (int i = 0; i < 3; ++i) e.pos[i] = strtof(t[k + i].c_str(), nullptr);
        for (int i = 0; i < 3; ++i) e.rot[i] = strtof(t[k + 3 + i].c_str(), nullptr);
        k += 6;
    } else if (e.act == Act::Effect || e.act == Act::Appearance || e.act == Act::Tag || e.act == Act::Light ||
               e.act == Act::Anim) {
        if (t.size() < k + 1) return false;
        e.name = t[k++];
        if (e.act == Act::Anim && k < t.size() && (t[k] == "back" || t[k] == "loop" || t[k] == "once"))
            e.mode = t[k] == "once" ? "" : t[k], ++k;
    }
    for (; k + 1 < t.size(); k += 2) {
        if (t[k] == "if") e.ifFact = t[k + 1];
        else if (t[k] == "unless") e.unlessFact = t[k + 1];
        else if (t[k] == "else") e.elseName = t[k + 1];
        else return false;
    }
    return true;
}

int LoadWorldFile(const std::wstring& path, std::vector<Entry>& out, std::vector<std::string>& classes) {
    std::ifstream f(path.c_str());
    int n = 0;
    for (std::string line; std::getline(f, line);) {
        Entry e{};
        std::string cls;
        if (!ParseLine(line, e, cls)) continue;
        if (!cls.empty()) classes.push_back(cls);
        else out.push_back(e);
        ++n;
    }
    return n;
}

// The first, single-purpose lists (still read): one line each, the action given by the file.
void LoadLegacy(const wchar_t* file, const char* action, std::vector<Entry>& out, std::vector<std::string>& classes) {
    std::ifstream f((Dir() + file).c_str());
    for (std::string line; std::getline(f, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        Entry e{};
        std::string cls;
        if (ParseLine(std::string(action) + " " + line, e, cls)) {
            if (!cls.empty()) classes.push_back(cls);
            else out.push_back(e);
        }
    }
}

// <game> from <game>\bin\x64_dx12\tw3se
std::wstring GameRoot() {
    std::wstring d = Dir();
    for (int i = 0; i < 3; ++i) d = d.substr(0, d.find_last_of(L"\\/"));
    return d;
}

// Every <root>\<folder>\*\tw3se\world.txt (a quest's DLC or a mod brings its own edits).
int LoadFolderWorlds(const wchar_t* folder, std::vector<Entry>& out, std::vector<std::string>& classes) {
    int files = 0;
    std::wstring base = GameRoot() + L"\\" + folder + L"\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        std::wstring p = base + fd.cFileName + L"\\tw3se\\world.txt";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && LoadWorldFile(p, out, classes) > 0) ++files;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return files;
}

void LoadEdits() {
    std::vector<Entry> entries;
    std::vector<std::string> classes;
    LoadWorldFile(Dir() + L"\\world.txt", entries, classes);
    int files = LoadFolderWorlds(L"dlc", entries, classes) + LoadFolderWorlds(L"Mods", entries, classes);
    LoadLegacy(L"\\world_removed.txt", "remove", entries, classes);
    LoadLegacy(L"\\world_removed_classes.txt", "removeclass", entries, classes);
    LoadLegacy(L"\\world_moved.txt", "move", entries, classes);
    LoadLegacy(L"\\world_effect.txt", "effect", entries, classes);
    LoadLegacy(L"\\world_appearance.txt", "appearance", entries, classes);
    int conditional = 0;
    for (const Entry& e : entries) conditional += e.conditional() ? 1 : 0;
    Log("worldedit: %u edit(s) (%d conditional) + %u removed class(es), %d world.txt from dlc/Mods",
        static_cast<unsigned>(entries.size()), conditional, static_cast<unsigned>(classes.size()), files);
    std::lock_guard<std::mutex> l(g_mx);
    g_entries.swap(entries);
    g_removedClasses.swap(classes);
}

// --- applying them

// The handle block of an object (object+0x08) and whether it still holds that object (block+0x08).
void* BlockOf(void* obj) { return *reinterpret_cast<void**>(reinterpret_cast<char*>(obj) + 0x08); }
void* Alive(void* block) { return *reinterpret_cast<void**>(reinterpret_cast<char*>(block) + 0x08); }

// Reposition an entity via CEntity vtable slot 0x460 - void(CEntity*, Vector* pos, EulerAngles* rot) - the
// internal method behind the TeleportWithRotation native (6a, docs/engine/world.md), which also runs the
// physics/render sync. The Vector is x/y/z/w (w = 1).
// The slot moves between builds (the REDkit editor's CEntity has five more virtual methods before it): retail uses
// 0x460, the editor only the slot its table names - without it a move does nothing rather than call the wrong one.
void TeleportEntity(void* ent, const float* pos, const float* rot) {
    uint32_t slot = IsRedkitEditor() ? static_cast<uint32_t>(Address("CEntity.vtTeleport"))
                                     : FieldOr("CEntity.vtTeleport", 0x460);
    // A slot that Teleport itself calls (call [rax+slot] in its first bytes) is one of its inner setters, not
    // Teleport (in the editor the position setter's slot can come first). Such a slot is refused.
    if (const auto* t = reinterpret_cast<const uint8_t*>(Address("CEntity::Teleport_ed")); slot && t)
        for (int i = 0; i + 6 <= 0x60; ++i)
            if (t[i] == 0xFF && t[i + 1] == 0x90 && *reinterpret_cast<const uint32_t*>(t + i + 2) == slot) {
                slot = 0;
                break;
            }
    if (!slot) {
        static bool told = false;
        if (!told) Log("worldedit: no teleport slot for this build - moves are skipped");
        told = true;
        return;
    }
    void** vtbl = *reinterpret_cast<void***>(ent);
    auto teleport = reinterpret_cast<void (*)(void*, const float*, const float*)>(vtbl[slot / 8]);
    float pos4[4] = {pos[0], pos[1], pos[2], 1.0f};
    float rot3[3] = {rot[0], rot[1], rot[2]};
    teleport(ent, pos4, rot3);
}

// An entity's world position, asked of the engine (the GetWorldPosition method-native writes a Vector). Reading
// fields guessed from the layout gave a direction-like value for the player; this is what scripts see.
bool WorldPosGuarded(void* ent, float out[3]) {
    float v[4] = {0, 0, 0, 0};
    __try {
        if (!CallMethodV(ent, "GetWorldPosition", "", v)) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    out[0] = v[0];
    out[1] = v[1];
    out[2] = v[2];
    return true;
}

// One action on a live entity, SEH-guarded (no C++ objects in here: C2712). `on` false takes it back where that
// is possible: StopEffect, the "else" appearance, the removal (shown again), the move (back where it stood).
// A removed entity is hidden, not destroyed: Destroy does nothing to an entity of a layer (measured: the
// lamp stays, and the same entity takes an effect afterwards). Hidden = SetHideInGame(true, immediate), its running
// effects stopped and every enabled component switched off (collision, interaction, lights, sound). Seen in game:
// the lamp, its flame and its collision gone; taken back, all of it there again.
constexpr int kMaxComps = 64;

// What a hidden entity had: the components switched off (their handle blocks, a reference held) and the effects
// that were running ("fire,smoke"), so taking it back restores exactly those. A move keeps where the entity stood.
struct Hidden {
    void* comps[kMaxComps];
    int ncomps;
    int total, unasked;  // the last hiding: components in all, IsEnabled not answered (for the log)
    char fx[256];
    bool homeSet;
    float home[4], homeRot[4];  // position, EulerAngles as GetWorldRotation writes them (pitch, yaw, roll)
};

void HideGuarded(void* ent, bool hide, Hidden* h) {
    char* e = reinterpret_cast<char*>(ent);
    char spec[80], list[1024];
    CallMethodV(ent, "SetHideInGame", hide ? "b:1 b:1" : "b:0 b:1", nullptr);
    // Effects run apart from the components (a hidden lamp left its flame in the air); the lamp starts its
    // flame itself with PlayEffect, so the running ones are noted (IsEffectActive over the template's effects)
    // before all are stopped, and those are played again when it is shown.
    if (hide) {
        h->fx[0] = 0;
        if (EntityEffectNames(ent, list, sizeof list)) {
            char* ctx = nullptr;
            for (char* n = strtok_s(list, ",", &ctx); n; n = strtok_s(nullptr, ",", &ctx)) {
                int32_t active = 0;
                _snprintf_s(spec, _TRUNCATE, "n:%s", n);
                if (CallMethodV(ent, "IsEffectActive", spec, &active) && (active & 0xff)) {
                    if (h->fx[0]) strncat_s(h->fx, ",", _TRUNCATE);
                    strncat_s(h->fx, n, _TRUNCATE);
                }
            }
        }
        CallMethodV(ent, "StopAllEffects", "", nullptr);
        void** comps = *reinterpret_cast<void***>(e + 0xf0);  // the entity's components (CEntity::AttachToWorld walks them)
        uint32_t count = *reinterpret_cast<uint32_t*>(e + 0xf8);
        h->total = comps ? static_cast<int>(count) : -1;
        h->unasked = 0;
        for (uint32_t i = 0; comps && i < count && i < 512 && h->ncomps < kMaxComps; ++i) {
            void* c = comps[i];
            int32_t enabled = 0;
            if (c && !CallMethodV(c, "IsEnabled", "", &enabled)) ++h->unasked;
            if (!c || !(enabled & 0xff)) continue;
            CallMethodV(c, "SetEnabled", "b:0", nullptr);
            void* block = BlockOf(c);
            if (block) {
                InterlockedIncrement(reinterpret_cast<volatile LONG*>(block));
                h->comps[h->ncomps++] = block;
            }
        }
    } else {
        for (int i = 0; i < h->ncomps; ++i) {
            void* c = Alive(h->comps[i]);
            if (c) CallMethodV(c, "SetEnabled", "b:1", nullptr);
        }
        h->ncomps = 0;
        strncpy_s(list, h->fx, _TRUNCATE);
        char* ctx = nullptr;
        for (char* n = strtok_s(list, ",", &ctx); n; n = strtok_s(nullptr, ",", &ctx)) {
            _snprintf_s(spec, _TRUNCATE, "n:%s", n);
            CallMethodV(ent, "PlayEffect", spec, nullptr);
        }
        h->fx[0] = 0;
    }
}

bool g_arriving = false;  // Apply from the attach: an object coming into the world (a move is there at once)

int ApplyGuarded(void* ent, Act act, const char* name, const char* elseName, const float* pos, const float* rot,
                 bool on, Hidden* hidden) {
    char spec[80];
    int32_t r = 1;
    __try {
        switch (act) {
            case Act::Remove:
                HideGuarded(ent, on, hidden);
                break;
            case Act::Move:
                // the first move notes where it stood, so a fact that ends the edit puts it back while the game runs
                if (on && hidden && !hidden->homeSet) {
                    float v[4] = {0, 0, 0, 0}, a[4] = {0, 0, 0, 0};
                    if (CallMethodV(ent, "GetWorldPosition", "", v) && CallMethodV(ent, "GetWorldRotation", "", a)) {
                        memcpy(hidden->home, v, sizeof v);
                        memcpy(hidden->homeRot, a, sizeof a);
                        hidden->homeSet = true;
                    }
                }
                if (on) TeleportEntity(ent, pos, rot);
                else if (hidden && hidden->homeSet) {
                    TeleportEntity(ent, hidden->home, hidden->homeRot);
                    hidden->homeSet = false;
                }
                break;
            case Act::Effect: {
                _snprintf_s(spec, _TRUNCATE, "n:%s", name);
                r = 0;
                CallMethodV(ent, on ? "PlayEffect" : "StopEffect", spec, &r);  // false: the entity has no such effect
                break;
            }
            case Act::Light: {
                // (the game's SetLights: the entity's gameplay light; "on" / "off" while the edit holds, the other
                // way when it ends)
                bool want = strcmp(name, "off") != 0;
                if (!on) want = !want;
                void* handle = nullptr;
                CallMethodV(ent, "GetComponentByClassName", "n:CGameplayLightComponent", &handle);
                void* comp = handle ? ResolveHandle(handle) : nullptr;
                if (comp) CallMethodV(comp, "SetLight", want ? "b:1" : "b:0", nullptr);
                r = comp ? 1 : 0;  // 0: the entity has no gameplay light
                break;
            }
            case Act::Anim: {
                // (the object's own move, PlayPropertyAnimation(name, count, lengthScale, EPropertyCurveMode):
                // PCM_Forward 0, PCM_Backward 1, measured with w3.enum; count 0 again and again)
                bool loop = strcmp(elseName, "loop") == 0, back = strcmp(elseName, "back") == 0;
                if (loop && !on) {
                    _snprintf_s(spec, _TRUNCATE, "n:%s b:0", name);
                    r = CallMethodV(ent, "StopPropertyAnimation", spec, nullptr) ? 1 : 0;
                    break;
                }
                int dir = (back == on) ? 1 : 0;  // the edit ending: the other way, back to where it stood
                _snprintf_s(spec, _TRUNCATE, "n:%s i:%d f:%s e:%d", name, loop ? 0 : 1,
                            g_arriving && !loop ? "0.01" : "1", dir);
                r = CallMethodV(ent, "PlayPropertyAnimation", spec, nullptr) ? 1 : 0;
                break;
            }
            case Act::Tag:
                // (the tag manager learns it: theGame.GetEntityByTag finds the object, measured)
                _snprintf_s(spec, _TRUNCATE, "n:%s", name);
                CallMethodV(ent, on ? "AddTag" : "RemoveTag", spec, nullptr);
                break;
            case Act::Appearance: {
                const char* which = on ? name : elseName;
                if (!which || !*which) break;
                void* handle = nullptr;
                CallMethodV(ent, "GetComponentByClassName", "n:CAppearanceComponent", &handle);
                void* comp = handle ? ResolveHandle(handle) : nullptr;
                _snprintf_s(spec, _TRUNCATE, "s:%s", which);  // ApplyAppearance takes a string, not a name
                if (comp) CallMethodV(comp, "ApplyAppearance", spec, nullptr);
                r = comp ? 1 : 0;  // 0: the entity has no appearance component
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("worldedit: an edit faulted on ent=%p (swallowed)", ent);
        r = -1;
    }
    return r;
}

int FactGuarded(const char* spec);

void Apply(void* ent, const Entry& e, bool on, Hidden* hidden) {
    int r = ApplyGuarded(ent, e.act, e.name.c_str(), e.act == Act::Anim ? e.mode.c_str() : e.elseName.c_str(),
                         e.pos, e.rot, on, hidden);
    static const char* names[] = {"remove", "move", "effect", "appearance", "tag", "light", "anim"};
    char why[200] = "";  // a conditional one: the facts as they were read (what made it switch)
    if (!e.ifFact.empty())
        _snprintf_s(why, _TRUNCATE, " if %s=%d", e.ifFact.c_str(), FactGuarded(("s:" + e.ifFact).c_str()));
    if (!e.unlessFact.empty()) {
        size_t n = strlen(why);
        _snprintf_s(why + n, sizeof why - n, _TRUNCATE, " unless %s=%d", e.unlessFact.c_str(),
                    FactGuarded(("s:" + e.unlessFact).c_str()));
    }
    char comps[96] = "";  // a removal: what the hiding found
    if (e.act == Act::Remove && on && hidden)
        _snprintf_s(comps, _TRUNCATE, " - components: %d off of %d, %d not asked", hidden->ncomps, hidden->total,
                    hidden->unasked);
    Log("worldedit: %s %s%s%s on ent=%p (%s)%s%s", on ? "applied" : "took back", names[static_cast<int>(e.act)],
        e.name.empty() ? "" : " ", e.name.c_str(), ent, r > 0 ? "ok" : r == 0 ? "the entity has none" : "fault", why,
        comps);
}

int FactGuarded(const char* spec) {
    int32_t v = 0;
    __try {
        if (!CallFunctionV("FactsQuerySum", spec, nullptr, &v)) v = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0;
    }
    return v;
}

int Fact(const std::string& name, std::map<std::string, int>& cache) {
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    int v = FactGuarded(("s:" + name).c_str());
    cache[name] = v;
    return v;
}

bool Holds(const Entry& e, std::map<std::string, int>& cache) {
    if (!e.ifFact.empty() && Fact(e.ifFact, cache) <= 0) return false;
    if (!e.unlessFact.empty() && Fact(e.unlessFact, cache) > 0) return false;
    return true;
}

// Conditional edits on entities that are loaded: main thread only (the tick and the attach tasks run there).
struct Tracked {
    void* block;  // a reference is held on it: it stays valid, Alive() tells if the entity does
    void* ent;
    Entry entry;
    bool applied;
    Hidden hidden;  // remove: what was switched off, to switch on again
};
std::vector<Tracked> g_tracked;
float g_sincePoll = 0;

// Gives back a reference on a handle block, the way the engine does (0x0236e131 ff.): the last reference of a
// block whose object is gone frees the block. Only called when the object is gone (or for blocks of objects no
// handle owns, flag 2 clear), so the engine's "last handle of a handle-owned object destroys it" never applies.
void Release(void* block) {
    if (!block) return;
    if (InterlockedDecrement(reinterpret_cast<volatile LONG*>(block)) == 0 && !Alive(block)) {
        if (auto free = Fn<void (*)(void*)>("Memory::Free")) free(block);
    }
}

// --- every pre-placed entity while it is loaded: GUID -> handle blocks (a reference held on each). Lets an edit
// added while the game runs reach the object at once, and scripts find any object of the game by its GUID
// (TW3SE_EntityByGuid). Entities made at run time (counter GUIDs) and objects a handle owns (flag 2) are not kept.
struct GuidHash {
    size_t operator()(const Guid& g) const { return g.d[0] ^ (g.d[1] * 31u) ^ (g.d[2] * 131u) ^ (g.d[3] * 1031u); }
};
struct GuidEq {
    bool operator()(const Guid& a, const Guid& b) const { return GuidMatch(a, b.d); }
};
std::mutex g_liveMx;
std::unordered_map<Guid, std::vector<void*>, GuidHash, GuidEq> g_live;
size_t g_liveCount = 0;

bool Lasting(const uint32_t* k) { return (k[0] || k[1] || k[2] || k[3]) && !(k[0] == 0xffffffffu && k[1] == 0xffffffffu); }

// Why attaching entities were not kept (worldlist): the REDkit editor's register stayed empty.
volatile LONG g_attaches = 0, g_noBlock = 0, g_notLasting = 0, g_owned = 0;

void Remember(const uint32_t* key, void* block) {
    if (!Lasting(key)) {
        InterlockedIncrement(&g_notLasting);
        return;
    }
    if (*reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(block) + 4) & 2) {
        InterlockedIncrement(&g_owned);
        return;
    }
    Guid g{{key[0], key[1], key[2], key[3]}};
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(block));
    std::lock_guard<std::mutex> l(g_liveMx);
    g_live[g].push_back(block);
    ++g_liveCount;
}

// The loaded entity with this GUID (its handle block), or null. A dead one found on the way is let go.
void* FindLive(const Guid& g) {
    std::lock_guard<std::mutex> l(g_liveMx);
    auto it = g_live.find(g);
    if (it == g_live.end()) return nullptr;
    for (void* b : it->second)
        if (Alive(b)) return b;
    return nullptr;
}

// On every tick a slice of the registry: entities that are gone are let go. A whole pass at once took 9-11 ms over
// 109 000 objects of Novigrad (measured), a hitch every few seconds; a slice of the hash buckets per tick
// costs a fraction of a millisecond, and a pass goes round about once a second. A rehash meanwhile only moves the
// cursor's meaning (a bucket looked at twice or a round later): harmless.
size_t g_pruneCursor = 0;
constexpr size_t kPruneBuckets = 2048;

void PruneSome() {
    std::lock_guard<std::mutex> l(g_liveMx);
    size_t n = g_live.bucket_count();
    if (!n) return;
    std::vector<Guid> empty;
    for (size_t k = 0; k < kPruneBuckets && k < n; ++k) {
        size_t b = (g_pruneCursor + k) % n;
        for (auto it = g_live.begin(b); it != g_live.end(b); ++it) {
            auto& v = it->second;
            for (size_t i = 0; i < v.size();) {
                if (!Alive(v[i])) {
                    Release(v[i]);
                    v.erase(v.begin() + i);
                    --g_liveCount;
                } else {
                    ++i;
                }
            }
            if (v.empty()) empty.push_back(it->first);
        }
    }
    g_pruneCursor = (g_pruneCursor + kPruneBuckets) % n;
    for (const Guid& g : empty) g_live.erase(g);
}

// A matched entity, handed from the attach hook to the next tick.
struct Match {
    void* block;
    void* ent;
    Entry entry;
};

void MatchTask(void* p) {
    Match* m = static_cast<Match*>(p);
    bool keep = false;  // a tracked entry keeps the task's reference on the block
    if (Alive(m->block) == m->ent) {
        g_arriving = true;
        if (!m->entry.conditional()) {
            Hidden h{};
            Apply(m->ent, m->entry, true, &h);
        } else {
            keep = true;
            std::map<std::string, int> cache;
            bool holds = GetThePlayer() && Holds(m->entry, cache);
            g_tracked.push_back({m->block, m->ent, m->entry, false, {}});
            Tracked& t = g_tracked.back();
            if (holds) Apply(t.ent, t.entry, true, &t.hidden);
            t.applied = holds;
        }
        g_arriving = false;
    }
    if (!keep) Release(m->block);
    delete m;
}

void PollTick(float dt, void*) {
    PruneSome();
    if (g_tracked.empty()) return;
    g_sincePoll += dt;
    if (g_sincePoll < 0.25f || !GetThePlayer()) return;
    g_sincePoll = 0;
    std::map<std::string, int> cache;
    for (size_t i = 0; i < g_tracked.size();) {
        Tracked& t = g_tracked[i];
        if (Alive(t.block) != t.ent) {  // streamed out or destroyed: the next attach tracks it again
            Release(t.block);
            for (int k = 0; k < t.hidden.ncomps; ++k) Release(t.hidden.comps[k]);
            g_tracked.erase(g_tracked.begin() + i);
            continue;
        }
        bool holds = Holds(t.entry, cache);
        if (holds != t.applied) {
            Apply(t.ent, t.entry, holds, &t.hidden);
            t.applied = holds;
        }
        ++i;
    }
}

// An entity's class name via RTTI: GetObjectClass -> CClass+0x2c name id -> CName string.
const char* ClassName(void* ent) {
    void* cls = GetObjectClass(ent);
    if (!cls) return nullptr;
    uint32_t id = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(cls) + 0x2c);
    return GetCNameString(id);
}

// Reads the entity's key and class under SEH; false if it is not readable.
bool ReadKey(void* ent, uint32_t key[4], const char** cls, void** block) {
    __try {
        const uint32_t* c = reinterpret_cast<const uint32_t*>(reinterpret_cast<char*>(ent) + 0xcc);
        for (int i = 0; i < 4; ++i) key[i] = c[i];
        *cls = ClassName(ent);
        *block = BlockOf(ent);
        return *block != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

volatile LONG g_matched = 0;  // entities matched since start (worldlist)

void Queue(void* block, void* ent, const Entry& e) {
    InterlockedIncrement(&g_matched);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(block));  // keeps the block valid until the task is done
    RunOnMainThread(&MatchTask, new Match{block, ent, e});
}

void MatchEntity(void* ent) {
    uint32_t key[4];
    const char* cls = nullptr;
    void* block = nullptr;
    InterlockedIncrement(&g_attaches);
    if (!ReadKey(ent, key, &cls, &block)) {
        InterlockedIncrement(&g_noBlock);
        return;
    }
    Remember(key, block);
    std::lock_guard<std::mutex> l(g_mx);
    if (cls)
        for (const std::string& rc : g_removedClasses)
            if (rc == cls) {
                Entry e{};
                e.act = Act::Remove;
                Queue(block, ent, e);
                return;
            }
    for (const Entry& e : g_entries)
        if (GuidMatch(e.g, key)) Queue(block, ent, e);
}

// Discovery log (config worldedit_log = on): class and GUID of each attaching entity, so an author can read a
// GUID from the log. Rate-limited; off by default. Positions stream in near the player, so the lines around a
// fresh load are the nearby objects.
void LogAttach(void* ent, void* world) {
    if (g_logged >= 400) return;
    __try {
        const char* cn = ClassName(ent);
        const uint32_t* e = reinterpret_cast<const uint32_t*>(reinterpret_cast<char*>(ent) + 0xcc);
        Log("worldedit/attach: %-26s %08x %08x %08x %08x world %p", cn ? cn : "?", e[0], e[1], e[2], e[3], world);
        ++g_logged;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

using AttachToWorld_t = uintptr_t (*)(void* ent, void* world);
AttachToWorld_t oAttach = nullptr;

void* volatile g_world = nullptr;  // the world the last entity attached to (the loaded one)

uintptr_t AttachHook(void* ent, void* world) {
    // only a real object: in the REDkit editor the second parameter was 0xC0 on every call, not the world
    if (reinterpret_cast<uintptr_t>(world) > 0x10000) g_world = world;
    uintptr_t r = oAttach(ent, world);
    if (ent) {
        if (g_log) LogAttach(ent, world);
        MatchEntity(ent);  // remembers it (the registry) and queues its edits
    }
    return r;
}

// Reading the files again on every game-loaded event picks up a DLC installed or loaded meanwhile. Measured
// (tools/worldreload_test.py): a save loaded in the running game fires OnGameStart before the world's objects
// attach, so they find the new edits; on a cold start the layers attach before RestoreSession, their matches are
// queued or tracked already and stay (an entity of the session before is dropped by its dead handle block).
void OnLoadedReload(void*) {
    g_logged = 0;
    LoadEdits();
}

// --- console

// worldedit <line>: one world.txt line, appended to tw3se\world.txt and active from the next load.
// One edit while the game runs: kept for the session and applied at once to the object if it is loaded (its next
// tick). `persist`: also appended to tw3se\\world.txt, so it holds after a load. Returns whether the line was read;
// *live tells whether the object was loaded.
bool AddEdit(const std::string& line, bool persist, bool* live) {
    Entry e{};
    std::string cls;
    *live = false;
    if (!ParseLine(line, e, cls)) return false;
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (!cls.empty()) g_removedClasses.push_back(cls);
        else g_entries.push_back(e);
    }
    if (cls.empty()) {
        if (void* block = FindLive(e.g)) {
            Queue(block, Alive(block), e);
            *live = true;
        }
    }
    if (persist) {
        std::ofstream f((Dir() + L"\\world.txt").c_str(), std::ios::app);
        f << line << "\n";
    }
    return true;
}

// --- natives

// import function TW3SE_EntityByGuid(guid : string) : CEntity;  -> the loaded object with this GUID, or NULL
// A handle return: the block in the result with one reference more, as the engine's own natives return one.
void Tw3EntityByGuid(void* /*self*/, void* frame, void* result) {
    uint8_t buf[16] = {0};
    ReadScriptParam(frame, buf);
    EndScriptParams(frame);
    const char* text = *reinterpret_cast<char**>(buf);
    Guid g{};
    void* block = nullptr;
    if (text && sscanf_s(text, "%x %x %x %x", &g.d[0], &g.d[1], &g.d[2], &g.d[3]) == 4 && !GuidZero(g))
        block = FindLive(g);
    if (!result) return;
    if (block) InterlockedIncrement(reinterpret_cast<volatile LONG*>(block));
    *reinterpret_cast<void**>(result) = block;
}

// import function TW3SE_WorldEdit(line : string) : bool;  -> one world.txt line for this session, applied at once
// to the object if it is loaded. Not written to a file: what has to hold after a load goes into the mod's
// tw3se\world.txt (with `if <fact>` for what depends on the story - facts are kept in the save).
// import function TW3SE_HideEntity(e : CEntity, hide : bool) : bool;
// The same hiding a `remove` edit does, from a script: hidden with its running effects stopped and its components
// switched off; `hide` false brings back exactly those (a lamp's flame too, which its own GetAutoEffect does not
// give). Returns whether the entity was there. For the session; what has to hold goes into world.txt.
std::map<void*, Hidden> g_scriptHidden;  // handle block -> what it had (main thread: natives run there)

void Tw3HideEntity(void* /*self*/, void* frame, void* result) {
    uint8_t ebuf[16] = {0}, bbuf[16] = {0};
    ReadScriptParam(frame, ebuf);
    ReadScriptParam(frame, bbuf);
    EndScriptParams(frame);
    void* ent = oAttach ? ResolveHandle(*reinterpret_cast<void**>(ebuf)) : nullptr;
    bool hide = bbuf[0] != 0;
    void* block = ent ? BlockOf(ent) : nullptr;
    bool ok = block && Alive(block) == ent;
    if (ok) {
        auto it = g_scriptHidden.find(block);
        if (hide && it == g_scriptHidden.end()) {
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(block));
            Hidden& h = g_scriptHidden[block];
            h = Hidden{};
            ApplyGuarded(ent, Act::Remove, "", "", nullptr, nullptr, true, &h);
        } else if (!hide && it != g_scriptHidden.end()) {
            ApplyGuarded(ent, Act::Remove, "", "", nullptr, nullptr, false, &it->second);
            Release(block);
            g_scriptHidden.erase(it);
        }
    }
    if (result) *reinterpret_cast<bool*>(result) = ok;
}

void Tw3WorldEdit(void* /*self*/, void* frame, void* result) {
    uint8_t buf[16] = {0};
    ReadScriptParam(frame, buf);
    EndScriptParams(frame);
    const char* text = *reinterpret_cast<char**>(buf);
    bool live = false;
    bool ok = oAttach && text && AddEdit(text, false, &live);  // no attach hook (REDkit editor): no world edits
    if (result) *reinterpret_cast<bool*>(result) = ok;
}

void AddLine(const std::string& line, char* reply, uint32_t size) {
    bool live = false;
    if (!AddEdit(line, true, &live)) {
        _snprintf_s(reply, size, _TRUNCATE,
                    "error not an edit: '%s' (remove|move|effect|appearance <d0> <d1> <d2> <d3> ... [if f] [unless f])",
                    line.c_str());
        return;
    }
    _snprintf_s(reply, size, _TRUNCATE, "ok added to tw3se\\world.txt; %s",
                live ? "the object is loaded: applied now" : "the object is not loaded: it applies when it comes");
}

void CmdWorldEdit(const char* arg, char* reply, uint32_t size) { AddLine(arg ? arg : "", reply, size); }
void CmdWorldRemove(const char* arg, char* reply, uint32_t size) { AddLine(std::string("remove ") + (arg ? arg : ""), reply, size); }
void CmdWorldMove(const char* arg, char* reply, uint32_t size) { AddLine(std::string("move ") + (arg ? arg : ""), reply, size); }
void CmdWorldEffect(const char* arg, char* reply, uint32_t size) { AddLine(std::string("effect ") + (arg ? arg : ""), reply, size); }
void CmdWorldAppearance(const char* arg, char* reply, uint32_t size) {
    AddLine(std::string("appearance ") + (arg ? arg : ""), reply, size);
}

void CmdWorldList(const char*, char* reply, uint32_t size) {
    size_t n, c, cond = 0;
    {
        std::lock_guard<std::mutex> l(g_mx);
        n = g_entries.size();
        c = g_removedClasses.size();
        for (const Entry& e : g_entries) cond += e.conditional() ? 1 : 0;
    }
    size_t applied = 0, live = 0;
    for (const Tracked& t : g_tracked) applied += t.applied ? 1 : 0;
    {
        std::lock_guard<std::mutex> l(g_liveMx);
        live = g_live.size();
    }
    _snprintf_s(reply, size, _TRUNCATE,
                "%zu edit(s), %zu conditional, %zu removed class(es); %ld matched since start, %zu tracked now, "
                "%zu applied; %zu objects of the game loaded; attaches %ld (no handle block %ld, run-time GUID %ld, "
                "handle-owned %ld)",
                n, cond, c, static_cast<long>(g_matched), g_tracked.size(), applied, live, static_cast<long>(g_attaches),
                static_cast<long>(g_noBlock), static_cast<long>(g_notLasting), static_cast<long>(g_owned));
}

// worldfind <d0> <d1> <d2> <d3>: is the object with this GUID loaded, what is it, where
void CmdWorldFind(const char* arg, char* reply, uint32_t size) {
    Guid g{};
    if (!arg || sscanf_s(arg, "%x %x %x %x", &g.d[0], &g.d[1], &g.d[2], &g.d[3]) != 4 || GuidZero(g)) {
        strncpy_s(reply, size, "usage: worldfind <d0> <d1> <d2> <d3>", _TRUNCATE);
        return;
    }
    void* block = FindLive(g);
    void* ent = block ? Alive(block) : nullptr;
    if (!ent) {
        strncpy_s(reply, size, "not loaded", _TRUNCATE);
        return;
    }
    const char* cls = ClassName(ent);
    char fx[512];
    if (!EntityEffectNames(ent, fx, sizeof fx)) fx[0] = 0;
    _snprintf_s(reply, size, _TRUNCATE, "loaded: %s at %p; effects: %s", cls ? cls : "?", ent, fx[0] ? fx : "none");
}

// Teleport the player (and read its world position back from the engine). No args: report the position. Uses the
// same vtable slot 0x460 the world move uses, on an entity that always exists.
void CmdPlayerTp(const char* arg, char* reply, uint32_t size) {
    void* pl = GetThePlayer();
    if (!pl) {
        strncpy_s(reply, size, "no player (menu or no spawn)", _TRUNCATE);
        return;
    }
    float pos[3] = {0, 0, 0};
    WorldPosGuarded(pl, pos);
    float tgt[3], rot[3] = {0, 0, 0};
    if (arg && sscanf_s(arg, "%f %f %f", &tgt[0], &tgt[1], &tgt[2]) == 3) {
        // the engine takes the new position over on the next frame: the reply says where the player was
        ApplyGuarded(pl, Act::Move, "", "", tgt, rot, true, nullptr);
        _snprintf_s(reply, size, _TRUNCATE, "ok teleported to (%.2f %.2f %.2f), was at (%.2f %.2f %.2f)", tgt[0],
                    tgt[1], tgt[2], pos[0], pos[1], pos[2]);
        return;
    }
    _snprintf_s(reply, size, _TRUNCATE, "player at (%.2f %.2f %.2f)", pos[0], pos[1], pos[2]);
}

}  // namespace

void* CurrentWorld() { return g_world; }

void WorldForEachEntity(void (*visit)(void* entity, void* block, void* ctx), void* ctx) {
    if (!visit) return;
    std::vector<std::pair<void*, void*>> snapshot;  // (entity, block), taken under the lock
    {
        std::lock_guard<std::mutex> l(g_liveMx);
        snapshot.reserve(g_liveCount);
        for (auto& [g, blocks] : g_live)
            for (void* b : blocks) {
                void* e = Alive(b);
                if (e) snapshot.emplace_back(e, b);
            }
    }
    for (auto& [e, b] : snapshot) visit(e, b, ctx);
}

void* WorldEntityByGuid(const uint32_t guid[4]) {
    if (!guid) return nullptr;
    Guid g{{guid[0], guid[1], guid[2], guid[3]}};
    void* block = GuidZero(g) ? nullptr : FindLive(g);
    return block ? Alive(block) : nullptr;
}

bool WorldEntityGuid(void* entity, uint32_t out[4]) {
    uint32_t key[4];
    const char* cls = nullptr;
    void* block = nullptr;
    if (!entity || !out || !ReadKey(entity, key, &cls, &block) || !Lasting(key)) return false;
    for (int i = 0; i < 4; ++i) out[i] = key[i];
    return true;
}

bool WorldEditLine(const char* line) {
    bool live = false;
    return line && AddEdit(line, false, &live);
}

void QueueWorldEditNatives() {
    QueueNative("TW3SE_EntityByGuid", &Tw3EntityByGuid, 1);
    QueueNative("TW3SE_WorldEdit", &Tw3WorldEdit, 1);
    QueueNative("TW3SE_HideEntity", &Tw3HideEntity, 2);
}

void InstallWorldEdit() {
    QueueWorldEditNatives();
    HookNamed("CEntity::AttachToWorld", reinterpret_cast<void*>(&AttachHook), reinterpret_cast<void**>(&oAttach));
    if (!oAttach) {
        Log("worldedit: attach hook unavailable - world edits off");
        return;
    }
    g_log = ConfigBool("worldedit_log", false);
    LoadEdits();
    RegisterOnGameLoaded(&OnLoadedReload, nullptr);
    RunOnMainThread([](void*) { RegisterTick(&PollTick, nullptr); }, nullptr);
    RegisterCommand("worldedit", &CmdWorldEdit,
                    "worldedit <line>: add a world edit (remove|move|effect|appearance <guid> ... [if f] [unless f])");
    RegisterCommand("worldlist", &CmdWorldList, "worldlist: how many world edits are loaded and applied");
    RegisterCommand("worldfind", &CmdWorldFind, "worldfind <d0> <d1> <d2> <d3>: is this object loaded, what is it");
    RegisterCommand("worldremove", &CmdWorldRemove, "worldremove <d0> <d1> <d2> <d3>: = worldedit remove ...");
    RegisterCommand("worldmove", &CmdWorldMove, "worldmove <d0> <d1> <d2> <d3> <x> <y> <z> <pitch> <yaw> <roll>");
    RegisterCommand("worldeffect", &CmdWorldEffect, "worldeffect <d0> <d1> <d2> <d3> <effectName>");
    RegisterCommand("worldappearance", &CmdWorldAppearance, "worldappearance <d0> <d1> <d2> <d3> <appearanceName>");
    RegisterCommand("playertp", &CmdPlayerTp, "playertp [x y z]: teleport the player (no args: report position)");
    Log("worldedit: attach hook installed");
}

}  // namespace tw3se
