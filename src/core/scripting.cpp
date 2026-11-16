// TW3SE core - new native functions for WitcherScript.
//
// At start-up the engine registers its own global script natives. TW3SE adds its own at the same point, before any
// script is compiled, so a script can import them like the game's own:
//     import function TW3SE_GetVersion() : int;
//
// The native calling convention and the registration sequence below follow the MIT-licensed ScriptHookW3 (same
// engine family, see _research/ScriptHookW3). A native is called as native(self, frame, result): `frame` carries the
// script bytecode cursor at +0x30; a native reads its parameters from there (for no parameters, it steps one byte
// past the end-of-arguments marker) and writes its return value through `result`. Registration builds a CName for the
// name, creates the engine's function object, and registers it on the global RTTI system.
//
// Verified in-game on 5.0 (build 0x6abd8695): TW3SE_GetVersion() registers, a script imports it, and a
// call returns 256 (0.1.0). The engine's own allocator is avoided (its signature differs by build) - the CFunction
// is built into our own session-long memory. On by default; a file tw3se\natives.off disables it.
#include "core.h"
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>
#pragma comment(lib, "user32.lib")  // GetAsyncKeyState (TW3SE_KeyPressed)

namespace tw3se {
namespace {

struct ScriptFrame {
    void* context;      // +0x00: the calling script's context (passed through to parameter handlers)
    uint8_t pad1[0x18]; // +0x08 .. +0x1f
    // +0x20: the base an embedded CProperty* (opcode 0x0f out/by-reference argument) resolves its offset
    // against - GScriptNativeTable[0x0f] does `*(prop+0x24) & 0x20 ? *(object+0x20) : object`, then adds
    // `*(prop+0x20)` (FINDINGS_OUTPARAM3.md). Ties into the X3 finding that a latent call's new
    // frame inherits this field from the caller as "CFunction* of the running function" - a compiled
    // function's own locals likely hang off its own +0x20 the same way a script object's vars hang off
    // object+0x20 (scripting.md section 8).
    void* object;       // +0x20 .. +0x27
    uint8_t pad2[0x08]; // +0x28 .. +0x2f
    uint8_t* code;      // +0x30: cursor into the script bytecode (the arguments to read)
};
// These offsets are load-bearing: context at +0x00, object (locals base for 0x0f) at +0x20, the bytecode cursor at
// +0x30. A wrong pad shifted `code` to +0x34 once and faulted every native call - lock them down.
static_assert(offsetof(ScriptFrame, context) == 0x00, "ScriptFrame.context must be +0x00");
static_assert(offsetof(ScriptFrame, object) == 0x20, "ScriptFrame.object must be +0x20");
static_assert(offsetof(ScriptFrame, code) == 0x30, "ScriptFrame.code must be +0x30");
using Native = void (*)(void* self, ScriptFrame* frame, void* result);

// import function TW3SE_GetVersion() : int;  -> (major << 16) | (minor << 8) | patch
void Tw3GetVersion(void* /*self*/, ScriptFrame* frame, void* result) {
    frame->code += 1;  // no parameters: step past the end-of-arguments marker
    if (result)
        *reinterpret_cast<int32_t*>(result) =
            (TW3SE_VERSION_MAJOR << 16) | (TW3SE_VERSION_MINOR << 8) | TW3SE_VERSION_PATCH;
}

// Reading a native's parameters: each argument is an expression evaluated through the engine's generic opcode
// table (GScriptNativeTable). Read the opcode byte at the cursor, index the table, call the handler as
// (context, frame, &out). After the last parameter, skip one terminator byte. (Recipe from the built-in
// funcLogChannel; see docs/ABI_NOTES.md.)
using OpHandler = void (*)(void* context, ScriptFrame* frame, void* out);

void ReadParam(ScriptFrame* frame, void* out) {
    uint8_t op = *frame->code;
    frame->code += 1;
    auto table = reinterpret_cast<OpHandler*>(Address("GScriptNativeTable"));
    if (table && op) {
        OpHandler h = table[op];
        if (h) h(frame->context, frame, out);
    }
}

// import function TW3SE_MountDLC(path : string) : bool;  -> mount a DLC folder into the running game
void Tw3MountDLC(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t buf[16] = {0};  // a String out-buffer begins with a char* to the data (>=16 bytes to be safe)
    ReadParam(frame, buf);
    frame->code += 1;       // skip the end-of-arguments marker
    const char* path = *reinterpret_cast<char**>(buf);
    bool ok = false;
    char reply[256] = {0};
    if (path) {
        ok = LiveLoadDLC(path, reply, sizeof reply);  // mount + register (caller still reloads a save)
        Log("native TW3SE_MountDLC(\"%s\") -> %s", path, reply);
    } else {
        Log("native TW3SE_MountDLC: no path given");
    }
    if (result) *reinterpret_cast<bool*>(result) = ok;
    // The engine-allocated path string is intentionally not freed (avoids a Memory::Free whose signature
    // differs by build); a mount is rare and the leak is negligible.
}

std::wstring Widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

// A WitcherScript string value (6a, docs/ABI_NOTES.md): a heap pointer to the characters (incl. the null
// terminator) and the allocation's byte length (= strlen + 1, there is no separate capacity).
struct WitcherString {
    char* data;
    uint32_t length;
};
// Writes `text` (len bytes) into a native's `result` slot as a WitcherScript string, using the engine's own
// default allocator (the one every native registration uses - Memory::AllocDefault). The result slot of a
// returning native comes in fresh/empty, so the previous buffer is not freed (a filled slot would need
// Memory::Free(data) first). On any failure the string is left empty.
void SetScriptString(void* result, const char* text, size_t len) {
    if (!result) return;
    auto s = reinterpret_cast<WitcherString*>(result);
    s->data = nullptr;
    s->length = 0;
    char* mem = static_cast<char*>(EngineAlloc(len + 1, 1));
    if (!mem) return;
    if (len) memcpy(mem, text, len);
    mem[len] = '\0';
    s->data = mem;
    s->length = static_cast<uint32_t>(len + 1);
}


// import function TW3SE_WriteFile(text : string) : bool;  -> writes `text` to tw3se\script_out.txt
// A capability plain WitcherScript never had: persisting data to disk from a script. Fixed file name; see
// TW3SE_WriteNamed below for the two-parameter variant with a caller-chosen name.
void Tw3WriteFile(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t textBuf[16] = {0};
    ReadParam(frame, textBuf);
    frame->code += 1;  // skip the end-of-arguments marker
    const char* text = *reinterpret_cast<char**>(textBuf);
    bool ok = false;
    if (text) {
        std::wstring path = Dir() + L"\\script_out.txt";
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            ok = WriteFile(h, text, static_cast<DWORD>(strlen(text)), &wrote, nullptr) != 0;
            CloseHandle(h);
        }
        Log("native TW3SE_WriteFile(%u bytes) -> %s", static_cast<unsigned>(strlen(text)), ok ? "ok" : "failed");
    }
    if (result) *reinterpret_cast<bool*>(result) = ok;
}

// A file name a script may use: no path separators, no "..", no characters Windows refuses, and none of Windows'
// device names. "CON", "NUL", "COM1", "LPT1" (also "con.txt") are valid in every folder and open the device: a
// TW3SE_ReadNamed("CON") read from the console and held the main thread (found looking for such gaps).
bool SafeFileName(const char* name) {
    if (!name || !*name || strlen(name) > 200 || strstr(name, "..")) return false;
    for (const char* c = name; *c; ++c)
        if (static_cast<unsigned char>(*c) < 0x20 || strchr("\\/:<>\"|?*", *c)) return false;
    std::string base(name, strcspn(name, "."));  // "con.txt" is CON as well
    while (!base.empty() && base.back() == ' ') base.pop_back();
    for (char& c : base) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    static const char* devices[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$", "CLOCK$"};
    for (const char* d : devices)
        if (base == d) return false;
    if (base.size() == 4 && (base.rfind("COM", 0) == 0 || base.rfind("LPT", 0) == 0) && isdigit(static_cast<unsigned char>(base[3])))
        return false;
    return true;
}

// A caller-named file lives under tw3se\out\ (SafeFileName), so a script can only reach that one folder. Returns
// false (and leaves `path` untouched) for a rejected name.
bool SafeOutPath(const char* name, std::wstring& path) {
    if (!SafeFileName(name)) return false;
    std::wstring dir = Dir() + L"\\out";
    CreateDirectoryW(dir.c_str(), nullptr);
    path = dir + L"\\" + Widen(name);
    return true;
}

// import function TW3SE_WriteNamed(fileName : string, text : string) : bool;  -> writes `text` to tw3se\out\<fileName>
// The first multi-parameter native. Key finding (measured with a parameter probe): the compiler fills
// a native's parameter metadata (CFunction+0x28 array / +0x30 count) from the `import function` declaration - our own
// one-parameter natives already show a populated array although RegisterOne never touches those fields. So a
// multi-parameter native needs nothing special at registration: just read N parameters in declaration order.
void Tw3WriteNamed(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t nameBuf[16] = {0};
    uint8_t textBuf[16] = {0};
    ReadParam(frame, nameBuf);  // parameters are read in declaration order
    ReadParam(frame, textBuf);
    frame->code += 1;           // skip the end-of-arguments marker (once, after the last parameter)
    const char* name = *reinterpret_cast<char**>(nameBuf);
    const char* text = *reinterpret_cast<char**>(textBuf);
    bool ok = false;
    std::wstring path;
    if (text && SafeOutPath(name, path)) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            ok = WriteFile(h, text, static_cast<DWORD>(strlen(text)), &wrote, nullptr) != 0;
            CloseHandle(h);
        }
        Log("native TW3SE_WriteNamed(\"%s\", %u bytes) -> %s", name, static_cast<unsigned>(strlen(text)), ok ? "ok" : "failed");
    } else {
        Log("native TW3SE_WriteNamed: rejected name");
    }
    if (result) *reinterpret_cast<bool*>(result) = ok;
}

// import function TW3SE_AppendNamed(fileName : string, text : string) : bool;  -> appends to tw3se\out\<name>
// Multi-parameter; builds persistent logs/data from WitcherScript across many calls.
void Tw3AppendNamed(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t nameBuf[16] = {0};
    uint8_t textBuf[16] = {0};
    ReadParam(frame, nameBuf);
    ReadParam(frame, textBuf);
    frame->code += 1;
    const char* name = *reinterpret_cast<char**>(nameBuf);
    const char* text = *reinterpret_cast<char**>(textBuf);
    std::wstring path;
    bool ok = false;
    if (name && text && SafeOutPath(name, path)) {
        HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD wrote = 0;
            ok = WriteFile(h, text, static_cast<DWORD>(strlen(text)), &wrote, nullptr) != 0;
            CloseHandle(h);
        }
    }
    if (result) *reinterpret_cast<bool*>(result) = ok;
}

