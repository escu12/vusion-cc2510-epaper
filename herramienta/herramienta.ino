/*
 * Herramienta unica VUSION para ESP32 DevKit V1: grabador del CC2510 + consola
 * del puente (antes eran dos sketches separados; ya no hay que cambiar de
 * sketch). El firmware llega por el puerto serie desde el PC (grabar.ps1), sin
 * recompilar el ESP32.
 *
 * Cableado:
 *   GPIO25 -> RST   GPIO26 -> DC   GPIO27 -> DD (directo)   GPIO32 -[1k]-> DD
 *     (la placa que se quiera grabar: puente o etiqueta, nunca las dos a la vez)
 *   GPIO16 (RX2) <- P0_3 del puente      GPIO17 (TX2) -> P0_5 del puente
 *
 * Monitor serie a 115200, fin de linea "Nueva linea":
 *  Radio (por el puente):
 *   t TEXTO     manda TEXTO a la etiqueta destino ('|' separa lineas, max 4 x 24)
 *   d N         cambia la etiqueta destino (1..254, 255 = todas)
 *   ping        trama vacia, para ver si el puente contesta
 *  Placa conectada a RST/DC/DD:
 *   r           reset normal y espera el saludo del puente (si es el puente)
 *   x           reset y volcado hex del UART2 durante 2 s
 *   m [seg]     reset y monitor de texto del UART2 (firmwares de diagnostico)
 *   escuchar [s] volcado hex del UART2 sin reiniciar nada (por defecto 30 s)
 *   info        chip ID, estado y si esta bloqueado
 *   grabar      graba el firmware recibido, verifica y arranca
 *   verificar   compara la flash con el firmware recibido
 *   borrar      chip erase (desbloquea; se pierde todo lo que hubiera)
 *   progreso    lee XDATA 0xFDF0/0xFDF1 (resetea el CC2510 y lo deja parado)
 *   xdata       vuelca XDATA 0xFD00-0xFDFF (idem)
 *   run         reset normal (arranca el firmware)
 *   vel N       microsegundos por medio ciclo de DC (por defecto 2)
 *  Imagen (la usa imagen.ps1):
 *   IMG <bytes> <suma> y los 11 248 bytes de la imagen (2 planos en el orden del
 *                      panel); la envia por radio a la etiqueta destino.
 *  Carga del firmware (lo usa grabar.ps1):
 *   W <bytes> <suma>   y a continuacion los bytes en binario; suma = suma de
 *                      todos los bytes modulo 2^32. Responde "OK RECIBIDO".
 */

#define PIN_RST   25
#define PIN_DC    26
#define PIN_DD_I  27
#define PIN_DD_O  32
#define PIN_RX2   16
#define PIN_TX2   17

#define FW_MAX    32768
#define PAGINA    1024

#define SOF        0x7E
#define RADIO_MAX  60
#define TIPO_TEXTO 'T'
#define TIPO_ACK   'A'
#define REINTENTOS 5
#define TIPO_PREPARAR 'E'
#define TIPO_IMAGEN   'I'
#define TIPO_MOSTRAR  'M'
#define IMG_BYTES     11248      // 2 planos x 296 lineas x 19 bytes
#define IMG_TROZO     55         // RADIO_MAX - 5 bytes de cabecera

// Mezcla (XOR por posicion) de los datos de imagen: la radio no tiene
// blanqueado y las rachas largas de 00/FF llegaban corruptas. Misma tabla que
// comun/radio.h; la etiqueta la deshace.
static const uint8_t mezcla[64] = {
  0x3C, 0xA3, 0x34, 0x72, 0xD7, 0xFB, 0xE1, 0x7A, 0x01, 0x29, 0x38, 0x93, 0x32, 0xE6, 0x05, 0xFB,
  0xA0, 0x6B, 0xCB, 0x80, 0xB2, 0xB6, 0xC0, 0x27, 0xAE, 0x2D, 0x95, 0x93, 0xEA, 0x48, 0x9E, 0x0C,
  0xBC, 0xBA, 0xEC, 0xD8, 0x2E, 0xCC, 0xFF, 0x3B, 0xD9, 0xFB, 0xCB, 0x84, 0xD7, 0xF5, 0x0C, 0x72,
  0x42, 0x19, 0x34, 0xDB, 0xF0, 0x48, 0xF6, 0x75, 0x3E, 0xE9, 0xF0, 0x80, 0xCD, 0x9D, 0xF5, 0xCD,
};

