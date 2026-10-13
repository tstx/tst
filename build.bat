@echo off
rem Kompiliert main.cpp mit MSVC (Visual Studio 2019) nach build\main.exe und startet es.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (echo vcvars64.bat nicht gefunden & exit /b 1)
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /EHsc /W4 /Zi main.cpp /Fe:build\main.exe /Fo:build\ /Fd:build\ || exit /b 1
build\main.exe

