// An .asi mod that uses TW3SE (TW3SE 1.2, tw3se_asi.h): no TW3SE plugin export, nothing linked. Without TW3SE it does
// nothing. Built as TW3SEAsiExample.asi; put it in <game>\bin\x64_dx12.
//
//     import function AsiExample_State() : int;       // TW3SE_STATE_* bits of the game's state
//     TW3SE_Send("asiexample.cmd", "ping");            // the mod answers on "asiexample.reply": "pong ping"
#include "tw3se/tw3se_asi.h"
#include <string>

static int State() { return static_cast<int>(tw3se::game_state()); }

static void OnCommand(const char*, const char* text, void*) {
    std::string reply = std::string("pong ") + (text ? text : "");
    tw3se::send("asiexample.reply", reply.c_str());
}

extern "C" __declspec(dllexport) void InitializeASI() {
    if (!tw3se::asi::connect()) return;
    tw3se::register_native<&State>("AsiExample_State");
    tw3se::on_message("asiexample.cmd", &OnCommand);
    tw3se::api()->Log("AsiExample", "connected to TW3SE (interface %u)", tw3se::api()->interfaceVersion);
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(self);
    return TRUE;
}
