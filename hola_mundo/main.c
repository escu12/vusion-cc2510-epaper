/*
 * Hola mundo para VUSION 2.6 BWR (CC2510 + Pervasive Displays SE2266FS093)
 * Compilar con SDCC (ver build.ps1).
 *
 * Pantalla: 152 x 296 (152 = fuente/columnas, 296 = lineas de puerta).
 * Se dibuja en apaisado (296 x 152 logicos) generando los pixeles al vuelo,
 * porque el framebuffer (2 x 5624 bytes) no cabe en los 4 KB de RAM.
 *
 * Pines (de la placa hermana GL120, PENDIENTES DE CONFIRMAR en RFRTx024A):
 *   P0_0 EPD power enable (activo bajo)   P0_1 CS    P0_3 SDI   P0_5 CLK
 *   P1_2 DC    P1_3 BUSY (bajo = ocupado)   P2_0 RESET
 *
 * Progreso: el byte XDATA 0xFDF0 guarda por donde va el programa, para leerlo
 * despues por el interfaz de depuracion (la RAM se conserva al entrar en debug).
 */

#include <stdint.h>

/* ---------- SFR del CC2510 ---------- */
__sfr __at(0x80) P0;
__sfr __at(0x90) P1;
__sfr __at(0xA0) P2;
__sfr __at(0xFD) P0DIR;
__sfr __at(0xFE) P1DIR;
__sfr __at(0xFF) P2DIR;
__sfr __at(0xF3) P0SEL;
__sfr __at(0xF4) P1SEL;
__sfr __at(0xF5) P2SEL;

__sbit __at(0x80) EPD_PWR;    /* P0_0 */
__sbit __at(0x81) EPD_CS;     /* P0_1 */
__sbit __at(0x83) EPD_SDI;    /* P0_3 */
__sbit __at(0x85) EPD_CLK;    /* P0_5 */
__sbit __at(0x92) EPD_DC;     /* P1_2 */
__sbit __at(0x93) EPD_BUSY;   /* P1_3 */
__sbit __at(0xA0) EPD_RST;    /* P2_0 */

/* ---------- Diagnostico ---------- */
#define PROG_INICIO      0x01
#define PROG_RESET_OK    0x02
#define PROG_INIT_OK     0x03
#define PROG_NEGRO_OK    0x04
#define PROG_ROJO_OK     0x05
#define PROG_POWERON_OK  0x06
#define PROG_REFRESH_OK  0x07
#define PROG_FIN         0x08
#define PROG_TIMEOUT     0x80  /* | paso en el que se agoto el tiempo */

__xdata __at(0xFDF0) volatile uint8_t progreso;
__xdata __at(0xFDF1) volatile uint8_t busy_al_empezar;  /* nivel de BUSY tras el reset */

/* Polaridad de los planos: cambiar si sale invertido */
#define NEGRO_ES_1  1
#define ROJO_ES_1   1

/* ---------- Retardos (aprox., reloj RC de 13 MHz tras reset) ---------- */
static void delay_ms(uint16_t ms)
{
    volatile uint16_t i;
    while (ms--)
        for (i = 0; i < 650; i++)
            ;
}

/* ---------- SPI por software (modo 0, MSB primero) ---------- */
static void spi_byte(uint8_t b)
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
    spi_byte(c);
    EPD_CS = 1;
}

static void epd_cmd_dato(uint8_t c, uint8_t d)
{
    epd_cmd(c);
    EPD_DC = 1;
    EPD_CS = 0;
    spi_byte(d);
    EPD_CS = 1;
}

/* BUSY bajo = ocupado. Devuelve 0 si se agota el tiempo (~40 s). */
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

/* ---------- Fuente 5x7 (columnas, bit 0 arriba) ---------- */
static const char letras[] = " !.+0125ACDGHILMNOPSUV6";
static const uint8_t fuente[][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, /*   */
    {0x00, 0x00, 0x5F, 0x00, 0x00}, /* ! */
    {0x00, 0x60, 0x60, 0x00, 0x00}, /* . */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* + */
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
};

static uint8_t glifo_col(char c, uint8_t col)
{
    uint8_t i;
    for (i = 0; letras[i]; i++)
        if (letras[i] == c)
            return fuente[i][col];
    return 0;
}

