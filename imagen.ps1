# Convierte una imagen (PNG/JPG/BMP) a la pantalla de la VUSION 2.6 (296 x 152,
# blanco/negro/rojo) y la envia por radio a la etiqueta a traves de la herramienta
# del ESP32 y el puente.
#
# Uso:  imagen.bat doom.png               convierte, guarda la vista previa y envia
#       imagen.bat doom.png -SoloVista    solo genera <imagen>_vista.png
#       imagen.bat logo.png -SinTramado   colores planos (mejor para texto/logos simples)
#       imagen.bat foto.png -Foto         fotos: grises tramados, rojo solo en lo rojo
# Cierra el monitor serie del IDE de Arduino antes: el puerto tiene que estar libre.
param(
    [Parameter(Mandatory = $true)][string]$imagen,
    [switch]$SoloVista,
    [switch]$SinTramado,
    [switch]$Foto,
    [string]$Puerto = "COM8"
)
$ErrorActionPreference = "Stop"

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Drawing2D;

public static class Vusion {
    public const int ANCHO = 296, ALTO = 152, BYTES_LINEA = 19, PLANO = 296 * 19;
    // Paleta del panel: 0 blanco, 1 negro, 2 rojo (para la vista previa)
    static readonly int[,] PAL = { { 255, 255, 255 }, { 0, 0, 0 }, { 220, 30, 30 } };
    // Paleta de referencia para decidir el color: el "rojo" del panel se compara
    // con un naranja y el "negro" con un azul muy oscuro, para que naranjas y
    // amarillos tiren a rojo solido y los azules a negro (con rojo y negro puros
    // casi todo acababa en blanco punteado).
    public static int[,] REF = { { 255, 255, 250 }, { 25, 35, 55 }, { 235, 110, 35 } };

    // Modo foto: escala de grises con niveles automaticos y tramado blanco/negro;
    // solo los pixeles claramente rojos (saturados y de tono rojo) van a rojo.
    // Con la paleta de color normal la piel salia roja.
    public static double GAMMA = 1.4;
    static void Foto(double[,,] c, int[,] idx) {
        double[,] lum = new double[ALTO, ANCHO];
        bool[,] rojo = new bool[ALTO, ANCHO];
        int[] hist = new int[256];
        int n = 0;
        for (int y = 0; y < ALTO; y++)
            for (int x = 0; x < ANCHO; x++) {
                double r = c[y, x, 0], g = c[y, x, 1], b = c[y, x, 2];
                lum[y, x] = 0.299 * r + 0.587 * g + 0.114 * b;
                rojo[y, x] = r > 110 && r - Math.Max(g, b) > 70;
                if (!rojo[y, x]) { hist[(int)lum[y, x]]++; n++; }
            }
        // El rojo solo se queda en zonas rojas grandes (>= 35 de los 49 pixeles de
        // su entorno 7x7): los labios salian como puntos rojos sueltos
        bool[,] grande = new bool[ALTO, ANCHO];
        for (int y = 0; y < ALTO; y++)
            for (int x = 0; x < ANCHO; x++) {
                if (!rojo[y, x]) continue;
                int vec = 0;
                for (int dy = -3; dy <= 3; dy++)
                    for (int dx = -3; dx <= 3; dx++) {
                        int yy = y + dy, xx = x + dx;
                        if (yy >= 0 && yy < ALTO && xx >= 0 && xx < ANCHO && rojo[yy, xx]) vec++;
                    }
                grande[y, x] = vec >= 35;
            }
        for (int y = 0; y < ALTO; y++)
            for (int x = 0; x < ANCHO; x++)
                if (rojo[y, x] && !grande[y, x]) { rojo[y, x] = false; hist[(int)lum[y, x]]++; n++; }
        // Niveles: estirar del percentil 2 al 98
        int lo = 0, hi = 255, acc = 0;
        for (int i = 0; i < 256; i++) { acc += hist[i]; if (acc > n * 0.02) { lo = i; break; } }
        acc = 0;
        for (int i = 255; i >= 0; i--) { acc += hist[i]; if (acc > n * 0.02) { hi = i; break; } }
        if (hi <= lo) { lo = 0; hi = 255; }
        // y gamma > 1 para oscurecer los medios: sin ella la piel se iba a blanco y
        // solo quedaban los rasgos mas oscuros
        for (int y = 0; y < ALTO; y++)
            for (int x = 0; x < ANCHO; x++) {
                double v = Math.Max(0, Math.Min(255, (lum[y, x] - lo) * 255.0 / (hi - lo)));
                lum[y, x] = 255 * Math.Pow(v / 255, GAMMA);
            }
        // Floyd-Steinberg en gris; el rojo no reparte error
        for (int y = 0; y < ALTO; y++)
            for (int x = 0; x < ANCHO; x++) {
                if (rojo[y, x]) { idx[y, x] = 2; continue; }
                double v = lum[y, x];
                int k = v < 128 ? 1 : 0;
                idx[y, x] = k;
                double err = v - (k == 1 ? 0 : 255);
                if (x + 1 < ANCHO) lum[y, x + 1] += err * 7 / 16;
                if (y + 1 < ALTO) {
                    if (x > 0) lum[y + 1, x - 1] += err * 3 / 16;
                    lum[y + 1, x] += err * 5 / 16;
                    if (x + 1 < ANCHO) lum[y + 1, x + 1] += err * 1 / 16;
                }
            }
    }

