@echo off
rem Builds and runs Engine/Tests/test_simd_special.c (sw_rt.h SIMD special cases).
setlocal
set OUT=%TEMP%\sw_simd_test
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /I..\Runtime\include /Fe:"%OUT%\test_simd_special.exe" /Fo:"%OUT%\\" test_simd_special.c || exit /b 1
"%OUT%\test_simd_special.exe"
