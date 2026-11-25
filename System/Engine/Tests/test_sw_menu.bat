@echo off
rem Builds and runs Engine/Tests/test_sw_menu.c (in-game menu input filter).
setlocal
set OUT=%TEMP%\sw_menu_test
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /W4 /Fe:"%OUT%\test_sw_menu.exe" /Fo:"%OUT%\\" test_sw_menu.c ..\Runtime\src\sw_menu.c || exit /b 1
"%OUT%\test_sw_menu.exe"
