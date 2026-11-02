# SPDX-License-Identifier: GPL-3.0-only
# Creates an uncompressed experimental staging directory. Never runs a binary.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$LauncherPath,
    [Parameter(Mandatory=$true)][string]$LoaderPath,
    [Parameter(Mandatory=$true)][string]$ModPath,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [ValidateSet('Lan','Steam')][string]$Transport = 'Lan',
    [string]$LoaderSourceDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path -Parent $PSScriptRoot
$workspace = Split-Path -Parent (Split-Path -Parent $repo)
if (-not $LoaderSourceDirectory) { $LoaderSourceDirectory = Join-Path (Split-Path -Parent $repo) 'TesmioLoader' }
$loaderRepo = (Resolve-Path -LiteralPath $LoaderSourceDirectory).ProviderPath
$output = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\')
function Inside([string]$Child, [string]$Parent) {
    return $Child.StartsWith($Parent.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)
}
function No-Reparse([string]$Path) {
    $current = $Path
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            if ((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse path refused: $current" }
        }
        $parent = Split-Path -Parent $current
        if ($parent -eq $current) { break }
        $current = $parent
    }
}
if (-not (Inside $output (Join-Path $workspace 'outputs')) -and -not (Inside $output (Join-Path $repo 'local/packages'))) {
    throw 'Output must be a new directory inside workspace outputs or repository local/packages.'
}
No-Reparse $output
if (Test-Path -LiteralPath $output) { throw 'Output already exists; no overwrite or merge is allowed.' }
$selected = New-Object 'System.Collections.Generic.List[object]'
function Select-File([string]$Source, [string]$Relative) {
    No-Reparse $Source
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Missing selected source/runtime file: $Source" }
    if ((Inside ([IO.Path]::GetFullPath($Source)) $output)) { throw 'Input cannot be inside the output directory.' }
    $selected.Add([pscustomobject]@{ Source=$Source; Relative=$Relative })
}
function Select-SourceTree([string]$Root, [string]$Directory, [string]$Prefix) {
    $start = Join-Path $Root $Directory
    No-Reparse $start
    if (-not (Test-Path -LiteralPath $start -PathType Container)) { throw "Missing source directory: $start" }
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($start)
    $extensions = @('.c','.cpp','.h','.hpp','.inl','.rc','.ps1','.mjs','.py','.json','.vcxproj','.filters','.slnx','.md','.txt','.bat','.example','.cmake','.in')
    while ($pending.Count) {
        $directoryPath = $pending.Pop()
        foreach ($entry in Get-ChildItem -LiteralPath $directoryPath -Force) {
            if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Source reparse entry refused: $($entry.FullName)" }
            if ($entry.PSIsContainer) {
                if ($entry.Name -notin @('local','.git','build','obj','bin','node_modules','__pycache__')) { $pending.Push($entry.FullName) }
                continue
            }
            if ($extensions -inotcontains $entry.Extension -and $entry.Name -ne '.gitignore' -and $entry.Name -notmatch '^(LICENSE|COPYING|NOTICE)(\..*)?$') { continue }
            $relative = $entry.FullName.Substring($Root.TrimEnd('\').Length + 1).Replace('\','/')
            Select-File $entry.FullName ($Prefix + '/' + $relative)
        }
    }
}
Select-File ((Resolve-Path -LiteralPath $LauncherPath).ProviderPath) 'runtime/tesmiolauncher.exe'
Select-File ((Resolve-Path -LiteralPath $LoaderPath).ProviderPath) 'runtime/tesmioloader.dll'
Select-File ((Resolve-Path -LiteralPath $ModPath).ProviderPath) 'runtime/RepublicMP.dll'
foreach ($name in @('Prepare-Coop.ps1','Launch-Coop.ps1','README_RU.md')) {
    Select-File (Join-Path $repo ('packaging/' + $name)) $name
}
$sourceDirs = @('api','core','determinism','game','games','lockstep','modes','qa','save','session','third_party','tools','transport','ui','world','packaging')
foreach ($name in $sourceDirs) { Select-SourceTree $repo $name 'sources/RepublicMP' }
foreach ($name in @('.gitignore','LICENSE','README.md','LOCAL_README_RU.md','ARCHITECTURE.md','CHANGELOG.md','mp_config.h.example','RepublicMP.slnx','RepublicMP.vcxproj','RepublicMP.vcxproj.filters')) {
    Select-File (Join-Path $repo $name) ('sources/RepublicMP/' + $name)
}
Select-SourceTree $loaderRepo 'src' 'sources/TesmioLoader'
foreach ($name in @('.gitignore','LICENSE','README.md','README_RU.md','build.bat','logo.ico')) {
    Select-File (Join-Path $loaderRepo $name) ('sources/TesmioLoader/' + $name)
}
# Everything is selected and validated before creating any output. No arbitrary
# recursive repository copy: local/, game assets, PDBs and logs cannot enter it.
New-Item -ItemType Directory -Path $output | Out-Null
foreach ($entry in $selected) {
    $target = Join-Path $output $entry.Relative
    $parent = Split-Path -Parent $target
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    Copy-Item -LiteralPath $entry.Source -Destination $target
}
$runtimeHashes = [ordered]@{}
foreach ($name in @('tesmiolauncher.exe','tesmioloader.dll','RepublicMP.dll')) {
    $key = 'runtime/' + $name
    $runtimeHashes[$key] = (Get-FileHash -LiteralPath (Join-Path $output $key) -Algorithm SHA256).Hash.ToLowerInvariant()
}
[ordered]@{schema=1; transport=$Transport; files=$runtimeHashes} | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.json') -Encoding UTF8
$sourceHashes = [ordered]@{}
foreach ($entry in $selected) {
    if ($entry.Relative.StartsWith('sources/')) { $sourceHashes[$entry.Relative] = (Get-FileHash -LiteralPath (Join-Path $output $entry.Relative) -Algorithm SHA256).Hash.ToLowerInvariant() }
}
[ordered]@{schema=1; files=$sourceHashes} | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $output 'SOURCE_SHA256SUMS.json') -Encoding UTF8
@"
EXPERIMENTAL SOURCE/RUNTIME PACKAGE — $Transport
Packaging itself makes no claim about multiplayer correctness.
Read REPORT_RU.md and QA_RESULTS.json, when supplied, for the measured DLL's
actual completed tests and limitations. Same-PC tests do not prove remote play.
No game files, saves, accounts, private logs or QA fixture assets are included.
Sources: E3418/RepublicMP and MaxLegend/TesmioLoader plus the local modifications
included under sources/. Preserve GPL and third-party license notices.
Rebuild RepublicMP with sources/RepublicMP/tools/Build-Local.ps1 -Transport $Transport.
Rebuild the loader/launcher with sources/TesmioLoader/build.bat using official MSVC.
No compiler or dependency installer is executed by this packaging script.
"@ | Set-Content -LiteralPath (Join-Path $output 'PACKAGE_STATUS.txt') -Encoding UTF8
Write-Output "Staged $($selected.Count) selected files. No archive created: $output"
