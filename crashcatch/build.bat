@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d %~dp0
if not exist build\obj mkdir build\obj
rc /nologo /fo build\obj\crashcatch.res src\crashcatch.rc || exit /b 1
cl /nologo /d1trimfile:%~dp0 /O2 /MT /EHsc /W4 /std:c++17 /DUNICODE /D_UNICODE /Fobuild\obj\ src\crashcatch.cpp build\obj\crashcatch.res /link /OUT:build\crashcatch.exe /SUBSYSTEM:CONSOLE || exit /b 1
echo built build\crashcatch.exe
