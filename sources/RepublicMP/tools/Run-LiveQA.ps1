# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Python,
 [ValidateRange(0,60)][int]$LongMinutes=0,
 [switch]$Construction,
 [switch]$Rejoin,
 [ValidateSet('none','clean','lag-jitter')][string]$ChaosProfile='none',
 [ValidateRange(0,600000)][int]$DisconnectAfterMs=0)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$tool=Join-Path $repo 'third_party/coop-anything/gameslot/gameslot.py'
$env:GAMESLOT_DIR=Join-Path $repo 'local/gameslot'
$env:COOP_QA_OWNER='wrsr-liveqa-'+$PID
$env:COOP_PYTHON=(Resolve-Path -LiteralPath $Python).Path
$env:COOP_QA_LONG_MINUTES=[string]$LongMinutes
$env:COOP_QA_CONSTRUCTION=if($Construction){'1'}else{'0'}
$env:COOP_QA_REJOIN=if($Rejoin){'1'}else{'0'}
$env:COOP_QA_CHAOS=$ChaosProfile
$env:COOP_QA_DISCONNECT_MS=[string]$DisconnectAfterMs
$runsRoot=Join-Path $repo 'local/qa-runs'
$before=@(Get-ChildItem -LiteralPath $runsRoot -Directory -ErrorAction SilentlyContinue | ForEach-Object FullName)
& $env:COOP_PYTHON -I $tool acquire --owner $env:COOP_QA_OWNER --slots 2 --ttl 3600 --wait 0
if($LASTEXITCODE -ne 0){throw 'Could not acquire two game slots'}
$result=1
try {
    Push-Location $repo
    try { & node (Join-Path $PSScriptRoot 'run-live-qa.mjs'); $result=$LASTEXITCODE } finally { Pop-Location }
} finally {
    try {
        $newRuns=@(Get-ChildItem -LiteralPath $runsRoot -Directory | Where-Object {$before -notcontains $_.FullName})
        foreach($run in $newRuns){
            if(Test-Path -LiteralPath (Join-Path $run.FullName 'processes.json')){ & (Join-Path $PSScriptRoot 'Stop-QAProcesses.ps1') -RunDirectory $run.FullName }
        }
    } finally { & $env:COOP_PYTHON -I $tool release --owner $env:COOP_QA_OWNER }
}
exit $result
