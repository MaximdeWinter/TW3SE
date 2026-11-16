// Dear ImGui settings for plugins and .asi mods that draw with TW3SE's ImGui (tw3se_imgui.h).
// They match TW3SE's own (src\core\imconfig_tw3se.h): the same ImGui version and the same data layout, so a plugin's
// ImGui works on TW3SE's context. Change nothing here except IM_ASSERT.
#pragma once
#define IMGUI_DISABLE_DEMO_WINDOWS
#define IMGUI_DISABLE_DEBUG_TOOLS
#define IMGUI_DISABLE_OBSOLETE_KEYIO
// An ImGui check that fails would end the game in a build without NDEBUG; here it does nothing.
#define IM_ASSERT(_EXPR) ((void)(_EXPR))
