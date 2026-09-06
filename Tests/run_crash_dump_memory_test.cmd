@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if not "%errorlevel%"=="0" exit /b 1
pushd "%~dp0..\.codex-temp-dia2dump"
cl /nologo /std:c++20 /EHsc /MT /O2 /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN "%~dp0crash_dump_memory_test.cpp" /Fecrash_dump_memory_test.exe /Focrash_dump_memory_test.obj /link user32.lib
if not "%errorlevel%"=="0" exit /b 1
crash_dump_memory_test.exe
set result=%errorlevel%
popd
exit /b %result%