int T = 2;                      // microsegundos por medio ciclo de DC (probado a 2 y 5)
static uint8_t fw[FW_MAX];      // firmware recibido por el puerto serie
static uint32_t fwLen = 0;
static uint8_t img[IMG_BYTES];  // imagen recibida por el puerto serie

uint8_t destino = 1;
uint8_t seq = 0;

/* =================== Interfaz de depuracion del CC2510 =================== */
void ddOut() { pinMode(PIN_DD_O, OUTPUT); }
void ddIn()  { pinMode(PIN_DD_O, INPUT); }

void ccWrite(uint8_t b) {
  ddOut();
  for (int i = 0; i < 8; i++) {
    digitalWrite(PIN_DD_O, (b & 0x80) ? HIGH : LOW);
    digitalWrite(PIN_DC, HIGH); delayMicroseconds(T);
    b <<= 1;
    digitalWrite(PIN_DC, LOW);  delayMicroseconds(T);
  }
}

bool ccWaitReady() {
  ddIn(); delayMicroseconds(T);
  for (int n = 0; n < 200; n++) {
    if (digitalRead(PIN_DD_I) == LOW) return true;
    for (int i = 0; i < 8; i++) {
      digitalWrite(PIN_DC, HIGH); delayMicroseconds(T);
      digitalWrite(PIN_DC, LOW);  delayMicroseconds(T);
    }
  }
  return false;
}

uint8_t ccRead() {
  ddIn();
  uint8_t d = 0;
  for (int i = 0; i < 8; i++) {
    digitalWrite(PIN_DC, HIGH); delayMicroseconds(T);
    d <<= 1;
    if (digitalRead(PIN_DD_I)) d |= 1;
    digitalWrite(PIN_DC, LOW);  delayMicroseconds(T);
  }
  return d;
}

void tomarLineas() {
  pinMode(PIN_DD_I, INPUT);
  pinMode(PIN_DC, OUTPUT);  digitalWrite(PIN_DC, LOW);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  digitalWrite(PIN_DD_O, LOW); ddIn();
}

void ccEnter() {
  tomarLineas();
  digitalWrite(PIN_RST, LOW);  delay(5);
  digitalWrite(PIN_DC, HIGH);  delayMicroseconds(T);
  digitalWrite(PIN_DC, LOW);   delayMicroseconds(T);
  digitalWrite(PIN_DC, HIGH);  delayMicroseconds(T);
  digitalWrite(PIN_DC, LOW);   delay(5);
  digitalWrite(PIN_RST, HIGH); delay(5);
}

/* Reset normal (sin flancos en DC). RST se queda activamente a 1: si se suelta,
   el flanco lento hacia que la mitad de los arranques se quedaran a 13 MHz.
   DC se mantiene a 0 durante el reset (flotando podria meter al chip en
   depuracion) y se suelta despues, porque P2_2 es tambien el enable del
   elevador del LED y el firmware lo usa. */
void ccRun() {
  tomarLineas();
  digitalWrite(PIN_RST, LOW);  delay(10);
  digitalWrite(PIN_RST, HIGH); delay(5);
  pinMode(PIN_DC, INPUT);
  ddIn();
}

uint8_t cmd1(uint8_t c) { ccWrite(c); ccWaitReady(); return ccRead(); }
uint8_t status()        { return cmd1(0x34); }
uint8_t halt()          { return cmd1(0x44); }
uint8_t resume()        { return cmd1(0x4C); }

uint16_t chipId() {
  ccWrite(0x68); ccWaitReady();
  uint8_t h = ccRead(), l = ccRead();
  return (h << 8) | l;
}

uint8_t instr(int n, uint8_t a, uint8_t b = 0, uint8_t c = 0) {
  ccWrite(0x54 + n); ccWrite(a);
  if (n > 1) ccWrite(b);
  if (n > 2) ccWrite(c);
  ccWaitReady(); return ccRead();
}

void writeXDATA(uint16_t addr, const uint8_t *d, int len) {
  instr(3, 0x90, addr >> 8, addr & 0xFF);   // MOV DPTR,#addr
  for (int i = 0; i < len; i++) {
    instr(2, 0x74, d[i]);                   // MOV A,#d
    instr(1, 0xF0);                         // MOVX @DPTR,A
    instr(1, 0xA3);                         // INC DPTR
  }
}

