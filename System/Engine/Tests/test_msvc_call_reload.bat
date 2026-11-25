@echo off
rem Builds and runs Engine/Tests/test_msvc_call_reload.c with the chunk flags
rem (/O1 /Oi) and /O2: generated-code call results must survive MSVC's optimiser.
rem Optional argument: extra cl flags (e.g. /DSW_TEST_OLD_RELOAD to see the old failure).
setlocal
set OUT=%TEMP%\sw_call_reload_test
if not exist "%OUT%" mkdir "%OUT%"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set RC=0
for %%O in ("/O1 /Oi" "/O2") do (
  cl /nologo /c /O2 /W3 /GS- /DSW_TEST_STUBS /I..\Runtime\include /Fo:"%OUT%\stubs.obj" test_msvc_call_reload.c >nul || exit /b 1
  cl /nologo /c %%~O /W3 /GS- /wd4102 %1 /I..\Runtime\include /Fo:"%OUT%\test.obj" test_msvc_call_reload.c >nul || exit /b 1
  link /nologo /out:"%OUT%\test_msvc_call_reload.exe" "%OUT%\test.obj" "%OUT%\stubs.obj" >nul || exit /b 1
  echo %%~O:
  "%OUT%\test_msvc_call_reload.exe" || set RC=1
)
exit /b %RC%
