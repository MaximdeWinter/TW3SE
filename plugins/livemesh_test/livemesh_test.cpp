// Test of the live meshes (interface 26): a mesh component's copy whose vertices this plugin writes every tick.
// Lua drives it:
//   w3.plugin.lm_open(<component address>, <TWDB file under tw3se\>)  the base positions and normals from
//       w3.deform.dump (taken before), then LiveMeshOpen; answers the id
//   w3.plugin.lm_amp(<metres>)   how far the vertices swing (a wave along the mesh's height), 0 = the base
//   w3.plugin.lm_lift(<metres>)  added to every z (the test mesh rests 5 m below its root)
//   w3.plugin.lm_stats()         state, writes, microseconds a tick in LiveMeshWrite, the tick time
//   w3.plugin.lm_close()
//   w3.plugin.lm_tex(<depot path of a 512 x 512 texture> | '')  TextureWrite every tick (moving stripes), '' stops
#include "tw3se/tw3se.hpp"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const TW3SE_Interface* g_api = nullptr;

struct Part {
    uint32_t index, count;
    std::vector<float> pos, nrm, out;
};
static std::vector<Part> g_parts;
static int32_t g_id = 0;
static float g_amp = 0.05f, g_time = 0, g_lift = 0;
static uint64_t g_writes = 0, g_us = 0, g_usMax = 0, g_ticks = 0;
static double g_dtSum = 0, g_dtMax = 0;
static std::string g_lastError;
static std::string g_tex;
static std::vector<uint8_t> g_pix;
static uint64_t g_texWrites = 0, g_texUs = 0;

static uint64_t Micros() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart * 1000000 / f.QuadPart);
}

static std::string TwDir() {
    char path[MAX_PATH] = {};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&TwDir), &self);
    GetModuleFileNameA(self, path, MAX_PATH);
    std::string p = path;
    for (int i = 0; i < 2; ++i) p = p.substr(0, p.find_last_of('\\'));  // ...\tw3se\plugins\x.dll -> ...\tw3se
    return p;
}

static bool ReadTwdb(const std::string& file) {
    g_parts.clear();
    FILE* f = nullptr;
    if (fopen_s(&f, file.c_str(), "rb") || !f) return false;
    uint32_t head[3] = {};
    bool ok = fread(head, 4, 3, f) == 3 && head[0] == 0x42445754;
    for (uint32_t i = 0; ok && i < head[2]; ++i) {
        uint32_t ph[3];
        if (fread(ph, 4, 3, f) != 3) { ok = false; break; }
        Part p{ph[0], ph[2]};
        p.pos.resize(3ull * p.count);
        p.nrm.resize(3ull * p.count);
        p.out.resize(3ull * p.count);
        ok = fread(p.pos.data(), 4, p.pos.size(), f) == p.pos.size() && fread(p.nrm.data(), 4, p.nrm.size(), f) == p.nrm.size();
        g_parts.push_back(std::move(p));
    }
    fclose(f);
    return ok;
}

static void OnTick(float dt, void*) {
    ++g_ticks;
    g_dtSum += dt;
    if (dt > g_dtMax) g_dtMax = dt;
    if (!g_tex.empty()) {
        static float tt = 0;
        tt += dt;
        g_pix.resize(512ull * 512 * 4);
        int shift = static_cast<int>(tt * 120);
        for (int y = 0; y < 512; ++y)
            for (int x = 0; x < 512; ++x) {
                uint8_t* q = &g_pix[4ull * (y * 512 + x)];
                bool on = ((x + shift) / 32) % 2 == 0;
                q[0] = on ? 255 : 30, q[1] = on ? 40 : 200, q[2] = 60, q[3] = static_cast<uint8_t>(255 - y / 2);
            }
        uint64_t t0 = Micros();
        if (g_api->TextureWrite(g_tex.c_str(), g_pix.data(), 512, 512, 1)) ++g_texWrites;
        else g_lastError = g_api->LastError();
        g_texUs += Micros() - t0;
    }
    if (!g_id) return;
    g_time += dt;
    uint64_t t0 = Micros();
    for (Part& p : g_parts) {
        for (uint32_t i = 0; i < p.count; ++i) {
            const float* b = &p.pos[3ull * i];
            float s = g_amp * std::sin(g_time * 4.0f + b[2] * 12.0f);
            p.out[3ull * i + 0] = b[0] + s;
            p.out[3ull * i + 1] = b[1];
            p.out[3ull * i + 2] = b[2] + g_lift;
        }
        if (g_api->LiveMeshWrite(g_id, static_cast<int>(p.index), p.out.data(), p.nrm.data(), static_cast<int>(p.count))) {
            ++g_writes;
            g_lastError.clear();
        } else
            g_lastError = g_api->LastError();
    }
    uint64_t us = Micros() - t0;
    g_us += us;
    if (us > g_usMax) g_usMax = us;
}

