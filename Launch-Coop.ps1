# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([string]$TestDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$package = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
$localRoot = Join-Path $package 'local'
if (-not $TestDirectory) { $TestDirectory = Join-Path $localRoot 'game' }
$destination = [IO.Path]::GetFullPath($TestDirectory).TrimEnd('\')
if (-not $destination.StartsWith($localRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Only a prepared copy strictly inside package local can be launched.' }
if ($destination.Length -gt 200) { throw 'Prepared-copy path is too long for the native launcher. Use a package path with destination <=200 characters.' }
function Assert-NoReparse([string]$Path) {
    $current = $Path
    while ($current) {
        $item = Get-Item -LiteralPath $current -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse path refused: $current" }
        $parent = Split-Path -Parent $current
        if ($parent -eq $current) { break }
        $current = $parent
    }
}
function Assert-Hash([string]$Path, [string]$Hash) {
    Assert-NoReparse $Path
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Hash) { throw "SHA-256 mismatch: $Path" }
}
Assert-NoReparse $destination
# A prepared copy can have been changed since preparation. Reject redirects
# anywhere in its tree, including config/save paths that the game will write.
$pending = New-Object 'System.Collections.Generic.Stack[string]'
$pending.Push($destination)
while ($pending.Count) {
    foreach ($entry in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
        if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Prepared-copy reparse entry refused: $($entry.FullName)" }
        if ($entry.PSIsContainer) { $pending.Push($entry.FullName) }
    }
}
$marker = Join-Path $destination 'TEST-COPY.txt'
Assert-NoReparse $marker
$record = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
if ($record.schema -ne 1 -or $record.gameDirectory -ine $destination) { throw 'Prepared-copy marker is missing or belongs to a different directory. Prepare a fresh copy.' }
$exe = Join-Path $destination 'SOVIET64.exe'
Assert-Hash $exe '296644a9f207d609031fc2ae73fed2dcb34619a1d55a35d1c7b51965ce6841b8'
Assert-Hash (Join-Path $destination 'C3DDLL64.dll') '65fcb6845c3cb7c25a22121f25904a855ea88657e31a3b50a15adac26e231516'
$manifestPath = Join-Path $package 'SHA256SUMS.json'
Assert-NoReparse $manifestPath
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema -ne 1) { throw 'Unsupported runtime manifest schema.' }
$privateRuntime = Join-Path $destination 'coop-runtime'
foreach ($name in @('tesmiolauncher.exe','tesmioloader.dll','RepublicMP.dll')) {
    $key = 'runtime/' + $name
    $hash = [string]$manifest.files.$key
    if ($hash -notmatch '^[a-fA-F0-9]{64}$') { throw "Missing runtime pin: $key" }
    Assert-Hash (Join-Path $package $key) $hash
    $installed = if ($name -eq 'RepublicMP.dll') { Join-Path $privateRuntime 'plugins/RepublicMP.dll' } else { Join-Path $privateRuntime $name }
    Assert-Hash $installed $hash
}
$plugins = @(Get-ChildItem -LiteralPath (Join-Path $privateRuntime 'plugins') -Force)
$allowedPluginFiles = @('RepublicMP.dll', 'RepublicMP.profile.ini')
if (@($plugins | Where-Object { $_.PSIsContainer -or $allowedPluginFiles -inotcontains $_.Name }).Count -gt 0) { throw 'Unexpected files in the private plugins directory.' }
$ini = Join-Path $privateRuntime 'tesmioloader.ini'
Assert-NoReparse $ini
$content = Get-Content -LiteralPath $ini -Raw
$expectedIni = @"
[tesmioloader]
game_exe=$exe
version_check=1
plugins=1
vfs=0
trace_reads=0
log_game=1
probe_map=0
probe_texel=0
save_manifest=0
menu_patch=1
version=package
menu_tag=tesmioloader v. package
[plugins]
republicmp=1
"@
if (($content.Replace("`r`n", "`n").Trim()) -cne ($expectedIni.Replace("`r`n", "`n").Trim())) {
    throw 'Private loader configuration changed. Restore the prepared configuration or prepare a fresh copy.'
}
Write-Host 'Ensure licensed Steam is already open. Select Host/Join in the in-game MULTIPLAYER menu.'
$launcher = Join-Path $privateRuntime 'tesmiolauncher.exe'
$loader = Join-Path $privateRuntime 'tesmioloader.dll'
# Explicit paths are mandatory. The wrapper never asks the launcher to search
# Steam or auto-discover the original installation. Do not add --ignore-version.
$launcherArguments=@('--game',('"'+$exe+'"'),'--dll',('"'+$loader+'"'),'--nogui')
$startInfo=New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName=$launcher
$startInfo.Arguments=[string]::Join(' ',$launcherArguments)
$startInfo.WorkingDirectory=$privateRuntime
$startInfo.UseShellExecute=$false
$startInfo.CreateNoWindow=$true
$startInfo.RedirectStandardOutput=$true
$startInfo.RedirectStandardError=$true
$launcherProcess=[System.Diagnostics.Process]::Start($startInfo)
# Windows PowerShell 5.1 need not set LASTEXITCODE for a GUI-subsystem EXE.
# Wait for this loader process only; Start-Process -Wait can also wait for its
# game child, which must remain running after successful injection.
try {
    $stdoutTask=$launcherProcess.StandardOutput.ReadToEndAsync()
    $stderrTask=$launcherProcess.StandardError.ReadToEndAsync()
    $launcherProcess.WaitForExit()
    $launcherExitCode=$launcherProcess.ExitCode
    if ($stdoutTask.Result) { Write-Host $stdoutTask.Result }
    if ($stderrTask.Result) { [Console]::Error.Write($stderrTask.Result) }
}
finally { $launcherProcess.Dispose() }
if ($launcherExitCode -ne 0) { throw "Launcher failed with exit code $launcherExitCode. No fallback launch was attempted." }
