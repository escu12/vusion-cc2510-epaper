/*
 * Etiqueta receptora VUSION 2.6 BWR.
 * Escucha la radio y contesta ACK a las tramas dirigidas a MI_ID (o a todas):
 *   'T' texto suelto ('|' separa hasta 4 lineas grandes): se pinta enseguida.
 *   'L' una linea de la lista [indice][total][texto]: se guarda sin pintar.
 *       El indice 0 borra la lista anterior.
 *   'P' pintar la lista guardada (hasta 10 lineas de 48 caracteres, la 0 en rojo).
 *   'E' preparar imagen: borra su zona de la flash W25X10 (tarda ~0,5 s).
 *   'I' trozo de imagen [pos alta][pos baja][bytes]: se guarda en la flash.
 *   'M' pintar la imagen guardada (2 planos de 5624 bytes ya en el orden del
 *       panel: 296 lineas x 19 bytes, primero negro y luego rojo).
 * Antes de pintar algo recibido, el LED blanco da 3 destellos.
 * Al arrancar muestra una pantalla de espera.
 *
 * Progreso en XDATA 0xFDF0 (leer con 'progreso' en herramienta.ino):
 *   0x10 arrancado, 0x11 pantalla de espera pintada, 0x12 escuchando,
 *   0x20 trama recibida, 0x21 pintado, 0x8x timeout de BUSY.
 * 0xFDF2 = tramas validas recibidas (contador). 0xFDF4-6 = JEDEC ID de la flash.
 */

#include <stdint.h>
#include "epd.h"
#include "radio.h"
#include "flash.h"

#define MI_ID  0x01

__xdata __at(0xFDF0) volatile uint8_t progreso;
__xdata __at(0xFDF2) volatile uint8_t tramas;
__xdata __at(0xFDF4) volatile uint8_t jedec[3];

/* Imagen en la flash: sectores 16-18 (los 0-9 los usa el puente para el NFC) */
#define IMG_DIR    0x10000UL
#define IMG_PLANO  5624U          /* 296 lineas x 19 bytes */

/* Modo grande: 4 lineas de 24 caracteres a escala 2 */
#define LINEAS     4
#define COLUMNAS   24
/* Modo lista: 10 lineas de 48 caracteres a escala 1 */
#define L_LINEAS   10
#define L_COLUMNAS 48
#define L_Y0       4
#define L_PASO     14

static __xdata char lineas[LINEAS][COLUMNAS + 1];
static __xdata char lista[L_LINEAS][L_COLUMNAS + 1];
static __xdata char pie[L_COLUMNAS + 1];
static __xdata uint8_t trama[RADIO_MAX];
static uint8_t modo_lista;
static uint8_t total_lista;

/* ---------- Contenido ---------- */
/* Linea de la lista en la que cae y (0xFF = ninguna) y su y0 */
static uint8_t linea_lista(uint8_t y, uint8_t *y0)
{
    uint8_t l;
    if (y < L_Y0) return 0xFF;
    l = (y - L_Y0) / L_PASO;
    if (l >= L_LINEAS) return 0xFF;
    *y0 = L_Y0 + l * L_PASO;
    if (y - *y0 >= 7) return 0xFF;
    return l;
}

static uint8_t px_negro(uint16_t x, uint8_t y)
{
    uint8_t l, y0;
    if (modo_lista) {
        l = linea_lista(y, &y0);
        if (l == 0xFF || l == 0)            /* la 0 (la mas nueva) va en rojo */
            return 0;
        return texto(lista[l], 4, y0, 0, x, y);
    }
    if (x < 2 || x >= ANCHO - 2 || y < 2 || y >= ALTO - 2)
        return 1;
    for (l = 0; l < LINEAS; l++)
        if (texto(lineas[l], 4, 10 + l * 26, 1, x, y))
            return 1;
    return 0;
}

static uint8_t px_rojo(uint16_t x, uint8_t y)
{
    uint8_t l, y0;
    if (modo_lista) {
        l = linea_lista(y, &y0);
        if (l == 0)
            return texto(lista[0], 4, y0, 0, x, y);
        if (y == L_Y0 + L_PASO - 4 && x >= 4 && x < ANCHO - 4)
            return 1;                       /* raya bajo la mas nueva */
        return texto(pie, 4, ALTO - 9, 0, x, y);
    }
    if (y >= 118 && y < 121 && x >= 4 && x < ANCHO - 4)
        return 1;
    return texto(pie, 6, 130, 0, x, y);
}

