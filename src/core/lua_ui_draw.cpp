// TW3SE core - more of w3.ui: drawing in a window, the mouse, the last item,
// window options, style, fonts at exact pixel sizes, pictures, popups, the clipboard. Shared with the UI lab
// like lua_ui.cpp. Colours are 0xRRGGBBAA as in w3.draw; positions are screen pixels (w3.ui.pos() gives where the
// next widget goes, w3.ui.window_rect() the window).
#include <windows.h>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
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

void Check(lua_State* L) {
    if (!g_uiFrame) luaL_error(L, "w3.ui works inside a w3.on_ui function");
}
float Num(lua_State* L, int i) {
    double v = luaL_checknumber(L, i);
    if (!std::isfinite(v) || std::fabs(v) > 1e9) luaL_argerror(L, i, "a finite number is needed");
    return static_cast<float>(v);
}
float OptNum(lua_State* L, int i, float d) { return lua_isnoneornil(L, i) ? d : Num(L, i); }
ImU32 Col(lua_State* L, int i, uint32_t d = 0xFFFFFFFF) {
    uint32_t c = lua_isnoneornil(L, i) ? d : static_cast<uint32_t>(luaL_checkinteger(L, i));
    return IM_COL32((c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, c & 255);
}
ImVec4 Col4(uint32_t c) {
    return ImVec4(((c >> 24) & 255) / 255.0f, ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f);
}
// Calls the function at idx, then close (also after an error, which is raised again).
template <class Close> int Body(lua_State* L, int idx, Close close) {
    int err = LUA_OK;
    if (lua_isfunction(L, idx)) {
        lua_pushvalue(L, idx);
        err = lua_pcall(L, 0, 0, 0);
    }
    close();
    if (err != LUA_OK) return lua_error(L);
    return 0;
}
int Button(lua_State* L, int i) {  // 1 left, 2 right, 3 middle -> ImGuiMouseButton
    int b = static_cast<int>(luaL_optinteger(L, i, 1));
    if (b < 1 || b > 3) luaL_argerror(L, i, "1 (left), 2 (right) or 3 (middle)");
    return b - 1;
}

// --- fonts: Segoe UI at exact pixel sizes, as Qt sizes it (font-size: 13px = an em of 13 px) ---
struct Font {
    int px;
    bool bold;
    ImFont* font;
};
std::vector<Font> g_fonts;

// The em of a TrueType font against ImGui's size (ImGui sizes by ascent - descent, Qt by the em).
float EmToImGui(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0.0f;
    std::vector<unsigned char> d;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && d.size() < (1u << 20)) d.insert(d.end(), buf, buf + n);
    fclose(f);
    auto u16 = [&](size_t o) { return o + 2 <= d.size() ? (d[o] << 8) | d[o + 1] : 0; };
    auto s16 = [&](size_t o) { return static_cast<int16_t>(u16(o)); };
    size_t head = 0, hhea = 0;
    for (int t = 0, tables = u16(4); t < tables; ++t) {
        size_t rec = 12 + 16 * t;
        if (rec + 16 > d.size()) break;
        size_t off = (size_t(d[rec + 8]) << 24) | (d[rec + 9] << 16) | (d[rec + 10] << 8) | d[rec + 11];
        if (!memcmp(&d[rec], "head", 4)) head = off;
        if (!memcmp(&d[rec], "hhea", 4)) hhea = off;
    }
    int upem = head ? u16(head + 18) : 0;
    if (!upem || !hhea) return 0.0f;
    return static_cast<float>(s16(hhea + 4) - s16(hhea + 6)) / upem;
}

// a text's style argument: true or "bold" Segoe UI bold (1), "mono" Consolas (2), else Segoe UI (0)
int StyleArg(lua_State* L, int i) {
    if (lua_type(L, i) == LUA_TSTRING) {
        const char* t = lua_tostring(L, i);
        return !strcmp(t, "bold") ? 1 : !strcmp(t, "mono") ? 2 : 0;
    }
    return lua_toboolean(L, i) ? 1 : 0;
}

ImFont* FindFont(int px, bool bold) {
    ImFont* best = nullptr;
    int bestd = 1 << 30;
    for (const Font& f : g_fonts) {
        int dd = std::abs(f.px - px) * 2 + (f.bold != bold ? 1 : 0);
        if (dd < bestd) bestd = dd, best = f.font;
    }
    return best;
}
ImFont* OptFont(lua_State* L, int sizeIdx, int boldIdx) {
    if (lua_isnoneornil(L, sizeIdx)) return nullptr;
    bool bold = lua_isstring(L, boldIdx) ? strcmp(lua_tostring(L, boldIdx), "bold") == 0 : lua_toboolean(L, boldIdx) != 0;
    return FindFont(static_cast<int>(Num(L, sizeIdx) + 0.5f), bold);
}

// --- w3.ui.draw: shapes into the window's draw list (or the layer chosen with w3.ui.draw.layer) ---
ImDrawList* g_layer = nullptr;  // set by w3.ui.draw.layer while its function runs
ImDrawList* DL() { return g_layer ? g_layer : ImGui::GetWindowDrawList(); }

