# Script natives

New WitcherScript functions. Put the `import function` line at the top of your script, then call the function.
Several mods can import the same function.

## All functions

```
import function TW3SE_GetVersion() : int;                        // 66048 = 1.2.0 ((major<<16)|(minor<<8)|patch)
import function TW3SE_Log(text : string);                        // one line in tw3se.log

import function TW3SE_WriteFile(text : string) : bool;           // write tw3se\script_out.txt
import function TW3SE_WriteNamed(fileName : string, text : string) : bool;   // write tw3se\out\<name>
import function TW3SE_AppendNamed(fileName : string, text : string) : bool;  // append to tw3se\out\<name>
import function TW3SE_ReadNamed(fileName : string) : string;     // read tw3se\out\<name> ("" if missing)
import function TW3SE_FileSize(fileName : string) : int;         // size of tw3se\out\<name> (-1 if missing)
import function TW3SE_DeleteNamed(fileName : string) : bool;     // delete tw3se\out\<name>
import function TW3SE_FileExists(fileName : string) : bool;      // is tw3se\<fileName> there
import function TW3SE_GetConfig(key : string) : string;          // a value from tw3se.config

import function TW3SE_SetVar(key : string, value : string) : bool;  // store a value outside the save
import function TW3SE_GetVar(key : string) : string;             // read it ("" if unset)
import function TW3SE_HasVar(key : string) : bool;               // is it set
import function TW3SE_DeleteVar(key : string) : bool;            // remove it

import function TW3SE_KeyPressed(vk : int) : bool;               // key is down
import function TW3SE_KeyJustPressed(vk : int) : bool;           // key went down this frame
import function TW3SE_GetConfigKey(setting : string) : int;      // a key code from tw3se.config

import function TW3SE_EntityGuid(e : CEntity) : string;          // GUID of an object ("d0 d1 d2 d3")
import function TW3SE_EntityByGuid(guid : string) : CEntity;     // object by GUID (while loaded)
import function TW3SE_EntityEffects(e : CEntity) : string;       // its effect names ("fire,smoke")
import function TW3SE_EntityTemplate(e : CEntity) : CEntityTemplate;  // its template
import function TW3SE_HideEntity(e : CEntity, hide : bool) : bool;    // hide or show it
import function TW3SE_WorldEdit(line : string) : bool;           // one world edit line, right away
import function TW3SE_Emit(text : string);                       // one line for an external tool

import function TW3SE_Lua(code : string) : string;               // run Lua, get its output back
import function TW3SE_OnScriptsReloaded(obj : IScriptable, eventName : name);  // obj.eventName() after each live reload
import function TW3SE_MountDLC(path : string) : bool;            // mount a DLC folder while the game runs

import function TW3SE_PlaySound(file : string, optional entity : CEntity, optional bone : name,
                                optional volume : float, optional loop : bool) : int;   // play a WAV or OGG file
import function TW3SE_StopSound(sound : int) : bool;             // stop it
import function TW3SE_SetSoundVolume(sound : int, volume : float) : bool;
import function TW3SE_SoundPlaying(sound : int) : bool;          // still playing

import function TW3SE_FoliagePlant(tree : CResource, x : float, y : float, z : float, yaw : float, scale : float,
                                   half : float, height : float) : bool;   // plant a tree
import function TW3SE_FoliageRemoveAt(tree : CResource, x : float, y : float, z : float, radius : float) : bool;

import function TW3SE_GetInt(o : IScriptable, field : string) : int;        // a field by name
import function TW3SE_GetFloat(o : IScriptable, field : string) : float;
import function TW3SE_GetBool(o : IScriptable, field : string) : bool;
import function TW3SE_GetString(o : IScriptable, field : string) : string;  // any field as text
import function TW3SE_GetObject(o : IScriptable, field : string) : IScriptable;
import function TW3SE_SetInt(o : IScriptable, field : string, value : int) : bool;
import function TW3SE_SetFloat(o : IScriptable, field : string, value : float) : bool;
import function TW3SE_SetBool(o : IScriptable, field : string, value : bool) : bool;
import function TW3SE_SetString(o : IScriptable, field : string, value : string) : bool;   // text, as the field's type
import function TW3SE_SetObject(o : IScriptable, field : string, value : IScriptable) : bool;
import function TW3SE_HasField(o : IScriptable, field : string) : bool;
import function TW3SE_HasMethod(o : IScriptable, method : string) : bool;
import function TW3SE_Call(o : IScriptable, method : string, optional a1 : string, optional a2 : string,
                           optional a3 : string, optional a4 : string, optional a5 : string, optional a6 : string) : string;
import function TW3SE_CallObject(o : IScriptable, method : string, optional a1 : string, optional a2 : string,
                                 optional a3 : string, optional a4 : string, optional a5 : string, optional a6 : string) : IScriptable;
import function TW3SE_ClassOf(o : IScriptable) : string;
import function TW3SE_ClassParent(className : string) : string;
import function TW3SE_Fields(className : string) : string;     // "name:type,name:type,..."
import function TW3SE_Methods(className : string) : string;    // "name,name,..."
import function TW3SE_EnumValue(enumName : string, valueName : string) : int;
import function TW3SE_EnumName(enumName : string, value : int) : string;
import function TW3SE_HookEvent(className : string, method : string, listener : IScriptable, eventName : name) : int;
import function TW3SE_Unhook(id : int) : bool;
import function TW3SE_LastError() : string;                    // the reason the last of these functions failed
import function TW3SE_AllObjects(className : string) : int;    // every live object of a class: how many
import function TW3SE_ObjectAt(index : int) : IScriptable;     // one of them, from 0

import function TW3SE_StringToName(value : string) : name;       // the name of a text ('None' for "")

import function TW3SE_Send(channel : string, message : string);  // a message to the channel's listeners
import function TW3SE_Receive(channel : string) : string;        // the channel's next message ("" when none)
import function TW3SE_Pending(channel : string) : int;           // how many messages of the channel wait

import function TW3SE_SavedSet(mod : string, key : string, value : string) : bool;   // a value that goes with the save
import function TW3SE_SavedGet(mod : string, key : string) : string;                 // read it ("" if unset)
import function TW3SE_SavedHas(mod : string, key : string) : bool;
import function TW3SE_SavedRemove(mod : string, key : string) : bool;

import function TW3SE_HttpRequest(method : string, url : string, body : string, channel : string) : bool;  // the answer comes on the channel
```

