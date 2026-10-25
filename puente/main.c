/*
 * Puente radio <-> UART en una placa VUSION sin pantalla, con memoria de
 * textos NFC.
 *
 * UART 115200 8N1 (datasheet CC2510, tablas 50 y 55):
 *   P0_3 = TX de USART0 (Alt.1)  -> ESP32 GPIO16 (RX2)
 *   P0_5 = RX de USART1 (Alt.1)  <- ESP32 GPIO17 (TX2)
 *
 * Modem de peticion/respuesta para el ESP32 (sin cambios):
 *   ESP32 -> puente : 0x7E n  trama[n]            (trama = dst tipo seq datos...)
 *   puente -> ESP32 : 0x7E m  trama[m] rssi lqi   (lo recibido por radio tras emitir)
 *                     0x7E 0x00                   (nada recibido en la ventana)
 *   Al arrancar     : 0x7E 0x01 'B' 0x00 0x00
 *
 * Ademas, por su cuenta:
 *   - Cada segundo lee el NFC de la placa (NTAG I2C). Si el movil ha escrito un
 *     texto nuevo, lo guarda en la flash W25X10 en un anillo de 10 posiciones
 *     (un sector de 4 KB por posicion; el 11.o sustituye al mas antiguo).
 *   - Cuando cambia la memoria (y una vez al arrancar) envia la lista entera a
 *     la etiqueta ETIQUETA: una trama 'L' por texto (el mas nuevo primero) y una
 *     'P' para que pinte, cada una con reintentos hasta recibir su ACK. Si falla
 *     (p. ej. la etiqueta esta refrescando) lo reintenta cada 30 s.
 *
 * Diagnostico en XDATA (leer con 'progreso'/'xdata' en herramienta.ino):
 *   0xFDF0 progreso, 0xFDF2 tramas UART, 0xFDF3 respuestas,
 *   0xFDF4 textos guardados, 0xFDF5 posicion mas nueva, 0xFDF6 ultimo envio
 *   de la lista (1 = completo, 0 = fallo), 0xFDF7 lecturas NFC fallidas, 0xFDF8-A JEDEC ID.
 */

#include <stdint.h>
#include <cc2510fx.h>
#include "radio.h"
#include "flash.h"
#include "nfc.h"

#define VENTANA_RESPUESTA  150000UL   /* sondeos de RX tras emitir */
#define SOF                0x7E

#define ETIQUETA     0x01             /* destino de los envios automaticos */
#define POSICIONES   10
#define TEXTO_MAX    100              /* caracteres guardados por texto */
#define L_COLUMNAS   48               /* ancho de linea de la lista en el receptor */

/* Tics del Sleep Timer con WOR_RES = 2^10: 1024 / (26 MHz / 750) = 29,5 ms */
#define TIC_SEGUNDO  34
#define TIC_REINTENTO 1016            /* 30 s */

/* Cabecera de cada posicion en la flash */
#define MAGICO       0xA5
/* [0] MAGICO, [1..2] numero de secuencia, [3] longitud, [4..] texto */

__xdata __at(0xFDF0) volatile uint8_t progreso;
__xdata __at(0xFDF2) volatile uint8_t tramas_uart;
__xdata __at(0xFDF3) volatile uint8_t respuestas;
__xdata __at(0xFDF4) volatile uint8_t guardados;
__xdata __at(0xFDF5) volatile uint8_t pos_nueva;
__xdata __at(0xFDF6) volatile uint8_t ultimo_envio;
__xdata __at(0xFDF7) volatile uint8_t fallos_nfc;
__xdata __at(0xFDF8) volatile uint8_t jedec[3];

static __xdata uint8_t buf[RADIO_MAX];
static __xdata uint8_t ent[4 + TEXTO_MAX];
static __xdata char txt_nfc[TEXTO_MAX + 1];
static __xdata char txt_ant[TEXTO_MAX + 1];   /* lectura anterior (estabilidad) */

static uint16_t seq_flash;      /* secuencia del texto mas nuevo */
static uint8_t pendiente;       /* hay que enviar la lista */
static uint8_t seq_radio;

/* ---------- Tiempo ---------- */
static uint16_t ahora(void)
{
    uint8_t lo = WORTIME0;        /* leer primero el byte bajo */
    uint8_t hi = WORTIME1;
    return ((uint16_t)hi << 8) | lo;
}