uint8_t readXDATA1(uint16_t addr) {
  instr(3, 0x90, addr >> 8, addr & 0xFF);
  return instr(1, 0xE0);                    // MOVX A,@DPTR
}

/* Pagina de 1 KB: datos a XDATA 0xF000 y rutina en 0xF400 que la escribe y
   termina con un breakpoint (metodo TI SWRA124, como CC2510Lib) */
bool writePage(uint16_t addr, const uint8_t *buf) {
  uint8_t rutina[] = {
    0x75, 0xAD, (uint8_t)((addr >> 9) & 0x7E),  // MOV FADDRH,#pagina (direccion en palabras)
    0x75, 0xAC, 0x00,                           // MOV FADDRL,#0
    0x75, 0xAE, 0x01,                           // MOV FCTL,#01   borrar pagina
    0xE5, 0xAE,                                 // e: MOV A,FCTL
    0x20, 0xE7, 0xFB,                           //    JB ACC.7,e  (BUSY)
    0x90, 0xF0, 0x00,                           // MOV DPTR,#0F000h
    0x7F, 0x02,                                 // MOV R7,#2      512 palabras
    0x7E, 0x00,                                 // MOV R6,#0
    0x75, 0xAE, 0x02,                           // MOV FCTL,#02   escribir
    0x7D, 0x02,                                 // w: MOV R5,#2   palabra = 2 bytes
    0xE0,                                       // b: MOVX A,@DPTR
    0xA3,                                       //    INC DPTR
    0xF5, 0xAF,                                 //    MOV FWDATA,A
    0xDD, 0xFA,                                 //    DJNZ R5,b
    0xE5, 0xAE,                                 // s: MOV A,FCTL
    0x20, 0xE6, 0xFB,                           //    JB ACC.6,s  (SWBSY)
    0xDE, 0xF1,                                 //    DJNZ R6,w
    0xDF, 0xEF,                                 //    DJNZ R7,w
    0xA5                                        // breakpoint -> CPU parada
  };

  halt();
  writeXDATA(0xF000, buf, PAGINA);
  writeXDATA(0xF000 + PAGINA, rutina, sizeof(rutina));
  instr(3, 0x75, 0xC7, 0x51);                   // MOV MEMCTR,#51h (como CC2510Lib)
  instr(3, 0x02, 0xF4, 0x00);                   // LJMP 0F400h -> PC
  resume();

  unsigned long t0 = millis();
  while (millis() - t0 < 3000) {
    if (status() & 0x20) { halt(); return true; }   // CPU_HALTED
    delay(10);
  }
  halt();
  return false;
}

int verificar() {
  int fallos = 0;
  instr(3, 0x90, 0x00, 0x00);
  for (uint32_t i = 0; i < fwLen; i++) {
    instr(1, 0xE4);                         // CLR A
    uint8_t v = instr(1, 0x93);             // MOVC A,@A+DPTR
    instr(1, 0xA3);
    if (v != fw[i]) {
      if (fallos < 10) Serial.printf("  0x%04X: leido %02X, esperado %02X\n", (unsigned)i, v, fw[i]);
      fallos++;
    }
  }
  return fallos;
}

/* 0 = no es CC2510, 1 = bloqueado, 2 = listo */
int entrarYComprobar() {
  ccEnter();
  uint16_t id = chipId();
  uint8_t st = status();
  Serial.printf("CHIP_ID=0x%04X  STATUS=0x%02X  T=%d us\n", id, st, T);
  if ((id & 0xFF00) != 0x8100) { Serial.println("ERROR: no responde un CC2510"); return 0; }
  // El bit DEBUG_LOCKED del STATUS no es fiable: probar una instruccion
  if (instr(2, 0x74, 0x5A) != 0x5A) return 1;
  return 2;
}

void grabar() {
  if (!fwLen) { Serial.println("ERROR: no hay firmware cargado (usa grabar.ps1)"); return; }
  Serial.printf("\n===== GRABAR %u bytes =====\n", (unsigned)fwLen);
  int e = entrarYComprobar();
  if (e == 1) Serial.println("ERROR: chip bloqueado (usa 'borrar' o grabar.ps1 -Borrar)");
  if (e != 2) return;

  unsigned long t00 = millis();
  static uint8_t pag[PAGINA];
  for (uint32_t base = 0; base < fwLen; base += PAGINA) {
    for (int i = 0; i < PAGINA; i++)
      pag[i] = (base + i < fwLen) ? fw[base + i] : 0xFF;
    unsigned long t0 = millis();
    bool ok = writePage(base, pag);
    Serial.printf("Pagina 0x%04X: %s (%lu ms)\n", (unsigned)base, ok ? "OK" : "TIMEOUT", millis() - t0);
    if (!ok) { Serial.println("ERROR: TIMEOUT escribiendo"); return; }
  }

  Serial.println("Verificando...");
  int f = verificar();
  if (f) { Serial.printf("ERROR: VERIFICACION FALLIDA, %d bytes distintos\n", f); return; }
  Serial.printf("VERIFICACION OK en %lu ms. Arrancando firmware.\n", millis() - t00);
  ccRun();
}

