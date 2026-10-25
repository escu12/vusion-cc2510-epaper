/*
 * Flash SPI Winbond W25X10 (128 KB) de la placa VUSION 2.6 (chip marcado "1H820").
 * Patillaje medido: CS# P1_4, CLK P1_5, DI(MOSI) P1_6, DO(MISO) P1_7, WP# y VCC
 * alimentados desde P1_0 (P1_0 = 1 la enciende; alimenta tambien el NFC).
 * SPI por software, modo 0. Paginas de 256 B, sectores de 4 KB.
 */

#ifndef FLASH_H
#define FLASH_H

#include <stdint.h>
#include <cc2510fx.h>

#define FL_CS    P1_4
#define FL_CLK   P1_5
#define FL_MOSI  P1_6
#define FL_MISO  P1_7
#define FL_VCC   P1_0

#define FLASH_SECTOR 4096UL

static uint8_t fl_spi(uint8_t b)
{
    uint8_t n, r = 0;
    for (n = 0; n < 8; n++) {
        FL_MOSI = (b & 0x80) ? 1 : 0;
        b <<= 1;
        FL_CLK = 1;
        r = (r << 1) | FL_MISO;
        FL_CLK = 0;
    }
    return r;
}

static void fl_dir(uint32_t a)
{
    fl_spi(a >> 16);
    fl_spi(a >> 8);
    fl_spi(a);
}

static void fl_esperar(void)
{
    FL_CS = 0;
    fl_spi(0x05);                 /* RDSR */
    while (fl_spi(0) & 0x01)      /* BUSY */
        ;
    FL_CS = 1;
}

static void fl_wren(void)
{
    FL_CS = 0; fl_spi(0x06); FL_CS = 1;
}

/* Enciende la flash (y el NFC) y la saca de deep power-down.
   Devuelve el JEDEC ID en 24 bits (0xEF3011 para la W25X10). */
static uint32_t flash_iniciar(void)
{
    volatile uint16_t i;
    uint32_t id;

    P1SEL &= ~0xF1;
    FL_CS = 1; FL_CLK = 0; FL_MOSI = 0;
    FL_VCC = 1;
    P1DIR = (P1DIR | 0x71) & ~0x80;   /* P1_0, CS, CLK, MOSI salida; MISO entrada */
    for (i = 0; i < 20000; i++)       /* ~10 ms para que suba VCC */
        ;
    FL_CS = 0; fl_spi(0xAB); fl_dir(0); FL_CS = 1;
    for (i = 0; i < 200; i++)
        ;
    FL_CS = 0;
    fl_spi(0x9F);
    id = (uint32_t)fl_spi(0) << 16;
    id |= (uint16_t)fl_spi(0) << 8;
    id |= fl_spi(0);
    FL_CS = 1;
    return id;
}

static void flash_leer(uint32_t a, uint8_t *d, uint16_t n)
{
    FL_CS = 0;
    fl_spi(0x03);
    fl_dir(a);
    while (n--)
        *d++ = fl_spi(0);
    FL_CS = 1;
}

static void flash_borrar_sector(uint32_t a)
{
    fl_wren();
    FL_CS = 0; fl_spi(0x20); fl_dir(a); FL_CS = 1;
    fl_esperar();
}

/* n <= 256 y sin cruzar un limite de pagina */
static void flash_escribir(uint32_t a, const uint8_t *d, uint16_t n)
{
    fl_wren();
    FL_CS = 0;
    fl_spi(0x02);
    fl_dir(a);
    while (n--)
        fl_spi(*d++);
    FL_CS = 1;
    fl_esperar();
}

#endif
