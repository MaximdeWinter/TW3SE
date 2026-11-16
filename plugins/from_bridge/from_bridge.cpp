// Witcher Native Bridge's own example (examples/ExampleRemastered; Witcher Native Bridge by rejuvenate, MIT license)
// as a TW3SE plugin. The script side stays exactly as it was: the same five imports, the same exec function
// (from_bridge.ws beside this file). Each native shows the bridge's lines above it and the TW3SE function below.
//
// What changes for a bridge mod:
//   - #include <WitcherNativeBridgeApi.h> and WitcherNativeBridge.lib  ->  #include "tw3se/tw3se.hpp" (header only)
//   - DllMain with a thread that registers  ->  TW3SE_PluginLoad, called once at the start
//   - a native reads its parameters and writes its result itself  ->  a plain C++ function with typed parameters
//   - the .asi in bin\x64_dx12  ->  the .dll in bin\x64_dx12\tw3se\plugins
//
// Build (x64 Native Tools prompt, from the SDK folder):
//   cl /nologo /O2 /MT /LD /EHsc /std:c++17 /Iinclude example\from_bridge.cpp /link /OUT:from_bridge.dll
#include "tw3se/tw3se.hpp"

#include <string>

// Bridge:
//   void IntNative(void*, void* frame, void* result) {
//       int value = WNB_ReadIntParameter(frame);
//       WNB_ReturnInt(frame, result, value);
//   }
static int Example_Int(int value) { return value; }

// Bridge: WNB_ReadBoolParameter, WNB_ReturnBool
static bool Example_Bool(bool value) { return value; }

// Bridge: WNB_ReadFloatParameter, WNB_ReturnFloat
static float Example_Float(float value) { return value; }

// Bridge: WNB_ReadStringParameter into a WNB_String, a UTF-8 to UTF-16 conversion (ReadWideString, 20 lines),
// WNB_ReturnString with the wide text. A std::string parameter arrives as UTF-8 and goes back as UTF-8.
static std::string Example_String(std::string value) { return value; }

// Bridge: WNB_ReadNameParameter, WNB_ReturnName
static tw3se::name Example_Name(tw3se::name value) { return value; }

TW3SE_PLUGIN("FromBridge", "YourName", 0x00010000, /*minInterface*/ 21)  // 21: string and name returns

// Bridge: RegisterAll() with WNB_GetApiVersion(), a table of {name, function} and WNB_RegisterNative for each.
// The parameter count comes from each function's signature.
TW3SE_EXPORT int TW3SE_PluginLoad(const TW3SE_Interface* api) {
    tw3se::init(api);
    tw3se::register_native<&Example_Int>("Example_Int");
    tw3se::register_native<&Example_Bool>("Example_Bool");
    tw3se::register_native<&Example_Float>("Example_Float");
    tw3se::register_native<&Example_String>("Example_String");
    tw3se::register_native<&Example_Name>("Example_Name");
    api->Log("FromBridge", "5 example natives registered");
    return 1;
}
