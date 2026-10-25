# Compila un firmware del CC2510 con SDCC: <proyecto>\build\main.bin
# Uso: .\build.ps1 receptor   (o puente, hola_mundo, nfc, prueba_led, explora...)
# Normalmente no se llama a mano: lo usa grabar.ps1.
param(
    [string]$proyecto = "hola_mundo",
    [string]$Sdcc = "C:\Program Files\SDCC"
)
$ErrorActionPreference = "Stop"
$sdccExe = Join-Path $Sdcc "bin\sdcc.exe"
$mkbin   = Join-Path $Sdcc "bin\makebin.exe"
if (-not (Test-Path $sdccExe)) { throw "No encuentro SDCC en $Sdcc (instalalo o usa -Sdcc <carpeta>)" }
# sdcpp solo encuentra cc1 si SDCC\bin esta en el PATH
$env:PATH = (Join-Path $Sdcc "bin") + ";$env:PATH"
$src = Join-Path $PSScriptRoot $proyecto
$out = Join-Path $src "build"
New-Item -ItemType Directory -Force $out | Out-Null

# XDATA: 0xF000-0xFCFF para SDCC (0xFD00-0xFDFF queda libre para diagnostico)
# Rutas relativas: si la carpeta tiene espacios, PowerShell 5.1 las parte
Push-Location $src
try {
    & $sdccExe -mmcs51 --model-small --code-size 0x8000 --iram-size 0x100 `
        --xram-loc 0xF000 --xram-size 0x0D00 -I../comun -o build/ main.c
    if ($LASTEXITCODE) { throw "sdcc fallo" }
} finally { Pop-Location }

& $mkbin -p "$out\main.ihx" "$out\main.bin"
$len = (Get-Item "$out\main.bin").Length
"Firmware $proyecto : $len bytes -> $proyecto\build\main.bin"