int DLine(lua_State* L) {  // line(x1, y1, x2, y2 [, colour, width])
    Check(L);
    DL()->AddLine(ImVec2(Num(L, 1), Num(L, 2)), ImVec2(Num(L, 3), Num(L, 4)), Col(L, 5), OptNum(L, 6, 1.0f));
    return 0;
}
int DRect(lua_State* L) {  // rect(x, y, w, h [, colour, width]): the outline, inside the box (as a Qt border)
    Check(L);
    float x = Num(L, 1), y = Num(L, 2), w = Num(L, 3), h = Num(L, 4), t = OptNum(L, 6, 1.0f);
    ImU32 c = Col(L, 5);
    ImDrawList* dl = DL();  // four filled strips: whole pixels, as Qt draws a border (AddRect would blur a 1 px line)
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + t), c);
    dl->AddRectFilled(ImVec2(x, y + h - t), ImVec2(x + w, y + h), c);
    dl->AddRectFilled(ImVec2(x, y + t), ImVec2(x + t, y + h - t), c);
    dl->AddRectFilled(ImVec2(x + w - t, y + t), ImVec2(x + w, y + h - t), c);
    return 0;
}
int DFill(lua_State* L) {  // fill(x, y, w, h [, colour [, rounding]])
    Check(L);
    float x = Num(L, 1), y = Num(L, 2);
    DL()->AddRectFilled(ImVec2(x, y), ImVec2(x + Num(L, 3), y + Num(L, 4)), Col(L, 5), OptNum(L, 6, 0.0f));
    return 0;
}
int DCircle(lua_State* L) {  // circle(x, y, r [, colour, width, segments])
    Check(L);
    DL()->AddCircle(ImVec2(Num(L, 1), Num(L, 2)), Num(L, 3), Col(L, 4), static_cast<int>(OptNum(L, 6, 0)),
                    OptNum(L, 5, 1.0f));
    return 0;
}
int DDisc(lua_State* L) {  // disc(x, y, r [, colour, segments])
    Check(L);
    DL()->AddCircleFilled(ImVec2(Num(L, 1), Num(L, 2)), Num(L, 3), Col(L, 4), static_cast<int>(OptNum(L, 5, 0)));
    return 0;
}
int DEllipse(lua_State* L) {  // ellipse(x, y, rx, ry [, colour, width]): the outline (width 0: filled)
    Check(L);
    float t = OptNum(L, 6, 1.0f);
    ImVec2 c(Num(L, 1), Num(L, 2)), r(Num(L, 3), Num(L, 4));
    if (t <= 0) DL()->AddEllipseFilled(c, r, Col(L, 5));
    else DL()->AddEllipse(c, r, Col(L, 5), 0.0f, 0, t);
    return 0;
}
int DBezier(lua_State* L) {  // bezier(x1, y1, cx1, cy1, cx2, cy2, x2, y2 [, colour, width])
    Check(L);
    DL()->AddBezierCubic(ImVec2(Num(L, 1), Num(L, 2)), ImVec2(Num(L, 3), Num(L, 4)), ImVec2(Num(L, 5), Num(L, 6)),
                         ImVec2(Num(L, 7), Num(L, 8)), Col(L, 9), OptNum(L, 10, 1.0f));
    return 0;
}
std::vector<ImVec2> Points(lua_State* L, int i) {
    luaL_checktype(L, i, LUA_TTABLE);
    std::vector<ImVec2> pts;
    int n = static_cast<int>(lua_rawlen(L, i));
    for (int k = 1; k + 1 <= n; k += 2) {
        lua_rawgeti(L, i, k);
        lua_rawgeti(L, i, k + 1);
        pts.emplace_back(static_cast<float>(lua_tonumber(L, -2)), static_cast<float>(lua_tonumber(L, -1)));
        lua_pop(L, 2);
    }
    return pts;
}
int DPoly(lua_State* L) {  // poly({x1, y1, x2, y2, ...} [, colour, width, closed])
    Check(L);
    auto pts = Points(L, 1);
    if (pts.size() >= 2)
        DL()->AddPolyline(pts.data(), static_cast<int>(pts.size()), Col(L, 2), lua_toboolean(L, 4) ? ImDrawFlags_Closed : 0,
                          OptNum(L, 3, 1.0f));
    return 0;
}
int DFillPoly(lua_State* L) {  // fill_poly({x1, y1, ...} [, colour]): a convex shape
    Check(L);
    auto pts = Points(L, 1);
    if (pts.size() >= 3) DL()->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), Col(L, 2));
    return 0;
}
int DInvert(lua_State* L) {  // invert(x, y, w, h): the pixels under the box inverted (Qt's text cursor)
    Check(L);
    float x = Num(L, 1), y = Num(L, 2);
    ImDrawList* dl = DL();
    ImDrawCallback cb = UiInvertCallback();
    if (!cb) return 0;
    dl->AddCallback(cb, nullptr);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + Num(L, 3), y + Num(L, 4)), IM_COL32_WHITE);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    return 0;
}
int DText(lua_State* L) {  // text(x, y, colour, s [, size, bold, wrap_width, background, "gray"]): y is the line's top
    Check(L);
    lua_settop(L, 9);  // luaL_tolstring pushes: the optional arguments stay where they are
    size_t len = 0;
    const char* s = luaL_tolstring(L, 4, &len);
    float wrap = OptNum(L, 7, 0.0f);
    if (wrap <= 0 && !lua_isnoneornil(L, 5)) {  // Qt's text (lua_ui_text.cpp), when its fonts are there
        int bold = StyleArg(L, 6);
        uint32_t c = lua_isnoneornil(L, 3) ? 0xFFFFFFFFu : static_cast<uint32_t>(luaL_checkinteger(L, 3));
        uint32_t bg = lua_isnoneornil(L, 8) ? 0u : static_cast<uint32_t>(luaL_checkinteger(L, 8));
        bool gray = lua_isstring(L, 9) && strcmp(lua_tostring(L, 9), "gray") == 0;  // a translucent window's text
        if (UiDrawText(DL(), Num(L, 1), Num(L, 2), c, bg, s, len, Num(L, 5), bold, gray)) {
            lua_pop(L, 1);
            return 0;
        }
    }
    ImFont* f = OptFont(L, 5, 6);
    if (!f) f = ImGui::GetFont();
    DL()->AddText(f, f->FontSize, ImVec2(Num(L, 1), Num(L, 2)), Col(L, 3), s, s + len, wrap);
    lua_pop(L, 1);
    return 0;
}
int DTextScaled(lua_State* L) {  // text_scaled(x, y, colour, s, size [, bold]): any size (a zoomed view's text)
    Check(L);
    size_t len = 0;
    const char* s = luaL_checklstring(L, 4, &len);
    uint32_t c = static_cast<uint32_t>(luaL_checkinteger(L, 3));
    lua_pushboolean(L, UiDrawTextScaled(DL(), Num(L, 1), Num(L, 2), c, s, len, Num(L, 5), StyleArg(L, 6)));
    return 1;
}
int DImage(lua_State* L) {  // image(id, x, y, w, h [, u0, v0, u1, v1, tint])
    Check(L);
    ImTextureID id = static_cast<ImTextureID>(luaL_checkinteger(L, 1));
    if (!id) return 0;
    float x = Num(L, 2), y = Num(L, 3);
    DL()->AddImage(id, ImVec2(x, y), ImVec2(x + Num(L, 4), y + Num(L, 5)), ImVec2(OptNum(L, 6, 0), OptNum(L, 7, 0)),
                   ImVec2(OptNum(L, 8, 1), OptNum(L, 9, 1)), Col(L, 10));
    return 0;
}
int DClip(lua_State* L) {  // clip(x, y, w, h, fn): what fn draws stays inside the box
    Check(L);
    float x = Num(L, 1), y = Num(L, 2);
    ImDrawList* dl = DL();
    dl->PushClipRect(ImVec2(x, y), ImVec2(x + Num(L, 3), y + Num(L, 4)), true);
    return Body(L, 5, [dl] { dl->PopClipRect(); });
}
int DLayer(lua_State* L) {  // layer("front" | "back", fn): over (under) every window
    Check(L);
    const char* n = luaL_checkstring(L, 1);
    ImDrawList* was = g_layer;
    g_layer = !strcmp(n, "front") ? ImGui::GetForegroundDrawList()
              : !strcmp(n, "back") ? ImGui::GetBackgroundDrawList()
                                   : ImGui::GetWindowDrawList();
    return Body(L, 2, [was] { g_layer = was; });
}

