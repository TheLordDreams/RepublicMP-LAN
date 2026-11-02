# SPDX-License-Identifier: GPL-3.0-only
# Real loader/game process, damaged DOS-stub byte ONLY in a disposable EXE copy.
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$root=[IO.Path]::GetFullPath((Join-Path $repo 'local/test-installs/host'))
$exe=Join-Path $root 'SOVIET64.exe'
$build=Join-Path $root 'tesmioloader/build'
$dll=Join-Path $repo 'local/build/lan/RepublicMP.dll'
if (-not (Test-Path -LiteralPath (Join-Path $root 'TEST-COPY.txt'))) {throw 'Not an isolated copy'}
if (-not (Test-Path -LiteralPath $dll)) {throw 'Local DLL not built'}
$expected='296644a9f207d609031fc2ae73fed2dcb34619a1d55a35d1c7b51965ce6841b8'
if ((Get-FileHash -LiteralPath $exe).Hash -ne $expected) {throw 'Unexpected test EXE before mutation'}
$live=@(Get-Process -Name SOVIET64 -ErrorAction SilentlyContinue | Where-Object {[string]::Equals($_.Path,$exe,[StringComparison]::OrdinalIgnoreCase)})
if ($live.Count) {throw 'Test copy is already running'}
$backup=Join-Path $root ('SOVIET64.exe.before-guard-'+(Get-Date -Format yyyyMMddHHmmss))
Copy-Item -LiteralPath $exe -Destination $backup
$log=Join-Path $build 'tesmioloader.log'
if(Test-Path -LiteralPath $log){Copy-Item -LiteralPath $log -Destination (Join-Path $repo 'local/guard-previous-loader.log')}
Copy-Item -LiteralPath $dll -Destination (Join-Path $build 'plugins/RepublicMP.dll')
$gamePid=0
try {
    $bytes=[IO.File]::ReadAllBytes($exe); $bytes[0x40]=$bytes[0x40] -bxor 1; [IO.File]::WriteAllBytes($exe,$bytes)
    $stdout=Join-Path $repo 'local/guard-launch.stdout'
    $stderr=Join-Path $repo 'local/guard-launch.stderr'
    $launcher=Join-Path $build 'tesmiolauncher.exe'
    $arguments='--nogui --game "'+$exe+'"'
    $start=Get-Date
    $process=Start-Process -FilePath $launcher -ArgumentList $arguments -WorkingDirectory $build -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    $matched=$false
    $deadline=(Get-Date).AddSeconds(45)
    while((Get-Date) -lt $deadline){
        if(Test-Path -LiteralPath $stdout){$out=Get-Content -LiteralPath $stdout -Raw; if($out -match 'pid (\d+) created suspended'){$gamePid=[int]$Matches[1]}}
        if(Test-Path -LiteralPath $log){$item=Get-Item -LiteralPath $log; if($item.LastWriteTime -ge $start){$text=Get-Content -LiteralPath $log -Raw; if($text -match 'BUILD GUARD: SOVIET64.exe SHA256=\w+ REFUSED' -and $text -match 'plugin not initialized'){$matched=$true;break}}}
        Start-Sleep -Milliseconds 200
    }
    if(-not $matched){throw 'Native guard refusal was not observed in the real loader log'}
    if($text -match 'republicmp  universal hook:'){throw 'Plugin installed a hook after the guard should have refused Init'}
    Copy-Item -LiteralPath $log -Destination (Join-Path $repo 'local/native-guard-refusal.log')
    Write-Output 'PASS: real native plugin refused altered EXE before its hooks'
} finally {
    try {
        # Recover a positively identified copied process even if launch output failed.
        $ownedProcesses=@(Get-Process -Name SOVIET64 -ErrorAction SilentlyContinue | Where-Object {[string]::Equals($_.Path,$exe,[StringComparison]::OrdinalIgnoreCase)})
        foreach($owned in $ownedProcesses){
            $null=$owned.CloseMainWindow()
            if(-not $owned.WaitForExit(5000)){Stop-Process -Id $owned.Id; $owned.WaitForExit(5000)|Out-Null}
        }
    } finally {
        $remaining=@(Get-Process -Name SOVIET64 -ErrorAction SilentlyContinue | Where-Object {[string]::Equals($_.Path,$exe,[StringComparison]::OrdinalIgnoreCase)})
        if($remaining.Count){throw "Owned test process is still running. Recovery backup retained at: $backup"}
        Copy-Item -LiteralPath $backup -Destination $exe
        if((Get-FileHash -LiteralPath $exe).Hash -ne $expected){throw "Restored test EXE hash mismatch. Recovery backup: $backup"}
        Write-Output 'Restored disposable test EXE to exact baseline hash'
    }
}
