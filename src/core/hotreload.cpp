// TW3SE core - WitcherScript live reload in the running game: change a .ws file, `reloadscripts force`,
// the new code runs without a restart.
//
// What the engine does (measured): at start-up one routine compiles and applies every script,
// CScriptingSystem::CompileAndApplyAll: CompileAllScripts (the files GatherScriptFiles found), prepare (runs only
// while the byte this+0x175 is 0 and sets it to 1), ApplyCompiledScripts (needs 0x175 = 1; relinks the RTTI and
// swaps the new code in, live scriptables get OnScriptReloading / OnScriptReloaded), finish. The REDkit editor's
// "Reload scripts" runs the same chain. The game's own ReloadScripts only compiles and applies (no prepare) and
// fails in the game: there 0x175 is 0 after the start-up pass. So the reload here gathers the files and runs the
// start-up routine again, with the arguments the game gave it at start-up (recorded by a hook on it).
//
// Watched closely: every step writes a line to tw3se.log before and after it, with the state bytes 0x174..0x17f,
// its result and how long it took; a fault writes its address and the game addresses on the stack. The log flushes
// each line, so a crash leaves the last step it reached.
//
//     reloadscripts            the state bytes and what the start-up pass was called with
//     reloadscripts force      reload now (main thread, end of the game tick)
#include "core.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace tw3se {
void LuaScriptsRecompiling();  // lua.cpp: arguments built before the reload are stale
void LuaScriptsReloaded();     // lua.cpp: the hooks go back on the new functions
void LuaScriptsReloadDone();   // lua.cpp: the reload is over: TW3SE_OnScriptsReloaded events, w3.on_reload functions

namespace {

using Pass_t = bool (*)(void* unused, void* param2, void* files, uint64_t a4, uint64_t a5);
using Gather_t = void (*)(void* unused, void* files, void* other);
using Compile_t = bool (*)(void* a1, void* a2, void* a3, void* result, uint64_t a5, uint64_t a6);
using Step_t = bool (*)(void* self, uint64_t a2);                     // prepare, finish
using Apply_t = bool (*)(void* self, void* param2, void* result, uint64_t full);
using Relink_t = bool (*)(void* self, void* a2, void* a3, uint64_t a4);
using Swap_t = bool (*)(void* ctx, void* result, uint64_t full, uint64_t a4);
using ResultFree_t = void (*)(void* result);

// The engine's array as the gather fills it: data, count (12 bytes; 16 here so a capacity word has room).
struct EngineArray {
    void* data;
    uint32_t size;
    uint32_t pad;
};

Pass_t oPass = nullptr;
Compile_t oCompile = nullptr;
Step_t oPrepare = nullptr, oFinish = nullptr;
Apply_t oApply = nullptr;
Relink_t oRelink = nullptr;
Swap_t oSwap = nullptr;
uint32_t g_reloads = 0;
bool g_startSeen = false;
uint64_t g_startA4 = 0, g_startA5 = 1;
void* g_startParam2 = nullptr;
uint32_t g_startFiles = 0;
size_t g_sameFileSets = 0;  // sets of mods that bring the same game script file (PreScanClashes)

double Now() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}

void* Self() {
    uintptr_t g = Address("GScriptGlobals");
    return g ? *reinterpret_cast<void**>(g) : nullptr;
}

// The state bytes around the guard, as hex: 0x174..0x17f (0x175 prepared, 0x176 compile failed, 0x177 apply
// failed, 0x178 applied).
std::string Flags() {
    void* self = Self();
    if (!self) return "(no scripting system)";
    char s[64];
    const uint8_t* b = reinterpret_cast<const uint8_t*>(self) + 0x174;
    snprintf(s, sizeof s, "174..17f = %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", b[0], b[1], b[2],
             b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
    return s;
}

// The function a `call rel32` at fn+off goes to; null when the byte there is no call (another build).
void* CallTarget(uintptr_t fn, uint32_t off) {
    if (!fn) return nullptr;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(fn + off);
    if (p[0] != 0xE8) return nullptr;
    return const_cast<uint8_t*>(p + 5) + *reinterpret_cast<const int32_t*>(p + 1);
}

// The target of the one direct call in fn's first `range` bytes whose target starts with `head`, or null (none, or
// more than one). A game update can move prepare and finish in CompileAndApplyAll (5.01: +1f3 and +217, build
// 6adfbd7a: +1eb and +20f).
void* CallTo(uintptr_t fn, const uint8_t* head, size_t n, uint32_t range = 0x400) {
    if (!fn) return nullptr;
    void* found = nullptr;
    for (uint32_t off = 0; off < range; ++off) {
        void* t = CallTarget(fn, off);
        uint8_t got[32] = {};
        if (!t || n > sizeof got || !SafeRead(t, got, n) || memcmp(got, head, n) != 0) continue;
        if (found && found != t) return nullptr;
        found = t;
    }
    return found;
}
const uint8_t kPrepareHead[24] = {0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x70, 0x80, 0xb9,
                                  0x75, 0x01, 0x00, 0x00, 0x00, 0x0f, 0xb6, 0xf2, 0x48, 0x8b, 0xf9, 0x74};
const uint8_t kResultFreeHead[24] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89,  // 5.01 +234
                                     0x74, 0x24, 0x18, 0x48, 0x89, 0x7c, 0x24, 0x20, 0x41, 0x54, 0x41, 0x56};
const uint8_t kFinishHead[24] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89,
                                 0x74, 0x24, 0x20, 0x88, 0x54, 0x24, 0x10, 0x57, 0x41, 0x54, 0x41, 0x55};

