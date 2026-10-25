/*
 * Lectura (y escritura opcional) del NFC NXP NTAG I2C desde el CC2510.
 * Conexiones encontradas con explora/: SDA = P0_4, SCL = P0_6, VCC con P1_0 = 1
 * (P1_0 alimenta tambien la flash W25X10). Direccion I2C 0x55.
 *
 * Por I2C la memoria va en bloques de 16 bytes (bloque n = paginas NFC 4n..4n+3):
 *   bloque 0      : UID, bloqueos estaticos y Capability Container (NO ESCRIBIR:
 *                   el byte 0 es la direccion I2C y los bloqueos son irreversibles)
 *   bloque 1...   : memoria de usuario, el NDEF empieza aqui
 *   0x3A / 0x7A   : configuracion en versiones de 1K / 2K
 *
 * MODO = 0 solo lee (hazlo primero y apunta tu bloque 0, B38 y la URL original).
 * MODO = 1 escribe un NDEF de texto de prueba en los bloques 1-3.
 * MODO = 2 restaura una URL (pon la tuya en URL_ORIGINAL).
 * MODO = 3 desbloquea el lado NFC (el movil podra escribir), MODO = 4 vuelve a
 * bloquearlo como venia (valores tipicos de SES-imagotag; comprueba los tuyos).
 * Antes de los modos 3/4 pon en UID1..UID3 los bytes 1-3 del UID de TU chip
 * (leidos con MODO 0): si no coinciden, no se escribe nada.
 * Informe por el UART (P0_3, 115200) -> 'm' en herramienta.ino.
 */

#include <stdint.h>
#include <cc2510fx.h>

#ifndef MODO
#define MODO 0
#endif

#define DIR_NFC 0x55

/* Bytes 1-3 del UID de tu chip (MODO 0, bloque 0). Proteccion para los modos 3/4. */
#define UID1 0x00
#define UID2 0x00
#define UID3 0x00
#define SDA_BIT 0x10   /* P0_4 */
#define SCL_BIT 0x40   /* P0_6 */

/* ---------- UART ---------- */
static void uart_init(void)
{
    PERCFG &= ~0x01;
    P0SEL |= 0x08;
    P0DIR |= 0x08;
    U0CSR = 0x80;
    U0UCR = 0x02;
    U0BAUD = 34;
    U0GCR = 12;
}

static void tx(char c)
{
    UTX0IF = 0;
    U0DBUF = c;
    while (!UTX0IF)
        ;
    UTX0IF = 0;
}

static void txs(const char *s) { while (*s) tx(*s++); }

static void txh(uint8_t v)
{
    static const char h[] = "0123456789ABCDEF";
    tx(h[v >> 4]);
    tx(h[v & 15]);
}

static void delay_ms(uint16_t ms)
{
    volatile uint16_t i;
    while (ms--)
        for (i = 0; i < 1300; i++)
            ;
}

static void espera(void)
{
    volatile uint8_t i;
    for (i = 0; i < 60; i++)
        ;
}

/* ---------- I2C open-drain con pull-up interno ---------- */
#define SDA_BAJO()  (P0DIR |= SDA_BIT)
#define SDA_ALTO()  (P0DIR &= ~SDA_BIT)
#define SCL_BAJO()  (P0DIR |= SCL_BIT)
#define SCL_ALTO()  (P0DIR &= ~SCL_BIT)
#define SDA_LEE()   ((P0 & SDA_BIT) != 0)

static void i2c_start(void)
{
    SDA_ALTO(); espera(); SCL_ALTO(); espera();
    SDA_BAJO(); espera();
    SCL_BAJO(); espera();
}

static void i2c_stop(void)
{
    SDA_BAJO(); espera();
    SCL_ALTO(); espera();
    SDA_ALTO(); espera();
}

static uint8_t i2c_tx(uint8_t b)
{
    uint8_t n, ack;
    for (n = 0; n < 8; n++) {
        if (b & 0x80) SDA_ALTO(); else SDA_BAJO();
        espera();
        SCL_ALTO(); espera();
        b <<= 1;
        SCL_BAJO(); espera();
    }
    SDA_ALTO(); espera();
    SCL_ALTO(); espera();
    ack = !SDA_LEE();
    SCL_BAJO(); espera();
    return ack;
}

