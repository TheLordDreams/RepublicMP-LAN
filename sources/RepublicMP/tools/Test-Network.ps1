# SPDX-License-Identifier: GPL-3.0-only
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$locator=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $locator -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC not found. Nothing installed.' }
$out=Join-Path $repo 'local/network-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$batch=Join-Path $out 'build-test.cmd'
@"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++20 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /I"$repo\third_party\tesmio" "$PSScriptRoot\test-network.cpp" /Fe:network-tests.exe /link ws2_32.lib
if errorlevel 1 exit /b 1
network-tests.exe
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding ascii
Push-Location $out
try { & $env:ComSpec /d /c $batch; $result=$LASTEXITCODE } finally { Pop-Location }
if($result -ne 0) { throw "Native TCP tests failed ($result)" }