    // Devuelve los 2 planos en el orden del panel y la vista previa
    public static byte[] Convertir(string ruta, bool tramar, bool foto, out Bitmap vista) {
        // 1) Ajustar a 296 x 152 sin deformar, centrado sobre blanco
        Bitmap lienzo = new Bitmap(ANCHO, ALTO);
        using (Image orig = Image.FromFile(ruta))
        using (Graphics g = Graphics.FromImage(lienzo)) {
            g.Clear(Color.White);
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            double esc = Math.Min((double)ANCHO / orig.Width, (double)ALTO / orig.Height);
            int w = (int)(orig.Width * esc), h = (int)(orig.Height * esc);
            g.DrawImage(orig, (ANCHO - w) / 2, (ALTO - h) / 2, w, h);
        }

        // 2) Reducir a 3 colores (Floyd-Steinberg si se pide tramado)
        double[,,] c = new double[ALTO, ANCHO, 3];
        for (int y = 0; y < ALTO; y++)
            for (int x = 0; x < ANCHO; x++) {
                Color p = lienzo.GetPixel(x, y);
                c[y, x, 0] = p.R; c[y, x, 1] = p.G; c[y, x, 2] = p.B;
            }
        int[,] idx = new int[ALTO, ANCHO];
        if (foto) Foto(c, idx);
        else for (int y = 0; y < ALTO; y++)
            for (int x = 0; x < ANCHO; x++) {
                int mejor = 0; double dmin = double.MaxValue;
                for (int k = 0; k < 3; k++) {
                    double d = 0;
                    for (int ch = 0; ch < 3; ch++) {
                        double e = c[y, x, ch] - REF[k, ch];
                        d += e * e;
                    }
                    if (d < dmin) { dmin = d; mejor = k; }
                }
                idx[y, x] = mejor;
                if (!tramar) continue;
                for (int ch = 0; ch < 3; ch++) {
                    double err = c[y, x, ch] - REF[mejor, ch];
                    if (x + 1 < ANCHO) c[y, x + 1, ch] += err * 7 / 16;
                    if (y + 1 < ALTO) {
                        if (x > 0) c[y + 1, x - 1, ch] += err * 3 / 16;
                        c[y + 1, x, ch] += err * 5 / 16;
                        if (x + 1 < ANCHO) c[y + 1, x + 1, ch] += err * 1 / 16;
                    }
                }
            }

        // 3) Vista previa al doble de tamano
        vista = new Bitmap(ANCHO * 2, ALTO * 2);
        for (int y = 0; y < ALTO; y++)
            for (int x = 0; x < ANCHO; x++) {
                int k = idx[y, x];
                Color col = Color.FromArgb(PAL[k, 0], PAL[k, 1], PAL[k, 2]);
                vista.SetPixel(2 * x, 2 * y, col); vista.SetPixel(2 * x + 1, 2 * y, col);
                vista.SetPixel(2 * x, 2 * y + 1, col); vista.SetPixel(2 * x + 1, 2 * y + 1, col);
            }

        // 4) Orden del panel (igual que epd_plano en comun/epd.h): 296 lineas de
        //    19 bytes; linea L = columna x = 295 - L (ESPEJO_X); bit 7 = y mas alta
        //    del byte. Plano 0: 1 = negro. Plano 1: 1 = rojo.
        byte[] datos = new byte[2 * PLANO];
        for (int l = 0; l < ANCHO; l++) {
            int x = ANCHO - 1 - l;
            for (int b = 0; b < BYTES_LINEA; b++) {
                byte negro = 0, rojo = 0;
                for (int bit = 0; bit < 8; bit++) {
                    int y = b * 8 + bit;
                    negro = (byte)((negro << 1) | (idx[y, x] == 1 ? 1 : 0));
                    rojo = (byte)((rojo << 1) | (idx[y, x] == 2 ? 1 : 0));
                }
                datos[l * BYTES_LINEA + b] = negro;
                datos[PLANO + l * BYTES_LINEA + b] = rojo;
            }
        }
        lienzo.Dispose();
        return datos;
    }
}
"@

$ruta = (Resolve-Path $imagen).Path
$vista = $null
$datos = [Vusion]::Convertir($ruta, -not $SinTramado, [bool]$Foto, [ref]$vista)
$rutaVista = [IO.Path]::ChangeExtension($ruta, $null).TrimEnd('.') + "_vista.png"
$vista.Save($rutaVista, [System.Drawing.Imaging.ImageFormat]::Png)
$vista.Dispose()
Write-Host "Vista previa: $rutaVista"
if ($SoloVista) { return }

$suma = [uint32]0
foreach ($b in $datos) { $suma = [uint32](($suma + $b) % 4294967296) }

$p = New-Object System.IO.Ports.SerialPort $Puerto, 115200
$p.DtrEnable = $false          # sin DTR/RTS el ESP32 no se reinicia al abrir
$p.RtsEnable = $false
$p.NewLine = "`n"
$p.Open()

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
    $p.WriteLine("IMG $($datos.Length) $suma")
    Esperar "LISTO" 5
    $p.Write($datos, 0, $datos.Length)
    Esperar "IMAGEN ENVIADA" 180
}
finally {
    $p.Close()
}
