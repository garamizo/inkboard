# First-time board setup on Windows (`just setup`): PlatformIO and include/secrets.h.
$ErrorActionPreference = "Stop"
Set-Location (Split-Path $PSScriptRoot -Parent)

$idePio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\pio.exe"
if (Get-Command pio -ErrorAction SilentlyContinue) {
    Write-Host "PlatformIO: $(pio --version)"
} elseif (Test-Path $idePio) {
    # Two PlatformIO versions wipe each other's builds (#1): use the VS Code extension's copy.
    $dir = Split-Path $idePio -Parent
    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$dir", "User")
    Write-Host "PlatformIO: added the VS Code extension's copy ($dir) to your PATH; open a new terminal"
} else {
    uv tool install platformio
    uv tool update-shell
    Write-Host "PlatformIO: installed; open a new terminal so pio is on PATH"
}

if (Test-Path include\secrets.h) {
    Write-Host "Wi-Fi: include\secrets.h exists"
} else {
    Copy-Item include\secrets.h.example include\secrets.h
    Write-Host "Wi-Fi: created include\secrets.h; put your Wi-Fi name and password and your FRED API key in it"
}