/* ---------- Utilidades de texto ---------- */
static void copiar_n(char *dst, const char *src, uint8_t max)
{
    uint8_t i = 0;
    while (src[i] && i < max) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static char *poner_num(char *p, int16_t v)
{
    char tmp[6];
    uint8_t n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { tmp[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

static char *poner_txt(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

/* Reparte el texto de la trama en lineas separadas por '|' */
static void repartir(const uint8_t *d, uint8_t n)
{
    uint8_t l, c = 0, i;
    for (l = 0; l < LINEAS; l++) lineas[l][0] = 0;
    l = 0;
    for (i = 0; i < n && l < LINEAS; i++) {
        if (d[i] == '|') { lineas[l][c] = 0; l++; c = 0; continue; }
        if (c < COLUMNAS) { lineas[l][c++] = d[i]; lineas[l][c] = 0; }
    }
}

static void pie_estado(uint8_t seq, uint8_t rssi)
{
    char *p = pie;
    p = poner_txt(p, modo_lista ? "NFC " : "ID 1  RSSI ");
    if (modo_lista) {
        p = poner_num(p, total_lista);
        p = poner_txt(p, "/10   RSSI ");
    }
    p = poner_num(p, radio_dbm(rssi));
    p = poner_txt(p, " DBM  #");
    p = poner_num(p, seq);
    *p = 0;
}

/* LED blanco (confirmado con prueba_led): se enciende con P2_2 = 1 (enable
   del elevador TPS61071) y P2_1 = 1. Son tambien DC y DD de depuracion. */
static void led_init(void)
{
    P2SEL &= ~0x06;
    P2_1 = 0;
    P2_2 = 0;
    P2DIR |= 0x06;
}

static void destellos(uint8_t n)
{
    while (n--) {
        P2_2 = 1; P2_1 = 1;
        delay_ms(80);
        P2_1 = 0; P2_2 = 0;
        delay_ms(150);
    }
}

static void pintar(void)
{
    destellos(3);                   /* aviso de mensaje recibido */
    progreso = epd_pintar() ? 0x21 : 0x82;
}

/* ---------- Imagen en la flash ---------- */
static void imagen_borrar(void)
{
    uint8_t s;
    for (s = 0; s < 3; s++)         /* 11 248 bytes caben en 3 sectores de 4 KB */
        flash_borrar_sector(IMG_DIR + s * FLASH_SECTOR);
}

/* Guarda un trozo sin cruzar limites de pagina de 256 bytes */
static void imagen_trozo(uint16_t pos, const uint8_t *d, uint8_t n)
{
    uint32_t a = IMG_DIR + pos;
    uint8_t hueco;
    if (pos + n > 2 * IMG_PLANO)
        return;
    hueco = 256 - (a & 0xFF);
    if (n > hueco) {
        flash_escribir(a, d, hueco);
        a += hueco; d += hueco; n -= hueco;
    }
    flash_escribir(a, d, n);
}

/* Envia un plano al panel leyendolo de la flash byte a byte */
static void plano_flash(uint32_t dir)
{
    uint16_t i;
    EPD_DC = 1;
    EPD_CS = 0;
    FL_CS = 0;
    fl_spi(0x03);
    fl_dir(dir);
    for (i = 0; i < IMG_PLANO; i++)
        epd_spi(fl_spi(0));
    FL_CS = 1;
    EPD_CS = 1;
}

static void pintar_imagen(void)
{
    destellos(3);
    epd_inicio();
    epd_cmd(0x10);
    plano_flash(IMG_DIR);
    epd_cmd(0x13);
    plano_flash(IMG_DIR + IMG_PLANO);
    progreso = epd_final() ? 0x21 : 0x82;
}

void main(void)
{
    uint8_t n, seq, ultimo = 0xFF, l;
    uint8_t ack[4];

    progreso = 0x10;
    tramas = 0;
    modo_lista = 0;
    total_lista = 0;
    radio_reloj_xosc();
    epd_pines();
    led_init();
    radio_init();
    {
        uint32_t id = flash_iniciar();      /* P1_0: enciende flash (y NFC) */
        jedec[0] = id >> 16;
        jedec[1] = id >> 8;
        jedec[2] = id;
    }

    copiar_n(lineas[0], "ESPERANDO RADIO...", COLUMNAS);
    lineas[1][0] = 0;
    copiar_n(lineas[2], "2433 MHZ 250 KBPS", COLUMNAS);
    lineas[3][0] = 0;
    copiar_n(pie, "ID 1  RECEPTOR VUSION", L_COLUMNAS);
    for (l = 0; l < L_LINEAS; l++) lista[l][0] = 0;
    if (!epd_pintar()) progreso = 0x81;
    else progreso = 0x11;

    for (;;) {
        progreso = (progreso & 0x80) | 0x12;
        n = radio_rx(trama, 0);
        if (n < 3)
            continue;
        if (trama[0] != MI_ID && trama[0] != DST_TODAS)
            continue;
        if (trama[1] != TIPO_TEXTO && trama[1] != TIPO_LINEA && trama[1] != TIPO_PINTAR &&
            trama[1] != TIPO_PREPARAR && trama[1] != TIPO_IMAGEN && trama[1] != TIPO_MOSTRAR)
            continue;

        tramas++;
        progreso = 0x20;
        seq = trama[2];

        /* ACK inmediato (antes de los ~20 s de refresco) */
        ack[0] = DST_PUENTE;
        ack[1] = TIPO_ACK;
        ack[2] = seq;
        ack[3] = radio_rssi;
        radio_tx(ack, 4);

        if (seq == ultimo)          /* reintento del emisor: ya procesado */
            continue;
        ultimo = seq;

        if (trama[1] == TIPO_LINEA && n >= 5) {
            uint8_t k;
            l = trama[3];
            if (l == 0)
                for (k = 0; k < L_LINEAS; k++) lista[k][0] = 0;
            if (l < L_LINEAS) {
                trama[n < RADIO_MAX ? n : RADIO_MAX - 1] = 0;
                copiar_n(lista[l], (char *)trama + 5, L_COLUMNAS);
                total_lista = trama[4];
            }
        } else if (trama[1] == TIPO_PINTAR) {
            modo_lista = 1;
            pie_estado(seq, radio_rssi);
            pintar();
        } else if (trama[1] == TIPO_TEXTO) {
            modo_lista = 0;
            repartir(trama + 3, n - 3);
            pie_estado(seq, radio_rssi);
            pintar();
        } else if (trama[1] == TIPO_PREPARAR) {
            imagen_borrar();
        } else if (trama[1] == TIPO_IMAGEN && n > 5) {
            uint16_t pos = ((uint16_t)trama[3] << 8) | trama[4];
            uint8_t k;
            for (k = 5; k < n; k++)       /* deshacer la mezcla (ver radio.h) */
                trama[k] ^= mezcla[(pos + k - 5) & 63];
            imagen_trozo(pos, trama + 5, n - 5);
        } else if (trama[1] == TIPO_MOSTRAR) {
            pintar_imagen();
        }
    }
}
