@echo off
rem builds build\lua.exe (Lua 5.4 from deps\lua) for test\e2e.js
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d %~dp0..\..\..
if not exist build\obj\luaexe mkdir build\obj\luaexe
set L=deps\lua\src
cl /nologo /O2 /MT /w /Fobuild\obj\luaexe\ /Febuild\lua.exe %L%\lua.c %L%\lapi.c %L%\lcode.c %L%\lctype.c %L%\ldebug.c %L%\ldo.c %L%\ldump.c %L%\lfunc.c %L%\lgc.c %L%\llex.c %L%\lmem.c %L%\lobject.c %L%\lopcodes.c %L%\lparser.c %L%\lstate.c %L%\lstring.c %L%\ltable.c %L%\ltm.c %L%\lundump.c %L%\lvm.c %L%\lzio.c %L%\lauxlib.c %L%\lbaselib.c %L%\lcorolib.c %L%\ldblib.c %L%\liolib.c %L%\lmathlib.c %L%\loadlib.c %L%\loslib.c %L%\lstrlib.c %L%\ltablib.c %L%\lutf8lib.c %L%\linit.c