## Files

The file functions work inside `tw3se\out\`. Names with a path, `..`, the characters `<>:"|?*` or a Windows device
name (`CON`, `NUL`, `COM1` ...) are refused, and so are names over 200 characters. `ReadNamed` reads up to 1 MiB.

```
TW3SE_DeleteNamed("run.log");
TW3SE_AppendNamed("run.log", "started\n");
LogChannel('MyMod', "size=" + IntToString(TW3SE_FileSize("run.log")));
LogChannel('MyMod', "text=" + TW3SE_ReadNamed("run.log"));
```

## Variables outside the save

A key/value store in `tw3se\vars.txt`, kept between sessions. `GetVar` sees a change right away. The file is
written once a second at most and when the game closes. Values are strings, use `IntToString` / `StringToInt` for
numbers. A value holds up to 64 KiB, all values together 16 MiB; `SetVar` returns false above that.

```
var n : int;
n = StringToInt(TW3SE_GetVar("times_entered"), 0);   // 0 the first time
TW3SE_SetVar("times_entered", IntToString(n + 1));
```

## Hotkeys

`TW3SE_KeyPressed` is true while the key is held. `TW3SE_KeyJustPressed` is true once per press (call it once per
frame). `TW3SE_GetConfigKey` reads a key code from `tw3se.config`, so the player can rebind it there.

```
// tw3se.config:  teleport_key = 0x74      (F5)
var key : int;
key = TW3SE_GetConfigKey("teleport_key");
if (key != 0 && TW3SE_KeyJustPressed(key)) { /* once per press */ }
```

