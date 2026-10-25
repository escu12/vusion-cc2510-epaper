/*
 * Exploracion de pines libres del CC2510 para identificar el chip "1H820"
 * (LGA ~12 pads junto al micro). Informa en texto por el UART del puente
 * (P0_3 TX, 115200) -> leer con 'm' en herramienta.ino.
 *
 * 1) Nivel de cada pin con pull-up y con pull-down.
 * 2) Barrido I2C (direcciones 0x08-0x77) en todas las parejas SDA/SCL.
 * 3) Si no aparece nada: lo mismo poniendo cada pin a 1 como posible VDD.
 * Para cada dispositivo encontrado lee los registros 0x00, 0x0F y 0x75.
 *
 * No toca P0_3/P0_5 (UART), P2_1/P2_2 (depuracion) ni las lineas de la pantalla.
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

/* ---------- UART (solo TX por P0_3) ---------- */
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

/* ---------- Acceso generico a pines ---------- */
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

static void espera(uint8_t n)
{
    volatile uint8_t i;
    for (i = 0; i < n; i++)
        ;
}

static void delay_ms(uint16_t ms)
{
    volatile uint16_t i;
    while (ms--)
        for (i = 0; i < 1300; i++)
            ;
}

/* Todos los candidatos como entrada con pull-up, latch a 0 (para open-drain) */
static void soltar_todo(void)
{
    uint8_t i;
    for (i = 0; i < N; i++) {
        p_dir(i, 0);
        p_out(i, 0);
    }
}

/* ---------- I2C por software, open-drain con pull-up interno ---------- */
static uint8_t sda, scl;
#define D() espera(60)     /* I2C lento (~5 kHz) para no confundir subidas lentas con ACK */

static void sda_bajo(void) { p_dir(sda, 1); }
static void sda_alto(void) { p_dir(sda, 0); }
static void scl_bajo(void) { p_dir(scl, 1); }
static void scl_alto(void) { p_dir(scl, 0); D(); }

static void i2c_start(void)
{
    sda_alto(); scl_alto(); D();
    sda_bajo(); D();
    scl_bajo(); D();
}

static void i2c_stop(void)
{
    sda_bajo(); D();
    scl_alto(); D();
    sda_alto(); D();
}

static uint8_t i2c_byte(uint8_t b)    /* devuelve 1 si hay ACK */
{
    uint8_t n, ack;
    for (n = 0; n < 8; n++) {
        if (b & 0x80) sda_alto(); else sda_bajo();
        D();
        scl_alto();
        b <<= 1;
        scl_bajo();
    }
    sda_alto(); D();
    scl_alto();
    ack = !p_in(sda);
    scl_bajo(); D();
    return ack;
}

static uint8_t i2c_leer(void)
{
    uint8_t n, v = 0;
    sda_alto();
    for (n = 0; n < 8; n++) {
        scl_alto();
        v = (v << 1) | p_in(sda);
        scl_bajo(); D();
    }
    sda_alto(); D();                 /* NACK */
    scl_alto();
    scl_bajo(); D();
    return v;
}

static uint8_t leer_reg(uint8_t dir, uint8_t reg, uint8_t *v)
{
    uint8_t ok;
    i2c_start();
    ok = i2c_byte(dir << 1) && i2c_byte(reg);
    if (ok) {
        i2c_start();
        ok = i2c_byte((dir << 1) | 1);
        if (ok) *v = i2c_leer();
    }
    i2c_stop();
    return ok;
}

static uint8_t encontrados;
static uint8_t acks[0x78 / 8];   /* mapa de bits de direcciones con ACK */

/* Un dispositivo real responde a 1-4 direcciones; si responden muchas, es una
   linea lenta o cargada, no un chip. Solo se detallan las parejas con pocas. */
