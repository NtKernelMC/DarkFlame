@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
set "build=%TEMP%\plow_marks_test_build"
if not exist "%build%" mkdir "%build%"
pushd "%build%"
cl /nologo /I"%~dp0..\Client\third_party\imgui" /std:c++20 /EHsc /MT /Od /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS "%~dp0plow_marks_test.cpp" /Feplow_marks_test.exe /Foplow_marks_test.obj /link "%~dp0..\obj\DarkFlameClient\Release\x86\imgui.obj" "%~dp0..\obj\DarkFlameClient\Release\x86\imgui_draw.obj" "%~dp0..\obj\DarkFlameClient\Release\x86\imgui_tables.obj" "%~dp0..\obj\DarkFlameClient\Release\x86\imgui_widgets.obj" "%~dp0..\obj\DarkFlameClient\Release\x86\imgui_impl_dx9.obj" user32.lib imm32.lib gdi32.lib d3d9.lib
if errorlevel 1 exit /b 1
plow_marks_test.exe "%TEMP%\plow_marks_test"
set result=%errorlevel%
popd
exit /b %result%
