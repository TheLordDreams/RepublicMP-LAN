# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$RunDirectory)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$allowedRoot=[IO.Path]::GetFullPath((Join-Path $repo 'local/test-installs')).TrimEnd('\')+'\'
$run=[IO.Path]::GetFullPath($RunDirectory)
$runsRoot=[IO.Path]::GetFullPath((Join-Path $repo 'local/qa-runs')).TrimEnd('\')+'\'
if(-not $run.StartsWith($runsRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Run must be inside private qa-runs'}
$records=Get-Content -LiteralPath (Join-Path $run 'processes.json') -Raw|ConvertFrom-Json
foreach($record in $records){
    if(-not $record.pid){continue}
    $exe=[IO.Path]::GetFullPath($record.exe)
    if(-not $exe.StartsWith($allowedRoot,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($exe) -ne 'SOVIET64.exe'){throw 'Process path escapes test installs'}
    $owned=Get-Process -Id $record.pid -ErrorAction SilentlyContinue
    if(-not $owned){continue}
    if(-not [string]::Equals($owned.Path,$exe,[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing to stop a reused PID with another executable'}
    $null=$owned.CloseMainWindow()
    if(-not $owned.WaitForExit(10000)){Stop-Process -Id $owned.Id;Write-Output "Forced stop of own test process $($owned.Id)"}else{Write-Output "Graceful close of own test process $($owned.Id)"}
}
