param(
    [string]$ProcessName = 'gta_sa',
    [string]$OutputDirectory = "$PSScriptRoot\..\bin\Release\x86\CrashDumps",
    [int]$TimeoutSeconds = 1800,
    [int]$WarmupSeconds = 30
)

$ErrorActionPreference = 'Stop'
$debugger = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x86\cdb.exe'
if(!(Test-Path -LiteralPath $debugger)) { throw "CDB missing: $debugger" }
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
if($outputPath -match '["\r\n]') { throw 'Unsupported output path' }
[IO.Directory]::CreateDirectory($outputPath) | Out-Null
$watchLog = Join-Path $outputPath 'HangWatch.log'
$mutex = New-Object Threading.Mutex($false, ('Local\DarkFlameHangWatch_' + $ProcessName))
if(!$mutex.WaitOne(0)) { $mutex.Dispose(); throw 'A hang watcher is already running' }

function Write-WatchLog([string]$Message)
{
    $line = '[{0:yyyy-MM-dd HH:mm:ss}] {1}' -f (Get-Date), $Message
    Add-Content -LiteralPath $watchLog -Value $line -Encoding UTF8
    Write-Output $line
}

try
{
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $trackedId = 0
    $unresponsiveSince = $null
    Write-WatchLog "Ready; process=$ProcessName; waiting for up to $TimeoutSeconds seconds"
    while((Get-Date) -lt $deadline)
    {
        $game = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
            Sort-Object StartTime -Descending | Select-Object -First 1
        if(!$game)
        {
            if($trackedId) { Write-WatchLog "Process $trackedId exited before capture" }
            $trackedId = 0
            $unresponsiveSince = $null
            Start-Sleep -Seconds 2
            continue
        }
        if($trackedId -ne $game.Id)
        {
            $trackedId = $game.Id
            $unresponsiveSince = $null
            Write-WatchLog "Found process=$trackedId; started=$($game.StartTime.ToString('o'))"
        }
        if(((Get-Date) - $game.StartTime).TotalSeconds -lt $WarmupSeconds)
        {
            Start-Sleep -Seconds 2
            continue
        }
        $game.Refresh()
        if(!$game.MainWindowHandle -or $game.Responding)
        {
            $unresponsiveSince = $null
            Start-Sleep -Seconds 2
            continue
        }
        if(!$unresponsiveSince)
        {
            $unresponsiveSince = Get-Date
            Write-WatchLog "Window stopped responding; confirming process=$trackedId"
        }
        if(((Get-Date) - $unresponsiveSince).TotalSeconds -lt 5)
        {
            Start-Sleep -Seconds 2
            continue
        }

        $casePath = Join-Path $outputPath ('Hang_{0:yyyyMMdd_HHmmss}_P{1}' -f (Get-Date), $trackedId)
        [IO.Directory]::CreateDirectory($casePath) | Out-Null
        Write-WatchLog "Capturing process=$trackedId to $casePath"
        $game | Select-Object Id,ProcessName,StartTime,CPU,WorkingSet64,MainWindowHandle |
            Format-List | Out-File (Join-Path $casePath 'process.txt') -Encoding UTF8
        $releasePath = [IO.Path]::GetFullPath("$PSScriptRoot\..\bin\Release\x86")
        foreach($name in 'TramBot.log','DarkFlame.log','DarkFlameClient.dll','DarkFlameClient.pdb')
        {
            $sourcePath = Join-Path $releasePath $name
            if(Test-Path -LiteralPath $sourcePath)
            {
                try { Copy-Item -LiteralPath $sourcePath -Destination $casePath }
                catch { Write-WatchLog "Could not copy ${name}: $_" }
            }
        }
        $captured = 0
        for($sample = 1; $sample -le 2; ++$sample)
        {
            $current = Get-Process -Id $trackedId -ErrorAction SilentlyContinue
            if(!$current -or $current.StartTime -ne $game.StartTime) { break }
            $dumpPath = Join-Path $casePath "hang-$sample.dmp"
            $commandPath = Join-Path $casePath "capture-$sample.txt"
            @(('.dump /mA "' + $dumpPath + '"'), '~* kb', 'q') |
                Set-Content -LiteralPath $commandPath -Encoding ASCII
            & $debugger -pv -p $trackedId -y $casePath -cf $commandPath 2>&1 |
                Out-File (Join-Path $casePath "debugger-$sample.log") -Encoding UTF8
            $debuggerCode = $LASTEXITCODE
            $dump = Get-Item -LiteralPath $dumpPath -ErrorAction SilentlyContinue
            if(!$dump -or $dump.Length -lt 4096)
            {
                Write-WatchLog "Capture $sample failed; cdb=$debuggerCode; see debugger-$sample.log"
                break
            }
            ++$captured
            Write-WatchLog "Saved $dumpPath ($($dump.Length) bytes)"
            if($sample -lt 2) { Start-Sleep -Seconds 3 }
        }
        Write-WatchLog "Finished: $captured snapshots; game was not terminated by this script"
        if(!$captured) { exit 1 }
        exit 0
    }
    Write-WatchLog 'Stopped: waiting period expired'
}
finally
{
    $mutex.ReleaseMutex()
    $mutex.Dispose()
}
