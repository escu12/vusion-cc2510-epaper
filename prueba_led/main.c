/*
 * Prueba del LED blanco de la VUSION 2.6. Segun la placa hermana GL120:
 * P2_1 = LED (comparte con DD de depuracion), P2_2 = enable del elevador del
 * LED TPS61071 (comparte con DC). Sin confirmar en RFRTx024A.
 *
 * Para cada combinacion pinta en la pantalla "PRUEBA n" y los niveles, y
 * despues hace parpadear esa combinacion ~30 s (150 ms encendido, 350 ms en
 * reposo con P2_1 = P2_2 = 0). Al final pinta "FIN". Unos 3 minutos en total.
 *   PRUEBA 1: P2_2 = 1, P2_1 = 1
 *   PRUEBA 2: P2_2 = 1, P2_1 = 0
 *   PRUEBA 3: P2_2 = 0, P2_1 = 1
 */

#include <stdint.h>
#include "epd.h"
#include "radio.h"

static __xdata char linea1[25], linea2[25], linea3[25];

static uint8_t px_negro(uint16_t x, uint8_t y)
{
    if (x < 2 || x >= ANCHO - 2 || y < 2 || y >= ALTO - 2)
        return 1;
    return texto(linea1, 10, 20, 2, x, y) || texto(linea3, 10, 110, 1, x, y);
}

static uint8_t px_rojo(uint16_t x, uint8_t y)
{
    return texto(linea2, 10, 70, 1, x, y);
}

static void poner(char *d, const char *s)
{
    while ((*d++ = *s++))
        ;
}

static void reposo(void) { P2_2 = 0; P2_1 = 0; }

static void prueba(char n, uint8_t en, uint8_t led)
{
    uint8_t i;

    poner(linea1, "PRUEBA 0");
    linea1[7] = n;
    poner(linea2, "P2.2=0  P2.1=0");     /* la fuente no tiene '_' */
    linea2[5] = '0' + en;
    linea2[13] = '0' + led;
    poner(linea3, "MIRA EL LED (30 S)");
    reposo();
    epd_pintar();

    for (i = 0; i < 60; i++) {
        P2_2 = en;
        P2_1 = led;
        delay_ms(150);
        reposo();
        delay_ms(350);
    }
}

void main(void)
{
    radio_reloj_xosc();          /* 26 MHz: delay_ms de epd.h cuenta con ello */
    epd_pines();

    P2SEL &= ~0x06;
    reposo();
    P2DIR |= 0x06;               /* P2_1 y P2_2 salidas */

    prueba('1', 1, 1);
    prueba('2', 1, 0);
    prueba('3', 0, 1);

    poner(linea1, "FIN");
    poner(linea2, "DIME EN CUAL");
    poner(linea3, "PARPADEO EL LED");
    epd_pintar();
    for (;;)
        ;
}