bool SafeWrite(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void** WrapCollector(void* collector);  // below, with the compiler's messages
extern std::vector<std::string> g_messages;
extern std::map<std::wstring, std::string> g_good;
void NoteScriptCache(bool fallback);   // below, with the statement lines
void PutBackScriptCache();
void LinesOff(const std::string& why);
void LinesRetry();
}  // namespace
bool ScriptLinesOn();
namespace {

// compile as with -debugscripts: statement lines in the code (see below). Before every compile: the start-up pass
// and a live reload, which compiles without the pass (with the scripts loaded from the cache at the start, a
// reload would lose the lines)
void LinesFlag() {
    if (ScriptLinesOn() && Self()) {
        uint8_t zero = 0;
        SafeWrite(static_cast<char*>(Self()) + 0x17c, &zero, 1);
    }
}

// The start-up pass failed for good (with and without statement lines): the game shows its list of errors (Exit,
// Retry, Skip) and ends on Exit. Players take that for TW3SE's fault (the errors come from two mods that
// clash). A message box before the game's list says what it is, with the first errors and, for "already defined",
// the files that clash; `script_error_message = off` leaves it out.
void TellCompileFailure() {
    if (g_messages.empty()) return;
    bool newer = false;
    std::string list;
    for (size_t i = 0; i < g_messages.size() && i < 6; ++i) {
        const std::string& m = g_messages[i];
        newer = newer || m.find("needs a newer TW3SE") != std::string::npos;
        list += "- " + (m.size() > 360 ? m.substr(0, 360) + "..." : m) + "\n";
    }
    if (g_messages.size() > 6) list += "- ... (" + std::to_string(g_messages.size() - 6) + " more in tw3se.log)\n";
    std::string text = newer
        ? "The game's scripts did not compile. The game shows its list of errors next.\n\nA mod needs a newer version of TW3SE "
          "than the one installed. Update TW3SE, or remove that mod.\n\n" + list
        : "The game's scripts did not compile. The game shows its list of errors next.\n\nThe errors are in these mods' scripts. "
          "The game stops the same way without TW3SE.\n\n" + list +
          "\nMost often two mods clash (\"already defined\": keep it in one of them), or a mod was made for another "
          "version of the game. Remove or update the mod named first.";
    if (!newer && g_sameFileSets)
        text += "\n\n" + std::to_string(g_sameFileSets) + " set(s) of mods bring the same game script file, and the game "
                "takes only one of each (tw3se.log names them: \"... bring ...\"). Script Merger makes one file of them.";
    text += "\n\nAll errors: bin\\x64_dx12\\tw3se\\tw3se.log\nThis message: script_error_message = off in tw3se.config";
    bool box = ConfigBool("script_error_message", true);
    std::string flat = text;
    std::replace(flat.begin(), flat.end(), '\n', ' ');
    Log("scripts: %s: %s", box ? "message box" : "message (script_error_message = off, no box)", flat.c_str());
    if (!box) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &w[0], n);
    MessageBoxW(nullptr, w.c_str(), L"TW3SE: the game's scripts did not compile",
                MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
}

bool PassHook(void* unused, void* param2, void* files, uint64_t a4, uint64_t a5) {
    uint32_t n = files ? reinterpret_cast<EngineArray*>(files)->size : 0;
    void** gameVtable = nullptr;
    bool startup = !g_startSeen;
    if (!g_startSeen) {   // the game's own start-up call: remember how it was called
        g_startSeen = true;
        g_startParam2 = param2;
        g_startA4 = a4 & 0xff;
        g_startA5 = a5 & 0xff;
        g_startFiles = n;
        if (ConfigBool("compile_messages", true)) gameVtable = WrapCollector(param2);  // its messages into the log too
        g_messages.clear();
        LinesRetry();  // the game compiles: scripts changed, they may compile with lines again
    }
    Log("reload: script pass begins (param2 %p, %u files, a4 %llu, a5 %llu; %s)", param2, n,
        static_cast<unsigned long long>(a4 & 0xff), static_cast<unsigned long long>(a5 & 0xff), Flags().c_str());
    LinesFlag();
    double t = Now();
    bool ok = oPass(unused, param2, files, a4, a5);
    // With statement lines the compiler checks code it drops without them (Log and LogChannel calls): a mod that
    // compiles in the game's normal mode can fail here (a LogChannel with three arguments). The start-up pass
    // then runs once more without lines, so the game starts as it would without TW3SE.
    bool fallback = false;
    if (startup && !ok && ScriptLinesOn()) {
        LinesOff(g_messages.empty() ? std::string("no message") : g_messages[0]);
        uint8_t one = 1;
        SafeWrite(static_cast<char*>(Self()) + 0x17c, &one, 1);
        g_messages.clear();
        Log("reload: script pass once more without statement lines");
        ok = oPass(unused, param2, files, a4, a5);
        fallback = true;
    }
    if (startup) {
        if (ok) NoteScriptCache(fallback);
        else PutBackScriptCache();
    }
    if (gameVtable) {
        SafeWrite(param2, &gameVtable, 8);
        if (!ok)
            Log("scripts: the game's scripts did not compile (%zu error(s) above, \"compiler ERROR\"); the game will "
                "end itself", g_messages.size());
    }
    if (startup && !ok) {  // the game ends itself with code 0: the crash report comes all the same (crashreport.cpp)
        std::string note = "ended: the game's scripts did not compile";
        if (!g_messages.empty()) note += "; the first error: " + g_messages[0];
        SessionNote(note.c_str());
        TellCompileFailure();
    }
    Log("reload: script pass ends ok=%d in %.0f ms (%s)", ok, Now() - t, Flags().c_str());
    return ok;
}

bool CompileHook(void* a1, void* a2, void* a3, void* result, uint64_t a5, uint64_t a6) {
    LinesFlag();
    Log("reload:   compile begins (%s)", Flags().c_str());
    double t = Now();
    bool ok = oCompile(a1, a2, a3, result, a5, a6);
    uint8_t mode = result ? *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(result) + 0x6c) : 0xff;
    Log("reload:   compile ends ok=%d in %.0f ms, result+0x6c=%u (%s)", ok, Now() - t, mode, Flags().c_str());
    return ok;
}

// The game applies compiled scripts on its own too: at the main menu it loads mod.io and REDkit mods
// (CContentManager::LoadModioMods: prepare, each mod's compiled scripts, finish). That changes the functions like a
// live reload, so Lua's hooks come off before it and go back on after it, as around RunPass (with mod.io mods
// and Lua hooks on, the game crashes in finish). `keepHooks` is the test of `reloadscripts gamepass keep`: the
// hooks stay on.
bool g_ownPass = false;     // RunPass: the hooks are off for the whole pass already
bool g_keepHooks = false;
bool g_gameParked = false;  // taken off in prepare, back on after finish

bool PrepareHook(void* self, uint64_t a2) {
    if (!g_ownPass && !g_keepHooks) {
        LuaScriptsRecompiling();
        g_gameParked = true;
    }
    Log("reload:   prepare begins (%s)", Flags().c_str());
    double t = Now();
    bool ok = oPrepare(self, a2);
    Log("reload:   prepare ends ok=%d in %.0f ms (%s)", ok, Now() - t, Flags().c_str());
    return ok;
}

bool ApplyHook(void* self, void* param2, void* result, uint64_t full) {
    uint8_t mode = result ? *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(result) + 0x6c) : 0xff;
    Log("reload:   apply begins full=%llu result+0x6c=%u (%s)", static_cast<unsigned long long>(full & 0xff), mode,
        Flags().c_str());
    double t = Now();
    bool ok = oApply(self, param2, result, full);
    Log("reload:   apply ends ok=%d in %.0f ms (%s)", ok, Now() - t, Flags().c_str());
    return ok;
}

bool RelinkHook(void* self, void* a2, void* a3, uint64_t a4) {
    Log("reload:     relink RTTI begins");
    double t = Now();
    bool ok = oRelink(self, a2, a3, a4);
    Log("reload:     relink RTTI ends in %.0f ms", Now() - t);
    return ok;
}

bool SwapHook(void* ctx, void* result, uint64_t full, uint64_t a4) {
    Log("reload:     swap in the new code begins (live scriptables: OnScriptReloading / OnScriptReloaded)");
    double t = Now();
    bool ok = oSwap(ctx, result, full, a4);
    Log("reload:     swap ends ok=%d in %.0f ms", ok, Now() - t);
    return ok;
}

bool FinishHook(void* self, uint64_t a2) {
    Log("reload:   finish begins (%s)", Flags().c_str());
    double t = Now();
    bool ok = oFinish(self, a2);
    Log("reload:   finish ends ok=%d in %.0f ms (%s)", ok, Now() - t, Flags().c_str());
    if (g_gameParked) {
        g_gameParked = false;
        LuaScriptsReloaded();
    }
    return ok;
}

void HookAt(void* target, void* detour, void** orig, const char* what) {
    if (!target || !Hook(target, detour, orig)) Log("reload: %s step not hooked (another build?)", what);
}

