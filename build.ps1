# Builds the solution and puts the one deployable file in dist\:
#
#   dist\LSConsole2.dll   -> InnerSpace\x64\Extensions\ISXDK35\
#
# The host, Ultralight and the page travel inside it (tools/pack-runtime.ps1, run by the LSConsole2
# project before it compiles; the staged runtime is in build\x64\<config>\runtime\ for the harness).
# Nothing is copied into InnerSpace here; see deploy.ps1.
param([string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

& (Join-Path $root 'tools\fetch-ultralight.ps1')

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found (vswhere).' }

& $msbuild (Join-Path $root 'LSConsole2.sln') /m /nologo /v:minimal "/p:Configuration=$Configuration" /p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$out = Join-Path $root "build\x64\$Configuration"
$dist = Join-Path $root 'dist'
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item (Join-Path $out 'ext\LSConsole2.dll') $dist

Write-Host "staged: $dist"
