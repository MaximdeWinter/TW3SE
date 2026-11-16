// The script of Witcher Native Bridge's example (by rejuvenate, MIT license), unchanged: the natives come from from_bridge.dll now.
// In a mod: Mods\modFromBridge\content\scripts\local\from_bridge.ws. In the game's console: example_imports()
import function Example_Int(value : int) : int;
import function Example_Bool(value : bool) : bool;
import function Example_Float(value : float) : float;
import function Example_String(value : string) : string;
import function Example_Name(value : name) : name;

exec function example_imports()
{
    LogChannel('Example_Int', Example_Int(10));
    LogChannel('Example_Bool', Example_Bool(true));
    LogChannel('Example_Float', Example_Float(1.254));
    LogChannel('Example_String', Example_String("hello"));
    LogChannel('Example_Name', Example_Name('test'));
}
