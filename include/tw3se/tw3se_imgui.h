/* TW3SE: draw your own windows with Dear ImGui over the game (plugin interface 21, TW3SE 1.2).
 *
 * TW3SE draws them in its overlay with its fonts and cursor, and routes keyboard and mouse. No DirectX
 * hook, no ImGui backend and no input code of your own.
 *
 * Build: add tw3se\imgui\imgui.cpp, imgui_draw.cpp, imgui_tables.cpp and imgui_widgets.cpp from this SDK to your
 * project (the same ImGui version as TW3SE, with its settings in tw3se\imgui\imconfig.h). Then:
 *
 *     #include "tw3se/tw3se_imgui.h"
 *
 *     static bool g_open = false;
 *     static void DrawMenu() {
 *         if (!g_open) return;
 *         ImGui::Begin("My trainer", &g_open);
 *         if (ImGui::Button("Heal")) tw3se::call_method(...);
 *         ImGui::End();
 *         if (!g_open) tw3se::imgui::capture_input(false);
 *     }
 *
 *     // in TW3SE_PluginLoad (or after tw3se::init in an .asi, see tw3se_asi.h):
 *     tw3se::imgui::on_draw(&DrawMenu);
 *     tw3se::imgui::overlay(true);              // windows over the game while TW3SE's menu is closed
 *     // on your key: g_open = true; tw3se::imgui::capture_input(true);   (all input to your menu)
 *
 * The callbacks run once per frame on the game's main thread, inside TW3SE's ImGui frame. A callback that faults
 * will be logged and taken out; the game goes on.
 */
#pragma once
#include "tw3se/imgui/imgui.h"
#include "tw3se/tw3se.hpp"

namespace tw3se {
namespace imgui {

inline void _thunk(void* imguiContext, void* user) {
    static bool allocators = [] {  // once per DLL: memory that crosses to TW3SE comes from TW3SE's allocator
        void *a = nullptr, *f = nullptr, *u = nullptr;
        api()->ImGuiAllocators(&a, &f, &u);
        if (a && f) ImGui::SetAllocatorFunctions(reinterpret_cast<ImGuiMemAllocFunc>(a),
                                                 reinterpret_cast<ImGuiMemFreeFunc>(f), u);
        return true;
    }();
    (void)allocators;
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(imguiContext));
    reinterpret_cast<void (*)()>(user)();
}

/* Calls fn once per frame inside TW3SE's ImGui frame. False on a TW3SE before 1.2, or when this SDK's ImGui and
 * TW3SE's differ (tw3se.log says which). */
inline bool on_draw(void (*fn)()) {
    if (!fn || !api() || api()->interfaceVersion < 21) return false;
    return api()->RegisterOnImGui(&_thunk, reinterpret_cast<void*>(fn), IMGUI_VERSION_NUM,
                                  static_cast<uint32_t>(sizeof(ImGuiIO)), static_cast<uint32_t>(sizeof(ImGuiStyle))) != 0;
}

/* Your windows show over the game while TW3SE's menu is closed. The game keeps the input your windows do not take. */
inline void overlay(bool on) {
    if (api() && api()->interfaceVersion >= 21) api()->ImGuiOverlay(on ? 1 : 0);
}

/* Your menu is open: every key and mouse message goes to ImGui, the cursor shows and moves freely. False gives the
 * input back to the game. Main thread only (a draw callback, a tick, a console command). */
inline void capture_input(bool on) {
    if (api() && api()->interfaceVersion >= 21) api()->ImGuiCaptureInput(on ? 1 : 0);
}

}  // namespace imgui
}  // namespace tw3se