// --- the compiler's messages. The start-up pass hands the compiler an object that collects them (vtable: [0] dtor,
// [1] nothing, [2] and [3] a message each: ctx = the file as an engine string, its line at +0xc; msg = an engine
// string). The reload hands it one of ours: every message goes to tw3se.log and into the reply.
struct EngineString {   // an engine AnsiString: 8-bit text and its length (with the terminating zero)
    const char* data;
    uint32_t size;
};

std::vector<std::string> g_messages;   // errors (slot 3)
uint32_t g_warnings = 0;                // slot 2: the base game alone brings ~1000 (entry functions and the like)

// The engine strings copied out under a guard: file, line, text.
bool CopyMessage(const EngineString* ctx, const EngineString* msg, char* file, char* text, int* line) {
    __try {
        *line = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(ctx) + 0xc);
        for (int k = 0; k < 2; ++k) {
            const EngineString* s = k ? msg : ctx;
            char* out = k ? text : file;
            uint32_t i = 0;
            if (s && s->data)
                for (; i < s->size && i < 1023 && s->data[i]; ++i) out[i] = s->data[i];
            out[i] = 0;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A .ws file as 8-bit text: the game's own are UTF-16 (a BOM FF FE), mods are either.
std::string AsciiOf(const std::string& data) {
    if (data.size() < 2 || static_cast<uint8_t>(data[0]) != 0xFF || static_cast<uint8_t>(data[1]) != 0xFE) return data;
    std::string out;
    out.reserve(data.size() / 2);
    for (size_t i = 2; i + 1 < data.size(); i += 2) out += data[i + 1] ? '?' : data[i];
    return out;
}

bool IdentChar(char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// The text without comments and string literals (blanked, so offsets stay): a struct commented out is no declaration
// (the usual fix for "already defined" comments one copy out).
std::string StripComments(const std::string& s) {
    std::string out = s;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '/') {
            while (i < out.size() && out[i] != '\n') out[i++] = ' ';
        } else if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '*') {
            for (; i < out.size() && !(out[i] == '*' && i + 1 < out.size() && out[i + 1] == '/'); ++i)
                if (out[i] != '\n') out[i] = ' ';
            if (i + 1 < out.size()) {
                out[i] = ' ';
                out[++i] = ' ';
            }
        } else if (out[i] == '"') {
            for (++i; i < out.size() && out[i] != '"' && out[i] != '\n'; ++i) out[i] = ' ';
        }
    }
    return out;
}

// `keyword name` in the text: struct/class/enum for a type, var for a property
bool Declares(const std::string& text, const std::string& name, bool type) {
    for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) {
        if ((at && IdentChar(text[at - 1])) || IdentChar(text[at + name.size()])) continue;
        size_t e = at;
        while (e && isspace(static_cast<unsigned char>(text[e - 1]))) --e;
        size_t b = e;
        while (b && IdentChar(text[b - 1])) --b;
        std::string word = text.substr(b, e - b);
        if (type ? (word == "struct" || word == "class" || word == "enum") : word == "var") return true;
    }
    return false;
}

// "Struct 'X' is already defined" / "Property 'C::p' was already exported": two script files declare the same thing,
// in most cases two mods that import the same part of the engine (Better Candles and Shared Imports
// Remastered). The hint names the files that declare it, so the log says which mods clash.
std::string ClashHint(const char* text) {
    if (!strstr(text, "already defined") && !strstr(text, "already exported") && !strstr(text, "already imported"))
        return "";
    const char* q = strchr(text, '\'');
    const char* e = q ? strchr(q + 1, '\'') : nullptr;
    if (!e || e - q > 200) return "";
    std::string name(q + 1, e);
    size_t c = name.rfind("::");
    bool type = c == std::string::npos;
    if (!type) name = name.substr(c + 2);
    std::string where;
    int found = 0;
    for (const auto& [path, data] : g_good) {
        if (data.find(name) == std::string::npos && AsciiOf(data).find(name) == std::string::npos) continue;
        if (!Declares(StripComments(AsciiOf(data)), name, type)) continue;
        std::string rel;  // ASCII enough for a log line
        for (wchar_t ch : path) rel += ch < 128 ? static_cast<char>(ch) : '?';
        size_t m = rel.find("\\Mods\\");
        if (m == std::string::npos) m = rel.find("\\dlc\\");
        if (m != std::string::npos) rel = rel.substr(m + 1);
        size_t cs = rel.find("\\content\\scripts\\");
        if (cs != std::string::npos) rel.erase(cs, 16);
        where += (found++ ? ", " : "") + rel;
        if (found == 6) break;
    }
    if (found < 2) return "";
    return " - declared in " + where + ": the mods clash, keep it in one of them";
}

void MessageGuarded(int slot, const EngineString* ctx, const EngineString* msg) {
    char file[1024], text[1024];
    int line = 0;
    if (!CopyMessage(ctx, msg, file, text, &line)) return;
    if (slot == 2) {
        ++g_warnings;
        if (strstr(file, "[content0]")) return;   // the base game's own: not worth a line each
    }
    std::string m = std::string(file) + "(" + std::to_string(line) + "): " + text;
    if (const char* t = slot == 3 ? strstr(text, "TW3SE_") : nullptr) {  // a TW3SE function this version lacks
        size_t n = 6;
        while (isalnum(static_cast<unsigned char>(t[n])) || t[n] == '_') ++n;
        std::string fn(t, n);
        if (!NativeKnown(fn)) {
            char hint[160];
            snprintf(hint, sizeof hint, " - TW3SE %d.%d.%d has no %s: the mod needs a newer TW3SE", TW3SE_VERSION_MAJOR,
                     TW3SE_VERSION_MINOR, TW3SE_VERSION_PATCH, fn.c_str());
            m += hint;
        }
    }
    if (slot == 3) m += ClashHint(text);
    Log("reload: compiler %s %s", slot == 3 ? "ERROR" : "warning", m.c_str());
    if (slot == 3 && g_messages.size() < 20) g_messages.push_back(m);
}

void* RecvDtor(void* self, uint32_t) { return self; }
void RecvNothing(void*) {}
void RecvMessage2(void*, const EngineString* ctx, const EngineString* msg) { MessageGuarded(2, ctx, msg); }
void RecvMessage3(void*, const EngineString* ctx, const EngineString* msg) { MessageGuarded(3, ctx, msg); }

struct Receiver {
    void** vtable;
    uint8_t fields[0x60];   // the game's own keeps two arrays at +0x08 and +0x14; never read through ours
};
void* g_recvVtable[4] = {reinterpret_cast<void*>(&RecvDtor), reinterpret_cast<void*>(&RecvNothing),
                         reinterpret_cast<void*>(&RecvMessage2), reinterpret_cast<void*>(&RecvMessage3)};
Receiver g_receiver = {g_recvVtable, {}};

