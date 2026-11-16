// TW3SE core - the link an external tool (an editor) drives the game through.
//
//   exec <cmd>[<US><cmd>...]   run console commands (exec functions, `addfact(...)`, ...) through the engine's own
//                              command executor, the one the script debugger's "exec" uses. US = 0x1f separates
//                              several commands; all of them run in the same tick, in order.
//   events <seq>               the lines scripts sent after sequence number <seq>: TW3SE_Emit, and LogChannel on
//                              the channels in `events_channels` (default Cj).
//
// With these two the editor needs neither the script debugger port nor scriptslog.txt, so it works with a game
// started the normal way (no -debugscripts).
#include "core.h"
#include <deque>
#include <vector>
#include <mutex>

namespace tw3se {
namespace {

// The engine's string: a dynamic char array whose size counts the terminating NUL. Packed to 4 like the engine
// stores it (the command queue keeps it at offset 4 of a 16-byte entry).
#pragma pack(push, 4)
struct AnsiString {
    char* data;
    uint32_t size;
};
#pragma pack(pop)

// bool ExecCommand(unused, const AnsiString* command, AnsiString* output): runs one command line, captures what
// the engine logs meanwhile into *output (allocated by the engine, freed with Memory::Free). docs/engine/link.md.
using ExecCommand_t = bool (*)(void* unused, const AnsiString* command, AnsiString* output);
using Free_t = void (*)(void*);
using NamePoolGet_t = void* (*)();
using NamePoolAdd_t = uint32_t (*)(void* pool, const char* name);

// SEH must live in a function without C++ objects that need unwinding (C2712).
bool ExecGuarded(ExecCommand_t exec, const AnsiString* cmd, AnsiString* out, DWORD* code) {
    __try {
        return exec(nullptr, cmd, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

// REDkit editor: the game console's own executor (ConsoleExecCommand_ed, eb). (context, command line, quiet): it
// cuts the function name out of the line, checks the exec flag and calls it. Main thread only. It logs its result
// through the engine's formatting log, which TW3SE does not hook, so the name is checked here first.
using ConsoleExec_t = void (*)(void* context, const AnsiString* line, bool quiet);

bool ConsoleExecGuarded(ConsoleExec_t exec, const AnsiString* line, DWORD* code) {
    __try {
        exec(nullptr, line, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

void CmdExecEditor(const char* arg, char* reply, uint32_t size) {
    auto exec = Fn<ConsoleExec_t>("ConsoleExecCommand_ed");
    if (!exec) {
        strncpy_s(reply, size, "error exec not available in this build", _TRUNCATE);
        return;
    }
    std::string all = arg ? arg : "";
    int ran = 0, failed = 0;
    std::string firstError;
    size_t start = 0;
    while (start <= all.size()) {
        size_t end = all.find('\x1f', start);
        if (end == std::string::npos) end = all.size();
        std::string cmd = all.substr(start, end - start);
        start = end + 1;
        if (cmd.empty()) continue;
        ++ran;
        size_t nameEnd = cmd.find_first_of("( ");
        std::string name = cmd.substr(0, nameEnd);
        if (!FindGlobalFn(name.c_str())) {
            ++failed;
            if (firstError.empty()) firstError = cmd + ": unknown global function";
            continue;
        }
        AnsiString in{&cmd[0], static_cast<uint32_t>(cmd.size() + 1)};
        DWORD code = 0;
        if (!ConsoleExecGuarded(exec, &in, &code)) {
            ++failed;
            if (firstError.empty()) firstError = cmd + " (fault " + std::to_string(code) + ")";
        }
    }
    if (!failed) _snprintf_s(reply, size, _TRUNCATE, "ok %d", ran);
    else {
        Log("link: exec failed: %s", firstError.c_str());
        _snprintf_s(reply, size, _TRUNCATE, "error %d of %d failed: %s", failed, ran, firstError.c_str());
    }
}

void CmdExec(const char* arg, char* reply, uint32_t size) {
    if (IsRedkitEditor()) return CmdExecEditor(arg, reply, size);
    auto exec = Fn<ExecCommand_t>("CRemoteConnection::ExecCommand");
    auto free = Fn<Free_t>("Memory::Free");
    if (!exec || !free) {
        strncpy_s(reply, size, "error exec not available in this build", _TRUNCATE);
        return;
    }
    std::string all = arg ? arg : "";
    int ran = 0, failed = 0;
    std::string firstError;
    size_t start = 0;
    while (start <= all.size()) {
        size_t end = all.find('\x1f', start);
        if (end == std::string::npos) end = all.size();
        std::string cmd = all.substr(start, end - start);
        start = end + 1;
        if (cmd.empty()) continue;
        AnsiString in{&cmd[0], static_cast<uint32_t>(cmd.size() + 1)};
        AnsiString out{nullptr, 0};
        DWORD code = 0;
        bool ok = ExecGuarded(exec, &in, &out, &code);
        ++ran;
        if (!ok) {
            ++failed;
            if (firstError.empty()) {
                firstError = cmd;
                if (code) firstError += " (fault " + std::to_string(code) + ")";
                else if (out.data && out.size > 1) firstError += ": " + std::string(out.data);
            }
        }
        if (out.data) free(out.data);
    }
    if (!failed) _snprintf_s(reply, size, _TRUNCATE, "ok %d", ran);
    else {
        Log("link: exec failed: %s", firstError.c_str());
        _snprintf_s(reply, size, _TRUNCATE, "error %d of %d failed: %s", failed, ran, firstError.c_str());
    }
}

// Lines from TW3SE_Emit, each with a sequence number. Every reader keeps its own position, so several readers
// (the editor's listener, a one-off query) never take lines from each other. The oldest lines go past kKeep.
std::mutex g_mx;
std::deque<std::pair<uint64_t, std::string>> g_lines;
uint64_t g_seq = 0;
constexpr size_t kKeep = 8192;
constexpr size_t kReplyBytes = 3800;  // stays under the 4096 a client reads at once

void Push(const char* text) {
    std::string line = text ? text : "";
    for (char& c : line)
        if (c == '\n' || c == '\r') c = ' ';
    std::lock_guard<std::mutex> l(g_mx);
    g_lines.emplace_back(++g_seq, std::move(line));
    if (g_lines.size() > kKeep) g_lines.pop_front();
}

// import function TW3SE_Emit(text : string);  -> one line for the external tool (`events`)
void Tw3Emit(void* /*self*/, void* frame, void* /*result*/) {
    uint8_t buf[16] = {0};
    ReadScriptParam(frame, buf);
    EndScriptParams(frame);
    Push(*reinterpret_cast<char**>(buf));
}

// LogChannel lines of the channels in `events_channels` (default Cj) go into
// the same lines, so scripts that must run without TW3SE (a quest's runtime) still reach the tool. TW3SE takes
// the place of the engine's log sink and passes every line on to the sink it replaced (with -debugscripts that
// one writes scriptslog.txt). docs/engine/link.md.
using LogSink_t = void (*)(const uint32_t* channel, const AnsiString* text);
LogSink_t g_nextSink = nullptr;
LogSink_t* g_sinkSlot = nullptr;
std::vector<uint32_t> g_channels;

void LogSink(const uint32_t* channel, const AnsiString* text) {
    if (channel && text && text->data && text->size > 1)
        for (uint32_t c : g_channels)
            if (*channel == c) {
                Push(text->data);
                break;
            }
    if (g_nextSink) g_nextSink(channel, text);
}

// Once per tick on the main thread: take the sink slot, and take it back if the engine set its own meanwhile (it
// does at start with -debugscripts). Exec's capture never shows here: it is in the slot only inside a command.
void SinkTick(float, void*) {
    LogSink_t now = *g_sinkSlot;
    if (now == &LogSink) return;
    g_nextSink = now;
    *g_sinkSlot = &LogSink;
}

// Without -debugscripts the script compiler drops every call to Log/LogChannel: they never reach the native.
// `script_logs = on` turns that one branch in the compiler (CScriptCompiler::CompileCall+0x3a, `je` past the
// drop) into a `jmp`, so the calls stay. It has to happen before the scripts compile (here, at DLL load). The
// bytes are checked first; anything unexpected leaves the code alone. Costs what -debugscripts costs: every
// LogChannel in every script builds its text again.
void KeepScriptLogs() {
    if (!ConfigBool("script_logs", false)) return;
    auto f = reinterpret_cast<uint8_t*>(Address("CScriptCompiler::CompileCall"));
    static const uint8_t expect[9] = {0x80, 0xB8, 0x7C, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x84};  // cmp [rax+17c],0; je
    if (!f || memcmp(f + 0x33, expect, sizeof expect) != 0) {
        Log("link: script_logs not applied - the compiler code differs in this build");
        return;
    }
    uint8_t* je = f + 0x3a;
    int32_t rel = 0;
    memcpy(&rel, je + 2, 4);
    int32_t jmpRel = rel + 1;  // a 5-byte jmp ends one byte before the 6-byte je: same target
    uint8_t patch[6] = {0xE9, 0, 0, 0, 0, 0x90};
    memcpy(patch + 1, &jmpRel, 4);
    DWORD old = 0;
    if (!VirtualProtect(je, sizeof patch, PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(je, patch, sizeof patch);
    VirtualProtect(je, sizeof patch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), je, sizeof patch);
    Log("link: script_logs on - Log/LogChannel calls compile without -debugscripts");
}

void StartSink(void*) {
    g_sinkSlot = reinterpret_cast<LogSink_t*>(Address("GScriptLogSink"));
    auto poolGet = Fn<NamePoolGet_t>("CNamesPool::Get");
    auto poolAdd = Fn<NamePoolAdd_t>("CNamesPool::AddEntry");
    if (!g_sinkSlot || !poolGet || !poolAdd) {
        Log("link: LogChannel lines unavailable in this build (TW3SE_Emit still works)");
        return;
    }
    std::string list = ConfigGet("events_channels", "Cj");
    char buf[512];
    strncpy_s(buf, list.c_str(), _TRUNCATE);
    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, ", ", &ctx); tok; tok = strtok_s(nullptr, ", ", &ctx))
        g_channels.push_back(poolAdd(poolGet(), tok));
    if (g_channels.empty()) return;
    SinkTick(0, nullptr);
    RegisterTick(&SinkTick, nullptr);
    Log("link: LogChannel lines of '%s' go to events", list.c_str());
}

// The REDkit editor has no sink slot. Every LogChannel line with an active channel passes one function with fixed
// parameters (hash, category "Script_<channel>", level, finished text), from whatever thread logs. TW3SE hooks it
// at start-up, so lines arrive in edit mode too.
using LogChannelText_t = void (*)(uint32_t hash, const char* category, int level, const char* text);
LogChannelText_t g_nextText = nullptr;
std::vector<std::string> g_channelNames;

void LogChannelTextHook(uint32_t hash, const char* category, int level, const char* text) {
    if (category && text && *text && strncmp(category, "Script_", 7) == 0)
        for (const std::string& c : g_channelNames)
            if (_stricmp(category + 7, c.c_str()) == 0) {
                Push(text);
                break;
            }
    g_nextText(hash, category, level, text);
}

void StartSinkEditor() {
    std::string list = ConfigGet("events_channels", "Cj");
    char buf[512];
    strncpy_s(buf, list.c_str(), _TRUNCATE);
    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, ", ", &ctx); tok; tok = strtok_s(nullptr, ", ", &ctx)) g_channelNames.push_back(tok);
    if (g_channelNames.empty()) return;
    HookNamed("ScriptLogChannelText_ed", reinterpret_cast<void*>(&LogChannelTextHook),
              reinterpret_cast<void**>(&g_nextText));
    if (g_nextText) Log("link: LogChannel lines of '%s' go to events (REDkit editor)", list.c_str());
}

}  // namespace

// Answered on the pipe thread (no tick needed): "seq <last>[ lost <n>]" then one line per event. Without an
// argument only the current position comes back, so a new reader starts at the end, like tailing a log.
std::string EventsReply(const char* args) {
    std::lock_guard<std::mutex> l(g_mx);
    if (!args || !*args) return "seq " + std::to_string(g_seq);
    uint64_t since = _strtoui64(args, nullptr, 10);
    if (since > g_seq) since = 0;  // the position is from an earlier game session
    uint64_t lost = 0;
    if (!g_lines.empty() && since + 1 < g_lines.front().first) lost = g_lines.front().first - since - 1;
    std::string body;
    uint64_t last = since;
    for (auto& [seq, text] : g_lines) {
        if (seq <= since) continue;
        if (body.size() + text.size() + 1 > kReplyBytes) break;  // the rest comes with the next call
        body += '\n';
        body += text;
        last = seq;
    }
    std::string head = "seq " + std::to_string(last);
    if (lost) head += " lost " + std::to_string(lost);
    return head + body;
}

// one line for the external tool from the core itself (as TW3SE_Emit does for scripts)
void LinkEmit(const char* line) {
    Push(line);
}

// The pipe's `exec` for Lua (w3.exec, main thread): "ok <n>" or "error ...".
std::string LinkExec(const char* cmds) {
    char reply[1024] = {0};
    CmdExec(cmds, reply, sizeof reply);
    return reply;
}

// The pipe's `events` for Lua (w3.link.since): every line after `since`, no reply size limit. -> the last seq.
uint64_t LinkLines(uint64_t since, std::vector<std::string>* out, uint64_t* lost) {
    std::lock_guard<std::mutex> l(g_mx);
    if (since > g_seq) since = 0;  // a position from an earlier game session
    *lost = (!g_lines.empty() && since + 1 < g_lines.front().first) ? g_lines.front().first - since - 1 : 0;
    uint64_t last = since;
    for (auto& [seq, text] : g_lines) {
        if (seq <= since) continue;
        out->push_back(text);
        last = seq;
    }
    return last;
}
uint64_t LinkSeq() {
    std::lock_guard<std::mutex> l(g_mx);
    return g_seq;
}

void QueueLinkNatives() { QueueNative("TW3SE_Emit", &Tw3Emit, 1); }

void InstallLink() {
    QueueLinkNatives();
    if (IsRedkitEditor()) {
        StartSinkEditor();  // the editor compiles Log/LogChannel calls by itself: no script_logs patch
    } else {
        KeepScriptLogs();
        RunOnMainThread(&StartSink, nullptr);  // the first tick: the engine has set up its own sink by then
    }
    RegisterCommand("exec", &CmdExec, "exec <command>[\\x1f<command>...]: run console commands like the script debugger");
}

}  // namespace tw3se
