@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
pushd "%~dp0..\.codex-temp-dia2dump"
cl /nologo /I"%~dp0..\Client\third_party\imgui" /std:c++20 /EHsc /MT /Gy /O2 /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS "%~dp0pilot_ui_preview.cpp" /Fepilot_ui_preview.exe /Fopilot_ui_preview.obj /link /LTCG /OPT:REF "%~dp0..\obj\DarkFlameClient\Release\x86\imgui.obj" "%~dp0..\obj\DarkFlameClient\Release\x86\imgui_draw.obj" "%~dp0..\obj\DarkFlameClient\Release\x86\imgui_tables.obj" "%~dp0..\obj\DarkFlameClient\Release\x86\imgui_widgets.obj" "%~dp0..\obj\DarkFlameClient\Release\x86\imgui_impl_dx9.obj" user32.lib imm32.lib gdi32.lib d3d9.lib
if errorlevel 1 exit /b 1
pilot_ui_preview.exe active
if errorlevel 1 exit /b 1
pilot_ui_preview.exe standby
if errorlevel 1 exit /b 1
pilot_ui_preview.exe offline
if errorlevel 1 exit /b 1
pilot_ui_preview.exe fault
set result=%errorlevel%
popd
exit /b %result%