// --- the mouse, the last widget, positions ---
int UMouse(lua_State* L) {  // mouse() -> x, y, wheel, horizontal wheel
    Check(L);
    ImGuiIO& io = ImGui::GetIO();
    lua_pushnumber(L, io.MousePos.x);
    lua_pushnumber(L, io.MousePos.y);
    lua_pushnumber(L, io.MouseWheel);
    lua_pushnumber(L, io.MouseWheelH);
    return 4;
}
int UMouseDown(lua_State* L) { Check(L); lua_pushboolean(L, ImGui::IsMouseDown(Button(L, 1))); return 1; }
int UMouseClicked(lua_State* L) { Check(L); lua_pushboolean(L, ImGui::IsMouseClicked(Button(L, 1))); return 1; }
int UMouseReleased(lua_State* L) { Check(L); lua_pushboolean(L, ImGui::IsMouseReleased(Button(L, 1))); return 1; }
int UMouseDouble(lua_State* L) { Check(L); lua_pushboolean(L, ImGui::IsMouseDoubleClicked(Button(L, 1))); return 1; }
int UMouseDrag(lua_State* L) {  // mouse_drag([button, threshold]) -> dx, dy since the press (0, 0 below the threshold)
    Check(L);
    ImVec2 d = ImGui::GetMouseDragDelta(Button(L, 1), OptNum(L, 2, -1.0f));
    lua_pushnumber(L, d.x);
    lua_pushnumber(L, d.y);
    return 2;
}
int UHit(lua_State* L) {  // hit(id, w, h) -> clicked button (0: none), hovered, held: a box that takes the mouse
    Check(L);
    float w = Num(L, 2), h = Num(L, 3);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    bool pressed = ImGui::InvisibleButton(luaL_checkstring(L, 1), ImVec2(w, h),
                                          ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                              ImGuiButtonFlags_MouseButtonMiddle);
    int clicked = 0;  // a click counts on release over the box, as with Qt's buttons
    if (pressed) {
        for (int b = 0; b < 3 && !clicked; ++b)
            if (ImGui::IsMouseReleased(b)) clicked = b + 1;
        if (!clicked) clicked = 1;
    }
    lua_pushinteger(L, clicked);
    lua_pushboolean(L, ImGui::IsItemHovered());
    lua_pushboolean(L, ImGui::IsItemActive());
    return 3;
}
int UItem(lua_State* L) {  // item() -> x, y, w, h, hovered, active, clicked: the widget before
    Check(L);
    ImVec2 a = ImGui::GetItemRectMin(), s = ImGui::GetItemRectSize();
    lua_pushnumber(L, a.x);
    lua_pushnumber(L, a.y);
    lua_pushnumber(L, s.x);
    lua_pushnumber(L, s.y);
    lua_pushboolean(L, ImGui::IsItemHovered());
    lua_pushboolean(L, ImGui::IsItemActive());
    lua_pushboolean(L, ImGui::IsItemClicked(0));
    return 7;
}
int UPos(lua_State* L) {  // pos() -> x, y where the next widget goes
    Check(L);
    ImVec2 p = ImGui::GetCursorScreenPos();
    lua_pushnumber(L, p.x);
    lua_pushnumber(L, p.y);
    return 2;
}
int USetPos(lua_State* L) {  // set_pos(x, y)
    Check(L);
    ImGui::SetCursorScreenPos(ImVec2(Num(L, 1), Num(L, 2)));
    return 0;
}
int UDummy(lua_State* L) {  // dummy(w, h): room, nothing drawn
    Check(L);
    ImGui::Dummy(ImVec2(Num(L, 1), Num(L, 2)));
    return 0;
}
int UAvail(lua_State* L) {  // avail() -> w, h left in the window
    Check(L);
    ImVec2 a = ImGui::GetContentRegionAvail();
    lua_pushnumber(L, a.x);
    lua_pushnumber(L, a.y);
    return 2;
}
int UWindowRect(lua_State* L) {  // window_rect() -> x, y, w, h
    Check(L);
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    lua_pushnumber(L, p.x);
    lua_pushnumber(L, p.y);
    lua_pushnumber(L, s.x);
    lua_pushnumber(L, s.y);
    return 4;
}
int UScreen(lua_State* L) {  // screen() -> w, h of the game's picture
    Check(L);
    ImVec2 s = ImGui::GetIO().DisplaySize;
    lua_pushnumber(L, s.x);
    lua_pushnumber(L, s.y);
    return 2;
}
int UWindowHovered(lua_State* L) {  // window_hovered(): the mouse is over this window (or one of its children)
    Check(L);
    lua_pushboolean(L, ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem));
    return 1;
}
int UWants(lua_State* L) {  // wants() -> mouse, keyboard, text: what the UI takes from the game
    Check(L);
    ImGuiIO& io = ImGui::GetIO();
    lua_pushboolean(L, io.WantCaptureMouse);
    lua_pushboolean(L, io.WantCaptureKeyboard);
    lua_pushboolean(L, io.WantTextInput);
    return 3;
}
int UCursor(lua_State* L) {  // cursor("arrow" | "text" | "hand" | "move" | "ew" | "ns" | "nesw" | "nwse" | "no" | "none")
    Check(L);
    static const std::pair<const char*, ImGuiMouseCursor> names[] = {
        {"arrow", ImGuiMouseCursor_Arrow}, {"text", ImGuiMouseCursor_TextInput}, {"hand", ImGuiMouseCursor_Hand},
        {"move", ImGuiMouseCursor_ResizeAll}, {"ew", ImGuiMouseCursor_ResizeEW}, {"ns", ImGuiMouseCursor_ResizeNS},
        {"nesw", ImGuiMouseCursor_ResizeNESW}, {"nwse", ImGuiMouseCursor_ResizeNWSE}, {"no", ImGuiMouseCursor_NotAllowed},
        {"none", ImGuiMouseCursor_None}};
    const char* n = luaL_checkstring(L, 1);
    for (const auto& [k, v] : names)
        if (!strcmp(k, n)) {
            ImGui::SetMouseCursor(v);
            return 0;
        }
    return luaL_argerror(L, 1, "arrow, text, hand, move, ew, ns, nesw, nwse, no or none");
}
int UTime(lua_State* L) {  // time() -> seconds since the UI started, frame number, seconds of the last frame
    Check(L);
    lua_pushnumber(L, ImGui::GetTime());
    lua_pushinteger(L, ImGui::GetFrameCount());
    lua_pushnumber(L, ImGui::GetIO().DeltaTime);
    return 3;
}

