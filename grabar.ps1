# Compila un firmware y lo graba en el CC2510 conectado a RST/DC/DD a traves de
# la herramienta del ESP32 (herramienta/herramienta.ino), sin recompilar el ESP32.
#
# Uso:  grabar.bat receptor            (o puente, hola_mundo, ...)
#       grabar.bat puente -Borrar      chip erase antes (placas bloqueadas)
#       grabar.bat receptor -Vel 5     reloj de depuracion mas lento (por defecto 2)
# Cierra el monitor serie del IDE de Arduino antes: el puerto tiene que estar libre.
param(
    [Parameter(Mandatory = $true)][string]$proyecto,
    [switch]$Borrar,
    [string]$Puerto = "COM8",
    [int]$Vel = 2
)
$ErrorActionPreference = "Stop"

& (Join-Path $PSScriptRoot "build.ps1") $proyecto
$bin = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot "$proyecto\build\main.bin"))
$suma = [uint32]0
foreach ($b in $bin) { $suma = [uint32](($suma + $b) % 4294967296) }

$p = New-Object System.IO.Ports.SerialPort $Puerto, 115200
$p.DtrEnable = $false          # sin DTR/RTS el ESP32 no se reinicia al abrir
$p.RtsEnable = $false
$p.NewLine = "`n"
$p.Open()

# Lee del ESP32 mostrando lo que llega hasta encontrar $patron (o ERROR)
function Esperar([string]$patron, [int]$seg) {
    $texto = ""
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $seg) {
        $nuevo = $p.ReadExisting()
        if ($nuevo) {
            Write-Host -NoNewline $nuevo
            $texto += $nuevo
            if ($texto -match "ERROR") { throw "La herramienta devolvio un error" }
            if ($texto -match $patron) { return }
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Sin respuesta del ESP32 ($patron). Comprueba que herramienta.ino esta subida y el puerto libre."
}

try {
    Start-Sleep -Milliseconds 300
    $p.DiscardInBuffer()
    $p.WriteLine("vel $Vel")
    Esperar "T = " 5

    if ($Borrar) {
        $p.WriteLine("borrar")
        Esperar "BORRADO OK" 20
    }

    $p.WriteLine("W $($bin.Length) $suma")
    Esperar "LISTO" 5
    $p.Write($bin, 0, $bin.Length)
    Esperar "OK RECIBIDO" 15

    $p.WriteLine("grabar")
    Esperar "VERIFICACION OK" 240
    Write-Host "`nGrabado $proyecto ($($bin.Length) bytes)."
}
finally {
    $p.Close()
}
