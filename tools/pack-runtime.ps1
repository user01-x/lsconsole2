# Stages the render host's runtime -- LSConsole2Host.exe, Ultralight's DLLs and resources, the
# console page and the licences -- and packs it into one file that LSConsole2.dll carries as a
# resource and unpacks into %LOCALAPPDATA%\LSConsole2\runtime\<id>\ the first time it loads
# (src/ext/Runtime.cpp). Run by the LSConsole2 project before it compiles.
#
#   pack-runtime.ps1 -HostExe <LSConsole2Host.exe> -Ultralight <SDK folder> -Page <src\page>
#                    -Stage <runtime folder to lay out> -Out <pack file>
#
# The staged folder is the runtime as it will be unpacked (the test harness runs the host from it).
# Pack format, little-endian:
#   "LSC2PAK1", uint32 count, uint64 id (FNV-1a over every path and its bytes: same files, same id)
#   per file: uint16 path length, the path (UTF-8, backslashes, relative), uint32 size,
#             uint32 packed size, the packed bytes (Windows Compression API, LZMS, buffer mode)
# The pack is rebuilt only when a staged file is newer than it (packing takes about ten seconds).
param(
    [Parameter(Mandatory)][string]$HostExe,
    [Parameter(Mandatory)][string]$Ultralight,
    [Parameter(Mandatory)][string]$Page,
    [Parameter(Mandatory)][string]$Stage,
    [Parameter(Mandatory)][string]$Out
)
$ErrorActionPreference = 'Stop'

# Stage. Copy-Item keeps each file's time, so an unchanged runtime leaves the pack alone.
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
New-Item -ItemType Directory -Force (Join-Path $Stage 'assets'), (Join-Path $Stage 'resources') | Out-Null
Copy-Item $HostExe $Stage
Copy-Item (Join-Path $Ultralight 'bin\*.dll') $Stage
Copy-Item (Join-Path $Ultralight 'resources\*') (Join-Path $Stage 'resources')
Copy-Item (Join-Path $Ultralight 'license\*') (Join-Path $Stage 'resources')
Copy-Item (Join-Path $Page '*') (Join-Path $Stage 'assets')

$files = Get-ChildItem -LiteralPath $Stage -Recurse -File | Sort-Object FullName
if ((Test-Path $Out) -and -not ($files | Where-Object { $_.LastWriteTimeUtc -gt (Get-Item $Out).LastWriteTimeUtc })) {
    Write-Host "runtime pack up to date: $Out"
    return
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RuntimePack {
    [DllImport("cabinet.dll", SetLastError = true)] static extern bool CreateCompressor(uint alg, IntPtr alloc, out IntPtr h);
    [DllImport("cabinet.dll", SetLastError = true)] static extern bool Compress(IntPtr h, byte[] src, UIntPtr srcSize, byte[] dst, UIntPtr dstSize, out UIntPtr outSize);
    [DllImport("cabinet.dll")] static extern bool CloseCompressor(IntPtr h);
    public static byte[] Lzms(byte[] src) {
        IntPtr h;
        if (!CreateCompressor(5 /* COMPRESS_ALGORITHM_LZMS */, IntPtr.Zero, out h)) throw new Exception("CreateCompressor failed: " + Marshal.GetLastWin32Error());
        try {
            UIntPtr need;
            Compress(h, src, (UIntPtr)src.Length, null, UIntPtr.Zero, out need); // asks for the size
            byte[] dst = new byte[(int)need];
            UIntPtr got;
            if (!Compress(h, src, (UIntPtr)src.Length, dst, need, out got)) throw new Exception("Compress failed: " + Marshal.GetLastWin32Error());
            Array.Resize(ref dst, (int)got);
            return dst;
        } finally { CloseCompressor(h); }
    }
    public static ulong Fnv(ulong h, byte[] data) { foreach (byte b in data) { h ^= b; h *= 1099511628211UL; } return h; }
}
'@

$root = (Resolve-Path -LiteralPath $Stage).Path.TrimEnd('\')
[uint64]$id = 14695981039346656037
$entries = foreach ($f in $files) {
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $path = [System.Text.Encoding]::UTF8.GetBytes($f.FullName.Substring($root.Length + 1))
    $id = [RuntimePack]::Fnv([RuntimePack]::Fnv($id, $path), $bytes)
    [pscustomobject]@{ Path = $path; Size = $bytes.Length; Packed = [RuntimePack]::Lzms($bytes) }
}

New-Item -ItemType Directory -Force (Split-Path -Parent $Out) | Out-Null
$w = New-Object System.IO.BinaryWriter([System.IO.File]::Create($Out))
try {
    $w.Write([System.Text.Encoding]::ASCII.GetBytes('LSC2PAK1'))
    $w.Write([uint32]$entries.Count)
    $w.Write([uint64]$id)
    foreach ($e in $entries) {
        $w.Write([uint16]$e.Path.Length); $w.Write($e.Path)
        $w.Write([uint32]$e.Size); $w.Write([uint32]$e.Packed.Length); $w.Write($e.Packed)
    }
} finally { $w.Close() }
$raw = ($entries | Measure-Object Size -Sum).Sum
Write-Host ("runtime pack: {0} files, {1:N1} MB -> {2:N1} MB, id {3:x16}" -f $entries.Count, ($raw / 1MB), ((Get-Item $Out).Length / 1MB), $id)
