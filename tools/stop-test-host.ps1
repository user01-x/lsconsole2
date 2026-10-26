# Stops render hosts started from this repo's dist\ (the harness's), never one running from an
# InnerSpace install: those belong to live sessions.
$dist = Join-Path (Split-Path -Parent $PSScriptRoot) 'dist'
Get-CimInstance Win32_Process -Filter "Name='LSConsole2Host.exe'" | ForEach-Object {
    $p = Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue
    $path = try { $p.MainModule.FileName } catch { '' }
    if ($path -and $path.StartsWith($dist, [StringComparison]::OrdinalIgnoreCase)) {
        Stop-Process -Id $_.ProcessId -Force
        Wait-Process -Id $_.ProcessId -Timeout 5 -ErrorAction SilentlyContinue
        Write-Host "stopped test host $($_.ProcessId)"
    }
}