// import function TW3SE_ProbeArg(cn : name);
// RE helper (read-only): logs the raw bytecode the compiler emitted for a `name` argument, so the name-literal
// opcode is confirmed from the live compiled stream before any CName encoding is built - not by trying an opcode
// (which could corrupt the VM), but by reading what the compiler already wrote. Then consumes the argument
// through the engine's own handler so the VM stays consistent.
void LogProbeBytes(const uint8_t* c) {
    char line[256] = {0};
    int n = 0;
    __try {
        for (int i = 0; i < 48; ++i) n += sprintf_s(line + n, sizeof line - n, "%02x ", c[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Log("probearg: bytes = %s", line);
}

void Tw3ProbeArg(void* /*self*/, ScriptFrame* frame, void* /*result*/) {
    LogProbeBytes(frame->code);
    // for a nested call as the argument, the function the compiler refers to (pointer and opcode)
    void* inner = FindGlobalFn("TW3SE_ProbeName");
    uint32_t op = 0;
    if (inner) SafeRead(reinterpret_cast<char*>(inner) + 0xe0, &op, 4);
    Log("probearg: TW3SE_ProbeName CFunction %p opcode %u (0x%x)", inner, op, op);
    uint8_t nameBuf[16] = {0};
    ReadParam(frame, nameBuf);  // the engine's own opcode handler (safe); advances frame->code
    frame->code += 1;
    Log("probearg: after read, out[0..3] = %08x", *reinterpret_cast<uint32_t*>(nameBuf));
}

// import function TW3SE_ProbeName(v : int) : name;  -> the name "tw3se_probe_<v>" (a nested call to measure)
void Tw3ProbeName(void* /*self*/, ScriptFrame* frame, void* result) {
    int32_t v = 0;
    ReadParam(frame, &v);
    frame->code += 1;
    char buf[48];
    sprintf_s(buf, "tw3se_probe_%d", v);
    Log("probename: called with %d", v);
    if (result) *reinterpret_cast<uint32_t*>(result) = InternName(buf);
}

// Probes for the compiler's bytecode (scripts/tw3se_probe_more.ws). One handler
// under several names with 1 or 2 parameters of any type: per parameter it logs the bytes at the cursor, reads the
// parameter through the engine's own handler into a large buffer, and logs which bytes of the frame (first
// 0x100) the read changed (an out parameter is expected to leave the variable's address there).
void LogFrameDiff(const uint8_t* before, const uint8_t* after, const char* tag) {
    char line[400] = {0};
    int n = 0;
    for (size_t off = 0; off < 0x100 && n < 360; off += 8) {
        uint64_t a, b;
        memcpy(&a, before + off, 8);
        memcpy(&b, after + off, 8);
        if (a != b && off != 0x30) n += sprintf_s(line + n, sizeof line - n, " +0x%zx:%llx", off, (unsigned long long)b);
    }
    Log("probe %s: frame changed at%s", tag, n ? line : " (nothing besides the cursor)");
}

bool CopyFrameGuarded(void* dst, const void* src) {
    __try {
        memcpy(dst, src, 0x100);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The whole frame as qwords with offsets (first 0x90 bytes): for the out-parameter analysis, to see whether the
// address the 0x0f handler yields matches a frame field (local-base pointer). Goes to the log file, not the console.
void LogFrameFull(const void* frame, const char* tag) {
    for (size_t off = 0; off < 0x90; off += 0x20) {
        uint64_t q[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k) memcpy(&q[k], static_cast<const char*>(frame) + off + k * 8, 8);
        Log("probe %s frame +0x%02zx: %016llx %016llx %016llx %016llx", tag, off, (unsigned long long)q[0],
            (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3]);
    }
}

void ProbeParams(const char* tag, ScriptFrame* frame, int count, bool dumpFrame = false) {
    if (dumpFrame) LogFrameFull(frame, tag);
    for (int i = 0; i < count; ++i) {
        LogProbeBytes(frame->code);
        alignas(16) uint8_t before[0x100] = {0}, after[0x100] = {0}, out[256] = {0};
        bool snap = CopyFrameGuarded(before, frame);
        ReadParam(frame, out);
        if (snap && CopyFrameGuarded(after, frame)) LogFrameDiff(before, after, tag);
        uint64_t v0, v1;
        memcpy(&v0, out, 8);
        memcpy(&v1, out + 8, 8);
        Log("probe %s: param %d out = %016llx %016llx, frame %p, out %p", tag, i, (unsigned long long)v0,
            (unsigned long long)v1, static_cast<void*>(frame), static_cast<void*>(out));
    }
    LogProbeBytes(frame->code);  // the end marker should be next
    frame->code += 1;
}
template <int N> void Tw3ProbeN(void*, ScriptFrame* frame, void*) { ProbeParams(N == 1 ? "p1" : "p2", frame, N); }
void Tw3ProbeOut(void*, ScriptFrame* frame, void*) { ProbeParams("out", frame, 1, true); }  // dumps the frame

void* FindMethod(void* object, const char* methodName);  // below

// The CFunction pointers a probe run refers to, so the bytes above can be matched to them.
void LogProbeFunctions() {
    for (const char* n : {"tw3se_probe_sname", "TW3SE_ProbeArg", "TW3SE_Probe2a", "TW3SE_Probe1o", "TW3SE_Probe1h"})
        Log("probe: CFunction %s = %p", n, FindGlobalFn(n));
    void* player = GetThePlayer();
    void* m = player ? FindMethod(player, "TW3SE_ProbeM") : nullptr;
    void* v = player ? FindMethod(player, "GetVoicetag") : nullptr;
    Log("probe: CFunction CR4Player.TW3SE_ProbeM = %p, GetVoicetag = %p, player %p, class %p", m, v, player,
        player ? GetObjectClass(player) : nullptr);
    void* cls = FindClassByName("TW3SE_ProbeClass");
    Log("probe: CClass TW3SE_ProbeClass = %p", cls);
}

void Tw3ProbeFns(void*, ScriptFrame* frame, void*) {
    frame->code += 1;
    LogProbeFunctions();
}

// import function TW3SE_EntityGuid(e : CEntity) : string;
// Returns the entity's own 16-byte GUID (CEntity+0xcc, the field GetGuidHash reads) as four hex dwords
// "d0 d1 d2 d3", or "" for an empty handle. This is exactly the key world-edit persistence matches on, so an
// editor can read a selected object's GUID here and hand it to worldremove/worldmove. It also exercises an
// entity parameter: the handler resolves either a direct object pointer or a refcounted handle (object at +0x08).
void Tw3EntityGuid(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t buf[16] = {0};
    ReadParam(frame, buf);
    frame->code += 1;
    char out[64] = {0};
    __try {
        // An entity parameter arrives as a refcounted handle (verified in game: the raw value is a handle, not
        // an object pointer, so GetObjectClass on it faults). ResolveHandle returns the real object at handle+0x08.
        void* obj = ResolveHandle(*reinterpret_cast<void**>(buf));
        if (obj && GetObjectClass(obj)) {
            const uint32_t* g = reinterpret_cast<const uint32_t*>(reinterpret_cast<char*>(obj) + 0xcc);
            _snprintf_s(out, _TRUNCATE, "%08x %08x %08x %08x", g[0], g[1], g[2], g[3]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    SetScriptString(result, out, strlen(out));
}

// import function TW3SE_FileSize(fileName : string) : int;  -> size of tw3se\out\<name> in bytes, -1 if missing
void Tw3FileSize(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t nameBuf[16] = {0};
    ReadParam(frame, nameBuf);
    frame->code += 1;
    const char* name = *reinterpret_cast<char**>(nameBuf);
    std::wstring path;
    int32_t size = -1;
    if (name && SafeOutPath(name, path)) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) &&
            !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            size = static_cast<int32_t>(fad.nFileSizeLow);  // files here are small; low 32 bits suffice
    }
    if (result) *reinterpret_cast<int32_t*>(result) = size;
}

// import function TW3SE_DeleteNamed(fileName : string) : bool;  -> deletes tw3se\out\<name>
void Tw3DeleteNamed(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t nameBuf[16] = {0};
    ReadParam(frame, nameBuf);
    frame->code += 1;
    const char* name = *reinterpret_cast<char**>(nameBuf);
    std::wstring path;
    bool ok = false;
    if (name && SafeOutPath(name, path)) ok = DeleteFileW(path.c_str()) != 0;
    if (result) *reinterpret_cast<bool*>(result) = ok;
}

// import function TW3SE_ReadNamed(fileName : string) : string;  -> contents of tw3se\out\<name>, "" if missing
// The first string-RETURNING native. A capability plain WitcherScript never had: reading file data back into a
// script. Builds the return value as a WitcherScript string via SetScriptString (engine allocator).
void Tw3ReadNamed(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t nameBuf[16] = {0};
    ReadParam(frame, nameBuf);
    frame->code += 1;
    const char* name = *reinterpret_cast<char**>(nameBuf);
    std::string contents;
    std::wstring path;
    if (name && SafeOutPath(name, path)) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            if (GetFileType(h) == FILE_TYPE_DISK && GetFileSizeEx(h, &sz) && sz.QuadPart > 0 &&
                sz.QuadPart <= (1 << 20)) {  // a file on disk (a device never reads here), cap 1 MiB
                contents.resize(static_cast<size_t>(sz.QuadPart));
                DWORD got = 0;
                if (ReadFile(h, &contents[0], static_cast<DWORD>(sz.QuadPart), &got, nullptr))
                    contents.resize(got);
                else
                    contents.clear();
            }
            CloseHandle(h);
        }
    }
    SetScriptString(result, contents.c_str(), contents.size());
}

// import function TW3SE_Log(text : string);  -> writes a line to tw3se.log
void Tw3Log(void* /*self*/, ScriptFrame* frame, void* /*result*/) {
    uint8_t buf[16] = {0};
    ReadParam(frame, buf);
    frame->code += 1;
    const char* t = *reinterpret_cast<char**>(buf);
    Log("script: %s", t ? t : "(null)");
}

// import function TW3SE_GetConfig(key : string) : string;  -> value of `key` from tw3se\tw3se.config, "" if unset
// Lets a script read mod configuration (the basis for in-game settings menus).
void Tw3GetConfig(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t keyBuf[16] = {0};
    ReadParam(frame, keyBuf);
    frame->code += 1;
    const char* key = *reinterpret_cast<char**>(keyBuf);
    std::string val = key ? ConfigGet(key, "") : std::string();
    SetScriptString(result, val.c_str(), val.size());
}

// import function TW3SE_StringToName(value : string) : name;  -> the name of a text ('None' for "")
// WitcherScript has NameToString only; a name built at run time (a tag, an item, a fact) needs this.
void Tw3StringToName(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t buf[16] = {0};
    ReadParam(frame, buf);
    frame->code += 1;
    const char* s = *reinterpret_cast<char**>(buf);
    if (result) *reinterpret_cast<uint32_t*>(result) = s && *s && strcmp(s, "None") != 0 ? InternName(s) : 0;
}

// import function TW3SE_FileExists(fileName : string) : bool;  -> is tw3se\<fileName> present?
void Tw3FileExists(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t buf[16] = {0};
    ReadParam(frame, buf);
    frame->code += 1;
    const char* name = *reinterpret_cast<char**>(buf);
    bool ok = false;
    if (SafeFileName(name))
        ok = GetFileAttributesW((Dir() + L"\\" + Widen(name)).c_str()) != INVALID_FILE_ATTRIBUTES;
    if (result) *reinterpret_cast<bool*>(result) = ok;
}

// import function TW3SE_KeyPressed(vk : int) : bool;  -> is the virtual key currently down? (hotkey frameworks)
void Tw3KeyPressed(void* /*self*/, ScriptFrame* frame, void* result) {
    int32_t vk = 0;
    ReadParam(frame, &vk);  // an int parameter uses a 4-byte out buffer
    frame->code += 1;
    bool down = vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (result) *reinterpret_cast<bool*>(result) = down;
}

// ---------------------------------------------------------------------------------------------------------
// Persistent variable store: a key/value store a mod can read and write from WitcherScript and that survives
// across sessions. WriteNamed can already dump text to disk, but there was no structured state a mod could
// just keep ("have I done this yet?", a counter, a found-location flag). Backed by one file, tw3se\vars.txt,
// loaded once on first use. A change only marks the store; the tick writes it at most once a second and the
// game's exit writes the rest (a write on every change stands the game still for 14 s at 5000 SetVar in one
// tick). Written atomically (temp file + replace), so a crash mid-write can never leave a half-written
// store; a crash loses at most the last second. One global store for now; per-save scoping can key off the save
// name later. Format: one "key<TAB>value" line per entry, with backslash/newline/tab escaped in both fields.
std::map<std::string, std::string> g_vars;
bool g_varsLoaded = false;
bool g_varsDirty = false;
size_t g_varsBytes = 0;  // keys and values together (the limit in TW3SE_SetVar)
ULONGLONG g_varsSaved = 0;

std::string VarEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c == '\r') o += "\\r";  // a value ending in \r keeps it at the next load
        else o += c;
    }
    return o;
}
std::string VarUnescape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            o += (n == 'n') ? '\n' : (n == 't') ? '\t' : (n == 'r') ? '\r' : n;
        } else {
            o += s[i];
        }
    }
    return o;
}
void VarsLoad() {
    if (g_varsLoaded) return;
    g_varsLoaded = true;
    std::wstring path = Dir() + L"\\vars.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    std::string buf;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= (64 << 20)) {  // far above what SetVar lets in
        buf.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        if (!ReadFile(h, &buf[0], static_cast<DWORD>(sz.QuadPart), &got, nullptr)) buf.clear();
        else buf.resize(got);
    }
    CloseHandle(h);
    size_t start = 0;
    while (start < buf.size()) {
        size_t nl = buf.find('\n', start);
        if (nl == std::string::npos) nl = buf.size();
        std::string line = buf.substr(start, nl - start);
        start = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (tab != std::string::npos)
            g_vars[VarUnescape(line.substr(0, tab))] = VarUnescape(line.substr(tab + 1));
    }
    for (auto& kv : g_vars) g_varsBytes += kv.first.size() + kv.second.size();
}
bool VarsSave() {
    std::string out;
    for (auto& kv : g_vars) {
        out += VarEscape(kv.first);
        out += '\t';
        out += VarEscape(kv.second);
        out += '\n';
    }
    std::wstring path = Dir() + L"\\vars.txt";
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = out.empty() || WriteFile(h, out.data(), static_cast<DWORD>(out.size()), &wrote, nullptr) != 0;
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

// import function TW3SE_SetVar(key : string, value : string) : bool;  -> store value under key, persisted
void Tw3SetVar(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t keyBuf[16] = {0}, valBuf[16] = {0};
    ReadParam(frame, keyBuf);
    ReadParam(frame, valBuf);
    frame->code += 1;
    const char* key = *reinterpret_cast<char**>(keyBuf);
    const char* val = *reinterpret_cast<char**>(valBuf);
    bool ok = false;
    if (key && *key && val) {
        VarsLoad();
        // a limit on the way in (the load reads at most 4 MiB; one mod that stored more would lose every mod's
        // variables at the next start): 1 KiB a key, 64 KiB a value, 16 MiB in all
        size_t klen = strlen(key), vlen = strlen(val);
        auto it = g_vars.find(key);
        size_t old = it == g_vars.end() ? 0 : klen + it->second.size();
        if (klen > 1024 || vlen > (64u << 10) || g_varsBytes - old + klen + vlen > (16u << 20)) {
            static int logged = 0;
            if (logged++ < 5) Log("vars: a value of %zu bytes refused (1 KiB a key, 64 KiB a value, 16 MiB in all)", vlen);
        } else {
            g_varsBytes = g_varsBytes - old + klen + vlen;
            g_vars[key] = val;
            g_varsDirty = ok = true;   // written by TickVars
        }
    }
    if (result) *reinterpret_cast<bool*>(result) = ok;
}

// import function TW3SE_GetVar(key : string) : string;  -> stored value, or "" if the key is unset
void Tw3GetVar(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t keyBuf[16] = {0};
    ReadParam(frame, keyBuf);
    frame->code += 1;
    const char* key = *reinterpret_cast<char**>(keyBuf);
    VarsLoad();
    std::string val;
    if (key) { auto it = g_vars.find(key); if (it != g_vars.end()) val = it->second; }
    SetScriptString(result, val.c_str(), val.size());
}

// import function TW3SE_HasVar(key : string) : bool;  -> is the key set?
void Tw3HasVar(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t keyBuf[16] = {0};
    ReadParam(frame, keyBuf);
    frame->code += 1;
    const char* key = *reinterpret_cast<char**>(keyBuf);
    VarsLoad();
    bool has = key && g_vars.find(key) != g_vars.end();
    if (result) *reinterpret_cast<bool*>(result) = has;
}

// import function TW3SE_DeleteVar(key : string) : bool;  -> remove the key; true if it existed and is gone
void Tw3DeleteVar(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t keyBuf[16] = {0};
    ReadParam(frame, keyBuf);
    frame->code += 1;
    const char* key = *reinterpret_cast<char**>(keyBuf);
    bool ok = false;
    if (key) {
        VarsLoad();
        auto it = g_vars.find(key);
        if (it != g_vars.end()) {
            g_varsBytes -= it->first.size() + it->second.size();
            g_vars.erase(it);
            g_varsDirty = ok = true;
        }
    }
    if (result) *reinterpret_cast<bool*>(result) = ok;
}

// import function TW3SE_KeyJustPressed(vk : int) : bool;  -> true only on the frame the key goes from up to
// down (edge-detected), so a hotkey fires once and does not repeat while held. Call it once per frame per key
// (the usual "poll in an update loop" pattern); the previous state is tracked per key between calls.
std::map<int, bool> g_keyWas;
void Tw3KeyJustPressed(void* /*self*/, ScriptFrame* frame, void* result) {
    int32_t vk = 0;
    ReadParam(frame, &vk);
    frame->code += 1;
    bool down = vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool was = g_keyWas[vk];
    g_keyWas[vk] = down;
    if (result) *reinterpret_cast<bool*>(result) = down && !was;
}

// import function TW3SE_GetConfigKey(setting : string) : int;  -> a virtual-key code from tw3se.config, 0 if unset
// Lets a user rebind a mod's hotkey in tw3se.config (e.g. `teleport_key = 0x74`) without touching the script.
// Accepts decimal or 0x-hex. Pair with TW3SE_KeyPressed / TW3SE_KeyJustPressed. (The parameter is not called
// `name` on purpose: `name` is a WitcherScript type keyword and cannot be used as a parameter name.)
void Tw3GetConfigKey(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t keyBuf[16] = {0};
    ReadParam(frame, keyBuf);
    frame->code += 1;
    const char* setting = *reinterpret_cast<char**>(keyBuf);
    int32_t vk = 0;
    if (setting) {
        std::string v = ConfigGet(setting, "");
        if (!v.empty()) vk = static_cast<int32_t>(strtol(v.c_str(), nullptr, 0));  // base 0: 0x.. hex else decimal
    }
    if (result) *reinterpret_cast<int32_t*>(result) = vk;
}

// Hot-reload: recompile the WitcherScript sources and swap in the new functions while the game runs (change a
// script, see it without reloading the game). Engine entry (docs/engine/scripting.md section 9):
// CScriptingSystem::ReloadScripts(this, param2). `this` is the GScriptGlobals singleton (one deref of the
// GScriptGlobals pointer - the same base GetThePlayer/GetTheGame use); param2 is passed straight through, we
// use nullptr. Runs on the main thread (console handlers and script natives both already run there).
//
// Two safety rules:
//  1. Guard byte this+0x175 must be != 0 or ReloadScripts is a silent no-op returning false (likely a dev/
//     editor state). We read it first and report, rather than call into a wordless no-op.
//  2. Pointer safety across a reload is not guaranteed - any CFunction*/CClass*/handle/opcode resolved before
//     a reload must be treated as invalid afterwards and re-resolved. The core caches none of these across a
//     reload; plugins are warned in the docs.
// The call itself is SEH-guarded so a fault is contained and reported, not a hard crash.
using ReloadScripts_t = bool (*)(void* self, void* param2);

bool CallReloadGuarded(ReloadScripts_t fn, void* self, DWORD* code) {
    __try {
        return fn(self, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

bool ReloadScriptsNow(std::string& detail) {
    auto fn = Fn<ReloadScripts_t>("CScriptingSystem::ReloadScripts");
    uintptr_t gaddr = Address("GScriptGlobals");
    void* self = gaddr ? *reinterpret_cast<void**>(gaddr) : nullptr;
    if (!fn || !self) {
        detail = "reload entry or scripting singleton not resolved on this build - no action taken";
        Log("reloadscripts: %s (fn=%p self=%p)", detail.c_str(), reinterpret_cast<void*>(fn), self);
        return false;
    }
    // Rule 1: the guard byte. 0 -> the engine would do nothing; do not even call, just report.
    uint8_t guard = *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(self) + 0x175);
    if (!guard) {
        detail = "engine guard this+0x175=0: runtime reload not enabled in this game state (likely a dev-only "
                 "path) - not calling";
        Log("reloadscripts: %s", detail.c_str());
        return false;
    }
    Log("reloadscripts: guard set (0x175=%u), invoking CScriptingSystem::ReloadScripts(self=%p)", guard, self);
    DWORD code = 0;
    bool ok = CallReloadGuarded(fn, self, &code);
    if (code) {
        detail = "ReloadScripts faulted (exception 0x" + std::to_string(code) + ") - treat cached pointers as invalid";
        Log("reloadscripts: exception 0x%08x during reload", code);
        return false;
    }
    uint8_t failFlag = *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(self) + 0x176);  // 1 = compile error
    detail = ok ? "reload ran (recompiled); re-resolve any cached script pointers"
                : ("reload returned false (compile-error flag 0x176=" + std::to_string(failFlag) + ")");
    Log("reloadscripts: done ok=%d compileErrFlag(0x176)=%u", static_cast<int>(ok), failFlag);
    return ok;
}

// import function TW3SE_ReloadScripts() : bool;  -> recompile scripts at runtime; true if the reload actually ran
void Tw3ReloadScripts(void* /*self*/, ScriptFrame* frame, void* result) {
    frame->code += 1;  // no parameters
    std::string detail;
    bool ok = ReloadScriptsNow(detail);
    if (result) *reinterpret_cast<bool*>(result) = ok;
}

// A script name: a 32-bit index into the engine's name pool (layout as in the MIT ScriptHookW3, w3types).
struct CName {
    uint32_t index;
};

// Named engine entry points (resolved by signature in addresses.cpp).
using NamePoolGet_t = void* (*)();
using NamePoolAdd_t = uint32_t (*)(void* pool, const char* name);  // proven in content.cpp (mounting)
using CFunctionCtor_t = void* (*)(void* mem, const CName* name, Native native);
using GetRtti_t = void* (*)();
using RegisterGlobal_t = void (*)(void* rtti, void* fn);
using RegisterCoreNatives_t = void (*)();

RegisterCoreNatives_t g_origRegisterCore = nullptr;

// Finds a globally registered function (incl. built-in engine natives) by its name-pool id, via the
// CRTTISystem function hashmap. Offsets byte-confirmed (docs/engine/scripting.md section 7).
void* FindGlobalFunction(void* rtti, uint32_t id) {
    if (!rtti) return nullptr;
    uint32_t bucketCount = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(rtti) + 0x40);
    if (!bucketCount) return nullptr;
    void** buckets = *reinterpret_cast<void***>(reinterpret_cast<char*>(rtti) + 0x60);
    if (!buckets) return nullptr;
    void* node = buckets[id % bucketCount];
    while (node) {
        if (*reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(node) + 0x10) == id)
            return *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x8);
        node = *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x18);
    }
    return nullptr;
}

