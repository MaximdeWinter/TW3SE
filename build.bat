@echo off
rem Builds TW3SE into build\:
rem   build\dinput8.dll                  loader (goes to <game>\bin\x64_dx12\)
rem   build\tw3se\tw3se.dll              core   (goes to <game>\bin\x64_dx12\tw3se\)
rem   build\tw3se\plugins\example.dll    example plugin
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d %~dp0
if not exist build\obj\loader mkdir build\obj\loader
if not exist build\obj\core mkdir build\obj\core
if not exist build\obj\example mkdir build\obj\example
if not exist build\obj\template mkdir build\obj\template
if not exist build\obj\lua mkdir build\obj\lua
if not exist build\tw3se\plugins mkdir build\tw3se\plugins
rem /EHa: when a guard takes a fault, the C++ objects in the frames it leaves are destroyed too (a lock is released,
rem a string freed). With /EHsc they were skipped, and a fault under a held lock left it held.
rem /d1trimfile: __FILE__ in an assert names the file from the repository folder on, the PDB path is the
rem file name alone (/PDBALTPATH)
set TRIM=/d1trimfile:%~dp0
set CFLAGS=/nologo %TRIM% /O2 /MT /LD /EHa /W3 /std:c++17 /Iinclude /Ideps\minhook\include /Isrc\core /Ideps\lua\src

