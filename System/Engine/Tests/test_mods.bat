@echo off
rem Builds and runs Engine/Tests/test_mods.c: mod support (romfs overlay, exefs patches, cheats) against the
rem fixture in mods_fixture\ - no game files needed.  The fixture is copied to %TEMP% first (the test adds a
rem junction that points out of a mod folder and writes mods.log there).
setlocal
set OUT=%TEMP%\sw_mods_test
if exist "%OUT%\fx" rmdir /s /q "%OUT%\fx"
if not exist "%OUT%" mkdir "%OUT%"
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /W4 /wd4996 /I..\Runtime\include /Fe:"%OUT%\test_mods.exe" /Fo:"%OUT%\\" test_mods.c ..\Runtime\src\sw_mods.c ..\Runtime\src\sw_romfs.c user32.lib || exit /b 1
xcopy /e /i /q /y mods_fixture "%OUT%\fx" >nul || exit /b 1
mklink /J "%OUT%\fx\mods\ModBeta\romfs\bin\escape" "%OUT%\fx\outside" >nul || exit /b 1
"%OUT%\test_mods.exe" "%OUT%\fx" "%OUT%" || exit /b 1
"%OUT%\test_mods.exe" "%OUT%\fx" "%OUT%\nomods_run" nomods || exit /b 1
"%OUT%\test_mods.exe" "%OUT%\fx" "%OUT%\badcfg_run" badcfg || exit /b 1
echo test_mods: OK