// Calls a globally-registered native from C++ with string arguments, synthesizing the exact argument bytecode
// the compiler emits (measured in-game): a string literal is opcode 0x07 + int32 length + raw bytes; the
// end-of-arguments marker is one byte (0x20) that the handler only skips, never reads. The native's handler is
// GScriptNativeTable[CFunction+0xe0]; we build a CScriptStackFrame whose cursor (frame+0x30) points at the
// synthesized bytecode and whose context (frame+0x00) is `self`, then invoke it. The result (int/bool) is
// written to `result`. Returns false if the function or table entry is missing. (String-argument globals for
// now; object/handle arguments need their own push opcode - future.)
bool CallGlobalFunction(const char* name, const std::vector<std::string>& strArgs, void* self, void* result) {
    auto poolGet = Fn<NamePoolGet_t>("CNamesPool::Get");
    auto poolAdd = Fn<NamePoolAdd_t>("CNamesPool::AddEntry");
    auto getRtti = Fn<GetRtti_t>("SRTTI::Get");
    if (!poolGet || !poolAdd || !getRtti) return false;
    void* fn = FindGlobalFunction(getRtti(), poolAdd(poolGet(), name));
    if (!fn) return false;
    uint32_t opcode = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(fn) + 0xe0);
    auto table = reinterpret_cast<OpHandler*>(Address("GScriptNativeTable"));
    if (!table) return false;
    OpHandler h = table[opcode];
    if (!h) return false;
    std::vector<uint8_t> bc;
    for (const std::string& a : strArgs) {
        bc.push_back(0x07);  // push string literal
        uint32_t len = static_cast<uint32_t>(a.size());
        bc.insert(bc.end(), reinterpret_cast<uint8_t*>(&len), reinterpret_cast<uint8_t*>(&len) + 4);
        bc.insert(bc.end(), a.begin(), a.end());
    }
    bc.push_back(0x20);  // end-of-arguments marker (skipped, never read)
    ScriptFrame frame{};
    frame.context = self;
    frame.code = bc.data();
    h(self, &frame, result);
    return true;
}

// Builds the string-argument bytecode + end marker the compiler would emit (shared by global and method calls).
std::vector<uint8_t> BuildArgBytecode(const std::vector<std::string>& strArgs) {
    std::vector<uint8_t> bc;
    for (const std::string& a : strArgs) {
        bc.push_back(0x07);  // push string literal
        uint32_t len = static_cast<uint32_t>(a.size());
        bc.insert(bc.end(), reinterpret_cast<uint8_t*>(&len), reinterpret_cast<uint8_t*>(&len) + 4);
        bc.insert(bc.end(), a.begin(), a.end());
    }
    bc.push_back(0x20);  // end-of-arguments marker
    return bc;
}

// Finds a method-native on an object's class (incl. inherited) by name. Method-natives live in the class's own
// function list (CClass+0x48 pointer / +0x50 count - same layout as the property list), not the global
// hashmap; walk the parent chain via CClass+0x10 (6a, scripting.md section 3). Returns the CFunction* or null.
void* FindMethod(void* object, const char* methodName) {
    void* cls = GetObjectClass(object);
    if (!cls || !methodName) return nullptr;
    // Match by resolved name string (GetCNameString of CFunction+0x10): a method's stored CName id does not
    // compare equal to a freshly AddEntry'd id the way global/property ids do, so match on the text instead.
    for (char* c = reinterpret_cast<char*>(cls); c; c = *reinterpret_cast<char**>(c + 0x10)) {
        void** funcs = *reinterpret_cast<void***>(c + 0x48);
        uint32_t count = *reinterpret_cast<uint32_t*>(c + 0x50);
        if (!funcs || count > 8192) continue;  // cap guards a wrong offset from reading garbage
        for (uint32_t i = 0; i < count; ++i) {
            char* f = reinterpret_cast<char*>(funcs[i]);
            if (!f) continue;
            const char* fn = GetCNameString(*reinterpret_cast<uint32_t*>(f + 0x10));
            if (fn && strcmp(fn, methodName) == 0) return f;
        }
    }
    return nullptr;
}

// Calls a method-native on `object` with string args. Same invoke as a global (CFunction+0xe0 ->
// GScriptNativeTable[idx]) but `self` is passed via the context (first handler argument). result receives the
// return (int/bool/pointer). Returns false if the method or table entry is missing.
bool CallMethod(void* object, const char* methodName, const std::vector<std::string>& strArgs, void* result) {
    void* fn = FindMethod(object, methodName);
    if (!fn) return false;
    uint32_t opcode = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(fn) + 0xe0);
    // Method-natives live in a SEPARATE table from global natives: GScriptMethodTable, 24-byte entries, the
    // handler pointer at byte 0 of each entry (6a, CFunction::CFunctionMethod). The same opcode value indexes
    // this table, not GScriptNativeTable (whose slot stays null for methods - that was the earlier mystery).
    char* mtable = reinterpret_cast<char*>(Address("GScriptMethodTable"));
    if (!mtable) return false;
    OpHandler h = *reinterpret_cast<OpHandler*>(mtable + static_cast<size_t>(opcode) * 24);
    if (!h) return false;
    std::vector<uint8_t> bc = BuildArgBytecode(strArgs);
    ScriptFrame frame{};
    frame.context = object;  // the method's `self` is passed via the context
    frame.code = bc.data();
    h(object, &frame, result);
    return true;
}