// --- keys through ImGui (they reach the UI only while it has the keyboard) ---
ImGuiKey KeyByName(const char* n) {
    static std::map<std::string, ImGuiKey> byName;
    if (byName.empty())
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
            byName[ImGui::GetKeyName(static_cast<ImGuiKey>(k))] = static_cast<ImGuiKey>(k);
    auto it = byName.find(n);
    return it == byName.end() ? ImGuiKey_None : it->second;
}
// --- input from a script (an agent: click, type and press in the UI from outside) ---
// w3.ui.feed{x, y, button, down, wheel, key, chars, ctrl, shift, alt}: one step, given to ImGui at the start of a
// frame - a step a frame, in order. The mouse stays where the last step put it for FEED_HOLD frames (the real cursor
// would take hover and the press away at once).
struct FeedStep {
    bool pos = false;
    float x = 0, y = 0;
    int button = -1;
    bool down = false;
    float wheel = 0;
    std::string key, chars;
    int ctrl = -1, shift = -1, alt = -1;
};
std::deque<FeedStep> g_feed;
float g_feedX = 0, g_feedY = 0;
int g_feedHold = 0;
const int FEED_HOLD = 30;

void FeedApply() {  // the frame's step (UiFeedApply)
    ImGuiIO& io = ImGui::GetIO();
    if (g_feed.empty()) {
        if (g_feedHold > 0) --g_feedHold, io.AddMousePosEvent(g_feedX, g_feedY);
        return;
    }
    FeedStep s = g_feed.front();
    g_feed.pop_front();
    if (s.pos) g_feedX = s.x, g_feedY = s.y;
    if (s.pos || g_feedHold > 0) io.AddMousePosEvent(g_feedX, g_feedY);
    g_feedHold = FEED_HOLD;
    if (s.ctrl >= 0) io.AddKeyEvent(ImGuiMod_Ctrl, s.ctrl != 0);
    if (s.shift >= 0) io.AddKeyEvent(ImGuiMod_Shift, s.shift != 0);
    if (s.alt >= 0) io.AddKeyEvent(ImGuiMod_Alt, s.alt != 0);
    if (s.button >= 0 && s.button < 5) io.AddMouseButtonEvent(s.button, s.down);
    if (s.wheel != 0) io.AddMouseWheelEvent(0, s.wheel);
    if (!s.key.empty()) {
        ImGuiKey k = KeyByName(s.key.c_str());
        if (k != ImGuiKey_None) io.AddKeyEvent(k, s.down);
    }
    if (!s.chars.empty()) io.AddInputCharactersUTF8(s.chars.c_str());
}
int UFeed(lua_State* L) {  // feed{...} -> steps waiting (see FeedStep)
    Check(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    FeedStep s;
    auto num = [&](const char* k, float* out) {
        lua_getfield(L, 1, k);
        bool got = lua_isnumber(L, -1) != 0;
        if (got) *out = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return got;
    };
    auto flag = [&](const char* k) {
        lua_getfield(L, 1, k);
        int v = lua_isnil(L, -1) ? -1 : (lua_toboolean(L, -1) ? 1 : 0);
        lua_pop(L, 1);
        return v;
    };
    auto str = [&](const char* k) {
        lua_getfield(L, 1, k);
        std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        return v;
    };
    float x = 0, y = 0, b = -1;
    s.pos = num("x", &x) & num("y", &y);
    s.x = x, s.y = y;
    if (num("button", &b)) s.button = static_cast<int>(b);
    s.down = flag("down") == 1;
    num("wheel", &s.wheel);
    s.key = str("key");
    s.chars = str("chars");
    s.ctrl = flag("ctrl"), s.shift = flag("shift"), s.alt = flag("alt");
    if (!s.key.empty() && KeyByName(s.key.c_str()) == ImGuiKey_None) return luaL_argerror(L, 1, "key: a name as ImGui names it");
    if (g_feed.size() < 4096) g_feed.push_back(s);
    lua_pushinteger(L, static_cast<lua_Integer>(g_feed.size()));
    return 1;
}
int UFeedPending(lua_State* L) {  // feed_pending() -> steps not yet given, frames the mouse is still held
    Check(L);
    lua_pushinteger(L, static_cast<lua_Integer>(g_feed.size()));
    lua_pushinteger(L, g_feedHold);
    return 2;
}

int UPressed(lua_State* L) {  // pressed(name [, repeat]): "Enter", "Escape", "Delete", "A", "F2", "UpArrow" ... (ImGui's names)
    Check(L);
    ImGuiKey k = KeyByName(luaL_checkstring(L, 1));
    if (k == ImGuiKey_None) return luaL_argerror(L, 1, "a key name as ImGui names it");
    lua_pushboolean(L, ImGui::IsKeyPressed(k, lua_toboolean(L, 2) != 0));
    return 1;
}
int UKeyDown(lua_State* L) {
    Check(L);
    ImGuiKey k = KeyByName(luaL_checkstring(L, 1));
    if (k == ImGuiKey_None) return luaL_argerror(L, 1, "a key name as ImGui names it");
    lua_pushboolean(L, ImGui::IsKeyDown(k));
    return 1;
}
int UMods(lua_State* L) {  // mods() -> ctrl, shift, alt
    Check(L);
    ImGuiIO& io = ImGui::GetIO();
    lua_pushboolean(L, io.KeyCtrl);
    lua_pushboolean(L, io.KeyShift);
    lua_pushboolean(L, io.KeyAlt);
    return 3;
}
int UFocusNext(lua_State* L) {  // focus_next(): the next widget takes the keyboard (a text field: typing goes in)
    Check(L);
    ImGui::SetKeyboardFocusHere();
    return 0;
}

// --- style, fonts, windows ---
ImGuiCol ColByName(const char* n) {
    static std::map<std::string, ImGuiCol> byName;
    if (byName.empty())
        for (int c = 0; c < ImGuiCol_COUNT; ++c) {  // "FrameBgHovered" -> "frame_bg_hovered"
            std::string s;
            for (const char* p = ImGui::GetStyleColorName(c); *p; ++p) {
                if (isupper(static_cast<unsigned char>(*p)) && !s.empty()) s += '_';
                s += static_cast<char>(tolower(static_cast<unsigned char>(*p)));
            }
            byName[s] = c;
        }
    auto it = byName.find(n);
    return it == byName.end() ? -1 : it->second;
}
struct VarName {
    const char* name;
    ImGuiStyleVar var;
    bool vec;
};
const VarName kVars[] = {
    {"alpha", ImGuiStyleVar_Alpha, false}, {"disabled_alpha", ImGuiStyleVar_DisabledAlpha, false},
    {"window_padding", ImGuiStyleVar_WindowPadding, true}, {"window_rounding", ImGuiStyleVar_WindowRounding, false},
    {"window_border", ImGuiStyleVar_WindowBorderSize, false}, {"window_min_size", ImGuiStyleVar_WindowMinSize, true},
    {"child_rounding", ImGuiStyleVar_ChildRounding, false}, {"child_border", ImGuiStyleVar_ChildBorderSize, false},
    {"popup_rounding", ImGuiStyleVar_PopupRounding, false}, {"popup_border", ImGuiStyleVar_PopupBorderSize, false},
    {"frame_padding", ImGuiStyleVar_FramePadding, true}, {"frame_rounding", ImGuiStyleVar_FrameRounding, false},
    {"frame_border", ImGuiStyleVar_FrameBorderSize, false}, {"item_spacing", ImGuiStyleVar_ItemSpacing, true},
    {"item_inner_spacing", ImGuiStyleVar_ItemInnerSpacing, true}, {"indent", ImGuiStyleVar_IndentSpacing, false},
    {"cell_padding", ImGuiStyleVar_CellPadding, true}, {"scrollbar_size", ImGuiStyleVar_ScrollbarSize, false},
    {"scrollbar_rounding", ImGuiStyleVar_ScrollbarRounding, false}, {"grab_min_size", ImGuiStyleVar_GrabMinSize, false},
    {"grab_rounding", ImGuiStyleVar_GrabRounding, false}, {"tab_rounding", ImGuiStyleVar_TabRounding, false},
    {"tab_border", ImGuiStyleVar_TabBorderSize, false}, {"tab_bar_border", ImGuiStyleVar_TabBarBorderSize, false},
    {"button_text_align", ImGuiStyleVar_ButtonTextAlign, true},
    {"selectable_text_align", ImGuiStyleVar_SelectableTextAlign, true}};

int UStyle(lua_State* L) {  // style({frame_bg = 0x2a2a2dff, frame_padding = {5, 5}, ...}, fn)
    Check(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    int cols = 0, vars = 0;
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        const char* k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : nullptr;
        if (!k) {
            lua_pop(L, 1);
            continue;
        }
        ImGuiCol c = ColByName(k);
        bool done = false;
        if (c >= 0 && lua_isinteger(L, -1)) {
            ImGui::PushStyleColor(c, Col4(static_cast<uint32_t>(lua_tointeger(L, -1))));
            ++cols;
            done = true;
        }
        for (const VarName& v : kVars) {
            if (done || strcmp(v.name, k)) continue;
            if (v.vec && lua_istable(L, -1)) {
                lua_rawgeti(L, -1, 1);
                lua_rawgeti(L, -2, 2);
                ImGui::PushStyleVar(v.var, ImVec2(static_cast<float>(lua_tonumber(L, -2)), static_cast<float>(lua_tonumber(L, -1))));
                lua_pop(L, 2);
            } else if (!v.vec && lua_isnumber(L, -1)) {
                ImGui::PushStyleVar(v.var, static_cast<float>(lua_tonumber(L, -1)));
            } else {
                ImGui::PopStyleColor(cols);
                ImGui::PopStyleVar(vars);
                return luaL_error(L, "w3.ui.style: %s needs %s", k, v.vec ? "{x, y}" : "a number");
            }
            ++vars;
            done = true;
        }
        lua_pop(L, 1);
        if (!done) {
            ImGui::PopStyleColor(cols);
            ImGui::PopStyleVar(vars);
            return luaL_error(L, "w3.ui.style: unknown key %s", k);
        }
    }
    return Body(L, 2, [cols, vars] {
        ImGui::PopStyleColor(cols);
        ImGui::PopStyleVar(vars);
    });
}
int UFont(lua_State* L) {  // font(size [, "bold"], fn): Segoe UI at size px (an em, as Qt's font-size)
    Check(L);
    int fnIdx = lua_isfunction(L, 2) ? 2 : 3;
    ImFont* f = FindFont(static_cast<int>(Num(L, 1) + 0.5f), fnIdx == 3 && lua_isstring(L, 2) && !strcmp(lua_tostring(L, 2), "bold"));
    if (!f) return Body(L, fnIdx, [] {});
    ImGui::PushFont(f);
    return Body(L, fnIdx, [] { ImGui::PopFont(); });
}
int UTextSize(lua_State* L) {  // text_size(s [, size, bold, wrap_width]) -> w, h (Qt's advance and line height)
    Check(L);
    lua_settop(L, 4);
    size_t len = 0;
    const char* s = luaL_tolstring(L, 1, &len);
    if (!lua_isnoneornil(L, 2) && OptNum(L, 4, 0.0f) <= 0) {
        int bold = StyleArg(L, 3);
        float px = Num(L, 2);
        int a = 0, d = 0;
        if (UiTextMetrics(px, bold, &a, &d)) {
            int w = UiTextWidth(s, len, px, bold);
            lua_pop(L, 1);
            lua_pushinteger(L, w);
            lua_pushinteger(L, a + d);
            return 2;
        }
    }
    ImFont* f = OptFont(L, 2, 3);
    if (!f) f = ImGui::GetFont();
    ImVec2 sz = f->CalcTextSizeA(f->FontSize, FLT_MAX, OptNum(L, 4, 0.0f), s, s + len);
    lua_pop(L, 1);
    lua_pushnumber(L, sz.x);
    lua_pushnumber(L, sz.y);
    return 2;
}
int UTextHint(lua_State* L) {  // text_hint(s, size [, bold]) -> the width Qt's size hints use (QFontMetrics::size)
    Check(L);
    lua_settop(L, 3);
    size_t len = 0;
    const char* s = luaL_tolstring(L, 1, &len);
    int bold = StyleArg(L, 3);
    int w = UiTextHintWidth(s, len, Num(L, 2), bold);
    lua_pop(L, 1);
    lua_pushinteger(L, w);
    return 1;
}
int UFontMetrics(lua_State* L) {  // font_metrics(size [, bold]) -> ascent, descent, line height (px, as Qt's)
    Check(L);
    if (!lua_isnoneornil(L, 1)) {
        int bold = StyleArg(L, 2);
        int a = 0, d = 0;
        if (UiTextMetrics(Num(L, 1), bold, &a, &d)) {
            lua_pushinteger(L, a);
            lua_pushinteger(L, d);
            lua_pushinteger(L, a + d);
            return 3;
        }
    }
    ImFont* f = OptFont(L, 1, 2);
    if (!f) f = ImGui::GetFont();
    lua_pushnumber(L, f->Ascent);
    lua_pushnumber(L, -f->Descent);
    lua_pushnumber(L, f->FontSize);
    return 3;
}

// panel(id, x, y, w, h, opts, fn): a window without a title bar at a fixed place, the way an editor's bar and panel
// stand on the game. opts: {bg = colour, border = colour, pad = {x, y}, scroll = bool, front = bool, inputs = bool}
int UPanel(lua_State* L) {
    Check(L);
    const char* id = luaL_checkstring(L, 1);
    float x = Num(L, 2), y = Num(L, 3), w = Num(L, 4), h = Num(L, 5);
    bool scroll = false, front = true, inputs = true;
    uint32_t bg = 0x18181aff, border = 0;
    float px = 0, py = 0, bw = 0;
    if (lua_istable(L, 6)) {
        lua_getfield(L, 6, "bg");
        if (lua_isinteger(L, -1)) bg = static_cast<uint32_t>(lua_tointeger(L, -1));
        lua_getfield(L, 6, "border");
        if (lua_isinteger(L, -1)) border = static_cast<uint32_t>(lua_tointeger(L, -1)), bw = 1;
        lua_getfield(L, 6, "scroll");
        scroll = lua_toboolean(L, -1) != 0;
        lua_getfield(L, 6, "front");
        if (!lua_isnil(L, -1)) front = lua_toboolean(L, -1) != 0;
        lua_getfield(L, 6, "inputs");
        if (!lua_isnil(L, -1)) inputs = lua_toboolean(L, -1) != 0;
        lua_getfield(L, 6, "pad");
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            px = static_cast<float>(lua_tonumber(L, -2));
            py = static_cast<float>(lua_tonumber(L, -1));
            lua_pop(L, 2);
        }
        lua_pop(L, 6);
    }
    ImGuiWindowFlags f = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav;
    if (!scroll) f |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (!front) f |= ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!inputs) f |= ImGuiWindowFlags_NoInputs;
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px, py));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, bw);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col4(bg));
    ImGui::PushStyleColor(ImGuiCol_Border, Col4(border));
    bool open = ImGui::Begin(id, nullptr, f);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    // a panel over (nearly) the whole screen goes behind every window while the TW3SE Menu is open: the menu and its
    // windows get the mouse, the panel keeps it elsewhere (an editor HUD over the whole screen would take every
    // hover and click from the menu)
    ImVec2 screen = ImGui::GetIO().DisplaySize;
    if (g_panelsBehind && w >= screen.x * 0.9f && h >= screen.y * 0.9f)
        ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
    if (!open) {
        ImGui::End();
        return 0;
    }
    return Body(L, 7, [] { ImGui::End(); });
}
int UChildAt(lua_State* L) {  // region(id, w, h, opts, fn): a child region at the cursor (scroll = bool, bg = colour, pad = {x, y})
    Check(L);
    const char* id = luaL_checkstring(L, 1);
    float w = Num(L, 2), h = Num(L, 3);
    bool scroll = false;
    uint32_t bg = 0;
    float px = 0, py = 0;
    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "scroll");
        scroll = lua_toboolean(L, -1) != 0;
        lua_getfield(L, 4, "bg");
        if (lua_isinteger(L, -1)) bg = static_cast<uint32_t>(lua_tointeger(L, -1));
        lua_getfield(L, 4, "pad");
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1);
            lua_rawgeti(L, -2, 2);
            px = static_cast<float>(lua_tonumber(L, -2));
            py = static_cast<float>(lua_tonumber(L, -1));
            lua_pop(L, 2);
        }
        lua_pop(L, 3);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px, py));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col4(bg));
    ImGuiWindowFlags f = scroll ? 0 : (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::BeginChild(id, ImVec2(w, h), (px || py) ? ImGuiChildFlags_AlwaysUseWindowPadding : 0, f);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    return Body(L, 5, [] { ImGui::EndChild(); });
}
int UScroll(lua_State* L) {  // scroll() -> x, y, max x, max y of this window
    Check(L);
    lua_pushnumber(L, ImGui::GetScrollX());
    lua_pushnumber(L, ImGui::GetScrollY());
    lua_pushnumber(L, ImGui::GetScrollMaxX());
    lua_pushnumber(L, ImGui::GetScrollMaxY());
    return 4;
}
int USetScroll(lua_State* L) {  // set_scroll(x, y) (nil: unchanged)
    Check(L);
    if (!lua_isnoneornil(L, 1)) ImGui::SetScrollX(Num(L, 1));
    if (!lua_isnoneornil(L, 2)) ImGui::SetScrollY(Num(L, 2));
    return 0;
}
int UDisabled(lua_State* L) {  // disabled(flag, fn)
    Check(L);
    bool d = lua_toboolean(L, 1) != 0;
    ImGui::BeginDisabled(d);
    return Body(L, 2, [] { ImGui::EndDisabled(); });
}

