// TW3SE core - w3.proc (radish's encoders, REDkit's wcc_lite): a program run without a console window, its output
// read line by line while it runs. Shared with the UI lab.
//
//   local p = w3.proc.run(command_line [, {cwd = dir}])    -> id (nil, error when it cannot start)
//   for _, line in ipairs(w3.proc.lines(p)) do ... end      the lines since the last call (stdout and stderr)
//   local code = w3.proc.exit(p)                            nil while it runs, its exit code after
//   w3.proc.kill(p)   w3.proc.close(p)                       end it; forget it (after its last lines were read)
//   w3.proc.open(path_or_url [, {select = true}])           opened as Windows opens it (Explorer, the browser);
//                                                           select: its folder in Explorer, the file chosen
//
// Lua's os.execute and io.popen open a console window over the game; this does not. A window to be seen (a folder,
// a web page) comes with open: run starts everything hidden, explorer.exe too.
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "lua.h"
#include "lauxlib.h"

namespace tw3se {
namespace {

struct Proc {
    HANDLE process = nullptr;
    std::mutex mx;
    std::string partial;               // a line not finished yet
    std::vector<std::string> lines;    // finished lines not read yet
    bool done = false;
    bool closed = false;  // w3.proc.close: the reader closes the handle when the program ends
    DWORD code = 0;
};
std::mutex g_mx;
std::map<int, std::shared_ptr<Proc>> g_procs;
int g_next = 1;

std::wstring Wide(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

// The reader: the pipe until the program closes it, then its exit code.
void Read(std::shared_ptr<Proc> p, HANDLE pipe) {
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(pipe, buf, sizeof buf, &got, nullptr) && got > 0) {
        std::lock_guard<std::mutex> lk(p->mx);
        p->partial.append(buf, got);
        size_t nl;
        while ((nl = p->partial.find('\n')) != std::string::npos) {
            std::string line = p->partial.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            p->lines.push_back(line);
            p->partial.erase(0, nl + 1);
        }
    }
    CloseHandle(pipe);
    WaitForSingleObject(p->process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(p->process, &code);
    std::lock_guard<std::mutex> lk(p->mx);
    if (!p->partial.empty()) p->lines.push_back(p->partial), p->partial.clear();
    p->code = code;
    p->done = true;
    if (p->closed) CloseHandle(p->process), p->process = nullptr;  // forgotten while it ran
}

std::shared_ptr<Proc> Get(lua_State* L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_procs.find(id);
    if (it == g_procs.end()) luaL_argerror(L, 1, "no such process (w3.proc.run gives the id)");
    return it->second;
}

int PRun(lua_State* L) {
    std::wstring cmd = Wide(luaL_checkstring(L, 1));
    std::wstring cwd;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "cwd");
        if (lua_isstring(L, -1)) cwd = Wide(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        lua_pushnil(L);
        lua_pushstring(L, "no pipe");
        return 2;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    DWORD err = GetLastError();
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        lua_pushnil(L);
        lua_pushfstring(L, "could not start (error %d)", static_cast<int>(err));
        return 2;
    }
    CloseHandle(pi.hThread);
    auto p = std::make_shared<Proc>();
    p->process = pi.hProcess;
    std::thread(Read, p, rd).detach();
    int id;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        id = g_next++;
        g_procs[id] = p;
    }
    lua_pushinteger(L, id);
    return 1;
}

int PLines(lua_State* L) {
    auto p = Get(L);
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lk(p->mx);
        lines.swap(p->lines);
    }
    lua_createtable(L, static_cast<int>(lines.size()), 0);
    for (size_t i = 0; i < lines.size(); ++i) {
        lua_pushlstring(L, lines[i].data(), lines[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int PExit(lua_State* L) {
    auto p = Get(L);
    std::lock_guard<std::mutex> lk(p->mx);
    if (!p->done) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(p->code));
    return 1;
}

int PKill(lua_State* L) {
    auto p = Get(L);
    TerminateProcess(p->process, 1);
    return 0;
}

int PClose(lua_State* L) {
    int id = static_cast<int>(luaL_checkinteger(L, 1));
    std::shared_ptr<Proc> p;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_procs.find(id);
        if (it == g_procs.end()) return 0;
        p = it->second;
        g_procs.erase(it);
    }
    std::lock_guard<std::mutex> lk(p->mx);
    if (p->done && p->process) CloseHandle(p->process), p->process = nullptr;
    else p->closed = true;  // a running one: its reader still waits on it and closes it
    return 0;
}

// ShellExecute on its own thread: it may wait for Explorer or the browser, the game must not
int POpen(lua_State* L) {
    std::wstring target = Wide(luaL_checkstring(L, 1));
    bool select = false;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "select");
        select = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    std::thread([target, select] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (select) {
            std::wstring args = L"/select,\"" + target + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
    lua_pushboolean(L, 1);
    return 1;
}

}  // namespace

// w3.proc into the table on top of the stack (w3)
void LuaProcOpen(lua_State* L) {
    static const luaL_Reg fns[] = {{"run", &PRun},   {"lines", &PLines}, {"exit", &PExit},
                                   {"kill", &PKill}, {"close", &PClose}, {"open", &POpen},
                                   {nullptr, nullptr}};
    luaL_newlib(L, fns);
    lua_setfield(L, -2, "proc");
}

}  // namespace tw3se