// Builds argument bytecode from a space-separated typespec, with the literal push-opcodes measured in game:
//   i:N  int   (0x04 + int32)      f:N  float  (0x06 + float32)
//   s:T  string(0x07 + len + bytes) b:1  bool true (0x0a, no operand)   b:0  bool false (0x0b, no operand)
//   n:T  name  (0x08 + int32 CName id) - the compiler interns the name at compile time and embeds its id;
//              we intern through the same CNamesPool, so the bytecode is byte-identical to a compiled call
//              (confirmed in game via TW3SE_ProbeArg: 'fire' -> 08 d4 23 00 00).
//   (both bool literals read from the compiled stream with TW3SE_ProbeArg: false -> 0b, true -> 0a)
// A bare token (no "x:" prefix) is a string. Only these are safe to synthesize; other types have unknown opcodes
// (a wrong opcode can corrupt the VM), so an unsupported token sets *ok=false and the caller must not invoke.
// The end marker 0x20 always closes the stream.
std::vector<uint8_t> BuildArgsFromSpec(const char* spec, bool* ok) {
    *ok = true;
    std::vector<uint8_t> bc;
    auto put = [&](const void* p, size_t n) { bc.insert(bc.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    char buf[1024] = {0};
    if (spec) strncpy_s(buf, spec, _TRUNCATE);
    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, " ", &ctx); tok; tok = strtok_s(nullptr, " ", &ctx)) {
        char type = 's';
        const char* val = tok;
        if (tok[0] && tok[1] == ':') { type = tok[0]; val = tok + 2; }
        if (type == 'i') { bc.push_back(0x04); int32_t v = static_cast<int32_t>(strtol(val, nullptr, 0)); put(&v, 4); }
        else if (type == 'f') { bc.push_back(0x06); float v = strtof(val, nullptr); put(&v, 4); }
        else if (type == 'e') {  // an enum's value: 0x09 and one byte (docs/engine/bytecode.md 6; > 255 unmeasured)
            long v = strtol(val, nullptr, 0);
            if (v < 0 || v > 255) *ok = false;
            else { bc.push_back(0x09); bc.push_back(static_cast<uint8_t>(v)); }
        }
        else if (type == 'b') {
            if (strcmp(val, "1") == 0 || _stricmp(val, "true") == 0) bc.push_back(0x0a);
            else if (strcmp(val, "0") == 0 || _stricmp(val, "false") == 0) bc.push_back(0x0b);
            else *ok = false;
        }
        else if (type == 'n') {
            auto poolGet = Fn<NamePoolGet_t>("CNamesPool::Get");
            auto poolAdd = Fn<NamePoolAdd_t>("CNamesPool::AddEntry");
            if (poolGet && poolAdd) { bc.push_back(0x08); uint32_t id = poolAdd(poolGet(), val); put(&id, 4); }
            else *ok = false;
        }
        else { bc.push_back(0x07); uint32_t len = static_cast<uint32_t>(strlen(val)); put(&len, 4); put(val, len); }
    }
    bc.push_back(0x20);
    return bc;
}

// Call a global native / a method with a typespec (int/float/string/bool-true), using the measured push-
// opcodes. Mirror the invoke of the string-arg versions above, but build typed bytecode. Return false if the
// function is missing or the typespec has an unsupported token.
bool CallGlobalSpec(const char* name, const char* spec, void* self, void* result) {
    bool ok = true;
    std::vector<uint8_t> bc = BuildArgsFromSpec(spec, &ok);
    if (!ok) return false;
    auto poolGet = Fn<NamePoolGet_t>("CNamesPool::Get");
    auto poolAdd = Fn<NamePoolAdd_t>("CNamesPool::AddEntry");
    auto getRtti = Fn<GetRtti_t>("SRTTI::Get");
    if (!poolGet || !poolAdd || !getRtti) return false;
    void* fn = FindGlobalFunction(getRtti(), poolAdd(poolGet(), name));
    if (!fn) return false;
    uint32_t opcode = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(fn) + 0xe0);
    auto table = reinterpret_cast<OpHandler*>(Address("GScriptNativeTable"));
    if (!table || !table[opcode]) return false;
    ScriptFrame frame{};
    frame.context = self;
    frame.code = bc.data();
    table[opcode](self, &frame, result);
    return true;
}
bool CallMethodSpec(void* object, const char* methodName, const char* spec, void* result) {
    bool ok = true;
    std::vector<uint8_t> bc = BuildArgsFromSpec(spec, &ok);
    if (!ok) return false;
    void* fn = FindMethod(object, methodName);
    if (!fn) return false;
    // (a method written in WitcherScript - neither native nor imported: CNode.AddTag - has no handler in the table;
    // its body runs through the VM, as Lua's calls run one. Before, AddTag from a world edit said ok and did nothing)
    uint32_t flags = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(fn) + 0x18);
    if (!(flags & 1) && !(flags & 8)) {
        bool faulted = false;
        return InvokeScript(fn, object, bc.data(), result, &faulted) && !faulted;
    }
    uint32_t opcode = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(fn) + 0xe0);
    char* mtable = reinterpret_cast<char*>(Address("GScriptMethodTable"));
    if (!mtable) return false;
    OpHandler h = *reinterpret_cast<OpHandler*>(mtable + static_cast<size_t>(opcode) * 24);
    if (!h) return false;
    ScriptFrame frame{};
    frame.context = object;
    frame.code = bc.data();
    h(object, &frame, result);
    return true;
}

