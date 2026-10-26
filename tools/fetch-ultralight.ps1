# Downloads the Ultralight 1.4.0 free SDK (Windows x64) into third_party/ultralight.
#
# 1.4.0 is the CPU-capable release (2.0 is GPU-only) and the version ISXOgreX ships, though
# that no longer matters: the renderer runs in its own process.
# Licence: third_party/ultralight/license/LICENSE.txt (free for companies under US$100k turnover).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tp = Join-Path $root 'third_party'
$archive = Join-Path $tp 'ultralight-free-sdk-1.4.0-win-x64.7z'
$dest = Join-Path $tp 'ultralight'

if (Test-Path (Join-Path $dest 'include\Ultralight\Ultralight.h')) { Write-Host "SDK already present: $dest"; exit 0 }
New-Item -ItemType Directory -Force $tp | Out-Null

if (-not (Test-Path $archive)) {
    Write-Host 'Downloading Ultralight 1.4.0 SDK...'
    Invoke-WebRequest -Uri 'https://ultralig.ht/api/v1/sdk/download?platform=windows-x64&version=1.4.0' -OutFile $archive
}

$7z = @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $7z) { throw '7-Zip is required to unpack the SDK (7z.exe not found).' }
& $7z x -y "-o$dest" $archive | Out-Null
if ($LASTEXITCODE -ne 0) { throw "7z failed ($LASTEXITCODE)" }
Write-Host "SDK unpacked to $dest"
