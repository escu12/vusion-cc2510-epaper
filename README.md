# vusion-cc2510-epaper

Firmware propio para las etiquetas electrónicas de estantería **SES-imagotag VUSION 2.6 BWR**
(placa *imagotag RFRTx024A*, microcontrolador **TI CC2510**) y herramientas para grabarlas con
un **ESP32**: texto, listas y **imágenes** por radio a 2,4 GHz, flash SPI, NFC y LED.

*English version: [README_EN.md](README_EN.md)*

> ⚠️ Usa solo etiquetas que sean tuyas. Grabar firmware propio **borra para siempre** el
> original (la flash viene bloqueada y no se puede copiar). Todo esto es experimental.

## Qué se consigue

- Grabar el CC2510 por su interfaz de depuración con un ESP32 (bit-bang), **unos 6 s** por
  firmware, enviando el binario por el puerto serie (`grabar.bat`).
- Enlace de radio propio entre etiquetas (2433 MHz, 250 kbps MSK) con ACK y reintentos.
- Una etiqueta sin pantalla hace de **puente** UART ↔ radio para el ESP32.
- La etiqueta con pantalla muestra **texto**, una **lista de 10 líneas** y **imágenes**
  296 × 152 en blanco/negro/rojo (`imagen.bat foto.png`), guardadas en su flash.
- El puente lee los textos que escribes **con el móvil por NFC**, guarda los 10 últimos en su
  flash y se los manda a la pantalla cuando cambian.
- El LED blanco da 3 destellos al recibir algo.

## Hardware identificado

Placa **RFRTx024A** (VUSION 2.6 BWR, GL320). Todo comprobado en la placa real.

| Cara superior | Cara inferior |
|---|---|
| ![Cara superior](docs/placa_superior.jpg) | ![Cara inferior](docs/placa_inferior.jpg) |

