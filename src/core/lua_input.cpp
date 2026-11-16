// TW3SE core - w3.input (for an editor in the game): the keys and the mouse as Windows has them,
// the way a program next to the game reads them (GetAsyncKeyState, GetCursorPos), whatever the UI or the game does
// with the input.
//
//   w3.input.down(vk)          a key or mouse button held now (a virtual-key code), only with the game in front
//   w3.input.front()           the game is the window in front
//   w3.input.cursor()          -> x, y on the screen
//   w3.input.set_cursor(x, y)  the cursor put there (the game's own calls stay kept from it while the UI is open)
//   w3.input.client()          -> x, y, w, h: the game's picture on the screen
//   w3.input.wheel()           -> wheel notches since the last call (1 = one notch away from you)
#include "core.h"
#include "lua.h"
#include "lauxlib.h"

namespace tw3se {
bool UiGameInFront();
bool UiKeyDown(int vk);
bool UiCursorPos(int* x, int* y);
bool UiSetCursorPos(int x, int y);
bool UiClientRect(int* x, int* y, int* w, int* h);
double UiTakeWheel();

namespace {
int IDown(lua_State* L) {
    lua_pushboolean(L, UiKeyDown(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}
int IFront(lua_State* L) {
    lua_pushboolean(L, UiGameInFront());
    return 1;
}
int ICursor(lua_State* L) {
    int x = 0, y = 0;
    if (!UiCursorPos(&x, &y)) return 0;
    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    return 2;
}
int ISetCursor(lua_State* L) {
    lua_pushboolean(L, UiSetCursorPos(static_cast<int>(luaL_checkinteger(L, 1)), static_cast<int>(luaL_checkinteger(L, 2))));
    return 1;
}
int IClient(lua_State* L) {
    int x = 0, y = 0, w = 0, h = 0;
    if (!UiClientRect(&x, &y, &w, &h)) return 0;
    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 4;
}
int IWheel(lua_State* L) {
    lua_pushnumber(L, UiTakeWheel());
    return 1;
}
}  // namespace

void LuaInputOpen(lua_State* L) {
    static const luaL_Reg fns[] = {{"down", &IDown},     {"front", &IFront},   {"cursor", &ICursor},
                                   {"set_cursor", &ISetCursor}, {"client", &IClient}, {"wheel", &IWheel},
                                   {nullptr, nullptr}};
    luaL_newlib(L, fns);
    lua_setfield(L, -2, "input");
}

}  // namespace tw3se
