// TW3SE core - the time of a running story scene (for a scene preview; docs/engine/scenes.md "Time
// of a running scene"): where the scene stands in its section, pausing it, and moving it to any second of the section,
// forwards and backwards, through the game's own section player. The player keeps one playing plan per loaded section;
// a section is its elements one after the other. A seek is a tick of the section's state machine with a forced time:
// ahead inside the current element it plays on to the time, anywhere else it restarts the section and runs every event
// up to the time in that one call, so poses, camera, placements and look-ats are those of normal playback.
#include "core.h"

#include <cstring>
#include <string>

namespace tw3se {
namespace {

template <class T> T Get(const void* base, size_t off) {
    T v{};
    SafeRead(static_cast<const char*>(base) + off, &v, sizeof v);
    return v;
}

bool PutByte(uint8_t* at, uint8_t v) {
    __try {
        *at = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

char* CurrentPlan(char* player) {
    int32_t id = Get<int32_t>(player, FieldOr("CStoryScenePlayer.planId", 0x1e4));
    char** plans = Get<char**>(player, FieldOr("CStoryScenePlayer.plans", 0x5d8));
    uint32_t n = Get<uint32_t>(player, FieldOr("CStoryScenePlayer.plansCount", 0x5e0));
    if (id < 0 || !plans || n > 256) return nullptr;
    for (uint32_t i = 0; i < n; ++i) {
        char* p = Get<char*>(plans, i * sizeof(void*));
        if (p && Get<int32_t>(p, 0) == id) return p;
    }
    return nullptr;
}

char* Element(char* plan, uint32_t i) {
    uint32_t n = Get<uint32_t>(plan, FieldOr("StorySceneSectionPlayingPlan.numElements", 0x50));
    if (i >= n || n > 4096) return nullptr;
    char** list = Get<char**>(plan, FieldOr("StorySceneSectionPlayingPlan.elements", 0x54));
    return list ? Get<char*>(list, i * sizeof(void*)) : nullptr;
}

float Start(char* e) { return Get<float>(e, FieldOr("IStorySceneElementInstance.startTime", 0x1c)); }
float Local(char* e) { return Get<float>(e, FieldOr("IStorySceneElementInstance.localTime", 0x18)); }
float Duration(char* e) { return Get<float>(e, FieldOr("IStorySceneElementInstance.duration", 0x20)); }

bool Where(char* player, char** plan, char** elem, float* t, float* length) {
    *plan = CurrentPlan(player);
    if (!*plan) return false;
    int32_t cur = Get<int32_t>(*plan, FieldOr("StorySceneSectionPlayingPlan.currElement", 0x10));
    uint32_t n = Get<uint32_t>(*plan, FieldOr("StorySceneSectionPlayingPlan.numElements", 0x50));
    *elem = cur >= 0 ? Element(*plan, static_cast<uint32_t>(cur)) : nullptr;
    char* last = n ? Element(*plan, n - 1) : nullptr;
    *t = *elem ? Start(*elem) + Local(*elem) : 0;
    *length = last ? Start(last) + Duration(last) : 0;
    return true;
}

// Each event keeps a "started" and a "finished" byte in the plan's instance buffer; cleared before the engine restarts
// the section, so the events that ran already start again (the editor's forced tick does the same).
void ClearEventFlags(char* plan) {
    uint32_t a = FieldOr("CStorySceneEvent.instanceVarA", 0), b = FieldOr("CStorySceneEvent.instanceVarB", 0);
    if (!a || !b) return;
    char** ev = Get<char**>(plan, FieldOr("StorySceneSectionPlayingPlan.events", 0x6c));
    uint32_t n = Get<uint32_t>(plan, FieldOr("StorySceneSectionPlayingPlan.eventsCount", 0x74));
    char* ib = Get<char*>(plan, FieldOr("StorySceneSectionPlayingPlan.instanceBuffer", 0xb0));
    if (!ev || !ib || n > 65536) return;
    uint32_t size = Get<uint32_t>(ib, 0x20);
    uint8_t* data = Get<uint8_t*>(ib, 0x28);
    if (!data) return;
    for (uint32_t i = 0; i < n; ++i) {
        char* e = Get<char*>(ev, i * sizeof(void*));
        if (!e) continue;
        uint32_t va = Get<uint32_t>(e, a), vb = Get<uint32_t>(e, b);
        if (va < size) PutByte(data + va, 0);
        if (vb < size) PutByte(data + vb, 0);
    }
}

using Pause_t = void (*)(void*, uint8_t, uint16_t);
using Process_t = bool (*)(void*, uint32_t, void*);
using Collector_t = void (*)(void*);

bool PauseGuarded(Pause_t f, void* player, bool on) {
    __try {
        f(player, on ? 1 : 0, 0x100);  // a bit of TW3SE's own: the engine uses 2, 4 and 8
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int SeekGuarded(Process_t process, Collector_t construct, Collector_t destruct, void* player, char* state, float t) {
    __try {
        alignas(16) uint8_t ctx[0x440];
        memset(ctx, 0, sizeof ctx);
        construct(ctx + 0x30);
        ctx[0x08] = 1;  // forceTime
        memcpy(ctx + 0x0c, &t, 4);
        state[0] = 1;   // the player's "in its tick"
        bool ok = process(player, 0, ctx);
        state[0] = 0;
        destruct(ctx + 0x30);
        return ok ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        state[0] = 0;
        return -1;
    }
}

}  // namespace

// Where the scene stands: seconds in the current section, its length, the section (a CStorySceneSection*).
std::string SceneTime(void* player, float* t, float* length, void** section) {
    char* plan;
    char* elem;
    if (!player) return "no scene";
    if (Get<uint8_t>(player, FieldOr("CStoryScenePlayer.isStopped", 0x49b))) return "the scene has stopped";
    if (!Where(static_cast<char*>(player), &plan, &elem, t, length)) return "the scene has no section playing";
    *section = Get<void*>(player, FieldOr("CStoryScenePlayer.currentSection", 0x1d0));
    return "";
}

// The elements of the current section, in order: start, duration, time inside, whether it runs (for a
// timeline's line boundaries and to see where a seek stands).
std::string SceneElements(void* player, std::vector<SceneElement>* out, int* current) {
    char* plan;
    char* elem;
    float t, length;
    if (!player) return "no scene";
    if (!Where(static_cast<char*>(player), &plan, &elem, &t, &length)) return "the scene has no section playing";
    *current = Get<int32_t>(plan, FieldOr("StorySceneSectionPlayingPlan.currElement", 0x10));
    uint32_t n = Get<uint32_t>(plan, FieldOr("StorySceneSectionPlayingPlan.numElements", 0x50));
    for (uint32_t i = 0; i < n && i < 4096; ++i) {
        char* e = Element(plan, i);
        if (!e) continue;
        out->push_back({static_cast<int>(i), Start(e), Duration(e), Local(e), Get<uint8_t>(e, 0x25) != 0});
    }
    return "";
}

std::string ScenePause(void* player, bool on) {
    auto pause = reinterpret_cast<Pause_t>(Address("CStoryScenePlayer::Pause"));
    if (!pause) return "pausing a scene is not known for this game build";
    if (!player || !PauseGuarded(pause, player, on)) return "the pause faulted inside the game (caught)";
    return "";
}

std::string SceneSeek(void* player, float t) {
    auto process = reinterpret_cast<Process_t>(Address("CStoryScenePlayer::ProcessState"));
    auto construct = reinterpret_cast<Collector_t>(Address("StorySceneEventsCollector::Construct"));
    auto destruct = reinterpret_cast<Collector_t>(Address("StorySceneEventsCollector::Destruct"));
    if (!process || !construct || !destruct) return "seeking in a scene is not known for this game build";
    char* p = static_cast<char*>(player);
    if (!p) return "no scene";
    if (Get<uint8_t>(p, FieldOr("CStoryScenePlayer.isStopped", 0x49b))) return "the scene has stopped";
    char* state = p + FieldOr("CStoryScenePlayer.internalState", 0x180);
    if (Get<uint8_t>(state, 0) || Get<void*>(state, 0x58)) return "the scene is in its tick or changes its section (try the next tick)";
    char* plan;
    char* elem;
    float now = 0, length = 0;
    if (!Where(p, &plan, &elem, &now, &length) || length <= 0) return "the scene has no section playing";
    if (!(t == t)) return "a number is needed";
    if (t < 0) t = 0;
    if (t > length - 1.0f / 60) t = length - 1.0f / 60;  // at the end the section would finish
    bool ahead = elem && t >= now && t < Start(elem) + Duration(elem);
    if (!ahead) ClearEventFlags(plan);  // the engine restarts the section: every event runs again
    int r = SeekGuarded(process, construct, destruct, player, state, t);
    if (r < 0) return "the seek faulted inside the game (caught)";
    return "";
}

}  // namespace tw3se