static int LuaOpen(int argc, const char* const* argv, char* reply, uint32_t size, void*) {
    if (argc < 2) return snprintf(reply, size, "lm_open(component address, TWDB file)"), 0;
    if (!ReadTwdb(TwDir() + "\\" + argv[1])) return snprintf(reply, size, "cannot read %s", argv[1]), 0;
    void* comp = reinterpret_cast<void*>(_strtoui64(argv[0], nullptr, 10));
    g_id = g_api->LiveMeshOpen(comp);
    if (!g_id) return snprintf(reply, size, "open failed: %s", g_api->LastError()), 0;
    g_writes = g_us = g_usMax = 0;
    snprintf(reply, size, "%d", g_id);
    return 1;
}

static int LuaAmp(int argc, const char* const* argv, char* reply, uint32_t size, void*) {
    if (argc > 0) g_amp = static_cast<float>(atof(argv[0]));
    snprintf(reply, size, "%.3f", g_amp);
    return 1;
}

static int LuaLift(int argc, const char* const* argv, char* reply, uint32_t size, void*) {
    if (argc > 0) g_lift = static_cast<float>(atof(argv[0]));
    snprintf(reply, size, "%.3f", g_lift);
    return 1;
}

static int LuaTex(int argc, const char* const* argv, char* reply, uint32_t size, void*) {
    g_tex = argc > 0 ? argv[0] : "";
    snprintf(reply, size, "%llu writes, %.1f us each in TextureWrite", static_cast<unsigned long long>(g_texWrites),
             g_texWrites ? static_cast<double>(g_texUs) / g_texWrites : 0.0);
    g_texWrites = g_texUs = 0;
    return 1;
}

static int LuaStats(int, const char* const*, char* reply, uint32_t size, void*) {
    int vc = 0, chunks = 0;
    int info = g_id ? g_api->LiveMeshInfo(g_id, g_parts.empty() ? 0 : static_cast<int>(g_parts[0].index), &vc, &chunks) : 0;
    uint64_t verts = 0;
    for (const Part& p : g_parts) verts += p.count;
    snprintf(reply, size,
             "id %d state %d info %d (%d vertices, %d chunks) parts %zu verts %llu writes %llu write us/tick %.1f max %llu "
             "tick ms %.2f max %.2f over %llu ticks err '%s'",
             g_id, g_id ? g_api->LiveMeshState(g_id) : -1, info, vc, chunks, g_parts.size(),
             static_cast<unsigned long long>(verts), static_cast<unsigned long long>(g_writes),
             g_ticks ? static_cast<double>(g_us) / g_ticks : 0.0, static_cast<unsigned long long>(g_usMax),
             g_ticks ? 1000 * g_dtSum / g_ticks : 0.0, 1000 * g_dtMax, static_cast<unsigned long long>(g_ticks),
             g_lastError.c_str());
    g_ticks = 0;
    g_dtSum = g_dtMax = 0;
    g_us = g_usMax = 0;
    return 1;
}

static int LuaClose(int, const char* const*, char* reply, uint32_t size, void*) {
    int r = g_id ? g_api->LiveMeshClose(g_id) : 0;
    snprintf(reply, size, "%d %s", r, r ? "" : g_api->LastError());
    g_id = 0;
    return 1;
}

TW3SE_PLUGIN("LiveMeshTest", "TW3SE tests", 0x00010000, /*minInterface*/ 26)

TW3SE_EXPORT int TW3SE_PluginLoad(const TW3SE_Interface* api) {
    g_api = api;
    tw3se::init(api);
    api->RegisterOnTick(&OnTick, nullptr);
    api->RegisterLuaFunction("lm_open", &LuaOpen, nullptr);
    api->RegisterLuaFunction("lm_amp", &LuaAmp, nullptr);
    api->RegisterLuaFunction("lm_stats", &LuaStats, nullptr);
    api->RegisterLuaFunction("lm_lift", &LuaLift, nullptr);
    api->RegisterLuaFunction("lm_tex", &LuaTex, nullptr);
    api->RegisterLuaFunction("lm_close", &LuaClose, nullptr);
    return 1;
}
