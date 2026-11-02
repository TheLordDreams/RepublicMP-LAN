# SPDX-License-Identifier: GPL-3.0-only
# Prepares files ONLY; does not run a game, Steam, launcher, or installer.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$GameDirectory)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$source=(Resolve-Path -LiteralPath $GameDirectory).Path.TrimEnd('\','/')
$local=Join-Path $repo 'local'
$destinationRoot=[IO.Path]::GetFullPath((Join-Path $local 'test-installs'))
if ($destinationRoot.StartsWith($source+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Test root must not be inside source installation' }
$plugin=Join-Path $local 'releases/RepublicMP-v0.7.1.dll'
$loader=Join-Path $local 'releases/loader'
if ((Get-FileHash -LiteralPath $plugin -Algorithm SHA256).Hash -ne '2c0dad02ecdb84cd51a0bf6ee773ac8434113ed205e8aff33ae5a00b335aa67e') { throw 'Upstream plugin hash mismatch' }
$allowlist=@{
    'SOVIET64.exe'='296644a9f207d609031fc2ae73fed2dcb34619a1d55a35d1c7b51965ce6841b8'
    'C3DDLL64.dll'='65fcb6845c3cb7c25a22121f25904a855ea88657e31a3b50a15adac26e231516'
}
foreach($name in $allowlist.Keys) {
    if ((Get-FileHash -LiteralPath (Join-Path $source $name) -Algorithm SHA256).Hash -ne $allowlist[$name]) { throw "Unsupported source $name" }
}
$excluded=@('save','save_cloud','save_terraineditor','screenshots','workshop_wip') | ForEach-Object {Join-Path $source "media_soviet/$_"}
foreach($role in @('host','client')) {
    $target=[IO.Path]::GetFullPath((Join-Path $destinationRoot $role))
    if (-not $target.StartsWith($destinationRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Target escapes private test root' }
    if (Test-Path -LiteralPath $target) { throw "Refusing to overwrite an existing test install: $role" }
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    $log=Join-Path $local "copy-$role.log"
    # Physical copies; no /MIR, hardlinks, reparse-point traversal or source writes.
    & robocopy.exe $source $target /E /COPY:DAT /DCOPY:DAT /XJ /R:0 /W:0 /NFL /NDL /NJH /NJS "/LOG:$log" /XD @excluded /XF log.html config.ini controls.ini
    if($LASTEXITCODE -ge 8){throw "Robocopy failed for $role ($LASTEXITCODE), see private log"}
    foreach($name in @('save','save_cloud','save_terraineditor','screenshots')) { New-Item -ItemType Directory -Force -Path (Join-Path $target "media_soviet/$name") | Out-Null }
    foreach($name in @('config','controls')) { Copy-Item -LiteralPath (Join-Path $target "media_soviet/${name}_default.ini") -Destination (Join-Path $target "media_soviet/$name.ini") }
    $build=Join-Path $target 'tesmioloader/build'
    New-Item -ItemType Directory -Force -Path (Join-Path $build 'plugins') | Out-Null
    Copy-Item -LiteralPath (Join-Path $loader 'tesmioloader.dll'),(Join-Path $loader 'tesmiolauncher.exe') -Destination $build
    Copy-Item -LiteralPath $plugin -Destination (Join-Path $build 'plugins/RepublicMP.dll')
    @"
[tesmioloader]
trace_reads=0
log_game=1
vfs=0
probe_map=0
probe_texel=0
version_check=1
plugins=1
menu_patch=1
version=b0.3.6
game_exe=$target\SOVIET64.exe
[plugins]
republicmp=1
"@ | Set-Content -LiteralPath (Join-Path $build 'tesmioloader.ini') -Encoding utf8
    "[profile]`nnick=$role`nmax_players=2`ndlc_check=1`n" | Set-Content -LiteralPath (Join-Path $build 'plugins/RepublicMP.profile.ini') -Encoding utf8
    foreach($name in $allowlist.Keys) {
        if ((Get-FileHash -LiteralPath (Join-Path $target $name) -Algorithm SHA256).Hash -ne $allowlist[$name]) { throw "Copied binary mismatch: $role $name" }
    }
    "Prepared official upstream 0.7.1. Not the locally modified source build. Never launched." | Set-Content -LiteralPath (Join-Path $target 'TEST-COPY.txt')
    Write-Output "PREPARED: $role (upstream 0.7.1; not launched or co-op tested)"
}
foreach($name in $allowlist.Keys) {
    if ((Get-FileHash -LiteralPath (Join-Path $source $name) -Algorithm SHA256).Hash -ne $allowlist[$name]) { throw "Original binary changed: $name" }
}
Write-Output 'Original EXE and engine hashes unchanged. Launch requires separate Steam/account authorization.'
