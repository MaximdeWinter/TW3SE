// TW3SE core - w3.ui (lua_ui.cpp), shared by TW3SE and the UI lab. The host (ui.cpp in the game, lab.cpp in
// the lab) runs the w3.on_ui functions inside an ImGui frame with g_uiFrame set, and gives the functions below.
#pragma once
#include <vector>
struct lua_State;
struct ImDrawList;
struct ImDrawCmd;
typedef void (*ImDrawCallback)(const ImDrawList* parent_list, const ImDrawCmd* cmd);

namespace tw3se {
extern bool g_uiFrame;   // inside an ImGui frame: w3.ui calls work
extern bool g_panelsBehind;  // the TW3SE Menu is open: full-screen panels go behind it (ui.cpp sets it, the lab not)
extern int g_uiTables;   // w3.ui.table bodies open right now

void LuaUiOpen(lua_State* L);      // w3.ui into the table on top of the stack (w3)
void LuaUiOpenMore(lua_State* L);  // the drawing, mouse, style, font, image and popup functions (into w3.ui at -1)

// Segoe UI at the pixel sizes of a Qt theme, for w3.ui.font: the host calls it while it makes the atlas.
int UiAddFonts();
// ImGui's key repeat as Windows' (the keyboard delay and speed in the control panel), as Qt repeats: the host calls it once.
void UiKeyRepeatFromWindows();
// A Lua text field has the keyboard this frame (w3.ui.want_text): the game gets no keys. Read and cleared by the host.
bool UiTakeLuaWantText();
void UiFeedApply();               // w3.ui.feed's step for this frame (after the platform's NewFrame)
bool UiTakeLuaMouse(bool* take);  // w3.ui.take_mouse this frame (false: not called)
// w3.ui.icon's names and their code points in Windows' icon font (Segoe MDL2 Assets / Segoe Fluent Icons): the host
// merges these into its main font.
struct UiIconName { const char* name; unsigned short code; };
const UiIconName* UiIcons(int* count);

// from the host
bool UiIsOpen();
// the controller for w3.ui.key, set by the game's host (pad.cpp); null in a host without one (the lab)
extern unsigned (*g_uiPadMask)();         // the buttons held now (bits 0-15 XInput's, 16 LT, 17 RT)
extern const char* (*g_uiPadName)(int);   // 12 -> "Pad_A"
extern void (*g_uiLog)(const char* fmt, ...);  // the host's log (tw3se.log in the game; null in the lab)
void UiSetOverlay(bool on);     // w3.ui.overlay
// w3.ui.screenshot: the game's picture with the overlay as the next frame shows it, saved as a PNG (the game: uishot's
// back buffer copy, encoded on a thread) -> false when nothing is drawn yet; UiScreenshotsDone counts the saved ones
bool UiScreenshot(const char* path);
int UiScreenshotsDone();
// Measuring: a box of the back buffer in each of the next `frames` frames, raw files <dir>\0000.raw ... (u32 w, h,
// present number, then BGRA), w or h 0: the middle 60 % of the picture; UiShotBurstDone counts the files written.
bool UiShotBurst(const char* dir, int frames, int x, int y, int w, int h);
int UiShotBurstDone();
void UiSetCursorFree(bool on);  // w3.ui.cursor_free
void UiSetCursorOwn(bool on);   // w3.ui.cursor_own
// A PNG (or another picture WIC reads) as a texture for ImGui. -> its ImTextureID as an integer (0: not loaded),
// its size. The game uploads on the render thread: the id is valid at once, the picture shows once it is up.
unsigned long long UiLoadImage(const char* path, int* width, int* height);
void UiFreeImage(unsigned long long id);
// An RGBA texture made from pixels (the text atlas). -> its ImTextureID.
unsigned long long UiCreateTexture(int width, int height, const void* rgba);
// The callback that switches to the text shader for the draws after it (dual-source blending per colour channel;
// its user data: TextParams in lua_ui_text.cpp). ImDrawCallback_ResetRenderState switches back.
ImDrawCallback UiTextCallback();
// The callback that switches to inverting the destination (out = 1 - dst) for the draws after it.
ImDrawCallback UiInvertCallback();

// Text as Qt draws it (lua_ui_text.cpp)
int UiMakeTextFonts();
void UiMakeTextFontsLater();  // the same on a thread of its own (the text functions wait for it)
void UiSetTextGamma(float gamma, int mode);  // the lab's comparison with Qt
bool UiTextMetrics(float px, int bold, int* ascent, int* descent);
int UiTextWidth(const char* s, unsigned long long len, float px, int bold);
int UiTextOffsets(const char* s, unsigned long long len, float px, int bold, std::vector<int>* xs, std::vector<int>* bytes);
int UiTextBearings(const char* s, unsigned long long len, float px, int bold, std::vector<int>* rb, int* minRb);
int UiTextHintWidth(const char* s, unsigned long long len, float px, int bold);  // QFontMetrics::size()
bool UiDrawText(ImDrawList* dl, float x, float y, unsigned int colour, unsigned int bg, const char* s, unsigned long long len,
                float px, int bold, bool gray = false);
void UiSetGrayGamma(float gamma, int variant = -1);
bool UiDrawTextScaled(ImDrawList* dl, float x, float y, unsigned int colour, const char* s, unsigned long long len,
                      float px, int bold);  // any size: a made size's letters drawn smaller or larger
}  // namespace tw3se