// The template an entity was made from, handed to the script as an object (CEntityTemplate), so a script can ask
// it what an editor needs: GetAppearanceNames(template, names) lists the looks the entity can switch to. CEntity's
// `template` property holds a handle - a pointer to the template's handle block (refs @0, flags @4, object @8).
// A handle return is that pointer in the result with one reference more, the way the engine's own natives return
// one (EntityHandleGet, 0x0236e260). Null when the entity has none. SEH-guarded, no C++ objects inside (C2712).
void* TemplateBlockGuarded(void* ent) {
    __try {
        void* cls = GetObjectClass(ent);
        void* prop = cls ? FindProperty(cls, "template") : nullptr;
        void* field = prop ? PropertyPtr(ent, prop) : nullptr;
        void* block = field ? *reinterpret_cast<void**>(field) : nullptr;
        return block && ResolveHandle(block) ? block : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The effects an entity's template defines, comma-separated: what PlayEffect accepts on it. A cooked template keeps
// them in `cookedEffects` (CEntityTemplate+0xe0: an array {data, count} of entries whose first field is the effect's
// CName), an uncooked one in `effects` (+0xa4: CFXDefinition objects with a `name`). The size of a cooked entry is
// not in the script declarations; the stride is taken where every entry yields a known name (16, 24 or 32 bytes).
// Offsets from the RTTI property list (props CEntityTemplate). SEH-guarded, no C++ objects inside (C2712).
const char* CookedEffectName(char* data, uint32_t i, uint32_t stride) {
    return GetCNameString(*reinterpret_cast<uint32_t*>(data + static_cast<size_t>(i) * stride));
}

// Appends ",name" to the list unless it is there already (a template lists an effect cooked and uncooked).
void AddName(char* out, size_t size, const char* n) {
    size_t len = strlen(n);
    for (const char* p = out; (p = strstr(p, n)) != nullptr; p += len)
        if ((p == out || p[-1] == ',') && (p[len] == ',' || p[len] == 0)) return;
    if (out[0]) strncat_s(out, size, ",", _TRUNCATE);
    strncat_s(out, size, n, _TRUNCATE);
}

bool EffectsGuarded(void* ent, char* out, size_t size) {
    out[0] = 0;
    __try {
        void* block = TemplateBlockGuarded(ent);
        char* tmpl = block ? reinterpret_cast<char*>(ResolveHandle(block)) : nullptr;
        if (!tmpl) return false;
        void* cls = GetObjectClass(tmpl);
        void* cooked = cls ? FindProperty(cls, "cookedEffects") : nullptr;
        void* plain = cls ? FindProperty(cls, "effects") : nullptr;
        char* arr = cooked ? reinterpret_cast<char*>(PropertyPtr(tmpl, cooked)) : nullptr;
        if (arr) {
            char* data = *reinterpret_cast<char**>(arr);
            uint32_t count = *reinterpret_cast<uint32_t*>(arr + 8);
            if (data && count && count < 256) {
                static const uint32_t strides[] = {0x10, 0x18, 0x20};
                for (uint32_t stride : strides) {
                    bool all = true;
                    for (uint32_t i = 0; i < count && all; ++i) {
                        const char* n = CookedEffectName(data, i, stride);
                        all = n && n[0] && n[0] >= 0x20 && n[0] < 0x7f;
                    }
                    if (!all) continue;
                    for (uint32_t i = 0; i < count; ++i) AddName(out, size, CookedEffectName(data, i, stride));
                    break;
                }
            }
        }
        arr = plain ? reinterpret_cast<char*>(PropertyPtr(tmpl, plain)) : nullptr;
        if (arr) {
            void** data = *reinterpret_cast<void***>(arr);
            uint32_t count = *reinterpret_cast<uint32_t*>(arr + 8);
            for (uint32_t i = 0; data && i < count && i < 256; ++i) {
                void* fx = data[i];
                void* fcls = fx ? GetObjectClass(fx) : nullptr;
                void* np = fcls ? FindProperty(fcls, "name") : nullptr;
                uint32_t* id = np ? reinterpret_cast<uint32_t*>(PropertyPtr(fx, np)) : nullptr;
                const char* n = id ? GetCNameString(*id) : nullptr;
                if (n && *n) AddName(out, size, n);
            }
        }
        return out[0] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

// import function TW3SE_EntityEffects(e : CEntity) : string;  -> "fire,smoke", "" when it has none
void Tw3EntityEffects(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t buf[16] = {0};
    ReadParam(frame, buf);
    frame->code += 1;
    char out[2048];
    void* ent = ResolveHandle(*reinterpret_cast<void**>(buf));
    if (!ent || !EffectsGuarded(ent, out, sizeof out)) out[0] = 0;
    SetScriptString(result, out, strlen(out));
}

// import function TW3SE_EntityTemplate(e : CEntity) : CEntityTemplate;
void Tw3EntityTemplate(void* /*self*/, ScriptFrame* frame, void* result) {
    uint8_t buf[16] = {0};
    ReadParam(frame, buf);
    frame->code += 1;
    void* ent = ResolveHandle(*reinterpret_cast<void**>(buf));
    void* block = ent ? TemplateBlockGuarded(ent) : nullptr;
    if (!result) return;
    if (block) InterlockedIncrement(reinterpret_cast<volatile LONG*>(block));
    *reinterpret_cast<void**>(result) = block;
}

// Builds the engine function object for `native` under `name` and registers it on the global RTTI system.
// name -> CName via the name pool (the same call content.cpp uses for mounting), the CFunction is constructed
// into our own stable memory (never freed - the native lives for the session, as content.cpp does for a mounted
// package), then CRTTISystem::RegisterGlobalFunction. Runs only when opted in.
std::map<std::string, void*> g_ours;  // the CFunction of every native TW3SE registered (its own and plugins'), by name
void* FindOurs(const char* name) {
    auto it = g_ours.find(name);
    return it == g_ours.end() ? nullptr : it->second;
}
bool RegisterOne(const char* name, Native native, int params) {
    auto poolGet = Fn<NamePoolGet_t>("CNamesPool::Get");
    auto poolAdd = Fn<NamePoolAdd_t>("CNamesPool::AddEntry");
    auto ctor = Fn<CFunctionCtor_t>("CFunction::CFunction");
    auto getRtti = Fn<GetRtti_t>("SRTTI::Get");
    auto registerGlobal = Fn<RegisterGlobal_t>("CRTTISystem::RegisterGlobalFunction");
    if (!poolGet || !poolAdd || !ctor || !getRtti || !registerGlobal) {
        Log("scripting: %s not registered - a registration entry point is missing in this build", name);
        return false;
    }
    void* pool = poolGet();
    CName cn{poolAdd(pool, name)};
    // The CFunction is built in the engine's own default pool (Memory::AllocDefault) - the exact allocator every
    // engine native registration uses for its CFunction/CProperty, so our function object lives in the same pool
    // the engine expects for this object class (6a, docs/ABI_NOTES.md; verified in-game via the string natives).
    // It is never freed: the native lives for the whole session, as content.cpp keeps a mounted package. If the
    // allocator is unavailable on an unknown build, fall back to VirtualAlloc (the earlier proven workaround).
    void* mem = EngineAlloc(0xf8 /* sizeof(CFunction) */, 0x10);
    if (!mem) mem = VirtualAlloc(nullptr, 0x100, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        Log("scripting: %s not registered - allocation failed", name);
        return false;
    }
    void* fn = ctor(mem, &cn, native);
    void* obj = fn ? fn : mem;
    // Multi-parameter natives need nothing special here: the compiler fills the parameter metadata
    // (CFunction+0x28 array / +0x30 count) from the `import function` declaration itself (proven via the
    // measured with a parameter probe; docs/engine/scripting.md section 7). The handler just reads N
    // parameters in order. `params` is kept only for the log and the `natives` command.
    (void)params;
    registerGlobal(getRtti(), obj);
    g_ours[name] = obj;
    Log("scripting: registered native %s (%d param(s), name index %u)", name, params, cn.index);
    return true;
}

// Several mods may import the same TW3SE native (`import function TW3SE_Log(...)` in each). The compiler keeps two
// marks in CFunction+0x18 and rejects a second declaration on either: bit 21 "declared in this compile" ("Function
// '%s' has already been defined in current compilation unit") and bit 3, set by an import ("Global native function
// '%s' was already exported"); CScriptCompiler::DeclareGlobalFunction, docs/engine/scripting.md. The base game
// declares each of its natives once; TW3SE's are declared by every mod that uses them. Before each declaration of
// one of ours both bits are cleared, so every import is handled like the first (the import sets bit 3 again, and
// the signature check still runs). The game's own functions are not touched.
using DeclareGlobal_t = bool (*)(void* compiler, void* decl, uintptr_t canRedefine);
DeclareGlobal_t g_origDeclare = nullptr;

void* OurFunctionGuarded(void* decl, const char** outName) {
    __try {
        const char* name = *reinterpret_cast<const char**>(reinterpret_cast<char*>(decl) + 0x10);
        uint32_t size = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(decl) + 0x18);
        if (!name || size < 2) return nullptr;
        *outName = name;
        return FindOurs(name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool DeclareGlobalHook(void* compiler, void* decl, uintptr_t canRedefine) {
    LuaScriptsRecompiling();
    const char* name = nullptr;
    void* fn = OurFunctionGuarded(decl, &name);
    if (fn) {
        uint32_t& flags = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(fn) + 0x18);
        if (flags & (1u << 3)) {  // imported before: this is a further import of the same native
            flags &= ~(1u << 21);
            canRedefine = 1;
        }
    }
    bool ok = g_origDeclare(compiler, decl, canRedefine);
    if (fn) Log("scripting: compiler declared %s (%s)", name, ok ? "ok" : "rejected");  // proof a mod's import compiled
    return ok;
}

// All natives to register when the engine reaches its native-registration point: the core's own plus any a
// plugin queued during TW3SE_PluginLoad (plugins load before that point).
struct NativeDecl { std::string name; Native fn; int params; };
std::vector<NativeDecl> g_natives;

// --- native methods (docs/engine/scripting.md section 15): a handler becomes a method of a native class,
// called from WitcherScript after an `import function` inside the class (or `@addMethod(Class) import ...`). The
// method constructor takes the next number of GScriptMethodTable itself; CClass::AddFunction puts the function into
// the class's list, where the compiler finds it by name. Global natives and methods share one counter (+0x88 of the
// scripting system) over tables of 4096 slots the engine never checks: it is checked here.
struct QueuedMethod {
    std::string cls, name;
    Native fn;
};
std::vector<QueuedMethod> g_methods;
std::unordered_map<std::string, void*> g_ourMethods;  // "Class.Name" -> its CFunction
using MethodCtor_t = void* (*)(void* mem, void* cls, const CName* name, const void* desc);
using AddFunction_t = void (*)(void* cls, void* fn);

bool RegisterMethodOne(const std::string& cls, const std::string& name, Native native) {
    auto poolGet = Fn<NamePoolGet_t>("CNamesPool::Get");
    auto poolAdd = Fn<NamePoolAdd_t>("CNamesPool::AddEntry");
    auto ctor = Fn<MethodCtor_t>("CFunction::CFunctionMethod");
    auto add = Fn<AddFunction_t>("CClass::AddFunction");
    uintptr_t globals = Address("GScriptGlobals");
    if (!poolGet || !poolAdd || !ctor || !add || !globals) {
        Log("scripting: method %s.%s not registered - an entry point is missing in this build", cls.c_str(), name.c_str());
        return false;
    }
    void* klass = FindClassByName(cls.c_str());
    if (!klass) {
        Log("scripting: method %s.%s not registered - no class %s", cls.c_str(), name.c_str(), cls.c_str());
        return false;
    }
    void* system = *reinterpret_cast<void**>(globals);
    uint32_t used = 0;
    if (!system || !SafeRead(static_cast<char*>(system) + 0x88, &used, 4) || used >= 4096) {
        Log("scripting: method %s.%s not registered - the native tables are full (%u of 4096)", cls.c_str(), name.c_str(), used);
        return false;
    }
    CName cn{poolAdd(poolGet(), name.c_str())};
    struct {
        void* fn;
        int32_t adjust, vbase, vindex, pad;
    } desc{reinterpret_cast<void*>(native), 0, 0, 0, 0};
    void* mem = EngineAlloc(0xf8, 0x10);
    if (!mem) return false;
    void* fn = ctor(mem, klass, &cn, &desc);
    if (!fn) fn = mem;
    add(klass, fn);
    g_ourMethods[cls + "." + name] = fn;
    uint32_t number = 0;
    SafeRead(static_cast<char*>(fn) + 0xe0, &number, 4);
    Log("scripting: registered native method %s.%s (number %u)", cls.c_str(), name.c_str(), number);
    return true;
}

// --- enums of a mod (docs/engine/scripting.md section 16): built as the game builds its native enums (the
// constructor, one Add per value, RegisterType), so scripts use the enum and its value names without a declaration.
// RegisterType keeps an older type of the same name: the name is checked first.
struct QueuedEnum {
    std::string name;
    std::vector<std::pair<std::string, int32_t>> values;
};
std::vector<QueuedEnum> g_enums;
using EnumCtor_t = void* (*)(void* mem, const CName* name, uint32_t size, bool scripted);
using EnumAdd_t = void (*)(void* e, const CName* option, int32_t value);
using RegisterType_t = void (*)(void* rtti, void* type);

bool RegisterEnumOne(const QueuedEnum& q) {
    auto poolGet = Fn<NamePoolGet_t>("CNamesPool::Get");
    auto poolAdd = Fn<NamePoolAdd_t>("CNamesPool::AddEntry");
    auto ctor = Fn<EnumCtor_t>("CEnum::CEnum");
    auto add = Fn<EnumAdd_t>("CEnum::Add");
    auto reg = Fn<RegisterType_t>("CRTTISystem::RegisterType");
    auto getRtti = Fn<GetRtti_t>("SRTTI::Get");
    if (!poolGet || !poolAdd || !ctor || !add || !reg || !getRtti) {
        Log("scripting: enum %s not registered - an entry point is missing in this build", q.name.c_str());
        return false;
    }
    if (RttiFindType(q.name.c_str())) {
        Log("scripting: enum %s not registered - the game has a type of that name", q.name.c_str());
        return false;
    }
    void* pool = poolGet();
    CName cn{poolAdd(pool, q.name.c_str())};
    void* mem = EngineAlloc(0x80, 0x10);
    if (!mem) return false;
    memset(mem, 0, 0x80);
    void* e = ctor(mem, &cn, 4, false);
    if (!e) e = mem;
    for (const auto& [opt, value] : q.values) {
        CName on{poolAdd(pool, opt.c_str())};
        add(e, &on, value);
    }
    reg(getRtti(), e);
    Log("scripting: registered enum %s (%zu value(s))", q.name.c_str(), q.values.size());
    return true;
}

void RegisterAll() {  // C++ objects (vector/string) live here, not in the SEH frame below
    for (auto& n : g_natives) RegisterOne(n.name.c_str(), n.fn, n.params);
    for (auto& m : g_methods) RegisterMethodOne(m.cls, m.name, m.fn);
    for (auto& e : g_enums) RegisterEnumOne(e);
}
// Guarded so a registration fault (e.g. an ABI difference on an unusual build like the REDkit editor) disables
// our natives and logs, instead of taking the host process down. No C++ objects in this frame (SEH requirement).
bool RegisterAllGuarded() {
    __try {
        RegisterAll();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool g_nativesRegistered = false;  // the engine's registration point has passed: new natives register at once

// The natives plugins and Lua mods made, kept for the next start in safe mode: plugins and Lua mods are out then,
// and a WitcherScript mod that imports one of their natives would not compile (the game ends itself then). lua.cpp makes each of these a native that does nothing.
std::vector<std::string> g_modNativeNames;
std::wstring ModNativesFile() { return Dir() + L"\\out\\natives_of_mods.txt"; }

void WriteModNatives() {
    std::string text;
    for (const std::string& n : g_modNativeNames) text += n + "\n";
    CreateDirectoryW((Dir() + L"\\out").c_str(), nullptr);
    if (FILE* f = _wfopen(ModNativesFile().c_str(), L"wb")) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}

void RegisterCoreHook() {
    Log("scripting: RegisterCoreScriptNatives reached (editor=%d) - calling original", IsRedkitEditor() ? 1 : 0);
    g_origRegisterCore();  // let the engine register its own natives first (every time, in case it re-runs)
    if (g_nativesRegistered) return;  // register our natives only once - a second registration would be a redeclaration
    g_nativesRegistered = true;
    Log("scripting: original returned, registering %zu TW3SE native(s)", g_natives.size());
    if (RegisterAllGuarded())
        Log("scripting: %zu native(s) registered", g_natives.size());
    else
        Log("scripting: EXCEPTION during native registration - natives disabled (host unharmed)");
    if (!NoPlugins() && !NoLuaMods() && !PluginsLeftOut() && !LuaModsLeftOut()) WriteModNatives();  // the list for a later safe mode: from a full start only
}

// Console command (temporary, for the 0x0f/out-param investigation): report the handler pointer stored at
// GScriptNativeTable[idx] as an RVA (module-relative), so it can be disassembled statically without having to
// re-derive the bootstrap registration site. Read-only, no state change.
void CmdNativeAt(const char* arg, char* reply, uint32_t size) {
    int idx = -1;
    if (!arg || sscanf_s(arg, "%d", &idx) < 1 || idx < 0 || idx > 0xff) {
        strncpy_s(reply, size, "usage: nativeat <opcode 0..255>", _TRUNCATE);
        return;
    }
    using OpHandler = void (*)(void*, void*, void*);
    auto table = reinterpret_cast<OpHandler*>(Address("GScriptNativeTable"));
    if (!table) { strncpy_s(reply, size, "nativeat: GScriptNativeTable not resolved", _TRUNCATE); return; }
    OpHandler h = table[idx];
    if (!h) { _snprintf_s(reply, size, _TRUNCATE, "nativeat %d: null", idx); return; }
    uintptr_t rva = reinterpret_cast<uintptr_t>(h) - ImageBase();
    _snprintf_s(reply, size, _TRUNCATE, "nativeat %d: rva=0x%llx", idx, static_cast<unsigned long long>(rva));
}

// GScriptMethodTable[opcode] handler as an RVA (temp, analysis): the 24-byte entry's byte 0, module-relative, so a
// method-native handler can be disassembled statically from its opcode (catalog gives the opcode).
void CmdMethodAt(const char* arg, char* reply, uint32_t size) {
    int idx = -1;
    if (!arg || sscanf_s(arg, "%d", &idx) < 1 || idx < 0 || idx > 4095) {
        strncpy_s(reply, size, "usage: methodat <opcode 0..4095>", _TRUNCATE);
        return;
    }
    char* mtable = reinterpret_cast<char*>(Address("GScriptMethodTable"));
    if (!mtable) { strncpy_s(reply, size, "methodat: GScriptMethodTable not resolved", _TRUNCATE); return; }
    void* h = nullptr;
    int32_t adj = 0;
    SafeRead(mtable + static_cast<size_t>(idx) * 24, &h, sizeof h);
    SafeRead(mtable + static_cast<size_t>(idx) * 24 + 8, &adj, 4);
    if (!h) { _snprintf_s(reply, size, _TRUNCATE, "methodat %d: null", idx); return; }
    uintptr_t rva = reinterpret_cast<uintptr_t>(h) - ImageBase();
    _snprintf_s(reply, size, _TRUNCATE, "methodat %d: rva=0x%llx thisAdjust=%d", idx,
                static_cast<unsigned long long>(rva), adj);
}

// GScriptMethodTable this-adjustment scan (temp): 24-byte entries, handler at byte 0, a possible
// int32 this-adjust at byte 8 (read from the dispatcher). Reports how many entries have a non-null handler and how many
// of those carry a non-zero adjust, with the first few examples. Tells us whether the normal method-call path
// (which never applies an adjust) is ever wrong.
void CmdMethodAdjust(const char* /*arg*/, char* reply, uint32_t size) {
    char* table = reinterpret_cast<char*>(Address("GScriptMethodTable"));
    if (!table) { strncpy_s(reply, size, "methodadjust: GScriptMethodTable not resolved", _TRUNCATE); return; }
    int withHandler = 0, nonzero = 0;
    char examples[200] = {0};
    for (int i = 0; i < 4096; ++i) {
        void* h = nullptr;
        int32_t adj = 0;
        char* e = table + static_cast<size_t>(i) * 24;
        if (!SafeRead(e, &h, sizeof h) || !h) continue;
        ++withHandler;
        if (!SafeRead(e + 8, &adj, 4) || adj == 0) continue;
        ++nonzero;
        if (nonzero <= 6) {
            char one[32];
            _snprintf_s(one, sizeof one, _TRUNCATE, " %d=%d", i, adj);
            strncat_s(examples, sizeof examples, one, _TRUNCATE);
        }
    }
    _snprintf_s(reply, size, _TRUNCATE, "methodadjust: %d entries, %d with non-zero adjust;%s", withHandler, nonzero,
                nonzero ? examples : " all zero");
}

// Console command: find a function (incl. method-natives like GetMovingAgentComponent) via the global RTTI
// hashmap and report its opcode (CFunction+0xe0), parameter count (+0x30) and flags (+0x18). Tells us whether
// method-natives are globally findable (prep for calling WitcherScript functions from C++).
void CmdFindFn(const char* arg, char* reply, uint32_t size) {
    char name[128] = {0};
    if (!arg || sscanf_s(arg, "%127s", name, static_cast<unsigned>(sizeof name)) < 1 || !name[0]) {
        strncpy_s(reply, size, "usage: findfn <FunctionName>", _TRUNCATE);
        return;
    }
    auto poolGet = Fn<NamePoolGet_t>("CNamesPool::Get");
    auto poolAdd = Fn<NamePoolAdd_t>("CNamesPool::AddEntry");
    auto getRtti = Fn<GetRtti_t>("SRTTI::Get");
    if (!poolGet || !poolAdd || !getRtti) { strncpy_s(reply, size, "findfn: entry points missing", _TRUNCATE); return; }
    uint32_t id = poolAdd(poolGet(), name);
    void* fn = FindGlobalFunction(getRtti(), id);
    if (!fn) { _snprintf_s(reply, size, _TRUNCATE, "findfn %s: not in global hashmap (id %u)", name, id); return; }
    char* f = reinterpret_cast<char*>(fn);
    _snprintf_s(reply, size, _TRUNCATE, "findfn %s: %p opcode=%u params=%u flags=0x%x", name, fn,
                *reinterpret_cast<uint32_t*>(f + 0xe0), *reinterpret_cast<uint32_t*>(f + 0x30),
                *reinterpret_cast<uint32_t*>(f + 0x18));
}

// Console command: call a global native with an optional single string argument, report the int result.
// Proves calling WitcherScript functions from C++ (e.g. `callfn StrLen hello` -> 5).
void CmdCallFn(const char* arg, char* reply, uint32_t size) {
    char name[128] = {0};
    if (!arg || sscanf_s(arg, "%127s", name, static_cast<unsigned>(sizeof name)) < 1 || !name[0]) {
        strncpy_s(reply, size, "usage: callfn <GlobalFunction> [args: i:N f:N s:text b:1]", _TRUNCATE);
        return;
    }
    const char* rest = arg + strlen(name);
    while (*rest == ' ') ++rest;
    int32_t res = 0;
    if (!CallGlobalSpec(name, rest, nullptr, &res)) {
        _snprintf_s(reply, size, _TRUNCATE, "callfn %s: not callable (not found / unsupported arg)", name);
        return;
    }
    _snprintf_s(reply, size, _TRUNCATE, "callfn %s(%s) -> %d", name, rest, res);
}

// Console command: call a method-native on the live player, report the result as pointer and int. Proves
// method-native calls (e.g. `callm GetMovingAgentComponent` -> the component pointer).
void CmdCallMethod(const char* arg, char* reply, uint32_t size) {
    char method[128] = {0};
    if (!arg || sscanf_s(arg, "%127s", method, static_cast<unsigned>(sizeof method)) < 1 || !method[0]) {
        strncpy_s(reply, size, "usage: callm <Method> [args: i:N f:N s:text b:1]  (on thePlayer)", _TRUNCATE);
        return;
    }
    const char* rest = arg + strlen(method);
    while (*rest == ' ') ++rest;
    void* player = GetThePlayer();
    if (!player) { strncpy_s(reply, size, "callm: no player (main menu?)", _TRUNCATE); return; }
    uint64_t res = 0;
    if (!CallMethodSpec(player, method, rest, &res)) {
        // diagnostic: replicate FindMethod's scan and report what it sees
        void* cls = GetObjectClass(player);
        int total = 0, classes = 0, seen = 0;
        uint32_t op = 0;
        for (char* c = reinterpret_cast<char*>(cls); c; c = *reinterpret_cast<char**>(c + 0x10)) {
            ++classes;
            void** funcs = *reinterpret_cast<void***>(c + 0x48);
            uint32_t count = *reinterpret_cast<uint32_t*>(c + 0x50);
            if (!funcs || count > 8192) continue;
            for (uint32_t i = 0; i < count; ++i) {
                char* f = reinterpret_cast<char*>(funcs[i]);
                if (!f) continue;
                ++total;
                const char* fn = GetCNameString(*reinterpret_cast<uint32_t*>(f + 0x10));
                if (fn && strcmp(fn, method) == 0) { ++seen; op = *reinterpret_cast<uint32_t*>(f + 0xe0); }
            }
        }
        auto table = reinterpret_cast<OpHandler*>(Address("GScriptNativeTable"));
        void* handler = (table && op) ? reinterpret_cast<void*>(table[op]) : nullptr;
        _snprintf_s(reply, size, _TRUNCATE,
                    "callm %s: not callable. seen=%d op=%u table=%p handler=%p", method, seen, op,
                    reinterpret_cast<void*>(table), handler);
        return;
    }
    _snprintf_s(reply, size, _TRUNCATE, "callm %s -> 0x%llx (int %d)", method,
                static_cast<unsigned long long>(res), static_cast<int32_t>(res));
}

// Shared body for the `player`/`global` commands: on a live object, report its class, or read/set a named
// property (values treated as float - fits vitality, stamina, position components, ...). `label` names the
// object in the reply. `obj` must be non-null.
void ReportObjectProp(const char* label, void* obj, const char* field, const char* valstr, char* reply,
                      uint32_t size) {
    void* cls = GetObjectClass(obj);
    if (!cls) { _snprintf_s(reply, size, _TRUNCATE, "%s: GetObjectClass returned null", label); return; }
    if (!field || !field[0]) {
        _snprintf_s(reply, size, _TRUNCATE, "%s: object %p, class ok, %u properties (use: <field> [value])",
                    label, obj, CountProperties(cls));
        return;
    }
    void* p = FindProperty(cls, field);
    if (!p) { _snprintf_s(reply, size, _TRUNCATE, "%s: field '%s' not found", label, field); return; }
    float* fp = reinterpret_cast<float*>(PropertyPtr(obj, p));
    if (!fp) { _snprintf_s(reply, size, _TRUNCATE, "%s: property pointer null", label); return; }
    uint32_t off = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(p) + 0x20);
    if (valstr && valstr[0]) {
        float oldv = *fp;
        float newv = strtof(valstr, nullptr);
        *fp = newv;
        _snprintf_s(reply, size, _TRUNCATE, "%s.%s @0x%x: %g -> %g (set)", label, field, off, oldv, newv);
    } else {
        _snprintf_s(reply, size, _TRUNCATE, "%s.%s @0x%x = %g (int %d)", label, field, off, *fp,
                    *reinterpret_cast<int32_t*>(fp));
    }
}

// Console command: read or set a property of the live player. `player` lists the class; `player <field>` reads
// it; `player <field> <value>` sets it (float). Only valid in-game (null in the main menu).
void CmdPlayer(const char* arg, char* reply, uint32_t size) {
    void* player = GetThePlayer();
    if (!player) { strncpy_s(reply, size, "player: not available (main menu or no spawn yet)", _TRUNCATE); return; }
    char field[128] = {0}, valstr[64] = {0};
    if (arg) sscanf_s(arg, "%127s %63s", field, static_cast<unsigned>(sizeof field), valstr,
                      static_cast<unsigned>(sizeof valstr));
    ReportObjectProp("player", player, field, valstr, reply, size);
}

// Field offset of a predefined WitcherScript global in GScriptGlobals, or 0 if the name is unknown.
uint32_t ScriptGlobalOffset(const char* name) {
    struct GlobalDef { const char* n; uint32_t off; };
    static const GlobalDef globals[] = {
        {"theGame", 0x20}, {"thePlayer", 0x28}, {"theCamera", 0x30}, {"theHUD", 0x38}, {"theSound", 0x40},
        {"theDebug", 0x48}, {"theTimer", 0x50}, {"theInput", 0x58}, {"theTelemetry", 0x60}};
    for (const GlobalDef& g : globals) if (name && _stricmp(g.n, name) == 0) return g.off;
    return 0;
}

// Console command: same for any predefined WitcherScript global. `global <name> [field] [value]`, name in
// theGame/thePlayer/theCamera/theHUD/theSound/theDebug/theTimer/theInput/theTelemetry. theGame is valid even
// in the main menu, so this is the safe way to verify the GScriptGlobals chain without a spawned player.
void CmdGlobal(const char* arg, char* reply, uint32_t size) {
    char name[64] = {0}, field[128] = {0}, valstr[64] = {0};
    int parsed = arg ? sscanf_s(arg, "%63s %127s %63s", name, static_cast<unsigned>(sizeof name), field,
                                static_cast<unsigned>(sizeof field), valstr, static_cast<unsigned>(sizeof valstr))
                     : 0;
    if (parsed < 1 || !name[0]) { strncpy_s(reply, size, "usage: global <name> [field] [value]", _TRUNCATE); return; }
    uint32_t off = ScriptGlobalOffset(name);
    if (!off) { _snprintf_s(reply, size, _TRUNCATE, "global: unknown name '%s'", name); return; }
    void* obj = GetScriptGlobal(off);
    if (!obj) { _snprintf_s(reply, size, _TRUNCATE, "global %s: not available (null)", name); return; }
    ReportObjectProp(name, obj, field, valstr, reply, size);
}

// Console command: dump a class's method-natives (name list from CClass+0x48 ptr / +0x50 count, walking the
// parent chain) to tw3se.log - diagnostic for the method-call path. `methods <global|ClassName> [filter]`.
void CmdMethods(const char* arg, char* reply, uint32_t size) {
    char name[64] = {0}, filter[64] = {0};
    int parsed = arg ? sscanf_s(arg, "%63s %63s", name, static_cast<unsigned>(sizeof name), filter,
                                static_cast<unsigned>(sizeof filter)) : 0;
    if (parsed < 1 || !name[0]) { strncpy_s(reply, size, "usage: methods <global|ClassName> [filter]", _TRUNCATE); return; }
    void* cls = nullptr;
    uint32_t off = ScriptGlobalOffset(name);
    if (off) { void* obj = GetScriptGlobal(off); cls = obj ? GetObjectClass(obj) : nullptr; }
    else cls = FindClassByName(name);
    if (!cls) { _snprintf_s(reply, size, _TRUNCATE, "methods %s: class not found", name); return; }
    uint32_t total = 0, shown = 0, classes = 0;
    for (char* c = reinterpret_cast<char*>(cls); c; c = *reinterpret_cast<char**>(c + 0x10)) {
        ++classes;
        void** funcs = *reinterpret_cast<void***>(c + 0x48);
        uint32_t count = *reinterpret_cast<uint32_t*>(c + 0x50);
        if (!funcs || count > 4096) continue;  // cap guards against a wrong offset reading garbage
        for (uint32_t i = 0; i < count; ++i) {
            char* f = reinterpret_cast<char*>(funcs[i]);
            if (!f) continue;
            ++total;
            const char* fn = GetCNameString(*reinterpret_cast<uint32_t*>(f + 0x10));
            if (filter[0] && (!fn || !strstr(fn, filter))) continue;
            Log("method %s @opcode %u", fn ? fn : "?", *reinterpret_cast<uint32_t*>(f + 0xe0));
            ++shown;
        }
    }
    _snprintf_s(reply, size, _TRUNCATE, "methods %s: %u classes walked, %u methods, %u logged", name, classes,
                total, shown);
}

// Console command: dump a class's properties (name @ offset) to tw3se.log - the reflection browser. `props
// <name> [filter]` where name is a predefined global (thePlayer, theGame, ...) or a class name; optional
// filter is a case-insensitive substring on the property name. Full list goes to tw3se.log; the reply is a
// summary. Uses GetCNameString (id -> name) so the real property names appear. Great for finding a field.
void CmdProps(const char* arg, char* reply, uint32_t size) {
    char name[64] = {0}, filter[64] = {0};
    int parsed = arg ? sscanf_s(arg, "%63s %63s", name, static_cast<unsigned>(sizeof name), filter,
                                static_cast<unsigned>(sizeof filter)) : 0;
    if (parsed < 1 || !name[0]) { strncpy_s(reply, size, "usage: props <global|ClassName> [filter]", _TRUNCATE); return; }
    void* cls = nullptr;
    uint32_t off = ScriptGlobalOffset(name);
    if (off) {
        void* obj = GetScriptGlobal(off);
        if (!obj) { _snprintf_s(reply, size, _TRUNCATE, "props %s: global not available (null)", name); return; }
        cls = GetObjectClass(obj);
    } else {
        cls = FindClassByName(name);
    }
    if (!cls) { _snprintf_s(reply, size, _TRUNCATE, "props %s: class not found", name); return; }
    char lower[64];
    for (int i = 0; filter[i] && i < 63; ++i) lower[i] = static_cast<char>(tolower(filter[i])), lower[i + 1] = 0;
    if (!filter[0]) lower[0] = 0;
    uint32_t total = 0, shown = 0;
    for (char* c = reinterpret_cast<char*>(cls); c; c = *reinterpret_cast<char**>(c + 0x10)) {
        void** props = *reinterpret_cast<void***>(c + 0x30);
        uint32_t n = *reinterpret_cast<uint32_t*>(c + 0x38);
        if (!props) continue;
        for (uint32_t i = 0; i < n; ++i) {
            char* p = reinterpret_cast<char*>(props[i]);
            if (!p) continue;
            ++total;
            uint32_t id = *reinterpret_cast<uint32_t*>(p + 0x10);
            uint32_t poff = *reinterpret_cast<uint32_t*>(p + 0x20);
            const char* pname = GetCNameString(id);
            if (lower[0]) {
                if (!pname) continue;
                char pl[128]; int j = 0;
                for (; pname[j] && j < 127; ++j) pl[j] = static_cast<char>(tolower(pname[j]));
                pl[j] = 0;
                if (!strstr(pl, lower)) continue;
            }
            Log("prop %s @0x%x (id %u)", pname ? pname : "?", poff, id);
            ++shown;
        }
    }
    _snprintf_s(reply, size, _TRUNCATE, "%s: %u properties total, %u logged to tw3se.log%s", name, total, shown,
                lower[0] ? " (filtered)" : "");
}

// Console command: list the registered WitcherScript natives (name + parameter count).
void CmdNatives(const char*, char* reply, uint32_t size) {
    std::string s = "natives (" + std::to_string(g_natives.size()) + "):";
    for (auto& n : g_natives) s += " " + n.name + "/" + std::to_string(n.params);
    strncpy_s(reply, size, s.c_str(), _TRUNCATE);
}

}  // namespace

// The effects an entity's template defines, comma-separated (TW3SE_EntityEffects; worldedit keeps the running ones
// of an entity it hides). False when it has none.
bool EntityEffectNames(void* ent, char* out, size_t size) { return EffectsGuarded(ent, out, size); }

// Plugins (and the core) queue natives here before the engine registers them. See include/tw3se/tw3se.h.
void QueueNative(const char* name, void (*native)(void*, void*, void*), int params) {
    if (name && native) g_natives.push_back({name, reinterpret_cast<Native>(native), params});
}

void NoteModNative(const char* name) {
    if (name && *name && std::find(g_modNativeNames.begin(), g_modNativeNames.end(), name) == g_modNativeNames.end())
        g_modNativeNames.push_back(name);
}

std::vector<std::string> ModNativesLastTime() {
    std::vector<std::string> out;
    FILE* f = _wfopen(ModNativesFile().c_str(), L"rb");
    if (!f) return out;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) out.push_back(s);
    }
    fclose(f);
    return out;
}

// A native of that name is queued or registered (the compiler's messages name a TW3SE_ function this TW3SE lacks).
bool NativeKnown(const std::string& name) {
    for (const auto& n : g_natives)
        if (n.name == name) return true;
    return false;
}

// A native that may come after the registration point (a Lua mod's w3.native): queued before it, registered at once
// after it (on the main thread). Scripts that import a late one see it after the next `reloadscripts force`.
bool NativesRegistered() { return g_nativesRegistered; }

bool RegisterNativeAnytime(const char* name, void (*native)(void*, void*, void*), int params) {
    if (!name || !native) return false;
    if (!g_nativesRegistered) {
        QueueNative(name, native, params);
        return true;
    }
    g_natives.push_back({name, reinterpret_cast<Native>(native), params});
    return RegisterOne(name, reinterpret_cast<Native>(native), params);
}

// A native method: queued before the registration point, registered at once after it (scripts that import a late
// one see it after the next `reloadscripts force`). OurMethod: its CFunction, for the parameters the compiler filled in.
bool RegisterNativeMethodAnytime(const char* cls, const char* name, void (*native)(void*, void*, void*)) {
    if (!cls || !*cls || !name || !*name || !native) return false;
    if (!g_nativesRegistered) {
        g_methods.push_back({cls, name, reinterpret_cast<Native>(native)});
        return true;
    }
    return RegisterMethodOne(cls, name, reinterpret_cast<Native>(native));
}
// An enum of a mod: queued before the registration point, registered at once after it (scripts see it after the
// next `reloadscripts force`).
bool RegisterEnumAnytime(const std::string& name, const std::vector<std::pair<std::string, int32_t>>& values) {
    if (name.empty() || values.empty()) return false;
    if (!g_nativesRegistered) {
        g_enums.push_back({name, values});
        return true;
    }
    return RegisterEnumOne({name, values});
}

void* OurMethod(const std::string& full) {
    auto it = g_ourMethods.find(full);
    return it == g_ourMethods.end() ? nullptr : it->second;
}

// Calls a globally-registered WitcherScript native from C++ with string arguments (C-friendly wrapper around
// CallGlobalFunction, for core.h / the plugin API). result receives the int/bool return.
bool CallGlobalFunc(const char* name, const char* const* stringArgs, int argCount, void* self, void* result) {
    std::vector<std::string> args;
    for (int i = 0; i < argCount && stringArgs; ++i)
        if (stringArgs[i]) args.emplace_back(stringArgs[i]);
    return CallGlobalFunction(name, args, self, result);
}

// C-friendly wrapper around CallMethod (method-native on an object), for core.h / the plugin API.
bool CallMethodFunc(void* object, const char* method, const char* const* stringArgs, int argCount, void* result) {
    std::vector<std::string> args;
    for (int i = 0; i < argCount && stringArgs; ++i)
        if (stringArgs[i]) args.emplace_back(stringArgs[i]);
    return CallMethod(object, method, args, result);
}

// Typed-argument call wrappers for the plugin API (v9): `argspec` is a space-separated typespec,
// e.g. "i:1000 f:3.5 s:hello" (int 0x04, float 0x06, string 0x07, bool-true 0x0a; see BuildArgsFromSpec).
bool CallFunctionV(const char* name, const char* argspec, void* self, void* result) {
    return CallGlobalSpec(name, argspec ? argspec : "", self, result);
}
bool CallMethodV(void* object, const char* method, const char* argspec, void* result) {
    Crumb("engine call: method", reinterpret_cast<uintptr_t>(object), reinterpret_cast<uintptr_t>(method));
    DoingScope d("an engine method call", reinterpret_cast<uintptr_t>(object));
    return CallMethodSpec(object, method, argspec ? argspec : "", result);
}

namespace {
// The handler call alone, SEH-guarded (no C++ objects here, C2712). An access violation inside the native is
// caught; the native's half-done work stays as it is.
// Where a native call faulted ("native call faulted (opcode n)" alone names no place to look at): the
// exception, the address it read or wrote, and the first callers, unwound from the faulting context.
struct FaultInfo {
    DWORD code = 0;
    uintptr_t at = 0, access = 0, kind = 0;
    uintptr_t callers[8] = {};
};
thread_local FaultInfo t_fault;

int RecordFault(EXCEPTION_POINTERS* ep) {
    t_fault = FaultInfo{};
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_EXECUTE_HANDLER;
    t_fault.code = ep->ExceptionRecord->ExceptionCode;
    t_fault.at = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->NumberParameters >= 2) {
        t_fault.kind = ep->ExceptionRecord->ExceptionInformation[0];    // 0 read, 1 write, 8 execute
        t_fault.access = ep->ExceptionRecord->ExceptionInformation[1];
    }
    CONTEXT ctx = *ep->ContextRecord;
    for (int i = 0; i < 8 && ctx.Rip; ++i) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        if (!f) {
            if (!MemReadable(reinterpret_cast<void*>(ctx.Rsp), 8)) break;
            ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
            ctx.Rsp += 8;
        } else {
            void* hd = nullptr;
            DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, f, &ctx, &hd, &est, nullptr);
        }
        t_fault.callers[i] = static_cast<uintptr_t>(ctx.Rip);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

std::string ModOff(uintptr_t a) {  // module+offset (and the name from addresses.inc for the game) of an address
    char b[300];
    HMODULE m = nullptr;
    if (a && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(a), &m) && m) {
        wchar_t path[MAX_PATH] = L"?";
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* n = wcsrchr(path, L'\\');
        snprintf(b, sizeof b, "%ls+0x%llx", n ? n + 1 : path, static_cast<unsigned long long>(a - reinterpret_cast<uintptr_t>(m)));
        std::string s = b;
        if (reinterpret_cast<uintptr_t>(m) == ImageBase()) {
            std::string nm = NearestAddressName(a);
            if (!nm.empty()) s += " (" + nm + ")";
        }
        return s;
    }
    snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(a));
    return b;
}

bool CallHandlerGuarded(OpHandler h, void* self, ScriptFrame* frame, void* result) {
    __try {
        h(self, frame, result);
        return true;
    } __except (RecordFault(GetExceptionInformation())) {
        return false;
    }
}
}  // namespace

