/*
 * NFC NXP NTAG I2C plus 1K (NT3H2111) de la placa VUSION 2.6, por I2C.
 * SDA = P0_4, SCL = P0_6, direccion 0x55; se alimenta con P1_0 = 1
 * (lo hace flash_iniciar()). I2C por software, open-drain con pull-up interno.
 *
 * nfc_leer_texto() extrae el texto del primer registro NDEF de tipo 'T'
 * (memoria de usuario desde el bloque 1). Solo lee: nunca escribe el chip.
 */

#ifndef NFC_H
#define NFC_H

#include <stdint.h>
#include <cc2510fx.h>

#define NFC_DIR   0x55
#define NFC_SDA   0x10   /* P0_4 */
#define NFC_SCL   0x40   /* P0_6 */
#define NFC_BLOQUES 8    /* bloques 1-8 = 128 bytes de NDEF como maximo */

#define SDA_BAJO()  (P0DIR |= NFC_SDA)
#define SDA_ALTO()  (P0DIR &= (uint8_t)~NFC_SDA)
#define SCL_BAJO()  (P0DIR |= NFC_SCL)
#define SCL_ALTO()  (P0DIR &= (uint8_t)~NFC_SCL)
#define SDA_LEE()   ((P0 & NFC_SDA) != 0)

static void i2c_esp(void)
{
    volatile uint8_t i;
    for (i = 0; i < 60; i++)
        ;
}

static void i2c_start(void)
{
    SDA_ALTO(); i2c_esp(); SCL_ALTO(); i2c_esp();
    SDA_BAJO(); i2c_esp();
    SCL_BAJO(); i2c_esp();
}

static void i2c_stop(void)
{
    SDA_BAJO(); i2c_esp();
    SCL_ALTO(); i2c_esp();
    SDA_ALTO(); i2c_esp();
}

static uint8_t i2c_tx(uint8_t b)
{
    uint8_t n, ack;
    for (n = 0; n < 8; n++) {
        if (b & 0x80) SDA_ALTO(); else SDA_BAJO();
        i2c_esp();
        SCL_ALTO(); i2c_esp();
        b <<= 1;
        SCL_BAJO(); i2c_esp();
    }
    SDA_ALTO(); i2c_esp();
    SCL_ALTO(); i2c_esp();
    ack = !SDA_LEE();
    SCL_BAJO(); i2c_esp();
    return ack;
}

static uint8_t i2c_rx(uint8_t ack)
{
    uint8_t n, v = 0;
    SDA_ALTO();
    for (n = 0; n < 8; n++) {
        i2c_esp();
        SCL_ALTO(); i2c_esp();
        v = (v << 1) | SDA_LEE();
        SCL_BAJO();
    }
    if (ack) SDA_BAJO(); else SDA_ALTO();
    i2c_esp();
    SCL_ALTO(); i2c_esp();
    SCL_BAJO(); i2c_esp();
    SDA_ALTO();
    return v;
}

static void nfc_pines(void)
{
    P0INP &= (uint8_t)~(NFC_SDA | NFC_SCL);
    P2INP &= ~0x20;               /* pull-up en el puerto 0 */
    P0SEL &= (uint8_t)~(NFC_SDA | NFC_SCL);
    P0 &= (uint8_t)~(NFC_SDA | NFC_SCL);
    SDA_ALTO(); SCL_ALTO();
}

/* Lee un bloque de 16 bytes. 0 si el chip no contesta (p. ej. el movil lo
   esta usando por NFC en ese momento). */
static uint8_t nfc_bloque(uint8_t n, uint8_t *d)
{
    uint8_t i;
    i2c_start();
    if (!i2c_tx(NFC_DIR << 1) || !i2c_tx(n)) { i2c_stop(); return 0; }
    i2c_stop();
    i2c_start();
    if (!i2c_tx((NFC_DIR << 1) | 1)) { i2c_stop(); return 0; }
    for (i = 0; i < 16; i++)
        d[i] = i2c_rx(i < 15);
    i2c_stop();
    return 1;
}

static __xdata uint8_t nfc_mem[NFC_BLOQUES * 16];

/* Copia en txt (max-1 caracteres + 0) el texto del registro NDEF 'T'.
   Devuelve la longitud, 0 si no hay texto, 0xFF si no se pudo leer el chip. */
static uint8_t nfc_leer_texto(char *txt, uint8_t max)
{
    uint8_t i, p, len, tipo_len, plen, sr, il, lang, n;

    for (i = 0; i < NFC_BLOQUES; i++)
        if (!nfc_bloque(i + 1, nfc_mem + i * 16))
            return 0xFF;

    /* Buscar el TLV NDEF (0x03), saltando TLV nulos (0x00) y otros */
    p = 0;
    while (p < sizeof(nfc_mem) - 2) {
        if (nfc_mem[p] == 0x00) { p++; continue; }
        if (nfc_mem[p] == 0xFE) return 0;
        if (nfc_mem[p] == 0x03) break;
        p += 2 + nfc_mem[p + 1];          /* otro TLV: saltarlo */
    }
    if (p >= sizeof(nfc_mem) - 2) return 0;
    len = nfc_mem[p + 1];
    if (len == 0xFF || len < 4) return 0; /* longitud de 3 bytes: no soportada */
    p += 2;

    /* Registro: cabecera, long. tipo, long. payload (1 o 4 bytes), [ID], tipo */
    sr = nfc_mem[p] & 0x10;
    il = nfc_mem[p] & 0x08;
    if ((nfc_mem[p] & 0x07) != 0x01) return 0;    /* TNF = bien conocido */
    tipo_len = nfc_mem[p + 1];
    if (!sr) return 0;                            /* solo registros cortos */
    plen = nfc_mem[p + 2];
    p += 3;
    if (il) p += 1 + nfc_mem[p];                  /* longitud de ID: saltar */
    if (tipo_len != 1 || nfc_mem[p] != 'T') return 0;
    p += 1;

    lang = nfc_mem[p] & 0x3F;                     /* byte de estado */
    if (plen < 1 + lang) return 0;
    n = plen - 1 - lang;
    p += 1 + lang;
    if (n > max - 1) n = max - 1;
    if (p + n > sizeof(nfc_mem)) n = sizeof(nfc_mem) - p;
    for (i = 0; i < n; i++)
        txt[i] = nfc_mem[p + i];
    txt[n] = 0;
    return n;
}

#endif
