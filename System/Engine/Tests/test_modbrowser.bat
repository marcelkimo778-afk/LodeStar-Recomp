@echo off
rem Builds and runs Engine/Tests/test_modbrowser.cpp: the mod browser's mock source, install pipeline (safe
rem extraction, layouts, executables refused, zip slip / bomb / links rejected, update, uninstall) and version
rem compatibility - offline, no game.  Fixture: modbrowser_fixture (make_modbrowser_fixture.py).
setlocal
set OUT=%TEMP%\sw_modbrowser_test
if not exist "%OUT%" mkdir "%OUT%"
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /W4 /EHsc /D_CRT_SECURE_NO_WARNINGS /std:c++17 /wd4100 /Fe:"%OUT%\test_modbrowser.exe" /Fo:"%OUT%\\" test_modbrowser.cpp ..\Runtime\ui\mb_util.cpp ..\Runtime\ui\mb_http.cpp ..\Runtime\ui\mb_sources.cpp ..\Runtime\ui\mb_install.cpp ..\Runtime\ui\mb_browser.cpp winhttp.lib bcrypt.lib || exit /b 1
"%OUT%\test_modbrowser.exe" "%~dp0modbrowser_fixture" "%OUT%\run" || exit /b 1
echo test_modbrowser: OK
