// TW3SE core - developer console on the pipe \\.\pipe\tw3se (one line in, one reply out; tools/tw3se.py).
// Commands come from the core and from plugins (RegisterCommand); they run on the main thread.
#include "core.h"

#include <shlobj.h>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>

namespace tw3se {
namespace {

struct Entry {
    TW3SE_Command handler;
    std::string help;
};
std::mutex g_mx;
std::map<std::string, Entry> g_commands;

struct Job {
    TW3SE_Command handler;
    std::string args;
    char reply[4096] = {};
};

// A command's handler under a guard (a plugin's command too): a crash in it becomes the command's reply. No C++
// objects here (C2712).
bool CommandGuarded(TW3SE_Command handler, const char* args, char* reply, uint32_t size, DWORD* code) {
    __try {
        handler(args, reply, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

void RunCommand(TW3SE_Command handler, const char* args, char* reply, uint32_t size) {
    DWORD code = 0;
    if (!CommandGuarded(handler, args, reply, size, &code))
        snprintf(reply, size, "error: the command crashed (0x%08lx); the game goes on", code);
}

void RunJob(void* p) {
    auto j = static_cast<Job*>(p);
    RunCommand(j->handler, j->args.c_str(), j->reply, sizeof j->reply);
}

void CmdHelp(const char*, char* reply, uint32_t size) {
    std::string s = "TW3SE " + std::to_string(TW3SE_VERSION_MAJOR) + "." + std::to_string(TW3SE_VERSION_MINOR) + "." +
                    std::to_string(TW3SE_VERSION_PATCH) + " commands:";
    std::lock_guard<std::mutex> l(g_mx);
    for (auto& [name, e] : g_commands) s += " | " + name + (e.help.empty() ? "" : " - " + e.help);
    strncpy_s(reply, size, s.c_str(), _TRUNCATE);
}

std::string Execute(const std::string& line) {
    std::string name = line.substr(0, line.find(' '));
    std::string args = line.size() > name.size() ? line.substr(name.size() + 1) : "";
    if (name == "ping") return "pong";  // answered here: tells a client the game is up before the first tick
    if (name == "events") return EventsReply(args.c_str());  // answered here too: no tick needed (link.cpp)
    TW3SE_Command handler = nullptr;
    {
        std::lock_guard<std::mutex> l(g_mx);
        auto it = g_commands.find(name);
        if (it != g_commands.end()) handler = it->second.handler;
    }
    if (!handler) return "error unknown command '" + name + "' (try help)";
    if (IsRedkitEditor() && !MainThreadTicking()) return "error the game is not running in the editor (press Play)";
    auto job = new Job{handler, args};
    if (!RunAndWait(&RunJob, job, 60000)) return "error timeout (is the game ticking?)";  // job leaked on purpose
    std::string reply = job->reply;
    delete job;
    return reply;
}

// One client connection: one line in, one reply out.
DWORD WINAPI ServeClient(void* param) {
    HANDLE p = param;
    char buf[4096];
    DWORD n = 0;
    if (ReadFile(p, buf, sizeof buf - 1, &n, nullptr) && n) {
        buf[n] = 0;
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        // Longer than the buffer: the rest is read away and the line refused (cut off, the client would still write
        // and this thread wait for it to read the reply).
        bool tooLong = n == sizeof buf - 1 && !memchr(buf, '\n', n);
        if (tooLong) {
            char sink[4096];
            DWORD got = 0, avail = 0;
            ULONGLONG until = GetTickCount64() + 2000;
            while (GetTickCount64() < until && PeekNamedPipe(p, nullptr, 0, nullptr, &avail, nullptr)) {
                if (!avail) {
                    Sleep(5);
                    continue;
                }
                if (!ReadFile(p, sink, sizeof sink, &got, nullptr) || !got) break;
                if (memchr(sink, '\n', got)) break;
                until = GetTickCount64() + 2000;
            }
        }
        // `!...`: the watchdog, right here on the pipe's thread (it answers while the main thread hangs)
        std::string reply = tooLong            ? "error line too long (at most 4095 bytes)"
                            : (!line.empty() && line[0] == '!') ? WatchdogCommand(line.substr(1))
                                                                 : Execute(line);
        // the editor sends exec/events many times a second: only other commands go to the log
        std::string name = line.substr(0, line.find(' '));
        if (tooLong)
            Log("console: a line longer than 4095 bytes refused ('%.40s...')", line.c_str());
        else if (name != "ping" && name != "state" && name != "exec" && name != "events")
            Log("console: '%s' -> %s", line.c_str(), reply.c_str());
        reply += "\n";
        DWORD w;
        WriteFile(p, reply.data(), static_cast<DWORD>(reply.size()), &w, nullptr);
        FlushFileBuffers(p);
    }
    DisconnectNamedPipe(p);
    CloseHandle(p);
    return 0;
}

// The pipe's name: `pipe = <name>` in tw3se.config, else tw3se (the REDkit editor: tw3se_redkit, so the editor and
// the game run side by side). A second game install sets its own name (with two games on one name the second gets
// no pipe and tools talk to the wrong game).
std::string g_pipe;

std::string PipeFromConfig() {
    std::string name = ConfigGet("pipe", IsRedkitEditor() ? "tw3se_redkit" : "tw3se");
    bool ok = !name.empty() && name.size() <= 64;
    for (char c : name)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') ok = false;
    if (ok) return name;
    std::string fallback = IsRedkitEditor() ? "tw3se_redkit" : "tw3se";
    Log("console: pipe = '%s' in tw3se.config is no valid name (letters, digits, _ and -, at most 64) - using %s",
        name.c_str(), fallback.c_str());
    return fallback;
}

HANDLE NewInstance(DWORD extraFlags) {
    std::wstring name = L"\\\\.\\pipe\\" + std::wstring(g_pipe.begin(), g_pipe.end());
    return CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | extraFlags, PIPE_TYPE_BYTE | PIPE_WAIT,
                            PIPE_UNLIMITED_INSTANCES, 8192, 8192, 0, nullptr);
}

// Several clients at once (the editor polls events while an exec waits for its tick), one thread each. Two
// listeners wait side by side, so a client almost never arrives in the moment between one connection and the
// next waiting instance. `param` is an instance created beforehand, or null.
DWORD WINAPI Listen(void* param) {
    HANDLE p = param;
    for (;; p = nullptr) {
        if (!p) p = NewInstance(0);
        if (p == INVALID_HANDLE_VALUE) {
            Sleep(50);  // out of instances or resources for a moment: try again
            continue;
        }
        if (ConnectNamedPipe(p, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            HANDLE t = CreateThread(nullptr, 0, &ServeClient, p, 0, nullptr);
            if (t) CloseHandle(t);
            else ServeClient(p);
        } else {
            CloseHandle(p);
        }
    }
}

}  // namespace

// A console command from the in-game console (ui.cpp), already on the main thread: the handler runs right here
// (Execute would wait for the next tick, which is this one).
std::string RunCommandLine(const std::string& line) {
    std::string name = line.substr(0, line.find(' '));
    std::string args = line.size() > name.size() ? line.substr(name.size() + 1) : "";
    if (name == "events") return EventsReply(args.c_str());
    TW3SE_Command handler = nullptr;
    {
        std::lock_guard<std::mutex> l(g_mx);
        auto it = g_commands.find(name);
        if (it != g_commands.end()) handler = it->second.handler;
    }
    if (!handler) return "error unknown command '" + name + "' (try /help)";
    std::string reply(4096, '\0');
    RunCommand(handler, args.c_str(), &reply[0], static_cast<uint32_t>(reply.size()));
    reply.resize(strlen(reply.c_str()));
    Log("console (in game): '%s' -> %s", line.c_str(), reply.c_str());
    return reply;
}

// `report`: one file with what a bug report needs: versions, settings, plugins, Lua mods, the mods and DLCs
// of the game folder, and the end of tw3se.log. Written to tw3se\out\report.txt.
std::string ReadTextFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::string s(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD got = 0;
    if (!s.empty()) ReadFile(h, &s[0], static_cast<DWORD>(s.size()), &got, nullptr);
    CloseHandle(h);
    s.resize(got);
    return s;
}

std::string FolderNames(const std::wstring& dir) {
    std::string out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return "(none)";
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        char n[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, n, sizeof n, nullptr, nullptr);
        out += std::string(out.empty() ? "" : ", ") + n;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return out.empty() ? "(none)" : out;
}

// The mods the game loads beside Mods\ and dlc\ (a report with "[Mods] (none)" can have its mods from
// mod.io): mod.io's (%PUBLIC%\mod.io\8254, the game's id there), Steam Workshop's
// (steamapps\workshop\content\292030), and the game's own two files about them in Documents\The Witcher 3:
// modioModsConfig.json (which are on) and modioModsLoadFailDump.json (written when a mod failed to load: script
// error, content error, missing scripts blob).
namespace {
std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

// The "name" values of a JSON file, in order, without a parser: enough to name mods in a report.
std::string JsonNames(const std::string& json) {
    std::string out;
    int n = 0;
    for (size_t at = 0; n < 60 && (at = json.find("ame\"", at)) != std::string::npos; ++at) {
        if (at == 0 || (json[at - 1] != 'n' && json[at - 1] != 'N')) continue;
        size_t q = json.find('"', json.find(':', at + 4) + 1);
        size_t e = q == std::string::npos ? q : json.find('"', q + 1);
        if (e == std::string::npos || e - q > 120) continue;
        out += (n++ ? ", " : "") + json.substr(q + 1, e - q - 1);
        at = e;
    }
    return out;
}

std::string Trimmed(const std::string& s, size_t max) {
    return s.size() > max ? s.substr(0, max) + "\r\n... (" + std::to_string(s.size()) + " bytes)" : s;
}
}  // namespace

std::string ModioText() {
    std::string r;
    std::wstring modio = KnownFolder(FOLDERID_Public) + L"\\mod.io\\8254";
    std::string names = JsonNames(ReadTextFile(modio + L"\\metadata\\state.json"));
    r += "[mod.io] " + FolderNames(modio + L"\\mods") + (names.empty() ? "" : " (" + names + ")") + "\r\n";
    std::wstring game = Dir() + L"\\..\\..\\..";
    r += "[Steam Workshop] " + FolderNames(game + L"\\..\\..\\workshop\\content\\292030") + "\r\n";
    std::wstring docs = KnownFolder(FOLDERID_Documents) + L"\\The Witcher 3\\";
    for (const wchar_t* f : {L"modioModsConfig.json", L"modioModsLoadFailDump.json"}) {
        std::string text = ReadTextFile(docs + f);
        if (text.empty() || text == "{}") continue;
        char name[64];
        WideCharToMultiByte(CP_UTF8, 0, f, -1, name, sizeof name, nullptr, nullptr);
        r += std::string("[") + name + "]\r\n" + Trimmed(text, 4000) + "\r\n";
    }
    return r;
}

namespace {
void CmdReport(const char*, char* reply, uint32_t size) {
    bool ok = false;
    std::string r = BuildReport(&ok);
    snprintf(reply, size, ok ? "report written: bin\\x64_dx12\\tw3se\\out\\report.txt (%u bytes)" : "error the report "
             "could not be written", static_cast<unsigned>(r.size()));
}
}  // namespace

// What a bug report needs: versions, plugins, Lua mods, config, mods, the script stack, the last Lua errors with
// their tracebacks and the end of the log; written to tw3se\out\report.txt and returned (the console's "Copy
// report" puts it on the clipboard).
std::string BuildReport(bool* written) {
    std::wstring game = Dir() + L"\\..\\..\\..";
    std::string r = "TW3SE report\r\n\r\n";
    WIN32_FILE_ATTRIBUTE_DATA crash{};
    if (GetFileAttributesExW((Dir() + L"\\out\\crash_report.txt").c_str(), GetFileExInfoStandard, &crash)) {
        FILETIME local;
        SYSTEMTIME t;
        FileTimeToLocalFileTime(&crash.ftLastWriteTime, &local);
        FileTimeToSystemTime(&local, &t);
        char line[128];
        snprintf(line, sizeof line, "[crash report] tw3se\\out\\crash_report.txt, from %04d-%02d-%02d %02d:%02d\r\n",
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
        r += line;
        // its head: how that session ended, its faults and what Windows saw (the part that names the cause; a post
        // with "Copy report" carries it)
        std::string cr = ReadTextFile(Dir() + L"\\out\\crash_report.txt");
        size_t from = cr.find("[the last session]"), to = cr.find("[tw3se.config]");
        if (from != std::string::npos) {
            std::string head = cr.substr(from, to != std::string::npos && to > from ? to - from : std::string::npos);
            if (head.size() > 3000) head = head.substr(0, 3000) + "\r\n...\r\n";
            r += head;
        }
        r += "\r\n";
    }
    for (const char* c : {"version", "plugins", "natives", "luamods", "ui state", "reloadscripts"})
        r += std::string("[") + c + "] " + RunCommandLine(c) + "\r\n";
    r += "\r\n[tw3se.config]\r\n" + ReadTextFile(Dir() + L"\\tw3se.config") + "\r\n";
    r += "\r\n[Mods] " + FolderNames(game + L"\\Mods") + "\r\n[dlc] " + FolderNames(game + L"\\dlc") + "\r\n";
    r += ModioText();
    r += "\r\n[script stack, main thread]\r\n" + ScriptStackText(true);
    r += "\r\n[the last Lua errors, newest first]\r\n" + LuaRecentErrors();
    std::string log = ReadTextFile(Dir() + L"\\tw3se.log");
    size_t cut = log.size(), lines = 0;
    while (cut > 0 && lines < 200)
        if (log[--cut] == '\n') ++lines;
    r += "\r\n[tw3se.log, the last 200 lines]\r\n" + log.substr(cut) + "\r\n";
    std::wstring out = Dir() + L"\\out";
    CreateDirectoryW(out.c_str(), nullptr);
    out += L"\\report.txt";
    HANDLE h = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD w = 0;
    bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, r.data(), static_cast<DWORD>(r.size()), &w, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (written) *written = ok;
    return r;
}

bool RegisterCommand(const char* name, TW3SE_Command handler, const char* help) {
    if (!name || !*name || !handler) return false;
    std::lock_guard<std::mutex> l(g_mx);
    if (g_commands.count(name)) {
        Log("console: command '%s' registered twice - the first one stays", name);
        return false;
    }
    g_commands[name] = {handler, help ? help : ""};
    return true;
}

// A plugin is unloaded: its commands go (their handlers lie in [lo, hi)).
int UnregisterCommandsIn(uintptr_t lo, uintptr_t hi) {
    std::lock_guard<std::mutex> l(g_mx);
    int n = 0;
    for (auto it = g_commands.begin(); it != g_commands.end();) {
        uintptr_t f = reinterpret_cast<uintptr_t>(it->second.handler);
        if (f >= lo && f < hi) {
            it = g_commands.erase(it);
            ++n;
        } else {
            ++it;
        }
    }
    return n;
}

void StartConsole() {
    RegisterCommand("help", &CmdHelp, "this list");
    RegisterCommand("report", &CmdReport, "report: what a bug report needs, in tw3se\\out\\report.txt");
    // The first instance is created with FILE_FLAG_FIRST_PIPE_INSTANCE: a second game with TW3SE cannot join
    // the same pipe name and answer for this one.
    g_pipe = PipeFromConfig();
    HANDLE first = NewInstance(FILE_FLAG_FIRST_PIPE_INSTANCE);
    if (first == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY)
            Log("console: PIPE TAKEN - \\\\.\\pipe\\%s belongs to another process (another game or editor with TW3SE). "
                "This game has no console. Give one of them its own name: pipe = <name> in its tw3se.config",
                g_pipe.c_str());
        else
            Log("console: pipe \\\\.\\pipe\\%s not created (%lu)", g_pipe.c_str(), err);
        return;
    }
    Log("console: pipe \\\\.\\pipe\\%s", g_pipe.c_str());
    CloseHandle(CreateThread(nullptr, 0, &Listen, first, 0, nullptr));
    CloseHandle(CreateThread(nullptr, 0, &Listen, nullptr, 0, nullptr));
}

}  // namespace tw3se
