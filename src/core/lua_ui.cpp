// TW3SE core - w3.ui, Dear ImGui for Lua: the same file goes into TW3SE and into a UI lab outside the game, so a
// window drawn there looks as it does in the game.
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "lua_ui.h"

namespace tw3se {
namespace {

// --- w3.ui: Dear ImGui for Lua windows. ui.cpp builds ImGui's frame on the main thread while the in-game UI is open
// and runs the w3.on_ui functions inside it (LuaRunUi). Calls with a body (window, tree, collapsing, table, child,
// id) take a function and close what they opened even when it raises an error.
}  // namespace
bool g_uiFrame = false;
int g_uiTables = 0;  // w3.ui.table bodies open right now
unsigned (*g_uiPadMask)() = nullptr;        // the host's controller (pad.cpp in the game)
const char* (*g_uiPadName)(int) = nullptr;
void (*g_uiLog)(const char* fmt, ...) = nullptr;
namespace {

void UiCheck(lua_State* L) {
    if (!g_uiFrame) luaL_error(L, "w3.ui works inside a w3.on_ui function");
}

// What ImGui would only assert (and then run on with: ui.row() outside a table reads through a NULL table) is a Lua
// error here: finite numbers, a window title, rows and cells inside a table.
double UiNum(lua_State* L, int i) {
    double v = luaL_checknumber(L, i);
    if (!std::isfinite(v) || std::fabs(v) > 1e15) luaL_argerror(L, i, "a finite number is needed");
    return v;
}

// Calls the function at `idx` (no arguments), then `close`; an error inside is raised again after the close.
template <class Close> int UiBody(lua_State* L, int idx, bool open, Close close) {
    int err = LUA_OK;
    if (open && lua_isfunction(L, idx)) {
        lua_pushvalue(L, idx);
        err = lua_pcall(L, 0, 0, 0);
    }
    close();
    if (err != LUA_OK) return lua_error(L);
    lua_pushboolean(L, open);
    return 1;
}

// w3.ui.window(title, fn [, width, height [, closable]]): a window; fn draws its content. With closable its title
// bar has an X: the second value is false once it was clicked (the caller stops drawing the window then).
int LUiWindow(lua_State* L) {
    UiCheck(L);
    const char* title = luaL_checkstring(L, 1);
    if (!*title) return luaL_argerror(L, 1, "a window needs a title");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (lua_isnumber(L, 3) && lua_isnumber(L, 4))
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(UiNum(L, 3)), static_cast<float>(UiNum(L, 4))),
                                 ImGuiCond_FirstUseEver);
    bool closable = lua_toboolean(L, 5) != 0;
    bool keep = true;
    bool open = closable ? ImGui::Begin(title, &keep) : ImGui::Begin(title);
    int n = UiBody(L, 2, open, [] { ImGui::End(); });
    if (!closable) return n;
    lua_pushboolean(L, keep);
    return n + 1;
}
int LUiTree(lua_State* L) {  // w3.ui.tree(label, fn)
    UiCheck(L);
    bool open = ImGui::TreeNode(luaL_checkstring(L, 1));
    return UiBody(L, 2, open, [open] { if (open) ImGui::TreePop(); });
}
int LUiCollapsing(lua_State* L) {  // w3.ui.collapsing(label, fn)
    UiCheck(L);
    bool open = ImGui::CollapsingHeader(luaL_checkstring(L, 1));
    return UiBody(L, 2, open, [] {});
}
int LUiChild(lua_State* L) {  // w3.ui.child(id, height, fn [, width]): a scrolling region (0 = the rest)
    UiCheck(L);
    const char* id = luaL_checkstring(L, 1);
    float h = lua_isnoneornil(L, 2) ? 0.0f : static_cast<float>(UiNum(L, 2));
    float w = lua_isnoneornil(L, 4) ? 0.0f : static_cast<float>(UiNum(L, 4));
    bool open = ImGui::BeginChild(id, ImVec2(w, h), ImGuiChildFlags_Borders);
    return UiBody(L, 3, true, [open] { (void)open; ImGui::EndChild(); });
}
int LUiId(lua_State* L) {  // w3.ui.id(key, fn): widgets inside fn get their own ids (same labels in a loop)
    UiCheck(L);
    ImGui::PushID(luaL_tolstring(L, 1, nullptr));
    lua_pop(L, 1);
    return UiBody(L, 2, true, [] { ImGui::PopID(); });
}
// A field of the table at `t`: a boolean (nil: `def`), a number (nil: `def`), a string (nil: nullptr).
bool UiOptBool(lua_State* L, int t, const char* k, bool def) {
    if (!lua_istable(L, t)) return def;
    lua_getfield(L, t, k);
    bool v = lua_isnil(L, -1) ? def : lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}
double UiOptNum(lua_State* L, int t, const char* k, double def) {
    if (!lua_istable(L, t)) return def;
    lua_getfield(L, t, k);
    double v = def;
    if (lua_isnumber(L, -1)) v = lua_tonumber(L, -1);
    else if (!lua_isnil(L, -1)) luaL_error(L, "'%s' needs a number", k);
    lua_pop(L, 1);
    if (!std::isfinite(v) || std::fabs(v) > 1e15) luaL_error(L, "'%s' needs a finite number", k);
    return v;
}
bool UiOptHas(lua_State* L, int t, const char* k) {
    if (!lua_istable(L, t)) return false;
    lua_getfield(L, t, k);
    bool has = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return has;
}

