# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param(
 [Parameter(Mandatory=$true)][string]$GameDirectory,
 [string]$BeforeJson = (Join-Path (Split-Path -Parent $PSScriptRoot) 'local/original-saves-before.json'),
 [string]$ReportJson = (Join-Path (Split-Path -Parent $PSScriptRoot) 'local/original-saves-verification.json')
)
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath $GameDirectory).ProviderPath.TrimEnd('\')+'\'
$before=@(Get-Content -LiteralPath $BeforeJson -Raw | ConvertFrom-Json)
$expected=@{}
foreach($item in $before){
 $file=[IO.Path]::GetFullPath($item.file)
 if(-not $file.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)){throw 'Baseline path outside specified original installation'}
 $expected[$file]=$item
}
$current=@{}
foreach($name in @('save','save_cloud','save_terraineditor')){
 $folder=Join-Path $root ('media_soviet/'+$name)
 if(Test-Path -LiteralPath $folder){foreach($file in Get-ChildItem -LiteralPath $folder -File -Recurse){$current[$file.FullName]=$file}}
}
$changed=@();$missing=@();$added=@()
foreach($file in $expected.Keys){
 if(-not $current.ContainsKey($file)){$missing+=$file.Substring($root.Length);continue}
 $hash=(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
 if($hash -ne $expected[$file].sha256 -or $current[$file].Length -ne $expected[$file].bytes){$changed+=$file.Substring($root.Length)}
}
foreach($file in $current.Keys){if(-not $expected.ContainsKey($file)){$added+=$file.Substring($root.Length)}}
$ok=($changed.Count -eq 0 -and $missing.Count -eq 0 -and $added.Count -eq 0)
[ordered]@{verifiedAt=[DateTime]::UtcNow.ToString('o');pass=$ok;baselineFiles=$expected.Count;currentFiles=$current.Count;changed=$changed;missing=$missing;added=$added} |
 ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportJson -Encoding UTF8
Write-Output "Original save verification: pass=$ok, $($expected.Count) baseline files, changed=$($changed.Count), missing=$($missing.Count), added=$($added.Count)"
if(-not $ok){exit 1}
