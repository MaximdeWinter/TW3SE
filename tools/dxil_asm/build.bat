@echo off
rem Builds dxil_asm.exe and copies dxcompiler.dll + dxil.dll from DXC 1.9 next to it (the game's shaders are
rem validator 1.9; the Windows SDK's DXC is 1.7). DXC: github.com/microsoft/DirectXShaderCompiler/releases,
rem unpacked to _cache\dxc19.
setlocal
set SDKBIN=%~dp0..\..\_cache\dxc19\bin\x64
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d %~dp0
cl /nologo /O2 /EHsc /std:c++17 dxil_asm.cpp /Fe:dxil_asm.exe /Fo:dxil_asm.obj || exit /b 1
copy /y "%SDKBIN%\dxcompiler.dll" . >nul
copy /y "%SDKBIN%\dxil.dll" . >nul
del dxil_asm.obj
