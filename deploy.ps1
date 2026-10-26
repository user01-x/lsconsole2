# Copies dist\ into an InnerSpace install. Unload the extension in every session first
# (`ext -unload LSConsole2`), or the DLL is locked; the host exits by itself 5 minutes after the
# last session disconnects (or end it in Task Manager: LSConsole2Host.exe).
param([string]$InnerSpace = "${env:ProgramFiles(x86)}\InnerSpace")
$ErrorActionPreference = 'Stop'
$dist = Join-Path $PSScriptRoot 'dist'
$ext = Join-Path $InnerSpace 'x64\Extensions\ISXDK35'
if (-not (Test-Path (Join-Path $dist 'LSConsole2.dll'))) { throw 'dist\ is empty: run build.ps1 first.' }
if (-not (Test-Path $ext)) { throw "not an InnerSpace install: $ext" }

Copy-Item (Join-Path $dist 'LSConsole2.dll') $ext -Force
# Earlier builds installed the host in an LSConsole2 folder here; it now travels inside the DLL.
Remove-Item -Recurse -Force (Join-Path $ext 'LSConsole2') -ErrorAction SilentlyContinue
Write-Host "deployed to $ext"