/* ---------- UART ---------- */
static void uart_init(void)
{
    PERCFG &= ~0x03;          /* U0CFG = U1CFG = Alt.1 */
    P0SEL |= 0x28;            /* P0_3 y P0_5 a periferico */
    P0DIR |= 0x08;            /* P0_3 salida */

    U0CSR = 0x80;             /* USART0 en UART, receptor apagado (solo TX) */
    U0UCR = 0x02;             /* 8N1, bit de stop a nivel alto, sin flujo */
    U0BAUD = 34;              /* 115200 a 26 MHz */
    U0GCR = 12;

    U1CSR = 0xC0;             /* USART1 en UART, receptor encendido (solo RX) */
    U1UCR = 0x02;
    U1BAUD = 34;
    U1GCR = 12;

    UTX0IF = 0;
    URX1IF = 0;
}

static void uart_tx(uint8_t b)
{
    UTX0IF = 0;
    U0DBUF = b;
    while (!UTX0IF)
        ;
    UTX0IF = 0;
}

static uint8_t uart_rx(uint8_t *b)
{
    uint16_t t = 0;
    while (!URX1IF)
        if (++t == 0)
            return 0;
    URX1IF = 0;
    *b = U1DBUF;
    return 1;
}

static void responder(const uint8_t *d, uint8_t n)
{
    uint8_t i;
    uart_tx(SOF);
    uart_tx(n);
    if (!n)
        return;
    for (i = 0; i < n; i++)
        uart_tx(d[i]);
    uart_tx(radio_rssi);
    uart_tx(radio_lqi);
}

/* Trama del ESP32 (ya llego un byte: URX1IF activo) */
static void atender_uart(void)
{
    uint8_t n, i, m;

    URX1IF = 0;
    if (U1DBUF != SOF)
        return;
    if (!uart_rx(&n) || n == 0 || n > RADIO_MAX)
        return;
    for (i = 0; i < n; i++)
        if (!uart_rx(&buf[i]))
            return;

    tramas_uart++;
    radio_tx(buf, n);
    m = radio_rx(buf, VENTANA_RESPUESTA);
    if (m)
        respuestas++;
    responder(buf, m);
}

/* ---------- Anillo de textos en la flash ---------- */
static uint32_t dir_pos(uint8_t p) { return (uint32_t)p * FLASH_SECTOR; }

/* Recorre las 10 posiciones y localiza la mas nueva */
static void anillo_cargar(void)
{
    uint8_t p, cab[4];
    uint16_t s;

    guardados = 0;
    pos_nueva = POSICIONES - 1;
    seq_flash = 0;
    for (p = 0; p < POSICIONES; p++) {
        flash_leer(dir_pos(p), cab, 4);
        if (cab[0] != MAGICO || cab[3] > TEXTO_MAX)
            continue;
        guardados++;
        s = cab[1] | ((uint16_t)cab[2] << 8);
        if (guardados == 1 || (int16_t)(s - seq_flash) > 0) {
            seq_flash = s;
            pos_nueva = p;
        }
    }
}

/* Posicion fisica del k-esimo texto (0 = mas antiguo) */
static uint8_t pos_de(uint8_t k)
{
    return (pos_nueva + POSICIONES + 1 - guardados + k) % POSICIONES;
}

static uint8_t leer_texto(uint8_t p, char *t)
{
    uint8_t n;
    flash_leer(dir_pos(p), ent, 4);
    if (ent[0] != MAGICO || ent[3] > TEXTO_MAX) { t[0] = 0; return 0; }
    n = ent[3];
    flash_leer(dir_pos(p) + 4, (uint8_t *)t, n);
    t[n] = 0;
    return n;
}

static void anillo_guardar(const char *t, uint8_t n)
{
    uint8_t p = (pos_nueva + 1) % POSICIONES, i;

    seq_flash++;
    ent[0] = MAGICO;
    ent[1] = seq_flash;
    ent[2] = seq_flash >> 8;
    ent[3] = n;
    for (i = 0; i < n; i++)
        ent[4 + i] = t[i];

    flash_borrar_sector(dir_pos(p));       /* el 11.o borra el mas antiguo */
    flash_escribir(dir_pos(p), ent, 4 + n);
    pos_nueva = p;
    if (guardados < POSICIONES)
        guardados++;
    pendiente = 1;
}