void borrar() {
  Serial.println("\n===== CHIP ERASE =====");
  if (entrarYComprobar() == 0) return;
  cmd1(0x14);                                // CHIP_ERASE
  unsigned long t0 = millis();
  uint8_t st;
  do { delay(5); st = status(); } while (!(st & 0x80) && millis() - t0 < 3000);
  int e = entrarYComprobar();
  if (e == 2) Serial.printf("BORRADO OK (%lu ms), chip desbloqueado\n", millis() - t0);
  else Serial.println("ERROR: sigue bloqueado tras el borrado");
}

void info() {
  int e = entrarYComprobar();
  if (e == 1) Serial.println("Chip BLOQUEADO (solo se puede borrar)");
  if (e == 2) Serial.println("Chip desbloqueado");
  ccRun();
}

void leerProgreso() {
  ccEnter();
  uint8_t p = readXDATA1(0xFDF0), b = readXDATA1(0xFDF1);
  Serial.printf("progreso=0x%02X  FDF1=0x%02X  (el CC2510 queda parado: 'run' para arrancarlo)\n", p, b);
}

void volcarXDATA() {
  ccEnter();
  instr(3, 0x90, 0xFD, 0x00);               // MOV DPTR,#0FD00h
  for (int i = 0; i < 256; i++) {
    if (i % 16 == 0) Serial.printf("\nFD%02X:", i);
    Serial.printf(" %02X", instr(1, 0xE0)); // MOVX A,@DPTR
    instr(1, 0xA3);                         // INC DPTR
  }
  Serial.println("\n(el CC2510 queda parado: 'run' para arrancarlo)");
}

/* Recepcion binaria del firmware: "W <bytes> <suma>" + bytes */
void recibirFirmware(const String &l) {
  int a = l.indexOf(' '), b = l.indexOf(' ', a + 1);
  uint32_t n = l.substring(a + 1, b).toInt();
  uint32_t suma = strtoul(l.substring(b + 1).c_str(), NULL, 10);
  if (n == 0 || n > FW_MAX) { Serial.println("ERROR: tamano no valido"); return; }
  Serial.println("LISTO");
  uint32_t r = 0, s = 0, t0 = millis();
  while (r < n && millis() - t0 < 10000) {
    int c = Serial.read();
    if (c < 0) continue;
    fw[r++] = c;
    s += c;
  }
  if (r != n) { fwLen = 0; Serial.printf("ERROR: recibidos %u de %u bytes\n", (unsigned)r, (unsigned)n); return; }
  if (s != suma) { fwLen = 0; Serial.println("ERROR: suma incorrecta"); return; }
  fwLen = n;
  Serial.printf("OK RECIBIDO %u bytes\n", (unsigned)n);
}

/* =================== Consola del puente (UART2) =================== */
int dbm(uint8_t r) { return (int8_t)r / 2 - 71; }

/* Espera una trama del puente. Devuelve longitud (0 = sin respuesta por radio),
   o -1 si el puente no contesta por UART. */
int leerTrama(uint8_t *d, uint8_t &rssi, uint8_t &lqi, uint32_t ms) {
  uint32_t t0 = millis();
  auto leer = [&](uint8_t &b) -> bool {
    while (!Serial2.available())
      if (millis() - t0 > ms) return false;
    b = Serial2.read();
    return true;
  };
  uint8_t b, n;
  do { if (!leer(b)) return -1; } while (b != SOF);
  if (!leer(n)) return -1;
  if (n == 0) return 0;
  if (n > RADIO_MAX) return -1;
  for (int i = 0; i < n; i++) if (!leer(d[i])) return -1;
  if (!leer(rssi) || !leer(lqi)) return -1;
  return n;
}

