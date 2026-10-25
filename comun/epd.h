/*
 * Pantalla SE2266FS093 (PDi 2,66" BWR, 152 x 296) para VUSION 2.6 / CC2510.
 * Pinout y secuencia confirmados con hola_mundo (ver CLAUDE.md).
 *
 * El fichero que lo incluye debe definir:
 *   static uint8_t px_negro(uint16_t x, uint8_t y);
 *   static uint8_t px_rojo(uint16_t x, uint8_t y);
 * con coordenadas logicas apaisadas (ANCHO x ALTO = 296 x 152).
 *
 * Requiere reloj de cristal a 26 MHz (radio_reloj_xosc()) para que delay_ms cuadre.
 */

#ifndef EPD_H
#define EPD_H

#include <stdint.h>
#include <cc2510fx.h>

#define EPD_PWR   P0_0   /* activo bajo */
#define EPD_CS    P0_1
#define EPD_SDI   P0_3
#define EPD_CLK   P0_5
#define EPD_DC    P1_2
#define EPD_BUSY  P1_3   /* bajo = ocupado */
#define EPD_RST   P2_0

#define ANCHO     296
#define ALTO      152
#define ESPEJO_X  1
#define NEGRO_ES_1 1
#define ROJO_ES_1  1

static uint8_t px_negro(uint16_t x, uint8_t y);
static uint8_t px_rojo(uint16_t x, uint8_t y);

/* Aproximado para 26 MHz */
static void delay_ms(uint16_t ms)
{
    volatile uint16_t i;
    while (ms--)
        for (i = 0; i < 1300; i++)
            ;
}

static void epd_spi(uint8_t b)
{
    uint8_t n;
    for (n = 0; n < 8; n++) {
        EPD_SDI = (b & 0x80) ? 1 : 0;
        EPD_CLK = 1;
        b <<= 1;
        EPD_CLK = 0;
    }
}

static void epd_cmd(uint8_t c)
{
    EPD_DC = 0;
    EPD_CS = 0;
    epd_spi(c);
    EPD_CS = 1;
}

static void epd_cmd_dato(uint8_t c, uint8_t d)
{
    epd_cmd(c);
    EPD_DC = 1;
    EPD_CS = 0;
    epd_spi(d);
    EPD_CS = 1;
}

static uint8_t epd_esperar(void)
{
    uint16_t t = 40000;
    delay_ms(1);
    while (!EPD_BUSY) {
        delay_ms(1);
        if (--t == 0)
            return 0;
    }
    return 1;
}

/* Pines del panel como GPIO y panel apagado */
static void epd_pines(void)
{
    P0SEL &= ~0x2B;
    P1SEL &= ~0x0C;
    P2SEL &= ~0x01;
    EPD_PWR = 1;
    EPD_CS = 1;
    EPD_CLK = 0;
    EPD_SDI = 0;
    EPD_DC = 0;
    EPD_RST = 0;
    P0DIR |= 0x2B;
    P1DIR |= 0x04;
    P1DIR &= ~0x08;
    P2DIR |= 0x01;
}

static void epd_plano(uint8_t rojo)
{
    uint16_t linea, x;
    uint8_t b, bit, v, p;

    EPD_DC = 1;
    EPD_CS = 0;
    for (linea = 0; linea < ANCHO; linea++) {
        x = ESPEJO_X ? (ANCHO - 1 - linea) : linea;
        for (b = 0; b < ALTO / 8; b++) {
            v = 0;
            for (bit = 0; bit < 8; bit++) {
                p = rojo ? px_rojo(x, (b << 3) | bit)
                         : px_negro(x, (b << 3) | bit);
                v = (v << 1) | p;
            }
            if (rojo ? !ROJO_ES_1 : !NEGRO_ES_1)
                v = ~v;
            epd_spi(v);
        }
    }
    EPD_CS = 1;
}

/* Alimentar, resetear e inicializar el panel (antes de enviar los planos) */
static void epd_inicio(void)
{
    EPD_PWR = 0;
    delay_ms(20);
    EPD_RST = 1;  delay_ms(5);
    EPD_RST = 0;  delay_ms(10);
    EPD_RST = 1;  delay_ms(5);

    epd_cmd_dato(0x00, 0x0E);   /* soft reset */
    delay_ms(5);
    epd_cmd_dato(0xE5, 0x19);   /* temperatura: 25 C */
    epd_cmd_dato(0xE0, 0x02);   /* activar temperatura */
}

/* Refrescar con los planos ya enviados y apagar. 0 si BUSY no se libera. */
static uint8_t epd_final(void)
{
    uint8_t ok = 1;

    epd_cmd(0x04);
    if (!epd_esperar()) ok = 0;
    epd_cmd(0x12);
    if (ok && !epd_esperar()) ok = 0;
    epd_cmd(0x02);
    epd_esperar();

    EPD_RST = 0;
    EPD_PWR = 1;
    return ok;
}

/* Refresco completo generando los pixeles con px_negro/px_rojo. Unos 20 s. */
static uint8_t epd_pintar(void)
{
    epd_inicio();
    epd_cmd(0x10);
    epd_plano(0);
    epd_cmd(0x13);
    epd_plano(1);
    return epd_final();
}

/* ---------- Fuente 5x7 ASCII 0x20-0x5A (minusculas -> mayusculas) ---------- */
static const uint8_t fuente[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x56,0x20,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00}, {0x2A,0x1C,0x7F,0x1C,0x2A}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},                                                   /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E},                                                   /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00},
    {0x14,0x14,0x14,0x14,0x14}, {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},                                                   /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},                       /* Z */
};

static uint8_t glifo_col(char c, uint8_t col)
{
    if (c >= 'a' && c <= 'z')
        c -= 32;
    if (c < 0x20 || c > 0x5A)
        c = '?';
    return fuente[c - 0x20][col];
}

/* 1 si (x,y) cae en un pixel encendido del texto s (escala 1<<esc) */
static uint8_t texto(const char *s, uint16_t x0, uint8_t y0, uint8_t esc,
                     uint16_t x, uint8_t y)
{
    uint16_t cx;
    uint8_t cy, ci, col;
    if (x < x0 || y < y0)
        return 0;
    cy = (y - y0) >> esc;
    if (cy >= 7)
        return 0;
    cx = (x - x0) >> esc;
    ci = cx / 6;
    col = cx % 6;
    if (col == 5)
        return 0;
    while (ci--) {
        if (!*s)
            return 0;
        s++;
    }
    if (!*s)
        return 0;
    return (glifo_col(*s, col) >> cy) & 1;
}

#endif