std::string AddressPlace(uintptr_t a) { return ModOff(a); }

void* g_invokeThread = nullptr;
void* g_invokeParams = nullptr;

bool InvokeNative(void* fn, void* self, const uint8_t* code, void* result, bool* faulted, void* localsBase,
                  bool applyThisAdjust) {
    if (faulted) *faulted = false;
    if (!fn || !code) return false;
    uint32_t opcode = 0, flags = 0;
    if (!SafeRead(reinterpret_cast<char*>(fn) + 0xe0, &opcode, 4) || !SafeRead(reinterpret_cast<char*>(fn) + 0x18, &flags, 4))
        return false;
    OpHandler h = nullptr;
    if (flags & 2) {  // global function (docs/engine/scripting.md section 11, bit 1)
        auto table = reinterpret_cast<OpHandler*>(Address("GScriptNativeTable"));
        if (table) SafeRead(table + opcode, &h, sizeof h);
    } else {
        char* mtable = reinterpret_cast<char*>(Address("GScriptMethodTable"));
        if (mtable) {
            char* entry = mtable + static_cast<size_t>(opcode) * 24;
            SafeRead(entry, &h, sizeof h);
            int32_t adjust = 0;  // method entry byte 8: this-adjustment the dispatcher applies (16 for state methods)
            if (applyThisAdjust && self && SafeRead(entry + 8, &adjust, 4) && adjust)
                self = reinterpret_cast<char*>(self) + adjust;
        }
    }
    if (!h) return false;
    // The frame handed to the native (main thread). Literal arguments use only context and cursor, but a nested
    // call in the arguments (opcode 0x1c) reads fields further on: a 0x38-byte frame took stack garbage there and
    // broke the stack. A large zeroed frame is enough. It is a local, not a shared buffer, so a native
    // that calls back into Lua (TW3SE_Lua) which calls another native does not corrupt the outer frame.
    alignas(16) uint8_t frameMem[0x1000];
    memset(frameMem, 0, sizeof frameMem);
    auto frame = reinterpret_cast<ScriptFrame*>(frameMem);
    frame->context = self;
    frame->code = const_cast<uint8_t*>(code);
    frame->object = localsBase;
    // +0x18: the running CFunction. The VM hands a native the calling frame, and some read the caller's name there
    // (FadeOutAsync: [frame+0x18]+0x10); null faulted them. The called function stands in for the caller.
    *reinterpret_cast<void**>(frameMem + 0x18) = fn;
    // +0x40: the frame's script thread; a latent script function called from here is appended to its frame stack
    // (Dispatch). Set only for the one call that asked for it (lua.cpp, latent calls).
    *reinterpret_cast<void**>(frameMem + 0x40) = g_invokeThread;
    g_invokeThread = nullptr;
    *reinterpret_cast<void**>(frameMem + 0x28) = g_invokeParams;  // by-value structs (opcode 0x10, lua.cpp BuildArgs)
    g_invokeParams = nullptr;
    bool ok = CallHandlerGuarded(h, self, frame, result);
    if (ok) return true;
    if (faulted) *faulted = true;
    Log("native call faulted (opcode %u) - caught, the game goes on: 0x%08lx at %s, %s 0x%llx", opcode, t_fault.code,
        ModOff(t_fault.at).c_str(), t_fault.kind == 1 ? "write" : t_fault.kind == 8 ? "execute" : "read",
        static_cast<unsigned long long>(t_fault.access));
    for (uintptr_t c : t_fault.callers)
        if (c) Log("    from %s", ModOff(c).c_str());
    return false;
}

