/*
  AI-Lens — Tahap 1 (prototipe pengalaman pengguna, tanpa lensa)
  Firmware ESP32: menerima teks jawaban AI lewat Bluetooth Low Energy
  dan menampilkannya di layar OLED 128x64.

  Board   : ESP32 Dev Module (ESP32-WROOM-32 / DevKit V1)
  Layar   : OLED 0.96" SSD1306 I2C (atau 1.3" SH1106, ganti konstruktor di bawah)
  Library : U8g2 (oliver) — dari Library Manager Arduino IDE
            BLE bawaan core "esp32 by Espressif" (versi 2.x atau 3.x)
  Partisi : Tools > Partition Scheme > "Huge APP (3MB No OTA/1MB SPIFFS)"

  Protokol (Nordic UART Service, satu perintah per baris diakhiri '\n'):
    HP -> ESP32
      MSG <teks>      ganti isi layar (baris baru di dalam teks = byte 0x1F)
      ADD <teks>      tambahkan teks (untuk streaming)
      CFG k=v;k=v     mode=page|rsvp|ticker; font=0|1|2; wpm=60..1000;
                      auto=0|1; bar=0|1; contrast=0..255; rot=0|2|m|mv
      NEXT / PREV     sama dengan tombol fisik
      GOTO <n>        lompat ke halaman n (mulai 1)
      WAIT            tampilkan "Berpikir..."
      CLEAR           kosongkan layar
      PING            dibalas PONG
    ESP32 -> HP
      PONG v1 128x64 | PAGE x/y | END | PAUSE 0/1 | BTN ASK | BTN CLEAR | CFG OK

  Tombol:
    NEXT (tombol BOOT, GPIO0)  pendek: halaman berikut / jeda-lanjut (RSVP, ticker)
                               tahan : minta HP mulai input suara (BTN ASK)
    PREV (GPIO4 ke GND)        pendek: halaman sebelumnya / mundur (RSVP) / ulang (ticker)
                               tahan : kosongkan layar

  Serial Monitor (115200, line ending "Newline") menerima perintah yang sama,
  jadi layar bisa dites tanpa HP, misalnya ketik:  MSG Halo dari AI-Lens
*/

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <vector>

// ===================== Pengaturan perangkat keras =====================
#define DEVICE_NAME    "AI-Lens"
#define PIN_SDA        21
#define PIN_SCL        22
#define PIN_BTN_NEXT   0      // tombol BOOT bawaan board
#define PIN_BTN_PREV   4      // tombol tambahan: GPIO4 <-> GND
#define LONG_PRESS_MS  700

// Pilih SATU konstruktor sesuai modul OLED:
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);    // OLED 0.96"
// U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, PIN_SCL, PIN_SDA);  // OLED 1.3"

#define SCREEN_W   128
#define SCREEN_H   64
#define BAR_H      3        // ruang garis progres di bawah layar
#define MAX_LINES  400
#define MAX_TEXT   6000

static const char* NUS_SERVICE = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_RX      = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";  // HP -> ESP32
static const char* NUS_TX      = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";  // ESP32 -> HP

// Huruf monospace: ukuran sel tetap, jadi tata letak sama persis dengan simulator di aplikasi.
struct FontDef { const uint8_t* font; uint8_t w; uint8_t h; };
static const FontDef FONTS[3] = {
  { u8g2_font_6x10_tf,   6, 10 },   // 0 kecil : 21 huruf x 6 baris
  { u8g2_font_7x14_tf,   7, 14 },   // 1 sedang: 18 huruf x 4 baris
  { u8g2_font_10x20_tf, 10, 20 },   // 2 besar : 12 huruf x 3 baris
};

enum Mode { MODE_PAGE, MODE_RSVP, MODE_TICKER };

struct Config {
  Mode     mode        = MODE_PAGE;
  uint8_t  font        = 1;
  uint16_t wpm         = 200;
  bool     autoAdvance = false;
  bool     bar         = true;
  uint8_t  contrast    = 200;
  String   rot         = "0";
} cfg;

// ===================== Status tampilan =====================
String fullText;
String tickText;
std::vector<String> lines;
std::vector<String> words;

int  page = 0;
bool waiting = false;
bool endSent = false;
bool dirty = true;
unsigned long pageShownAt = 0;

int  rsvpIdx = 0;
bool rsvpPaused = false;
unsigned long rsvpNext = 0;

float tickX = SCREEN_W;
bool  tickPaused = false;
unsigned long tickLast = 0;