void enviarTrama(const uint8_t *d, uint8_t n) {
  while (Serial2.available()) Serial2.read();
  Serial2.write(SOF);
  Serial2.write(n);
  Serial2.write(d, n);
}

void mandarTexto(const String &txt) {
  uint8_t t[RADIO_MAX];
  int n = txt.length();
  if (n > RADIO_MAX - 3) n = RADIO_MAX - 3;
  seq++;
  t[0] = destino;
  t[1] = TIPO_TEXTO;
  t[2] = seq;
  memcpy(t + 3, txt.c_str(), n);

  for (int intento = 1; intento <= REINTENTOS; intento++) {
    enviarTrama(t, n + 3);
    uint8_t r[RADIO_MAX], rssi, lqi;
    int m = leerTrama(r, rssi, lqi, 1000);
    if (m < 0) {                       // puede perderse si el puente leia el NFC
      Serial.printf("  intento %d: el puente no contesta por UART\n", intento);
      continue;
    }
    if (m >= 4 && r[1] == TIPO_ACK && r[2] == seq) {
      Serial.printf("  ACK de la etiqueta en intento %d: ella nos oye a %d dBm, "
                    "nosotros a ella a %d dBm (LQI %u)\n",
                    intento, dbm(r[3]), dbm(rssi), lqi & 0x7F);
      Serial.println("  La pantalla tarda unos 20 s en refrescar.");
      return;
    }
    Serial.printf("  intento %d: %s\n", intento, m ? "respuesta inesperada" : "sin ACK");
    delay(100);
  }
  Serial.println("  Sin respuesta. Comprueba que la etiqueta esta alimentada y escuchando.");
}

void ping() {
  uint8_t t[3] = { destino, '?', 0 };   // tipo que nadie atiende ('P' = pintar)
  enviarTrama(t, 3);
  uint8_t r[RADIO_MAX], rssi, lqi;
  int m = leerTrama(r, rssi, lqi, 1000);
  if (m < 0) Serial.println("  el puente no contesta por UART");
  else if (m == 0) Serial.println("  el puente responde; nadie contesto por radio");
  else Serial.printf("  recibido %d bytes por radio, %d dBm\n", m, dbm(rssi));
}

void reiniciarPuente() {
  ccRun();
  uint8_t r[RADIO_MAX], rssi, lqi;
  int m = leerTrama(r, rssi, lqi, 1500);
  if (m == 1 && r[0] == 'B') Serial.println("Puente arrancado y respondiendo por UART.");
  else Serial.println("Reset hecho; no ha saludado ningun puente por UART "
                      "(normal si en RST/DC/DD esta conectada la etiqueta).");
}

void volcar(uint32_t ms) {
  uint32_t t0 = millis();
  int n = 0;
  Serial.print("  UART2:");
  while (millis() - t0 < ms) {
    if (Serial2.available()) { Serial.printf(" %02X", Serial2.read()); n++; }
  }
  Serial.printf("  (%d bytes)\n", n);
}

/* Trama de radio con reintentos hasta el ACK de la etiqueta destino */
bool enviarConAck(uint8_t tipo, const uint8_t *d, uint8_t n, uint32_t espera) {
  uint8_t t[RADIO_MAX];
  seq++;
  t[0] = destino;
  t[1] = tipo;
  t[2] = seq;
  memcpy(t + 3, d, n);
  for (int intento = 0; intento < 8; intento++) {
    enviarTrama(t, n + 3);
    uint8_t r[RADIO_MAX], rssi, lqi;
    int m = leerTrama(r, rssi, lqi, espera);
    if (m >= 4 && r[1] == TIPO_ACK && r[2] == seq) return true;
    delay(20);
  }
  return false;
}