// Runs a WitcherScript function's original body (for a hook's orig()). A script function has no table handler to
// call directly; instead the VM dispatcher (Dispatch, addresses.inc) runs the interpreter over its bytecode
// (CFunction+0x88) when the native bit is clear. To run the original without touching the shared, registered
// CFunction (which the hook has flagged native + redirected), this makes a local copy of the CFunction with the
// native bit cleared and hands that copy to Dispatch: the dispatcher reads the copy's bytecode pointer (the same
// bytes as the real one) and runs it, the real object stays untouched. `code` is the re-encoded argument bytecode
// the dispatcher binds into the new locals; `localsBase` carries any by-reference (out) argument slots.
using DispatchFn = bool (*)(void*, void*, void*, void*);  // Dispatch(CFunction*, context, callingFrame, result)

bool CallDispatchGuarded(DispatchFn d, void* fnCopy, void* self, ScriptFrame* frame, void* result) {
    __try {
        d(fnCopy, self, frame, result);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InvokeScript(void* fn, void* self, const uint8_t* code, void* result, bool* faulted, void* localsBase) {
    if (faulted) *faulted = false;
    auto dispatch = Fn<DispatchFn>("Dispatch");
    if (!dispatch || !fn || !code) return false;
    alignas(16) uint8_t copy[0x100];  // covers every field Dispatch/BindParameters read (<= +0xe0)
    if (!SafeRead(fn, copy, sizeof copy)) return false;
    uint32_t flags = 0;
    memcpy(&flags, copy + 0x18, 4);
    flags &= ~1u;  // clear the native bit: the dispatcher runs the script interpreter over this copy's bytecode
    memcpy(copy + 0x18, &flags, 4);
    alignas(16) uint8_t frameMem[0x1000];
    memset(frameMem, 0, sizeof frameMem);
    auto frame = reinterpret_cast<ScriptFrame*>(frameMem);
    frame->context = self;
    frame->code = const_cast<uint8_t*>(code);
    frame->object = localsBase;  // by-reference (out) argument slots, resolved by the 0x0f handler
    *reinterpret_cast<void**>(frameMem + 0x28) = g_invokeParams;  // by-value structs (opcode 0x10, lua.cpp BuildArgs)
    g_invokeParams = nullptr;
    if (CallDispatchGuarded(dispatch, copy, self, frame, result)) return true;
    if (faulted) *faulted = true;
    Log("script call faulted (hook orig) - caught, the game goes on");
    return false;
}

// Runs a hooked script function's original body with the caller's real frame (the reentrant / off-thread fallback,
// where re-encoding the arguments is not possible): clears the native bit on the real CFunction just for this one
// call so the dispatcher takes the interpreter path, runs it with the real frame (which advances the cursor), then
// restores the bit. The script VM is single-threaded, so the window is tiny.
static bool WriteU32Guarded(void* dst, uint32_t v) {
    __try {
        *reinterpret_cast<uint32_t*>(dst) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RunScriptOriginal(void* fn, void* ctx, void* frame, void* result) {
    auto dispatch = Fn<DispatchFn>("Dispatch");
    if (!dispatch || !fn) return false;
    uint32_t flags = 0;
    if (!SafeRead(reinterpret_cast<char*>(fn) + 0x18, &flags, 4)) return false;
    WriteU32Guarded(reinterpret_cast<char*>(fn) + 0x18, flags & ~1u);  // clear the native bit for this one call
    bool ok = CallDispatchGuarded(dispatch, fn, ctx, reinterpret_cast<ScriptFrame*>(frame), result);
    WriteU32Guarded(reinterpret_cast<char*>(fn) + 0x18, flags);  // restore the native bit
    return ok;
}

// Script events: the engine calls a script function from C++ (CallEvent, CallFunction, inlined at 367
// places) through ScriptCallDirect, which runs the bytecode and never looks at the native bit. A hooked script
// function (native bit + fresh opcode, lua.cpp) is sent to Dispatch from here instead, the way the engine's own
// C++ bridge calls a native: every argument as opcode 0x10 with its parameter property, read from the caller's
// parameter buffer at frame+0x28. Dispatch then runs the hook's trampoline.
namespace {
using ScriptCallFn = bool (*)(void* fn, void* ctx, void* params, void* result);
ScriptCallFn g_scriptCallOrig = nullptr;

bool ScriptCallDetour(void* fn, void* ctx, void* params, void* result) {
    uint32_t flags = 0;
    if (!SafeRead(reinterpret_cast<char*>(fn) + 0x18, &flags, 4) || !(flags & 1) || !IsHookedScriptFunction(fn))
        return g_scriptCallOrig(fn, ctx, params, result);
    auto dispatch = Fn<DispatchFn>("Dispatch");
    void** props = nullptr;
    uint32_t np = 0;
    SafeRead(reinterpret_cast<char*>(fn) + 0x28, &props, 8);
    SafeRead(reinterpret_cast<char*>(fn) + 0x30, &np, 4);
    void* prop[32] = {};
    if (!dispatch || np > 32 || (np && !SafeRead(props, prop, np * sizeof(void*))))
        return g_scriptCallOrig(fn, ctx, params, result);
    // as the compiler writes a call: an out parameter or an array by reference (0x0f, its address in the caller's
    // buffer through frame+0x20), everything else by value (0x10, through frame+0x28). 0x10 on an out struct gave
    // the hook the value where it expects the address, and orig() wrote the struct over a camera controller.
    uint8_t code[32 * 9 + 1] = {};
    for (uint32_t i = 0; i < np; ++i) {
        uint32_t pflags = 0;
        void* ptype = nullptr;
        SafeRead(reinterpret_cast<char*>(prop[i]) + 0x24, &pflags, 4);
        SafeRead(reinterpret_cast<char*>(prop[i]) + 0x08, &ptype, 8);
        bool byRef = (pflags & 0x200) || RttiKind(ptype) == 3;
        code[i * 9] = byRef ? 0x0f : 0x10;
        memcpy(code + i * 9 + 1, &prop[i], 8);
    }
    code[np * 9] = 0x20;  // end of arguments (the hook's fallback runs the original over this frame)
    alignas(16) uint8_t frameMem[0x1000];
    memset(frameMem, 0, sizeof frameMem);
    memcpy(frameMem + 0x00, &ctx, 8);
    memcpy(frameMem + 0x18, &fn, 8);
    memcpy(frameMem + 0x20, &params, 8);
    memcpy(frameMem + 0x28, &params, 8);
    uint8_t* cursor = code;
    memcpy(frameMem + 0x30, &cursor, 8);
    bool ok = CallDispatchGuarded(dispatch, fn, ctx, reinterpret_cast<ScriptFrame*>(frameMem), result);
    if (!ok) Log("script event through a hook faulted - caught, the game goes on");
    return ok;
}
}  // namespace

// Puts the detour on at the first hook of a script function (no cost to the engine's calls before that).
bool EnsureScriptCallHook() {
    if (g_scriptCallOrig) return true;
    static bool tried = false;
    if (tried) return false;
    tried = true;
    void* target = reinterpret_cast<void*>(Address("ScriptCallDirect"));
    if (!target || !Hook(target, reinterpret_cast<void*>(&ScriptCallDetour), reinterpret_cast<void**>(&g_scriptCallOrig))) {
        Log("script hooks: ScriptCallDirect not hooked, events the engine raises will skip Lua hooks");
        return false;
    }
    return true;
}

uint32_t InternName(const char* name) {
    // "" and "None" are the empty name, id 0 ('' as an optional tag of its own would make
    // FindGameplayEntitiesInRange find nothing)
    if (name && (!*name || strcmp(name, "None") == 0)) return 0;
    auto poolGet = Fn<NamePoolGet_t>("CNamesPool::Get");
    auto poolAdd = Fn<NamePoolAdd_t>("CNamesPool::AddEntry");
    return (poolGet && poolAdd && name) ? poolAdd(poolGet(), name) : 0;
}

// Calls an existing native handler with the caller's real frame (used by a hook dispatcher to run the original it
// replaced). SEH-guarded; a fault inside is caught so a hook never takes the game down. True if it ran.
bool CallRawHandler(void* handler, void* ctx, void* frame, void* result) {
    if (!handler) return false;
    __try {
        reinterpret_cast<OpHandler>(handler)(ctx, reinterpret_cast<ScriptFrame*>(frame), result);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Overwrites a String FIELD of a live object (not a fresh result slot): frees the old buffer first, then sets the
// new one. The old data pointer looks like a real heap pointer before it is freed, so a garbage slot is not freed.
// SEH-guarded as a whole (an unexpected field layout must not crash). Returns false when the allocator is missing
// or something faults.
void SetScriptResultString(void* result, const char* text, size_t len) { SetScriptString(result, text, len); }

bool WriteStringField(void* field, const char* text, size_t len) {
    if (!field) return false;
    __try {
        auto s = reinterpret_cast<WitcherString*>(field);
        char* old = s->data;
        if (old && reinterpret_cast<uintptr_t>(old) > 0x10000)
            if (auto freeFn = Fn<void (*)(void*)>("Memory::Free")) freeFn(old);
        s->data = nullptr;
        s->length = 0;
        SetScriptString(field, text, len);
        return s->data != nullptr || len == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* FindGlobalFn(const char* name) {
    auto getRtti = Fn<GetRtti_t>("SRTTI::Get");
    uint32_t id = InternName(name);
    return (getRtti && id) ? FindGlobalFunction(getRtti(), id) : nullptr;
}

uint32_t ScriptGlobalOffsetOf(const char* name) { return ScriptGlobalOffset(name); }

// Read one of the calling script's parameters inside a native handler (see docs/ABI_NOTES.md).
void ReadScriptParam(void* frame, void* out) { ReadParam(reinterpret_cast<ScriptFrame*>(frame), out); }

// Skip the end-of-arguments marker: call once, after reading all parameters, before returning from a native.
void EndScriptParams(void* frame) { reinterpret_cast<ScriptFrame*>(frame)->code += 1; }

// Where a native's argument cursor stands, and putting it back (a guarded native that faulted halfway through its
// arguments: plugins.cpp reads them again from the start, so the script goes on at the right byte).
void* ScriptParamCursor(void* frame) { return reinterpret_cast<ScriptFrame*>(frame)->code; }
void SetScriptParamCursor(void* frame, void* cursor) {
    reinterpret_cast<ScriptFrame*>(frame)->code = static_cast<uint8_t*>(cursor);
}

void InstallScripting() {
    // Config: natives on by default; `natives = off` in tw3se.config (or the file tw3se\natives.off) disables them.
    if (!ConfigBool("natives", true) ||
        GetFileAttributesW((Dir() + L"\\natives.off").c_str()) != INVALID_FILE_ATTRIBUTES) {
        Log("scripting: natives disabled (config)");
        return;
    }
    // Queue the core's own natives (plugins add theirs via the interface during TW3SE_PluginLoad, which runs
    // before the engine reaches its native-registration point).
    g_natives.push_back({"TW3SE_GetVersion", &Tw3GetVersion, 0});
    g_natives.push_back({"TW3SE_MountDLC", &Tw3MountDLC, 1});
    g_natives.push_back({"TW3SE_WriteFile", &Tw3WriteFile, 1});
    g_natives.push_back({"TW3SE_WriteNamed", &Tw3WriteNamed, 2});
    g_natives.push_back({"TW3SE_AppendNamed", &Tw3AppendNamed, 2});
    g_natives.push_back({"TW3SE_FileSize", &Tw3FileSize, 1});
    g_natives.push_back({"TW3SE_DeleteNamed", &Tw3DeleteNamed, 1});
    g_natives.push_back({"TW3SE_ReadNamed", &Tw3ReadNamed, 1});
    g_natives.push_back({"TW3SE_GetConfig", &Tw3GetConfig, 1});
    g_natives.push_back({"TW3SE_StringToName", &Tw3StringToName, 1});
    QueueMessageNatives();  // TW3SE_Send, TW3SE_Receive, TW3SE_Pending (messages.cpp)
    g_natives.push_back({"TW3SE_SetVar", &Tw3SetVar, 2});
    g_natives.push_back({"TW3SE_GetVar", &Tw3GetVar, 1});
    g_natives.push_back({"TW3SE_HasVar", &Tw3HasVar, 1});
    g_natives.push_back({"TW3SE_DeleteVar", &Tw3DeleteVar, 1});
    g_natives.push_back({"TW3SE_Log", &Tw3Log, 1});
    g_natives.push_back({"TW3SE_FileExists", &Tw3FileExists, 1});
    g_natives.push_back({"TW3SE_KeyPressed", &Tw3KeyPressed, 1});
    g_natives.push_back({"TW3SE_KeyJustPressed", &Tw3KeyJustPressed, 1});
    g_natives.push_back({"TW3SE_GetConfigKey", &Tw3GetConfigKey, 1});
    g_natives.push_back({"TW3SE_ReloadScripts", &Tw3ReloadScripts, 0});
    g_natives.push_back({"TW3SE_EntityGuid", &Tw3EntityGuid, 1});
    g_natives.push_back({"TW3SE_EntityTemplate", &Tw3EntityTemplate, 1});
    g_natives.push_back({"TW3SE_EntityEffects", &Tw3EntityEffects, 1});
    g_natives.push_back({"TW3SE_ProbeArg", &Tw3ProbeArg, 1});
    g_natives.push_back({"TW3SE_ProbeName", &Tw3ProbeName, 1});
    g_natives.push_back({"TW3SE_Probe2a", &Tw3ProbeN<2>, 2});
    g_natives.push_back({"TW3SE_Probe1o", &Tw3ProbeOut, 1});
    g_natives.push_back({"TW3SE_Probe1h", &Tw3ProbeN<1>, 1});
    g_natives.push_back({"TW3SE_Probe1n", &Tw3ProbeN<1>, 1});
    g_natives.push_back({"TW3SE_ProbeFns", &Tw3ProbeFns, 0});
    void* target = reinterpret_cast<void*>(Address("RegisterCoreScriptNatives"));
    if (!target ||
        !Hook(target, reinterpret_cast<void*>(&RegisterCoreHook), reinterpret_cast<void**>(&g_origRegisterCore))) {
        Log("scripting: could not hook native registration - natives unavailable");
        return;
    }
    HookNamed("CScriptCompiler::DeclareGlobalFunction", reinterpret_cast<void*>(&DeclareGlobalHook),
              reinterpret_cast<void**>(&g_origDeclare));  // without it two mods importing one native clash
    RegisterCommand("natives", &CmdNatives, "list the registered WitcherScript natives");
    RegisterCommand("findfn", &CmdFindFn, "findfn <FunctionName>: look a function up in the global hashmap");
    RegisterCommand("nativeat", &CmdNativeAt, "nativeat <opcode>: GScriptNativeTable[opcode] handler as an RVA (analysis)");
    RegisterCommand("methodadjust", &CmdMethodAdjust, "methodadjust: scan GScriptMethodTable this-adjust bytes (analysis)");
    RegisterCommand("methodat", &CmdMethodAt, "methodat <opcode>: GScriptMethodTable[opcode] handler as an RVA (analysis)");
    RegisterCommand("callfn", &CmdCallFn, "callfn <GlobalFunction> [stringArg]: call a native from C++");
    RegisterCommand("callm", &CmdCallMethod, "callm <Method> [stringArg]: call a method-native on thePlayer");
    RegisterCommand("player", &CmdPlayer, "player [field] [value]: read or set a live player property");
    RegisterCommand("global", &CmdGlobal, "global <name> [field] [value]: read/set a predefined WS global");
    RegisterCommand("props", &CmdProps, "props <global|ClassName> [filter]: dump property names to tw3se.log");
    RegisterCommand("methods", &CmdMethods, "methods <global|ClassName> [filter]: dump method names to tw3se.log");
    Log("scripting: native registration hooked");
}

// The store for C++ and Lua (F2's mod settings, lua.cpp w3._var_get/_var_set), the same limits as TW3SE_SetVar.
bool VarGet(const std::string& key, std::string& value) {
    VarsLoad();
    auto it = g_vars.find(key);
    if (it == g_vars.end()) return false;
    value = it->second;
    return true;
}
bool VarSet(const std::string& key, const std::string* value) {
    VarsLoad();
    auto it = g_vars.find(key);
    size_t old = it == g_vars.end() ? 0 : key.size() + it->second.size();
    if (!value) {
        if (it == g_vars.end()) return false;
        g_varsBytes -= old;
        g_vars.erase(it);
        g_varsDirty = true;
        return true;
    }
    if (key.empty() || key.size() > 1024 || value->size() > (64u << 10) ||
        g_varsBytes - old + key.size() + value->size() > (16u << 20))
        return false;
    if (it != g_vars.end() && it->second == *value) return true;
    g_varsBytes = g_varsBytes - old + key.size() + value->size();
    g_vars[key] = *value;
    g_varsDirty = true;
    return true;
}

void TickVars() {
    if (!g_varsDirty || GetTickCount64() - g_varsSaved < 1000) return;
    g_varsDirty = false;
    g_varsSaved = GetTickCount64();
    if (!VarsSave()) Log("vars: tw3se\\vars.txt could not be written");
}

namespace {
void FlushVarsPlain() {
    if (g_varsDirty) {
        g_varsDirty = false;
        VarsSave();
    }
}
}  // namespace

// At the game's exit (DLL_PROCESS_DETACH): no log, no lock - the other threads are gone, one may have held it.
void FlushVars() {
    __try {
        FlushVarsPlain();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// One of TW3SE's own natives by name, from TW3SE's own list (they never move); the engine's global function map
// only when it is not in the list yet (a per-call lookup of TW3SE_LuaArg<n> in the engine's map can fail for a
// short stretch).
void* OurNative(const char* name) {
    void* fn = FindOurs(name);
    return fn ? fn : FindGlobalFn(name);
}

}  // namespace tw3se