// w3.ui.table(id, columns, fn [, opts]): columns with headers. A column is its header ("Name") or a table of it:
// {"Stages", width = 40} a fixed width, {"Name", stretch = 2} a share of the rest, {"Date", fit = true} as wide as
// its widest cell, sort = false not sortable, sort = "asc" / "desc" the order the table starts in, resize = false,
// hide = true (hidden until the header's right-click menu shows it). opts: sortable, multi_sort, height (a scrolling
// table, its header stays; 0: down to the window's bottom), scroll_x, freeze (columns kept on the left while it
// scrolls sideways), borders, row_bg, resizable, reorder, hideable, headers. The opts may also come before fn.
int LUiTable(lua_State* L) {
    UiCheck(L);
    const char* id = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    int fn = 3, opt = 4;
    if (lua_istable(L, 3)) fn = 4, opt = 3;
    if (!lua_isnoneornil(L, opt) && !lua_istable(L, opt)) return luaL_argerror(L, opt, "a table of options");
    int cols = static_cast<int>(lua_rawlen(L, 2));
    if (cols < 1 || cols > 64) return luaL_error(L, "w3.ui.table: 1 to 64 columns");
    ImGuiTableFlags f = 0;
    if (UiOptBool(L, opt, "borders", true)) f |= ImGuiTableFlags_Borders;
    if (UiOptBool(L, opt, "row_bg", true)) f |= ImGuiTableFlags_RowBg;
    if (UiOptBool(L, opt, "resizable", true)) f |= ImGuiTableFlags_Resizable;
    if (UiOptBool(L, opt, "reorder", false)) f |= ImGuiTableFlags_Reorderable;
    if (UiOptBool(L, opt, "hideable", false)) f |= ImGuiTableFlags_Hideable;
    if (UiOptBool(L, opt, "multi_sort", false)) f |= ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti;
    bool sortable = UiOptBool(L, opt, "sortable", false);
    bool scrollY = UiOptHas(L, opt, "height");
    float height = static_cast<float>(UiOptNum(L, opt, "height", 0));
    if (scrollY) f |= ImGuiTableFlags_ScrollY;
    if (UiOptBool(L, opt, "scroll_x", false)) f |= ImGuiTableFlags_ScrollX;
    int freeze = static_cast<int>(UiOptNum(L, opt, "freeze", 0));
    bool headers = UiOptBool(L, opt, "headers", true);
    // the columns are read before the table opens (an error in them leaves nothing open)
    struct Col { std::string label; ImGuiTableColumnFlags flags; float w; };
    std::vector<Col> spec(static_cast<size_t>(cols));
    for (int i = 1; i <= cols; ++i) {
        lua_rawgeti(L, 2, i);
        int c = lua_gettop(L);
        Col& col = spec[static_cast<size_t>(i - 1)];
        col.flags = 0;
        col.w = 0;
        if (lua_istable(L, c)) {
            lua_rawgeti(L, c, 1);
            if (lua_isnil(L, -1)) lua_pop(L, 1), lua_getfield(L, c, "label");
            if (lua_isstring(L, -1)) col.label = lua_tostring(L, -1);
            lua_pop(L, 1);
            if (UiOptHas(L, c, "width")) {
                col.flags |= ImGuiTableColumnFlags_WidthFixed;
                col.w = static_cast<float>(UiOptNum(L, c, "width", 0));
            } else if (UiOptBool(L, c, "fit", false)) {
                col.flags |= ImGuiTableColumnFlags_WidthFixed;
            } else if (UiOptHas(L, c, "stretch")) {
                col.flags |= ImGuiTableColumnFlags_WidthStretch;
                col.w = static_cast<float>(UiOptNum(L, c, "stretch", 1));
            }
            if (!UiOptBool(L, c, "resize", true)) col.flags |= ImGuiTableColumnFlags_NoResize;
            if (UiOptBool(L, c, "hide", false)) col.flags |= ImGuiTableColumnFlags_DefaultHide;
            lua_getfield(L, c, "sort");
            const char* s = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
            if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) col.flags |= ImGuiTableColumnFlags_NoSort;
            else if (s && strcmp(s, "asc") == 0) col.flags |= ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortAscending;
            else if (s && strcmp(s, "desc") == 0) col.flags |= ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending;
            else if (!lua_isnil(L, -1)) return luaL_error(L, "w3.ui.table: a column's sort is \"asc\", \"desc\" or false");
            if (s) sortable = true;  // a column that names its start order makes the table sortable
            lua_pop(L, 1);
        } else if (lua_isstring(L, c)) {
            col.label = lua_tostring(L, c);
        }
        lua_pop(L, 1);
    }
    if (sortable) f |= ImGuiTableFlags_Sortable;
    ImVec2 outer(0, 0);
    if (scrollY) outer.y = height > 0 ? height : -FLT_MIN;
    bool open = ImGui::BeginTable(id, cols, f, outer);
    if (open) {
        if (scrollY || freeze > 0) ImGui::TableSetupScrollFreeze(freeze, scrollY ? 1 : 0);
        for (const Col& col : spec) ImGui::TableSetupColumn(col.label.c_str(), col.flags, col.w);
        if (headers) ImGui::TableHeadersRow();
    }
    if (open) ++g_uiTables;
    return UiBody(L, fn, open, [open] { if (open) --g_uiTables, ImGui::EndTable(); });
}
void UiInTable(lua_State* L) {
    UiCheck(L);
    if (g_uiTables <= 0) luaL_error(L, "w3.ui.row and w3.ui.col work inside a w3.ui.table function");
}
int LUiRow(lua_State* L) { UiInTable(L); ImGui::TableNextRow(); return 0; }       // w3.ui.row(): the next table row
int LUiCol(lua_State* L) { UiInTable(L); ImGui::TableNextColumn(); return 0; }    // w3.ui.col(): the next cell
// w3.ui.table_sort() -> column, ascending, changed: how the table is sorted (a click on a header changes it), nil
// when it is not. changed is true once after each change and on the first frame. table_sort(true) -> {{column,
// ascending}, ...}, changed: every column of a multi_sort table, the first one first.
int LUiTableSort(lua_State* L) {
    UiInTable(L);
    ImGuiTableSortSpecs* s = ImGui::TableGetSortSpecs();
    if (!s || s->SpecsCount < 1) {
        lua_pushnil(L);
        return 1;
    }
    bool changed = s->SpecsDirty;
    s->SpecsDirty = false;
    if (lua_toboolean(L, 1)) {
        lua_createtable(L, s->SpecsCount, 0);
        for (int i = 0; i < s->SpecsCount; ++i) {
            lua_createtable(L, 2, 0);
            lua_pushinteger(L, s->Specs[i].ColumnIndex + 1);
            lua_rawseti(L, -2, 1);
            lua_pushboolean(L, s->Specs[i].SortDirection != ImGuiSortDirection_Descending);
            lua_rawseti(L, -2, 2);
            lua_rawseti(L, -2, i + 1);
        }
        lua_pushboolean(L, changed);
        return 2;
    }
    lua_pushinteger(L, s->Specs[0].ColumnIndex + 1);
    lua_pushboolean(L, s->Specs[0].SortDirection != ImGuiSortDirection_Descending);
    lua_pushboolean(L, changed);
    return 3;
}
// w3.ui.rows(count, fn): fn(i) for the rows 1 to count that can be seen, the others are skipped (a list of thousands
// costs what the visible part costs). In a table each one is a row of its own; each one has its own ids. The rows
// need one height, the first one's.
int LUiRows(lua_State* L) {
    UiCheck(L);
    lua_Integer n = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (n <= 0) return 0;
    if (n > 0x7fffffff) return luaL_argerror(L, 1, "too many rows");
    ImGuiTable* table = g_uiTables > 0 ? ImGui::GetCurrentTable() : nullptr;
    if (table && table->InnerWindow != ImGui::GetCurrentWindow()) table = nullptr;
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(n));
    int err = LUA_OK;
    while (err == LUA_OK && clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd && err == LUA_OK; ++i) {
            if (table) ImGui::TableNextRow();
            ImGui::PushID(i);
            lua_pushvalue(L, 2);
            lua_pushinteger(L, i + 1);
            err = lua_pcall(L, 1, 0, 0);
            ImGui::PopID();
        }
    }
    clip.End();
    if (err != LUA_OK) return lua_error(L);
    return 0;
}
int LUiText(lua_State* L) {  // w3.ui.text(s [, 0xRRGGBBAA])
    UiCheck(L);
    const char* s = luaL_tolstring(L, 1, nullptr);
    if (lua_isinteger(L, 2)) {
        uint32_t c = static_cast<uint32_t>(lua_tointeger(L, 2));
        ImGui::TextColored(ImVec4(((c >> 24) & 255) / 255.0f, ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f,
                                  (c & 255) / 255.0f), "%s", s);
    } else {
        ImGui::TextUnformatted(s);
    }
    return 0;
}
int LUiButton(lua_State* L) {  // w3.ui.button(label) -> true when clicked
    UiCheck(L);
    lua_pushboolean(L, ImGui::Button(luaL_checkstring(L, 1)));
    return 1;
}
int LUiCheckbox(lua_State* L) {  // w3.ui.checkbox(label, value) -> changed, value
    UiCheck(L);
    bool v = lua_toboolean(L, 2) != 0;
    bool changed = ImGui::Checkbox(luaL_checkstring(L, 1), &v);
    lua_pushboolean(L, changed);
    lua_pushboolean(L, v);
    return 2;
}
int LUiInputText(lua_State* L) {  // w3.ui.input_text(label, text [, on_enter [, hint]]) -> changed, text
    UiCheck(L);
    size_t len = 0;
    const char* s = luaL_optlstring(L, 2, "", &len);
    std::vector<char> buf(len + 512, '\0');
    memcpy(buf.data(), s, len);
    ImGuiInputTextFlags f = lua_toboolean(L, 3) ? ImGuiInputTextFlags_EnterReturnsTrue : 0;
    const char* hint = lua_isstring(L, 4) ? lua_tostring(L, 4) : nullptr;  // grey text while the field is empty
    bool changed = hint ? ImGui::InputTextWithHint(luaL_checkstring(L, 1), hint, buf.data(), buf.size(), f)
                        : ImGui::InputText(luaL_checkstring(L, 1), buf.data(), buf.size(), f);
    lua_pushboolean(L, changed);
    lua_pushstring(L, buf.data());
    return 2;
}
// w3.ui.input_multiline(id, text [, width, height]) -> changed, text: a text field of many lines (Tab goes in as a
// tab); the buffer grows with the text (ImGui's resize callback)
int MultilineResize(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = &(*s)[0];
    }
    return 0;
}
int LUiInputMultiline(lua_State* L) {
    UiCheck(L);
    const char* id = luaL_checkstring(L, 1);
    size_t len = 0;
    const char* s = luaL_optlstring(L, 2, "", &len);
    float w = lua_isnoneornil(L, 3) ? -1.0f : static_cast<float>(UiNum(L, 3));
    float h = lua_isnoneornil(L, 4) ? -1.0f : static_cast<float>(UiNum(L, 4));
    std::string buf(s, len);
    buf.reserve(len + 1);
    bool changed = ImGui::InputTextMultiline(id, &buf[0], buf.capacity() + 1, ImVec2(w, h),
                                             ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_AllowTabInput,
                                             &MultilineResize, &buf);
    lua_pushboolean(L, changed);
    lua_pushlstring(L, buf.c_str(), strlen(buf.c_str()));
    return 2;
}
int LUiInputNumber(lua_State* L) {  // w3.ui.input_number(label, n) -> changed (on Enter), n
    UiCheck(L);
    double v = lua_isnoneornil(L, 2) ? 0.0 : UiNum(L, 2);
    // "changed" when the field is left or Enter pressed after an edit (fuzz_ui.py: EnterReturnsTrue is refused by
    // ImGui's number fields, every call asserted)
    ImGui::InputDouble(luaL_checkstring(L, 1), &v, 0, 0, "%.6g");
    bool changed = ImGui::IsItemDeactivatedAfterEdit();
    lua_pushboolean(L, changed);
    lua_pushnumber(L, v);
    return 2;
}
int LUiSlider(lua_State* L) {  // w3.ui.slider(label, value, min, max) -> changed, value
    UiCheck(L);
    float v = static_cast<float>(UiNum(L, 2));
    float lo = static_cast<float>(UiNum(L, 3)), hi = static_cast<float>(UiNum(L, 4));
    bool changed = ImGui::SliderFloat(luaL_checkstring(L, 1), &v, lo, hi);
    lua_pushboolean(L, changed);
    lua_pushnumber(L, v);
    return 2;
}
int LUiCombo(lua_State* L) {  // w3.ui.combo(label, current, {items}) -> changed, value
    UiCheck(L);
    const char* label = luaL_checkstring(L, 1);
    const char* cur = luaL_optstring(L, 2, "");
    luaL_checktype(L, 3, LUA_TTABLE);
    std::string picked = cur;
    bool changed = false;
    if (ImGui::BeginCombo(label, cur)) {
        for (int i = 1, n = static_cast<int>(lua_rawlen(L, 3)); i <= n; ++i) {
            lua_rawgeti(L, 3, i);
            const char* item = lua_tostring(L, -1);
            if (item && ImGui::Selectable(item, picked == item)) {
                picked = item;
                changed = true;
            }
            lua_pop(L, 1);
        }
        ImGui::EndCombo();
    }
    lua_pushboolean(L, changed);
    lua_pushstring(L, picked.c_str());
    return 2;
}
// w3.ui.selectable(label, selected [, opts]) -> clicked, double-clicked. opts: span = true (across every column of
// its table row), overlap = true (widgets drawn over it still take their clicks), fit = true (as wide as its text,
// for several on one line; without it one reaches to the right edge and covers the next), width, height,
// disabled = true. Without a width it reaches to the right edge.
int LUiSelectable(lua_State* L) {
    UiCheck(L);
    const char* label = luaL_checkstring(L, 1);
    bool sel = lua_toboolean(L, 2) != 0;
    if (!lua_isnoneornil(L, 3) && !lua_istable(L, 3)) return luaL_argerror(L, 3, "a table of options");
    ImGuiSelectableFlags f = ImGuiSelectableFlags_AllowDoubleClick;
    if (UiOptBool(L, 3, "span", false)) f |= ImGuiSelectableFlags_SpanAllColumns;
    if (UiOptBool(L, 3, "overlap", false)) f |= ImGuiSelectableFlags_AllowOverlap;
    if (UiOptBool(L, 3, "disabled", false)) f |= ImGuiSelectableFlags_Disabled;
    ImVec2 size(static_cast<float>(UiOptNum(L, 3, "width", 0)), static_cast<float>(UiOptNum(L, 3, "height", 0)));
    if (size.x == 0 && UiOptBool(L, 3, "fit", false)) size.x = ImGui::CalcTextSize(label, nullptr, true).x;
    // a row of a table is as high as a button: the selection covers it, its text on the buttons' line
    bool tall = (f & ImGuiSelectableFlags_SpanAllColumns) && size.y == 0;
    if (tall) {
        size.y = ImGui::GetFrameHeight();
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(ImGui::GetStyle().SelectableTextAlign.x, 0.5f));
    }
    bool clicked = ImGui::Selectable(label, sel, f, size);
    if (tall) ImGui::PopStyleVar();
    lua_pushboolean(L, clicked);
    lua_pushboolean(L, clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
    return 2;
}