## Objects by GUID

`TW3SE_EntityByGuid` gives you any loaded object by its GUID (from
`TW3SE_EntityGuid` or `worldedit_log`). It returns NULL while the object is unloaded. Everything CEntity has works
on it.

```
var lamp : CEntity;
lamp = TW3SE_EntityByGuid("bd5160b6 45e69f5e cf66a598 084ca441");   // a street lamp in Novigrad
if (lamp) {
    lamp.StopEffect('fire');
}
```

`TW3SE_HideEntity(e, true)` hides the object with its effects, light, sound and collision.
`TW3SE_HideEntity(e, false)` shows it again with the same effects.

`TW3SE_WorldEdit` applies one line of [world.txt](world-edit.md) right away, for this session. For edits that
should stay after a load, use the mod's `tw3se\world.txt`.

```
TW3SE_WorldEdit("remove bd5160b6 45e69f5e cf66a598 084ca441");
```

## Effects and appearances

`TW3SE_EntityEffects` lists the effects of an object (the names `PlayEffect` and `StopEffect` take).
`TW3SE_EntityTemplate` gives its template. With the game's `GetAppearanceNames` you get its appearances.

```
var names : array<name>;
var tmpl : CEntityTemplate;
LogChannel('MyMod', TW3SE_EntityEffects(barrel));     // "fire,smoke_light"
tmpl = TW3SE_EntityTemplate(chest);
if (tmpl) {
    GetAppearanceNames(tmpl, names);                  // 1_full, 2_empty
}
```

## Trees

`TW3SE_FoliagePlant` draws a tree of the foliage at a position: `yaw` in degrees, `scale` 1.0 for its normal size,
`half` and `height` the size of its box in metres (half the width, the height). `TW3SE_FoliageRemoveAt` takes out
the trees of that kind within `radius` metres (at most 50) of a point. A planted tree is visual only (the player walks
through it) and stays for this session.

```
var tree : CResource;
tree = LoadResource("environment\vegetation\trees\abies\abies_common_bigest.srt", true);
TW3SE_FoliagePlant(tree, 120.0, -45.0, 12.5, 30.0, 1.0, 4.0, 18.0);
```

A newly planted tree needs about a third of a second before `TW3SE_FoliageRemoveAt` finds it.

## Lua and DLC

`TW3SE_Lua(code)` runs Lua code (see [Lua](../LUA.md)) and returns what it printed or returned, as a string.

`TW3SE_MountDLC(path)` mounts a DLC folder while the game runs. Its quests start after the next save load.

`TW3SE_OnScriptsReloaded(obj, 'OnMyReload')` makes TW3SE call `obj.OnMyReload()` after every live reload (see
[Live reload](live-reload.md)). It needs Lua (on by default).

## Sounds

`TW3SE_PlaySound` plays a WAV or OGG file and returns a number for the other three functions. The file is an
absolute path or a path from the game folder (`mods\modMyMod\sounds\door.ogg`). With an entity the sound is in 3D at
it, or at one of its bones, and you hear it from the camera. Called with the file alone, it plays flat. A volume of 1
(or none) plays the file as it is, `loop` repeats it until `TW3SE_StopSound`. A sound at an entity stops when the
entity is gone.

```
var s : int;
s = TW3SE_PlaySound("mods\modMyMod\sounds\hum.ogg", thePlayer, 'head', 0.8, true);
TW3SE_StopSound(s);
```

## Fields, calls and hooks by name

These functions reach fields and methods by a name known at run time, list a class's fields and methods, and hook any
function. They need Lua (on by default).

```
var level : int;
level = TW3SE_GetInt(thePlayer, "approachAttack");
TW3SE_SetString(thePlayer, "equippedSign", "ST_Igni");       // an enum by its name
TW3SE_SetString(thePlayer, "MOUNT_DISTANCE_CBT", "4");      // the text becomes a float
```

