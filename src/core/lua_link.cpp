// TW3SE core - w3.exec and w3.link: what an external tool does through the pipe's `exec` and `events` (link.cpp),
// for a Lua mod. An editor in the game talks to its scripts this way as it does from outside: the same commands,
// the same lines back.
//
//   local ok, err = w3.exec('cj_set_place("main")')     console commands (exec functions); several with "\x1f"
//   local seq = w3.link.mark()                           the newest line's number (start reading from here)
//   local seq, lines, lost = w3.link.since(seq)          the lines after it (TW3SE_Emit, LogChannel on Cj)
//   local reply = w3.command("questlive on")             a TW3SE command (RegisterCommand: the core's, a plugin's),
//                                                        as the in-game console runs it - its reply
#include "core.h"
#include "lua.h"
#include "lauxlib.h"

namespace tw3se {
std::string RunCommandLine(const std::string& line);   // console.cpp: a command, on this (the main) thread
namespace {

int LExec(lua_State* L) {
    std::string r = LinkExec(luaL_checkstring(L, 1));
    if (r.rfind("ok", 0) == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    lua_pushstring(L, r.c_str());
    return 2;
}

int LCommand(lua_State* L) {
    std::string r = RunCommandLine(luaL_checkstring(L, 1));
    lua_pushlstring(L, r.data(), r.size());
    return 1;
}

int LMark(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(LinkSeq()));
    return 1;
}

int LSince(lua_State* L) {
    uint64_t since = static_cast<uint64_t>(luaL_optinteger(L, 1, 0));
    std::vector<std::string> lines;
    uint64_t lost = 0;
    uint64_t last = LinkLines(since, &lines, &lost);
    lua_pushinteger(L, static_cast<lua_Integer>(last));
    lua_createtable(L, static_cast<int>(lines.size()), 0);
    for (size_t i = 0; i < lines.size(); ++i) {
        lua_pushlstring(L, lines[i].data(), lines[i].size());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_pushinteger(L, static_cast<lua_Integer>(lost));
    return 3;
}

}  // namespace

// w3.exec and w3.link into the table on top of the stack (w3)
void LuaLinkOpen(lua_State* L) {
    lua_pushcfunction(L, &LExec);
    lua_setfield(L, -2, "exec");
    lua_pushcfunction(L, &LCommand);
    lua_setfield(L, -2, "command");
    static const luaL_Reg fns[] = {{"mark", &LMark}, {"since", &LSince}, {nullptr, nullptr}};
    luaL_newlib(L, fns);
    lua_setfield(L, -2, "link");
}

}  // namespace tw3se