// The start-up pass: the game's own collector gets a copy of its vtable whose message slots write each message to
// tw3se.log and then hand it on, so the game does with it what it always does (a mod that does not compile ends the
// game ~20 s after the start; its file, line and error are in tw3se.log).
using MsgFn = void (*)(void*, const EngineString*, const EngineString*);
MsgFn g_gameMsg2 = nullptr, g_gameMsg3 = nullptr;
// [0] is the slot before the vtable: the class's RTTI locator, copied too, so the copy is the same type to anything
// that asks. `compile_messages = off` in tw3se.config leaves the collector alone.
void* g_proxyStore[33] = {};
void** const g_proxyVtable = g_proxyStore + 1;
void ProxyMessage2(void* self, const EngineString* ctx, const EngineString* msg) {
    MessageGuarded(2, ctx, msg);
    g_gameMsg2(self, ctx, msg);
}
void ProxyMessage3(void* self, const EngineString* ctx, const EngineString* msg) {
    MessageGuarded(3, ctx, msg);
    g_gameMsg3(self, ctx, msg);
}
void** WrapCollector(void* collector) {  // the collector's own vtable (to put back), or null when left alone
    void** vt = nullptr;
    if (!collector || !SafeRead(collector, &vt, 8) || !vt || !SafeRead(vt - 1, g_proxyStore, sizeof g_proxyStore))
        return nullptr;
    if (!g_proxyVtable[2] || !g_proxyVtable[3]) return nullptr;
    g_gameMsg2 = reinterpret_cast<MsgFn>(g_proxyVtable[2]);
    g_gameMsg3 = reinterpret_cast<MsgFn>(g_proxyVtable[3]);
    g_proxyVtable[2] = reinterpret_cast<void*>(&ProxyMessage2);
    g_proxyVtable[3] = reinterpret_cast<void*>(&ProxyMessage3);
    void** proxy = g_proxyVtable;
    return SafeWrite(collector, &proxy, 8) ? vt : nullptr;
}

// --- the last script files that compiled: the files of Mods\ and dlc\ as the game compiled them at start-up, then
// after every good reload. A reload that fails puts them back for one more pass (the game keeps running on them),
// then the changed files return so they can be fixed.
std::map<std::wstring, std::string> g_good;

std::string ReadAll(const std::wstring& path, bool* ok) {
    std::string out;
    *ok = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size) && size.QuadPart < (16 << 20)) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        *ok = out.empty() || (ReadFile(h, &out[0], static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return out;
}

bool WriteAll(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) && put == data.size());
    CloseHandle(h);
    return ok;
}

void ScanDir(const std::wstring& dir, std::map<std::wstring, std::string>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring p = dir + L"\\" + n;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanDir(p, out);
        } else if (n.size() > 3 && _wcsicmp(n.c_str() + n.size() - 3, L".ws") == 0) {
            bool ok;
            std::string data = ReadAll(p, &ok);
            if (ok) out[p] = std::move(data);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// <game> from <game>\bin\x64_dx12\tw3se
std::wstring GameRoot() {
    std::wstring d = Dir();
    for (int i = 0; i < 3; ++i) d = d.substr(0, d.find_last_of(L"\\/"));
    return d;
}

// Every .ws in <game>\Mods\*\content\scripts and <game>\dlc\*\content\scripts.
std::map<std::wstring, std::string> ScanScripts() {
    std::map<std::wstring, std::string> out;
    std::wstring root = GameRoot();
    for (const wchar_t* folder : {L"\\Mods", L"\\dlc"}) {
        WIN32_FIND_DATAW fd;
        std::wstring base = root + folder;
        HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            ScanDir(base + L"\\" + n + L"\\content\\scripts", out);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

std::string Narrow(const std::wstring& w) {
    std::string out(w.size() * 3 + 1, '\0');
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0],
                                  static_cast<int>(out.size()), nullptr, nullptr);
    out.resize(len > 0 ? len : 0);
    return out;
}

// Where a fault inside the reload happened: the address (witcher3+rva), and the game addresses on the stack above
// it. The engine is then half-way through a reload; the game will likely not live long, but the log says where.
int Filter(EXCEPTION_POINTERS* ep, DWORD* code) {
    *code = ep->ExceptionRecord->ExceptionCode;
    uintptr_t base = ImageBase();
    uintptr_t at = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    Log("reload: FAULT 0x%08lx at witcher3+%llx (read/write %llu at %p), rcx=%p rdx=%p r8=%p", *code,
        static_cast<unsigned long long>(at - base),
        static_cast<unsigned long long>(ep->ExceptionRecord->ExceptionInformation[0]),
        reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]),
        reinterpret_cast<void*>(ep->ContextRecord->Rcx), reinterpret_cast<void*>(ep->ContextRecord->Rdx),
        reinterpret_cast<void*>(ep->ContextRecord->R8));
    const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(ep->ContextRecord->Rsp);
    char trail[400] = "";
    size_t len = 0;
    for (int i = 0, n = 0; i < 512 && n < 16; ++i) {
        uintptr_t v = sp[i];
        if (v > base && v < base + 0x8000000) {
            len += snprintf(trail + len, sizeof trail - len, " +%llx", static_cast<unsigned long long>(v - base));
            ++n;
        }
    }
    Log("reload: stack (game addresses):%s", trail);
    return EXCEPTION_EXECUTE_HANDLER;
}

bool PassGuarded(Pass_t fn, void* files, DWORD* code) {
    __try {
        return fn(nullptr, &g_receiver, files, g_startA4, g_startA5);
    } __except (Filter(GetExceptionInformation(), code)) {
        return false;
    }
}

// The compile step alone into a result of our own (nothing is prepared or applied), then the result freed.
bool CompileOnlyGuarded(void* files, uint8_t* result, DWORD* code) {
    __try {
        return oCompile(Self(), nullptr, files, result, 0, g_startA5);
    } __except (Filter(GetExceptionInformation(), code)) {
        return false;
    }
}

// `reloadscripts gamepass [keep] [blob=<path>]` (a test): the game's own apply as at the main menu with mod.io mods:
// prepare, a mod's precompiled.rsblob (when given), finish, through the hooks above. keep = the Lua hooks stay on
// during it (TW3SE up to 1.2.0). The blob step is the game's per-mod apply (CContentManager::LoadModioMods+0x1096
// calls it: scripting system, 0, the file's path, two failure flags), found by its bytes in this build of
// witcher3.exe only. Local mods' blobs load before the Lua mods; mod.io mods load later, with the hooks on.
using ApplyBlob_t = bool (*)(void* self, uint64_t zero, void* path, uint8_t* failA, uint8_t* failB);
struct BlobPath {  // the engine's 8-bit string: text and size with the terminating zero
    const char* data;
    uint32_t size;
};

// After a blob that failed (flag 0x177), LoadModioMods+0x10ce calls this: the scripts as they were before the pass
// go back (it relinks the RTTI). After that relink between prepare and finish, finish can crash.
// The only place in witcher3.exe's code with these first bytes, or null (a game update moves both functions; their
// bytes stay the same).
void* FindUnique(const uint8_t* bytes, size_t n) {
    const uint8_t* text = nullptr;
    size_t size = 0;
    ExeText(&text, &size);
    const uint8_t* found = nullptr;
    for (const uint8_t* at = text; text && at + n <= text + size; ++at) {
        at = static_cast<const uint8_t*>(memchr(at, bytes[0], text + size - n - at + 1));
        if (!at) break;
        if (memcmp(at, bytes, n) == 0) {
            if (found) return nullptr;
            found = at;
        }
    }
    return const_cast<uint8_t*>(found);
}

