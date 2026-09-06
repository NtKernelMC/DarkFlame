$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath("$PSScriptRoot\..")
$build = Join-Path $repo '.codex-temp-dia2dump\lua-table-interop'
$runtime = Join-Path $repo 'Client\third_party\mta_lua'
[IO.Directory]::CreateDirectory($build) | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $build 'host')) | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $build 'embedded')) | Out-Null
# Independent host uses the original table implementation, not the code under test.
$hostTable = & git -C $repo show '931f97f:Client/third_party/mta_lua/ltable.c'
if($LASTEXITCODE) { throw 'Original Lua table source unavailable (commit 931f97f)' }
$hostTable | Set-Content -LiteralPath (Join-Path $build 'host\ltable.c') -Encoding ASCII
$names = 'lapi','lcode','ldebug','ldo','lfunc','lgc','llex','lmem','lobject','lopcodes','lparser','lstate','lstring','ltable','ltm','lvm','lzio'
$common = @('/nologo','/O2','/MT','/DNDEBUG','/D_CRT_SECURE_NO_WARNINGS',('/I"' + $runtime + '"'))
$hostSources = foreach($name in $names)
{
    if($name -eq 'ltable') { '"' + (Join-Path $build 'host\ltable.c') + '"' }
    else { '"' + (Join-Path $runtime "$name.c") + '"' }
}
$hostArgs = $common + @('/LD','/DLUA_BUILD_AS_DLL',('/Fo"' + $build + '\host\\"'),('/Fe"' + $build + '\LuaTableHost.dll"')) + $hostSources
$hostArgs | Set-Content (Join-Path $build 'host.rsp') -Encoding ASCII
& cl.exe ('@' + (Join-Path $build 'host.rsp'))
if($LASTEXITCODE) { throw 'Host Lua build failed' }
$sources = $names | ForEach-Object { '"' + (Join-Path $runtime "$_.c") + '"' }
$sources += '"' + (Join-Path $repo 'Client\embedded_lua_loader.c') + '"'
$sources += '"' + (Join-Path $repo 'Tests\lua_table_interop_test.cpp') + '"'
$testArgs = $common + @('/EHsc','/std:c++20',('/Fo"' + $build + '\embedded\\"'),('/Fe"' + $build + '\LuaTableInteropTest.exe"')) + $sources
$testArgs | Set-Content (Join-Path $build 'test.rsp') -Encoding ASCII
& cl.exe ('@' + (Join-Path $build 'test.rsp'))
if($LASTEXITCODE) { throw 'Embedded Lua test build failed' }
Push-Location $build
try
{
    & '.\LuaTableInteropTest.exe'
    if($LASTEXITCODE) { throw "Interop test failed: $LASTEXITCODE" }
}
finally { Pop-Location }