// w3.ui.select(set, key [, keys]): what a click on the row `key` does to the selection `set` (a table key -> true),
// as in a file list: alone it selects only that row, Ctrl adds or takes it away, Shift selects from the last row
// clicked to this one along `keys` (the keys in the order shown), Ctrl+Shift adds that range.
int LUiSelect(lua_State* L) {
    UiCheck(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checkany(L, 2);
    if (lua_isnil(L, 2)) return luaL_argerror(L, 2, "a key is needed");
    bool hasKeys = lua_istable(L, 3);
    const ImGuiIO& io = ImGui::GetIO();
    // the last row clicked per selection, in a table with weak keys in the registry
    if (lua_getfield(L, LUA_REGISTRYINDEX, "tw3se.ui.anchor") != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_newtable(L);
        lua_pushstring(L, "k");
        lua_setfield(L, -2, "__mode");
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, "tw3se.ui.anchor");
    }
    int anchors = lua_gettop(L);
    auto clear = [&] {
        lua_newtable(L);  // the keys first: a table must not change while lua_next walks it
        int keys = lua_gettop(L), n = 0;
        lua_pushnil(L);
        while (lua_next(L, 1)) {
            lua_pop(L, 1);
            lua_pushvalue(L, -1);
            lua_rawseti(L, keys, ++n);
        }
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, keys, i);
            lua_pushnil(L);
            lua_settable(L, 1);
        }
        lua_pop(L, 1);
    };
    auto setAnchor = [&] {
        lua_pushvalue(L, 1);
        lua_pushvalue(L, 2);
        lua_rawset(L, anchors);
    };
    if (io.KeyShift && hasKeys) {
        lua_pushvalue(L, 1);
        lua_rawget(L, anchors);
        int anchor = lua_gettop(L), from = 0, to = 0;
        lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, 3));
        for (lua_Integer i = 1; i <= n && (!from || !to); ++i) {
            lua_rawgeti(L, 3, i);
            if (!from && !lua_isnil(L, anchor) && lua_rawequal(L, -1, anchor)) from = static_cast<int>(i);
            if (!to && lua_rawequal(L, -1, 2)) to = static_cast<int>(i);
            lua_pop(L, 1);
        }
        if (from && to) {
            if (!io.KeyCtrl) clear();
            if (from > to) std::swap(from, to);
            for (int i = from; i <= to; ++i) {
                lua_rawgeti(L, 3, i);
                lua_pushboolean(L, 1);
                lua_settable(L, 1);
            }
            return 0;
        }
    }
    if (io.KeyCtrl) {
        lua_pushvalue(L, 2);
        lua_gettable(L, 1);
        bool on = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_pushvalue(L, 2);
        if (on) lua_pushnil(L);
        else lua_pushboolean(L, 1);
        lua_settable(L, 1);
    } else {
        clear();
        lua_pushvalue(L, 2);
        lua_pushboolean(L, 1);
        lua_settable(L, 1);
    }
    setAnchor();
    return 0;
}

