// Pose and mimic hooks: by Tenour.
// Pose and mimic hooks (plugin interface v14): live skeleton and face control for plugins. A plugin registers a
// pose/mimic callback and turns it on per entity; the callback fires each frame after the engine evaluates the
// pose and before skinning, with writable local bone transforms (and, for the head, the mixed track weights).
//
// The engine point is the per-frame "local 4x4 -> model-space" combine (docs/FINDINGS_POSE.md, addenda
// 6 and 7): once per animated component the engine walks the bones, multiplies each bone's local 4x4 matrix
// (descriptor A+0x10, stride 0x40, A+0x08 = bone count) by its parent's model-space matrix (*(B) =
// component+0xddc) and writes the result to A+0x20 (A+0x28 = a format flag). Two near-identical variants
// (Combine_Format1/2) are called by a dispatcher (vtable slot 0x58 on the sub-object component+0x120). We hook
// both combine variants: at entry (before the original) a plugin may rewrite the local matrices a watched
// component is about to be skinned from. Config `posehook`: `auto` (default) installs the engine hook when a plugin
// registers a pose callback, so a game without such a plugin is untouched; `on` installs it at start; `diag` also
// logs the A layout of the first calls; `selfcb` adds a built-in test callback; `off` never installs it.
#include "core.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>

#include <windows.h>

