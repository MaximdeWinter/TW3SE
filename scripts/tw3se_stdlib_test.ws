// Verifies the usable native stdlib (0-1 parameter), a multi-parameter native, and a plugin native.
import function TW3SE_GetVersion() : int;
import function TW3SE_WriteFile(text : string) : bool;
import function TW3SE_WriteNamed(fileName : string, text : string) : bool;
import function TW3SE_AppendNamed(fileName : string, text : string) : bool;
import function TW3SE_FileSize(fileName : string) : int;
import function TW3SE_DeleteNamed(fileName : string) : bool;
import function TW3SE_ReadNamed(fileName : string) : string;
import function TW3SE_GetConfig(key : string) : string;
import function TW3SE_SetVar(key : string, value : string) : bool;
import function TW3SE_GetVar(key : string) : string;
import function TW3SE_GetConfigKey(setting : string) : int;
import function TW3SE_KeyJustPressed(vk : int) : bool;
import function ExampleDouble(x : int) : int;

exec function tw3se_stdlib() {
    var okW : bool;
    var okN : bool;
    LogChannel('Cj', "stdlib|version=" + IntToString(TW3SE_GetVersion()));
    okW = TW3SE_WriteFile("hello from witcherscript");
    if (okW) { LogChannel('Cj', "stdlib|write=y"); } else { LogChannel('Cj', "stdlib|write=n"); }
    okN = TW3SE_WriteNamed("named_from_ws.txt", "two params work from witcherscript");
    if (okN) { LogChannel('Cj', "stdlib|named=y"); } else { LogChannel('Cj', "stdlib|named=n"); }
    // string return: read back what WriteNamed just wrote
    LogChannel('Cj', "stdlib|read=" + TW3SE_ReadNamed("named_from_ws.txt"));
    // file round-trip: delete, append two bytes, read size back
    TW3SE_DeleteNamed("append_test.txt");
    TW3SE_AppendNamed("append_test.txt", "xy");
    LogChannel('Cj', "stdlib|size=" + IntToString(TW3SE_FileSize("append_test.txt")));
    LogChannel('Cj', "stdlib|cfg=" + TW3SE_GetConfig("natives"));
    // persistent var store: write a value, read it straight back
    TW3SE_SetVar("demo_state", "quest=started");
    LogChannel('Cj', "stdlib|var=" + TW3SE_GetVar("demo_state"));
    // input: read a rebindable key code from config (hotkey = 0x74 -> 116); edge-detect an unpressed key -> false
    LogChannel('Cj', "stdlib|cfgkey=" + IntToString(TW3SE_GetConfigKey("hotkey")));
    if (TW3SE_KeyJustPressed(123)) { LogChannel('Cj', "stdlib|edge=y"); } else { LogChannel('Cj', "stdlib|edge=n"); }
    LogChannel('Cj', "stdlib|pluginDouble=" + IntToString(ExampleDouble(21)));
}
