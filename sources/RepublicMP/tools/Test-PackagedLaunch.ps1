# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Python,[Parameter(Mandatory=$true)][string]$PackageDirectory)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$package=(Resolve-Path -LiteralPath $PackageDirectory).ProviderPath.TrimEnd('\')
$allowed=[IO.Path]::GetFullPath((Join-Path $repo 'local/packages')).TrimEnd('\')+'\'
if(-not $package.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)){throw 'Only a private validation package may be tested'}
if(@(Get-Process -Name SOVIET64 -ErrorAction SilentlyContinue).Count){throw 'Another game is running; normal package native events must not share a Windows session with it'}
$game=Join-Path $package 'local/game';$exe=Join-Path $game 'SOVIET64.exe'
$script=Join-Path $package 'Launch-Coop.ps1'
$log=Join-Path $game 'coop-runtime/tesmioloader.log'
$stdout=Join-Path $repo 'local/package-launch.stdout';$stderr=Join-Path $repo 'local/package-launch.stderr'
$tool=Join-Path $repo 'third_party/coop-anything/gameslot/gameslot.py'
$env:GAMESLOT_DIR=Join-Path $repo 'local/gameslot';$owner='wrsr-package-'+$PID
& $Python -I $tool acquire --owner $owner --slots 1 --ttl 900 --wait 0
if($LASTEXITCODE -ne 0){throw 'No game slot available'}
$gamePid=0;$pass=$false;$start=Get-Date;$clean=$false
try {
    $argsForShell=@('-NoProfile','-ExecutionPolicy','RemoteSigned','-File',('"'+$script+'"'),'-TestDirectory',('"'+$game+'"'))
    # Start-Process from bundled PS7 otherwise passes its PS7 module paths to
    # Windows PowerShell 5.1; use that shell's built-in modules for this child.
    $previousModulePath=$env:PSModulePath
    try {
        $env:PSModulePath=Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/Modules'
        $launcher=Start-Process -FilePath powershell.exe -ArgumentList $argsForShell -WorkingDirectory $package -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    } finally { $env:PSModulePath=$previousModulePath }
    $end=(Get-Date).AddMinutes(4)
    while((Get-Date) -lt $end){
        if(Test-Path -LiteralPath $stdout){
            $text=Get-Content -LiteralPath $stdout -Raw
            if(-not $gamePid -and $text -match 'pid (\d+) created suspended'){
                $gamePid=[int]$Matches[1];$owned=Get-Process -Id $gamePid
                if(-not [string]::Equals($owned.Path,$exe,[StringComparison]::OrdinalIgnoreCase)){throw 'Launcher PID executable does not match private package copy'}
                & $Python -I $tool register-pid --owner $owner --pid $gamePid
                if($LASTEXITCODE -ne 0){throw 'Could not register owned package process'}
            }
        }
        if($launcher.HasExited -and $launcher.ExitCode -ne 0){throw 'Packaged Launch-Coop.ps1 failed; see private stdout/stderr'}
        if($gamePid -and (Test-Path -LiteralPath $log) -and (Get-Item -LiteralPath $log).LastWriteTime -ge $start){
            $content=Get-Content -LiteralPath $log -Raw
            $owned=Get-Process -Id $gamePid -ErrorAction SilentlyContinue
            if($owned -and $owned.MainWindowHandle -ne 0 -and $owned.Responding -and
                $content -match 'BUILD GUARD: SOVIET64.exe SHA256=\w+ matched' -and
                $content -match 'BUILD GUARD: C3DDLL64.dll SHA256=\w+ matched' -and
                $content -match 'plugin\s+republicmp\s+0.7.2-lan1' -and
                $content -match 'IMGUI: initialized'){
                if($content -match 'QA INSTANCE EVENTS: CreateEventW isolation installed=1|QA VEHICLE TRACE: hook installed=1'){throw 'QA unexpectedly enabled in ordinary user package'}
                $pass=$true;Copy-Item -LiteralPath $log -Destination (Join-Path $repo 'local/package-launch-loader.log');break
            }
        }
        Start-Sleep -Milliseconds 500
    }
    if(-not $pass){throw 'Packaged ordinary-game startup not verified before deadline'}
} finally {
    try {
        if($gamePid){
            $owned=Get-Process -Id $gamePid -ErrorAction SilentlyContinue
            if($owned){
                if(-not [string]::Equals($owned.Path,$exe,[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing to stop a reused package PID'}
                $null=$owned.CloseMainWindow();$clean=$owned.WaitForExit(10000)
                if(-not $clean){Stop-Process -Id $owned.Id;$owned.WaitForExit(5000)|Out-Null}
            }else{$clean=$true}
        }
        [ordered]@{pass=$pass;verifiedAt=[DateTime]::UtcNow.ToString('o');dllSha256=(Get-FileHash -LiteralPath (Join-Path $package 'runtime/RepublicMP.dll')).Hash.ToLowerInvariant();cleanExit=$clean;
            launchScriptSha256=(Get-FileHash -LiteralPath $script).Hash.ToLowerInvariant();prepareScriptSha256=(Get-FileHash -LiteralPath (Join-Path $package 'Prepare-Coop.ps1')).Hash.ToLowerInvariant()} |
            ConvertTo-Json|Set-Content -LiteralPath (Join-Path $repo 'local/package-launch-validation.json') -Encoding UTF8
    } finally { & $Python -I $tool release --owner $owner }
}
Write-Output "Packaged ordinary launch verified: $pass; graceful game exit: $clean"