namespace tw3se {
namespace {

// The animated component's pose fields. In the REDkit editor the multiple-inheritance group with the pose
// dispatcher sits 0x10 further back (component+0x130 instead of +0x120: one more interface before it), and every
// field this file reads is reached through it: +0xdec, +0xe10, +0xe18 instead of +0xddc, +0xe00, +0xe08 (eb,
// from the type information in editor.exe and the retail/editor dumps: 94 bones at +0xe18). Not a FIELD row: the
// shift is compiler layout, no single instruction carries it.
uint32_t PoseOff(uint32_t retail) { return IsRedkitEditor() ? retail + 0x10 : retail; }
// The pose and mimic callbacks of all plugins (a list: a second plugin's RegisterOnPose keeps the first one's, and
// a combine thread sees ctx and callback together). A slot's ctx is
// written before its callback; the animation threads read the callback first.
template <class Fn>
struct CbList {
    static constexpr int kSlots = 16;
    struct Slot {
        std::atomic<Fn> fn{nullptr};
        std::atomic<void*> ctx{nullptr};
    } slot[kSlots];
    std::atomic<int> used{0};  // slots 0..used-1 have been handed out
    std::mutex mx;             // registration (main thread); the readers take none
    bool Any() const {
        for (int i = 0, n = used.load(std::memory_order_acquire); i < n; ++i)
            if (slot[i].fn.load(std::memory_order_relaxed)) return true;
        return false;
    }
    bool Add(Fn f, void* c) {
        std::lock_guard<std::mutex> l(mx);
        int n = used.load(), freeSlot = -1;
        for (int i = 0; i < n; ++i) {
            Fn have = slot[i].fn.load();
            if (have == f) {  // the same function again: its new context
                slot[i].ctx.store(c);
                return true;
            }
            if (!have && freeSlot < 0) freeSlot = i;
        }
        if (freeSlot < 0) {
            if (n >= kSlots) return false;
            freeSlot = n;
        }
        slot[freeSlot].ctx.store(c, std::memory_order_relaxed);
        slot[freeSlot].fn.store(f, std::memory_order_release);
        if (freeSlot == n) used.store(n + 1, std::memory_order_release);
        return true;
    }
    bool Remove(Fn f) {  // a callback that crashed
        for (int i = 0, n = used.load(); i < n; ++i) {
            Fn same = f;
            if (slot[i].fn.compare_exchange_strong(same, nullptr)) return true;
        }
        return false;
    }
    int RemoveIn(uintptr_t lo, uintptr_t hi) {  // the callbacks whose code lies in [lo, hi): a plugin's
        int k = 0;
        for (int i = 0, n = used.load(); i < n; ++i) {
            auto p = reinterpret_cast<uintptr_t>(slot[i].fn.load());
            if (p && p >= lo && p < hi) slot[i].fn.store(nullptr), ++k;
        }
        return k;
    }
    int Count() const {
        int k = 0;
        for (int i = 0, n = used.load(); i < n; ++i) k += slot[i].fn.load() != nullptr;
        return k;
    }
};
CbList<TW3SE_PoseCallback> g_poseCbs;
CbList<TW3SE_MimicCallback> g_mimicCbs;
std::atomic<int> g_cbInFlight{0};  // pose and mimic callbacks running right now (a plugin is unloaded after 0)
// A watched object: an entity (all its components' meshes fire) or one animated component. Its entity and GUID are
// read once on the main thread (WatchPose), so the combine thread only copies them.
struct WatchInfo {
    void* entity;
    uint32_t guid[4];
    void* body;  // the entity's root animated component (GetRootAnimatedComponent at watch time), or null
    int by;      // who watches (PoseWatchBy bits: 1 the plugin API and w3.pose_watch, 2 w3.pose); gone at 0
    bool items;  // a figure: the items hung on it count as its own (not for a component or an item watched itself)
};
std::unordered_map<void*, WatchInfo> g_poseWatch;  // entity or component -> what the callback reports
std::unordered_map<void*, WatchInfo> g_mimicWatch;  // entity or CMimicComponent -> what the mimic callback reports
std::atomic<int> g_mimicWatchCount{0};
std::mutex g_watchMx;                     // guards g_poseWatch (written on main thread, read on the combine thread)
std::atomic<int> g_watchCount{0};         // entries in g_poseWatch (the combine hook's fast out)
// Watches that take a figure's items with them. The item search runs only while there is one: a plugin that watches
// the head item or its CMimicComponent itself costs no search at all (a search for every watch of the player
// stalls the game every 5 s).
std::atomic<int> g_itemWatchers{0};
void CountItemWatchers() {  // under g_watchMx
    int n = 0;
    for (const auto& w : g_poseWatch) n += w.second.items;
    for (const auto& w : g_mimicWatch) n += w.second.items;
    g_itemWatchers.store(n, std::memory_order_relaxed);
}
bool g_hookAllowed = false;  // set by InstallPoseHook (config not `off`, the feature's addresses present)
bool g_hookInstalled = false;
std::mutex g_installMx;

// --- the engine combine hook ---------------------------------------------------------------------------------
// The two combine variants are plain __fastcall engine functions; a 4-argument register pass-through matches
// them (rcx = descriptor A, rdx = B, ... all in registers), the same shape the lifecycle hooks use here.
using CombineFn = void* (*)(void*, void*, void*, void*);
CombineFn oCombine1 = nullptr;
CombineFn oCombine2 = nullptr;
std::atomic<int> g_diagLeft{0};  // how many more combine calls to log the A layout for (armed by posehook=diag)
// Dev self-test / mapping. A combine call is identified by its entity: component = B - 0xddc (B is the second
// arg; FINDINGS_POSE.md addendum 7, confirmed live). g_twistComp selects which component to twist (nullptr = off,
// (void*)1 = broad: every humanoid count>=60); g_twistBone is the bone index (in COMBINE order) to twist.
// g_mapTarget arms a one-shot dump of the candidate remap array at A+0x18 for that component, to recover the
// combine-index -> skeleton-bone-index mapping (and so the bone names in combine order).
std::atomic<void*> g_twistComp{nullptr};
std::atomic<int> g_twistBone{-1};
std::atomic<int> g_twistSigIdx{-1};  // if >=0, twist only the submesh whose PAR[sigIdx]==sigVal (one mesh)
std::atomic<int> g_twistSigVal{0};
std::atomic<void*> g_mapTarget{nullptr};
std::atomic<int> g_mapDone{0};
// Mesh enumerator (dev): list every distinct combine of a component (each rendered mesh has its own combine with
// its own bone order). Keyed by the 3rd arg C (the mesh/skin descriptor, stable per mesh). Logs each once.
std::atomic<void*> g_meshListComp{nullptr};
std::atomic<int> g_meshListClear{0};
std::unordered_set<void*> g_seenMesh;
std::mutex g_meshMx;

struct ADump {
    int32_t count;
    void* local;
    void* out;
    int32_t flag;
    float m0[16];  // bone 0 local 4x4
    float m1[16];  // bone 1 local 4x4
};

// Read the descriptor A fields guarded (the combine runs on the animation thread; a bad pointer must not crash
// the game). No C++ objects here, so the __except unwinds cleanly.
bool ReadA(void* A, ADump* d) {
    __try {
        char* a = static_cast<char*>(A);
        d->count = *reinterpret_cast<int32_t*>(a + 0x08);
        d->local = *reinterpret_cast<void**>(a + 0x10);
        d->out = *reinterpret_cast<void**>(a + 0x20);
        d->flag = *reinterpret_cast<int32_t*>(a + 0x28);
        float* m = reinterpret_cast<float*>(d->local);
        for (int i = 0; i < 16; ++i) {
            d->m0[i] = m ? m[i] : 0.0f;
            d->m1[i] = m ? m[16 + i] : 0.0f;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void DiagOnce(const char* which, void* A) {
    if (g_diagLeft.load(std::memory_order_relaxed) <= 0) return;
    if (g_diagLeft.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
    ADump d;
    if (!ReadA(A, &d)) {
        Log("posehook/diag %s: A=%p read faulted", which, A);
        return;
    }
    Log("posehook/diag %s: A=%p count=%d local=%p out=%p flag=%d", which, A, d.count, d.local, d.out, d.flag);
    // row0 shows the rotation's first row; row3 (floats 12..15) shows the translation if this is DX row-vector.
    Log("  bone0 row0[%.3f %.3f %.3f %.3f] row3[%.3f %.3f %.3f %.3f]", d.m0[0], d.m0[1], d.m0[2], d.m0[3], d.m0[12],
        d.m0[13], d.m0[14], d.m0[15]);
    Log("  bone1 row0[%.3f %.3f %.3f %.3f] row3[%.3f %.3f %.3f %.3f]", d.m1[0], d.m1[1], d.m1[2], d.m1[3], d.m1[12],
        d.m1[13], d.m1[14], d.m1[15]);
}

// A pointer that can be one into the game's memory: in user space, not odd (the pose hook's checks, without a system
// call: they run in every combine)
bool InUserSpace(const void* p, size_t n) {
    auto a = reinterpret_cast<uintptr_t>(p);
    return a >= 0x10000 && a + n > a && a + n <= 0x7ffffffeffffULL && !(a & 1);
}

// Guarded raw copies (the combine runs on a worker thread; a bad pointer must not crash the game).
bool GRead(const void* src, void* dst, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool GWrite(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// One-shot mapping dump for a target component's body combine. The combine OUTPUT (A+0x20) holds `count`
// model-space matrices (the skinned subset); component+0xe00 holds the full skeleton's model-space matrices
// (all bones, query buffer). Both are the same pose this frame, so matching each output slot's translation
// (M[12..14]) to the nearest +0xe00 bone gives combine-slot -> skeleton-bone-index (addendum 8).
void MapDump(const char* which, void* A, void* comp, int count) {
    void* out = nullptr;
    void* e00 = nullptr;
    int32_t ecount = 0;
    GRead(static_cast<char*>(A) + 0x20, &out, 8);
    GRead(static_cast<char*>(comp) + PoseOff(0xddc), &e00, 8);   // MODEL-space parent buffer (same space as out!), not +0xe00
    GRead(static_cast<char*>(comp) + PoseOff(0xe08), &ecount, 4);
    int n = count;
    if (n < 0) n = 0;
    if (n > 256) n = 256;
    if (ecount < 0 || ecount > 512) ecount = 0;
    static char outbuf[256 * 64];
    static char e00buf[512 * 64];
    bool ok = out && e00 && ecount > 0 && GRead(out, outbuf, static_cast<size_t>(n) * 64) &&
              GRead(e00, e00buf, static_cast<size_t>(ecount) * 64);
    Log("posehook/map %s: comp=%p count=%d out=%p e00=%p ecount=%d read=%d", which, comp, count, out, e00, ecount,
        ok ? 1 : 0);
    if (!ok) return;
    // Dump raw translations in chunks for a tool to register offline: out[n] (entity-model-space) and
    // e00[ecount] (world-space) are the same pose under one rigid transform, so ICP/Kabsch recovers the exact
    // combine-slot -> skeleton-bone map (no geometric ambiguity, zero residual at convergence).
    auto dumpTrans = [&](const char* tag, const char* src, int cnt) {
        std::string s;
        char tmp[48];
        int per = 0, chunk = 0;
        for (int i = 0; i < cnt; ++i) {
            const float* t = reinterpret_cast<const float*>(src + i * 64) + 12;
            snprintf(tmp, sizeof tmp, "%.4f,%.4f,%.4f ", t[0], t[1], t[2]);
            s += tmp;
            if (++per >= 20) {
                Log("posehook/map %s %s[%d]: %s", which, tag, chunk++, s.c_str());
                s.clear();
                per = 0;
            }
        }
        if (per) Log("posehook/map %s %s[%d]: %s", which, tag, chunk, s.c_str());
    };
    dumpTrans("OUT", outbuf, n);
    dumpTrans("E00", e00buf, ecount);
    // local (A+0x10) row3 per slot = offset from parent (bind bone length, pose-independent): for tree+length match
    void* localp = nullptr;
    if (GRead(static_cast<char*>(A) + 0x10, &localp, 8) && localp) {
        static char lbuf[256 * 64];
        if (GRead(localp, lbuf, static_cast<size_t>(n) * 64)) dumpTrans("LOC", lbuf, n);
    }
    // combine-local parent table (addendum 9: A+0x00 int16, combine-local indices). Dump both interpretations:
    // A+0x00 as a pointer to the table, and A+0x00 inline, so a tool can tell which and match the tree topology
    // against the skeleton's reflected parentIndices.
    void* ptbl = nullptr;
    GRead(static_cast<char*>(A) + 0x00, &ptbl, 8);
    static int16_t par[256];
    bool pok = ptbl && GRead(ptbl, par, static_cast<size_t>(n) * 2);
    Log("posehook/map %s: parentTblPtr=%p read=%d", which, ptbl, pok ? 1 : 0);
    if (pok) {
        std::string s;
        char tmp[12];
        for (int i = 0; i < n; ++i) {
            snprintf(tmp, sizeof tmp, "%d ", par[i]);
            s += tmp;
        }
        Log("posehook/map %s PAR: %s", which, s.c_str());
    }
}

// --- GetPose: the whole skeleton in model space (for solvers over two figures) -------------------------
// *(component+0xddc) holds this frame's model-space 4x4 of every skeleton bone: the parent buffer every mesh's
// combine reads (Geralt's pelvis at z 0.93 m in it, so model space); component+0xe08 the bone count
// (94 = CSkeleton.bones on Geralt). The buffer is in runtime order: bones 0..lodBoneNum_1-1 (40 on Geralt, always
// resident) match CSkeleton.bones/parentIndices, the streamed bones after them are sorted differently at run time
// (FINDINGS_POSE.md, addenda 14 and 15). So names and parents come from the CSkeleton for those first bones only;
// the rest get "" and -1. RTTI offsets are found once on the main thread, everything after that is a guarded read,
// so GetPose works from a pose callback (the animation thread) too.
struct SkelOffsets {
    uint32_t skeleton = 0, bones = 0, parents = 0, boneName = 0, boneSize = 0, resident = 0;
    bool ok = false;
};
SkelOffsets g_so;

// --- items hung on a figure. Geralt's head is an item entity of its own (with its own CMimicComponent), so its
// components belong to the item, not to Geralt. A watched figure takes its items with it: the animation thread only
// looks the component's entity up in g_itemParent and notes an unknown one; the main thread asks the engine
// (CItemEntity.GetParentEntity) and fills the table. Nothing on the animation thread calls into an object it only
// read from memory (a GetClass call on what CNode.transformParent points at hangs the game in a job wait right after
// the first WatchPose).
// Entities come and go, and an address can belong to a new one later, so an answer is looked at again after a few
// seconds. Each entry on its own time: the table used to be emptied every 5 s as a whole, and every figure in view
// was asked about again in one tick - a stall of up to 64 ms on 1.2.0 every 5 s, and the watched head item without its
// callback for a frame each time. Meanwhile the old answer stands.
struct ItemEntry {
    void* parent;       // the figure it hangs on (null: none)
    ULONGLONG recheck;  // looked at again from here on, while still seen
    ULONGLONG seen;     // last asked about by an animation thread (an entry nobody asks about goes after 30 s)
};
std::unordered_map<void*, ItemEntry> g_itemParent;  // entity -> its entry, under g_watchMx
std::unordered_set<void*> g_itemAsk;                // entities the main thread still has to look at, under g_watchMx
// config posehook_items / command `poseitems`: 0 off (a watch covers the entity and its own components only), 1 the
// figure is read from memory, 2 the engine is asked (CItemEntity.GetParentEntity; kept to measure the hang)
std::atomic<int> g_itemsMode{1};
struct ItemStats {  // for the `poseitems` command, under g_watchMx
    uint64_t looked = 0;     // entities looked at
    double longestMs = 0;    // the longest tick spent on them
} g_itemStats;
bool DerivesFromGuarded(void* obj, const char* name);

// The watch entry of a component: the component itself, its entity, or the figure its item hangs on. Under g_watchMx.
const WatchInfo* FindWatch(std::unordered_map<void*, WatchInfo>& map, void* comp, void* owner) {
    auto it = map.find(comp);
    if (it == map.end() && owner) it = map.find(owner);
    if (it != map.end()) return &it->second;
    // only while a figure is watched (with a plugin's pose callback the combine runs this for every figure, also
    // while a save loads; asking the engine about each item then hangs the game in the load)
    if (!g_itemsMode.load(std::memory_order_relaxed) || !owner || g_itemWatchers.load(std::memory_order_relaxed) == 0)
        return nullptr;
    auto p = g_itemParent.find(owner);
    if (p == g_itemParent.end()) {
        if (g_itemAsk.size() < 1024 && g_itemAsk.insert(owner).second) Crumb("items: noted", reinterpret_cast<uintptr_t>(owner));
        return nullptr;
    }
    ULONGLONG now = GetTickCount64();
    p->second.seen = now;
    if (now >= p->second.recheck && g_itemAsk.size() < 1024) g_itemAsk.insert(owner);
    if (!p->second.parent) return nullptr;
    auto w = map.find(p->second.parent);
    return w != map.end() && w->second.items ? &w->second : nullptr;
}

struct HangOffsets {  // CNode.transformParent and IAttachment.parent, from the RTTI (main thread)
    uint32_t nodeParent = 0, attParent = 0;
    bool ok = false;
} g_ho;

void FindHangOffsets() {
    if (g_ho.ok) return;
    void* node = FindClassByName("CNode");
    void* att = FindClassByName("IAttachment");
    void* pTp = node ? FindProperty(node, "transformParent") : nullptr;
    void* pPar = att ? FindProperty(att, "parent") : nullptr;
    if (!pTp || !pPar) return;
    GRead(static_cast<char*>(pTp) + 0x20, &g_ho.nodeParent, 4);
    GRead(static_cast<char*>(pPar) + 0x20, &g_ho.attParent, 4);
    g_ho.ok = g_ho.nodeParent && g_ho.attParent;
}

// A live scriptable: its handle block (object+0x08) points back at it. Asks MemReadable before each read, so a
// garbage pointer (the animation thread notes whatever sits at component+0x30) costs no fault at all.
bool IsScriptable(void* o) {
    void* b = nullptr;
    void* back = nullptr;
    return o && MemReadable(static_cast<char*>(o) + 0x08, 8) && GRead(static_cast<char*>(o) + 0x08, &b, 8) && b &&
           MemReadable(static_cast<char*>(b) + 0x08, 8) && GRead(static_cast<char*>(b) + 0x08, &back, 8) && back == o;
}

// At most this many entities a tick, and no more once a millisecond is spent: what is left waits for the next tick.
// Right after a save loads every figure in view is noted at once.
constexpr size_t kItemsPerTick = 32;
constexpr double kItemsBudgetMs = 1.0;

void OnItemsTick(float, void*) {  // main thread: the entities the animation thread noted
    static ULONGLONG nextPurge = 0;
    ULONGLONG now = GetTickCount64();
    std::vector<void*> ask;
    {
        std::lock_guard<std::mutex> lk(g_watchMx);
        if (now >= nextPurge) {  // entries nobody asked about for 30 s: gone figures, items put away
            for (auto it = g_itemParent.begin(); it != g_itemParent.end();)
                it = now - it->second.seen > 30000 ? g_itemParent.erase(it) : std::next(it);
            nextPurge = now + 10000;
        }
        for (auto it = g_itemAsk.begin(); it != g_itemAsk.end() && ask.size() < kItemsPerTick;) {
            ask.push_back(*it);
            it = g_itemAsk.erase(it);
        }
    }
    if (ask.empty()) return;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    double spentMs = 0;
    std::vector<std::pair<void*, void*>> found;
    std::vector<void*> later;
    static int logged = 0;
    static uint32_t spread = 0x9e3779b9u;  // a few seconds' spread of the next look, so the entries do not come due together
    // Read, no engine call (asking the engine, CItemEntity.GetParentEntity, is followed by a hang in the next tick,
    // the workers frozen, the main thread in a spin wait). An item's transform parent is an
    // attachment whose parent is a component of the figure; every object on the way is checked to be live first.
    FindHangOffsets();
    const bool engine = g_itemsMode.load(std::memory_order_relaxed) == 2;
    for (void* e : ask) {
        if (spentMs >= kItemsBudgetMs) {
            later.push_back(e);
            continue;
        }
        void* parent = nullptr;
        if (engine && IsScriptable(e) && DerivesFromGuarded(e, "CItemEntity")) {
            alignas(16) uint8_t res[32] = {};
            Crumb("items: GetParentEntity", reinterpret_cast<uintptr_t>(e));
            DoingScope d("items: GetParentEntity", reinterpret_cast<uintptr_t>(e));
            if (CallMethodV(e, "GetParentEntity", "", res)) parent = ResolveHandle(*reinterpret_cast<void**>(res));
            Crumb("items: answered", reinterpret_cast<uintptr_t>(e), reinterpret_cast<uintptr_t>(parent));
            if (logged++ < 40) Log("posehook: item %p hangs on %p (engine)", e, parent);
            Crumb("items: logged", reinterpret_cast<uintptr_t>(e));
        // any entity hung on the figure, not only an item: a dangle entity pulled in by a CExternalProxyComponent
        // (so a mesh skinned to the dangle's own rig reaches the figure's watch)
        } else if (!engine && g_ho.ok && IsScriptable(e) && DerivesFromGuarded(e, "CEntity")) {
            Crumb("items: reading", reinterpret_cast<uintptr_t>(e));
            void* att = nullptr;
            void* node = nullptr;
            if (GRead(static_cast<char*>(e) + g_ho.nodeParent, &att, 8) && att && IsScriptable(att) &&
                GRead(static_cast<char*>(att) + g_ho.attParent, &node, 8) && node && IsScriptable(node)) {
                void* up = nullptr;
                if (DerivesFromGuarded(node, "CComponent")) GRead(static_cast<char*>(node) + 0x30, &up, 8);
                else up = node;
                if (up && IsScriptable(up) && DerivesFromGuarded(up, "CEntity")) parent = up;
            }
            Crumb("items: read", reinterpret_cast<uintptr_t>(e), reinterpret_cast<uintptr_t>(parent));
            if (logged++ < 40) Log("posehook: item %p hangs on %p", e, parent);
            Crumb("items: logged", reinterpret_cast<uintptr_t>(e));
        }
        found.emplace_back(e, parent);
        QueryPerformanceCounter(&t1);
        spentMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
    }
    std::lock_guard<std::mutex> lk(g_watchMx);
    Crumb("items: table locked", found.size());
    for (auto& f : found) {
        spread ^= spread << 13, spread ^= spread >> 17, spread ^= spread << 5;
        auto& entry = g_itemParent[f.first];
        entry.parent = f.second;
        entry.recheck = now + 4000 + spread % 2000;
        entry.seen = now;
    }
    for (void* e : later) g_itemAsk.insert(e);
    g_itemStats.looked += found.size();
    if (spentMs > g_itemStats.longestMs) g_itemStats.longestMs = spentMs;
}

void FindSkelOffsets() {  // main thread
    if (g_so.ok) return;
    void* anim = FindClassByName("CAnimatedComponent");
    void* skel = FindClassByName("CSkeleton");
    void* bone = RttiFindType("SSkeletonBone");
    void* pSkel = anim ? FindProperty(anim, "skeleton") : nullptr;
    void* pBones = skel ? FindProperty(skel, "bones") : nullptr;
    void* pPar = skel ? FindProperty(skel, "parentIndices") : nullptr;
    void* pName = bone ? FindProperty(bone, "nameAsCName") : nullptr;
    void* pLod = skel ? FindProperty(skel, "lodBoneNum_1") : nullptr;
    if (!pSkel || !pBones || !pPar || !pName) {
        Log("posehook: GetPose stays off (skeleton fields not found: %d%d%d%d)", !!pSkel, !!pBones, !!pPar, !!pName);
        return;
    }
    GRead(static_cast<char*>(pSkel) + 0x20, &g_so.skeleton, 4);
    GRead(static_cast<char*>(pBones) + 0x20, &g_so.bones, 4);
    GRead(static_cast<char*>(pPar) + 0x20, &g_so.parents, 4);
    GRead(static_cast<char*>(pName) + 0x20, &g_so.boneName, 4);
    if (pLod) GRead(static_cast<char*>(pLod) + 0x20, &g_so.resident, 4);
    g_so.boneSize = RttiSize(bone);
    g_so.ok = g_so.boneSize > 0;
    Log("posehook: GetPose ready (skeleton +0x%x, bones +0x%x, parents +0x%x, bone name +0x%x of %u)", g_so.skeleton,
        g_so.bones, g_so.parents, g_so.boneName, g_so.boneSize);
}

struct SkelInfo {
    std::vector<std::string> names;
    std::vector<const char*> ptrs;
    std::vector<int16_t> parents;
};
std::unordered_map<void*, SkelInfo> g_skels;  // CSkeleton* -> its names and parents (a skeleton resource is shared)
std::mutex g_skelMx;
std::unordered_map<void*, void*> g_entComp;   // watched entity -> its body component seen in the combine
std::unordered_map<void*, void*> g_entMimic;  // watched entity -> its CMimicComponent (the head) seen in the combine
std::unordered_map<void*, bool> g_isMimic;    // component -> it is a CMimicComponent (its class, read once)
bool DerivesFromGuarded(void* obj, const char* name);
struct GetBuf {
    std::vector<float> model;
    float m2w[16];
};
std::unordered_map<void*, GetBuf> g_getBufs;       // entity -> the copy GetPose(BODY) hands out
std::unordered_map<void*, GetBuf> g_getMimicBufs;  // entity -> the copy GetPose(MIMIC) hands out
std::mutex g_getMx;

const SkelInfo* SkelInfoOf(void* skel, bool resident);
// Components whose value at the skeleton's place led nowhere, with that value: read again only when it changes.
// Two int32 there can look like a pointer (0x2100000000: in user space, on 8 bytes) with no memory behind it; the
// guard caught the read 12 to 43 times a second (tester 2, 6806 faults in 9 minutes).
std::unordered_map<void*, void*> g_noSkel;
std::mutex g_noSkelMx;
const SkelInfo* SkeletonOf(void* comp) {
    void* block = nullptr;
    void* skel = nullptr;
    // asked first: with path tracing the game combines a component of a watched figure that holds no skeleton there
    // (31 000 caught faults in 3 minutes)
    char* at = static_cast<char*>(comp) + g_so.skeleton;
    if (!GRead(at, &block, 8)) return nullptr;
    {
        std::lock_guard<std::mutex> lk(g_noSkelMx);
        auto it = g_noSkel.find(comp);
        if (it != g_noSkel.end() && it->second == block) return nullptr;
    }
    // an object of the game lies on 8 bytes; what the testers' games had there were two int32 (0x24 and 0x22, 0x10 and
    // 0x1154: 11 000 caught reads in 10 minutes at one of them). 25.11.: 0x46 and 0 even pass as one on 8 bytes; the
    // guard caught that read, crash reports showed it as a fault - the memory asked about first (cached)
    if (!InUserSpace(block, 0x10) || (reinterpret_cast<uintptr_t>(block) & 7) ||
        !MemReadable(static_cast<char*>(block) + 0x08, 8) ||
        !GRead(static_cast<char*>(block) + 0x08, &skel, 8) || !InUserSpace(skel, 8) || (reinterpret_cast<uintptr_t>(skel) & 7)) {
        std::lock_guard<std::mutex> lk(g_noSkelMx);
        if (g_noSkel.size() > 4096) g_noSkel.clear();
        g_noSkel[comp] = block;
        return nullptr;
    }
    return SkelInfoOf(skel, true);
}
// A dangle's rig: every bone named, also the ones past the LOD count, which a dangle rig's own bones are.
std::unordered_map<void*, SkelInfo> g_rigSkels;  // under g_skelMx
const SkelInfo* SkelInfoOf(void* skel, bool useResident) {
    std::lock_guard<std::mutex> lk(g_skelMx);
    auto& cache = useResident ? g_skels : g_rigSkels;
    auto it = cache.find(skel);
    if (it != cache.end()) return &it->second;
    char* bones = nullptr;
    char* par = nullptr;
    uint32_t nb = 0, np = 0;
    if (!GRead(static_cast<char*>(skel) + g_so.bones, &bones, 8) || !GRead(static_cast<char*>(skel) + g_so.bones + 8, &nb, 4) ||
        !GRead(static_cast<char*>(skel) + g_so.parents, &par, 8) || !GRead(static_cast<char*>(skel) + g_so.parents + 8, &np, 4))
        return nullptr;
    if (!bones || !par || nb == 0 || nb > 2048 || np != nb) return nullptr;
    int32_t resident = static_cast<int32_t>(nb);  // the bones whose runtime order is the resource's
    if (useResident && g_so.resident) {
        GRead(static_cast<char*>(skel) + g_so.resident, &resident, 4);
        if (resident <= 0 || resident > static_cast<int32_t>(nb)) resident = static_cast<int32_t>(nb);
    }
    SkelInfo si;
    si.parents.resize(np);
    if (!GRead(par, si.parents.data(), static_cast<size_t>(np) * 2)) return nullptr;
    for (uint32_t i = 0; i < nb; ++i) {
        uint32_t id = 0;
        GRead(bones + static_cast<size_t>(i) * g_so.boneSize + g_so.boneName, &id, 4);
        const char* s = GetCNameString(id);
        bool known = static_cast<int32_t>(i) < resident;
        si.names.emplace_back(known && s ? s : "");
        if (!known) si.parents[i] = -1;
    }
    for (const std::string& s : si.names) si.ptrs.push_back(s.c_str());
    auto& slot = cache[skel];
    slot = std::move(si);
    slot.ptrs.clear();  // the strings moved: point at their new home
    for (const std::string& s : slot.names) slot.ptrs.push_back(s.c_str());
    return &slot;
}

// Drive modes: 1 = fixed 90 deg twist, 2 = wave (oscillate per frame), 3 = restore (write the saved original back).
std::atomic<int> g_driveMode{1};
std::unordered_map<int, std::array<float, 16>> g_saved;  // original local per bone (captured before first overwrite)
std::mutex g_saveMx;

// Drive one bone's local 4x4 before the original computes model-space. The engine does not refill local after we
// write it (a write persists until something rewrites it), so on the first drive of a bone we save its original
// matrix; restore mode writes that original back, returning the bone to exactly its pre-drive pose. Guarded.
void TwistBone(void* A, int count, int bone) {
    if (bone < 0 || bone >= count) return;
    void* local = nullptr;
    if (!GRead(static_cast<char*>(A) + 0x10, &local, 8) || !local) return;
    float cur[16];
    void* slot = static_cast<char*>(local) + static_cast<size_t>(bone) * 64;
    if (!GRead(slot, cur, 64)) return;
    int mode = g_driveMode.load(std::memory_order_relaxed);
    if (mode == 3) {  // restore: write the saved original back
        std::lock_guard<std::mutex> lk(g_saveMx);
        auto it = g_saved.find(bone);
        if (it != g_saved.end()) GWrite(slot, it->second.data(), 64);
        return;
    }
    {  // capture the original once, before the first overwrite of this bone
        std::lock_guard<std::mutex> lk(g_saveMx);
        if (g_saved.find(bone) == g_saved.end()) {
            std::array<float, 16> o;
            memcpy(o.data(), cur, 64);
            g_saved[bone] = o;
        }
    }
    float a = 1.5707963f;  // 90 deg
    if (mode == 2) {
        double t = static_cast<double>(GetTickCount64()) / 1000.0;
        a = 1.3f * static_cast<float>(sin(t * 3.0));  // +-74 deg at ~0.5 Hz
    }
    float c = cosf(a), s = sinf(a);
    // rotation about local X: row-major, translation kept in the last row
    float tw[16] = {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, cur[12], cur[13], cur[14], 1};
    GWrite(slot, tw, 64);
}

// Scan C (combine 3rd arg) and *(C+0x18) for the mesh's bone-identity remap: an int32 array of `count` values all
// in [0,105) (combine slot -> skeleton bone index). Addendum 10: C = self+0x68, C+0x18 is dereferenced in the
// combine. Dumps candidates so a tool can pick the real remap and read the skeleton names.
// Test a buffer as a remap (count distinct values in [0,105)) for both int16 and int32 strides; log if it looks
// like one. Returns true if a remap was logged.
bool TestRemap(const char* which, const char* tag, void* p, int n) {
    if (!p) return false;
    static int32_t v32[256];
    static int16_t v16[256];
    for (int w = 0; w < 2; ++w) {
        bool ok = w == 0 ? GRead(p, v32, static_cast<size_t>(n) * 4) : GRead(p, v16, static_cast<size_t>(n) * 2);
        if (!ok) continue;
        int inRange = 0, distinct = 0;
        static unsigned char seen[128];
        for (int i = 0; i < 106; ++i) seen[i] = 0;
        for (int i = 0; i < n; ++i) {
            int val = w == 0 ? v32[i] : v16[i];
            if (val >= 0 && val < 105) {
                inRange++;
                if (!seen[val]) { seen[val] = 1; distinct++; }
            }
        }
        if (inRange >= n * 9 / 10 && distinct >= n * 8 / 10) {
            std::string s;
            char tmp[12];
            for (int i = 0; i < n; ++i) { snprintf(tmp, sizeof tmp, "%d ", w == 0 ? v32[i] : v16[i]); s += tmp; }
            Log("posehook/remap %s: %s -> %p int%d[%d] inRange=%d distinct=%d REMAP: %s", which, tag, p, w == 0 ? 32 : 16,
                n, inRange, distinct, s.c_str());
            return true;
        }
    }
    return false;
}

// Scan C and everything it points to for the mesh's bone-identity remap (count distinct bone indices in [0,105)).
void ScanRemap(const char* which, void* C, int count) {
    if (!C) return;
    int n = count > 256 ? 256 : count;
    // C header as int32 (to spot counts/offsets)
    int32_t hdr[32];
    if (GRead(C, hdr, sizeof hdr)) {
        std::string s;
        char tmp[16];
        for (int i = 0; i < 32; ++i) { snprintf(tmp, sizeof tmp, "%d ", hdr[i]); s += tmp; }
        Log("posehook/remap %s: C=%p int32[0..32]: %s", which, C, s.c_str());
    }
    bool found = false;
    found |= TestRemap(which, "C-inline", C, n);
    for (int off = 0; off <= 0x80; off += 8) {
        void* p = nullptr;
        if (!GRead(static_cast<char*>(C) + off, &p, 8) || reinterpret_cast<uintptr_t>(p) < 0x10000) continue;
        char tag[24];
        snprintf(tag, sizeof tag, "*C+0x%x", off);
        found |= TestRemap(which, tag, p, n);
    }
    if (!found) Log("posehook/remap %s: no remap found in C/+0x00..0x80", which);
}

// The plugin's call alone: no C++ objects in a function with __try (C2712; with /EHa the inlined lock of
// g_poseCbs.Remove counted as one).
bool CallPoseGuarded(TW3SE_PoseCallback cb, void* entity, TW3SE_Pose* pose, void* ctx, DWORD* code) {
    __try {
        cb(entity, pose, ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

// Fire the plugin's pose callback for a watched component's body combine: the plugin may rewrite `local` (the
// writable 4x4 bone matrices) before the engine computes model-space. Guarded; a faulting plugin is dropped.
// A combine whose A does not hold the parent table and the matrices where TW3SE reads them gets no callback: a tester's
// game (a watched figure with a 3-slot dangle mesh) had floats at A+0x00, the read of the "table" faulted 77
// times a second (each one taken by a guard), and the plugin got that pointer and a `local` from the same struct to
// write into. No system call here (VirtualQuery in every combine held the combine jobs on the address space lock:
// 13 fps): a pointer has to lie in user space, every wrong one seen so far did not (0x00015ae8..., 0x7f7fffff...,
// -1); the table and both ends of the matrices are then read guarded. The first few are logged.
std::atomic<int> g_badCombines{0};

void FirePose(void* A, void* comp, int count, int kind, TW3SE_PoseCallback cb, void* ctx, const WatchInfo& who, void* rig) {
    void* local = nullptr;
    void* parents = nullptr;
    void* out = nullptr;
    if (!GRead(static_cast<char*>(A) + 0x10, &local, 8) || !local) return;
    GRead(static_cast<char*>(A) + 0x00, &parents, 8);  // combine-local parent indices (>=count = root)
    GRead(static_cast<char*>(A) + 0x20, &out, 8);      // output buffer: stable per-mesh id this frame
    const size_t lbytes = static_cast<size_t>(count) * 64;
    float probe[2];
    static thread_local std::vector<int16_t> table;
    table.resize(count);
    if (!InUserSpace(parents, static_cast<size_t>(count) * 2) || !InUserSpace(local, lbytes) ||
        !GRead(parents, table.data(), static_cast<size_t>(count) * 2) || !GRead(local, probe, 8) ||
        !GRead(static_cast<char*>(local) + lbytes - 8, probe, 8)) {
        int n = g_badCombines.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 5 || n == 1000 || n == 100000) {
            const char* cls = n <= 5 ? ApiClassName(GetObjectClass(comp)) : nullptr;
            Log("posehook: a combine of %p (%s, %d slots, kind %d%s) holds no parent table or matrices where TW3SE reads "
                "them (A+0x00 %p, A+0x10 %p) - no pose callback for it (%d so far)", comp, cls ? cls : "?", count, kind,
                rig ? ", a dangle's rig" : "", parents, local, n);
        }
        return;
    }
    TW3SE_Pose pose{};
    pose.entity = who.entity;
    pose.component = comp;
    memcpy(pose.guid, who.guid, sizeof pose.guid);
    pose.kind = kind;
    pose.boneCount = count;
    // A mesh's slots are its own bone order. The table at A+0x00 (pose.parents) holds each slot's own bone index in
    // the run-time skeleton, the GetPose order (measured on a 13-slot mesh: 0 Root, 9 pelvis,
    // 94.. its own bones; slot 3 = 9 = pelvis). So a slot's name is the skeleton's name at
    // that index, known for the always-resident bones (the others get "").
    static thread_local std::vector<const char*> names;
    static thread_local std::vector<int16_t> own;
    names.assign(count, "");
    own.assign(count, -1);
    const SkelInfo* sk = rig ? SkelInfoOf(rig, false) : g_so.ok ? SkeletonOf(comp) : nullptr;
    if (sk && parents && GRead(parents, own.data(), static_cast<size_t>(count) * 2)) {
        for (int i = 0; i < count; ++i)
            if (own[i] >= 0 && static_cast<size_t>(own[i]) < sk->ptrs.size()) names[i] = sk->ptrs[own[i]];
    }
    pose.boneNames = names.data();
    pose.parents = static_cast<const int16_t*>(parents);
    pose.local = static_cast<float*>(local);  // [count*16] row-major 4x4, translation in the last row (DX)
    pose.localStride = 16;
    // The entity's local-to-world matrix (CNode, +0x70, 4x4 row-major, translation in the last row - the same
    // layout as `local`; the player's world position there in the game and in the REDkit editor dumps).
    // Copied per call so the plugin never reads one the engine is writing.
    float m2w[16];
    if (who.entity && GRead(static_cast<char*>(who.entity) + 0x70, m2w, sizeof m2w) && m2w[15] == 1.0f)
        pose.modelToWorld = m2w;
    pose.mesh = out;
    g_cbInFlight.fetch_add(1);
    DWORD code = 0;
    // a crash in the plugin is dropped, like a tick or present callback (it would crash again every frame)
    if (!CallPoseGuarded(cb, who.entity, &pose, ctx, &code) && g_poseCbs.Remove(cb))
        Log("posehook: the pose callback %p crashed (0x%08lx) and was removed", cb, code);
    g_cbInFlight.fetch_sub(1);
}

// FNV-1a hash over a chunk's full parent table (count int16), so the enumerator dedups by BONE ORDER (not output
// pointer): identical-order chunks collapse to one, distinct orders each get logged with their full table.
uint32_t ParHash(void* A, int count) {
    void* ptbl = nullptr;
    GRead(static_cast<char*>(A) + 0x00, &ptbl, 8);
    if (!ptbl) return 0;
    static int16_t par[256];
    int n = count > 256 ? 256 : count;
    if (!GRead(ptbl, par, static_cast<size_t>(n) * 2)) return 0;
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; ++i) {
        h = (h ^ (par[i] & 0xff)) * 16777619u;
        h = (h ^ ((par[i] >> 8) & 0xff)) * 16777619u;
    }
    return h ? h : 1;
}

// Log a distinct combine (by bone order): key C, output buffer, count, hash and the FULL parent table.
void MeshListLog(const char* which, void* A, void* comp, void* C, int count, uint32_t h) {
    void* out = nullptr;
    void* ptbl = nullptr;
    GRead(static_cast<char*>(A) + 0x20, &out, 8);
    GRead(static_cast<char*>(A) + 0x00, &ptbl, 8);
    Log("posehook/mesh %s: comp=%p key=%p out=%p count=%d hash=%08x", which, comp, C, out, count, h);
    if (ptbl) {
        static int16_t par[256];
        int n = count > 256 ? 256 : count;
        if (GRead(ptbl, par, static_cast<size_t>(n) * 2)) {
            std::string s;
            char tmp[12];
            for (int i = 0; i < n; ++i) {
                snprintf(tmp, sizeof tmp, "%d ", par[i]);
                s += tmp;
            }
            Log("posehook/mesh %s hash=%08x PAR: %s", which, h, s.c_str());
        }
    }
}

// --- `posecombines` (to find a mesh skinned to a dangle's own rig): for a few
// seconds every combine with at least `min` slots, distinct by component and slot count, noted on the animation
// threads (memory reads only); `posecombines report` names the classes and how each owner hangs on what, on the
// main thread.
struct ProbeEntry {
    void* comp;
    void* owner;
    int32_t count;
    const char* which;
};
std::atomic<bool> g_probeOn{false};
std::atomic<ULONGLONG> g_probeUntil{0};
std::atomic<int32_t> g_probeMin{100};
std::mutex g_probeMx;
std::vector<ProbeEntry> g_probe;

void ProbeCombine(const char* which, void* comp, int32_t count) {
    if (count < g_probeMin.load(std::memory_order_relaxed)) return;
    if (GetTickCount64() > g_probeUntil.load(std::memory_order_relaxed)) {
        g_probeOn = false;
        return;
    }
    void* owner = nullptr;
    GRead(static_cast<char*>(comp) + 0x30, &owner, 8);
    std::lock_guard<std::mutex> lk(g_probeMx);
    for (const ProbeEntry& e : g_probe)
        if (e.comp == comp && e.count == count) return;
    if (g_probe.size() < 256) g_probe.push_back({comp, owner, count, which});
}

void* RigOf(void* constraint);  // below, with the dangle rest pose
void MainRestFill(void* comp, void* B);  // below, with the dangle rest pose
bool MainRestArmed();

void HandleCombine(const char* which, void* A, void* B, void* C) {
    void* twistComp = g_twistComp.load(std::memory_order_relaxed);
    void* mapTarget = g_mapTarget.load(std::memory_order_relaxed);
    void* meshListComp = g_meshListComp.load(std::memory_order_relaxed);
    bool cb = g_poseCbs.Any();  // a plugin's pose callback is registered
    bool watching = g_watchCount.load(std::memory_order_relaxed) > 0;
    bool probing = g_probeOn.load(std::memory_order_relaxed);
    bool restMain = MainRestArmed();
    if (!twistComp && !mapTarget && !cb && !meshListComp && !watching && !probing && !restMain) return;  // nothing armed: fast out
    void* comp = static_cast<char*>(B) - PoseOff(0xddc);  // entity identity (eb: B == component+0xddc)
    if (restMain && IsScriptable(comp)) MainRestFill(comp, B);  // before the plugins see the pose
    int32_t count = 0;
    if (!GRead(static_cast<char*>(A) + 0x08, &count, 4)) return;
    // A mesh skinned to a dangle entity's own rig: B is the constraint's model-space buffer
    // (constraint + bonesMS), not component+0xddc. The constraint's parent (+0x30) is the CAnimDangleComponent, which
    // points back at it (+0x130); that component stands for the mesh, and the slots index the constraint's rig.
    void* dangleRig = nullptr;
    if (!IsScriptable(comp)) {
        static const uint32_t bonesOff = FieldOr("CAnimSkeletalDangleConstraint.bonesMS", 0xac);
        void* constraint = static_cast<char*>(B) - bonesOff;
        void* dangle = nullptr;
        void* back = nullptr;
        if (GRead(static_cast<char*>(constraint) + 0x30, &dangle, 8) && IsScriptable(dangle) &&
            GRead(static_cast<char*>(dangle) + 0x130, &back, 8) && back == constraint) {
            comp = dangle;
            dangleRig = RigOf(constraint);
        }
    }
    if (probing) ProbeCombine(which, comp, count);
    if (meshListComp && comp == meshListComp && count >= 40) {
        uint32_t h = ParHash(A, count);  // dedup by BONE ORDER, so distinct orders (not output buffers) are listed
        std::lock_guard<std::mutex> lk(g_meshMx);
        if (g_meshListClear.exchange(0, std::memory_order_relaxed)) g_seenMesh.clear();
        void* hk = reinterpret_cast<void*>(static_cast<uintptr_t>(h));
        if (g_seenMesh.size() < 16 && g_seenMesh.insert(hk).second) MeshListLog(which, A, comp, C, count, h);
    }
    if (mapTarget && comp == mapTarget && count >= 70 && g_mapDone.exchange(1, std::memory_order_relaxed) == 0) {
        MapDump(which, A, comp, count);
        ScanRemap(which, C, count);
    }
    // every mesh of a watched entity or component (pose.mesh tells them apart). Nothing watched, nothing to look up: a
    // registered callback alone used to take the lock for every combine of every figure in view.
    if (watching && count > 0) {
        WatchInfo who{};
        bool watched = false;
        int kind = TW3SE_POSE_BODY;
        void* owner = nullptr;
        GRead(static_cast<char*>(comp) + 0x30, &owner, 8);  // CObject parent: the component's entity
        {
            std::lock_guard<std::mutex> lk(g_watchMx);
            const WatchInfo* w = FindWatch(g_poseWatch, comp, owner);
            if (w) {
                who = *w;
                watched = true;
                // The kind of this component: the head is a CMimicComponent, the body the root animated component
                // (any other component is OTHER; without a known root every non-head component counts as body).
                auto m = g_isMimic.find(comp);
                bool mimic = m != g_isMimic.end() ? m->second : (g_isMimic[comp] = DerivesFromGuarded(comp, "CMimicComponent"));
                kind = mimic ? TW3SE_POSE_MIMIC : (!who.body || who.body == comp) ? TW3SE_POSE_BODY : TW3SE_POSE_OTHER;
                if (kind == TW3SE_POSE_BODY) g_entComp[who.entity] = comp;  // GetPose(BODY) reads the skeleton here
                else if (kind == TW3SE_POSE_MIMIC) g_entMimic[who.entity] = comp;
            }
        }
        if (watched) Crumb("pose: watched combine", reinterpret_cast<uintptr_t>(comp), static_cast<uintptr_t>(kind),
                           static_cast<uintptr_t>(count));
        if (watched && cb && count <= 1024) {
            DoingScope d("the pose callback", reinterpret_cast<uintptr_t>(comp));
            for (int i = 0, n = g_poseCbs.used.load(std::memory_order_acquire); i < n; ++i)  // each plugin's in turn
                if (TW3SE_PoseCallback f = g_poseCbs.slot[i].fn.load(std::memory_order_acquire))
                    FirePose(A, comp, count, kind, f, g_poseCbs.slot[i].ctx.load(std::memory_order_relaxed), who, dangleRig);
        }
    }
    if (twistComp) {
        bool broad = (twistComp == reinterpret_cast<void*>(1));
        int sigIdx = g_twistSigIdx.load(std::memory_order_relaxed);
        int sigVal = g_twistSigVal.load(std::memory_order_relaxed);
        // Targeted twist: Geralt's body is SEVERAL submeshes, each its own combine with its own bone order, all
        // sharing the skin descriptor C - so neither count nor C isolates one. The stable per-submesh fingerprint
        // is the bone order itself: pose_selftest_sig matches the submesh whose PAR[sigIdx]==sigVal. pose_selftest
        // (sigIdx<0) twists every count==81 submesh.
        bool hit = false;
        if (broad) {
            hit = (count >= 60);
        } else if (comp == twistComp && count == 81) {
            if (sigIdx < 0) {
                hit = true;
            } else {
                void* ptbl = nullptr;
                int16_t pv = -1;
                GRead(static_cast<char*>(A) + 0x00, &ptbl, 8);
                if (ptbl) GRead(static_cast<int16_t*>(ptbl) + sigIdx, &pv, 2);
                hit = (pv == sigVal);
            }
        }
        if (hit) TwistBone(A, count, g_twistBone.load(std::memory_order_relaxed));
    }
}

// The combine runs inside an engine job: nothing may leave our code there. A C++ exception or a fault that came out
// of HandleCombine would end the job without counting it done, and the main thread waits for it forever (the
// game hangs, the main thread spinning in a job wait, no thread in TW3SE). Both are
// caught here and logged (the first few).
std::atomic<int> g_guardLogs{0};
void HandleCombineCpp(const char* which, void* A, void* B, void* C) {
    try {
        HandleCombine(which, A, B, C);
    } catch (const std::exception& e) {
        if (g_guardLogs.fetch_add(1) < 5) Log("posehook: exception in the combine hook (%s): %s", which, e.what());
    } catch (...) {
        if (g_guardLogs.fetch_add(1) < 5) Log("posehook: exception in the combine hook (%s)", which);
    }
}
void HandleCombineGuarded(const char* which, void* A, void* B, void* C) {
    __try {
        HandleCombineCpp(which, A, B, C);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_guardLogs.fetch_add(1) < 5) Log("posehook: fault 0x%08lx in the combine hook (%s)", GetExceptionCode(), which);
    }
}

void* Combine1Hook(void* a, void* b, void* c, void* d) {
    DiagOnce("fmt1", a);
    HandleCombineGuarded("fmt1", a, b, c);
    return oCombine1(a, b, c, d);
}
void* Combine2Hook(void* a, void* b, void* c, void* d) {
    DiagOnce("fmt2", a);
    HandleCombineGuarded("fmt2", a, b, c);
    return oCombine2(a, b, c, d);
}

// Built-in test callback (config posehook=selfcb): exercises the full RegisterOnPose -> FirePose -> "plugin writes
// local" path without a plugin, by twisting combine slot 15 (90 deg about local X, translation kept).
void TestPoseCb(void* entity, TW3SE_Pose* pose, void* ctx) {
    (void)entity;
    (void)ctx;
    int b = 15;
    if (!pose || !pose->local || b >= pose->boneCount) return;
    float* m = pose->local + b * pose->localStride;
    float tx = m[12], ty = m[13], tz = m[14];
    m[0] = 1; m[1] = 0; m[2] = 0;  m[3] = 0;
    m[4] = 0; m[5] = 0; m[6] = -1; m[7] = 0;
    m[8] = 0; m[9] = 1; m[10] = 0; m[11] = 0;
    m[12] = tx; m[13] = ty; m[14] = tz; m[15] = 1;
}

// The engine combine hooks, once. Allowed after InstallPoseHook ran (config not `off`, the feature's addresses were
// found); a pose callback registered by a plugin brings them in, so a game without such a plugin runs untouched.
void InstallCombineHooks(const char* why) {
    std::lock_guard<std::mutex> l(g_installMx);
    if (g_hookInstalled || !g_hookAllowed) return;
    g_hookInstalled = true;
    HookNamed("Combine_Format1", reinterpret_cast<void*>(&Combine1Hook), reinterpret_cast<void**>(&oCombine1));
    HookNamed("Combine_Format2", reinterpret_cast<void*>(&Combine2Hook), reinterpret_cast<void**>(&oCombine2));
    Log("posehook: engine hook installed (%s; fmt1=%s fmt2=%s)", why, oCombine1 ? "ok" : "no", oCombine2 ? "ok" : "no");
}

}  // namespace

std::string MimicStats();  // below

// --- the rest pose for the bones a dangle leaves alone (docs/engine/animation.md "Rest pose and
// dangle skeletons"). A dangle constraint writes into its model-space buffer only the bones it maps from the parent's
// skeleton and the ones it simulates; every other bone of its rig keeps what was in memory when the buffer was made
// (OnAttached reallocates without clearing), and a mesh skinned to such a bone flies apart. After the engine's last
// write of a frame (CAnimSkeletalDangleConstraint::EndUpdate) TW3SE sets each of them to its rest pose local to the
// parent times the parent: the way the main skeleton gets one. Only for the rigs a mod names (w3.anim.rest_bones):
// a cloth or hair constraint may write bones it does not list as simulated.
namespace {
struct DArr {  // {data, u32 count}
    void* data;
    uint32_t count;
};
std::mutex g_restMx;
std::unordered_set<void*> g_restRigs;  // CSkeleton* a mod asked for
// CSkeleton* -> its rest pose local, 16 floats a bone. Replaced, never changed: a rig unloaded with its world can come
// back at the same address with other bones, and a fill on another thread may still hold the old one.
struct RestLocal {
    const char* ls;  // the referencePoseLS it was made from
    uint32_t n;
    std::vector<float> m;
};
std::unordered_map<void*, std::shared_ptr<const RestLocal>> g_restLocal;
std::atomic<int> g_restCount{0};
std::atomic<uint64_t> g_restFills{0}, g_restBones{0};
std::atomic<uint64_t> g_startCalls{0}, g_endCalls{0};  // every constraint, to see whether the engine runs the step
using EndUpdate_t = uint64_t (*)(void* c, void* parentMS, void* parentWS, void* mapping);
EndUpdate_t oEndUpdate = nullptr;
EndUpdate_t oStartUpdate = nullptr;  // same parameters
using ToMatrix_t = void* (*)(const void* qs, float* out);

void* RigOf(void* constraint) {  // the rig: a handle at +0x58, the CSkeleton at handle+0x08
    void* h = nullptr;
    void* skel = nullptr;
    if (!GRead(static_cast<char*>(constraint) + 0x58, &h, 8) || !h) return nullptr;
    return GRead(static_cast<char*>(h) + 0x08, &skel, 8) ? skel : nullptr;
}

void Mul(const float* a, const float* b, float* out) {  // out = a x b, row-major 4x4 (translation in the last row)
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            out[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                             a[i * 4 + 3] * b[3 * 4 + j];
}

void RestFill(void* c, void* mapping) {  // C++ objects here; the caller guards
    void* skel = RigOf(c);
    if (!skel) return;
    std::shared_ptr<const RestLocal> rest;
    uint32_t n = 0;
    const char* ls = nullptr;
    GRead(static_cast<char*>(skel) + FieldOr("CSkeleton.boneCount", 0x70), &n, 4);
    GRead(static_cast<char*>(skel) + FieldOr("CSkeleton.referencePoseLS", 0x90), &ls, 8);
    if (!n || n > 2048 || !ls) return;
    {
        std::lock_guard<std::mutex> lk(g_restMx);
        if (!g_restRigs.count(skel)) return;
        auto it = g_restLocal.find(skel);
        if (it == g_restLocal.end() || it->second->ls != ls || it->second->n != n) {  // first frame of this rig
            auto toMatrix = reinterpret_cast<ToMatrix_t>(Address("EngineQsTransform::ToMatrix"));
            if (!toMatrix) return;
            auto r = std::make_shared<RestLocal>();
            r->ls = ls;
            r->n = n;
            r->m.resize(static_cast<size_t>(n) * 16);
            for (uint32_t b = 0; b < n; ++b) toMatrix(ls + static_cast<size_t>(b) * 48, r->m.data() + static_cast<size_t>(b) * 16);
            g_restLocal[skel] = r;
            rest = std::move(r);
        } else {
            rest = it->second;
        }
    }
    DArr ms{};
    GRead(static_cast<char*>(c) + FieldOr("CAnimSkeletalDangleConstraint.bonesMS", 0xac), &ms, sizeof ms);
    if (!ms.data || ms.count != n) return;
    const int16_t* par = nullptr;
    GRead(static_cast<char*>(skel) + 0x80, &par, 8);
    if (!par) return;
    static thread_local std::vector<char> driven;
    driven.assign(n, 0);
    DArr map{};
    if (mapping && GRead(mapping, &map, sizeof map) && map.data && map.count <= 4096)
        for (uint32_t k = 0; k < map.count; ++k) {
            int16_t own = static_cast<const int16_t*>(map.data)[k * 2];
            if (own >= 0 && static_cast<uint32_t>(own) < n) driven[own] = 1;
        }
    DArr sim{};
    if (GRead(static_cast<char*>(c) + 0xa0, &sim, sizeof sim) && sim.data && sim.count <= 4096)
        for (uint32_t k = 0; k < sim.count; ++k) {
            int32_t b = static_cast<const int32_t*>(sim.data)[k];
            if (b >= 0 && static_cast<uint32_t>(b) < n) driven[b] = 1;
        }
    auto* out = static_cast<float*>(ms.data);
    uint32_t filled = 0;
    for (uint32_t b = 0; b < n; ++b) {  // a parent comes before its child
        if (driven[b]) continue;
        const float* L = rest->m.data() + static_cast<size_t>(b) * 16;
        int16_t p = par[b];
        if (p >= 0 && static_cast<uint32_t>(p) < b) Mul(L, out + static_cast<size_t>(p) * 16, out + static_cast<size_t>(b) * 16);
        else memcpy(out + static_cast<size_t>(b) * 16, L, 64);
        ++filled;
    }
    g_restFills.fetch_add(1, std::memory_order_relaxed);
    g_restBones.fetch_add(filled, std::memory_order_relaxed);
}
void RestFillCpp(void* c, void* mapping) {  // nothing may leave TW3SE's code inside the engine's animation job
    try {
        RestFill(c, mapping);
    } catch (...) {
    }
}
void RestFillGuarded(void* c, void* mapping) {
    __try {
        RestFillCpp(c, mapping);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
uint64_t EndUpdateHook(void* c, void* parentMS, void* parentWS, void* mapping) {
    uint64_t r = oEndUpdate(c, parentMS, parentWS, mapping);
    g_endCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_restCount.load(std::memory_order_relaxed) > 0) RestFillGuarded(c, mapping);
    return r;
}
// Also after StartUpdate, which has just copied the mapped bones (a figure held in one pose gets no EndUpdate at
// all and would keep the rest pose in model space). The simulated bones are filled here too; where the
// simulation runs, it writes them again before EndUpdate.
uint64_t StartUpdateHook(void* c, void* parentMS, void* parentWS, void* mapping) {
    uint64_t r = oStartUpdate(c, parentMS, parentWS, mapping);
    g_startCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_restCount.load(std::memory_order_relaxed) > 0) RestFillGuarded(c, mapping);
    return r;
}

// --- the rest pose for bones a mod adds to a figure's main skeleton (w3.anim.rest_bones(rig, {names})). No animation
// of the game has a track for such a bone; the engine leaves it in a pose of its own when the figure stands. Before
// a mesh of a figure with such a rig is skinned, each bone named gets its rest pose local to its parent times the
// parent, in the figure's model-space buffer (*(component+0xddc)), which every mesh's combine and GetPose read. The
// bones are found by name in the rig, on the main thread; a parent in the bones the engine sorts anew at run time
// (from lodBoneNum_1 on, the game's own) cannot be found that way, and such a bone is refused.
struct MainRestBone {
    int32_t bone, parent;
    float local[16];
};
struct MainRest {
    std::vector<MainRestBone> bones;  // a parent before its child
};
std::unordered_map<void*, std::shared_ptr<const MainRest>> g_restMain;  // CSkeleton* -> its bones; under g_restMx
std::atomic<int> g_restMainCount{0};
std::atomic<uint64_t> g_restMainFills{0}, g_restMainWrites{0};
std::mutex g_restMainWriteMx;  // the meshes of one figure may be skinned on several threads at once
bool MainRestArmed() { return g_restMainCount.load(std::memory_order_relaxed) > 0; }

// In the combine of a figure's mesh (an animation thread; the caller guards): the named bones of its rig, if any.
void MainRestFill(void* comp, void* B) {
    if (!g_so.ok) return;
    void* block = nullptr;
    void* skel = nullptr;
    if (!GRead(static_cast<char*>(comp) + g_so.skeleton, &block, 8) || !block) return;
    if (!GRead(static_cast<char*>(block) + 0x08, &skel, 8) || !skel) return;
    std::shared_ptr<const MainRest> rest;
    {
        std::lock_guard<std::mutex> lk(g_restMx);
        auto it = g_restMain.find(skel);
        if (it == g_restMain.end()) return;
        rest = it->second;
    }
    int32_t n = 0;
    float* ms = nullptr;
    if (!GRead(static_cast<char*>(comp) + PoseOff(0xe08), &n, 4) || n <= 0 || n > 2048) return;
    if (!GRead(B, &ms, 8) || !ms) return;
    // every mesh of the figure goes through here first: the first one of a frame writes, the others find it written
    // (and none of them reads the buffer before its own call has come through here)
    std::lock_guard<std::mutex> lk(g_restMainWriteMx);
    float m[16];
    uint64_t wrote = 0;
    for (const MainRestBone& b : rest->bones) {
        if (b.bone >= n || b.parent >= n) continue;
        Mul(b.local, ms + static_cast<size_t>(b.parent) * 16, m);
        float* dst = ms + static_cast<size_t>(b.bone) * 16;
        if (memcmp(dst, m, 64) != 0) memcpy(dst, m, 64), ++wrote;
    }
    g_restMainFills.fetch_add(1, std::memory_order_relaxed);
    if (wrote) g_restMainWrites.fetch_add(wrote, std::memory_order_relaxed);
}

// The bones of `names` in a main rig (main thread). -> "" or what is wrong.
std::string MainRestMake(void* skel, const std::vector<std::string>& names, MainRest* out) {
    FindSkelOffsets();
    if (!g_so.ok) return "the skeleton's fields are not known for this game build";
    auto toMatrix = reinterpret_cast<ToMatrix_t>(Address("EngineQsTransform::ToMatrix"));
    if (!toMatrix) return "the rest pose is not known for this game build";
    char* bones = nullptr;
    int16_t* par = nullptr;
    const char* ls = nullptr;
    uint32_t nb = 0, np = 0, nls = 0;
    int32_t resident = 0;
    GRead(static_cast<char*>(skel) + g_so.bones, &bones, 8);
    GRead(static_cast<char*>(skel) + g_so.bones + 8, &nb, 4);
    GRead(static_cast<char*>(skel) + g_so.parents, &par, 8);
    GRead(static_cast<char*>(skel) + g_so.parents + 8, &np, 4);
    GRead(static_cast<char*>(skel) + FieldOr("CSkeleton.referencePoseLS", 0x90), &ls, 8);
    GRead(static_cast<char*>(skel) + FieldOr("CSkeleton.boneCount", 0x70), &nls, 4);
    if (g_so.resident) GRead(static_cast<char*>(skel) + g_so.resident, &resident, 4);
    if (!bones || !par || !ls || nb == 0 || nb > 2048 || np != nb || nls != nb) return "the rig's bones cannot be read";
    if (resident <= 0 || resident > static_cast<int32_t>(nb)) resident = static_cast<int32_t>(nb);
    std::vector<std::string> boneNames(nb);
    for (uint32_t i = 0; i < nb; ++i) {
        uint32_t id = 0;
        GRead(bones + static_cast<size_t>(i) * g_so.boneSize + g_so.boneName, &id, 4);
        const char* s = GetCNameString(id);
        boneNames[i] = s ? s : "";
    }
    std::vector<int32_t> idx;
    for (const std::string& want : names) {
        auto it = std::find(boneNames.begin(), boneNames.end(), want);
        if (it == boneNames.end()) return "the rig has no bone " + want;
        idx.push_back(static_cast<int32_t>(it - boneNames.begin()));
    }
    std::sort(idx.begin(), idx.end());
    idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
    for (int32_t b : idx) {
        int16_t p = 0;
        GRead(par + b, &p, 2);
        if (p < 0 || p >= b) return boneNames[b] + " has no parent before it";
        if (p >= resident && !std::binary_search(idx.begin(), idx.end(), static_cast<int32_t>(p)))
            return boneNames[b] + " hangs on " + boneNames[p] + ", which the engine sorts anew at run time (name that "
                   "bone too, or hang it on one of the first " + std::to_string(resident) + ")";
        MainRestBone e{};
        e.bone = b;
        e.parent = p;
        toMatrix(ls + static_cast<size_t>(b) * 48, e.local);
        out->bones.push_back(e);
    }
    return "";
}
}  // namespace

// w3.anim.rest_bones (lua.cpp): a dangle rig whose unmapped bones get the rest pose (on = 0 takes it off), or with
// `names` the bones of a main rig that get it. Main thread.
std::string PoseRestBones(void* skeleton, bool on, const std::vector<std::string>* names) {
    if (!skeleton || !IsScriptable(skeleton) || !DerivesFromGuarded(skeleton, "CSkeleton")) return "a CSkeleton (.w2rig) is needed";
    if (names && on) {
        auto rest = std::make_shared<MainRest>();
        std::string err = MainRestMake(skeleton, *names, rest.get());
        if (!err.empty()) return err;
        if (!g_hookAllowed) return "the pose hook is off (posehook = off in tw3se.config)";
        InstallCombineHooks("rest bones of a main skeleton");
        size_t count = rest->bones.size();
        {
            std::lock_guard<std::mutex> lk(g_restMx);
            g_restMain[skeleton] = std::move(rest);
            g_restMainCount.store(static_cast<int>(g_restMain.size()), std::memory_order_relaxed);
        }
        Log("posehook: rest pose for %zu bone(s) of the main rig %p", count, skeleton);
        return "";
    }
    if (!on) {
        std::lock_guard<std::mutex> lk(g_restMx);
        g_restMain.erase(skeleton);
        g_restMainCount.store(static_cast<int>(g_restMain.size()), std::memory_order_relaxed);
    }
    if (!oEndUpdate) {
        void* target = reinterpret_cast<void*>(Address("CAnimSkeletalDangleConstraint::EndUpdate"));
        if (!target || !Address("EngineQsTransform::ToMatrix")) return "the dangle update is not known for this game build";
        if (!Hook(target, reinterpret_cast<void*>(&EndUpdateHook), reinterpret_cast<void**>(&oEndUpdate)))
            return "the dangle update could not be hooked";
        Log("posehook: dangle rest pose hook in");
        void* start = reinterpret_cast<void*>(Address("CAnimSkeletalDangleConstraint::StartUpdate"));
        if (start && Hook(start, reinterpret_cast<void*>(&StartUpdateHook), reinterpret_cast<void**>(&oStartUpdate)))
            Log("posehook: dangle rest pose also after StartUpdate");
    }
    std::lock_guard<std::mutex> lk(g_restMx);
    if (on) g_restRigs.insert(skeleton);
    else g_restRigs.erase(skeleton);
    g_restCount.store(static_cast<int>(g_restRigs.size()), std::memory_order_relaxed);
    return "";
}
// A rig that was unloaded (its world ended): its address is taken out, so a new object there is not filled by mistake.
void PoseRestForget(void* skeleton) {
    std::lock_guard<std::mutex> lk(g_restMx);
    g_restRigs.erase(skeleton);
    g_restCount.store(static_cast<int>(g_restRigs.size()), std::memory_order_relaxed);
    g_restMain.erase(skeleton);
    g_restMainCount.store(static_cast<int>(g_restMain.size()), std::memory_order_relaxed);
}
std::string PoseRestStats() {
    char b[320];
    snprintf(b, sizeof b, "%d rig(s), %llu fill(s), %llu bone(s) set; the engine ran StartUpdate %llu, EndUpdate %llu time(s); "
             "main rigs %d, %llu fill(s), %llu bone(s) written",
             g_restCount.load(), static_cast<unsigned long long>(g_restFills.load()),
             static_cast<unsigned long long>(g_restBones.load()), static_cast<unsigned long long>(g_startCalls.load()),
             static_cast<unsigned long long>(g_endCalls.load()), g_restMainCount.load(),
             static_cast<unsigned long long>(g_restMainFills.load()), static_cast<unsigned long long>(g_restMainWrites.load()));
    return b;
}

void InstallPoseHook() {
    RegisterCommand("mimic", [](const char*, char* reply, uint32_t size) {
        strncpy_s(reply, size, MimicStats().c_str(), _TRUNCATE);
    }, "mimic: whether the mimic hook is in and how often it fired");
    std::string mode = ConfigGet("posehook", "auto");
    std::string im = ConfigGet("posehook_items", "on");
    g_itemsMode = im == "off" ? 0 : im == "engine" ? 2 : 1;
    // `posecombines [seconds] [minSlots]`: arms the probe (default 3 s, 100 slots); `posecombines report`: what came
    RegisterCommand("posecombines", [](const char* args, char* reply, uint32_t size) {
        std::string a = args ? args : "";
        if (a.rfind("report", 0) != 0) {
            int secs = 3, min = 100;
            sscanf_s(a.c_str(), "%d %d", &secs, &min);
            {
                std::lock_guard<std::mutex> lk(g_probeMx);
                g_probe.clear();
            }
            g_probeMin = min;
            g_probeUntil = GetTickCount64() + static_cast<ULONGLONG>(secs) * 1000;
            g_probeOn = true;
            InstallCombineHooks("posecombines");
            snprintf(reply, size, "posecombines: noting every combine with %d slots or more for %d s; then `posecombines report`",
                     min, secs);
            return;
        }
        std::vector<ProbeEntry> list;
        {
            std::lock_guard<std::mutex> lk(g_probeMx);
            list = g_probe;
        }
        FindHangOffsets();
        auto cls = [](void* o) -> std::string {
            return o && IsScriptable(o) ? RttiName(GetObjectClass(o)) : std::string(o ? "(not an object)" : "-");
        };
        std::string out = "posecombines: " + std::to_string(list.size()) + " distinct combine(s)";
        for (const ProbeEntry& e : list) {
            char line[512];
            const SkelInfo* sk = IsScriptable(e.comp) ? SkeletonOf(e.comp) : nullptr;
            std::string chain;  // the owner's transform parent: attachment -> its parent -> that one's entity
            void* att = nullptr;
            void* node = nullptr;
            if (g_ho.ok && IsScriptable(e.owner) && GRead(static_cast<char*>(e.owner) + g_ho.nodeParent, &att, 8) && att &&
                IsScriptable(att)) {
                chain = " hangs by " + cls(att);
                if (GRead(static_cast<char*>(att) + g_ho.attParent, &node, 8) && node && IsScriptable(node)) {
                    void* up = nullptr;
                    GRead(static_cast<char*>(node) + 0x30, &up, 8);
                    chain += " on " + cls(node) + " " + std::to_string(reinterpret_cast<uintptr_t>(node)) + " of " +
                             cls(up) + " " + std::to_string(reinterpret_cast<uintptr_t>(up));
                }
            }
            snprintf(line, sizeof line, "\n  %s %d slots: component %p %s (skeleton %zu bones), owner %p %s%s", e.which,
                     e.count, e.comp, cls(e.comp).c_str(), sk ? sk->parents.size() : static_cast<size_t>(0), e.owner,
                     cls(e.owner).c_str(), chain.c_str());
            out += line;
        }
        Log("%s", out.c_str());
        strncpy_s(reply, size, out.c_str(), _TRUNCATE);
    }, "posecombines [seconds] [minSlots] | report: every combine with many slots for a while: component, skeleton, owner and what it hangs on");
    RegisterCommand("poseitems", [](const char* args, char* reply, uint32_t size) {
        std::string a = args ? args : "";
        std::lock_guard<std::mutex> lk(g_watchMx);
        if (a == "off" || a == "data" || a == "engine") {
            g_itemsMode = a == "off" ? 0 : a == "engine" ? 2 : 1;
            g_itemParent.clear();
            g_itemAsk.clear();
        }
        size_t hanging = 0;
        for (const auto& e : g_itemParent) hanging += e.second.parent != nullptr;
        static const char* names[] = {"off", "data", "engine"};
        snprintf(reply, size,
                 "items on a watched figure: %s; %d watch(es) with items, %zu entities known (%zu hang on a figure), "
                 "%zu waiting, %llu looked at, longest tick %.2f ms",
                 names[g_itemsMode.load()], g_itemWatchers.load(), g_itemParent.size(), hanging, g_itemAsk.size(),
                 static_cast<unsigned long long>(g_itemStats.looked), g_itemStats.longestMs);
    }, "poseitems [off|data|engine]: how a watched figure finds its items (engine = CItemEntity.GetParentEntity), and what the search costs");
    RegisterTick(&OnItemsTick, nullptr);
    if (!g_itemsMode) Log("posehook: items hung on a figure stay out (config posehook_items = off)");
    if (mode == "off") {
        Log("posehook: off (config posehook=off); a plugin's pose callback stays dormant");
        return;
    }
    g_hookAllowed = true;
    if (mode == "auto") {
        Log("posehook: installs when a plugin registers a pose callback");
        if (g_poseCbs.Any()) InstallCombineHooks("a callback was registered before start");
        return;
    }
    InstallCombineHooks(mode.c_str());
    if (mode == "diag") g_diagLeft.store(6, std::memory_order_relaxed);  // log the first few calls' A layout
    if (mode == "selfcb") PoseRegisterCallback(&TestPoseCb, nullptr);    // built-in test callback (no plugin needed)
}

void PoseSelftestTarget(void* comp, int bone) {
    // comp = the component (B - 0xddc) to twist, or (void*)1 = broad (every humanoid); bone in COMBINE order.
    // No signature: twists the bone in every count==81 submesh of the component.
    g_twistBone.store(bone, std::memory_order_relaxed);
    g_twistSigIdx.store(-1, std::memory_order_relaxed);
    g_driveMode.store(1, std::memory_order_relaxed);
    g_twistComp.store(comp, std::memory_order_relaxed);
}

void PoseSelftestSig(void* comp, int bone, int sigIdx, int sigVal) {
    // Twist `bone` only in the submesh whose PAR[sigIdx]==sigVal (the stable per-submesh bone-order fingerprint),
    // so the slot -> body part anchor comes from exactly one mesh.
    g_twistBone.store(bone, std::memory_order_relaxed);
    g_twistSigIdx.store(sigIdx, std::memory_order_relaxed);
    g_twistSigVal.store(sigVal, std::memory_order_relaxed);
    g_driveMode.store(1, std::memory_order_relaxed);
    g_twistComp.store(comp, std::memory_order_relaxed);
}

void PoseWave(void* comp, int bone, int sigIdx, int sigVal, int on) {
    // Live per-frame proof: oscillate `bone` in the matching submesh every frame. on=0 stops the wave (bone stays
    // at its last value until restored) and disarms.
    if (on) {
        g_driveMode.store(2, std::memory_order_relaxed);
        g_twistBone.store(bone, std::memory_order_relaxed);
        g_twistSigIdx.store(sigIdx, std::memory_order_relaxed);
        g_twistSigVal.store(sigVal, std::memory_order_relaxed);
        g_twistComp.store(comp, std::memory_order_relaxed);
    } else {
        g_twistComp.store(nullptr, std::memory_order_relaxed);
    }
}

void PoseRestore(void* comp, int bone, int sigIdx, int sigVal) {
    // Write the bone's saved original matrix back each frame, returning it to its exact pre-drive pose. Keep this
    // armed (the write persists); disarm with pose_selftest(0,-1) once it looks right.
    g_twistBone.store(bone, std::memory_order_relaxed);
    g_twistSigIdx.store(sigIdx, std::memory_order_relaxed);
    g_driveMode.store(3, std::memory_order_relaxed);
    g_twistComp.store(comp, std::memory_order_relaxed);
}

void PoseMeshList(void* comp, int on) {
    // Arm/disarm the mesh enumerator: log each distinct combine (mesh) of `comp` once (key, out, count, PAR0-7).
    if (on) {
        g_meshListClear.store(1, std::memory_order_relaxed);
        g_meshListComp.store(comp, std::memory_order_relaxed);
    } else {
        g_meshListComp.store(nullptr, std::memory_order_relaxed);
    }
}

void PoseMapTarget(void* comp) {
    // arm a one-shot A+0x18 remap dump for this component's next combine call.
    g_mapDone.store(0, std::memory_order_relaxed);
    g_mapTarget.store(comp, std::memory_order_relaxed);
}

void PoseRegisterCallback(TW3SE_PoseCallback cb, void* ctx) {
    if (!cb) return;
    if (!g_poseCbs.Add(cb, ctx)) Log("posehook: pose callback %p refused (%d plugins have one already)", cb, g_poseCbs.kSlots);
    else InstallCombineHooks("a plugin registered a pose callback");
}

// Whether an object's class is or derives from `name` (CClass: parent +0x10, name CName +0x2c, both builds).
namespace {
bool DerivesFromGuarded(void* obj, const char* name) {
    __try {
        void* c = GetObjectClass(obj);
        for (int d = 0; c && d < 32; ++d) {
            const char* n = GetCNameString(*reinterpret_cast<uint32_t*>(static_cast<char*>(c) + 0x2c));
            if (n && strcmp(n, name) == 0) return true;
            c = *reinterpret_cast<void**>(static_cast<char*>(c) + 0x10);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

// What a watch is on (main thread): a component reports its entity, an entity itself. Only a figure takes its items
// with it - a component or an item watched itself needs no search for them. Neither an entity nor a component (a
// handle, a script object) is logged: nothing would ever fire for it, and nothing used to say so.
void WatchTarget(void* object, WatchInfo* who) {
    bool component = DerivesFromGuarded(object, "CComponent");
    bool entity = !component && DerivesFromGuarded(object, "CEntity");
    if (component) {
        void* owner = nullptr;
        GRead(static_cast<char*>(object) + 0x30, &owner, 8);
        if (owner) who->entity = owner;
    } else if (!entity) {
        Log("posehook: %p is neither an entity nor a component, nothing will fire for it", object);
    }
    who->items = entity && !DerivesFromGuarded(object, "CItemEntity");
    if (!WorldEntityGuid(who->entity, who->guid)) memset(who->guid, 0, sizeof who->guid);  // a run-time entity
}
}  // namespace

int PoseWatch(void* entity, int on) { return PoseWatchBy(entity, on, 1); }

int PoseWatchBy(void* entity, int on, int by) {
    // An entity (each of its animated components fires, one callback per rendered mesh) or one component. Runs on
    // the main thread: the entity behind a component (its CObject parent, +0x30) and the GUID are read here. Each
    // watcher has its bit (when w3.pose and a plugin watch the same figure, one's unwatch leaves the other's).
    if (!entity) return 0;
    if (!on) {
        std::lock_guard<std::mutex> lk(g_watchMx);
        auto it = g_poseWatch.find(entity);
        if (it != g_poseWatch.end() && (it->second.by &= ~by) == 0) g_poseWatch.erase(it);
        g_watchCount.store(static_cast<int>(g_poseWatch.size()), std::memory_order_relaxed);
        CountItemWatchers();
        return 1;
    }
    {
        std::lock_guard<std::mutex> lk(g_watchMx);
        auto it = g_poseWatch.find(entity);
        if (it != g_poseWatch.end()) {  // watched already (w3.pose asks every frame)
            it->second.by |= by;
            return 1;
        }
    }
    WatchInfo who{};
    who.entity = entity;
    who.by = by;
    WatchTarget(entity, &who);
    alignas(16) uint8_t res[32] = {};
    if (CallMethodV(who.entity, "GetRootAnimatedComponent", "", res)) who.body = ResolveHandle(*reinterpret_cast<void**>(res));
    FindSkelOffsets();  // GetPose's RTTI offsets, on the main thread
    {
        std::lock_guard<std::mutex> lk(g_watchMx);
        g_isMimic.clear();  // a component address may belong to a new component by now
        g_poseWatch[entity] = who;
        g_watchCount.store(static_cast<int>(g_poseWatch.size()), std::memory_order_relaxed);
        CountItemWatchers();
    }
    Log("posehook: watching %p (entity %p, body component %p%s)", entity, who.entity, who.body,
        who.items ? ", with its items" : "");
    InstallCombineHooks("an entity is watched (GetPose)");
    return 1;
}

// GetComponentPose (interface 24, also for a mesh skinned to a dangle entity's own rig): the last evaluated
// pose of one animated component, in model space of its own skeleton, with that skeleton's names and parents (bones
// without an animation track too); modelToWorld of the component's entity. Read-only, as GetPose.
std::unordered_map<void*, GetBuf> g_getCompBufs;  // by component, under g_getMx
int PoseGetDangle(void* dangle, TW3SE_Pose* out) {  // a CAnimDangleComponent: its constraint's rig and buffer
    void* constraint = nullptr;
    void* back = nullptr;
    if (!GRead(static_cast<char*>(dangle) + 0x130, &constraint, 8) || !constraint ||
        !GRead(static_cast<char*>(constraint) + 0x30, &back, 8) || back != dangle)
        return 0;
    void* rig = RigOf(constraint);
    const SkelInfo* sk = rig ? SkelInfoOf(rig, false) : nullptr;
    struct {
        float* data;
        uint32_t count;
    } ms{};
    if (!sk || !GRead(static_cast<char*>(constraint) + FieldOr("CAnimSkeletalDangleConstraint.bonesMS", 0xac), &ms, 12) ||
        !ms.data || ms.count == 0 || ms.count > 2048 || sk->parents.size() < ms.count)
        return 0;
    // bonesMS is in the model space of the figure the dangle hangs on: its entity's matrix (the item search knows it)
    void* entity = nullptr;
    GRead(static_cast<char*>(dangle) + 0x30, &entity, 8);
    void* figure = entity;
    {
        std::lock_guard<std::mutex> lk(g_watchMx);
        auto p = g_itemParent.find(entity);
        if (p != g_itemParent.end() && p->second.parent) figure = p->second.parent;
    }
    std::lock_guard<std::mutex> lk(g_getMx);
    GetBuf& gb = g_getCompBufs[dangle];
    gb.model.resize(static_cast<size_t>(ms.count) * 16);
    if (!GRead(ms.data, gb.model.data(), static_cast<size_t>(ms.count) * 64)) return 0;
    if (!figure || !GRead(static_cast<char*>(figure) + 0x70, gb.m2w, sizeof gb.m2w) || gb.m2w[15] != 1.0f) return 0;
    *out = TW3SE_Pose{};
    out->entity = figure;
    out->component = dangle;
    out->kind = TW3SE_POSE_OTHER;
    out->boneCount = static_cast<int32_t>(ms.count);
    out->boneNames = sk->ptrs.data();
    out->parents = sk->parents.data();
    out->local = gb.model.data();
    out->localStride = 16;
    out->modelToWorld = gb.m2w;
    return 1;
}

int PoseGetComponent(void* comp, TW3SE_Pose* out) {
    if (!comp || !out || !IsScriptable(comp)) return 0;
    if (DerivesFromGuarded(comp, "CAnimDangleComponent")) return PoseGetDangle(comp, out);
    if (!g_so.ok || !DerivesFromGuarded(comp, "CAnimatedComponent")) return 0;
    int32_t n = 0;
    void* buf = nullptr;
    if (!GRead(static_cast<char*>(comp) + PoseOff(0xe08), &n, 4) || n <= 0 || n > 1024) return 0;
    if (!GRead(static_cast<char*>(comp) + PoseOff(0xddc), &buf, 8) || !buf) return 0;
    const SkelInfo* sk = SkeletonOf(comp);
    if (!sk || sk->parents.size() < static_cast<size_t>(n)) return 0;
    void* entity = nullptr;
    GRead(static_cast<char*>(comp) + 0x30, &entity, 8);
    std::lock_guard<std::mutex> lk(g_getMx);
    GetBuf& gb = g_getCompBufs[comp];
    gb.model.resize(static_cast<size_t>(n) * 16);
    if (!GRead(buf, gb.model.data(), static_cast<size_t>(n) * 64)) return 0;
    if (!entity || !GRead(static_cast<char*>(entity) + 0x70, gb.m2w, sizeof gb.m2w) || gb.m2w[15] != 1.0f) return 0;
    *out = TW3SE_Pose{};
    out->entity = entity;
    out->component = comp;
    out->kind = DerivesFromGuarded(comp, "CMimicComponent") ? TW3SE_POSE_MIMIC : TW3SE_POSE_OTHER;
    out->boneCount = n;
    out->boneNames = sk->ptrs.size() >= static_cast<size_t>(n) ? sk->ptrs.data() : nullptr;
    out->parents = sk->parents.data();
    out->local = gb.model.data();
    out->localStride = 16;
    out->modelToWorld = gb.m2w;
    return 1;
}

int PoseGet(void* entity, int kind, TW3SE_Pose* out) {
    if (!entity || !out || (kind != TW3SE_POSE_BODY && kind != TW3SE_POSE_MIMIC) || !g_so.ok) return 0;
    void* comp = nullptr;
    WatchInfo who{};
    {
        std::lock_guard<std::mutex> lk(g_watchMx);
        auto& map = kind == TW3SE_POSE_MIMIC ? g_entMimic : g_entComp;
        const WatchInfo* known = nullptr;
        auto w = g_poseWatch.find(entity);
        if (w != g_poseWatch.end()) known = &w->second;
        else if ((w = g_mimicWatch.find(entity)) != g_mimicWatch.end()) known = &w->second;
        if (known) who = *known;
        else who.entity = entity;
        // a watched component asks for its entity's pose: the table goes by entity
        auto c = map.find(entity);
        if (c == map.end() && known) c = map.find(who.entity);
        if (c != map.end()) comp = c->second;
    }
    if (!comp) return 0;  // no frame of this figure seen yet (watched with WatchPose, after its first frame)
    int32_t n = 0;
    void* buf = nullptr;
    if (!GRead(static_cast<char*>(comp) + PoseOff(0xe08), &n, 4) || n <= 0 || n > 512) return 0;
    if (!GRead(static_cast<char*>(comp) + PoseOff(0xddc), &buf, 8) || !buf) return 0;
    const SkelInfo* sk = SkeletonOf(comp);
    if (!sk || sk->parents.size() < static_cast<size_t>(n)) return 0;
    std::lock_guard<std::mutex> lk(g_getMx);
    // stays at this address; refilled by the next GetPose of this figure and kind
    GetBuf& gb = (kind == TW3SE_POSE_MIMIC ? g_getMimicBufs : g_getBufs)[who.entity];
    gb.model.resize(static_cast<size_t>(n) * 16);
    if (!GRead(buf, gb.model.data(), static_cast<size_t>(n) * 64)) return 0;
    if (!who.entity || !GRead(static_cast<char*>(who.entity) + 0x70, gb.m2w, sizeof gb.m2w) || gb.m2w[15] != 1.0f)
        return 0;
    *out = TW3SE_Pose{};
    out->entity = who.entity;
    out->component = comp;
    out->kind = kind;
    memcpy(out->guid, who.guid, sizeof out->guid);
    out->boneCount = n;
    out->boneNames = sk->ptrs.size() >= static_cast<size_t>(n) ? sk->ptrs.data() : nullptr;
    out->parents = sk->parents.data();
    out->local = gb.model.data();  // model space, see above (a copy: writing it changes nothing in the game)
    out->localStride = 16;
    out->modelToWorld = gb.m2w;
    out->mesh = nullptr;
    return 1;
}

// --- the mimic hook: the face's track weights between the scene's mix and the face bones -------------------------
// CBehaviorGraphMimicsConverterNode::Sample (5.01 0x024d1630) first samples its track input (node+0x138) into a
// pooled output (+0x08 float* weights, +0x1c their count), then turns every weight into face bones with
// CMimicFace.mimicPoses. That one virtual call (`call [rax+0x1b0]`, 6 bytes, 29 bytes into Mimic_TrackSampleCall)
// becomes `call [rip+slot]` to MimicTrackSample: it runs the input's Sample and then the plugin's callback on the
// filled weights. The behaviour instance's +0x68 is the CMimicComponent; the track names are the `tracks` of its
// CMimicFace's floatTrackSkeleton. RTTI offsets are found on the main thread (WatchMimic).
namespace {
using SampleFn = void (*)(void*, void*, void*, void*);
struct MimicOffsets {
    uint32_t face = 0, trackSkel = 0, tracks = 0, trackName = 0, trackSize = 0;
    bool ok = false;
} g_mo;
struct TrackNames {
    std::vector<std::string> names;
    std::vector<const char*> ptrs;
};
std::unordered_map<void*, TrackNames> g_trackNames;  // floatTrackSkeleton -> its track names (under g_skelMx)
bool g_mimicInstalled = false;
std::atomic<uint64_t> g_mimicCalls{0}, g_mimicFired{0};  // for the `mimic` command

void FindMimicOffsets() {  // main thread
    if (g_mo.ok) return;
    void* mc = FindClassByName("CMimicComponent");
    void* mf = FindClassByName("CMimicFace");
    void* sk = FindClassByName("CSkeleton");
    void* tr = RttiFindType("SSkeletonTrack");
    void* pFace = mc ? FindProperty(mc, "mimicFace") : nullptr;
    void* pSkel = mf ? FindProperty(mf, "floatTrackSkeleton") : nullptr;
    void* pTracks = sk ? FindProperty(sk, "tracks") : nullptr;
    void* pName = tr ? FindProperty(tr, "nameAsCName") : nullptr;
    if (!pFace || !pSkel || !pTracks || !pName) {
        Log("posehook: mimic track names stay off (fields not found: %d%d%d%d)", !!pFace, !!pSkel, !!pTracks, !!pName);
        return;
    }
    GRead(static_cast<char*>(pFace) + 0x20, &g_mo.face, 4);
    GRead(static_cast<char*>(pSkel) + 0x20, &g_mo.trackSkel, 4);
    GRead(static_cast<char*>(pTracks) + 0x20, &g_mo.tracks, 4);
    GRead(static_cast<char*>(pName) + 0x20, &g_mo.trackName, 4);
    g_mo.trackSize = RttiSize(tr);
    g_mo.ok = g_mo.trackSize > 0;
}

void* HandleObj(void* holder, uint32_t off) {  // the object of a handle field ({refs, flags, object})
    void* block = nullptr;
    void* obj = nullptr;
    if (!holder || !GRead(static_cast<char*>(holder) + off, &block, 8) || !block) return nullptr;
    if (!GRead(static_cast<char*>(block) + 0x08, &obj, 8)) return nullptr;
    return obj;
}

const TrackNames* TracksOf(void* comp) {
    if (!g_mo.ok) return nullptr;
    void* skel = HandleObj(HandleObj(comp, g_mo.face), g_mo.trackSkel);
    if (!skel) return nullptr;
    std::lock_guard<std::mutex> lk(g_skelMx);
    auto it = g_trackNames.find(skel);
    if (it != g_trackNames.end()) return &it->second;
    char* arr = nullptr;
    uint32_t n = 0;
    if (!GRead(static_cast<char*>(skel) + g_mo.tracks, &arr, 8) || !GRead(static_cast<char*>(skel) + g_mo.tracks + 8, &n, 4) ||
        !arr || n == 0 || n > 1024)
        return nullptr;
    TrackNames& t = g_trackNames[skel];
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t id = 0;
        GRead(arr + static_cast<size_t>(i) * g_mo.trackSize + g_mo.trackName, &id, 4);
        const char* s = GetCNameString(id);
        t.names.emplace_back(s ? s : "");
    }
    for (const std::string& s : t.names) t.ptrs.push_back(s.c_str());
    return &t;
}

void FireMimic(TW3SE_MimicCallback cb, void* ctx, TW3SE_Mimic* m) {
    g_cbInFlight.fetch_add(1);
    __try {
        cb(m->entity, m, ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_mimicCbs.Remove(cb))  // dropped like the pose callback
            Log("posehook: the mimic callback %p crashed (0x%08lx) and was removed", cb, GetExceptionCode());
    }
    g_cbInFlight.fetch_sub(1);
}

// --- faces from Lua (w3.face): Lua runs on the main thread and the face is sampled on the animation threads,
// so Lua leaves the weights here by track name and the mimic hook writes them into every frame of that face: in
// place of the game's value, or on top of it. The names find their track
// indexes once per CMimicComponent.
struct FaceOverride {
    uint32_t guid[4];
    std::string owner;  // the Lua mod that set it (taken out with it)
    std::vector<std::string> names;
    std::vector<float> weights;
    bool add = false;
    void* comp = nullptr;    // the component the indexes belong to
    std::vector<int> index;  // per name: its track, -1 when the face has no such track
};
std::mutex g_faceMx;
std::unordered_map<void*, FaceOverride> g_faces;  // the watched entity -> its weights
std::atomic<int> g_faceCount{0};

void ApplyFace(const WatchInfo& who, void* comp, float* weights, uint32_t count) {  // an animation thread
    std::lock_guard<std::mutex> lk(g_faceMx);
    auto it = g_faces.find(who.entity);
    if (it == g_faces.end() || memcmp(it->second.guid, who.guid, sizeof who.guid) != 0) return;
    FaceOverride& f = it->second;
    if (f.comp != comp) {
        const TrackNames* t = TracksOf(comp);
        if (!t) return;
        f.index.assign(f.names.size(), -1);
        for (size_t k = 0; k < f.names.size(); ++k)
            for (size_t i = 0; i < t->names.size(); ++i)
                if (_stricmp(t->names[i].c_str(), f.names[k].c_str()) == 0) {
                    f.index[k] = static_cast<int>(i);
                    break;
                }
        f.comp = comp;
    }
    for (size_t k = 0; k < f.index.size(); ++k) {
        int i = f.index[k];
        if (i < 0 || static_cast<uint32_t>(i) >= count) continue;
        weights[i] = f.add ? weights[i] + f.weights[k] : f.weights[k];
    }
}

void MimicAfterSample(void* inst, void* out);

void MimicAfterCpp(void* inst, void* out) {  // like the combine: nothing may leave our code inside the engine job
    try {
        MimicAfterSample(inst, out);
    } catch (...) {
        if (g_guardLogs.fetch_add(1) < 5) Log("posehook: exception in the mimic hook");
    }
}
void MimicAfterGuarded(void* inst, void* out) {
    __try {
        MimicAfterCpp(inst, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_guardLogs.fetch_add(1) < 5) Log("posehook: fault 0x%08lx in the mimic hook", GetExceptionCode());
    }
}

void MimicTrackSample(void* node, void* ctx, void* inst, void* out) {
    SampleFn fn = (*reinterpret_cast<SampleFn**>(node))[0x1b0 / 8];
    fn(node, ctx, inst, out);
    MimicAfterGuarded(inst, out);
}

void MimicAfterSample(void* inst, void* out) {
    g_mimicCalls.fetch_add(1, std::memory_order_relaxed);
    if (g_mimicWatchCount.load(std::memory_order_relaxed) == 0) return;
    void* comp = nullptr;
    void* owner = nullptr;
    float* weights = nullptr;
    uint32_t count = 0;
    if (!GRead(static_cast<char*>(inst) + 0x68, &comp, 8) || !comp) return;
    GRead(static_cast<char*>(comp) + 0x30, &owner, 8);
    WatchInfo who{};
    {
        std::lock_guard<std::mutex> lk(g_watchMx);
        const WatchInfo* w = FindWatch(g_mimicWatch, comp, owner);
        if (!w) return;
        who = *w;
        g_entMimic[who.entity] = comp;  // GetPose(MIMIC) after WatchMimic alone (it needed a WatchPose before)
    }
    bool faces = g_faceCount.load(std::memory_order_relaxed) > 0;
    if (!faces && !g_mimicCbs.Any()) return;
    if (!GRead(static_cast<char*>(out) + 0x08, &weights, 8) || !GRead(static_cast<char*>(out) + 0x1c, &count, 4) ||
        !weights || count == 0 || count > 1024)
        return;
    if (faces) ApplyFace(who, comp, weights, count);  // first: a plugin sees the face as Lua set it
    if (!g_mimicCbs.Any()) return;
    static thread_local std::vector<const char*> names;
    names.assign(count, "");
    if (const TrackNames* t = TracksOf(comp))
        for (uint32_t i = 0; i < count && i < t->ptrs.size(); ++i) names[i] = t->ptrs[i];
    TW3SE_Mimic m{};
    m.entity = who.entity;
    m.component = comp;
    memcpy(m.guid, who.guid, sizeof m.guid);
    m.trackCount = static_cast<int32_t>(count);
    m.trackNames = names.data();
    m.weights = weights;
    g_mimicFired.fetch_add(1, std::memory_order_relaxed);
    Crumb("mimic: callback", reinterpret_cast<uintptr_t>(comp), count);
    DoingScope d("the mimic callback", reinterpret_cast<uintptr_t>(comp));
    for (int i = 0, n = g_mimicCbs.used.load(std::memory_order_acquire); i < n; ++i)  // each plugin's in turn
        if (TW3SE_MimicCallback cb = g_mimicCbs.slot[i].fn.load(std::memory_order_acquire))
            FireMimic(cb, g_mimicCbs.slot[i].ctx.load(std::memory_order_relaxed), &m);
}

}  // namespace
extern "C" LPVOID AllocateBuffer(LPVOID pOrigin);  // MinHook: a block near pOrigin (deps/minhook/src/buffer.c)
namespace {

// The call becomes `call [rip+slot]`, written as one 16-byte compare-exchange: an animation thread running through
// it sees the old or the new instruction, never half of each.
void InstallMimicHook() {
    std::lock_guard<std::mutex> l(g_installMx);
    if (g_mimicInstalled || !g_hookAllowed) return;
    uint8_t* site = reinterpret_cast<uint8_t*>(Address("Mimic_TrackSampleCall"));
    static const uint8_t kCall[6] = {0xFF, 0x90, 0xB0, 0x01, 0x00, 0x00};
    uint8_t* call = site ? site + 29 : nullptr;
    if (!call || memcmp(call, kCall, sizeof kCall) != 0) {
        Log("posehook: mimic hook not installed (the call site is not in this build)");
        return;
    }
    auto* block = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(call) & ~static_cast<uintptr_t>(15));
    size_t at = static_cast<size_t>(call - block);
    if (at + 6 > 16) {
        Log("posehook: mimic hook not installed (the call crosses a 16-byte line)");
        return;
    }
    void** slot = static_cast<void**>(AllocateBuffer(call));
    int64_t rel = slot ? reinterpret_cast<uint8_t*>(slot) - (call + 6) : 0;
    if (!slot || rel != static_cast<int32_t>(rel)) {
        Log("posehook: mimic hook not installed (no memory near the call site)");
        return;
    }
    *slot = reinterpret_cast<void*>(&MimicTrackSample);
    DWORD old = 0;
    if (!VirtualProtect(block, 16, PAGE_EXECUTE_READWRITE, &old)) {
        Log("posehook: mimic hook not installed (VirtualProtect failed)");
        return;
    }
    alignas(16) long long cur[2];
    alignas(16) long long next[2];
    memcpy(cur, block, 16);
    memcpy(next, cur, 16);
    auto* nb = reinterpret_cast<uint8_t*>(next);
    nb[at] = 0xFF;
    nb[at + 1] = 0x15;
    int32_t r32 = static_cast<int32_t>(rel);
    memcpy(nb + at + 2, &r32, 4);
    bool ok = _InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(block), next[1], next[0], cur) != 0;
    VirtualProtect(block, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), block, 16);
    g_mimicInstalled = ok;
    Log("posehook: mimic hook %s (call at %p, slot %p)", ok ? "installed" : "NOT installed (the code changed)", call, slot);
}
}  // namespace

void MimicRegisterCallback(TW3SE_MimicCallback cb, void* ctx) {
    if (!cb) return;
    if (!g_mimicCbs.Add(cb, ctx)) Log("posehook: mimic callback %p refused (%d plugins have one already)", cb, g_mimicCbs.kSlots);
    else InstallMimicHook();
}

int MimicWatch(void* entity, int on) { return MimicWatchBy(entity, on, 1); }

int MimicWatchBy(void* entity, int on, int by) {
    // An entity or its CMimicComponent, on the main thread (like WatchPose). Each watcher has its bit, as in
    // PoseWatchBy: 1 the plugin API, 4 w3.face.set, 8 w3.face.tracks (a face set from Lua leaves a
    // plugin's watch).
    if (!entity) return 0;
    if (!on) {
        std::lock_guard<std::mutex> lk(g_watchMx);
        auto it = g_mimicWatch.find(entity);
        if (it != g_mimicWatch.end() && (it->second.by &= ~by) == 0) g_mimicWatch.erase(it);
        g_mimicWatchCount.store(static_cast<int>(g_mimicWatch.size()), std::memory_order_relaxed);
        CountItemWatchers();
        return 1;
    }
    {
        std::lock_guard<std::mutex> lk(g_watchMx);
        auto it = g_mimicWatch.find(entity);
        if (it != g_mimicWatch.end()) {
            it->second.by |= by;
            return 1;
        }
    }
    WatchInfo who{};
    who.entity = entity;
    who.by = by;
    WatchTarget(entity, &who);
    FindMimicOffsets();
    FindSkelOffsets();  // GetPose(MIMIC) works on a face watched this way too
    bool fresh;
    {
        std::lock_guard<std::mutex> lk(g_watchMx);
        auto it = g_mimicWatch.find(entity);
        fresh = it == g_mimicWatch.end();
        if (!fresh) who.by |= it->second.by;  // another watcher came in meanwhile
        g_mimicWatch[entity] = who;
        g_mimicWatchCount.store(static_cast<int>(g_mimicWatch.size()), std::memory_order_relaxed);
        CountItemWatchers();
    }
    if (fresh) Log("posehook: watching the face of %p (entity %p%s)", entity, who.entity, who.items ? ", with its items" : "");
    InstallMimicHook();
    return 1;
}

// w3.face (main thread): the weights by track name, in place of the game's values or on top of them (add). The face
// is watched while it has them; an empty list takes them off.
int FaceSet(void* entity, const std::vector<std::pair<std::string, float>>& weights, bool add, const std::string& owner) {
    if (!entity) return 0;
    if (weights.empty()) return FaceClear(entity);
    if (!MimicWatchBy(entity, 1, 4)) return 0;
    WatchInfo who{};
    {
        std::lock_guard<std::mutex> lk(g_watchMx);
        auto it = g_mimicWatch.find(entity);
        if (it == g_mimicWatch.end()) return 0;
        who = it->second;
    }
    FaceOverride f;
    memcpy(f.guid, who.guid, sizeof f.guid);
    f.owner = owner;
    f.add = add;
    for (const auto& [name, w] : weights) {
        f.names.push_back(name);
        f.weights.push_back(w);
    }
    std::lock_guard<std::mutex> lk(g_faceMx);
    g_faces[who.entity] = std::move(f);
    g_faceCount.store(static_cast<int>(g_faces.size()), std::memory_order_relaxed);
    return 1;
}

int FaceClear(void* entity) {
    void* key = entity;
    {
        std::lock_guard<std::mutex> lk(g_watchMx);
        auto it = g_mimicWatch.find(entity);
        if (it != g_mimicWatch.end()) key = it->second.entity;
    }
    {
        std::lock_guard<std::mutex> lk(g_faceMx);
        g_faces.erase(key);
        g_faceCount.store(static_cast<int>(g_faces.size()), std::memory_order_relaxed);
    }
    return MimicWatchBy(entity, 0, 4);
}

int FaceClearOwner(const std::string& owner) {  // a Lua mod is taken out: its faces go back to the game
    std::vector<void*> gone;
    {
        std::lock_guard<std::mutex> lk(g_faceMx);
        for (const auto& [e, f] : g_faces)
            if (f.owner == owner) gone.push_back(e);
    }
    for (void* e : gone) FaceClear(e);
    return static_cast<int>(gone.size());
}

// The face's track names, once its mimic component has been sampled: the first call watches the face and returns
// nothing, a call a few frames later has them. That watch (bit 8) ends 5 s after the last call (FaceExpire, from
// Lua's tick), the way w3.pose's does: asking for the names leaves nothing behind (fuzz_lua).
std::unordered_map<void*, ULONGLONG> g_faceAsked;  // main thread
std::vector<std::string> FaceTracks(void* entity) {
    MimicWatchBy(entity, 1, 8);
    g_faceAsked[entity] = GetTickCount64();
    void* comp = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_watchMx);
        auto w = g_mimicWatch.find(entity);
        void* e = w != g_mimicWatch.end() ? w->second.entity : entity;
        auto it = g_entMimic.find(e);
        if (it != g_entMimic.end()) comp = it->second;
    }
    const TrackNames* t = comp ? TracksOf(comp) : nullptr;
    return t ? t->names : std::vector<std::string>{};
}

void FaceExpire() {  // main thread, each tick
    if (g_faceAsked.empty()) return;
    ULONGLONG now = GetTickCount64();
    for (auto it = g_faceAsked.begin(); it != g_faceAsked.end();) {
        if (now - it->second < 5000) {
            ++it;
            continue;
        }
        MimicWatchBy(it->first, 0, 8);
        it = g_faceAsked.erase(it);
    }
}

// A plugin is unloaded: its pose and mimic callbacks are taken out; returns once none of them runs any more.
void PoseWatchCounts(int* pose, int* mimic) {
    *pose = g_watchCount.load(std::memory_order_relaxed);
    *mimic = g_mimicWatchCount.load(std::memory_order_relaxed);
}

// RegisterOnPose(NULL) / RegisterOnMimic(NULL): the calling plugin's callbacks go, another plugin's stay.
int PoseCallbacksOffIn(uintptr_t lo, uintptr_t hi) { return g_poseCbs.RemoveIn(lo, hi); }
int MimicCallbacksOffIn(uintptr_t lo, uintptr_t hi) { return g_mimicCbs.RemoveIn(lo, hi); }

int DropPoseCallbacksIn(uintptr_t lo, uintptr_t hi) {
    int n = g_poseCbs.RemoveIn(lo, hi) + g_mimicCbs.RemoveIn(lo, hi);
    for (int i = 0; i < 2000 && g_cbInFlight.load() > 0; ++i) Sleep(1);
    return n;
}

// The `mimic` console command: whether the hook is in, how often the converter ran and the callback fired.
std::string MimicStats() {
    char b[200];
    snprintf(b, sizeof b, "mimic hook %s, %llu converter call(s), %llu callback(s), %d watched, %d callback(s) registered",
             g_mimicInstalled ? "on" : "off", static_cast<unsigned long long>(g_mimicCalls.load()),
             static_cast<unsigned long long>(g_mimicFired.load()), g_mimicWatchCount.load(),
             g_mimicCbs.Count());
    return b;
}

// Sampling an animation and laying it on a figure (for a scene preview, docs/engine/animation.md
// "Sampling an animation and laying a pose"): the engine's own sampler, every compression and the streamed buffers
// it has in memory. A pose is laid in local space just before the model-space step, so children follow their parents
// and every mesh, dangle and scene sees it.
namespace {
using FindAnim_t = char* (*)(void* container, const uint32_t* name);
using IsLoaded_t = uint32_t (*)(void* anim, uint32_t bones, uint32_t* available, uint32_t* resident);
using Sample_t = bool (*)(void* anim, float t, uint32_t bones, uint32_t tracks, float* out, float* trackOut);
using BonesMS_t = uint64_t (*)(char* output, void* skeleton, char* out, uint32_t maxBones);
BonesMS_t oBonesMS = nullptr;

struct PoseLayer {
    void* anim;
    float t, weight;
};
std::mutex g_layerMx;
std::unordered_map<void*, std::shared_ptr<const std::vector<PoseLayer>>> g_layers;  // by component
std::atomic<int> g_layerCount{0};
std::atomic<uint64_t> g_layerFrames{0};

void* SkeletonPtr(void* comp) {
    void* block = nullptr;
    void* skel = nullptr;
    if (!GRead(static_cast<char*>(comp) + g_so.skeleton, &block, 8) || !block) return nullptr;
    return GRead(static_cast<char*>(block) + 0x08, &skel, 8) ? skel : nullptr;
}

// The animation of that name among the component's sets, or 0 (and why).
void* FindAnimationGuarded(FindAnim_t find, void* cont, uint32_t name) {
    __try {
        char* entry = find(cont, &name);
        return entry ? *reinterpret_cast<void**>(entry + 0x20) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
// Without the container's own lookup (its signature does not match every build): its set list from the end, the way
// the lookup walks it, through the sets' reflected fields. The set added last wins, as there.
uint32_t PropOffset(const char* cls, const char* prop, uint32_t fallback) {
    void* c = FindClassByName(cls);
    void* p = c ? FindProperty(c, prop) : nullptr;
    uint32_t off = fallback;
    if (p) GRead(static_cast<char*>(p) + 0x20, &off, 4);
    return off;
}
void* WalkFindAnimation(void* cont, uint32_t id) {
    static const uint32_t anims = PropOffset("CSkeletalAnimationSet", "animations", 0x98);
    static const uint32_t entryAnim = PropOffset("CSkeletalAnimationSetEntry", "animation", 0x20);
    static const uint32_t animName = PropOffset("CSkeletalAnimation", "name", 0x18);
    DArr sets{};
    if (!GRead(static_cast<char*>(cont) + 0x58, &sets, sizeof sets) || !sets.data || sets.count > 4096) return nullptr;
    std::vector<void*> blocks(sets.count);
    if (!GRead(sets.data, blocks.data(), blocks.size() * 8)) return nullptr;
    std::vector<void*> entries;
    for (size_t i = blocks.size(); i-- > 0;) {
        void* set = nullptr;
        DArr list{};
        if (!blocks[i] || !GRead(static_cast<char*>(blocks[i]) + 0x08, &set, 8) || !set ||
            !GRead(static_cast<char*>(set) + anims, &list, sizeof list) || !list.data || list.count > 65536)
            continue;
        entries.resize(list.count);
        if (!GRead(list.data, entries.data(), entries.size() * 8)) continue;
        for (void* e : entries) {
            void* anim = nullptr;
            uint32_t n = 0;
            if (e && GRead(static_cast<char*>(e) + entryAnim, &anim, 8) && anim &&
                GRead(static_cast<char*>(anim) + animName, &n, 4) && n == id)
                return anim;
        }
    }
    return nullptr;
}

// --- animations made at run time (w3.anim.create): an engine animation built in memory for one rig. A
// CSkeletalAnimation whose animBuffer is a CAnimationBufferUncompressed, both made by their classes (TTypedClass slot
// 10 constructs an object in place) in the engine's memory, so CSkeletalAnimation::Sample plays it with the engine's
// own code (it calls the buffer's slot 42, which reads two whole frames through slot 40 and blends them). The
// uncompressed buffer (measured in its slot 40, 6adfbd7a): +0x10 frames, +0x14 bones, +0x18 float tracks, +0x1c
// floats a frame; then {pointer, count} arrays of 12 bytes: +0x20 one byte a value (1: the next value of the
// constant array), +0x2c the frames, +0x38 the constant values, +0x44 the tracks. A bone is 10 values: position xyz,
// rotation xyzw, scale xyz. Bones the source does not name keep the figure's own pose (the mask below).
struct CustomAnim {
    void* skel;
    uint32_t name;
    void* anim;
    std::vector<uint8_t> carry;  // per rig bone: 1 = the animation moves it
};
std::mutex g_customMx;
std::vector<std::shared_ptr<const CustomAnim>> g_custom;

std::shared_ptr<const CustomAnim> CustomFor(void* skel, uint32_t name) {
    std::lock_guard<std::mutex> l(g_customMx);
    for (auto it = g_custom.rbegin(); it != g_custom.rend(); ++it)
        if ((*it)->skel == skel && (*it)->name == name) return *it;
    return nullptr;
}
std::shared_ptr<const CustomAnim> CustomByAnim(void* anim) {
    std::lock_guard<std::mutex> l(g_customMx);
    for (auto& c : g_custom)
        if (c->anim == anim) return c;
    return nullptr;
}

using Construct_t = void (*)(void* cls, void* mem);
bool ConstructGuarded(Construct_t ctor, void* cls, void* mem) {
    __try {
        ctor(cls, mem);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// An object of a native class in the engine's memory, made by the class itself (its TTypedClass, slot 10).
void* NewNative(const char* className, std::string* err) {
    void* cls = RttiFindType(className);
    uint32_t size = cls ? RttiSize(cls) : 0;
    void* vt = nullptr;
    Construct_t ctor = nullptr;
    int construct = cls ? RttiSlot(cls, 10) : -1;  // the class's construct (11 in 5.02)
    if (!cls || !size || construct < 0 || !GRead(cls, &vt, 8) || !vt ||
        !GRead(static_cast<void**>(vt) + construct, &ctor, 8) || !ctor) {
        *err = std::string("the class ") + className + " cannot be made";
        return nullptr;
    }
    void* mem = EngineAlloc(size, 16);
    if (!mem) {
        *err = "no engine memory";
        return nullptr;
    }
    memset(mem, 0, size);
    if (!ConstructGuarded(ctor, cls, mem)) {
        *err = std::string("making a ") + className + " faulted";
        return nullptr;
    }
    return mem;
}

// The rig's bone names and its rest pose (48 bytes a bone), "" on success.
std::string RigOf(void* skel, std::vector<std::string>* names, std::vector<float>* rest) {
    if (!g_so.ok) return "the skeleton's fields are not known for this game build";
    char* bones = nullptr;
    const char* ls = nullptr;
    uint32_t nb = 0, nls = 0;
    GRead(static_cast<char*>(skel) + g_so.bones, &bones, 8);
    GRead(static_cast<char*>(skel) + g_so.bones + 8, &nb, 4);
    GRead(static_cast<char*>(skel) + FieldOr("CSkeleton.referencePoseLS", 0x90), &ls, 8);
    GRead(static_cast<char*>(skel) + FieldOr("CSkeleton.boneCount", 0x70), &nls, 4);
    if (!bones || !ls || nb == 0 || nb > 2048 || nls != nb) return "the rig's bones cannot be read";
    names->assign(nb, "");
    for (uint32_t i = 0; i < nb; ++i) {
        uint32_t id = 0;
        GRead(bones + static_cast<size_t>(i) * g_so.boneSize + g_so.boneName, &id, 4);
        const char* s = GetCNameString(id);
        (*names)[i] = s ? s : "";
    }
    rest->assign(static_cast<size_t>(nb) * 12, 0.0f);
    if (!GRead(ls, rest->data(), rest->size() * 4)) return "the rig's rest pose cannot be read";
    return "";
}

}  // namespace

// w3.anim.create: an animation `name` for the component's rig from `frames` x `boneNames` records of 10 floats
// (position xyz, rotation xyzw, local to the parent, scale xyz), at `fps`. Replaces one of the same name on the rig.
std::string AnimCreate(void* comp, const char* name, float fps, const std::vector<std::string>& boneNames,
                       const float* data, uint32_t frames, uint32_t* mapped) {
    FindSkelOffsets();  // (main thread: w3.anim.create is called from Lua)
    void* skel = SkeletonPtr(comp);
    if (!skel) return "the figure has no rig";
    if (!(fps > 0) || frames == 0) return "fps and at least one frame are needed";
    std::vector<std::string> rig;
    std::vector<float> rest;
    std::string err = RigOf(skel, &rig, &rest);
    if (!err.empty()) return err;
    uint32_t n = static_cast<uint32_t>(rig.size());
    std::vector<int32_t> to(boneNames.size(), -1);  // source bone -> rig bone
    auto c = std::make_shared<CustomAnim>();
    c->carry.assign(n, 0);
    *mapped = 0;
    for (size_t i = 0; i < boneNames.size(); ++i) {
        auto it = std::find(rig.begin(), rig.end(), boneNames[i]);
        if (it == rig.end()) continue;
        to[i] = static_cast<int32_t>(it - rig.begin());
        if (!c->carry[to[i]]) ++*mapped;
        c->carry[to[i]] = 1;
    }
    if (!*mapped) return "no bone of the animation is in the figure's rig";
    size_t stride = static_cast<size_t>(n) * 10;
    float* flat = static_cast<float*>(EngineAlloc(stride * frames * 4, 16));
    uint8_t* flags = static_cast<uint8_t*>(EngineAlloc(stride, 16));
    if (!flat || !flags) return "no engine memory";
    memset(flags, 0, stride);  // every value from the frames
    for (uint32_t f = 0; f < frames; ++f) {
        float* fr = flat + stride * f;
        for (uint32_t b = 0; b < n; ++b) {  // the rest pose for what the source does not name
            const float* r = rest.data() + static_cast<size_t>(b) * 12;
            float* o = fr + static_cast<size_t>(b) * 10;
            o[0] = r[0], o[1] = r[1], o[2] = r[2];
            o[3] = r[4], o[4] = r[5], o[5] = r[6], o[6] = r[7];
            o[7] = r[8], o[8] = r[9], o[9] = r[10];
        }
        const float* src = data + boneNames.size() * 10 * f;
        for (size_t i = 0; i < boneNames.size(); ++i)
            if (to[i] >= 0)
                for (int k = 0; k < 10; ++k)  // NaN: the rest pose's value (a rotation alone keeps the bone's place)
                    if (!std::isnan(src[i * 10 + k])) fr[static_cast<size_t>(to[i]) * 10 + k] = src[i * 10 + k];
    }
    void* buffer = NewNative("CAnimationBufferUncompressed", &err);
    if (!buffer) return err;
    void* anim = NewNative("CSkeletalAnimation", &err);
    if (!anim) return err;
    auto put32 = [](void* obj, uint32_t off, uint32_t v) { memcpy(static_cast<char*>(obj) + off, &v, 4); };
    auto putPtr = [](void* obj, uint32_t off, const void* p) { memcpy(static_cast<char*>(obj) + off, &p, 8); };
    put32(buffer, 0x10, frames);
    put32(buffer, 0x14, n);
    put32(buffer, 0x18, 0);
    put32(buffer, 0x1c, static_cast<uint32_t>(stride));
    putPtr(buffer, 0x20, flags);
    put32(buffer, 0x28, static_cast<uint32_t>(stride));
    putPtr(buffer, 0x2c, flat);
    put32(buffer, 0x34, static_cast<uint32_t>(stride * frames));
    uint32_t nameId = InternName(name);
    float duration = frames > 1 ? (frames - 1) / fps : 0.0f;
    put32(anim, FieldOr("CSkeletalAnimation.name", 0x18), nameId);
    putPtr(anim, FieldOr("CSkeletalAnimation.animBuffer", 0x20), buffer);
    memcpy(static_cast<char*>(anim) + 0x28, &fps, 4);       // framesPerSecond
    memcpy(static_cast<char*>(anim) + 0x2c, &duration, 4);  // duration
    c->skel = skel;
    c->name = nameId;
    c->anim = anim;
    std::lock_guard<std::mutex> l(g_customMx);
    g_custom.erase(std::remove_if(g_custom.begin(), g_custom.end(),
                                  [&](const std::shared_ptr<const CustomAnim>& x) {
                                      return x->skel == skel && x->name == nameId;
                                  }),
                   g_custom.end());  // (the old one's memory stays: a pose layer may still sample it this frame)
    g_custom.push_back(c);
    Log("anim: %s made for a rig of %u bones (%u of the animation's %zu in it), %u frames at %.1f fps", name, n,
        *mapped, boneNames.size(), frames, fps);
    return "";
}

namespace {
using SetPostLoad_t = void (*)(void* set);
uintptr_t g_postLoadFault = 0;  // where the last caught fault of a set's set-up was, for the log
int PostLoadFilter(EXCEPTION_POINTERS* ep) {
    g_postLoadFault = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
bool PostLoadGuarded(SetPostLoad_t fn, void* set) {
    __try {
        fn(set);
        return true;
    } __except (PostLoadFilter(GetExceptionInformation())) {
        return false;
    }
}
}  // namespace

// The made animation `name` of the component's rig into a fresh CSkeletalAnimationSet (w3.new), so the engine's own
// lookup by name finds it (slot animations, scenes, the behavior graph) once the set is in the figure's container.
// A set entry (+0x18 its set, +0x20 its animation; the animation's +0x10 its entry) in the set's `animations`, then
// the set's slot 7, which adds each entry to the global table name -> entries (measured in 6adfbd7a: the head of
// the name's chain goes to entry+0x48, the entry becomes the head). A set may only go through slot 7 once (twice
// would chain an entry to itself), so the set has to be empty.
std::string AnimPublish(void* set, void* comp, const char* name) {
    FindSkelOffsets();
    auto c = CustomFor(SkeletonPtr(comp), InternName(name));
    if (!c) return std::string("no made animation ") + name + " on this figure's rig";
    uint32_t animsOff = PropOffset("CSkeletalAnimationSet", "animations", 0x98), count = 0;
    GRead(static_cast<char*>(set) + animsOff + 8, &count, 4);
    if (count) return "the set has animations already (use a new one)";
    std::string why;
    auto postLoad = reinterpret_cast<SetPostLoad_t>(EngineResolve("CSkeletalAnimationSet::vf7", &why));
    if (!postLoad) return why;
    std::string err;
    void* entry = NewNative("CSkeletalAnimationSetEntry", &err);
    if (!entry) return err;
    void** arr = static_cast<void**>(EngineAlloc(8, 8));
    if (!arr) return "no engine memory";
    arr[0] = entry;
    char* e = static_cast<char*>(entry);
    memcpy(e + 0x18, &set, 8);
    memcpy(e + PropOffset("CSkeletalAnimationSetEntry", "animation", 0x20), &c->anim, 8);
    memcpy(static_cast<char*>(c->anim) + 0x10, &entry, 8);
    uint32_t one = 1;
    memcpy(static_cast<char*>(set) + animsOff, &arr, 8);
    memcpy(static_cast<char*>(set) + animsOff + 8, &one, 4);
    memcpy(static_cast<char*>(set) + animsOff + 12, &one, 4);
    if (!PostLoadGuarded(postLoad, set)) {
        // the set lets go of the entry again: kept, the engine would destroy a half set-up entry with the set later
        // (5.02, 26.11.: the game crashed in the set's destructor at the next load)
        void* none = nullptr;
        uint32_t zero = 0;
        memcpy(static_cast<char*>(set) + animsOff, &none, 8);
        memcpy(static_cast<char*>(set) + animsOff + 8, &zero, 4);
        memcpy(static_cast<char*>(set) + animsOff + 12, &zero, 4);
        memcpy(static_cast<char*>(c->anim) + 0x10, &none, 8);
        std::string at = AddressPlace(g_postLoadFault);
        Log("anim: %s: the set's set-up faulted at %s (caught), the set stays empty", name, at.c_str());
        return "the set's set-up faulted (caught)";
    }
    Log("anim: %s is in the engine's table of animations", name);
    return "";
}

namespace {

void* AnimationOf(void* comp, const char* name, std::string* err) {
    if (auto c = CustomFor(SkeletonPtr(comp), InternName(name))) return c->anim;  // w3.anim.create
    auto find = reinterpret_cast<FindAnim_t>(Address("CSkeletalAnimationContainer::FindAnimation"));
    if (!Address("CSkeletalAnimation::Sample") || !Address("CSkeletalAnimation::IsLoaded")) {
        *err = "sampling animations is not known for this game build";
        return nullptr;
    }
    void* cont = nullptr;
    GRead(static_cast<char*>(comp) + FieldOr("CAnimatedComponent.animationContainer", PoseOff(0xe80)), &cont, 8);
    uint32_t id = InternName(name);
    void* anim = nullptr;
    if (cont && id) anim = find ? FindAnimationGuarded(find, cont, id) : WalkFindAnimation(cont, id);
    void* buffer = nullptr;
    if (!anim) {
        *err = std::string("the figure has no animation ") + name;
    } else if (!GRead(static_cast<char*>(anim) + FieldOr("CSkeletalAnimation.animBuffer", 0x20), &buffer, 8) || !buffer) {
        *err = std::string(name) + " has no animation data";
        anim = nullptr;
    }
    return anim;
}

// 1 sampled, 0 nothing in memory yet, -1 a fault. bones is filled with what the animation does not carry.
int SampleGuarded(void* anim, float t, uint32_t n, float* bones) {
    auto isLoaded = reinterpret_cast<IsLoaded_t>(Address("CSkeletalAnimation::IsLoaded"));
    auto sample = reinterpret_cast<Sample_t>(Address("CSkeletalAnimation::Sample"));
    __try {
        uint32_t avail = 0, resident = 0;
        uint32_t state = isLoaded(anim, n, &avail, &resident);
        if (state == 0) return 0;
        uint32_t count = state == 2 ? n : (avail < n ? avail : n);
        if (!count) return 0;
        float duration = *reinterpret_cast<float*>(static_cast<char*>(anim) + 0x2c);
        if (t > duration) t = duration;
        if (!(t > 0)) t = 0;
        float track = 0;
        sample(anim, t, count, 0, bones, &track);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// a moves towards b by f: position and scale linear, the rotation linear and normalised on b's side
void Blend(float* a, const float* b, uint32_t n, float f) {
    for (uint32_t i = 0; i < n; ++i, a += 12, b += 12) {
        for (int k = 0; k < 4; ++k) a[k] += (b[k] - a[k]) * f;
        float dot = a[4] * b[4] + a[5] * b[5] + a[6] * b[6] + a[7] * b[7];
        float s = dot < 0 ? -1.0f : 1.0f;
        float len = 0;
        for (int k = 4; k < 8; ++k) {
            a[k] += (s * b[k] - a[k]) * f;
            len += a[k] * a[k];
        }
        if (len > 1e-12f) {
            len = 1.0f / sqrtf(len);
            for (int k = 4; k < 8; ++k) a[k] *= len;
        }
        for (int k = 8; k < 12; ++k) a[k] += (b[k] - a[k]) * f;
    }
}

void ApplyLayers(char* output, const std::vector<PoseLayer>& layers) {  // C++ objects here; the caller guards
    float* pose = nullptr;
    uint32_t n = 0;
    GRead(output, &pose, 8);
    GRead(output + FieldOr("SBehaviorGraphOutput.numBones", 0x18), &n, 4);
    if (!pose || n == 0 || n > 512) return;
    static thread_local std::vector<float> sample, mix;
    sample.resize(static_cast<size_t>(n) * 12);
    mix.resize(static_cast<size_t>(n) * 12);
    float sum = 0;
    for (const PoseLayer& l : layers) {
        if (!(l.weight > 0)) continue;
        memcpy(sample.data(), pose, static_cast<size_t>(n) * 48);  // bones the animation lacks keep the graph's
        if (SampleGuarded(l.anim, l.t, n, sample.data()) != 1) continue;
        if (auto c = CustomByAnim(l.anim))  // a made animation: the bones it does not name keep the graph's pose
            for (uint32_t b = 0; b < n; ++b)
                if (b >= c->carry.size() || !c->carry[b])
                    memcpy(sample.data() + static_cast<size_t>(b) * 12, pose + static_cast<size_t>(b) * 12, 48);
        sum += l.weight;
        if (sum == l.weight) memcpy(mix.data(), sample.data(), static_cast<size_t>(n) * 48);
        else Blend(mix.data(), sample.data(), n, l.weight / sum);
    }
    if (sum > 0) Blend(pose, mix.data(), n, sum < 1 ? sum : 1);  // below 1 the graph's pose fills the rest
    g_layerFrames.fetch_add(1, std::memory_order_relaxed);
}
void ApplyLayersCpp(char* output, const std::vector<PoseLayer>* layers) {  // nothing may leave TW3SE inside the job
    try {
        ApplyLayers(output, *layers);
    } catch (...) {
    }
}
void ApplyLayersGuarded(char* output, const std::vector<PoseLayer>* layers) {
    __try {
        ApplyLayersCpp(output, layers);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
// --- recording a figure's pose (w3.anim.record): each frame the local pose the step gets (after the layers), as 10
// floats a bone (position xyz, rotation xyzw, scale xyz) with the time; on stop resampled to a fixed rate.
struct PoseRec {
    uint32_t n = 0;
    std::vector<float> values;     // frames x n x 10
    std::vector<double> times;     // seconds since the start
    LARGE_INTEGER start{};
    size_t maxFrames = 0;
};
std::mutex g_recMx;
std::unordered_map<void*, PoseRec> g_recs;  // by component
std::atomic<int> g_recCount{0};
double g_qpcFreq = 0;

void RecordFrame(char* comp, char* output) {  // C++ objects here; the caller guards
    float* pose = nullptr;
    uint32_t n = 0;
    GRead(output, &pose, 8);
    GRead(output + FieldOr("SBehaviorGraphOutput.numBones", 0x18), &n, 4);
    if (!pose || n == 0 || n > 512) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    std::lock_guard<std::mutex> lk(g_recMx);
    auto it = g_recs.find(comp);
    if (it == g_recs.end()) return;
    PoseRec& r = it->second;
    if (r.n == 0) r.n = n;
    if (n != r.n || r.times.size() >= r.maxFrames) return;
    size_t at = r.values.size();
    r.values.resize(at + static_cast<size_t>(n) * 10);
    float* o = r.values.data() + at;
    for (uint32_t b = 0; b < n; ++b, o += 10) {
        const float* q = pose + static_cast<size_t>(b) * 12;
        o[0] = q[0], o[1] = q[1], o[2] = q[2];
        o[3] = q[4], o[4] = q[5], o[5] = q[6], o[6] = q[7];
        o[7] = q[8], o[8] = q[9], o[9] = q[10];
    }
    r.times.push_back(static_cast<double>(now.QuadPart - r.start.QuadPart) / g_qpcFreq);
}
void RecordFrameCpp(char* comp, char* output) {
    try {
        RecordFrame(comp, output);
    } catch (...) {
    }
}
void RecordFrameGuarded(char* comp, char* output) {
    __try {
        RecordFrameCpp(comp, output);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// --- a figure's proportions (w3.anim.shape): per bone a scale, an offset and a turn on its local pose, every frame
// after the layers. Children follow (their place is in the bone's space), and so does every mesh skinned to them.
// The engine's step to model space passes a bone's scale on to its children's size but not to their places
// (measured, _scratch/anim_scale_probe.py: the pelvis at 1.5 made the head 1.46 times as big and left it where it
// was). So "inherit" moves the bones below apart by the scales above them (places only; their size grows by the
// engine), and "only" keeps the scale on the bone itself: its direct children get 1/scale.
struct BoneShape {
    uint32_t bone;
    float scale[3], offset[3], rot[4];  // rot: a quaternion xyzw applied after the bone's own turn
    float length;                       // the bone's place times this: its distance to the parent
};
struct ShapeSet {
    std::vector<BoneShape> bones;
    std::vector<float> chain;    // per rig bone xyz: its place times the inherited scales above it (empty: none)
    std::vector<float> counter;  // per rig bone xyz: its own scale times this ("only" on its parent; empty: none)
};
std::mutex g_shapeMx;
std::unordered_map<void*, std::shared_ptr<const ShapeSet>> g_shapes;  // by component
std::atomic<int> g_shapeCount{0};

void ApplyShape(char* output, const ShapeSet& set) {
    float* pose = nullptr;
    uint32_t n = 0;
    GRead(output, &pose, 8);
    GRead(output + FieldOr("SBehaviorGraphOutput.numBones", 0x18), &n, 4);
    if (!pose || n == 0 || n > 512) return;
    if (set.chain.size() >= static_cast<size_t>(n) * 3)
        for (uint32_t b = 0; b < n; ++b) {
            const float* c = set.chain.data() + static_cast<size_t>(b) * 3;
            if (c[0] == 1 && c[1] == 1 && c[2] == 1) continue;
            float* q = pose + static_cast<size_t>(b) * 12;
            for (int k = 0; k < 3; ++k) q[k] *= c[k];
        }
    if (set.counter.size() >= static_cast<size_t>(n) * 3)
        for (uint32_t b = 0; b < n; ++b) {
            const float* c = set.counter.data() + static_cast<size_t>(b) * 3;
            if (c[0] == 1 && c[1] == 1 && c[2] == 1) continue;
            float* q = pose + static_cast<size_t>(b) * 12;
            for (int k = 0; k < 3; ++k) q[8 + k] *= c[k];
        }
    for (const BoneShape& s : set.bones) {
        if (s.bone >= n) continue;
        float* q = pose + static_cast<size_t>(s.bone) * 12;
        for (int k = 0; k < 3; ++k) q[k] = q[k] * s.length + s.offset[k];
        float ax = q[4], ay = q[5], az = q[6], aw = q[7], bx = s.rot[0], by = s.rot[1], bz = s.rot[2], bw = s.rot[3];
        q[4] = aw * bx + ax * bw + ay * bz - az * by;  // own turn, then the shape's
        q[5] = aw * by - ax * bz + ay * bw + az * bx;
        q[6] = aw * bz + ax * by - ay * bx + az * bw;
        q[7] = aw * bw - ax * bx - ay * by - az * bz;
        for (int k = 0; k < 3; ++k) q[8 + k] *= s.scale[k];
    }
}
void ApplyShapeGuarded(char* output, const ShapeSet* shape) {
    __try {
        ApplyShape(output, *shape);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// w3.anim.reach (24.11.): an arm reaches a point in the world. A two-bone IK on bicep, forearm and hand, laid on the
// local pose before the model-space step: the elbow bends until the grip point (`ahead` from the wrist toward the
// middle finger, `out` from the palm) is as far from the shoulder as the target, then the upper arm turns it onto
// the target. The elbow stays in the plane the animation gives it. Weight 0..1 blends from the animation's arm.
struct Reach {
    char hand;                            // 'l' or 'r'
    int upper, mid, end, mid1, mid3, index1, pinky1;
    std::shared_ptr<const std::vector<int16_t>> parents;
    float target[3];  // world
    float weight, ahead, out;
};
std::mutex g_reachMx;
std::unordered_map<void*, std::shared_ptr<const Reach>> g_reaches;  // by component
std::atomic<int> g_reachCount{0};
std::atomic<int> g_reachTransposed{-1};  // the engine's quaternion convention: -1 not known yet
void ApplyReachGuarded(char* output, char* comp, const Reach* r);

uint64_t BonesMSHook(char* output, void* skeleton, char* out, uint32_t maxBones) {
    bool layers0 = g_layerCount.load(std::memory_order_relaxed) > 0, rec = g_recCount.load(std::memory_order_relaxed) > 0,
         shaped = g_shapeCount.load(std::memory_order_relaxed) > 0,
         reaching = g_reachCount.load(std::memory_order_relaxed) > 0;
    if ((layers0 || rec || shaped || reaching) && output && out) {
        char* comp = out - FieldOr("CAnimatedComponent.bonesMS", PoseOff(0xddc));
        if (layers0) {
            std::shared_ptr<const std::vector<PoseLayer>> layers;
            {
                std::lock_guard<std::mutex> lk(g_layerMx);
                auto it = g_layers.find(comp);
                if (it != g_layers.end()) layers = it->second;
            }
            if (layers) ApplyLayersGuarded(output, layers.get());
        }
        if (shaped) {
            std::shared_ptr<const ShapeSet> shape;
            {
                std::lock_guard<std::mutex> lk(g_shapeMx);
                auto it = g_shapes.find(comp);
                if (it != g_shapes.end()) shape = it->second;
            }
            if (shape) ApplyShapeGuarded(output, shape.get());
        }
        if (reaching) {
            std::shared_ptr<const Reach> reach;
            {
                std::lock_guard<std::mutex> lk(g_reachMx);
                auto it = g_reaches.find(comp);
                if (it != g_reaches.end()) reach = it->second;
            }
            if (reach) ApplyReachGuarded(output, comp, reach.get());
        }
        if (rec) RecordFrameGuarded(comp, output);
    }
    return oBonesMS(output, skeleton, out, maxBones);
}

std::string EnsureBonesHook() {
    if (oBonesMS) return "";
    void* target = reinterpret_cast<void*>(Address("SBehaviorGraphOutput::GetBonesModelSpace"));
    if (!target) return "laying a pose is not known for this game build";
    if (!Hook(target, reinterpret_cast<void*>(&BonesMSHook), reinterpret_cast<void**>(&oBonesMS)))
        return "the model-space step could not be hooked";
    Log("posehook: animation pose hook in");
    return "";
}

using ToMatrixQs_t = void* (*)(const void* qs, float* out);
bool ToMatrixGuarded(ToMatrixQs_t f, const float* qs, float* out) {
    __try {
        f(qs, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::unordered_map<void*, GetBuf> g_sampleBufs;  // by component, under g_getMx

// --- w3.anim.reach's arithmetic. Model matrices are the engine's (ToMatrix, row vectors: child = local x parent,
// translation in the last row); a rotation built here is a column-vector 3x3 R, which turns a model matrix's rows
// (its axes) as M' = M x R^T.
struct V3 {
    float x, y, z;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return sqrtf(Dot(a, a)); }
V3 Unit(V3 a) {
    float l = Len(a);
    return l > 1e-8f ? a * (1.0f / l) : V3{0, 0, 0};
}
float AngleBetween(V3 a, V3 b) {
    float d = Dot(Unit(a), Unit(b));
    return acosf(d < -1 ? -1 : d > 1 ? 1 : d);
}
void AxisAngle(V3 k, float ang, float R[9]) {  // column-vector rotation about unit k
    float c = cosf(ang), s = sinf(ang), t = 1 - c;
    R[0] = c + t * k.x * k.x;       R[1] = t * k.x * k.y - s * k.z; R[2] = t * k.x * k.z + s * k.y;
    R[3] = t * k.x * k.y + s * k.z; R[4] = c + t * k.y * k.y;       R[5] = t * k.y * k.z - s * k.x;
    R[6] = t * k.x * k.z - s * k.y; R[7] = t * k.y * k.z + s * k.x; R[8] = c + t * k.z * k.z;
}
V3 Apply(const float R[9], V3 v) {
    return {R[0] * v.x + R[1] * v.y + R[2] * v.z, R[3] * v.x + R[4] * v.y + R[5] * v.z, R[6] * v.x + R[7] * v.y + R[8] * v.z};
}
void Rot3(const float* m16, float r[9]) {  // a model matrix's rotation rows, without scale
    for (int i = 0; i < 3; ++i) {
        V3 row = Unit({m16[i * 4], m16[i * 4 + 1], m16[i * 4 + 2]});
        r[i * 3] = row.x, r[i * 3 + 1] = row.y, r[i * 3 + 2] = row.z;
    }
}
void MulRowsByRT(const float M[9], const float R[9], float out[9]) {  // out = M x R^T
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            out[i * 3 + j] = M[i * 3] * R[j * 3] + M[i * 3 + 1] * R[j * 3 + 1] + M[i * 3 + 2] * R[j * 3 + 2];
}
void MulABT(const float A[9], const float B[9], float out[9]) {  // out = A x B^T (B a rotation: its inverse)
    MulRowsByRT(A, B, out);
}
void QuatToStd(const float q[4], float R[9]) {  // the usual column-vector matrix of a quaternion xyzw
    float x = q[0], y = q[1], z = q[2], w = q[3];
    R[0] = 1 - 2 * (y * y + z * z); R[1] = 2 * (x * y - z * w);     R[2] = 2 * (x * z + y * w);
    R[3] = 2 * (x * y + z * w);     R[4] = 1 - 2 * (x * x + z * z); R[5] = 2 * (y * z - x * w);
    R[6] = 2 * (x * z - y * w);     R[7] = 2 * (y * z + x * w);     R[8] = 1 - 2 * (x * x + y * y);
}
void StdToQuat(const float R[9], float q[4]) {
    float tr = R[0] + R[4] + R[8];
    if (tr > 0) {
        float s = sqrtf(tr + 1) * 2;
        q[3] = 0.25f * s, q[0] = (R[7] - R[5]) / s, q[1] = (R[2] - R[6]) / s, q[2] = (R[3] - R[1]) / s;
    } else if (R[0] > R[4] && R[0] > R[8]) {
        float s = sqrtf(1 + R[0] - R[4] - R[8]) * 2;
        q[3] = (R[7] - R[5]) / s, q[0] = 0.25f * s, q[1] = (R[1] + R[3]) / s, q[2] = (R[2] + R[6]) / s;
    } else if (R[4] > R[8]) {
        float s = sqrtf(1 + R[4] - R[0] - R[8]) * 2;
        q[3] = (R[2] - R[6]) / s, q[0] = (R[1] + R[3]) / s, q[1] = 0.25f * s, q[2] = (R[5] + R[7]) / s;
    } else {
        float s = sqrtf(1 + R[8] - R[0] - R[4]) * 2;
        q[3] = (R[3] - R[1]) / s, q[0] = (R[2] + R[6]) / s, q[1] = (R[5] + R[7]) / s, q[2] = 0.25f * s;
    }
}

void ApplyReach(char* output, char* comp, const Reach& r) {
    float* pose = nullptr;
    uint32_t n = 0;
    GRead(output, &pose, 8);
    GRead(output + FieldOr("SBehaviorGraphOutput.numBones", 0x18), &n, 4);
    const std::vector<int16_t>& par = *r.parents;
    if (!pose || n == 0 || n > 512 || par.size() < n || r.weight <= 0) return;
    int top = (std::max)({r.upper, r.mid, r.end, r.mid1, r.mid3, r.index1, r.pinky1});
    if (static_cast<uint32_t>(top) >= n) return;
    auto toMatrix = reinterpret_cast<ToMatrixQs_t>(Address("EngineQsTransform::ToMatrix"));
    void* entity = nullptr;
    float m2w[16];
    if (!toMatrix || !GRead(comp + 0x30, &entity, 8) || !entity || !GRead(static_cast<char*>(entity) + 0x70, m2w, 64)) return;
    // the model matrices up to the last bone needed (a parent comes before its child)
    std::vector<float> model(static_cast<size_t>(top + 1) * 16), local(static_cast<size_t>(top + 1) * 16);
    for (int b = 0; b <= top; ++b) {
        float* L = local.data() + static_cast<size_t>(b) * 16;
        if (!ToMatrixGuarded(toMatrix, pose + static_cast<size_t>(b) * 12, L)) return;
        int p = par[b];
        float* dst = model.data() + static_cast<size_t>(b) * 16;
        if (p >= 0 && p < b) Mul(L, model.data() + static_cast<size_t>(p) * 16, dst);
        else memcpy(dst, L, 64);
    }
    auto pos = [&](int b) {
        const float* m = model.data() + static_cast<size_t>(b) * 16;
        return V3{m[12], m[13], m[14]};
    };
    // the target in model space (m2w: model X, Y, Z axes in its first three rows, the place in the last)
    V3 tw{r.target[0] - m2w[12], r.target[1] - m2w[13], r.target[2] - m2w[14]};
    V3 ax{m2w[0], m2w[1], m2w[2]}, ay{m2w[4], m2w[5], m2w[6]}, az{m2w[8], m2w[9], m2w[10]};
    V3 T{Dot(tw, ax) / Dot(ax, ax), Dot(tw, ay) / Dot(ay, ay), Dot(tw, az) / Dot(az, az)};
    // the grip point
    V3 H = pos(r.end), F = Unit(pos(r.mid1) - H), S = Unit(pos(r.index1) - pos(r.pinky1));
    V3 N = Unit(Cross(F, S));
    if (Dot(N, pos(r.mid3) - pos(r.mid1)) < 0) N = N * -1.0f;
    V3 A = pos(r.upper), B = pos(r.mid), C = H + F * r.ahead + N * r.out;
    float a = Len(B - A), b = Len(C - B);
    if (a < 1e-4f || b < 1e-4f) return;
    float c = Len(T - A), lo = fabsf(a - b) + 1e-3f, hi = a + b - 1e-3f;
    c = c < lo ? lo : c > hi ? hi : c;
    float w = r.weight > 1 ? 1 : r.weight;
    // 1: the elbow to the right angle
    float R1[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    V3 u = A - B, v = C - B, k = Cross(u, v);
    if (Len(k) > 1e-6f) {
        float cosB = (a * a + b * b - c * c) / (2 * a * b);
        float want = acosf(cosB < -1 ? -1 : cosB > 1 ? 1 : cosB), now = AngleBetween(u, v);
        AxisAngle(Unit(k), (want - now) * w, R1);
    }
    V3 C1 = B + Apply(R1, C - B);
    // 2: the upper arm onto the target
    float R2[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    V3 d1 = C1 - A, d2 = T - A, k2 = Cross(d1, d2);
    if (Len(k2) > 1e-6f) AxisAngle(Unit(k2), AngleBetween(d1, d2) * w, R2);
    // the new local turns: the forearm's in the old upper arm's frame, the upper arm's in its parent's
    float Mmid[9], Mup[9], Mpar[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, t[9], Lmid[9], Lup[9];
    Rot3(model.data() + static_cast<size_t>(r.mid) * 16, Mmid);
    Rot3(model.data() + static_cast<size_t>(r.upper) * 16, Mup);
    if (par[r.upper] >= 0) Rot3(model.data() + static_cast<size_t>(par[r.upper]) * 16, Mpar);
    MulRowsByRT(Mmid, R1, t);
    MulABT(t, Mup, Lmid);
    MulRowsByRT(Mup, R2, t);
    MulABT(t, Mpar, Lup);
    // the engine's quaternion of a local matrix: its ToMatrix gives the usual matrix or its transpose. Decided once,
    // on the most turned bone of the chain's way (a turn near zero looks the same either way)
    if (g_reachTransposed.load() < 0) {
        float best = 0;
        int pick = -1;
        for (int b = 0; b <= top; ++b) {
            float R[9];
            QuatToStd(pose + static_cast<size_t>(b) * 12 + 4, R);
            float asym = fabsf(R[1] - R[3]) + fabsf(R[2] - R[6]) + fabsf(R[5] - R[7]);
            if (asym > best) best = asym, pick = b;
        }
        if (pick < 0 || best < 0.3f) return;
        float Rstd[9], Eng[9];
        QuatToStd(pose + static_cast<size_t>(pick) * 12 + 4, Rstd);
        Rot3(local.data() + static_cast<size_t>(pick) * 16, Eng);
        float same = 0, trans = 0;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) same += fabsf(Eng[i * 3 + j] - Rstd[i * 3 + j]), trans += fabsf(Eng[i * 3 + j] - Rstd[j * 3 + i]);
        g_reachTransposed = trans < same ? 1 : 0;
        Log("posehook: reach - the engine's rotation matrix is the %s one (bone %d, off %.3f / %.3f)", trans < same ? "transposed" : "usual", pick, same, trans);
    }
    auto write = [&](int bone, const float L[9]) {
        float* q = pose + static_cast<size_t>(bone) * 12 + 4;
        float nq[4];
        bool transposed = g_reachTransposed.load() == 1;
        float M[9];
        if (transposed)
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) M[i * 3 + j] = L[j * 3 + i];
        else
            memcpy(M, L, sizeof M);
        StdToQuat(M, nq);
        float len = sqrtf(nq[0] * nq[0] + nq[1] * nq[1] + nq[2] * nq[2] + nq[3] * nq[3]);
        if (len < 1e-6f) return;
        float sgn = (nq[0] * q[0] + nq[1] * q[1] + nq[2] * q[2] + nq[3] * q[3]) < 0 ? -1.0f : 1.0f;
        for (int k2i = 0; k2i < 4; ++k2i) q[k2i] = nq[k2i] / len * sgn;
    };
    write(r.mid, Lmid);
    write(r.upper, Lup);
}
void ApplyReachGuarded(char* output, char* comp, const Reach* r) {
    __try {
        ApplyReach(output, comp, *r);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
}  // namespace

// w3.anim.sample (lua.cpp): animation `name` of the component's sets at second t, in the component's model space like
// w3.pose. 0 with err set; 0 with err empty when its data is not in memory yet (a later frame). Main thread.
int PoseSample(void* comp, const char* name, float t, TW3SE_Pose* out, float* duration, std::string* err) {
    err->clear();
    FindSkelOffsets();
    if (!comp || !IsScriptable(comp) || !DerivesFromGuarded(comp, "CAnimatedComponent")) {
        *err = "an animated figure or component is needed";
        return 0;
    }
    void* anim = AnimationOf(comp, name, err);
    if (!anim) return 0;
    void* skel = SkeletonPtr(comp);
    const SkelInfo* sk = skel ? SkelInfoOf(skel, true) : nullptr;
    auto toMatrix = reinterpret_cast<ToMatrixQs_t>(Address("EngineQsTransform::ToMatrix"));
    uint32_t n = 0;
    const char* rest = nullptr;
    const int16_t* par = nullptr;
    if (skel) {
        GRead(static_cast<char*>(skel) + FieldOr("CSkeleton.boneCount", 0x70), &n, 4);
        GRead(static_cast<char*>(skel) + FieldOr("CSkeleton.referencePoseLS", 0x90), &rest, 8);
        GRead(static_cast<char*>(skel) + 0x80, &par, 8);
    }
    if (!sk || !toMatrix || !rest || !par || n == 0 || n > 512 || sk->parents.size() < n) {
        *err = "the figure's skeleton could not be read";
        return 0;
    }
    GRead(static_cast<char*>(anim) + 0x2c, duration, 4);
    std::vector<float> ls(static_cast<size_t>(n) * 12);
    std::vector<int16_t> parents(n);
    if (!GRead(rest, ls.data(), ls.size() * 4) || !GRead(par, parents.data(), static_cast<size_t>(n) * 2)) {
        *err = "the figure's skeleton could not be read";
        return 0;
    }
    int r = SampleGuarded(anim, t, n, ls.data());
    if (r < 0) *err = "the sampler faulted inside the game (caught)";
    if (r != 1) return 0;
    void* entity = nullptr;
    GRead(static_cast<char*>(comp) + 0x30, &entity, 8);
    std::lock_guard<std::mutex> lk(g_getMx);
    GetBuf& gb = g_sampleBufs[comp];
    gb.model.resize(static_cast<size_t>(n) * 16);
    for (uint32_t b = 0; b < n; ++b) {  // a parent comes before its child
        float m[16];
        if (!ToMatrixGuarded(toMatrix, ls.data() + static_cast<size_t>(b) * 12, m)) {
            *err = "the sampler faulted inside the game (caught)";
            return 0;
        }
        int16_t p = parents[b];
        float* dst = gb.model.data() + static_cast<size_t>(b) * 16;
        if (p >= 0 && static_cast<uint32_t>(p) < b) Mul(m, gb.model.data() + static_cast<size_t>(p) * 16, dst);
        else memcpy(dst, m, 64);
    }
    if (!entity || !GRead(static_cast<char*>(entity) + 0x70, gb.m2w, sizeof gb.m2w)) memset(gb.m2w, 0, sizeof gb.m2w);
    *out = TW3SE_Pose{};
    out->entity = entity;
    out->component = comp;
    out->kind = TW3SE_POSE_OTHER;
    out->boneCount = static_cast<int32_t>(n);
    out->boneNames = sk->ptrs.size() >= n ? sk->ptrs.data() : nullptr;
    out->parents = sk->parents.data();
    out->local = gb.model.data();
    out->localStride = 16;
    out->modelToWorld = gb.m2w;
    return 1;
}

// w3.anim.pose (lua.cpp): from the next frame on the component shows the blend of the layers {name, t, weight};
// empty takes them off. Main thread.
std::string PoseLayersSet(void* comp, const std::vector<std::tuple<std::string, float, float>>& layers) {
    FindSkelOffsets();
    if (!comp || !IsScriptable(comp) || !DerivesFromGuarded(comp, "CAnimatedComponent"))
        return "an animated figure or component is needed";
    auto list = std::make_shared<std::vector<PoseLayer>>();
    for (const auto& l : layers) {
        std::string err;
        void* anim = AnimationOf(comp, std::get<0>(l).c_str(), &err);
        if (!anim) return err;
        list->push_back({anim, std::get<1>(l), std::get<2>(l)});
    }
    if (!list->empty()) {
        std::string e = EnsureBonesHook();
        if (!e.empty()) return e;
    }
    std::lock_guard<std::mutex> lk(g_layerMx);
    if (list->empty()) g_layers.erase(comp);
    else g_layers[comp] = std::move(list);
    g_layerCount.store(static_cast<int>(g_layers.size()), std::memory_order_relaxed);
    return "";
}
// w3.anim.shape: the figure's bones by name with {scale xyz, offset xyz, rot xyzw}; empty takes the shape off. Main
// thread. A bone the rig lacks is an error.
// The bone names of an animated component's rig (a figure's, a head's mimic rig), in rig order.
std::string AnimBoneNames(void* comp, std::vector<std::string>* names) {
    FindSkelOffsets();
    if (!comp || !IsScriptable(comp) || !DerivesFromGuarded(comp, "CAnimatedComponent"))
        return "an animated figure or component is needed";
    void* skel = SkeletonPtr(comp);
    if (!skel) return "the component has no skeleton";
    std::vector<float> rest;
    return RigOf(skel, names, &rest);
}

std::string AnimShapeSet(void* comp, const std::vector<std::pair<std::string, std::array<float, 12>>>& bones) {
    FindSkelOffsets();
    if (!comp || !IsScriptable(comp) || !DerivesFromGuarded(comp, "CAnimatedComponent"))
        return "an animated figure or component is needed";
    auto list = std::make_shared<ShapeSet>();
    if (!bones.empty()) {
        void* skel = SkeletonPtr(comp);
        std::vector<std::string> rig;
        std::vector<float> rest;
        std::string e = RigOf(skel, &rig, &rest);
        if (!e.empty()) return e;
        size_t n = rig.size();
        std::vector<float> passOn(n * 3, 1.0f);  // what each bone passes on to the bones below it
        std::vector<int> only;                   // bones whose scale stays on themselves
        bool inherit = false;
        for (const auto& b : bones) {
            auto it = std::find(rig.begin(), rig.end(), b.first);
            if (it == rig.end()) return "the rig has no bone " + b.first;
            BoneShape s{static_cast<uint32_t>(it - rig.begin())};
            memcpy(s.scale, &b.second[0], 12);
            memcpy(s.offset, &b.second[3], 12);
            memcpy(s.rot, &b.second[6], 16);
            s.length = b.second[10];
            if (b.second[11] == 1) {
                inherit = true;
                memcpy(&passOn[static_cast<size_t>(s.bone) * 3], s.scale, 12);
            } else if (b.second[11] == 2) {
                only.push_back(static_cast<int>(s.bone));
            }
            list->bones.push_back(s);
        }
        std::vector<int16_t> parents;
        if (inherit || !only.empty()) {
            int16_t* par = nullptr;
            uint32_t np = 0;
            GRead(static_cast<char*>(skel) + g_so.parents, &par, 8);
            GRead(static_cast<char*>(skel) + g_so.parents + 8, &np, 4);
            parents.resize(np);
            if (!par || np != n || !GRead(par, parents.data(), np * 2)) return "the rig's parents cannot be read";
        }
        if (!only.empty()) {
            list->counter.assign(n * 3, 1.0f);
            for (int o : only) {
                const BoneShape* s = nullptr;
                for (const BoneShape& x : list->bones)
                    if (static_cast<int>(x.bone) == o) s = &x;
                for (size_t c = 0; c < n && s; ++c)
                    if (parents[c] == o)
                        for (int k = 0; k < 3; ++k)
                            list->counter[c * 3 + k] *= s->scale[k] != 0 ? 1.0f / s->scale[k] : 1.0f;
            }
        }
        if (inherit) {  // the parents come before their children in the rig
            list->chain.assign(n * 3, 1.0f);
            std::vector<float> below(n * 3, 1.0f);  // the scale a bone hands to its children: chain x its own pass-on
            for (size_t b = 0; b < n; ++b) {
                int p = parents[b];
                for (int k = 0; k < 3; ++k) {
                    if (p >= 0 && static_cast<size_t>(p) < b) list->chain[b * 3 + k] = below[static_cast<size_t>(p) * 3 + k];
                    below[b * 3 + k] = list->chain[b * 3 + k] * passOn[b * 3 + k];
                }
            }
        }
        e = EnsureBonesHook();
        if (!e.empty()) return e;
    }
    std::lock_guard<std::mutex> lk(g_shapeMx);
    if (list->bones.empty()) g_shapes.erase(comp);
    else g_shapes[comp] = std::move(list);
    g_shapeCount.store(static_cast<int>(g_shapes.size()));
    return "";
}

// w3.anim.reach: hand 'l' or 'r' reaches target (world) with weight (0 or no hand: off); ahead and out place the grip
// point in the hand (metres). Main thread. A second call for the same hand only moves the target and the weight.
std::string AnimReachSet(void* comp, char hand, const float* target, float weight, float ahead, float out) {
    FindSkelOffsets();
    if (!comp || !IsScriptable(comp) || !DerivesFromGuarded(comp, "CAnimatedComponent"))
        return "an animated figure or component is needed";
    if (hand != 'l' && hand != 'r') {
        std::lock_guard<std::mutex> lk(g_reachMx);
        g_reaches.erase(comp);
        g_reachCount.store(static_cast<int>(g_reaches.size()));
        return "";
    }
    std::shared_ptr<const Reach> was;
    {
        std::lock_guard<std::mutex> lk(g_reachMx);
        auto it = g_reaches.find(comp);
        if (it != g_reaches.end() && it->second->hand == hand) was = it->second;
    }
    auto r = std::make_shared<Reach>();
    if (was) {
        *r = *was;
    } else {
        void* skel = SkeletonPtr(comp);
        std::vector<std::string> rig;
        std::vector<float> rest;
        std::string e = RigOf(skel, &rig, &rest);
        if (!e.empty()) return e;
        auto find = [&](const char* name, int* at) {
            std::string full = std::string(1, hand) + "_" + name;
            auto it = std::find(rig.begin(), rig.end(), full);
            *at = it == rig.end() ? -1 : static_cast<int>(it - rig.begin());
            return *at >= 0;
        };
        r->hand = hand;
        if (!find("bicep", &r->upper) || !find("forearm", &r->mid) || !find("hand", &r->end) ||
            !find("middle1", &r->mid1) || !find("middle3", &r->mid3) || !find("index1", &r->index1) ||
            !find("pinky1", &r->pinky1))
            return "the rig lacks an arm's bones (bicep, forearm, hand, middle1, middle3, index1, pinky1)";
        uint32_t n = 0;
        const int16_t* par = nullptr;
        GRead(static_cast<char*>(skel) + FieldOr("CSkeleton.boneCount", 0x70), &n, 4);
        GRead(static_cast<char*>(skel) + 0x80, &par, 8);
        if (!par || n == 0 || n > 512 || n < rig.size()) return "the figure's skeleton could not be read";
        auto parents = std::make_shared<std::vector<int16_t>>(n);
        if (!GRead(par, parents->data(), static_cast<size_t>(n) * 2)) return "the figure's skeleton could not be read";
        if ((*parents)[r->end] != r->mid || (*parents)[r->mid] != r->upper)
            return "the arm is no chain bicep - forearm - hand in this rig";
        r->parents = parents;
        std::string e2 = EnsureBonesHook();
        if (!e2.empty()) return e2;
    }
    memcpy(r->target, target, 12);
    r->weight = weight;
    r->ahead = ahead;
    r->out = out;
    std::lock_guard<std::mutex> lk(g_reachMx);
    if (weight <= 0) g_reaches.erase(comp);
    else g_reaches[comp] = std::move(r);
    g_reachCount.store(static_cast<int>(g_reaches.size()));
    return "";
}

// w3.anim.record: from the next frame on the component's pose is written down (at most maxSeconds at 120 frames a
// second). Main thread.
std::string AnimRecordStart(void* comp, float maxSeconds) {
    FindSkelOffsets();
    if (!comp || !IsScriptable(comp) || !DerivesFromGuarded(comp, "CAnimatedComponent"))
        return "an animated figure or component is needed";
    std::string e = EnsureBonesHook();
    if (!e.empty()) return e;
    if (g_qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcFreq = static_cast<double>(f.QuadPart);
    }
    std::lock_guard<std::mutex> lk(g_recMx);
    PoseRec& r = g_recs[comp];
    r = PoseRec();
    QueryPerformanceCounter(&r.start);
    r.maxFrames = static_cast<size_t>((maxSeconds > 0 ? maxSeconds : 60.0f) * 120.0f) + 1;
    g_recCount.store(static_cast<int>(g_recs.size()));
    return "";
}

// Stops it and gives the recording at `fps`: the rig's bone names and frames x bones x 10 floats (each value linear
// between the two frames around its time; the rotation normalised on the nearer side). Main thread.
std::string AnimRecordStop(void* comp, float fps, std::vector<std::string>* bones, std::vector<float>* out) {
    PoseRec r;
    {
        std::lock_guard<std::mutex> lk(g_recMx);
        auto it = g_recs.find(comp);
        if (it == g_recs.end()) return "the figure is not being recorded";
        r = std::move(it->second);
        g_recs.erase(it);
        g_recCount.store(static_cast<int>(g_recs.size()));
    }
    if (r.times.size() < 2) return "fewer than two frames were recorded";
    std::vector<float> rest;
    std::string e = RigOf(SkeletonPtr(comp), bones, &rest);
    if (!e.empty()) return e;
    if (bones->size() != r.n) bones->resize(r.n);  // (the pose's bones are the rig's, in its order)
    if (!(fps > 0)) fps = 30;
    double length = r.times.back();
    size_t frames = static_cast<size_t>(length * fps) + 1, stride = static_cast<size_t>(r.n) * 10, k = 0;
    out->assign(frames * stride, 0.0f);
    for (size_t f = 0; f < frames; ++f) {
        double t = f / static_cast<double>(fps);
        while (k + 2 < r.times.size() && r.times[k + 1] < t) ++k;
        double span = r.times[k + 1] - r.times[k];
        float w = span > 0 ? static_cast<float>((t - r.times[k]) / span) : 0.0f;
        w = w < 0 ? 0 : w > 1 ? 1 : w;
        const float* a = r.values.data() + k * stride;
        const float* b = a + stride;
        float* o = out->data() + f * stride;
        for (size_t bone = 0; bone < r.n; ++bone, a += 10, b += 10, o += 10) {
            for (int c = 0; c < 10; ++c) o[c] = a[c] + (b[c] - a[c]) * w;
            float dot = a[3] * b[3] + a[4] * b[4] + a[5] * b[5] + a[6] * b[6], len = 0;
            for (int c = 3; c < 7; ++c) {
                o[c] = a[c] + ((dot < 0 ? -b[c] : b[c]) - a[c]) * w;
                len += o[c] * o[c];
            }
            if (len > 1e-12f)
                for (int c = 3; c < 7; ++c) o[c] /= sqrtf(len);
        }
    }
    Log("anim: recorded %zu frames over %.2f s, %u bones, %zu frames at %.0f fps", r.times.size(), length, r.n, frames, fps);
    return "";
}

std::string PoseLayerStats() {
    char b[96];
    snprintf(b, sizeof b, "%d figure(s) posed, %llu frame(s)", g_layerCount.load(),
             static_cast<unsigned long long>(g_layerFrames.load()));
    return b;
}

}  // namespace tw3se
