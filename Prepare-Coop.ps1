# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$GameDirectory,
    [string]$TestDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$package = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')
$localRoot = Join-Path $package 'local'
if (-not $TestDirectory) { $TestDirectory = Join-Path $localRoot 'game' }
$destination = [IO.Path]::GetFullPath($TestDirectory).TrimEnd('\')
$source = (Resolve-Path -LiteralPath $GameDirectory).ProviderPath.TrimEnd('\')
function Within([string]$Child, [string]$Parent) {
    return $Child.StartsWith($Parent.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)
}
function Assert-NoReparseAncestors([string]$Path) {
    $current = $Path
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse path refused: $current" }
        }
        $parent = Split-Path -Parent $current
        if ($parent -eq $current) { break }
        $current = $parent
    }
}
function Assert-Hash([string]$Path, [string]$Expected) {
    Assert-NoReparseAncestors $Path
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $Expected) { throw "SHA-256 mismatch: $Path" }
}
if (-not (Within $destination $localRoot)) { throw 'TestDirectory must be strictly inside this package local directory.' }
if ($destination.Length -gt 200) { throw 'Prepared-copy path is too long for the native launcher. Move the package to a shorter path (destination <=200 characters).' }
if ($source -ieq $destination -or (Within $source $destination) -or (Within $destination $source)) { throw 'Source and destination must be disjoint.' }
if (Test-Path -LiteralPath $destination) { throw 'Destination already exists. Choose a new directory inside package local; nothing will be overwritten.' }
Assert-NoReparseAncestors $package
Assert-NoReparseAncestors $source
Assert-NoReparseAncestors $destination
$pins = @{
    'SOVIET64.exe' = '296644a9f207d609031fc2ae73fed2dcb34619a1d55a35d1c7b51965ce6841b8'
    'C3DDLL64.dll' = '65fcb6845c3cb7c25a22121f25904a855ea88657e31a3b50a15adac26e231516'
}
foreach ($name in $pins.Keys) { Assert-Hash (Join-Path $source $name) $pins[$name] }
$manifestPath = Join-Path $package 'SHA256SUMS.json'
Assert-NoReparseAncestors $manifestPath
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schema -ne 1) { throw 'Unsupported runtime manifest schema.' }
$runtimeNames = @('tesmiolauncher.exe', 'tesmioloader.dll', 'RepublicMP.dll')
foreach ($name in $runtimeNames) {
    $key = 'runtime/' + $name
    $hash = [string]$manifest.files.$key
    if ($hash -notmatch '^[a-fA-F0-9]{64}$') { throw "Missing runtime pin: $key" }
    Assert-Hash (Join-Path $package $key) $hash
}
# Walk directories manually: reject a reparse point before entering it. This
# avoids following symlinks/junctions even before robocopy's /XJ protection.
$excludedNames = @('save','save_cloud','save_terraineditor','screenshots','workshop_wip')
$pending = New-Object 'System.Collections.Generic.Stack[string]'
$pending.Push($source)
while ($pending.Count) {
    $directory = $pending.Pop()
    foreach ($entry in Get-ChildItem -LiteralPath $directory -Force) {
        if ($excludedNames -icontains $entry.Name -and $entry.PSIsContainer) { continue }
        if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Source reparse entry refused: $($entry.FullName)" }
        if ($entry.PSIsContainer) { $pending.Push($entry.FullName) }
    }
}
$excludedDirectories = $excludedNames
$excludeFiles = @(
    (Join-Path $source 'log.html'),
    (Join-Path $source 'media_soviet/config.ini'),
    (Join-Path $source 'media_soviet/controls.ini')
)
New-Item -ItemType Directory -Path $destination | Out-Null
Write-Host 'Copying owned game files physically. This can take several minutes; no downloads.'
$copyArgs = @($source, $destination, '/E', '/XJ', '/COPY:DAT', '/DCOPY:DA', '/R:1', '/W:1', '/NP', '/NFL', '/NDL', '/XD') + $excludedDirectories + @('/XF') + $excludeFiles
& robocopy.exe @copyArgs
$copyResult = $LASTEXITCODE
if ($copyResult -ge 8) { throw "Copy failed (robocopy $copyResult). Incomplete destination kept for inspection; choose a new directory to retry." }
foreach ($name in $pins.Keys) { Assert-Hash (Join-Path $destination $name) $pins[$name] }
# Refuse preexisting mod payloads carried in from a modified game installation.
$privateRuntime = Join-Path $destination 'coop-runtime'
if (Test-Path -LiteralPath $privateRuntime) { throw 'Source contained coop-runtime. Use a clean owned installation.' }
New-Item -ItemType Directory -Path (Join-Path $privateRuntime 'plugins') | Out-Null
foreach ($name in @('tesmiolauncher.exe','tesmioloader.dll')) {
    Copy-Item -LiteralPath (Join-Path $package ('runtime/' + $name)) -Destination (Join-Path $privateRuntime $name)
}
Copy-Item -LiteralPath (Join-Path $package 'runtime/RepublicMP.dll') -Destination (Join-Path $privateRuntime 'plugins/RepublicMP.dll')
$media = Join-Path $destination 'media_soviet'
Copy-Item -LiteralPath (Join-Path $media 'config_default.ini') -Destination (Join-Path $media 'config.ini')
Copy-Item -LiteralPath (Join-Path $media 'controls_default.ini') -Destination (Join-Path $media 'controls.ini')
foreach ($name in @('save','save_cloud','save_terraineditor','screenshots')) {
    New-Item -ItemType Directory -Path (Join-Path $media $name) -Force | Out-Null
}
$copiedExe = Join-Path $destination 'SOVIET64.exe'
@"
[tesmioloader]
game_exe=$copiedExe
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
"@ | Set-Content -LiteralPath (Join-Path $privateRuntime 'tesmioloader.ini') -Encoding Unicode
$record = [ordered]@{ schema=1; gameDirectory=$destination; sourceVersion='1.1.1.9'; preparedAt=(Get-Date).ToString('o'); executableSha256=$pins['SOVIET64.exe']; engineSha256=$pins['C3DDLL64.dll'] }
$record | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destination 'TEST-COPY.txt') -Encoding UTF8
Write-Host "Prepared private copy: $destination"
Write-Host 'Keep Steam open under your own licensed account. Run Launch-Coop.ps1; this script does not start the game.'