static uint8_t i2c_rx(uint8_t ack)
{
    uint8_t n, v = 0;
    SDA_ALTO();
    for (n = 0; n < 8; n++) {
        espera();
        SCL_ALTO(); espera();
        v = (v << 1) | SDA_LEE();
        SCL_BAJO();
    }
    if (ack) SDA_BAJO(); else SDA_ALTO();
    espera();
    SCL_ALTO(); espera();
    SCL_BAJO(); espera();
    SDA_ALTO();
    return v;
}

static __xdata uint8_t blq[16];

static uint8_t leer_bloque(uint8_t n)
{
    uint8_t i;
    i2c_start();
    if (!i2c_tx(DIR_NFC << 1) || !i2c_tx(n)) { i2c_stop(); return 0; }
    i2c_stop();
    i2c_start();
    if (!i2c_tx((DIR_NFC << 1) | 1)) { i2c_stop(); return 0; }
    for (i = 0; i < 16; i++)
        blq[i] = i2c_rx(i < 15);
    i2c_stop();
    return 1;
}

#if MODO
static uint8_t escribir_raw(uint8_t n, const uint8_t *d)
{
    uint8_t i, t;
    i2c_start();
    if (!i2c_tx(DIR_NFC << 1) || !i2c_tx(n)) { i2c_stop(); return 0; }
    for (i = 0; i < 16; i++)
        if (!i2c_tx(d[i])) { i2c_stop(); return 0; }
    i2c_stop();
    /* La EEPROM tarda ~4 ms: esperar a que vuelva a contestar */
    for (t = 0; t < 50; t++) {
        delay_ms(1);
        i2c_start();
        if (i2c_tx(DIR_NFC << 1)) { i2c_stop(); return 1; }
        i2c_stop();
    }
    return 0;
}

static uint8_t escribir_bloque(uint8_t n, const uint8_t *d)
{
    if (n == 0) return 0;                 /* el bloque 0 solo con escribir_bloque0 */
    return escribir_raw(n, d);
}

/* Bloque 0 (datasheet NT3H2111 8.3.2/8.3.6/8.3.8): byte 0 = direccion I2C << 1
   (AA para seguir en 0x55; al leerlo siempre da 04), bytes 10-11 bloqueos
   estaticos, 12-15 CC. Desde I2C los bits de bloqueo y del CC pueden volver a 0. */
static uint8_t escribir_bloque0(const uint8_t *d)
{
    if (d[0] != (DIR_NFC << 1)) return 0; /* nunca cambiar la direccion */
    return escribir_raw(0, d);
}

/* Si el chip deja de contestar en 0x55, buscar donde quedo */
static void buscar_direccion(void)
{
    uint8_t a;
    txs("  Buscando el chip en otras direcciones:");
    for (a = 0x08; a < 0x78; a++) {
        i2c_start();
        if (i2c_tx(a << 1)) { txs(" 0x"); txh(a); }
        i2c_stop();
    }
    txs("\r\n");
}
#endif

static void volcar(uint8_t n)
{
    uint8_t i;
    txs("  B"); txh(n); txs(":");
    if (!leer_bloque(n)) { txs(" (sin respuesta)\r\n"); return; }
    for (i = 0; i < 16; i++) { tx(' '); txh(blq[i]); }
    txs("  |");
    for (i = 0; i < 16; i++)
        tx(blq[i] >= 0x20 && blq[i] < 0x7F ? blq[i] : '.');
    txs("|\r\n");
}

#if MODO == 1
/* NDEF: registro de texto "HOLA DESDE EL CC2510" (idioma "es") */
static const uint8_t ndef[48] = {
    0x03, 0x1B,                               /* TLV NDEF, 27 bytes */
    0xD1, 0x01, 0x17, 'T',                    /* MB|ME|SR, TNF=1, tipo "T", payload 23 */
    0x02, 'e', 's',
    'H','O','L','A',' ','D','E','S','D','E',' ','E','L',' ','C','C','2','5','1','0',
    0xFE,                                     /* TLV terminador */
};
#elif MODO == 2
/* NDEF de URI: 0x04 = "https://" + el resto. Sustituye XXXXXXXX por el codigo de
   tu etiqueta (lo leiste con MODO 0 o con una app NFC) si quieres reponer la suya.
   Longitudes: payload = 1 + caracteres, registro = 4 + payload, TLV = registro. */