// ===================== BLE =====================
BLECharacteristic* txChar = nullptr;
volatile bool bleConnected = false;
volatile bool connChanged = false;
volatile bool needAdvertise = false;
SemaphoreHandle_t rxLock;
String rxAccum;

void sendEvt(const String& s) {
  Serial.println("> " + s);
  if (!bleConnected || txChar == nullptr) return;
  txChar->setValue((uint8_t*)s.c_str(), s.length());
  txChar->notify();
}

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override { bleConnected = true; connChanged = true; }
  void onDisconnect(BLEServer* s) override { bleConnected = false; connChanged = true; needAdvertise = true; }
};

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String v = String(c->getValue().c_str());   // jalan di core 2.x (std::string) dan 3.x (String)
    if (xSemaphoreTake(rxLock, pdMS_TO_TICKS(50)) == pdTRUE) {
      rxAccum += v;
      xSemaphoreGive(rxLock);
    }
  }
};

void setupBle() {
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(517);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  BLEService* svc = server->createService(NUS_SERVICE);

  txChar = svc->createCharacteristic(NUS_TX, BLECharacteristic::PROPERTY_NOTIFY);
#if !defined(CONFIG_NIMBLE_ENABLED)
  txChar->addDescriptor(new BLE2902());   // NimBLE menambahkan descriptor ini otomatis
#endif

  BLECharacteristic* rx = svc->createCharacteristic(
      NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCB());

  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

// ===================== Utilitas teks UTF-8 =====================
static int u8len(const String& s) {
  int n = 0;
  for (size_t i = 0; i < s.length(); i++) if (((uint8_t)s[i] & 0xC0) != 0x80) n++;
  return n;
}

static int u8byteIndex(const String& s, int charIdx) {
  int n = 0;
  for (size_t i = 0; i < s.length(); i++) {
    if (((uint8_t)s[i] & 0xC0) != 0x80) {
      if (n == charIdx) return i;
      n++;
    }
  }
  return s.length();
}

static String u8sub(const String& s, int start, int count) {
  int a = u8byteIndex(s, start);
  int b = u8byteIndex(s, start + count);
  return s.substring(a, b);
}

// ===================== Tata letak =====================
int charsPerLine() { return SCREEN_W / FONTS[cfg.font].w; }
int linesPerPage() { return (SCREEN_H - (cfg.bar ? BAR_H : 0)) / FONTS[cfg.font].h; }
int pageCount() {
  int lpp = linesPerPage();
  int pc = (lines.size() + lpp - 1) / lpp;
  return pc < 1 ? 1 : pc;
}

void pushLine(const String& l) { if (lines.size() < MAX_LINES) lines.push_back(l); }

void wrapParagraph(const String& para, int cpl) {
  String cur;
  int curLen = 0;
  int i = 0, n = para.length();
  bool any = false;
  while (i < n) {
    while (i < n && para[i] == ' ') i++;
    if (i >= n) break;
    int j = para.indexOf(' ', i);
    if (j < 0) j = n;
    String w = para.substring(i, j);
    i = j;
    words.push_back(w);
    any = true;
    int wl = u8len(w);
    if (wl > cpl) {                       // kata lebih panjang dari satu baris: dipotong
      if (curLen > 0) { pushLine(cur); cur = ""; curLen = 0; }
      int pos = 0;
      while (wl - pos > cpl) { pushLine(u8sub(w, pos, cpl)); pos += cpl; }
      cur = u8sub(w, pos, cpl);
      curLen = wl - pos;
    } else if (curLen == 0) {
      cur = w; curLen = wl;
    } else if (curLen + 1 + wl <= cpl) {
      cur += ' '; cur += w; curLen += 1 + wl;
    } else {
      pushLine(cur); cur = w; curLen = wl;
    }
  }
  if (curLen > 0 || !any) pushLine(cur);
}

void rebuildLayout() {
  lines.clear();
  words.clear();
  int cpl = charsPerLine();
  int start = 0;
  int len = fullText.length();
  while (start <= len) {
    int nl = fullText.indexOf('\n', start);
    if (nl < 0) nl = len;
    wrapParagraph(fullText.substring(start, nl), cpl);
    start = nl + 1;
    if ((int)lines.size() >= MAX_LINES) break;
  }
  while (!lines.empty() && lines.back().length() == 0) lines.pop_back();

  tickText = fullText;
  tickText.replace("\n", "   ");
}

int wordsOnPage() {
  int lpp = linesPerPage();
  int n = 0;
  for (int i = page * lpp; i < (page + 1) * lpp && i < (int)lines.size(); i++) {
    const String& l = lines[i];
    if (l.length() == 0) continue;
    n++;
    for (size_t k = 0; k < l.length(); k++) if (l[k] == ' ') n++;
  }
  return n;
}

void notifyPage() { sendEvt("PAGE " + String(page + 1) + "/" + String(pageCount())); }

unsigned long rsvpDelay(const String& w) {
  float base = 60000.0f / (cfg.wpm < 60 ? 60 : cfg.wpm);
  char last = w.length() ? w[w.length() - 1] : ' ';
  float f = 1.0f;
  if (last == '.' || last == '!' || last == '?' || last == ':') f = 2.0f;
  else if (last == ',' || last == ';') f = 1.4f;
  if (u8len(w) > 8) f += 0.3f;
  return (unsigned long)(base * f);
}

// ===================== Perintah =====================
void setText(const String& t, bool append) {
  String clean = t;
  clean.replace((char)0x1F, '\n');
  if (append) {
    fullText += clean;
    endSent = false;                    // teks bertambah: halaman terakhir berubah
  } else {
    fullText = clean;
    page = 0;
    endSent = false;
    rsvpIdx = 0;
    rsvpPaused = false;
    tickX = SCREEN_W;
    tickPaused = false;
  }
  if ((int)fullText.length() > MAX_TEXT) fullText = fullText.substring(0, MAX_TEXT);
  waiting = false;
  rebuildLayout();
  if (page > pageCount() - 1) page = pageCount() - 1;
  if (!append) {
    pageShownAt = millis();
    rsvpNext = millis() + 400 + (words.empty() ? 0 : rsvpDelay(words[0]));
    tickLast = millis();
    notifyPage();
  }
  dirty = true;
}

void clearScreen() {
  fullText = "";
  waiting = false;
  page = 0;
  endSent = false;
  rebuildLayout();
  dirty = true;
}

void goNext() {
  if (fullText.length() == 0) return;
  if (cfg.mode == MODE_RSVP) {
    rsvpPaused = !rsvpPaused;
    if (!rsvpPaused) rsvpNext = millis() + 300;
    sendEvt(String("PAUSE ") + (rsvpPaused ? "1" : "0"));
  } else if (cfg.mode == MODE_TICKER) {
    tickPaused = !tickPaused;
    tickLast = millis();
    sendEvt(String("PAUSE ") + (tickPaused ? "1" : "0"));
  } else if (page < pageCount() - 1) {
    page++;
    pageShownAt = millis();
    notifyPage();
  } else if (!endSent) {
    endSent = true;
    sendEvt("END");
  }
  dirty = true;
}

void goPrev() {
  if (fullText.length() == 0) return;
  if (cfg.mode == MODE_RSVP) {
    rsvpIdx = rsvpIdx > 10 ? rsvpIdx - 10 : 0;
    rsvpNext = millis() + 500;
  } else if (cfg.mode == MODE_TICKER) {
    tickX = SCREEN_W;
    tickLast = millis();
  } else if (page > 0) {
    page--;
    pageShownAt = millis();
    notifyPage();
  }
  dirty = true;
}

void applyRotation() {
  if (cfg.rot == "2") u8g2.setDisplayRotation(U8G2_R2);
#ifdef U8G2_MIRROR
  else if (cfg.rot == "m") u8g2.setDisplayRotation(U8G2_MIRROR);
#endif
#ifdef U8G2_MIRROR_VERTICAL
  else if (cfg.rot == "mv") u8g2.setDisplayRotation(U8G2_MIRROR_VERTICAL);
#endif
  else u8g2.setDisplayRotation(U8G2_R0);
}

void applyConfig(const String& s) {
  int start = 0;
  while (start < (int)s.length()) {
    int sc = s.indexOf(';', start);
    if (sc < 0) sc = s.length();
    String kv = s.substring(start, sc);
    start = sc + 1;
    int eq = kv.indexOf('=');
    if (eq < 0) continue;
    String k = kv.substring(0, eq);
    String v = kv.substring(eq + 1);
    k.trim(); v.trim();
    if (k == "mode") cfg.mode = (v == "rsvp") ? MODE_RSVP : (v == "ticker") ? MODE_TICKER : MODE_PAGE;
    else if (k == "font") cfg.font = constrain(v.toInt(), 0, 2);
    else if (k == "wpm") cfg.wpm = constrain(v.toInt(), 60, 1000);
    else if (k == "auto") cfg.autoAdvance = (v == "1");
    else if (k == "bar") cfg.bar = (v == "1");
    else if (k == "contrast") { cfg.contrast = constrain(v.toInt(), 0, 255); u8g2.setContrast(cfg.contrast); }
    else if (k == "rot") { cfg.rot = v; applyRotation(); }
  }
  rebuildLayout();
  if (page > pageCount() - 1) page = pageCount() - 1;
  if (rsvpIdx >= (int)words.size()) rsvpIdx = words.empty() ? 0 : words.size() - 1;
  pageShownAt = millis();
  tickLast = millis();
  dirty = true;
  sendEvt("CFG OK");
}

void handleLine(String line) {
  line.replace("\r", "");
  if (line.length() == 0) return;
  if (line.startsWith("MSG ") || line == "MSG") setText(line.length() > 4 ? line.substring(4) : "", false);
  else if (line.startsWith("ADD ")) setText(line.substring(4), true);
  else if (line.startsWith("CFG ")) applyConfig(line.substring(4));
  else if (line == "NEXT") goNext();
  else if (line == "PREV") goPrev();
  else if (line.startsWith("GOTO ")) {
    int p = line.substring(5).toInt() - 1;
    if (p >= 0 && p < pageCount()) { page = p; pageShownAt = millis(); dirty = true; notifyPage(); }
  }
  else if (line == "WAIT") { waiting = true; dirty = true; }
  else if (line == "CLEAR") clearScreen();
  else if (line == "PING") sendEvt("PONG v1 128x64");
}

void processRx() {
  static String lineBuf;
  String chunk;
  if (xSemaphoreTake(rxLock, pdMS_TO_TICKS(5)) == pdTRUE) {
    chunk = rxAccum;
    rxAccum = "";
    xSemaphoreGive(rxLock);
  }
  while (Serial.available()) chunk += (char)Serial.read();
  if (chunk.length() == 0) return;
  lineBuf += chunk;
  int nl;
  while ((nl = lineBuf.indexOf('\n')) >= 0) {
    String line = lineBuf.substring(0, nl);
    lineBuf = lineBuf.substring(nl + 1);
    handleLine(line);
  }
  if ((int)lineBuf.length() > MAX_TEXT + 200) lineBuf = "";   // pengaman jika baris tidak pernah ditutup
}

// ===================== Tombol =====================
struct Button { uint8_t pin; bool down; unsigned long since; bool longFired; };
Button btnNext = { PIN_BTN_NEXT, false, 0, false };
Button btnPrev = { PIN_BTN_PREV, false, 0, false };

// return: 0 = tidak ada, 1 = tekan pendek, 2 = tekan lama
int pollButton(Button& b) {
  bool pressed = digitalRead(b.pin) == LOW;
  unsigned long now = millis();
  if (pressed && !b.down) { b.down = true; b.since = now; b.longFired = false; }
  else if (pressed && b.down && !b.longFired && now - b.since >= LONG_PRESS_MS) { b.longFired = true; return 2; }
  else if (!pressed && b.down) {
    b.down = false;
    if (!b.longFired && now - b.since >= 30) return 1;   // 30 ms = debounce
  }
  return 0;
}

void pollButtons() {
  int n = pollButton(btnNext);
  if (n == 1) goNext();
  else if (n == 2) sendEvt("BTN ASK");
  int p = pollButton(btnPrev);
  if (p == 1) goPrev();
  else if (p == 2) { clearScreen(); sendEvt("BTN CLEAR"); }
}

// ===================== Pewaktu (auto-advance, RSVP, ticker) =====================
void updateTimers() {
  unsigned long now = millis();
  if (fullText.length() == 0 || waiting) return;

  if (cfg.mode == MODE_PAGE && cfg.autoAdvance) {
    unsigned long dur = (unsigned long)wordsOnPage() * 60000UL / cfg.wpm;
    if (dur < 1500) dur = 1500;
    if (now - pageShownAt >= dur) {
      if (page < pageCount() - 1) { page++; pageShownAt = now; notifyPage(); dirty = true; }
      else if (!endSent) { endSent = true; sendEvt("END"); }
    }
  } else if (cfg.mode == MODE_RSVP && !rsvpPaused && !words.empty() && now >= rsvpNext) {
    if (rsvpIdx < (int)words.size() - 1) {
      rsvpIdx++;
      rsvpNext = now + rsvpDelay(words[rsvpIdx]);
      dirty = true;
    } else if (!endSent) {
      endSent = true;
      sendEvt("END");
    }
  } else if (cfg.mode == MODE_TICKER && !tickPaused) {
    float w = FONTS[cfg.font].w;
    float pxPerSec = cfg.wpm * 6.0f / 60.0f * w;     // ±6 karakter per kata (termasuk spasi)
    tickX -= pxPerSec * (now - tickLast) / 1000.0f;
    tickLast = now;
    float textW = u8len(tickText) * w;
    if (tickX < -textW) {
      if (!endSent) { endSent = true; sendEvt("END"); }
      tickX = SCREEN_W;                              // ulang dari kanan
    }
  }
}

// ===================== Gambar =====================
void drawCentered(const FontDef& f, int y, const String& s) {
  u8g2.setFont(f.font);
  int x = (SCREEN_W - u8len(s) * f.w) / 2;
  if (x < 0) x = 0;
  u8g2.drawUTF8(x, y, s.c_str());
}

void drawProgress(int num, int den) {
  if (!cfg.bar || den <= 1) return;
  int w = SCREEN_W * num / den;
  u8g2.drawBox(0, SCREEN_H - 2, w, 2);
}

void render() {
  u8g2.clearBuffer();
  const FontDef& f = FONTS[cfg.font];

  if (waiting) {
    String dots = "";
    for (int i = 0; i < (int)((millis() / 400) % 4); i++) dots += ".";
    drawCentered(FONTS[1], 25, "Berpikir" + dots);
  } else if (fullText.length() == 0) {
    drawCentered(FONTS[1], 12, "AI-Lens");
    drawCentered(FONTS[0], 40, bleConnected ? "HP terhubung" : "Menunggu HP...");
  } else if (cfg.mode == MODE_RSVP && !words.empty()) {
    int idx = rsvpIdx < (int)words.size() ? rsvpIdx : words.size() - 1;
    const String& w = words[idx];
    int fi = cfg.font;
    while (fi > 0 && u8len(w) * FONTS[fi].w > SCREEN_W) fi--;
    const FontDef& rf = FONTS[fi];
    int y = (SCREEN_H - BAR_H - rf.h) / 2;
    drawCentered(rf, y, u8len(w) * rf.w > SCREEN_W ? u8sub(w, 0, SCREEN_W / rf.w) : w);
    if (rsvpPaused) { u8g2.setFont(FONTS[0].font); u8g2.drawStr(0, 0, "II"); }
    drawProgress(idx + 1, words.size());
  } else if (cfg.mode == MODE_TICKER) {
    u8g2.setFont(f.font);
    int y = (SCREEN_H - f.h) / 2;
    int startChar = 0;
    float x = tickX;
    if (x < 0) {                                    // lewati huruf yang sudah keluar layar kiri
      startChar = (int)(-x / f.w) + 1;
      x += startChar * f.w;
    }
    int visible = SCREEN_W / f.w + 2;
    String part = u8sub(tickText, startChar, visible);
    if (x < SCREEN_W) u8g2.drawUTF8((int)x, y, part.c_str());
    if (tickPaused) { u8g2.setFont(FONTS[0].font); u8g2.drawStr(0, 0, "II"); }
  } else {
    u8g2.setFont(f.font);
    int lpp = linesPerPage();
    int start = page * lpp;
    for (int i = 0; i < lpp && start + i < (int)lines.size(); i++) {
      u8g2.drawUTF8(0, i * f.h, lines[start + i].c_str());
    }
    drawProgress(page + 1, pageCount());
  }
  u8g2.sendBuffer();
}

// ===================== Setup & loop =====================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
  pinMode(PIN_BTN_PREV, INPUT_PULLUP);

  u8g2.setBusClock(400000);
  u8g2.begin();
  u8g2.enableUTF8Print();
  u8g2.setFontPosTop();
  u8g2.setContrast(cfg.contrast);

  rxLock = xSemaphoreCreateMutex();
  setupBle();
  rebuildLayout();
  Serial.println("AI-Lens siap. Ketik perintah, mis.: MSG Halo dunia");
}

void loop() {
  if (connChanged) { connChanged = false; dirty = true; }
  if (needAdvertise) { needAdvertise = false; delay(300); BLEDevice::startAdvertising(); }

  processRx();
  pollButtons();
  updateTimers();

  static unsigned long lastFrame = 0;
  bool animating = waiting || (cfg.mode == MODE_TICKER && fullText.length() > 0 && !tickPaused);
  if (dirty || (animating && millis() - lastFrame >= 33)) {
    render();
    lastFrame = millis();
    dirty = false;
  }
  delay(2);
}