rem Lua 5.4 (deps\lua, MIT), compiled as C++ so a Lua error unwinds C++ frames (src\core\lua.cpp)
set L=deps\lua\src
cl /nologo %TRIM% /O2 /MT /EHsc /TP /w /c /Fobuild\obj\lua\ %L%\lapi.c %L%\lcode.c %L%\lctype.c %L%\ldebug.c %L%\ldo.c %L%\ldump.c %L%\lfunc.c %L%\lgc.c %L%\llex.c %L%\lmem.c %L%\lobject.c %L%\lopcodes.c %L%\lparser.c %L%\lstate.c %L%\lstring.c %L%\ltable.c %L%\ltm.c %L%\lundump.c %L%\lvm.c %L%\lzio.c %L%\lauxlib.c %L%\lbaselib.c %L%\lcorolib.c %L%\ldblib.c %L%\liolib.c %L%\lmathlib.c %L%\loadlib.c %L%\loslib.c %L%\lstrlib.c %L%\ltablib.c %L%\lutf8lib.c %L%\linit.c || exit /b 1
rem version data in each DLL (src\version.rc; DLLs without it look like malware to virus scanners)
if not exist build\obj\res mkdir build\obj\res
rc /nologo /Iinclude /d TW3SE_FILE=1 /fo build\obj\res\loader.res src\version.rc || exit /b 1
rc /nologo /Iinclude /d TW3SE_FILE=2 /fo build\obj\res\core.res src\version.rc || exit /b 1
rc /nologo /Iinclude /d TW3SE_FILE=3 /fo build\obj\res\example.res src\version.rc || exit /b 1
rc /nologo /Iinclude /d TW3SE_FILE=4 /fo build\obj\res\template.res src\version.rc || exit /b 1
rem the loader without the C runtime (with the static runtime, virus scanners take it for a trojan)
cl /nologo %TRIM% /O1 /GS- /Zl /LD /W3 /std:c++17 /Iinclude /Fobuild\obj\loader\ src\loader\dinput8.cpp build\obj\res\loader.res /link /NODEFAULTLIB /ENTRY:DllMain kernel32.lib /OUT:build\dinput8.dll /IMPLIB:build\obj\loader\dinput8.lib || exit /b 1
rem tw3se.asi: TW3SE's way in under another mod's ASI loader (src\asi), and the forwarders that stand in for Witcher
rem Native Bridge (src\compat), both without the C runtime like the loader
if not exist build\obj\asi mkdir build\obj\asi
if not exist build\obj\compat mkdir build\obj\compat
if not exist build\tw3se\compat mkdir build\tw3se\compat
rc /nologo /Iinclude /d TW3SE_FILE=5 /fo build\obj\res\compat.res src\version.rc || exit /b 1
rc /nologo /Iinclude /d TW3SE_FILE=6 /fo build\obj\res\asi.res src\version.rc || exit /b 1
cl /nologo %TRIM% /O1 /GS- /Zl /LD /W3 /std:c++17 /Fobuild\obj\asi\ src\asi\tw3se_asi.cpp build\obj\res\asi.res /link /NODEFAULTLIB /ENTRY:DllMain kernel32.lib /OUT:build\!tw3se.asi /IMPLIB:build\obj\asi\tw3se_asi.lib || exit /b 1
cl /nologo %TRIM% /O1 /GS- /Zl /LD /W3 /std:c++17 /Fobuild\obj\compat\ src\compat\wnb.cpp build\obj\res\compat.res /link /NODEFAULTLIB /ENTRY:DllMain kernel32.lib /OUT:build\tw3se\compat\WitcherNativeBridge.asi /IMPLIB:build\obj\compat\wnb.lib || exit /b 1
rem Dear ImGui 1.91.9b (deps\imgui, MIT): the in-game UI (src\core\ui.cpp). Every file that includes imgui.h gets
rem the same IMGUI_USER_CONFIG (src\core\imconfig_tw3se.h), so ImGui's structures match between them.
if not exist build\obj\imgui mkdir build\obj\imgui
set IMGUI=/Ideps\imgui /DIMGUI_USER_CONFIG=\"imconfig_tw3se.h\"
cl /nologo %TRIM% /O2 /MT /EHsc /w /c /std:c++17 /Isrc\core %IMGUI% /Fobuild\obj\imgui\ deps\imgui\imgui.cpp deps\imgui\imgui_draw.cpp deps\imgui\imgui_tables.cpp deps\imgui\imgui_widgets.cpp deps\imgui\imgui_impl_dx12.cpp deps\imgui\imgui_impl_win32.cpp || exit /b 1
rem stb_vorbis 1.22 (deps\stb, public domain or MIT): OGG files for the sounds (src\core\sound.cpp)
if not exist build\obj\stb mkdir build\obj\stb
cl /nologo %TRIM% /O2 /MT /w /c /DSTB_VORBIS_NO_STDIO /Fobuild\obj\stb\ deps\stb\stb_vorbis.c || exit /b 1
rem /Zi + /DEBUG: tw3se.pdb in build\obj\core (outside the release folder), so the watchdog names our functions
rem A game that runs this build holds tw3se.pdb open (its symbols name TW3SE's functions in reports), and the
rem linker cannot write it then. A file in use can still be renamed: the old one moves aside, the next build
rem deletes it once nothing holds it.
del /q build\obj\core\*.pdb.old >nul 2>nul
if exist build\obj\core\tw3se.pdb ren build\obj\core\tw3se.pdb tw3se.%RANDOM%.pdb.old >nul 2>nul
rem the stubs of w3.engine_hook (src\core\enghook.asm), assembled apart: the core's *.obj are the C++ files only
if not exist build\obj\asm mkdir build\obj\asm
ml64 /nologo /c /Fobuild\obj\asm\enghook.obj src\core\enghook.asm >nul || exit /b 1
cl %CFLAGS% %IMGUI% /Zi /Fdbuild\obj\core\ /Fobuild\obj\core\ src\core\*.cpp deps\minhook\src\buffer.c deps\minhook\src\hook.c deps\minhook\src\trampoline.c deps\minhook\src\hde\hde64.c build\obj\lua\*.obj build\obj\imgui\*.obj build\obj\stb\*.obj build\obj\asm\enghook.obj build\obj\res\core.res /link /DEBUG /OPT:REF /OPT:ICF /PDB:build\obj\core\tw3se.pdb /PDBALTPATH:tw3se.pdb /OUT:build\tw3se\tw3se.dll /IMPLIB:build\obj\core\tw3se.lib || exit /b 1
cl %CFLAGS% /Fobuild\obj\example\ plugins\example\example.cpp build\obj\res\example.res /link /OUT:build\tw3se\plugins\example.dll /IMPLIB:build\obj\example\example.lib || exit /b 1
cl %CFLAGS% /Fobuild\obj\template\ plugins\template\template.cpp build\obj\res\template.res /link /OUT:build\tw3se\plugins\template.dll /IMPLIB:build\obj\template\template.lib || exit /b 1
rem the ImGui example (a menu of its own, natives with strings and names): TW3SE's ImGui from the SDK folder
if not exist build\obj\imguiex mkdir build\obj\imguiex
cl %CFLAGS% /w /Fobuild\obj\imguiex\ plugins\imgui_example\imgui_example.cpp include\tw3se\imgui\imgui.cpp include\tw3se\imgui\imgui_draw.cpp include\tw3se\imgui\imgui_tables.cpp include\tw3se\imgui\imgui_widgets.cpp /link /OUT:build\tw3se\plugins\imgui_example.dll /IMPLIB:build\obj\imguiex\imgui_example.lib || exit /b 1
rem an .asi mod that uses TW3SE without being a plugin (tw3se_asi.h)
if not exist build\obj\asiex mkdir build\obj\asiex
cl %CFLAGS% /Fobuild\obj\asiex\ plugins\asi_example\asi_example.cpp /link /OUT:build\TW3SEAsiExample.asi /IMPLIB:build\obj\asiex\asi_example.lib || exit /b 1
rem the render API test (a root constant buffer on the game's main root signature), not part of a release
if not exist build\obj\rendertest mkdir build\obj\rendertest
cl %CFLAGS% /Fobuild\obj\rendertest\ plugins\render_test\render_test.cpp /link d3d12.lib /OUT:build\tw3se\plugins\render_test.dll /IMPLIB:build\obj\rendertest\render_test.lib || exit /b 1
rem the render example (data of its own in the game's shaders), in the SDK
if not exist build\obj\renderex mkdir build\obj\renderex
cl %CFLAGS% /Fobuild\obj\renderex\ plugins\render_example\render_example.cpp /link d3d12.lib /OUT:build\tw3se\plugins\render_example.dll /IMPLIB:build\obj\renderex\render_example.lib || exit /b 1
rem Witcher Native Bridge's example on TW3SE (SDK example\from_bridge.cpp): built here so it keeps compiling
if not exist build\obj\frombridge mkdir build\obj\frombridge
cl %CFLAGS% /Fobuild\obj\frombridge\ plugins\from_bridge\from_bridge.cpp /link /OUT:build\obj\frombridge\from_bridge.dll /IMPLIB:build\obj\frombridge\from_bridge.lib || exit /b 1
rem the plugin interface test (apifuzz), not part of a release
if not exist build\obj\apitest mkdir build\obj\apitest
cl %CFLAGS% /Fobuild\obj\apitest\ plugins\apitest\apitest.cpp /link /OUT:build\tw3se\plugins\apitest.dll /IMPLIB:build\obj\apitest\apitest.lib || exit /b 1
rem the skinning shader hook test (tools/skin_test.py), not part of a release
if not exist build\obj\skinhook_test mkdir build\obj\skinhook_test
cl %CFLAGS% /Fobuild\obj\skinhook_test\ plugins\skinhook_test\skinhook_test.cpp /link /OUT:build\tw3se\plugins\skinhook_test.dll /IMPLIB:build\obj\skinhook_test\skinhook_test.lib || exit /b 1
rem the live mesh test (interface 26), not part of a release
if not exist build\obj\livemesh_test mkdir build\obj\livemesh_test
cl %CFLAGS% /Fobuild\obj\livemesh_test\ plugins\livemesh_test\livemesh_test.cpp /link /OUT:build\tw3se\plugins\livemesh_test.dll /IMPLIB:build\obj\livemesh_test\livemesh_test.lib || exit /b 1
rem the crash resistance test (tools/faulttest.py), not part of a release: a native that faults, a plugin whose
rem load faults
if not exist build\obj\faulttest mkdir build\obj\faulttest
if not exist build\tests mkdir build\tests
cl %CFLAGS% /Fobuild\obj\faulttest\ plugins\faulttest\faulttest.cpp /link /OUT:build\tests\faulttest.dll /IMPLIB:build\obj\faulttest\faulttest.lib || exit /b 1
cl %CFLAGS% /DCRASH_IN_LOAD /Fobuild\obj\faulttest\ plugins\faulttest\faulttest.cpp /link /OUT:build\tests\faulttest_load.dll /IMPLIB:build\obj\faulttest\faulttest_load.lib || exit /b 1
echo built