static const uint8_t ndef[48] = {
    0x03, 0x1E,
    0xD1, 0x01, 0x1A, 'U', 0x04,
    'n','f','c','.','i','m','a','g','o','t','a','g','.','c','o','m','/',
    'X','X','X','X','X','X','X','X',
    0xFE,
};
#endif

void main(void)
{
    uint8_t n;

    SLEEP &= ~0x04;
    while (!(SLEEP & 0x40))
        ;
    while (CLKCON != 0x80)
        CLKCON = 0x80;
    SLEEP |= 0x04;

    uart_init();
    P0INP &= ~(SDA_BIT | SCL_BIT);
    P2INP &= ~0x20;                       /* pull-up en puerto 0 */
    P0SEL &= ~(SDA_BIT | SCL_BIT);
    P0 &= ~(SDA_BIT | SCL_BIT);           /* latch a 0: open-drain */
    SDA_ALTO(); SCL_ALTO();

    P1SEL &= ~0x01;
    P1_0 = 1;                             /* alimentar NFC (y flash) */
    P1DIR |= 0x01;
    delay_ms(300);

    txs("\r\n=== NFC NTAG I2C (0x55) MODO "); tx('0' + MODO); txs(" ===\r\n");
    txs("Bloque 0 y NDEF:\r\n");
    for (n = 0; n < 5; n++) volcar(n);
    txs("Configuracion (1K: 38-3A, 2K: 78-7A):\r\n");
    volcar(0x38); volcar(0x39); volcar(0x3A);
    volcar(0x78); volcar(0x79); volcar(0x7A);

#if MODO == 3 || MODO == 4
    {
        /* 3 = desbloquear el lado NFC, 4 = volver a los bloqueos originales
           (los que traia la etiqueta probada: B00 bytes 10-15 = FF FF E1 10 6D 0F,
            B38 bytes 8-10 = FF 3F 7F) */
        static __xdata uint8_t b0[16], b38[16];
        uint8_t i, ok;

        if (!leer_bloque(0)) { txs("ERROR: no se lee B00\r\n"); goto fin; }
        for (i = 0; i < 16; i++) b0[i] = blq[i];
        if (!leer_bloque(0x38)) { txs("ERROR: no se lee B38\r\n"); goto fin; }
        for (i = 0; i < 16; i++) b38[i] = blq[i];
        /* Comprobar que es el chip esperado (UID) antes de tocar nada */
        if (b0[1] != UID1 || b0[2] != UID2 || b0[3] != UID3) {
            txs("ERROR: UID inesperado, no se escribe nada\r\n");
            goto fin;
        }

        b0[0] = DIR_NFC << 1;             /* 0xAA: mantener 0x55 */
#if MODO == 3
        txs("DESBLOQUEANDO lado NFC...\r\n");
        b0[10] = 0x00; b0[11] = 0x00;     /* bloqueos estaticos */
        b0[15] = 0x00;                    /* CC: acceso lectura/escritura */
        b38[8] = 0x00; b38[9] = 0x00; b38[10] = 0x00;   /* bloqueos dinamicos */
#else
        txs("REBLOQUEANDO como estaba...\r\n");
        b0[10] = 0xFF; b0[11] = 0xFF;
        b0[15] = 0x0F;
        b38[8] = 0xFF; b38[9] = 0x3F; b38[10] = 0x7F;
#endif
        ok = escribir_bloque(0x38, b38);
        txs(ok ? "  B38 escrito\r\n" : "  B38 FALLO\r\n");
        ok = escribir_bloque0(b0);
        txs(ok ? "  B00 escrito\r\n" : "  B00 FALLO\r\n");
        if (!ok) buscar_direccion();
        txs("Relectura:\r\n");
        volcar(0); volcar(0x38);
    }
fin:
#elif MODO
    txs(MODO == 1 ? "Escribiendo NDEF de prueba...\r\n" : "Restaurando URL original...\r\n");
    for (n = 0; n < 3; n++) {
        txs("  B"); txh(n + 1);
        txs(escribir_bloque(n + 1, ndef + n * 16) ? " escrito\r\n" : " FALLO\r\n");
    }
    txs("Relectura:\r\n");
    for (n = 1; n < 4; n++) volcar(n);
#endif
    txs("=== FIN ===\r\n");
    for (;;)
        ;
}