// --- popups ---
int UOpenPopup(lua_State* L) {  // open_popup(id): the popup opens on the next popup(id, ...)
    Check(L);
    ImGui::OpenPopup(luaL_checkstring(L, 1));
    return 0;
}
int UPopup(lua_State* L) {  // popup(id, fn [, x, y]) -> open: fn draws it while it is open; x, y place it
    Check(L);
    const char* id = luaL_checkstring(L, 1);
    if (!lua_isnoneornil(L, 3)) ImGui::SetNextWindowPos(ImVec2(Num(L, 3), Num(L, 4)), ImGuiCond_Appearing);
    bool open = ImGui::BeginPopup(id, ImGuiWindowFlags_NoSavedSettings);
    if (!open) {
        lua_pushboolean(L, false);
        return 1;
    }
    Body(L, 2, [] { ImGui::EndPopup(); });
    lua_pushboolean(L, true);
    return 1;
}
int UClosePopup(lua_State* L) {
    Check(L);
    ImGui::CloseCurrentPopup();
    return 0;
}
int UPopupOpen(lua_State* L) {
    Check(L);
    lua_pushboolean(L, ImGui::IsPopupOpen(luaL_checkstring(L, 1)));
    return 1;
}

// --- pictures, the clipboard ---
int UImageLoad(lua_State* L) {  // image_load(path) -> id, w, h (nil when it cannot be read)
    int w = 0, h = 0;
    unsigned long long id = UiLoadImage(luaL_checkstring(L, 1), &w, &h);
    if (!id) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(id));
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 3;
}
int UImageFree(lua_State* L) {
    UiFreeImage(static_cast<unsigned long long>(luaL_checkinteger(L, 1)));
    return 0;
}
int UImage(lua_State* L) {  // image(id, w, h [, tint]): a picture as a widget
    Check(L);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = Num(L, 2), h = Num(L, 3);
    ImTextureID id = static_cast<ImTextureID>(luaL_checkinteger(L, 1));
    if (id) ImGui::GetWindowDrawList()->AddImage(id, p, ImVec2(p.x + w, p.y + h), ImVec2(0, 0), ImVec2(1, 1), Col(L, 4));
    ImGui::Dummy(ImVec2(w, h));
    return 0;
}
int UOverlay(lua_State* L) {  // overlay(on): this mod's windows stay over the game while the TW3SE Menu is closed
    UiSetOverlay(lua_toboolean(L, 1) != 0);
    return 0;
}
int UCursorOwn(lua_State* L) {  // cursor_own(on): the Windows cursor over the whole game, never the game's own
    UiSetCursorOwn(lua_toboolean(L, 1) != 0);
    return 0;
}
int UCursorFree(lua_State* L) {  // cursor_free(on): the cursor moves freely over the game (the game may not centre it)
    UiSetCursorFree(lua_toboolean(L, 1) != 0);
    return 0;
}
int UScreenshot(lua_State* L) {  // screenshot(path) -> the shots saved before it (wait for screenshots_done to pass)
    const char* path = luaL_checkstring(L, 1);
    int before = UiScreenshotsDone();
    if (!UiScreenshot(path)) {
        lua_pushnil(L);
        lua_pushstring(L, "the overlay is off (it could not start)");
        return 2;
    }
    lua_pushinteger(L, before);
    return 1;
}
int UScreenshotsDone(lua_State* L) {  // screenshots_done() -> how many screenshot() saved so far
    lua_pushinteger(L, UiScreenshotsDone());
    return 1;
}
int UChars(lua_State* L) {  // chars() -> the text typed this frame (UTF-8, "" when none); the UI must have the keyboard
    Check(L);
    std::string out;
    for (ImWchar c : ImGui::GetIO().InputQueueCharacters) {
        unsigned int cp = c;
        if (cp < 0x20 || cp == 0x7f) continue;
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) out += static_cast<char>(0xC0 | (cp >> 6)), out += static_cast<char>(0x80 | (cp & 0x3F));
        else out += static_cast<char>(0xE0 | (cp >> 12)), out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
             out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}
