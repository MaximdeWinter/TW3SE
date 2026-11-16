// MyPlugin: a TW3SE plugin to start from (Visual Studio 2022, MyPlugin.sln). Build Release|x64 and copy
// bin\Release\MyPlugin.dll into <game>\bin\x64_dx12\tw3se\plugins\. tw3se.log says when it loaded.
//
// It has the three things most mods need:
//   - a script function:  import function MyPlugin_Greet(who : string) : string;
//   - a menu on F6 with Dear ImGui, drawn by TW3SE (no DirectX code of your own)
//   - a console command:  python tools\tw3se.py myplugin
#include "tw3se/tw3se_imgui.h"
#include <windows.h>
#include <string>

static bool g_open = false;

// import function MyPlugin_Greet(who : string) : string;
static std::string Greet(std::string who) { return "Hello, " + who + ", from MyPlugin"; }

static void DrawMenu() {
    if (!g_open) return;
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("MyPlugin", &g_open)) {
        void* player = tw3se::the_player();
        auto money = player ? tw3se::call(player, "GetMoney") : std::vector<TW3SE_Value>{};
        ImGui::Text("Crowns: %lld", money.empty() ? 0LL : static_cast<long long>(money[0].v.i));
        if (ImGui::Button("Give 100 crowns") && player) tw3se::call(player, "AddMoney", {tw3se::value(100)});
    }
    ImGui::End();
    if (!g_open) tw3se::imgui::capture_input(false);
}

static void OnTick(float, void*) {
    static bool was = false;
    bool down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (down && !was && (tw3se::game_state() & TW3SE_STATE_IN_GAME)) {
        g_open = !g_open;
        tw3se::imgui::capture_input(g_open);  // the menu gets keyboard and mouse while it is open
    }
    was = down;
}

static void Cmd(const char*, char* reply, uint32_t size) {
    snprintf(reply, size, "MyPlugin: menu %s, game state 0x%x", g_open ? "open" : "closed", tw3se::game_state());
}

TW3SE_PLUGIN("MyPlugin", "YourName", 0x00010000, /*minInterface*/ 21)

TW3SE_EXPORT int TW3SE_PluginLoad(const TW3SE_Interface* api) {
    tw3se::init(api);
    tw3se::register_native<&Greet>("MyPlugin_Greet");
    tw3se::imgui::on_draw(&DrawMenu);
    tw3se::imgui::overlay(true);
    tw3se::on_tick(&OnTick);
    tw3se::register_command("myplugin", &Cmd, "myplugin: MyPlugin's state");
    return 1;
}
