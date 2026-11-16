// TW3SE plugin example: a menu of your own with Dear ImGui, and natives that return strings and names (TW3SE 1.2,
// interface 21). Drop imgui_example.dll into <game>\bin\x64_dx12\tw3se\plugins\.
//
// F7 opens and closes the menu. While it is open, keyboard and mouse go to it. The window "TW3SE ImGui example"
// shows the player's crowns and gives 100 more with a button.
//
//     import function ImGuiExample_Greet(who : string) : string;   // "Hello, <who>"
//     import function ImGuiExample_Tag(text : string) : name;      // the name of a text
//     import function ImGuiExample_TagText(tag : name) : string;   // the text of a name
#include "tw3se/tw3se_imgui.h"
#include <windows.h>
#include <string>

static bool g_open = false;
static int g_clicks = 0;

static void DrawMenu() {
    if (!g_open) return;
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("TW3SE ImGui example", &g_open)) {
        void* player = tw3se::the_player();
        auto money = player ? tw3se::call(player, "GetMoney") : std::vector<TW3SE_Value>{};
        ImGui::Text("Crowns: %lld", money.empty() ? 0LL : static_cast<long long>(money[0].v.i));
        if (ImGui::Button("Give 100 crowns") && player) {
            tw3se::call(player, "AddMoney", {tw3se::value(100)});
            ++g_clicks;
        }
        ImGui::Text("Clicked %d time(s)", g_clicks);
        ImGui::TextDisabled("F7 closes this menu");
    }
    ImGui::End();
    if (!g_open) tw3se::imgui::capture_input(false);
}

static void OnTick(float, void*) {
    static bool was = false;
    bool down = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (down && !was) {
        g_open = !g_open;
        tw3se::imgui::capture_input(g_open);
    }
    was = down;
}

// "imguiexample open|close": the same as F7, for a test from the console
static void Cmd(const char* args, char* reply, uint32_t size) {
    std::string a = args ? args : "";
    if (a == "open" || a == "close") {
        g_open = a == "open";
        tw3se::imgui::capture_input(g_open);
    }
    snprintf(reply, size, "menu %s, %d click(s)", g_open ? "open" : "closed", g_clicks);
}

static std::string Greet(std::string who) { return "Hello, " + who; }
static tw3se::name Tag(std::string text) { return tw3se::name(text.c_str()); }
static std::string TagText(tw3se::name tag) { return tag.valid() ? tag.str() : ""; }

TW3SE_PLUGIN("ImGui example", "MaximdeWinter", 0x00010000, /*minInterface*/ 21)

TW3SE_EXPORT int TW3SE_PluginLoad(const TW3SE_Interface* api) {
    tw3se::init(api);
    if (!tw3se::imgui::on_draw(&DrawMenu)) api->Log("ImGui example", "no ImGui (see tw3se.log)");
    tw3se::imgui::overlay(true);
    tw3se::on_tick(&OnTick);
    tw3se::register_command("imguiexample", &Cmd, "imguiexample open|close: the example plugin's menu");
    tw3se::register_native<&Greet>("ImGuiExample_Greet");
    tw3se::register_native<&Tag>("ImGuiExample_Tag");
    tw3se::register_native<&TagText>("ImGuiExample_TagText");
    return 1;
}
