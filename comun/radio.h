/*
 * Radio del CC2510 (nucleo tipo CC2500) por sondeo de RFTXRXIF, sin DMA.
 * 2433 MHz, 250 kbps MSK, longitud variable, CRC, RSSI/LQI anexados.
 * Ambos extremos (puente y etiquetas) usan exactamente esta configuracion.
 *
 * Trama en el aire: [len][dst][tipo][seq][datos...]   (len = bytes tras len)
 */

#ifndef RADIO_H
#define RADIO_H

#include <stdint.h>
#include <cc2510fx.h>

#define RADIO_MAX      60     /* bytes tras len */

#define DST_PUENTE     0x00
#define DST_TODAS      0xFF

#define TIPO_TEXTO     'T'    /* datos = texto, '|' separa lineas */
#define TIPO_ACK       'A'    /* datos = [rssi recibido] */
#define TIPO_LINEA     'L'    /* datos = [indice][total][texto]: una linea de la lista */
#define TIPO_PINTAR    'P'    /* sin datos: pintar la lista recibida */
#define TIPO_PREPARAR  'E'    /* sin datos: borrar la zona de imagen de la flash */
#define TIPO_IMAGEN    'I'    /* datos = [pos alta][pos baja][bytes]: trozo de imagen */
#define TIPO_MOSTRAR   'M'    /* sin datos: pintar la imagen guardada */

/* Los datos de imagen van mezclados (XOR) con esta tabla, indexada por la
   posicion del byte en la imagen: la radio no tiene activado el blanqueado y
   las rachas largas de 00/FF (zonas blancas o negras) llegaban corruptas.
   La misma tabla esta en herramienta.ino. */
static const uint8_t mezcla[64] = {
    0x3C, 0xA3, 0x34, 0x72, 0xD7, 0xFB, 0xE1, 0x7A, 0x01, 0x29, 0x38, 0x93, 0x32, 0xE6, 0x05, 0xFB,
    0xA0, 0x6B, 0xCB, 0x80, 0xB2, 0xB6, 0xC0, 0x27, 0xAE, 0x2D, 0x95, 0x93, 0xEA, 0x48, 0x9E, 0x0C,
    0xBC, 0xBA, 0xEC, 0xD8, 0x2E, 0xCC, 0xFF, 0x3B, 0xD9, 0xFB, 0xCB, 0x84, 0xD7, 0xF5, 0x0C, 0x72,
    0x42, 0x19, 0x34, 0xDB, 0xF0, 0x48, 0xF6, 0x75, 0x3E, 0xE9, 0xF0, 0x80, 0xCD, 0x9D, 0xF5, 0xCD,
};

#define RFST_SIDLE     0x04
#define RFST_SRX       0x02
#define RFST_STX       0x03
#define RFIF_IRQ_DONE  0x10

/* Cristal de 26 MHz como reloj del sistema (obligatorio para la radio) */
/* El cambio solo se hace efectivo cuando el HS RCOSC ha terminado de calibrarse,
   y CLKSPD/OSC solo se leen con el valor nuevo cuando el reloj ya ha cambiado.
   Esperar solo al bit OSC y apagar el RC enseguida dejaba la mitad de los
   arranques a 13 MHz (UART a media velocidad). */
static void radio_reloj_xosc(void)
{
    uint16_t t;
    SLEEP &= ~0x04;               /* encender XOSC */
    while (!(SLEEP & 0x40))       /* XOSC_STB */
        ;
    do {
        CLKCON = 0x80;            /* OSC = XOSC, TICKSPD = CLKSPD = 26 MHz */
        for (t = 0; t < 2000 && CLKCON != 0x80; t++)
            ;
    } while (CLKCON != 0x80);
    SLEEP |= 0x04;                /* ahora si: apagar el RC que no se usa */
}

static void radio_init(void)
{
    SYNC1    = 0xD3;
    SYNC0    = 0x91;
    PKTLEN   = RADIO_MAX;
    PKTCTRL1 = 0x04;              /* anexar RSSI + LQI/CRC_OK */
    PKTCTRL0 = 0x05;              /* longitud variable, CRC */
    ADDR     = 0x00;
    CHANNR   = 0x00;
    FSCTRL1  = 0x0A;
    FSCTRL0  = 0x00;
    FREQ2    = 0x5D;              /* 2433 MHz */
    FREQ1    = 0x93;
    FREQ0    = 0xB1;
    MDMCFG4  = 0x2D;              /* 250 kbps */
    MDMCFG3  = 0x3B;
    MDMCFG2  = 0x73;              /* MSK, sync 30/32 */
    MDMCFG1  = 0x22;
    MDMCFG0  = 0xF8;
    DEVIATN  = 0x00;
    MCSM1    = 0x00;              /* tras RX y TX -> IDLE */
    MCSM0    = 0x14;              /* calibrar al salir de IDLE */
    FOCCFG   = 0x1D;
    BSCFG    = 0x1C;
    AGCCTRL2 = 0xC7;
    AGCCTRL1 = 0x00;
    AGCCTRL0 = 0xB2;
    FREND1   = 0xB6;
    FREND0   = 0x10;
    FSCAL3   = 0xEA;
    FSCAL2   = 0x0A;
    FSCAL1   = 0x00;
    FSCAL0   = 0x11;
    PA_TABLE0 = 0xFE;             /* ~ +1 dBm */
    RFST = RFST_SIDLE;
}

static void radio_tx(const uint8_t *d, uint8_t n)
{
    uint8_t i;
    uint16_t t;

    RFST = RFST_SIDLE;
    RFIF &= ~RFIF_IRQ_DONE;
    RFTXRXIF = 0;
    RFST = RFST_STX;
    for (i = 0; i <= n; i++) {
        t = 0;
        while (!RFTXRXIF)
            if (++t == 0) { RFST = RFST_SIDLE; return; }
        RFTXRXIF = 0;
        RFD = i ? d[i - 1] : n;
    }
    t = 0;
    while (!(RFIF & RFIF_IRQ_DONE))
        if (++t == 0) break;
    RFIF &= ~RFIF_IRQ_DONE;
    RFST = RFST_SIDLE;
}

static uint8_t radio_rssi, radio_lqi;

/* Espera una trama valida (CRC OK). ventana = numero de sondeos antes de rendirse
   (0 = esperar siempre). Devuelve la longitud o 0. */
static uint8_t radio_rx(uint8_t *d, uint32_t ventana)
{
    uint8_t i, n, b;
    uint16_t t;

    RFST = RFST_SIDLE;
    RFIF &= ~RFIF_IRQ_DONE;
    RFTXRXIF = 0;
    RFST = RFST_SRX;

    while (!RFTXRXIF)
        if (ventana && --ventana == 0) { RFST = RFST_SIDLE; return 0; }
    RFTXRXIF = 0;
    n = RFD;
    if (n == 0 || n > RADIO_MAX) { RFST = RFST_SIDLE; return 0; }

    for (i = 0; i < n + 2; i++) {
        t = 0;
        while (!RFTXRXIF)
            if (++t == 0) { RFST = RFST_SIDLE; return 0; }
        RFTXRXIF = 0;
        b = RFD;
        if (i < n) d[i] = b;
        else if (i == n) radio_rssi = b;
        else radio_lqi = b;
    }
    RFST = RFST_SIDLE;
    return (radio_lqi & 0x80) ? n : 0;
}

/* RSSI en dBm (offset tipico 71 dB a 250 kbps) */
static int16_t radio_dbm(uint8_t r)
{
    return (int16_t)((int8_t)r) / 2 - 71;
}

#endif
