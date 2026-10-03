$ErrorActionPreference = 'Stop'
$taskRepo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$taskBuild = Join-Path $PSScriptRoot 'native-debug-dispatch-build'
[IO.Directory]::CreateDirectory($taskBuild) | Out-Null
$taskSource = [IO.File]::ReadAllText((Join-Path $taskRepo 'Client/lua_bridge.cpp'))
# Take exact production bodies; boundary failures abort instead of testing a copy.
$taskParts = @()
foreach($taskBoundary in @(
    @('bool HiddenActive()', 'std::string LuaText('),
    @('bool __stdcall HookCallHook(', 'bool __fastcall HookOnPreFunction('))) {
    $taskStart = $taskSource.IndexOf($taskBoundary[0])
    $taskEnd = $taskSource.IndexOf($taskBoundary[1], $taskStart)
    if($taskStart -lt 0 -or $taskEnd -le $taskStart) { throw 'Production function boundaries unavailable' }
    $taskParts += $taskSource.Substring($taskStart, $taskEnd - $taskStart)
}
[IO.File]::WriteAllText((Join-Path $taskBuild 'native_debug_dispatch_production.inc'), ($taskParts -join "`r`n"))
$taskRuntime = Join-Path $taskRepo 'Client/third_party/mta_lua'
$taskNames = 'lapi','lcode','ldebug','ldo','lfunc','lgc','llex','lmem','lobject','lopcodes','lparser','lstate','lstring','ltable','ltm','lvm','lzio'
$taskArgs = @('/nologo','/O2','/MT','/DNDEBUG','/D_CRT_SECURE_NO_WARNINGS','/EHsc','/std:c++20', ('/I"' + $taskRuntime + '"'), ('/I"' + $taskBuild + '"'), ('/Fo"' + $taskBuild + '\\"'), ('/Fe"' + $taskBuild + '\NativeDebugDispatchTest.exe"'))
$taskArgs += $taskNames | ForEach-Object { '"' + (Join-Path $taskRuntime "$_.c") + '"' }
$taskArgs += '"' + (Join-Path $PSScriptRoot 'native_debug_dispatch_test.cpp') + '"'
$taskArgs | Set-Content -LiteralPath (Join-Path $taskBuild 'test.rsp') -Encoding Unicode
& cl.exe ('@' + (Join-Path $taskBuild 'test.rsp'))
if($LASTEXITCODE) { throw 'Native dispatch test compilation failed' }
& (Join-Path $taskBuild 'NativeDebugDispatchTest.exe')
if($LASTEXITCODE) { throw 'Native dispatch regression failed' }
