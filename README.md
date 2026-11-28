# TW3SE (The Witcher 3 Script Extender)

TW3SE is a tool for mods. It adds new script functions and features to The Witcher 3 Remastered (DirectX 12) and to
the REDkit editor.

Players get it on Nexus Mods: https://www.nexusmods.com/witcher3/mods/13837

## Build

What is needed:

- Visual Studio 2022 Build Tools with the C++ x64 tools (`build.bat` calls their `vcvars64.bat`)
- Python 3 (for `package.bat` and the tools)

```
build.bat                     the DLLs, in build\
package.bat                   build.bat, then the files of both zips in build\release
python tools\nexus_zip.py     the two zips in build\nexus\<version>
```

## What is where

- `src\core`: TW3SE itself (`tw3se.dll`)
- `src\loader`, `src\asi`: the loader (`dinput8.dll`) and the start under another ASI loader (`!tw3se.asi`)
- `src\compat`: `WitcherNativeBridge.asi` for mods made for Witcher Native Bridge
- `include\tw3se`: the plugin API
- `lua`: the Lua library, examples, the mod template and the tests
- `plugins`: example and test plugins
- `docs\manual`, `docs\LUA.md`: the manual
- `crashcatch`: crashcatch, the crash reporter for players (`crashcatch\README.md`)

## crashcatch

crashcatch writes a report with minidumps when The Witcher 3 crashes. Players start it before the game and send the
zip it leaves. The ready program is under Releases.

```
crashcatch\build.bat          crashcatch\build\crashcatch.exe
```

## License

PolyForm Noncommercial 1.0.0 (`LICENSE`): use, change and share it for noncommercial purposes. A free mod that lists TW3SE as a
requirement is fine. The code in `deps` and `include\tw3se\imgui` keeps its own licenses
(`deps\THIRD_PARTY_NOTICES.txt`).

## Credits

- MaximdeWinter: idea, design, testing in the game and direction
- Tenour: the pose and mimic hooks and the live mesh deformation (blend shapes)
- Code written together with Claude