// --- drag and drop: the value dragged stays in the registry until the next drag, ImGui's payload holds its ref ---
int g_dragRef = LUA_NOREF;
const void* g_dragOwner = nullptr;  // the Lua state of the ref (a reloaded Lua does not read an old one)
const void* UiOwner(lua_State* L) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    const void* p = lua_topointer(L, -1);
    lua_pop(L, 1);
    return p;
}
const char* UiDragKind(lua_State* L, int i) {
    size_t len = 0;
    const char* kind = luaL_checklstring(L, i, &len);
    if (len < 1 || len > 32) luaL_argerror(L, i, "a kind of 1 to 32 characters");
    return kind;
}
// pushes the value of a payload of ours, else nothing (false)
bool UiPushPayload(lua_State* L, const ImGuiPayload* p) {
    if (!p || p->DataSize != sizeof(int) || g_dragOwner != UiOwner(L)) return false;
    int ref = *static_cast<const int*>(p->Data);
    if (ref != g_dragRef || ref == LUA_NOREF) return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    return true;
}
// w3.ui.drag(kind, value [, preview]) after a widget: the widget can be dragged, a drop(kind) gets the value.
// preview is the text at the mouse while it is dragged, or a function that draws it (default: the value when it is
// text or a number). -> true while it is dragged.
int LUiDrag(lua_State* L) {
    UiCheck(L);
    const char* kind = UiDragKind(L, 1);
    luaL_checkany(L, 2);
    bool text = lua_type(L, 3) == LUA_TSTRING || (lua_isnoneornil(L, 3) && lua_isstring(L, 2));
    ImGuiDragDropFlags f = ImGuiDragDropFlags_SourceAllowNullID;
    if (!text && !lua_isfunction(L, 3)) f |= ImGuiDragDropFlags_SourceNoPreviewTooltip;
    if (!ImGui::BeginDragDropSource(f)) {
        lua_pushboolean(L, false);
        return 1;
    }
    const void* owner = UiOwner(L);
    if (g_dragRef != LUA_NOREF && g_dragOwner == owner) luaL_unref(L, LUA_REGISTRYINDEX, g_dragRef);
    lua_pushvalue(L, 2);
    g_dragRef = luaL_ref(L, LUA_REGISTRYINDEX);
    g_dragOwner = owner;
    ImGui::SetDragDropPayload(kind, &g_dragRef, sizeof g_dragRef);
    int err = LUA_OK;
    if (lua_isfunction(L, 3)) {
        lua_pushvalue(L, 3);
        err = lua_pcall(L, 0, 0, 0);
    } else if (text) {
        ImGui::TextUnformatted(lua_tostring(L, lua_isnoneornil(L, 3) ? 2 : 3));
    }
    ImGui::EndDragDropSource();
    if (err != LUA_OK) return lua_error(L);
    lua_pushboolean(L, true);
    return 1;
}
// w3.ui.drop(kind) after a widget -> dropped, over: the value let go on it this frame (else nil), and the value
// dragged over it now (to mark where it would land).
int LUiDrop(lua_State* L) {
    UiCheck(L);
    const char* kind = UiDragKind(L, 1);
    if (!ImGui::BeginDragDropTarget()) {
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }
    const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kind, ImGuiDragDropFlags_AcceptBeforeDelivery);
    bool delivered = p && p->IsDelivery();
    bool ours = UiPushPayload(L, p);  // before the end: a delivered payload is cleared there
    ImGui::EndDragDropTarget();
    if (!ours) {
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }
    if (delivered) lua_pushvalue(L, -1);
    else lua_pushnil(L), lua_insert(L, -2);
    return 2;
}
// w3.ui.dragging([kind]) -> the value dragged now (of that kind), nil when nothing is
int LUiDragging(lua_State* L) {
    UiCheck(L);
    const ImGuiPayload* p = ImGui::GetDragDropPayload();
    if (p && !lua_isnoneornil(L, 1) && !p->IsDataType(UiDragKind(L, 1))) p = nullptr;
    if (!UiPushPayload(L, p)) lua_pushnil(L);
    return 1;
}

