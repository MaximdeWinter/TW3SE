// tw3se\compat\WitcherNativeBridge.asi: no code of its own. Its exports forward to tw3se.dll; the module
// exists so that a mod's import of "WitcherNativeBridge.asi" finds a loaded module of that name.
#include <windows.h>

#pragma comment(linker, "/export:WNB_GetApiVersion=tw3se.WNB_GetApiVersion")
#pragma comment(linker, "/export:WNB_RegisterNative=tw3se.WNB_RegisterNative")
#pragma comment(linker, "/export:WNB_ReadIntParameter=tw3se.WNB_ReadIntParameter")
#pragma comment(linker, "/export:WNB_ReadBoolParameter=tw3se.WNB_ReadBoolParameter")
#pragma comment(linker, "/export:WNB_ReadFloatParameter=tw3se.WNB_ReadFloatParameter")
#pragma comment(linker, "/export:WNB_ReadNameParameter=tw3se.WNB_ReadNameParameter")
#pragma comment(linker, "/export:WNB_ReadStringParameter=tw3se.WNB_ReadStringParameter")
#pragma comment(linker, "/export:WNB_ReturnVoid=tw3se.WNB_ReturnVoid")
#pragma comment(linker, "/export:WNB_ReturnInt=tw3se.WNB_ReturnInt")
#pragma comment(linker, "/export:WNB_ReturnBool=tw3se.WNB_ReturnBool")
#pragma comment(linker, "/export:WNB_ReturnFloat=tw3se.WNB_ReturnFloat")
#pragma comment(linker, "/export:WNB_ReturnString=tw3se.WNB_ReturnString")
#pragma comment(linker, "/export:WNB_ReturnName=tw3se.WNB_ReturnName")
#pragma comment(linker, "/export:WNB_ReturnNameFromString=tw3se.WNB_ReturnNameFromString")

extern "C" BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(self);
    return TRUE;
}