bool g_luaWantText = false;
int UWantText(lua_State* L) {  // want_text(): a text field of this mod has the keyboard (call every frame it has)
    Check(L);
    g_luaWantText = true;
    ImGui::SetNextFrameWantCaptureKeyboard(true);
    return 0;
}
int UTextOffsets(lua_State* L) {  // text_offsets(s, size [, bold]) -> {x...}, {byte...}: before each letter and at the end
    Check(L);
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    int bold = StyleArg(L, 3);
    std::vector<int> xs, bytes;
    UiTextOffsets(s, len, Num(L, 2), bold, &xs, &bytes);
    lua_createtable(L, static_cast<int>(xs.size()), 0);
    for (size_t i = 0; i < xs.size(); ++i) lua_pushinteger(L, xs[i]), lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    lua_createtable(L, static_cast<int>(bytes.size()), 0);
    for (size_t i = 0; i < bytes.size(); ++i) lua_pushinteger(L, bytes[i]), lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    return 2;
}
int UTextBearings(lua_State* L) {  // text_bearings(s, size [, bold]) -> {rb...}, least: each letter's right bearing
    Check(L);                        // and the font's least, in 1/64 px (Qt's QFixed), as Qt's line breaking has them
    size_t len = 0;
    const char* s = luaL_checklstring(L, 1, &len);
    int bold = StyleArg(L, 3);
    std::vector<int> rb;
    int least = 0;
    UiTextBearings(s, len, Num(L, 2), bold, &rb, &least);
    lua_createtable(L, static_cast<int>(rb.size()), 0);
    for (size_t i = 0; i < rb.size(); ++i) lua_pushinteger(L, rb[i]), lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    lua_pushinteger(L, least);
    return 2;
}
int UCaretMs(lua_State* L) {  // caret_ms() -> Windows' caret blink time (Qt's cursorFlashTime is twice it)
    UINT ms = GetCaretBlinkTime();
    lua_pushinteger(L, ms == INFINITE ? 0 : static_cast<lua_Integer>(ms));
    return 1;
}
int ULocale(lua_State* L) {  // locale() -> decimal sign, thousands sign, name: Windows' (Qt shows numbers with them)
    wchar_t dec[8] = L".", grp[8] = L",", name[LOCALE_NAME_MAX_LENGTH] = L"en-US";
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SDECIMAL, dec, 8);
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, grp, 8);
    GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH);
    const wchar_t* parts[] = {dec, grp, name};
    for (const wchar_t* w : parts) {
        char u[64] = {};
        WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, nullptr, nullptr);
        lua_pushstring(L, u);
    }
    return 3;
}
int UClipboard(lua_State* L) {  // clipboard([text]) -> the clipboard's text
    Check(L);
    if (lua_isstring(L, 1)) ImGui::SetClipboardText(lua_tostring(L, 1));
    const char* t = ImGui::GetClipboardText();
    lua_pushstring(L, t ? t : "");
    return 1;
}

}  // namespace

