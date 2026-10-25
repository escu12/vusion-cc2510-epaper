/*
 * Busca una flash SPI (chip "1H820", USON/WSON-8 junto al CC2510) probando
 * todas las asignaciones CS/CLK/MOSI/MISO entre los pines libres y leyendo
 * el JEDEC ID (0x9F). Antes manda 0xAB (salir de deep power-down).
 * Informa por el UART del puente (P0_3) -> 'm' en herramienta.ino.
 */

#include <stdint.h>
#include <cc2510fx.h>

#define N 12
static const char *const nombre[N] = {
    "P0_2", "P0_4", "P0_6", "P0_7", "P1_0", "P1_1",
    "P1_4", "P1_5", "P1_6", "P1_7", "P2_3", "P2_4"
};
static const uint8_t pport[N] = {0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2};
static const uint8_t pbit[N]  = {2, 4, 6, 7, 0, 1, 4, 5, 6, 7, 3, 4};

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

static uint8_t cs, ck, mo, mi;

/* SPI modo 0 por software */
static uint8_t spi(uint8_t b)
{
    uint8_t n, r = 0;
    for (n = 0; n < 8; n++) {
        p_out(mo, b & 0x80);
        b <<= 1;
        p_out(ck, 1);
        r = (r << 1) | p_in(mi);
        p_out(ck, 0);
    }
    return r;
}

static void preparar(void)
{
    uint8_t i;
    for (i = 0; i < N; i++)
        p_dir(i, 0);              /* resto en entrada con pull-up */
    p_out(cs, 1); p_dir(cs, 1);
    p_out(ck, 0); p_dir(ck, 1);
    p_out(mo, 0); p_dir(mo, 1);
}

static uint8_t id[3];

static void leer_id(void)
{
    p_out(cs, 0); spi(0xAB); spi(0); spi(0); spi(0); p_out(cs, 1);   /* despertar */
    delay_ms(1);
    p_out(cs, 0);
    spi(0x9F);
    id[0] = spi(0); id[1] = spi(0); id[2] = spi(0);
    p_out(cs, 1);
}

void main(void)
{
    uint16_t hallados = 0;
    uint8_t ok;

    SLEEP &= ~0x04;
    while (!(SLEEP & 0x40))
        ;
    /* esperar a que CLKCON lea 0x80 (cambio hecho); ver comun/radio.h */
    while (CLKCON != 0x80)
        CLKCON = 0x80;
    SLEEP |= 0x04;

    P0_0 = 1;
    P0DIR |= 0x01;
    uart_init();
    P0INP = 0;
    P1INP = 0;
    P2INP &= ~0xFF;               /* pulls activos, todos pull-up */
    P0SEL &= ~0xD4;
    P1SEL &= ~0xF3;
    delay_ms(300);
    txs("\r\n=== BUSQUEDA FLASH SPI (JEDEC 0x9F) ===\r\n");

    for (cs = 0; cs < N; cs++)
    for (ck = 0; ck < N; ck++) {
        if (ck == cs) continue;
        for (mo = 0; mo < N; mo++) {
            if (mo == cs || mo == ck) continue;
            for (mi = 0; mi < N; mi++) {
                if (mi == cs || mi == ck || mi == mo) continue;
                preparar();
                leer_id();
                /* Valido: fabricante no 00/FF y la lectura se repite */
                if (id[0] == 0x00 || id[0] == 0xFF)
                    continue;
                ok = id[0];
                leer_id();
                if (id[0] != ok)
                    continue;
                hallados++;
                if (hallados > 40) continue;
                txs("  CS="); txs(nombre[cs]);
                txs(" CLK="); txs(nombre[ck]);
                txs(" MOSI="); txs(nombre[mo]);
                txs(" MISO="); txs(nombre[mi]);
                txs("  ID="); txh(id[0]); txh(id[1]); txh(id[2]);
                txs("\r\n");
            }
        }
    }
    txs("Combinaciones con respuesta: ");
    txh(hallados >> 8); txh(hallados & 0xFF);
    txs("\r\n=== FIN ===\r\n");
    for (;;)
        ;
}