`TW3SE_GetString` gives any field as text: a number, `true`/`false`, a name, an enum value by its name, a `Vector` as
`"x y z w"`, an array as a list joined by commas. `TW3SE_SetString` turns text into the field's type the same way.

`TW3SE_Call` calls a method by name (`NULL` for a global function) with up to six arguments as text. Each argument
becomes its parameter's type, `"3 4 0"` a `Vector`. The return comes back as text; `TW3SE_CallObject` gives an object.
An empty text at the end leaves the argument out.

```
TW3SE_Call(NULL, "VecDistance", "0 0 0", "3 4 0");          // "5"
TW3SE_Call(thePlayer, "GetWorldPosition");                   // "-62.3 182.9 3.1 1"
```

`TW3SE_HookEvent` calls an event of your object after each call of a function. The event gets the object the
function ran on (`NULL` for a global function). `className` "" hooks a global function. `TW3SE_Unhook` takes the
hook out with the number `TW3SE_HookEvent` returned.

```
class MyListener extends CObject {
    event OnNameAsked(o : IScriptable) { TW3SE_Log("GetName on " + TW3SE_ClassOf(o)); }
}
listener = new MyListener in theGame;
id = TW3SE_HookEvent("CR4Player", "GetName", listener, 'OnNameAsked');
```

`TW3SE_AllObjects` finds every live object of a class and its subclasses, spawned ones, managers and
quest objects too, and returns how many there are. `TW3SE_ObjectAt` gives them one by one. The list holds its objects
until the next `TW3SE_AllObjects`.

```
var i, n : int;
n = TW3SE_AllObjects("CNewNPC");
for (i = 0; i < n; i += 1) {
    TW3SE_Log(((CNewNPC)TW3SE_ObjectAt(i)).GetDisplayName());
}
```

When one of these functions returns 0, "", NULL or false, `TW3SE_LastError()` gives the reason: a missing field, text
of the wrong type, a method the object lacks.

## External tools

`TW3SE_Emit` sends one line to a tool that reads the pipe's `events` command. Works in a normal game start. The
tool sends commands back with `exec` (see [Console commands](console-commands.md)).

```
exec function mymod_where() {
    TW3SE_Emit("where|" + VecToString(thePlayer.GetWorldPosition()));
}
```

## Names from text

`TW3SE_StringToName("q001_" + suffix)` gives the name of a text built at run time: a tag, an item, a fact. The game's
scripts have only `NameToString`.

## Messages

A message is a channel name and a text. `TW3SE_Send` hands it to every listener of the channel: C++ plugins, Lua
(`w3.on_message`) and scripts. A script reads the messages of a channel in order with `TW3SE_Receive`, which returns
`""` when none is waiting. The sender gets its own messages too, so two directions use two channels:

```
TW3SE_Send("mymod.cmd", "heal 100");
var reply : string = TW3SE_Receive("mymod.reply");
```

## Values that go with the save

`TW3SE_SavedSet(mod, key, value)` keeps a text with the current save. After a load, `TW3SE_SavedGet` gives the value
of that save; a new game starts empty. The values are in `tw3se\saves\<id>.lua`, beside Lua's `w3.saved`.

## Web requests

`TW3SE_HttpRequest("GET", "https://example.com/data.json", "", "mymod.web")` asks in the background. The answer comes
as a message on the channel: the status, a line break, the body (status 0 and the reason when the request failed):

```
var answer : string = TW3SE_Receive("mymod.web");   // "" until it came
```

## Your own functions

A plugin adds a function with one line:

```cpp
static int Add(int a, int b) { return a + b; }
tw3se::register_native<&Add>("Add");          // in TW3SE_PluginLoad
```

```
import function Add(a : int, b : int) : int;
```

Types: `int`, `float`, `bool`, `string` and `name` (as `tw3se::name`), for parameters and the return. Parameter names in the `import` line have to differ from type words
(`name`, `string`, `int` …). The number of parameters has to match the C++ function.

`natives = off` in `tw3se.config` turns TW3SE's own functions off.