/* "IMG <bytes> <suma>" + binario: recibir y mandar por radio */
void enviarImagen(const String &l) {
  int a = l.indexOf(' '), b = l.indexOf(' ', a + 1);
  uint32_t n = l.substring(a + 1, b).toInt();
  uint32_t suma = strtoul(l.substring(b + 1).c_str(), NULL, 10);
  if (n != IMG_BYTES) { Serial.printf("ERROR: la imagen debe tener %d bytes\n", IMG_BYTES); return; }
  Serial.println("LISTO");
  uint32_t r = 0, s = 0, t0 = millis();
  while (r < n && millis() - t0 < 10000) {
    int c = Serial.read();
    if (c < 0) continue;
    img[r++] = c;
    s += c;
  }
  if (r != n || s != suma) { Serial.println("ERROR: imagen mal recibida"); return; }
  Serial.printf("Imagen recibida. Enviando a la etiqueta %u...\n", destino);

  t0 = millis();
  if (!enviarConAck(TIPO_PREPARAR, NULL, 0, 1000)) { Serial.println("ERROR: la etiqueta no contesta"); return; }
  delay(1200);                                   // borrado de 3 sectores de la flash
  int fallos = 0;
  for (uint32_t pos = 0; pos < IMG_BYTES; pos += IMG_TROZO) {
    uint8_t d[2 + IMG_TROZO];
    uint8_t k = (IMG_BYTES - pos < IMG_TROZO) ? IMG_BYTES - pos : IMG_TROZO;
    d[0] = pos >> 8;
    d[1] = pos & 0xFF;
    for (uint8_t i = 0; i < k; i++)
      d[2 + i] = img[pos + i] ^ mezcla[(pos + i) & 63];
    if (!enviarConAck(TIPO_IMAGEN, d, k + 2, 300)) {
      Serial.printf("ERROR: trozo %u sin ACK\n", (unsigned)pos);
      return;
    }
    if ((pos / IMG_TROZO) % 40 == 0)
      Serial.printf("  %u / %d bytes\n", (unsigned)pos, IMG_BYTES);
  }
  if (!enviarConAck(TIPO_MOSTRAR, NULL, 0, 1000)) { Serial.println("ERROR: sin ACK al pintar"); return; }
  Serial.printf("IMAGEN ENVIADA en %lu ms. La pantalla tarda unos 20 s.\n", millis() - t0);
}

void ayuda() {
  Serial.println("Radio: t TEXTO | d N | ping    Placa: r | x | m [s] | info | grabar | "
                 "verificar | borrar | progreso | xdata | run | vel N");
}

/* =================== Programa =================== */
void setup() {
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, PIN_RX2, PIN_TX2);
  // Sin reset al arrancar: si el ESP32 se reinicia (alimentacion) no debe
  // reiniciar la placa conectada. RST a 1 activo y DC suelto.
  tomarLineas();
  pinMode(PIN_DC, INPUT);
  delay(200);
  Serial.println("\nHerramienta VUSION lista.");
  ayuda();
}

void loop() {
  if (!Serial.available()) return;
  String l = Serial.readStringUntil('\n');
  l.trim();
  if (l.startsWith("W ")) recibirFirmware(l);
  else if (l.startsWith("IMG ")) enviarImagen(l);
  else if (l.startsWith("t ")) {
    Serial.printf("-> etiqueta %u: \"%s\"\n", destino, l.substring(2).c_str());
    mandarTexto(l.substring(2));
  }
  else if (l.startsWith("d ")) { destino = l.substring(2).toInt(); Serial.printf("Destino = %u\n", destino); }
  else if (l == "ping") ping();
  else if (l == "r") reiniciarPuente();
  else if (l == "x") { ccRun(); volcar(2000); }
  else if (l == "escuchar" || l.startsWith("escuchar ")) {
    // volcado hex del UART2 sin reiniciar nada (p. ej. para ver el saludo del
    // puente al darle alimentacion a mano)
    int seg = l.length() > 9 ? l.substring(9).toInt() : 30;
    volcar((uint32_t)seg * 1000);
  }
  else if (l == "m" || l.startsWith("m ")) {
    int seg = l.length() > 2 ? l.substring(2).toInt() : 90;
    ccRun();
    uint32_t t0 = millis();
    while (millis() - t0 < (uint32_t)seg * 1000)
      if (Serial2.available()) Serial.write(Serial2.read());
    Serial.println("\n[fin del monitor]");
  }
  else if (l == "info") info();
  else if (l == "grabar") grabar();
  else if (l == "verificar") {
    if (!fwLen) Serial.println("ERROR: no hay firmware cargado");
    else if (entrarYComprobar() == 2) { Serial.printf("Fallos: %d\n", verificar()); ccRun(); }
  }
  else if (l == "borrar") borrar();
  else if (l == "progreso") leerProgreso();
  else if (l == "xdata") volcarXDATA();
  else if (l == "run") { ccRun(); Serial.println("Firmware arrancado."); }
  else if (l.startsWith("vel ")) { T = constrain(l.substring(4).toInt(), 1, 100); Serial.printf("T = %d us\n", T); }
  else if (l.length()) ayuda();
}
