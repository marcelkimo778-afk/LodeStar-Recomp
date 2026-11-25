@echo off
rem Builds and runs Engine/Tests/test_rsqrt_table.c (FRSQRTE estimate table == Arm search loop).
setlocal
set OUT=%TEMP%\sw_rsqrt_test
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O1 /Oi /I..\Runtime\include /Fe:"%OUT%\test_rsqrt_table.exe" /Fo:"%OUT%\\" test_rsqrt_table.c || exit /b 1
"%OUT%\test_rsqrt_table.exe"
