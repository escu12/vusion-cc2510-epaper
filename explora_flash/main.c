/*
 * Prueba dirigida de la flash SPI "1H820" (USON-8 junto al CC2510).
 * Patillaje medido por el usuario: 1 CS# -> P1_4, 2 DO -> P1_7, 4 GND,
 * 5 DI -> P1_6, 6 CLK -> (resistencia) P1_5, 7 HOLD# = 8 VCC.
 * = SPI de USART1 en Alt.2 (datasheet tabla 50). Aqui por software, modo 0.
 *
 * Como no contesta con los pines en reposo, se supone que su VCC pasa por un
 * interruptor (chip de 6 patas). Se prueba cada pin libre como encendido,
 * a 1 y a 0, y se lee el JEDEC ID (0x9F) y el ID de fabricante (0x90).
 * Informe por el UART del puente (P0_3) -> 'm' en herramienta.ino.
 */

#include <stdint.h>
#include <cc2510fx.h>

#define F_CS    P1_4
#define F_CLK   P1_5
#define F_MOSI  P1_6
#define F_MISO  P1_7

#define N 11
static const char *const nombre[N] = {
    "P0_0", "P0_1", "P0_2", "P0_7", "P1_0", "P1_1",
    "P1_2", "P1_3", "P2_0", "P2_3", "P2_4"
};
static const uint8_t pport[N] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2};
static const uint8_t pbit[N]  = {0, 1, 2, 7, 0, 1, 2, 3, 0, 3, 4};

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

static void delay_ms(uint16_t ms)
{
    volatile uint16_t i;
    while (ms--)
        for (i = 0; i < 1300; i++)
            ;
}

static uint8_t spi(uint8_t b)
{
    uint8_t n, r = 0;
    for (n = 0; n < 8; n++) {
        F_MOSI = (b & 0x80) ? 1 : 0;
        b <<= 1;
        F_CLK = 1;
        r = (r << 1) | F_MISO;
        F_CLK = 0;
    }
    return r;
}

static void probar(void)
{
    uint8_t a, b, c, m, d;
    F_CS = 0; spi(0xAB); spi(0); spi(0); spi(0); F_CS = 1;      /* salir de deep power-down */
    delay_ms(1);
    F_CS = 0; spi(0x9F); a = spi(0); b = spi(0); c = spi(0); F_CS = 1;
    F_CS = 0; spi(0x90); spi(0); spi(0); spi(0); m = spi(0); d = spi(0); F_CS = 1;
    txs(" JEDEC="); txh(a); txh(b); txh(c);
    txs(" REMS="); txh(m); txh(d);
    if (a != 0x00 && a != 0xFF && (b || c))
        txs("   <<< FLASH");
    txs("\r\n");
}

void main(void)
{
    uint8_t i, e, nivel;

    SLEEP &= ~0x04;
    while (!(SLEEP & 0x40))
        ;
    while (CLKCON != 0x80)
        CLKCON = 0x80;
    SLEEP |= 0x04;

    uart_init();
    P0INP = 0;
    P1INP = 0;
    P2INP = 0;                    /* pull-ups */
    P1SEL &= ~0xF0;
    F_CS = 1; F_CLK = 0; F_MOSI = 0;
    P1DIR = (P1DIR | 0x70) & ~0x80;   /* CS, CLK, MOSI salida; MISO entrada */

    delay_ms(300);
    txs("\r\n=== FLASH SPI: CS P1_4 CLK P1_5 MOSI P1_6 MISO P1_7 ===\r\n");

    for (i = 0; i < N; i++) { p_dir(i, 0); p_out(i, 0); }
    txs("  sin encender nada       :");
    probar();

    for (e = 0; e < N; e++)
        for (nivel = 0; nivel < 2; nivel++) {
            for (i = 0; i < N; i++) p_dir(i, 0);
            p_out(e, nivel);
            p_dir(e, 1);
            delay_ms(30);
            txs("  "); txs(nombre[e]); txs(nivel ? " = 1" : " = 0");
            txs("               :");
            probar();
            p_dir(e, 0);
            delay_ms(30);
        }
    txs("=== FIN ===\r\n");
    for (;;)
        ;
}