using Rollback_t = bool (*)(void* self);
Rollback_t RollbackFn() {  // 5.01: +270d050, build 6adfbd7a: +270b260
    static const uint8_t expect[24] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x50, 0x80, 0xb9, 0x75, 0x01, 0x00, 0x00,
                                       0x00, 0x48, 0x8b, 0xd9, 0x75, 0x08, 0x32, 0xc0, 0x48, 0x83, 0xc4, 0x50};
    static void* fn = FindUnique(expect, sizeof expect);
    return reinterpret_cast<Rollback_t>(fn);
}

ApplyBlob_t ApplyBlobFn() {  // 5.01: +270e900, build 6adfbd7a: +270cb00
    static const uint8_t expect[29] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x18, 0x56, 0x57, 0x41, 0x56,
                                       0x48, 0x81, 0xec, 0x20, 0x02, 0x00, 0x00, 0x80, 0xb9, 0x75, 0x01, 0x00, 0x00, 0x00};
    static void* fn = FindUnique(expect, sizeof expect);
    return reinterpret_cast<ApplyBlob_t>(fn);
}

void LogBlob(const char* blob, bool applied, const uint8_t* fails) {
    Log("reload:   blob %s applied=%d, failures %u %u (%s)", blob, applied, fails[0], fails[1], Flags().c_str());
}

// Each compiled-scripts file the game loads goes into the log with its outcome (the game can take out a mod.io
// mod's scripts and crash after it, and the log says which mod): the start-up cache, each mod's
// precompiled.rsblob. failRead: the file could not be read or is of another version; failApply: its code did not
// fit the game's scripts (a function or type it names is missing). The game takes a failed mod's scripts out again.
ApplyBlob_t oApplyBlob = nullptr;
ApplyBlob_t g_applyBlob = nullptr;  // the game's function, hooked

std::string BlobPathText(void* path) {
    BlobPath bp = {};
    char buf[300] = {};
    if (!SafeRead(path, &bp, sizeof bp) || !bp.data) return "(no path)";
    uint32_t n = bp.size < sizeof buf ? bp.size : sizeof buf - 1;
    if (!SafeRead(bp.data, buf, n)) return "(unreadable path)";
    buf[sizeof buf - 1] = 0;
    return buf;
}

bool ApplyBlobHook(void* self, uint64_t zero, void* path, uint8_t* failRead, uint8_t* failApply) {
    bool ok = oApplyBlob(self, zero, path, failRead, failApply);
    std::string p = BlobPathText(path);
    if (ok) {
        Log("scripts: compiled scripts loaded: %s", p.c_str());
        return ok;
    }
    const char* why = failRead && *failRead ? "the file could not be read, or it is of another game version"
                      : failApply && *failApply ? "its code does not fit the game's scripts: a function or type it uses is missing"
                                                : "not loaded";
    Log("scripts: compiled scripts did NOT load: %s (%s) - the game takes them out again", p.c_str(), why);
    // The game ends itself ~30 s later, with code 0 and without TW3SE too (measured): the crash report at
    // the next start says which mod it was
    std::string note = "a mod's compiled scripts did not load: " + p + " (" + why + "); the game ends itself after that";
    SessionNoteFor(note.c_str(), 120);
    return ok;
}

bool GamePassGuarded(DWORD* code, ApplyBlob_t apply, const char* blob, uint32_t blobSize, uint8_t* fails) {
    __try {
        if (!PrepareHook(Self(), 1)) return false;
        if (apply) {
            BlobPath path = {blob, blobSize};
            bool applied = apply(Self(), 0, &path, &fails[0], &fails[1]);
            LogBlob(blob, applied, fails);
            Rollback_t rollback = RollbackFn();
            if ((fails[0] || fails[1]) && *(static_cast<uint8_t*>(Self()) + 0x177) && rollback) {
                fails[1] |= 0x80;  // the reply says the rollback ran
                rollback(Self());
            }
        }
        return FinishHook(Self(), 1);
    } __except (Filter(GetExceptionInformation(), code)) {
        return false;
    }
}

bool GatherGuarded(Gather_t fn, EngineArray* files, EngineArray* other, DWORD* code) {
    __try {
        fn(nullptr, files, other);
        return true;
    } __except (Filter(GetExceptionInformation(), code)) {
        return false;
    }
}

std::string StartInfo() {
    char s[160];
    if (!g_startSeen) return "start-up pass not seen (TW3SE loaded after it?), reload uses a4 0, a5 1";
    snprintf(s, sizeof s, "start-up pass: %u files, param2 %p, a4 %llu, a5 %llu", g_startFiles, g_startParam2,
             static_cast<unsigned long long>(g_startA4), static_cast<unsigned long long>(g_startA5));
    return s;
}

// The bytes of a compile result as hex (0x70), to find where it keeps its errors (development).
std::string Hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; ++i) {
        snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
        if (i % 8 == 7) s += ' ';
    }
    return s;
}

// Gather and compile only (`reloadscripts check`): the result's bytes and its arrays' sizes into the log.
bool CheckOnly(std::string& detail) {
    auto gather = Fn<Gather_t>("GatherScriptFiles");
    uintptr_t pass = Address("CScriptingSystem::CompileAndApplyAll");
    auto freeResult = reinterpret_cast<ResultFree_t>(CallTo(pass, kResultFreeHead, sizeof kResultFreeHead));
    if (!gather || !oCompile || !freeResult) {
        detail = "gather, compile or result free not found";
        return false;
    }
    static EngineArray files = {}, other = {};
    files = {};
    other = {};
    DWORD code = 0;
    if (!GatherGuarded(gather, &files, &other, &code)) {
        detail = "fault while gathering";
        return false;
    }
    alignas(16) static uint8_t result[0x100];
    memset(result, 0, sizeof result);
    double t = Now();
    bool ok = CompileOnlyGuarded(&files, result, &code);
    Log("reload check: compile ok=%d in %.0f ms, %u files, fault 0x%08lx", ok, Now() - t, files.size, code);
    for (int o = 0; o < 0x70; o += 0x20) Log("reload check: result+%02x %s", o, Hex(result + o, 0x20).c_str());
    char s[96];
    snprintf(s, sizeof s, "compile ok=%d in %.0f ms (result bytes in tw3se.log)", ok, Now() - t);
    detail = s;
    if (!code) freeResult(result);
    return ok;
}

// One script pass over the files on disk now: gather, then the start-up routine. False with `detail` on failure.
bool RunPass(uint32_t n, std::string& detail) {
    auto gather = Fn<Gather_t>("GatherScriptFiles");
    if (!gather) {
        detail = "GatherScriptFiles not found on this build";
        return false;
    }
    static EngineArray files = {}, other = {};   // the engine owns what it put in; a leak per pass is fine
    files = {};
    other = {};
    DWORD code = 0;
    double t = Now();
    if (!GatherGuarded(gather, &files, &other, &code)) {
        detail = "fault while gathering the script files";
        return false;
    }
    Log("reload #%u: gathered %u + %u script file entries in %.0f ms", n, files.size, other.size, Now() - t);
    if (!files.size) {
        detail = "the gather found no script files";
        return false;
    }
    uint8_t* b = reinterpret_cast<uint8_t*>(Self());
    b[0x176] = 0;   // last pass's failure marks: the finish step reads them
    b[0x177] = 0;
    LuaScriptsRecompiling();
    g_ownPass = true;
    bool ok = PassGuarded(oPass, &files, &code);
    g_ownPass = false;
    LuaScriptsReloaded();
    if (code) {
        char s[64];
        snprintf(s, sizeof s, "fault 0x%08lx inside the pass (tw3se.log)", code);
        detail = s;
    } else if (!ok) {
        detail = "the scripts did not compile";
    }
    return ok && !code;
}

}  // namespace