| Componente | Detalle |
|---|---|
| MCU + radio | **TI CC2510** (8051, 32 KB flash, 4 KB RAM, radio tipo CC2500). Chip ID `0x8104` |
| Pantalla | Pervasive Displays **SE2266FS093** (2,66", 296 × 152, blanco/negro/rojo, SPI) |
| Flash externa | **Winbond W25X10** (1 Mbit = 128 KB), marcada `1H820`, USON-8. JEDEC `EF 30 11` |
| NFC | **NXP NTAG I2C plus 1K** (NT3H2111), I²C dirección `0x55` |
| Cristales | 26 MHz (radio y reloj) y 32,768 kHz en P2_3/P2_4 (marcado `A812N`) |
| LED | Blanco, con elevador TPS61071 |

### Pinout del CC2510

| Función | Pin |
|---|---|
| Pantalla: alimentación (activo bajo) | P0_0 |
| Pantalla: CS / SDI / CLK | P0_1 / P0_3 / P0_5 |
| Pantalla: DC / BUSY (bajo = ocupado) / RESET | P1_2 / P1_3 / P2_0 |
| Flash: CS / CLK / MOSI (DI) / MISO (DO) | P1_4 / P1_5 / P1_6 / P1_7 (SPI de USART1, Alt.2) |
| Flash: alimentación y WP# | **P1_0 = 1** (alimenta también el NFC) |
| NFC: SDA / SCL | P0_4 / P0_6 |
| LED | **P2_2 = 1** (enable del elevador) **y P2_1 = 1** |
| Depuración: DD / DC / RESET | P2_1 / P2_2 / RESET_N (comparten con el LED) |
| UART del puente (sin pantalla) | **P0_3 = TX** (USART0 Alt.1), **P0_5 = RX** (USART1 Alt.1) |

La secuencia de inicialización del panel es la "iTC small" de Pervasive Displays:
`0x00=0x0E` (reset), `0xE5=0x19` (25 °C), `0xE0=0x02`, plano negro `0x10`, plano rojo `0x13`,
y `0x04` / `0x12` / `0x02` esperando BUSY. Cada plano son 296 líneas × 19 bytes; en apaisado
hay que invertir el orden de las líneas.

### Pads de test de la cara superior (medidos)

Junto a la etiqueta plateada con el código QR, a los dos lados de los dos agujeros, hay 10 pads
redondos. Medidos con el polímetro en esta placa (mirando la cara superior, con el conector de
la pantalla a la izquierda):

![Pads de test de la cara superior](docs/placa_superior.jpg)

```
   lado izquierdo            lado derecho
        P0_3          (o)       VCC
        P0_4          (o)       P0_2   P0_5
  RST   P1_4                    P2_1   P2_2   GND
```

| Señal | Para qué |
|---|---|
| VCC / GND | Alimentación (3,3 V) |
| RST | Reset (grabación) |
| P2_1 | **DD** de depuración (y LED) |
| P2_2 | **DC** de depuración (y elevador del LED) |
| P0_3 / P0_5 | TX / RX del UART del puente (y SDI / CLK de la pantalla) |
| P0_4 | SDA del NFC |
| P1_4 | CS de la flash |
| P0_2 | Uso desconocido |

En este grupo está todo lo necesario para grabar (VCC, GND, RST, DD y DC) y el UART del puente:
es el sitio más cómodo para soldar los cables o poner pines pogo.

## Conexión al ESP32 (DevKit V1)

En la cara trasera de la placa hay puntos de test para la depuración. Todo a **3,3 V**, sin divisores
y **sin pila** puesta.

| ESP32 | Placa |
|---|---|
| 3V3 / GND | `+` / `-` |
| GPIO25 | RST |
| GPIO26 | DC (P2_2) |
| GPIO27 (directo) **y** GPIO32 (a través de 1 kΩ) | DD (P2_1) |
| GPIO16 (RX2) | P0_3 de la placa **puente** |
| GPIO17 (TX2) | P0_5 de la placa **puente** |

- RST/DC/DD van a la placa que quieras grabar (**nunca a dos placas a la vez**).
- El UART solo va al puente. **Nunca a la etiqueta con pantalla**: allí P0_5 es el reloj de la
  pantalla (salida) y se pelearía con el TX del ESP32.
- En la placa que no esté conectada al grabador, pon **10 kΩ de DC a GND**: con DC al aire, un
  reset puede dejar el CC2510 parado en modo depuración.

## Requisitos

- **SDCC 4.x** en `C:\Program Files\SDCC` (`winget install SDCC.SDCC`), o indica la ruta con `-Sdcc`.
- **Arduino IDE** o arduino-cli con el core **esp32**, para subir `herramienta/herramienta.ino`.
- Windows con PowerShell 5.1 (los scripts usan .NET; no hace falta instalar nada más).

## Uso rápido

1. Sube `herramienta/herramienta.ino` al ESP32 (una sola vez) y cierra el monitor serie.
2. Conecta RST/DC/DD a la etiqueta con pantalla y grábala (`-Borrar` la primera vez, porque
   viene bloqueada):
   ```bat
   grabar.bat receptor -Borrar
   ```
3. Pasa RST/DC/DD a una placa sin pantalla (la del puente), conéctale el UART y grábala:
   ```bat
   grabar.bat puente -Borrar
   ```
4. Manda una imagen:
   ```bat
   imagen.bat logo.png
   imagen.bat foto.png -Foto        :: fotos: grises tramados, rojo solo en zonas rojas
   imagen.bat logo.png -SoloVista   :: solo genera logo_vista.png para ver cómo queda
   ```
5. O escribe un texto con una app NFC en la placa puente: aparecerá en la lista de la pantalla.

En PowerShell hay que poner `.\` delante: `.\grabar.bat receptor`.

### Comandos de la herramienta (monitor serie a 115200, "Nueva línea")

| Comando | Qué hace |
|---|---|
| `t TEXTO` | Manda texto a la etiqueta (`\|` separa líneas, máx. 4 × 24) |
| `d N` | Etiqueta destino |
| `ping` | Comprueba que el puente contesta |
| `info`, `borrar`, `grabar`, `verificar`, `run` | Sobre la placa conectada a RST/DC/DD |
| `progreso`, `xdata` | Lee los bytes de diagnóstico (XDATA 0xFD00–0xFDFF) |
| `r`, `x`, `m [s]`, `escuchar [s]` | Reset y lectura del UART del puente |
| `vel N` | Velocidad del reloj de depuración en µs (2 por defecto) |

Después de subir cualquier sketch al ESP32, envía `run`: al reiniciarse el ESP32 puede dejar la
placa conectada parada.

## Firmwares

| Carpeta | Para qué |
|---|---|
| `receptor/` | Etiqueta con pantalla: texto, lista NFC, imágenes en flash, LED |
| `puente/` | Placa sin pantalla: módem UART ↔ radio + memoria de textos NFC en flash |
| `hola_mundo/` | Primera prueba de la pantalla |
| `nfc/` | Leer, escribir y desbloquear el NTAG I2C (ver más abajo) |
| `prueba_led/` | Prueba de las combinaciones del LED (lo indica en pantalla) |
| `explora*/` | Diagnóstico usado para encontrar la flash y el NFC (pull-ups, barrido I²C/SPI) |
| `comun/` | Pantalla (`epd.h`), radio (`radio.h`), flash (`flash.h`), NFC (`nfc.h`) |

### Protocolo de radio

Trama: `[len][dst][tipo][seq][datos]`, dst `0x00` = puente, `0xFF` = todas. Tipos: `T` texto,
`L` línea de la lista, `P` pintar la lista, `E` preparar imagen, `I` trozo de imagen
(`[pos16][55 bytes]`), `M` pintar imagen, `A` ACK (`[seq][rssi]`). El receptor envía el ACK
antes de pintar (un refresco tarda unos 20 s) e ignora secuencias repetidas.
Los datos de imagen van mezclados (XOR con una tabla fija) porque la radio no tiene blanqueado
y las rachas largas de `00`/`FF` llegaban corruptas.

## Cosas aprendidas (para no repetirlas)

- La flash del CC2510 viene **bloqueada**: solo se puede borrar entera. El bit DEBUG_LOCKED del
  STATUS sigue a 1 después del borrado; compruébalo ejecutando `MOV A,#0x5A`.
- Con la librería CCLib (adaptada al ESP32) el chip ID salía `0x0000`; con el bit-bang propio
  de `herramienta.ino` funciona, incluso a 2 µs por medio ciclo. No se averiguó la causa.
- Si el ESP32 **suelta** RST, el flanco lento hace que la mitad de los arranques se queden a
  13 MHz (UART a media velocidad). Hay que llevar RST activamente a 1.
- Cambio de reloj: esperar a que `CLKCON` lea `0x80` antes de apagar el RC (datasheet 12.1.5).
- El CC2510 no tiene ST0–ST2: el temporizador de reposo se lee con `WORTIME0/1`.
- El NTAG I2C viene bloqueado **desde el lado NFC**, pero desde I²C los bloqueos y el CC se
  pueden volver a 0 (datasheet NT3H2111, 8.3.6–8.3.8). Al escribir el bloque 0, el byte 0 es la
  dirección I²C: hay que poner `0xAA`.
- Todo alimentado del 3V3 del ESP32 funciona, pero los picos de la pantalla pueden reiniciarlo.

## Créditos y referencias

- [fishpepper/CC2510Lib](https://github.com/fishpepper/CC2510Lib) y [wavesoft/CCLib](https://github.com/wavesoft/CCLib): método de escritura de flash (TI SWRA124)
- [atc1441/ESP_CC_Flasher](https://github.com/atc1441/ESP_CC_Flasher): ideas para la herramienta
- [angrymew/firmware-cc2510](https://github.com/angrymew/firmware-cc2510): pinout de la placa hermana GL120
- [18nelli18/Monopink](https://github.com/18nelli18/Monopink)
- Datasheets de TI CC2510, NXP NT3H2111 y Winbond W25X10

## Licencia

GPL-3.0 (ver `LICENSE`). La rutina de escritura de flash deriva de CCLib / CC2510Lib (GPLv3).
