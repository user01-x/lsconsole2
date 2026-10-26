# Copies dist\LSConsole2.dll into an InnerSpace install. Unload the extension in every session and
# the uplink first (`ext -unload LSConsole2`), or the DLL is locked. A host still running from an
# older version doesn't matter: the new DLL unpacks and starts its own.
param([string]$InnerSpace = "${env:ProgramFiles(x86)}\InnerSpace")
$ErrorActionPreference = 'Stop'
$dist = Join-Path $PSScriptRoot 'dist'
$ext = Join-Path $InnerSpace 'x64\Extensions\ISXDK35'
$src = Join-Path $dist 'LSConsole2.dll'
$dst = Join-Path $ext 'LSConsole2.dll'
if (-not (Test-Path $src)) { throw 'dist\ is empty: run build.ps1 first.' }
if (-not (Test-Path $ext)) { throw "not an InnerSpace install: $ext" }

Copy-Item $src $dst -Force
# Make sure what's installed now is this build.
if ((Get-FileHash $src).Hash -ne (Get-FileHash $dst).Hash) {
    throw "LSConsole2.dll was not replaced (still loaded in a session or the uplink?): $dst"
}
# Earlier builds installed the host in an LSConsole2 folder here; it now travels inside the DLL.
Remove-Item -Recurse -Force (Join-Path $ext 'LSConsole2') -ErrorAction SilentlyContinue
Write-Host "deployed to $ext"