static void barrer_pareja(void)
{
    uint8_t dir, v, n = 0;
    sda_alto(); scl_alto(); D(); D();
    if (!p_in(sda) || !p_in(scl))
        return;
    for (dir = 0; dir < sizeof(acks); dir++)
        acks[dir] = 0;
    for (dir = 0x08; dir < 0x78; dir++) {
        i2c_start();
        v = i2c_byte(dir << 1);
        i2c_stop();
        if (v) { acks[dir >> 3] |= 1 << (dir & 7); n++; }
    }
    if (!n)
        return;
    txs("  SDA="); txs(nombre[sda]); txs(" SCL="); txs(nombre[scl]);
    if (n > 6) {
        txs(" ACK en "); txh(n); txs(" direcciones (falso)\r\n");
        return;
    }
    txs("\r\n");
    for (dir = 0x08; dir < 0x78; dir++) {
        if (!(acks[dir >> 3] & (1 << (dir & 7))))
            continue;
        encontrados++;
        txs("    *** dir=0x"); txh(dir);
        if (leer_reg(dir, 0x0F, &v)) { txs(" r0F="); txh(v); }
        if (leer_reg(dir, 0x00, &v)) { txs(" r00="); txh(v); }
        if (leer_reg(dir, 0x75, &v)) { txs(" r75="); txh(v); }
        txs("\r\n");
    }
}

/* Ciclos hasta que el pin sube con el pull-up tras tenerlo a 0 (0xFFFF = no sube) */
static uint16_t subida(uint8_t i)
{
    uint16_t t = 0;
    p_out(i, 0);
    p_dir(i, 1);
    delay_ms(1);
    p_dir(i, 0);
    while (!p_in(i))
        if (++t == 0xFFFF) break;
    return t;
}

static void barrido_i2c(uint8_t vdd)   /* vdd = indice del pin de alimentacion o 0xFF */
{
    for (sda = 0; sda < N; sda++)
        for (scl = 0; scl < N; scl++) {
            if (sda == scl || sda == vdd || scl == vdd)
                continue;
            barrer_pareja();
            sda_alto();
            scl_alto();
        }
}

void main(void)
{
    uint8_t i, pu, pd, v;

    /* Cristal de 26 MHz (para los baudios) */
    SLEEP &= ~0x04;
    while (!(SLEEP & 0x40))
        ;
    /* esperar a que CLKCON lea 0x80 (cambio hecho); ver comun/radio.h */
    while (CLKCON != 0x80)
        CLKCON = 0x80;
    SLEEP |= 0x04;

    /* Alimentador de pantalla apagado (P0_0 activo bajo) */
    P0_0 = 1;
    P0DIR |= 0x01;

    uart_init();
    delay_ms(300);
    txs("\r\n=== EXPLORACION CC2510 ===\r\n");

    P0INP = 0;               /* pulls activos en todos los puertos */
    P1INP = 0;
    P2INP &= ~0x1F;
    P0SEL &= ~0xD4;          /* candidatos de P0 como GPIO */
    P1SEL &= ~0xF3;
    P2SEL &= ~0x00;
    soltar_todo();

    txs("1) Niveles con pull-up / pull-down (P1_0 y P1_1 no tienen pulls):\r\n");
    for (i = 0; i < N; i++) {
        P2INP &= ~0xE0;      /* pull-up en P0, P1, P2 */
        delay_ms(2);
        pu = p_in(i);
        P2INP |= 0xE0;       /* pull-down */
        delay_ms(2);
        pd = p_in(i);
        txs("  "); txs(nombre[i]);
        txs(" PU="); tx('0' + pu);
        txs(" PD="); tx('0' + pd);
        if (pu && pd) txs("  <- forzado a 1");
        else if (!pu && !pd) txs("  <- forzado a 0");
        txs("\r\n");
    }
    P2INP &= ~0xE0;          /* volver a pull-up para I2C */

    txs("1b) Tiempo de subida con pull-up (bucles; mayor = linea cargada):\r\n");
    for (i = 0; i < N; i++) {
        uint16_t t = subida(i);
        txs("  "); txs(nombre[i]); txs(" ");
        txh(t >> 8); txh(t & 0xFF);
        txs("\r\n");
    }
    soltar_todo();

    txs("2) Barrido I2C sin alimentar desde GPIO...\r\n");
    encontrados = 0;
    barrido_i2c(0xFF);

    {
        txs("3) Probando cada pin como VDD...\r\n");
        for (v = 0; v < N; v++) {
            soltar_todo();
            p_out(v, 1);
            p_dir(v, 1);
            delay_ms(20);
            txs("  VDD="); txs(nombre[v]); txs("\r\n");
            barrido_i2c(v);
            p_out(v, 0);
        }
        soltar_todo();
    }

    txs("Encontrados: ");
    txh(encontrados);
    txs("\r\n=== FIN ===\r\n");
    for (;;)
        ;
}