// The reload. force=false only reports. Main thread only.
bool HotReload(bool force, std::string& detail) {
    void* self = Self();
    if (!oPass || !self) {
        detail = "script pass or scripting system not found on this build";
        return false;
    }
    if (!force) {
        detail = Flags() + "; " + StartInfo() + "; " + std::to_string(g_good.size()) + " script files kept as good";
        return false;
    }
    if (!OnMainThread()) {
        detail = "not on the main thread";
        return false;
    }
    uint8_t* b = reinterpret_cast<uint8_t*>(self);
    uint32_t n = ++g_reloads;
    Log("reload #%u: begins (%s; %s)", n, Flags().c_str(), StartInfo().c_str());
    if (b[0x175]) {   // a pass is prepared and not finished: running another one now is not what the engine expects
        detail = "the scripting system is mid-pass (0x175 set), not reloading";
        Log("reload #%u: %s", n, detail.c_str());
        return false;
    }
    std::map<std::wstring, std::string> now = ScanScripts();
    uint32_t changed = 0;
    for (auto& f : now) {
        auto g = g_good.find(f.first);
        if (g == g_good.end() || g->second != f.second) {
            ++changed;
            Log("reload #%u: changed %s", n, Narrow(f.first).c_str());
        }
    }
    for (auto& g : g_good)
        if (!now.count(g.first)) {
            ++changed;
            Log("reload #%u: removed %s", n, Narrow(g.first).c_str());
        }
    g_messages.clear();
    g_warnings = 0;
    double t = Now();
    bool ok = RunPass(n, detail);
    if (ok) {
        g_good = std::move(now);
        char s[96];
        snprintf(s, sizeof s, "ok, %u file(s) changed - #%u in %.0f ms", changed, n, Now() - t);
        detail = s;
        Log("reload #%u: ends %s (%s)", n, detail.c_str(), Flags().c_str());
        LuaScriptsReloadDone();
        return true;
    }
    // Failed: the engine dropped the old code with the new. Back to the last good files for one more pass, at once
    // (before the game ticks again), then the changed files return to disk.
    std::vector<std::string> errors = g_messages;
    Log("reload #%u: failed (%s), %zu message(s); passing the last good files once more", n, detail.c_str(),
        errors.size());
    std::vector<std::wstring> added;
    for (auto& f : now)
        if (!g_good.count(f.first)) {
            std::wstring held = f.first + L".tw3se_held";
            if (MoveFileExW(f.first.c_str(), held.c_str(), MOVEFILE_REPLACE_EXISTING)) added.push_back(f.first);
        }
    for (auto& g : g_good) {
        auto c = now.find(g.first);
        if (c == now.end() || c->second != g.second) WriteAll(g.first, g.second);
    }
    std::string again;
    bool back = RunPass(n, again);
    for (auto& g : g_good) {   // the files as they were edited, back on disk
        auto c = now.find(g.first);
        if (c == now.end()) DeleteFileW(g.first.c_str());
        else if (c->second != g.second) WriteAll(g.first, c->second);
    }
    for (auto& a : added) MoveFileExW((a + L".tw3se_held").c_str(), a.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (back) LuaScriptsReloadDone();  // the last good scripts are in again: their objects get the event too
    Log("reload #%u: last good files passed again ok=%d%s; edited files back on disk (%s)", n, back,
        back ? "" : (" (" + again + ")").c_str(), Flags().c_str());
    std::string msg = back ? "compile errors, the last good scripts run again" : ("compile errors, and the last good "
                                                                                 "scripts failed too: " + again);
    for (auto& e : errors) msg += " | " + e;
    char s[48];
    snprintf(s, sizeof s, " - #%u in %.0f ms", n, Now() - t);
    detail = msg + s;
    return false;
}

namespace {

// At process start: the script files the game is about to compile, as the first good state.
void KeepStartFiles() {
    double t = Now();
    g_good = ScanScripts();
    Log("reload: %zu script files of Mods\\ and dlc\\ kept as the good state (%.0f ms)", g_good.size(), Now() - t);
}

// Where a script file comes from, as the game names it: "Mods\<mod>\<path under content\scripts>" (or dlc\...), and
// the mod's folder name alone
std::string ScriptRel(const std::wstring& path, std::wstring* mod) {
    std::wstring low = path;
    std::transform(low.begin(), low.end(), low.begin(), towlower);
    size_t m = low.find(L"\\mods\\");
    if (m == std::wstring::npos) m = low.find(L"\\dlc\\");
    std::wstring rel = m == std::wstring::npos ? path : path.substr(m + 1);
    size_t c = rel.find(L'\\');
    size_t cs = c == std::wstring::npos ? std::wstring::npos : rel.find(L'\\', c + 1);
    if (mod) *mod = cs == std::wstring::npos ? L"" : rel.substr(c + 1, cs - c - 1);
    std::wstring lowRel = rel;
    std::transform(lowRel.begin(), lowRel.end(), lowRel.begin(), towlower);
    size_t sc = lowRel.find(L"\\content\\scripts\\");
    if (sc != std::wstring::npos) rel.erase(sc, 16);
    return Narrow(rel);
}

// Before the game compiles: a struct, class or enum declared in two different script files of Mods\ and dlc\ stops
// the compile ("already defined", e.g. Better Candles against Shared Imports Remastered). The log says it at
// the start, with both files. The same file in two mods is no clash: the game takes one of them (the order of
// mods.settings), the other one is left out. Mods switched off in mods.settings are left out here too.
void PreScanClashes() {
    double t = Now();
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> where;  // name -> (file, path in its mod)
    std::map<std::string, std::vector<std::string>> replacing;  // path under content\scripts -> the mods that have it
    bool merged = false;  // Script Merger's mod0000_MergedFiles: it settles the files two mods replace
    for (const auto& [path, data] : g_good) {
        std::wstring mod;
        std::string rel = ScriptRel(path, &mod);
        if (!mod.empty() && GameModSwitchedOff(mod)) continue;
        std::string modName = Narrow(mod);
        merged = merged || _stricmp(modName.c_str(), "mod0000_MergedFiles") == 0;
        std::string inMod = rel.substr((std::min)(rel.size(), rel.find('\\', rel.find('\\') + 1) + 1));
        std::transform(inMod.begin(), inMod.end(), inMod.begin(), ::tolower);
        replacing[inMod].push_back(modName);
        std::string text = StripComments(AsciiOf(data));
        for (const char* kw : {"struct", "class", "enum"}) {
            size_t klen = strlen(kw);
            for (size_t at = text.find(kw); at != std::string::npos; at = text.find(kw, at + klen)) {
                if ((at && IdentChar(text[at - 1])) || (at + klen < text.size() && IdentChar(text[at + klen]))) continue;
                size_t b = at + klen;
                while (b < text.size() && isspace(static_cast<unsigned char>(text[b]))) ++b;
                size_t e = b;
                while (e < text.size() && IdentChar(text[e])) ++e;
                if (e == b || (at + klen == b)) continue;
                auto& v = where[text.substr(b, e - b)];
                bool seen = false;
                for (auto& f : v) seen = seen || f.second == inMod;  // the same file in another mod: one of them counts
                if (!seen) v.push_back({rel, inMod});
            }
        }
        // fields: `var` right inside a class or struct, and `@addField(Class) var` ("already exported": Fog
        // Remastered's @addField of activated_LY_MoonPhase against Shared Imports' full import of that struct)
        auto ident = [&](size_t from, size_t* end) {
            while (from < text.size() && isspace(static_cast<unsigned char>(text[from]))) ++from;
            size_t e = from;
            while (e < text.size() && IdentChar(text[e])) ++e;
            *end = e;
            return text.substr(from, e - from);
        };
        std::string cls, pending, addField;
        int depth = 0;
        for (size_t i = 0; i < text.size();) {
            char c = text[i];
            if (c == '{') {
                if (depth == 0) cls = pending;
                pending.clear();
                ++depth;
                ++i;
            } else if (c == '}') {
                if (depth > 0 && --depth == 0) cls.clear();
                ++i;
            } else if (c == '@' && text.compare(i, 10, "@addField(") == 0) {
                size_t e = 0;
                addField = ident(i + 10, &e);
                size_t close = text.find(')', e);
                i = close == std::string::npos ? text.size() : close + 1;
            } else if (IdentChar(c) && (i == 0 || !IdentChar(text[i - 1]))) {
                size_t e = 0;
                std::string w = ident(i, &e);
                if (depth == 0 && (w == "struct" || w == "class")) {
                    pending = ident(e, &e);
                } else if (w == "var" && ((depth == 1 && !cls.empty()) || (depth == 0 && !addField.empty()))) {
                    std::string owner = depth == 1 ? cls : addField;
                    for (;;) {  // var a, b : int;
                        std::string name = ident(e, &e);
                        if (name.empty()) break;
                        auto& v = where[owner + "::" + name];
                        bool seen = false;
                        for (auto& f : v) seen = seen || f.second == inMod;
                        if (!seen) v.push_back({rel, inMod});
                        while (e < text.size() && isspace(static_cast<unsigned char>(text[e]))) ++e;
                        if (e >= text.size() || text[e] != ',') break;
                        ++e;
                    }
                    if (depth == 0) addField.clear();
                }
                i = e > i ? e : i + 1;
            } else {
                ++i;
            }
        }
    }
    // one line per set of files that clash, with what they both declare
    std::map<std::string, std::vector<std::string>> byFiles;
    int n = 0;
    for (const auto& [name, files] : where) {
        if (files.size() < 2) continue;
        size_t sep = name.find("::");
        if (sep != std::string::npos) {  // a field of a type that is itself declared twice: that one says it
            auto t = where.find(name.substr(0, sep));
            if (t != where.end() && t->second.size() > 1) continue;
        }
        std::string list;
        for (const auto& f : files) list += (list.empty() ? "" : " and ") + f.first;
        byFiles[list].push_back(name);
        ++n;
    }
    int lines = 0;
    for (const auto& [files, names] : byFiles) {
        if (++lines > 12) break;
        std::string what;
        for (size_t i = 0; i < names.size() && i < 6; ++i) what += (i ? ", " : "") + names[i];
        if (names.size() > 6) what += ", ... (" + std::to_string(names.size()) + " in all)";
        Log("scripts: WARNING - %s both declare %s: the game's compile stops at the second one (\"already defined\" "
            "or \"already exported\"); keep each in one of them", files.c_str(), what.c_str());
    }
    // Two mods that bring the same game script file: the game takes one of them, the other mod's changes to that file
    // are missing ("Could not find function": AMM and AutoLoot both bring ingamemenu.ws). Script Merger
    // makes one file of them in mod0000_MergedFiles; with that folder there, this is left to it.
    std::map<std::string, std::vector<std::string>> byMods;  // "modA and modB" -> the files
    for (const auto& [file, mods] : replacing) {
        if (mods.size() < 2 || merged) continue;
        std::string pair;
        for (const auto& m : mods) pair += (pair.empty() ? "" : " and ") + m;
        byMods[pair].push_back(file);
    }
    g_sameFileSets = byMods.size();
    lines = 0;
    for (const auto& [mods, files] : byMods) {
        if (++lines > 12) break;
        std::string list;
        for (size_t i = 0; i < files.size() && i < 4; ++i) list += (i ? ", " : "") + files[i];
        if (files.size() > 4) list += ", ... (" + std::to_string(files.size()) + " in all)";
        bool two = mods.find(" and ") == mods.rfind(" and ");
        Log("scripts: WARNING - %s %s bring %s: the game takes one of each, the other mods' changes to it are "
            "missing (\"Could not find function\"). Script Merger makes one file of them", mods.c_str(),
            two ? "both" : "all", list.c_str());
    }
    Log("scripts: %zu script files of Mods\\ and dlc\\ checked: %d type(s) or field(s) declared twice, %zu set(s) of "
        "mods bringing the same game file (%.0f ms)", g_good.size(), n, merged ? 0 : byMods.size(), Now() - t);
}

void CmdReload(const char* args, char* reply, uint32_t size) {
    std::string a = args ? args : "";
    std::string detail;
    bool force = a.find("force") != std::string::npos;
    if (a.find("gamepass") != std::string::npos) {
        if (!OnMainThread() || !Self() || !oPrepare || !oFinish) {
            strncpy_s(reply, size, "reloadscripts gamepass: not on the main thread, or a step not hooked", _TRUNCATE);
            return;
        }
        std::string blob;
        ApplyBlob_t apply = nullptr;
        size_t at = a.find("blob=");
        if (at != std::string::npos) {
            blob = a.substr(at + 5);
            apply = g_applyBlob ? g_applyBlob : ApplyBlobFn();
            if (!apply) {
                strncpy_s(reply, size, "reloadscripts gamepass: the blob step is not where this build has it", _TRUNCATE);
                return;
            }
        }
        g_keepHooks = a.find("keep") != std::string::npos;
        DWORD code = 0;
        uint8_t fails[2] = {};
        bool ok = GamePassGuarded(&code, apply, blob.c_str(), static_cast<uint32_t>(blob.size() + 1), fails);
        g_keepHooks = false;
        char s[160];
        snprintf(s, sizeof s, "reloadscripts gamepass: ok=%d, fault 0x%08lx (hooks %s)%s, blob failures %u %u", ok,
                 code, a.find("keep") != std::string::npos ? "kept on" : "off during it",
                 apply ? ", with the blob" : "", fails[0], fails[1]);
        strncpy_s(reply, size, s, _TRUNCATE);
        return;
    }
    if (a.find("check") != std::string::npos) {
        bool ok = CheckOnly(detail);
        std::string s = std::string("reloadscripts check: ") + (ok ? "" : "failed - ") + detail;
        strncpy_s(reply, size, s.c_str(), _TRUNCATE);
        return;
    }
    bool ok = HotReload(force, detail);
    std::string s = force ? std::string(ok ? "reloadscripts: ok - " : "reloadscripts: failed - ") + detail
                          : "reloadscripts: " + detail;
    strncpy_s(reply, size, s.c_str(), _TRUNCATE);
}

}  // namespace

// --- statement lines. The game compiles its scripts only when the script folders changed; otherwise it
// loads content\content0\x64.final.redscripts. Statement lines (opcode 0x0c, CFunction+0x98) are in the code only
// when the compiler ran as with -debugscripts (scripting system +0x17c = 0); without them w3.script_stack has no
// lines, w3.on_line and the debugger's breakpoints never fire. So TW3SE compiles that way (script_lines, on unless
// tw3se.config or F2 turns it off) and keeps the cache's mode with its size and time in the store: a cache of the
// other mode (or one of unknown mode when lines are wanted) is put aside before the game looks, the game compiles anew.
namespace {
bool g_linesWanted = false;
bool g_linesOff = false;  // wanted, but the scripts do not compile with them (LinesOff)
bool g_cacheReplaced = false;

std::wstring ScriptCachePath() { return GameRoot() + L"\\content\\content0\\x64.final.redscripts"; }

std::string ScriptCacheStamp() {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(ScriptCachePath().c_str(), GetFileExInfoStandard, &fa)) return "";
    char b[64];
    snprintf(b, sizeof b, "%lu.%lu.%lu", fa.nFileSizeLow, fa.ftLastWriteTime.dwHighDateTime, fa.ftLastWriteTime.dwLowDateTime);
    return b;
}

void PrepareScriptCache() {
    std::string f2;
    g_linesWanted = ConfigBool("script_lines", true) && !(VarGet("mod/TW3SE/script_lines", f2) && f2 == "false");
    std::string stamp = ScriptCacheStamp(), rec;
    if (stamp.empty()) {
        g_cacheReplaced = true;  // no cache: the game compiles, in the mode set below
        return;
    }
    VarGet("tw3se/script_cache", rec);
    bool known = rec.size() > 6 && rec.substr(rec.find(':') + 1) == stamp;
    bool hasLines = known && rec.rfind("lines:", 0) == 0;
    if (g_linesWanted && known && rec.rfind("nolines:", 0) == 0) {  // the same scripts failed with lines before
        g_linesOff = true;
        Log("scripts: statement lines off - the installed scripts did not compile with them (tw3se.log.prev has the "
            "error); they come back when a script changes");
        return;
    }
    if (g_linesWanted == hasLines && (known || !g_linesWanted)) return;
    std::wstring aside = ScriptCachePath() + L".tw3se_old";
    DeleteFileW(aside.c_str());
    if (MoveFileW(ScriptCachePath().c_str(), aside.c_str())) {
        g_cacheReplaced = true;
        Log("scripts: compiled scripts put aside (%s), the game compiles them %s statement lines", known ? "other mode" : "mode unknown",
            g_linesWanted ? "with" : "without");
    }
}

// After the start-up pass the game writes the cache a little later: each tick looks until it is there and has stood
// still for a second, then its mode goes into the store and the old one put aside is deleted.
std::string g_lastStamp;
ULONGLONG g_stampSince = 0;
bool g_noteDone = true;

void NoteTick(float, void*) {
    if (g_noteDone) return;
    std::string stamp = ScriptCacheStamp();
    if (stamp.empty() || stamp != g_lastStamp) {
        g_lastStamp = stamp;
        g_stampSince = GetTickCount64();
        return;
    }
    if (GetTickCount64() - g_stampSince < 1000) return;
    g_noteDone = true;
    std::string rec = std::string(g_linesOff ? "nolines:" : g_linesWanted ? "lines:" : "plain:") + stamp;
    VarSet("tw3se/script_cache", &rec);
    DeleteFileW((ScriptCachePath() + L".tw3se_old").c_str());
    Log("scripts: compiled scripts kept %s statement lines", ScriptLinesOn() ? "with" : "without");
}

void NoteScriptCache(bool fallback) {  // after the start-up pass that compiled: wait for the cache the game writes
    if (!g_cacheReplaced && !fallback) return;
    g_noteDone = false;
    static bool registered = false;
    if (!registered) {
        registered = true;
        RegisterTick(&NoteTick, nullptr);
    }
}
}  // namespace