/* Texto escalado x(1<<esc). Devuelve 1 si (x,y) cae en un pixel encendido. */
static uint8_t texto(const char *s, uint16_t x0, uint8_t y0, uint8_t esc,
                     uint16_t x, uint8_t y)
{
    uint16_t cx;
    uint8_t cy, ci, col;
    if (x < x0 || y < y0)
        return 0;
    cx = (x - x0) >> esc;
    cy = (y - y0) >> esc;
    if (cy >= 7)
        return 0;
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

/* ---------- Contenido (coordenadas logicas 296 x 152) ---------- */
#define ANCHO 296
#define ALTO  152

static const char txt1[] = "HOLA MUNDO!";   /* 11 car x 24 px = 264 */
static const char txt2[] = "CC2510 + VUSION 2.6";

static uint8_t px_negro(uint16_t x, uint8_t y)
{
    /* marco de 3 px */
    if (x < 3 || x >= ANCHO - 3 || y < 3 || y >= ALTO - 3)
        return 1;
    return texto(txt1, 18, 40, 2, x, y);
}

static uint8_t px_rojo(uint16_t x, uint8_t y)
{
    /* barra roja bajo el titulo + subtitulo */
    if (x >= 18 && x < 278 && y >= 76 && y < 82)
        return 1;
    return texto(txt2, 34, 100, 1, x, y);
}

/* Envia un plano: 296 lineas x 19 bytes. linea -> x logico, bit -> y logico.
   ESPEJO_X invierte el orden de las lineas (la primera prueba salio en espejo) */
#define ESPEJO_X 1

static void enviar_plano(uint8_t rojo)
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
            spi_byte(v);
        }
    }
    EPD_CS = 1;
}

void main(void)
{
    progreso = PROG_INICIO;

    /* GPIO: P0_0/1/3/5 salida, P1_2 salida, P1_3 entrada, P2_0 salida */
    P0SEL &= ~0x2B;
    P1SEL &= ~0x0C;
    P2SEL &= ~0x01;
    EPD_PWR = 1;            /* apagada */
    EPD_CS = 1;
    EPD_CLK = 0;
    EPD_SDI = 0;
    EPD_DC = 0;
    EPD_RST = 0;
    P0DIR |= 0x2B;
    P1DIR |= 0x04;
    P1DIR &= ~0x08;
    P2DIR |= 0x01;

    /* Alimentar el panel y secuencia de reset de PDi */
    EPD_PWR = 0;
    delay_ms(20);
    EPD_RST = 1;  delay_ms(5);
    EPD_RST = 0;  delay_ms(10);
    EPD_RST = 1;  delay_ms(5);
    busy_al_empezar = EPD_BUSY;
    progreso = PROG_RESET_OK;

    /* Inicializacion (familia iTC "small" de Pervasive Displays) */
    epd_cmd_dato(0x00, 0x0E);   /* soft reset */
    delay_ms(5);
    epd_cmd_dato(0xE5, 0x19);   /* temperatura: 25 C */
    epd_cmd_dato(0xE0, 0x02);   /* activar temperatura */
    progreso = PROG_INIT_OK;

    epd_cmd(0x10);              /* primer plano: blanco/negro */
    enviar_plano(0);
    progreso = PROG_NEGRO_OK;

    epd_cmd(0x13);              /* segundo plano: rojo */
    enviar_plano(1);
    progreso = PROG_ROJO_OK;

    epd_cmd(0x04);              /* DC/DC on */
    if (!epd_esperar()) { progreso = PROG_TIMEOUT | PROG_POWERON_OK; goto fin; }
    progreso = PROG_POWERON_OK;

    epd_cmd(0x12);              /* refresco */
    if (!epd_esperar()) { progreso = PROG_TIMEOUT | PROG_REFRESH_OK; goto fin; }
    progreso = PROG_REFRESH_OK;

    epd_cmd(0x02);              /* DC/DC off */
    epd_esperar();
    progreso = PROG_FIN;

fin:
    EPD_RST = 0;
    EPD_PWR = 1;                /* cortar alimentacion del panel */
    for (;;)
        ;
}
