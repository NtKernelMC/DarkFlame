@echo off
setlocal
set "PATH=%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x86 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
MSBuild.exe "%~dp0..\Client\DarkFlameClient.vcxproj" /p:Configuration=Release /p:Platform=Win32 /p:SolutionDir="%~dp0..\\" /p:TrackFileAccess=false /p:MinimalRebuildFromTracking=false /m /v:minimal /nologo
exit /b %errorlevel%