bool ScriptLinesOn() { return g_linesWanted && !g_linesOff; }

namespace {
void LinesOff(const std::string& why) {
    g_linesOff = true;
    Log("scripts: the scripts did not compile with statement lines (%s) - TW3SE compiles them without; w3.on_line, "
        "line breakpoints and the lines in w3.script_stack are off until that error is fixed", why.c_str());
}

void LinesRetry() {
    if (g_linesWanted) g_linesOff = false;
}

// The start-up pass failed after TW3SE put the cache aside: the old one goes back at once, so the game folder is as
// before (a player who sees compile errors from two clashing mods would take the moved cache for the cause).
void PutBackScriptCache() {
    if (!g_cacheReplaced) return;
    std::wstring aside = ScriptCachePath() + L".tw3se_old";
    if (GetFileAttributesW(aside.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (GetFileAttributesW(ScriptCachePath().c_str()) != INVALID_FILE_ATTRIBUTES) return;
    if (MoveFileW(aside.c_str(), ScriptCachePath().c_str()))
        Log("scripts: the game's compiled scripts put back as they were (the compile failed)");
}
}  // namespace

// At process start, before the scripts compile: the step hooks log the start-up pass too (and record its arguments).
void InstallHotReload() {
    if (IsRedkitEditor()) return;  // the editor has its own button for it
    PrepareScriptCache();
    uintptr_t pass = Address("CScriptingSystem::CompileAndApplyAll");
    uintptr_t apply = Address("CScriptingSystem::ApplyCompiledScripts");
    HookAt(reinterpret_cast<void*>(pass), reinterpret_cast<void*>(&PassHook), reinterpret_cast<void**>(&oPass), "pass");
    HookNamed("CScriptingSystem::CompileAllScripts", reinterpret_cast<void*>(&CompileHook),
              reinterpret_cast<void**>(&oCompile));
    HookAt(CallTo(pass, kPrepareHead, sizeof kPrepareHead), reinterpret_cast<void*>(&PrepareHook),
           reinterpret_cast<void**>(&oPrepare), "prepare");
    HookAt(CallTo(pass, kFinishHead, sizeof kFinishHead), reinterpret_cast<void*>(&FinishHook),
           reinterpret_cast<void**>(&oFinish), "finish");
    HookNamed("CScriptingSystem::ApplyCompiledScripts", reinterpret_cast<void*>(&ApplyHook),
              reinterpret_cast<void**>(&oApply));
    HookAt(CallTarget(apply, 0x36), reinterpret_cast<void*>(&RelinkHook), reinterpret_cast<void**>(&oRelink), "relink");
    HookAt(CallTarget(apply, 0xfb), reinterpret_cast<void*>(&SwapHook), reinterpret_cast<void**>(&oSwap), "swap");
    if (ApplyBlob_t blob = ApplyBlobFn()) {  // this build of witcher3.exe only (its bytes checked)
        g_applyBlob = blob;
        HookAt(reinterpret_cast<void*>(blob), reinterpret_cast<void*>(&ApplyBlobHook),
               reinterpret_cast<void**>(&oApplyBlob), "compiled scripts load");
    }
    KeepStartFiles();
    PreScanClashes();
    RegisterCommand("reloadscripts", &CmdReload, "reloadscripts [force|check|gamepass [keep] [blob=<path>]]: the reload state; force = recompile all scripts now; check = compile only; gamepass = the game's own apply as with mod.io mods, with a precompiled.rsblob when given (a test)");
}

}  // namespace tw3se
