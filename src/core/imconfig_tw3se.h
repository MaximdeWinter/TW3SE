// Dear ImGui settings for TW3SE (included through IMGUI_USER_CONFIG, build.bat).
// An ImGui assert would end the game (assert() in a build without NDEBUG): it goes to tw3se.log instead. ImGui's
// error recovery (io.ConfigErrorRecovery, ui.cpp) repairs what a broken Lua window leaves open.
#pragma once
void TW3SE_ImAssert(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) do { if (!(_EXPR)) TW3SE_ImAssert(#_EXPR, __FILE__, __LINE__); } while (0)
#define IMGUI_DISABLE_DEMO_WINDOWS
#define IMGUI_DISABLE_DEBUG_TOOLS
#define IMGUI_DISABLE_OBSOLETE_KEYIO