// Segoe UI regular and bold at every pixel size a Qt theme uses, for w3.ui.font (the host calls it while it
// builds the font atlas, after its own fonts). -> how many were added.
// w3.ui.feed's step for this frame (ui.cpp and the lab: after the platform's NewFrame, before ImGui's)
void UiFeedApply() { FeedApply(); }
bool g_panelsBehind = false;

int UiAddFonts() {
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    char dir[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, win, -1, dir, sizeof dir, nullptr, nullptr);
    static const ImWchar ranges[] = {0x0020, 0x017F, 0x0400, 0x045F, 0x0490, 0x0491, 0x2000, 0x206F, 0x2190,
                                     0x21FF, 0x2500, 0x25FF, 0x2700, 0x27BF, 0};  // (0x0400-0x045F, 0x0490-0x0491: Cyrillic)
    ImGuiIO& io = ImGui::GetIO();
    int added = 0;
    for (bool bold : {false, true}) {
        std::string path = std::string(dir) + (bold ? "\\Fonts\\segoeuib.ttf" : "\\Fonts\\segoeui.ttf");
        float k = EmToImGui(path);
        if (k <= 0) continue;
        for (int px : {9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 26, 28}) {
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            cfg.OversampleV = 1;
            cfg.PixelSnapH = true;
            snprintf(cfg.Name, sizeof cfg.Name, "Segoe UI %s%d px", bold ? "bold " : "", px);
            ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), px * k, &cfg, ranges);
            if (f) g_fonts.push_back({px, bold, f}), ++added;
        }
    }
    return added;
}

