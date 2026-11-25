@echo off
rem Builds and runs Engine/Tests/test_simd_fast.c (sw_rt.h fast SIMD paths vs the generic helpers), at /O1 /Oi as the chunks.
setlocal
set OUT=%TEMP%\sw_simd_fast_test
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O1 /Oi /I..\Runtime\include /Fe:"%OUT%\test_simd_fast.exe" /Fo:"%OUT%\\" test_simd_fast.c || exit /b 1
"%OUT%\test_simd_fast.exe"
