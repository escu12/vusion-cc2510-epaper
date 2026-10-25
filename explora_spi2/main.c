/*
 * Segunda busqueda de la flash SPI: bus compartido con la pantalla.
 * CLK = P0_5 y MOSI = P0_3 fijos (SPI de USART0 Alt.1, el que usa el panel);
 * se prueban todos los pares MISO / CS entre el resto de pines.
 * Pasada 0 con el alimentador del panel apagado (P0_0 = 1), pasada 1 encendido.
 *
 * P0_3 deja de ser UART, asi que los resultados van a XDATA y se leen por
 * depuracion con 'xdata' en herramienta.ino:
 *   0xFDF0 = numero de aciertos, 0xFDF1 = 0xA5 al terminar
 *   0xFD00 + 6*k = [cs, miso, pasada, id0, id1, id2]  (hasta 32 aciertos)
 */

#include <stdint.h>
#include <cc2510fx.h>

#define N 15
static const uint8_t pport[N] = {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 0, 1, 1};
static const uint8_t pbit[N]  = {2, 4, 6, 7, 0, 1, 4, 5, 6, 7, 3, 4, 1, 2, 3};
/* indices: 0 P0_2  1 P0_4  2 P0_6  3 P0_7  4 P1_0  5 P1_1  6 P1_4  7 P1_5
            8 P1_6  9 P1_7 10 P2_3 11 P2_4 12 P0_1 13 P1_2 14 P1_3        */

__xdata __at(0xFD00) uint8_t res[32 * 6];
__xdata __at(0xFDF0) volatile uint8_t aciertos;
__xdata __at(0xFDF1) volatile uint8_t terminado;

#define CLK  P0_5
#define MOSI P0_3

static void p_dir(uint8_t i, uint8_t salida)
{
    uint8_t m = 1 << pbit[i];
    switch (pport[i]) {
    case 0: if (salida) P0DIR |= m; else P0DIR &= ~m; break;
    case 1: if (salida) P1DIR |= m; else P1DIR &= ~m; break;
    default: if (salida) P2DIR |= m; else P2DIR &= ~m; break;
    }
}

static void p_out(uint8_t i, uint8_t v)
{
    uint8_t m = 1 << pbit[i];
    switch (pport[i]) {
    case 0: if (v) P0 |= m; else P0 &= ~m; break;
    case 1: if (v) P1 |= m; else P1 &= ~m; break;
    default: if (v) P2 |= m; else P2 &= ~m; break;
    }
}

static uint8_t p_in(uint8_t i)
{
    uint8_t m = 1 << pbit[i];
    switch (pport[i]) {
    case 0: return (P0 & m) != 0;
    case 1: return (P1 & m) != 0;
    default: return (P2 & m) != 0;
    }
}

static void delay_ms(uint16_t ms)
{
    volatile uint16_t i;
    while (ms--)
        for (i = 0; i < 1300; i++)
            ;
}

static uint8_t cs, mi;

static uint8_t spi(uint8_t b)
{
    uint8_t n, r = 0;
    for (n = 0; n < 8; n++) {
        MOSI = (b & 0x80) ? 1 : 0;
        b <<= 1;
        CLK = 1;
        r = (r << 1) | p_in(mi);
        CLK = 0;
    }
    return r;
}

static uint8_t id[3];

static void leer_id(void)
{
    p_out(cs, 0); spi(0xAB); spi(0); spi(0); spi(0); p_out(cs, 1);
    delay_ms(1);
    p_out(cs, 0);
    spi(0x9F);
    id[0] = spi(0); id[1] = spi(0); id[2] = spi(0);
    p_out(cs, 1);
}

void main(void)
{
    uint8_t i, pasada, a, b, c;

    aciertos = 0;
    terminado = 0;

    SLEEP &= ~0x04;
    while (!(SLEEP & 0x40))
        ;
    while (CLKCON != 0x80)
        CLKCON = 0x80;
    SLEEP |= 0x04;

    P0INP = 0;
    P1INP = 0;
    P2INP = 0;                    /* pulls activos, pull-up */
    P0SEL = 0;
    P1SEL = 0;
    P2SEL = 0;
    PERCFG = 0;
    U0CSR = 0;

    CLK = 0; MOSI = 0;
    P0DIR |= 0x28;                /* P0_3 y P0_5 salidas */

    for (pasada = 0; pasada < 2; pasada++) {
        P0_0 = pasada ? 0 : 1;    /* alimentador del panel (activo bajo) */
        P0DIR |= 0x01;
        delay_ms(50);

        for (cs = 0; cs < N; cs++)
            for (mi = 0; mi < N; mi++) {
                if (mi == cs)
                    continue;
                for (i = 0; i < N; i++)
                    p_dir(i, 0);
                p_out(cs, 1);
                p_dir(cs, 1);
                leer_id();
                a = id[0]; b = id[1]; c = id[2];
                if (a == 0x00 || a == 0xFF)
                    continue;
                if (b == 0x00 && c == 0x00)      /* solo bits sueltos: no es un ID */
                    continue;
                leer_id();
                if (id[0] != a || id[1] != b || id[2] != c)
                    continue;
                if (aciertos < 32) {
                    uint8_t k = aciertos * 6;
                    res[k] = cs; res[k + 1] = mi; res[k + 2] = pasada;
                    res[k + 3] = a; res[k + 4] = b; res[k + 5] = c;
                }
                aciertos++;
            }
    }
    P0_0 = 1;
    terminado = 0xA5;
    for (;;)
        ;
}