void UiKeyRepeatFromWindows() {
    int delay = 1, speed = 31;  // the control panel's steps: delay 0..3 = 250..1000 ms, speed 0..31 = 2.5..30 per second
    SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0);
    SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &speed, 0);
    ImGuiIO& io = ImGui::GetIO();
    io.KeyRepeatDelay = 0.25f * (delay + 1);
    io.KeyRepeatRate = 1.0f / (2.5f + (30.0f - 2.5f) * speed / 31.0f);
    io.MouseDoubleClickTime = GetDoubleClickTime() / 1000.0f;            // Qt takes Windows' (500 ms), ImGui 300
    io.MouseDoubleClickMaxDist = static_cast<float>(GetSystemMetrics(SM_CXDOUBLECLK));
}
bool UiTakeLuaWantText() {
    bool w = g_luaWantText;
    g_luaWantText = false;
    return w;
}
// take_mouse(on): whether the UI takes the mouse from the game this frame (else ImGui decides: over a window)
int g_luaMouse = -1;
int UTakeMouse(lua_State* L) {
    Check(L);
    g_luaMouse = lua_toboolean(L, 1) ? 1 : 0;
    return 0;
}
bool UiTakeLuaMouse(bool* take) {
    if (g_luaMouse < 0) return false;
    *take = g_luaMouse == 1;
    g_luaMouse = -1;
    return true;
}

void LuaUiOpenMore(lua_State* L) {  // into w3.ui at -1
    static const luaL_Reg more[] = {
        {"mouse", &UMouse}, {"mouse_down", &UMouseDown}, {"mouse_clicked", &UMouseClicked},
        {"mouse_released", &UMouseReleased}, {"mouse_double", &UMouseDouble}, {"mouse_drag", &UMouseDrag},
        {"hit", &UHit}, {"item", &UItem}, {"pos", &UPos}, {"set_pos", &USetPos}, {"dummy", &UDummy},
        {"avail", &UAvail}, {"window_rect", &UWindowRect}, {"screen", &UScreen}, {"window_hovered", &UWindowHovered},
        {"wants", &UWants}, {"cursor", &UCursor}, {"time", &UTime}, {"pressed", &UPressed}, {"feed", &UFeed}, {"feed_pending", &UFeedPending}, {"key_down", &UKeyDown}, {"take_mouse", &UTakeMouse},
        {"mods", &UMods}, {"focus_next", &UFocusNext}, {"style", &UStyle}, {"font", &UFont},
        {"text_size", &UTextSize}, {"text_hint", &UTextHint}, {"font_metrics", &UFontMetrics}, {"panel", &UPanel}, {"region", &UChildAt},
        {"scroll", &UScroll}, {"set_scroll", &USetScroll}, {"disabled", &UDisabled}, {"open_popup", &UOpenPopup},
        {"popup", &UPopup}, {"close_popup", &UClosePopup}, {"popup_open", &UPopupOpen},
        {"image_load", &UImageLoad}, {"image_free", &UImageFree}, {"image", &UImage}, {"clipboard", &UClipboard}, {"chars", &UChars}, {"want_text", &UWantText}, {"text_offsets", &UTextOffsets}, {"text_bearings", &UTextBearings},
        {"caret_ms", &UCaretMs}, {"locale", &ULocale}, {"overlay", &UOverlay}, {"cursor_free", &UCursorFree},
        {"cursor_own", &UCursorOwn},
        {"screenshot", &UScreenshot}, {"screenshots_done", &UScreenshotsDone},
        {nullptr, nullptr}};
    luaL_setfuncs(L, more, 0);
    static const luaL_Reg draw[] = {{"line", &DLine}, {"rect", &DRect}, {"fill", &DFill}, {"circle", &DCircle},
                                    {"disc", &DDisc}, {"ellipse", &DEllipse}, {"bezier", &DBezier}, {"poly", &DPoly},
                                    {"fill_poly", &DFillPoly}, {"text", &DText}, {"text_scaled", &DTextScaled},
                                    {"image", &DImage},
                                    {"clip", &DClip}, {"invert", &DInvert}, {"layer", &DLayer}, {nullptr, nullptr}};
    luaL_newlib(L, draw);
    lua_setfield(L, -2, "draw");
}

}  // namespace tw3se