// w3.ui.context(fn [, id]): a menu that opens on a right-click on the widget before it; fn draws it (menu_item,
// menu, selectable, ...). -> true while it is open.
int LUiContext(lua_State* L) {
    UiCheck(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    const char* id = luaL_optstring(L, 2, nullptr);
    bool open = ImGui::BeginPopupContextItem(id);
    return UiBody(L, 1, open, [open] { if (open) ImGui::EndPopup(); });
}
// w3.ui.menu_item(label [, shortcut [, checked [, enabled]]]) -> true when clicked: a line of a menu. shortcut is
// only shown; with checked it has a tick when true.
int LUiMenuItem(lua_State* L) {
    UiCheck(L);
    const char* label = luaL_checkstring(L, 1);
    const char* keys = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
    bool enabled = lua_isnoneornil(L, 4) || lua_toboolean(L, 4);
    bool clicked = ImGui::MenuItem(label, keys, lua_toboolean(L, 3) != 0, enabled);
    lua_pushboolean(L, clicked);
    return 1;
}
// w3.ui.menu(label, fn [, enabled]): a menu inside a menu, fn draws it while it is open
int LUiMenu(lua_State* L) {
    UiCheck(L);
    bool open = ImGui::BeginMenu(luaL_checkstring(L, 1), lua_isnoneornil(L, 3) || lua_toboolean(L, 3));
    return UiBody(L, 2, open, [open] { if (open) ImGui::EndMenu(); });
}

// w3.ui.icon(name) -> the icon as text for a label ("Play", "Lock", ...); w3.ui.icon() -> the list of names. The
// icons come from Windows' own icon font.
int LUiIcon(lua_State* L) {
    int n = 0;
    const UiIconName* icons = UiIcons(&n);
    if (lua_isnoneornil(L, 1)) {
        lua_createtable(L, n, 0);
        for (int i = 0; i < n; ++i) {
            lua_pushstring(L, icons[i].name);
            lua_rawseti(L, -2, i + 1);
        }
        return 1;
    }
    const char* name = luaL_checkstring(L, 1);
    for (int i = 0; i < n; ++i) {
        if (_stricmp(icons[i].name, name) != 0) continue;
        unsigned c = icons[i].code;
        char s[4] = {static_cast<char>(0xE0 | (c >> 12)), static_cast<char>(0x80 | ((c >> 6) & 63)),
                     static_cast<char>(0x80 | (c & 63)), 0};
        lua_pushstring(L, s);
        return 1;
    }
    return luaL_argerror(L, 1, "no icon of that name (w3.ui.icon() lists them)");
}
int LUiSameLine(lua_State* L) { UiCheck(L); ImGui::SameLine(); return 0; }
// w3.ui.align(): the text after it on this line sits as a button's label does (text next to buttons, a table cell
// beside a span selectable)
int LUiAlign(lua_State* L) { UiCheck(L); ImGui::AlignTextToFramePadding(); return 0; }
// w3.ui.over_window() -> true while the mouse is over any window or a widget is held (a click there is the UI's)
int LUiOverWindow(lua_State* L) {
    UiCheck(L);
    lua_pushboolean(L, ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                                              ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
                           ImGui::IsAnyItemActive());
    return 1;
}
int LUiSeparator(lua_State* L) { UiCheck(L); ImGui::Separator(); return 0; }
int LUiWidth(lua_State* L) {  // w3.ui.width(w): the next widget's width (-1 = to the edge)
    UiCheck(L);
    ImGui::SetNextItemWidth(static_cast<float>(UiNum(L, 1)));
    return 0;
}
int LUiIsOpen(lua_State* L) {  // w3.ui.is_open(): the in-game UI is open
    lua_pushboolean(L, UiIsOpen());
    return 1;
}
int LUiTabs(lua_State* L) {  // w3.ui.tabs(id, fn): a tab bar; fn holds its w3.ui.tab calls
    UiCheck(L);
    bool open = ImGui::BeginTabBar(luaL_checkstring(L, 1), ImGuiTabBarFlags_FittingPolicyScroll |
                                                              ImGuiTabBarFlags_TabListPopupButton);
    return UiBody(L, 2, open, [open] { if (open) ImGui::EndTabBar(); });
}
int LUiTab(lua_State* L) {  // w3.ui.tab(label, fn): one tab; fn draws it while it is the chosen one
    UiCheck(L);
    bool open = ImGui::BeginTabItem(luaL_checkstring(L, 1));
    return UiBody(L, 2, open, [open] { if (open) ImGui::EndTabItem(); });
}
// w3.ui.tooltip(text): shown while the mouse is over the widget before it. tooltip(fn): fn draws it (text, images).
int LUiTooltip(lua_State* L) {
    UiCheck(L);
    if (!lua_isfunction(L, 1)) {
        ImGui::SetItemTooltip("%s", luaL_checkstring(L, 1));
        return 0;
    }
    bool open = ImGui::BeginItemTooltip();
    UiBody(L, 1, open, [open] { if (open) ImGui::EndTooltip(); });
    return 0;
}
int LUiWrap(lua_State* L) {  // w3.ui.wrap(text): text that breaks at the window's edge
    UiCheck(L);
    ImGui::TextWrapped("%s", luaL_tolstring(L, 1, nullptr));
    return 0;
}
int LUiDim(lua_State* L) {  // w3.ui.dim(text): grey text
    UiCheck(L);
    ImGui::TextDisabled("%s", luaL_tolstring(L, 1, nullptr));
    return 0;
}
int LUiSliderInt(lua_State* L) {  // w3.ui.slider_int(label, value, min, max) -> changed, value
    UiCheck(L);
    int v = static_cast<int>(UiNum(L, 2));
    int lo = static_cast<int>(UiNum(L, 3)), hi = static_cast<int>(UiNum(L, 4));
    bool changed = ImGui::SliderInt(luaL_checkstring(L, 1), &v, lo, hi);
    lua_pushboolean(L, changed);
    lua_pushinteger(L, v);
    return 2;
}
int LUiSpacing(lua_State* L) { UiCheck(L); ImGui::Spacing(); return 0; }  // w3.ui.spacing(): a little room
// w3.ui.key(): the key pressed this frame as w3.on_key names it ("F6", "Ctrl+Shift+K", "Num5"), nil when none. For a
// settings field that waits for a key. Read from Windows, so it works while the UI has the keyboard.
int LUiKey(lua_State* L) {
    UiCheck(L);
    static bool was[256] = {};
    static const std::pair<int, const char*> named[] = {
        {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_ESCAPE, "Escape"}, {VK_TAB, "Tab"}, {VK_BACK, "Backspace"},
        {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"}, {VK_INSERT, "Insert"},
        {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"}, {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"},
        {VK_CAPITAL, "CapsLock"}, {VK_PAUSE, "Pause"}, {VK_ADD, "NumPlus"}, {VK_SUBTRACT, "NumMinus"},
        {VK_MULTIPLY, "NumMultiply"}, {VK_DIVIDE, "NumDivide"}, {VK_DECIMAL, "NumDecimal"}, {VK_OEM_MINUS, "Minus"},
        {VK_OEM_PLUS, "Plus"}, {VK_OEM_COMMA, "Comma"}, {VK_OEM_PERIOD, "Period"}, {VK_MBUTTON, "Mouse3"},
        {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"}};
    std::string hit;
    auto edge = [&](int vk) {
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        bool pressed = down && !was[vk];
        was[vk] = down;
        return pressed;
    };
    for (int vk = 'A'; vk <= 'Z'; ++vk) if (edge(vk) && hit.empty()) hit = std::string(1, static_cast<char>(vk));
    for (int vk = '0'; vk <= '9'; ++vk) if (edge(vk) && hit.empty()) hit = std::string(1, static_cast<char>(vk));
    for (int f = 0; f < 24; ++f) if (edge(VK_F1 + f) && hit.empty()) hit = "F" + std::to_string(f + 1);
    for (int k = 0; k < 10; ++k) if (edge(VK_NUMPAD0 + k) && hit.empty()) hit = "Num" + std::to_string(k);
    for (const auto& [vk, name] : named) if (edge(vk) && hit.empty()) hit = name;
    // a controller button; LB, RB, LT and RT count as modifiers ("Pad_LB+Pad_A"). Asked again after a pause, it only
    // takes the buttons held (the A that clicked the key field is no key).
    static uint32_t padWas = 0;
    static ULONGLONG padAt = 0;
    uint32_t pad = g_uiPadMask && g_uiPadName ? g_uiPadMask() : 0;
    ULONGLONG now = GetTickCount64();
    uint32_t pressed = now - padAt < 200 ? pad & ~padWas : 0;
    padWas = pad;
    padAt = now;
    constexpr uint32_t kShoulders = (1u << 8) | (1u << 9) | (1u << 16) | (1u << 17);
    if (hit.empty() && (pressed & ~kShoulders)) {
        for (int b = 0; b < 18 && hit.empty(); ++b)
            if ((pressed & ~kShoulders) >> b & 1) {
                for (int s : {8, 9, 16, 17})
                    if (pad >> s & 1) hit += std::string(g_uiPadName(s)) + "+";
                hit += g_uiPadName(b);
            }
        lua_pushstring(L, hit.c_str());
        return 1;
    }
    if (hit.empty()) {
        lua_pushnil(L);
        return 1;
    }
    std::string mods;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods += "Ctrl+";
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods += "Shift+";
    if (GetAsyncKeyState(VK_MENU) & 0x8000) mods += "Alt+";
    lua_pushstring(L, (hit == "Escape" ? hit : mods + hit).c_str());
    return 1;
}

}  // namespace

// checked by eye in both fonts (_scratch/icon_sheet.py)
const UiIconName* UiIcons(int* count) {
    static const UiIconName icons[] = {
        {"Play", 0xE768}, {"Pause", 0xE769}, {"Stop", 0xE71A}, {"Next", 0xE893}, {"Previous", 0xE892},
        {"Shuffle", 0xE8B1}, {"Repeat", 0xE8EE}, {"Volume", 0xE767}, {"Mute", 0xE74F}, {"Up", 0xE74A},
        {"Down", 0xE74B}, {"Left", 0xE72B}, {"Right", 0xE72A}, {"ChevronUp", 0xE70E}, {"ChevronDown", 0xE70D},
        {"ChevronLeft", 0xE76B}, {"ChevronRight", 0xE76C}, {"Lock", 0xE72E}, {"Unlock", 0xE785}, {"Add", 0xE710},
        {"Remove", 0xE738}, {"Delete", 0xE74D}, {"Close", 0xE711}, {"Check", 0xE73E}, {"Edit", 0xE70F},
        {"Save", 0xE74E}, {"Folder", 0xE8B7}, {"File", 0xE8A5}, {"Copy", 0xE8C8}, {"Paste", 0xE77F},
        {"Undo", 0xE7A7}, {"Redo", 0xE7A6}, {"Refresh", 0xE72C}, {"Search", 0xE721}, {"Filter", 0xE71C},
        {"Sort", 0xE8CB}, {"Settings", 0xE713}, {"More", 0xE712}, {"Menu", 0xE700}, {"List", 0xEA37},
        {"Star", 0xE734}, {"StarFill", 0xE735}, {"Heart", 0xEB51}, {"HeartFill", 0xEB52}, {"Eye", 0xE7B3},
        {"Hide", 0xED1A}, {"Info", 0xE946}, {"Warning", 0xE7BA}, {"Error", 0xE783}, {"Help", 0xE897},
        {"Person", 0xE77B}, {"People", 0xE716}, {"Camera", 0xE722}, {"Video", 0xE714}, {"Picture", 0xEB9F},
        {"Clock", 0xE823}, {"Calendar", 0xE787}, {"Home", 0xE80F}, {"Pin", 0xE718}, {"Unpin", 0xE77A},
        {"Link", 0xE71B}, {"Map", 0xE826}, {"Location", 0xE81D}, {"Globe", 0xE774}, {"Move", 0xE7C2},
        {"Rotate", 0xE7AD}, {"ZoomIn", 0xE8A3}, {"ZoomOut", 0xE71F}, {"FullScreen", 0xE740}, {"Download", 0xE896},
        {"Upload", 0xE898}, {"Sync", 0xE895}, {"Flag", 0xE7C1}, {"Tag", 0xE8EC}, {"Bulb", 0xEA80},
        {"Gripper", 0xE76F}, {"Mail", 0xE715}, {"Light", 0xE793}, {"Code", 0xE943}, {"Bug", 0xEBE8}};
    *count = static_cast<int>(sizeof icons / sizeof icons[0]);
    return icons;
}

// The w3.ui table on top of the stack's w3 table (-1): lua.cpp calls it when it makes w3.
void LuaUiOpen(lua_State* L) {
    static const luaL_Reg ui[] = {{"window", &LUiWindow}, {"tree", &LUiTree}, {"collapsing", &LUiCollapsing},
                           {"child", &LUiChild}, {"id", &LUiId}, {"table", &LUiTable}, {"row", &LUiRow},
                           {"col", &LUiCol}, {"text", &LUiText}, {"button", &LUiButton}, {"checkbox", &LUiCheckbox},
                           {"input_text", &LUiInputText}, {"input_multiline", &LUiInputMultiline}, {"input_number", &LUiInputNumber}, {"slider", &LUiSlider},
                           {"combo", &LUiCombo}, {"selectable", &LUiSelectable}, {"same_line", &LUiSameLine},
                           {"separator", &LUiSeparator}, {"width", &LUiWidth}, {"is_open", &LUiIsOpen},
                           {"tabs", &LUiTabs}, {"tab", &LUiTab}, {"tooltip", &LUiTooltip}, {"wrap", &LUiWrap},
                           {"dim", &LUiDim}, {"slider_int", &LUiSliderInt}, {"spacing", &LUiSpacing},
                           {"key", &LUiKey}, {"table_sort", &LUiTableSort}, {"rows", &LUiRows},
                           {"select", &LUiSelect}, {"drag", &LUiDrag}, {"drop", &LUiDrop},
                           {"dragging", &LUiDragging}, {"context", &LUiContext}, {"menu_item", &LUiMenuItem},
                           {"menu", &LUiMenu}, {"icon", &LUiIcon}, {"align", &LUiAlign},
                           {"over_window", &LUiOverWindow}, {nullptr, nullptr}};
    luaL_newlib(L, ui);
    LuaUiOpenMore(L);
    lua_setfield(L, -2, "ui");
}

}  // namespace tw3se
