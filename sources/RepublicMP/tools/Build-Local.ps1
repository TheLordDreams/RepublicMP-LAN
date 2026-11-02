# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([ValidateSet('Lan','Steam')][string]$Transport = 'Lan')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$builder = Get-Command MSBuild.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
if (-not $builder) {
    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $locator) {
        $builder = & $locator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    }
}
if (-not $builder) {
    Write-Output 'BLOCKED: MSBuild with MSVC C++ tools was not found. Project requires v145 and Windows SDK 10. Nothing was installed.'
    exit 3
}
$flavor = $Transport.ToLowerInvariant()
$output = Join-Path $repo "local/build/$flavor"
$intermediate = Join-Path $repo "local/obj/$flavor"
New-Item -ItemType Directory -Force -Path $output,$intermediate | Out-Null
$argsForBuild = @((Join-Path $repo 'RepublicMP.vcxproj'), '/m', '/p:Configuration=Release', '/p:Platform=x64',
    "/p:OutDir=$output\", "/p:IntDir=$intermediate\", "/p:RepublicMpTcpOnly=$($Transport -eq 'Lan')")
& $builder @argsForBuild
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$dll = Join-Path $output 'RepublicMP.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw 'Build returned success without RepublicMP.dll' }
Get-FileHash -LiteralPath $dll -Algorithm SHA256 | Format-List
Write-Output "Built $Transport source variant only. Live co-op QA is still required."
