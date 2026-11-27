@echo off
rem Builds TW3SE and assembles a drop-in release + a plugin SDK under build\release\.
rem   TW3SE\   what a player copies into the game folder, with the manual
rem   TW3SE-SDK\           headers, the example plugins, the VS template and the manual, for plugin authors
rem The engine notes (docs\engine) and the working notes stay in the repository.
call "%~dp0build.bat" || exit /b 1
cd /d %~dp0

set REL=build\release
set GAME=%REL%\TW3SE
set SDK=%REL%\TW3SE-SDK

rem --- drop-in: copy the two files into <game>\bin\x64_dx12\ (plus the example plugin) ---
if exist "%REL%" rmdir /s /q "%REL%"
mkdir "%GAME%\bin\x64_dx12\tw3se\plugins" 2>nul
copy /y build\dinput8.dll "%GAME%\bin\x64_dx12\dinput8.dll" >nul
copy /y build\tw3se\tw3se.dll "%GAME%\bin\x64_dx12\tw3se\tw3se.dll" >nul
copy /y build\tw3se\plugins\example.dll "%GAME%\bin\x64_dx12\tw3se\plugins\example.dll" >nul
rem !tw3se.asi: TW3SE starts under another mod's ASI loader too; compat: Witcher Native Bridge's functions for its mods (1.2)
copy /y "build\!tw3se.asi" "%GAME%\bin\x64_dx12\!tw3se.asi" >nul
xcopy /e /i /y build\tw3se\compat "%GAME%\bin\x64_dx12\tw3se\compat" >nul
copy /y docs\release\README.md "%GAME%\README.md" >nul
copy /y CHANGELOG.md "%GAME%\CHANGELOG.md" >nul
xcopy /e /i /y docs\manual "%GAME%\docs\manual" >nul
copy /y docs\LUA.md "%GAME%\docs\LUA.md" >nul
rem every w3. function as a web page with a search field (from lua\types\w3api.lua)
python tools\gen_api_html.py --out "%GAME%\docs\api.html" >nul || exit /b 1
rem Lua: the helper library, the examples and the tests (`luatest`); autorun\ and mods\ stay the player's
xcopy /e /i /y lua\lib "%GAME%\bin\x64_dx12\tw3se\lua\lib" >nul
xcopy /e /i /y lua\examples "%GAME%\bin\x64_dx12\tw3se\lua\examples" >nul
xcopy /e /i /y lua\tests "%GAME%\bin\x64_dx12\tw3se\lua\tests" >nul
rem the w3.* types for the editor: a mod folder in Mods\<modName>\tw3se finds them at ..\..\..\bin\x64_dx12\tw3se\lua\types
mkdir "%GAME%\bin\x64_dx12\tw3se\lua\types" 2>nul
copy /y lua\types\w3api.lua "%GAME%\bin\x64_dx12\tw3se\lua\types\w3api.lua" >nul
rem the game's outline shader with a channel for w3.highlight's colour of its own (tools/focus_patch.py)
mkdir "%GAME%\bin\x64_dx12\tw3se\shaders" 2>nul
copy /y shaders\ec02b0473a2af8a4.cso "%GAME%\bin\x64_dx12\tw3se\shaders\ec02b0473a2af8a4.cso" >nul || exit /b 1