static uint8_t iguales(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void copiar(char *d, const char *s)
{
    while ((*d++ = *s++))
        ;
}

/* Lee el NFC; guarda el texto si es nuevo y se ha leido igual dos veces */
static void revisar_nfc(void)
{
    static __xdata char ultimo[TEXTO_MAX + 1];
    uint8_t n;

    n = nfc_leer_texto(txt_nfc, TEXTO_MAX + 1);
    if (n == 0xFF) { fallos_nfc++; return; }
    if (n == 0) return;

    if (!iguales(txt_nfc, txt_ant)) {      /* cambio: esperar a otra lectura igual */
        copiar(txt_ant, txt_nfc);
        return;
    }
    if (guardados) {
        leer_texto(pos_nueva, ultimo);
        if (iguales(txt_nfc, ultimo))      /* ya es el mas nuevo guardado */
            return;
    }
    anillo_guardar(txt_nfc, n);
}

/* ---------- Envio por radio ---------- */
/* Envia una trama con reintentos hasta recibir su ACK */
static uint8_t enviar_trama(uint8_t *trama, uint8_t n)
{
    uint8_t intento, m;
    volatile uint16_t i;

    seq_radio++;
    trama[0] = ETIQUETA;
    trama[2] = seq_radio;
    for (intento = 0; intento < 5; intento++) {
        radio_tx(trama, n);
        m = radio_rx(buf, VENTANA_RESPUESTA);
        if (m >= 4 && buf[1] == TIPO_ACK && buf[2] == seq_radio)
            return 1;
        for (i = 0; i < 40000; i++)        /* pausa antes de reintentar */
            ;
    }
    return 0;
}

/* Lista completa, el mas nuevo primero, y orden de pintar */
static uint8_t enviar_lista(void)
{
    static __xdata uint8_t trama[RADIO_MAX];
    static __xdata char t[TEXTO_MAX + 1];
    uint8_t k, n;

    for (k = 0; k < guardados; k++) {
        n = leer_texto(pos_de(guardados - 1 - k), t);
        if (n > L_COLUMNAS) n = L_COLUMNAS;
        trama[1] = TIPO_LINEA;
        trama[3] = k;
        trama[4] = guardados;
        for (uint8_t i = 0; i < n; i++)
            trama[5 + i] = t[i];
        if (!enviar_trama(trama, 5 + n))
            return 0;
    }
    trama[1] = TIPO_PINTAR;
    return enviar_trama(trama, 3);
}

void main(void)
{
    uint16_t t_nfc, t_env, t;
    uint32_t id;

    progreso = 0x30;
    tramas_uart = 0;
    respuestas = 0;
    fallos_nfc = 0;
    ultimo_envio = 0xFF;
    seq_radio = 0;

    /* Sin pantalla: dejar apagado su alimentador (P0_0 activo bajo) */
    P0_0 = 1;
    P0DIR |= 0x01;

    radio_reloj_xosc();
    radio_init();
    uart_init();
    WORCTL = 0x02;                 /* Sleep Timer: 1 tic = 2^10 ciclos del RC */

    id = flash_iniciar();          /* enciende flash y NFC (P1_0) */
    jedec[0] = id >> 16;
    jedec[1] = id >> 8;
    jedec[2] = id;
    nfc_pines();
    txt_ant[0] = 0;
    anillo_cargar();
    pendiente = guardados != 0;    /* mostrar la lista guardada al arrancar */
    progreso = 0x31;

    radio_rssi = radio_lqi = 0;
    buf[0] = 'B';
    responder(buf, 1);

    t_nfc = t_env = ahora();
    for (;;) {
        progreso = 0x32;
        if (URX1IF) {
            progreso = 0x33;
            atender_uart();
        }
        t = ahora();
        if ((uint16_t)(t - t_nfc) >= TIC_SEGUNDO) {
            t_nfc = t;
            progreso = 0x34;
            revisar_nfc();
        }
        /* Lista pendiente: enviarla ya si es el primer intento, si no cada 30 s */
        if (pendiente && (ultimo_envio != 0 || (uint16_t)(t - t_env) >= TIC_REINTENTO)) {
            t_env = t;
            progreso = 0x35;
            pendiente = 0;
            ultimo_envio = enviar_lista();
            if (!ultimo_envio)
                pendiente = 1;
        }
    }
}
