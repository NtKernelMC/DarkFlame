@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
set "build=%~dp0..\design-preview\test-build"
if not exist "%build%" mkdir "%build%"
pushd "%build%"
cl /nologo /I"%~dp0..\Client\third_party\imgui" /I"%~dp0..\Agent\third_party\minhook" /std:c++20 /EHsc /MT /Gy /Od /utf-8 /DUNICODE /D_UNICODE /DIMGUI_ENABLE_TEST_ENGINE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS "%~dp0plow_marks_test.cpp" "%~dp0..\Client\third_party\imgui\imgui.cpp" "%~dp0..\Client\third_party\imgui\imgui_draw.cpp" "%~dp0..\Client\third_party\imgui\imgui_tables.cpp" "%~dp0..\Client\third_party\imgui\imgui_widgets.cpp" "%~dp0..\Client\third_party\imgui\backends\imgui_impl_dx9.cpp" /Feplow_marks_test.exe /link /LTCG /OPT:REF "%~dp0..\obj\DarkFlameClient\Release\x86\imgui_impl_win32.obj" "%~dp0..\obj\DarkFlameClient\Release\x86\TextEditor.obj" "%~dp0..\obj\DarkFlameClient\Release\x86\DarkFlameClient.res" user32.lib imm32.lib gdi32.lib d3d9.lib windowscodecs.lib ole32.lib
if errorlevel 1 exit /b 1
if "%~1"=="--build-only" exit /b 0
plow_marks_test.exe "%~dp0..\design-preview\renders"
set result=%errorlevel%
popd
exit /b %result%