rem --- SDK for plugin authors: headers + examples + the pipe tool + the manual ---
mkdir "%SDK%\include\tw3se" 2>nul
mkdir "%SDK%\example" 2>nul
mkdir "%SDK%\tools" 2>nul
rem the manual's commands run through tools\tw3se.py (live reload, lua, luatest)
copy /y tools\tw3se.py "%SDK%\tools\tw3se.py" >nul
copy /y tools\catalog.py "%SDK%\tools\catalog.py" >nul
copy /y tools\texcache.py "%SDK%\tools\texcache.py" >nul
copy /y tools\soundcache.py "%SDK%\tools\soundcache.py" >nul
xcopy /i /y tools\cr2w\*.py "%SDK%\tools\cr2w\" >nul
rem the WitcherScript debugger for VS Code (docs\manual\debugger.md); its tests stay in the repository
mkdir "%SDK%\tools\vscode-witcherscript" 2>nul
copy /y tools\vscode-witcherscript\package.json "%SDK%\tools\vscode-witcherscript\package.json" >nul
copy /y tools\vscode-witcherscript\extension.js "%SDK%\tools\vscode-witcherscript\extension.js" >nul
copy /y tools\vscode-witcherscript\adapter.js "%SDK%\tools\vscode-witcherscript\adapter.js" >nul
copy /y include\tw3se\tw3se.h "%SDK%\include\tw3se\tw3se.h" >nul
copy /y include\tw3se\tw3se.hpp "%SDK%\include\tw3se\tw3se.hpp" >nul
rem 1.2: an .asi mod using TW3SE, ImGui windows of your own (with the ImGui sources TW3SE runs)
copy /y include\tw3se\tw3se_asi.h "%SDK%\include\tw3se\tw3se_asi.h" >nul
copy /y include\tw3se\tw3se_imgui.h "%SDK%\include\tw3se\tw3se_imgui.h" >nul
xcopy /e /i /y include\tw3se\imgui "%SDK%\include\tw3se\imgui" >nul
copy /y plugins\imgui_example\imgui_example.cpp "%SDK%\example\imgui_example.cpp" >nul
copy /y plugins\asi_example\asi_example.cpp "%SDK%\example\asi_example.cpp" >nul
copy /y plugins\render_example\render_example.cpp "%SDK%\example\render_example.cpp" >nul
rem a Witcher Native Bridge mod moved to TW3SE: the bridge's own example, its script unchanged
copy /y plugins\from_bridge\from_bridge.cpp "%SDK%\example\from_bridge.cpp" >nul
copy /y plugins\from_bridge\from_bridge.ws "%SDK%\example\from_bridge.ws" >nul
rem a Visual Studio 2022 project to start from (MyPlugin.sln: a script function, an ImGui menu, a command)
mkdir "%SDK%\vs-template\src" 2>nul
copy /y sdk\vs-template\MyPlugin.sln "%SDK%\vs-template\MyPlugin.sln" >nul
copy /y sdk\vs-template\MyPlugin.vcxproj "%SDK%\vs-template\MyPlugin.vcxproj" >nul
copy /y sdk\vs-template\src\plugin.cpp "%SDK%\vs-template\src\plugin.cpp" >nul
copy /y plugins\example\example.cpp "%SDK%\example\example.cpp" >nul
copy /y plugins\template\template.cpp "%SDK%\example\template.cpp" >nul
copy /y scripts\tw3se_stdlib_test.ws "%SDK%\example\natives_demo.ws" >nul
rem (no .bat in the SDK: Nexus quarantines it; the build command is in docs\manual\first-plugin.md)
copy /y docs\release\README.md "%SDK%\README.md" >nul
copy /y CHANGELOG.md "%SDK%\CHANGELOG.md" >nul
xcopy /e /i /y docs\manual "%SDK%\docs\manual" >nul
copy /y docs\LUA.md "%SDK%\docs\LUA.md" >nul
copy /y docs\SDK.md "%SDK%\docs\SDK.md" >nul
copy /y "%GAME%\docs\api.html" "%SDK%\docs\api.html" >nul
rem Lua mods: the template (docs\manual\first-lua-mod.md), the types of w3.* and of the game's classes, their generator
xcopy /e /i /y lua\template "%SDK%\lua-mod-template" >nul
mkdir "%SDK%\types" 2>nul
copy /y lua\types\w3api.lua "%SDK%\types\w3api.lua" >nul
if exist lua\types\w3.lua copy /y lua\types\w3.lua "%SDK%\types\w3.lua" >nul
copy /y tools\gen_types.py "%SDK%\tools\gen_types.py" >nul
copy /y tools\pack_mod.py "%SDK%\tools\pack_mod.py" >nul
copy /y tools\bundle_extract.py "%SDK%\tools\bundle_extract.py" >nul
copy /y tools\to_annotations.py "%SDK%\tools\to_annotations.py" >nul
copy /y tools\enginemap.py "%SDK%\tools\enginemap.py" >nul

echo packaged: %REL%
