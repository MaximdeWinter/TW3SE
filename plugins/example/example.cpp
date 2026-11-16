// Example TW3SE plugin: the smallest useful one. Build it like tw3se's build.bat does and drop example.dll into
// <game>\bin\x64_dx12\tw3se\plugins\. It logs that it loaded, adds the console command "hello", and - the point -
// registers its own WitcherScript native, proving a plugin can extend the script language:
//     import function ExampleDouble(x : int) : int;
#include "tw3se/tw3se.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static const TW3SE_Interface* g_tw3se = nullptr;

static void Hello(const char* args, char* reply, uint32_t size) {
    snprintf(reply, size, "hello from the example plugin (args: '%s', game build %08x)", args ? args : "",
             g_tw3se->gameBuild);
}

// import function ExampleDouble(x : int) : int;  -> 2 * x, computed in the plugin.
// This file shows the raw C ABI (ReadParam/EndParams/result). For a terser C++ style - and multi-parameter
// natives written as ordinary typed functions - see plugins/template/template.cpp and include/tw3se/tw3se.hpp.
static void ExampleDouble(void* self, void* frame, void* result) {
    (void)self;
    int x = 0;
    g_tw3se->ReadParam(frame, &x);
    g_tw3se->EndParams(frame);
    if (result) *(int*)result = 2 * x;
    g_tw3se->Log("Example", "ExampleDouble(%d) -> %d", x, 2 * x);
}

// "exampleobject <d0> <d1> <d2> <d3> [world.txt action...]": an object of the game by its GUID (interface v12) -
// is it loaded, does its GUID read back the same, and, with an action ("effect fire"), a world edit on it.
static void ExampleObject(const char* args, char* reply, uint32_t size) {
    unsigned g[4] = {0};
    int used = 0;
    if (g_tw3se->interfaceVersion < 12 || !args ||
        sscanf_s(args, "%x %x %x %x %n", &g[0], &g[1], &g[2], &g[3], &used) < 4) {
        snprintf(reply, size, "usage: exampleobject <d0> <d1> <d2> <d3> [action ...] (interface 12)");
        return;
    }
    uint32_t guid[4] = {g[0], g[1], g[2], g[3]}, back[4] = {0};
    void* e = g_tw3se->EntityByGuid(guid);
    int same = e && g_tw3se->EntityGuid(e, back) && back[0] == guid[0] && back[1] == guid[1] &&
               back[2] == guid[2] && back[3] == guid[3];
    int edited = -1;
    const char* action = args + used;
    if (*action) {
        char line[256];
        char verb[32] = {0};
        int vn = 0;
        sscanf_s(action, "%31s %n", verb, (unsigned)sizeof verb, &vn);
        snprintf(line, sizeof line, "%s %08x %08x %08x %08x %s", verb, g[0], g[1], g[2], g[3], action + vn);
        edited = g_tw3se->WorldEdit(line);
    }
    snprintf(reply, size, "%s; guid reads back %s%s", e ? "loaded" : "not loaded", same ? "the same" : "different",
             edited < 0 ? "" : edited ? "; world edit taken" : "; world edit not read");
}

TW3SE_EXPORT int TW3SE_PluginQuery(TW3SE_PluginInfo* info) {
    info->name = "Example";
    info->author = "TW3SE";
    info->version = 0x00010000;
    info->minInterface = 2;   // uses RegisterNative (interface v2)
    return 1;
}

// A function for Lua mods (interface 16): w3.plugin.example_add(2, 3) -> "5". The arguments come as text.
static int LuaAdd(int argc, const char* const* argv, char* reply, uint32_t size, void*) {
    if (argc < 2) {
        snprintf(reply, size, "example_add needs two numbers");
        return 0;  // Lua raises the reply as its error
    }
    snprintf(reply, size, "%g", atof(argv[0]) + atof(argv[1]));
    return 1;
}

// The plugin runs Lua (interface 16): `example_lua <code>` in the console.
static void ExampleLua(const char* args, char* reply, uint32_t size) {
    if (!g_tw3se->RunLua(args, reply, size) && !reply[0]) snprintf(reply, size, "error");
}

// When this DLL was loaded (`example_loaded` in the console): it changes after `plugin reload Example`.
static unsigned long long g_loadedAt = 0;
static void ExampleLoaded(const char*, char* reply, uint32_t size) { snprintf(reply, size, "%llu", g_loadedAt); }

// Optional (interface 17): runs before `plugin reload` frees this DLL. Undo what lives outside TW3SE here (own
// threads, written bones). The return value is how many more ticks the plugin needs: its tick, pose and mimic
// callbacks keep running for them. 0 = it can go at once.
TW3SE_EXPORT int TW3SE_PluginUnload() {
    g_tw3se->Log("Example", "unloading");
    return 0;
}

TW3SE_EXPORT int TW3SE_PluginLoad(const TW3SE_Interface* tw3se) {
    g_tw3se = tw3se;
    g_loadedAt = GetTickCount64();
    tw3se->RegisterCommand("example_loaded", &ExampleLoaded, "example plugin: when it was loaded (changes on reload)");
    tw3se->Log("Example", "loaded, TW3SE interface %u", tw3se->interfaceVersion);
    tw3se->RegisterCommand("hello", &Hello, "example plugin: says hello");
    tw3se->RegisterNative("ExampleDouble", &ExampleDouble, 1);  // a plugin-provided WitcherScript native (1 param)
    if (tw3se->interfaceVersion >= 12) tw3se->RegisterCommand("exampleobject", &ExampleObject, "example plugin: an object of the game by its GUID");
    if (tw3se->interfaceVersion >= 16) {
        tw3se->RegisterLuaFunction("example_add", &LuaAdd, nullptr);
        tw3se->RegisterCommand("example_lua", &ExampleLua, "example plugin: runs Lua through the plugin");
    }
    return 1;
}
