# Builds both projects and stages the deployable layout in dist\:
#
#   dist\LSConsole2.dll                 -> InnerSpace\x64\Extensions\ISXDK35\
#   dist\LSConsole2\LSConsole2Host.exe      -> InnerSpace\x64\Extensions\ISXDK35\LSConsole2\
#   dist\LSConsole2\*.dll, resources\      (Ultralight runtime)
#   dist\LSConsole2\assets\console.html    (the page)
#
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
$ul = Join-Path $root 'third_party\ultralight'
$dist = Join-Path $root 'dist'
$hostDir = Join-Path $dist 'LSConsole2'
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force (Join-Path $hostDir 'assets') | Out-Null

Copy-Item (Join-Path $out 'ext\LSConsole2.dll') $dist
Copy-Item (Join-Path $out 'host\LSConsole2Host.exe') $hostDir
Copy-Item (Join-Path $ul 'bin\*.dll') $hostDir
Copy-Item -Recurse (Join-Path $ul 'resources') $hostDir
Copy-Item (Join-Path $root 'src\page\*') (Join-Path $hostDir 'assets')
Copy-Item (Join-Path $ul 'license\*') (Join-Path $hostDir 'resources')

Write-Host "staged: $dist"
