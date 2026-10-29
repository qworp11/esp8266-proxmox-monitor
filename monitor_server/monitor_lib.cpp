/*
 * MONITOR_LIB - библиотечная часть мониторинга для NodeMCU v3 (ESP8266)
 * Подключается из monitor_server.ino (там же все настройки Wi-Fi и адрес сервера).
 *
 * Экран живёт "своей жизнью": анимированные глаза (моргание, взгляд по сторонам,
 * реакции), а раз в 2-3 минуты - слайд-шоу: статус ОК/УПАЛ, нагрузка сервера
 * (CPU/RAM из Proxmox через микросервис pxmon на :8123), список VM, диски.
 */

#include "monitor_lib.h"
#include <ESP8266HTTPClient.h>

// ---------------- СЦЕНЫ (v2) и ночной сон: 0 = полный откат к v1 ----------------
#ifndef SCENES_ON
#define SCENES_ON 1
#endif

// ===================== OLED SSD1306 128x64 =====================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// I2C на NodeMCU v3: SDA -> D2 (GPIO4), SCL -> D1 (GPIO5)
const uint8_t PIN_SDA = 4;
const uint8_t PIN_SCL = 5;

// Встроенный светодиод (GPIO2, активный НИЗКИЙ уровень)
const uint8_t PIN_LED = LED_BUILTIN;

IPAddress targetIP;
bool targetIpValid = false;

static bool oledOk = false;

State state = ST_NOWIFI;

unsigned long lastPingAt  = 0;
unsigned long lastWifiTry = 0;
unsigned long bootAt      = 0;
int           lastRtt     = -1;   // ms, -1 = нет ответа

// ---------- ПИЩАЛКА (D5 = GPIO14, пассивный зуммер от ПК: GND + D5) ----------
const uint8_t PIN_BUZZER = D5;  // D5 полностью свободный пин (без boot-strap нюансов)
struct BeepStep { uint16_t freq; uint16_t ms; }; // freq=0 пауза; {0,0} конец (не цикл)
static const BeepStep PATTERN_SIREN[]   = { {2200, 700}, {1500, 450} };                          // гипервизор: вой
static const BeepStep PATTERN_VM[]      = { {1100,120},{0,110},{1100,120},{0,110},{1100,120},{0,120},{1600,160},{0,9000} }; // VM упала: 3 писка
static const BeepStep PATTERN_RECOVER[] = { {1400,90},{0,50},{1900,140},{0,0} };                  // всё хорошо
static const BeepStep PATTERN_BOOT[]    = { {2000,70},{0,40},{1600,90},{0,0} };                   // "я жив"

enum BeepMode { BM_NONE, BM_SIREN, BM_VM, BM_RECOVER, BM_BOOT };
static BeepMode        beepMode = BM_NONE;
static const BeepStep* beepPat  = nullptr;
static uint8_t         beepLen = 0, beepIdx = 0;
static bool            beepLoop = false;
static unsigned long   beepAt = 0;

static const char* beepName(BeepMode m) {
  switch (m) {
    case BM_SIREN:   return "siren (hypervisor down)";
    case BM_VM:      return "vm-alarm (a VM is down)";
    case BM_RECOVER: return "recover chirp";
    case BM_BOOT:    return "boot chirp";
    default:         return "-";
  }
}

static void beepStart(BeepMode m, const BeepStep* p, uint8_t len, bool loop) {
  if (beepPat == p && beepMode == m) return; // не дёргать один паттерн бесконечно
  analogWrite(PIN_LED, 255); // LED замолкает на время пищалки (общий таймер 1)
  beepPat = p; beepLen = len; beepLoop = loop; beepIdx = 0; beepMode = m;
  beepAt = 0;
  Serial.print(F("[BEEP] start: "));
  Serial.println(beepName(m));
}

static void beepStopNow(bool silent) {
  if (beepPat) {
    noTone(PIN_BUZZER);
    beepPat = nullptr; beepMode = BM_NONE;
    if (silent) beepLoop = false;
  }
}

static void beepTick() {
  if (!beepPat) return;
  unsigned long now = millis();
  if (now < beepAt) return;
  const BeepStep& s = beepPat[beepIdx];
  if (s.freq == 0 && s.ms == 0) { beepStopNow(false); return; }
  if (s.freq > 0) tone(PIN_BUZZER, s.freq);
  else            noTone(PIN_BUZZER);
  beepAt = now + s.ms;
  beepIdx++;
  if (beepIdx >= beepLen) {
    if (beepLoop) beepIdx = 0;
    else          { beepStopNow(false); return; }
  }
}

// ---------- LED: мигалка при аварии / дыхание в норме ----------
static const uint16_t BLINK_SEQ[] = { 70, 70, 70, 570 }; // двойная вспышка + пауза - аварийная "мигалка"
static const uint8_t  BLINK_LEN   = sizeof(BLINK_SEQ) / sizeof(BLINK_SEQ[0]);
static uint8_t        blinkIdx    = 0;
static bool           ledOn       = false;
static unsigned long  blinkAt     = 0;

// "дыхание" LED в норме (плавная волна яркости, ~4 сек)
static const unsigned long BREATH_MS = 4000UL;
// мигание экрана при аварии (инверсия флэш)
static unsigned long downFlashAt = 0;
static bool          downInvert  = false;
static bool          screenInvert = false;  // текущее состояние инверсии SSD1306

void ledWrite(bool on) {
  ledOn = on;
  digitalWrite(PIN_LED, on ? LOW : HIGH); // active LOW
}

void alertReset() {
  blinkIdx = 0;
  blinkAt  = millis();
  ledWrite(false);
}

void alertTick() {
  unsigned long now = millis();
  if (now - blinkAt >= BLINK_SEQ[blinkIdx]) {
    blinkAt = now;
    blinkIdx = (blinkIdx + 1) % BLINK_LEN;
    ledWrite(!ledOn);
  }
}

// ===================== ГЕОМЕТРИЯ ГЛАЗ =====================
// Проверено пиксельным моком (см. eyes_mock.py): глаз - вертикальная "пилюля",
// зрачок двигается внутри, веки сверху/снизу, брови трёх видов.
static const int  EYE_W = 30;
static const int  EYE_OK_Y  = 12;  // спокойный режим: глаза в центре
static const int  EYE_OK_H  = 38;
static const int  EYE_BAD_Y = 2;   // злой режим: глаза выше, снизу место для рта
static const int  EYE_BAD_H = 28;
static const int  EYE1_X = 18;
static const int  EYE2_X = 80;
static const int  MOUTH_X = 40, MOUTH_Y = 36, MOUTH_W = 48, MOUTH_H = 24;
static const float PUPIL_R_BASE = 7.0f;
static const int  BROW_Y = 8;

struct Pupil {
  float px, py;   // текущее смещение от центра
  float tx, ty;   // целевое смещение
  float pr, tr;   // текущий/целевой радиус
};

static Pupil eyeL = { 0, 0, 0, 0, PUPIL_R_BASE, PUPIL_R_BASE };
static Pupil eyeR = { 0, 0, 0, 0, PUPIL_R_BASE, PUPIL_R_BASE };

// ---- v3 living layer: ��㦨��, ᠪ����, ��堭�� ----
static float svelL[3] = { 0, 0, 0 };   // ᪮��� ��㦨�� (px, py, pr) ���� ����
static float svelR[3] = { 0, 0, 0 };   // �ࠢ� ����
static float gAx = 0.0f, gAy = 0.0f;   // ����-ᠪ���� (ᬥ饭�� ��窠 �� ���ᮢ��)
static int   gEhOff = 0;               // ��堭��: ᤢ�� ��� ���� �� y, -1..1
static unsigned long exhaleUntil = 0;  // �����-�����祭�� / ��������
static bool panicPrev = false;
static bool sleepPrev = false;
static unsigned long wakeUntil = 0;    // E|wake: �� ᯠ�� 2 ������

static void sstep(float& x, float& v, float tgt, float k, float d, float dt) {
  v += (k * (tgt - x) - d * v) * dt;
  if (v > 80.0f) v = 80.0f; else if (v < -80.0f) v = -80.0f;
  x += v * dt;
}

// v3: ��㦨���� ������� 楫�� + ����-ᠪ���� + ��堭�� (ࠧ � ����)
static void v3Phase(float dt, float sacAmp) {
  static float ph1 = 0.0f, ph2 = 0.0f, ph3 = 0.0f;
  ph1 += dt * 2.0f * PI / 1.7f;  if (ph1 > 2 * PI) ph1 -= 2 * PI;
  ph2 += dt * 2.0f * PI / 2.1f;  if (ph2 > 2 * PI) ph2 -= 2 * PI;
  ph3 += dt * 2.0f * PI / 2.6f;  if (ph3 > 2 * PI) ph3 -= 2 * PI;
  gAx = sacAmp * 0.6f * sinf(ph1);
  gAy = sacAmp * 0.35f * sinf(ph2 + 1.2f);
  gEhOff = (int)roundf(0.9f * sinf(ph3));
  sstep(eyeL.px, svelL[0], eyeL.tx, 110.0f, 11.0f, dt);
  sstep(eyeL.py, svelL[1], eyeL.ty, 110.0f, 11.0f, dt);
  sstep(eyeL.pr, svelL[2], eyeL.tr, 90.0f, 12.0f, dt);
  sstep(eyeR.px, svelR[0], eyeR.tx, 110.0f, 11.0f, dt);
  sstep(eyeR.py, svelR[1], eyeR.ty, 110.0f, 11.0f, dt);
  sstep(eyeR.pr, svelR[2], eyeR.tr, 90.0f, 12.0f, dt);
}

// поведение
static uint8_t brows = 0;        // 0 обычная, 1 поднята, 2 сердитая
static int lowerLid = 0;         // px прищура
static unsigned long nextBlinkAt = 0, blinkStart = 0;
static bool blinkActive = false, doubleBlink = false, blinkSecond = false;
static unsigned long nextLookAt = 0;
static unsigned long nextActAt  = 0;
static int  actMode = -1;        // -1 нет, 0 "замечает", 1 влево-вправо, 2 круг, 3 прищур
static int  actStep = 0;
static unsigned long actUntil = 0;
static unsigned long lastFrame = 0;
static const uint8_t FRAME_MS = 33; // ~30 fps

// =============================================================
// ДАННЫЕ СЕРВЕРА: микросервис pxmon запущен на TARGET:8123
// (на Proxmox: /usr/local/bin/pxmon.py + systemd pxmon.service).
// Отдаёт строки:  S|node|upsec|cpu%|ramMiB|ramMaxMiB
//                 V|id|name|r|s|memMiB|memMaxMiB
//                 D|storage|usedGiB|maxGiB   (топ-4 по размеру)
// =============================================================
enum DispMode { DM_EYES, DM_SLIDES };  // панель статуса теперь тоже слайд (№0)
static DispMode dispMode = DM_EYES;

static const unsigned long SLIDE_MS = 5000UL;           // каждый слайд 5 сек
static const unsigned long INFO_FIRST_MS  = 25000UL;    // первый показ после старта
static const unsigned long INFO_PERIOD_MIN = 120000UL;  // дальше раз в 2-3 минуты
static const unsigned long INFO_PERIOD_VAR = 61000UL;

static int slideIdx = 0, slideCount = 2;
static unsigned long slideUntil = 0;
static unsigned long nextInfoAt = 0;

struct VmSlide { int id; char name[14]; bool up; unsigned memMiB, memMaxMiB; };
struct ServerData {
  bool valid;
  char node[13];
  unsigned long uptime;
  float cpuPct;
  unsigned ramMiB, ramMaxMiB;
  unsigned long ltSec;   // локальные секунды с полуночи (для ночного режима)
  bool backup;           // 1 - идёт vzdump
  int8_t doy;            // день года 1..365 от pxmon (0 = не присылается; 1-7 = шапка)
  VmSlide vms[5]; int8_t nVms;
  char dName[5][9]; float dUsed[5], dMax[5]; int8_t nDisks;
};
static ServerData g_srv;
static char statsBuf[720];
static bool nightMode = false;                 // 23:00..07:00 по времени сервера
static const int NIGHT_START_HOUR = 23;
static const int NIGHT_END_HOUR = 7;

static char* nextTok(char*& p, char sep) {
  if (!p || !*p) return nullptr;
  char* s = p;
  char* e = strchr(p, sep);
  if (e) { *e = 0; p = e + 1; }
  else p = nullptr;
  return s;
}

// демо/серверные токены сцен ("peek|book|tiktok|pc|self|coffee|hat|none")
static void sceneSetToken(const char* tok);

static void parseStats() {
  memset(&g_srv, 0, sizeof(g_srv));
  char* cur = statsBuf;
  while (cur && *cur) {
    char* nl = strchr(cur, '\n');
    if (nl) *nl = 0;
    if (*cur == 'S' || *cur == 'V' || *cur == 'D' || *cur == 'E') {
      char* p = cur + 2;  // после маркера "X|"
      char* t;
      if (*cur == 'S') {
        t = nextTok(p, '|');  strlcpy(g_srv.node, t ? t : "?", sizeof(g_srv.node));
        t = nextTok(p, '|');  g_srv.uptime    = t ? atol(t) : 0;
        t = nextTok(p, '|');  g_srv.cpuPct    = t ? atof(t) : 0.0f;
        t = nextTok(p, '|');  g_srv.ramMiB    = t ? (unsigned)atol(t) : 0;
        t = nextTok(p, '|');  g_srv.ramMaxMiB = t ? (unsigned)atol(t) : 0;
        t = nextTok(p, '|');  g_srv.ltSec = t ? (unsigned long)atol(t) : 0;
        t = nextTok(p, '|');  g_srv.backup = (t && t[0] == '1');
        t = nextTok(p, '|');  g_srv.doy = (t && *t) ? (int8_t)atoi(t) : 0; // опция: день года для шапки
        g_srv.valid = true;
        int hour = (int)(g_srv.ltSec / 3600UL);
        nightMode = (g_srv.ltSec == 0UL) ? nightMode : (hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR);
      } else if (*cur == 'V' && g_srv.nVms < 5) {
        VmSlide& v = g_srv.vms[g_srv.nVms];
        t = nextTok(p, '|');  v.id = t ? atoi(t) : 0;
        t = nextTok(p, '|');  strlcpy(v.name, t ? t : "?", sizeof(v.name));
        t = nextTok(p, '|');  v.up = (t && t[0] == 'r');
        t = nextTok(p, '|');  v.memMiB = t ? (unsigned)atol(t) : 0;
        t = nextTok(p, '|');  v.memMaxMiB = t ? (unsigned)atol(t) : 0;
        g_srv.nVms++;
      } else if (*cur == 'D' && g_srv.nDisks < 5) {
        t = nextTok(p, '|');  strlcpy(g_srv.dName[g_srv.nDisks], t ? t : "?", 9);
        t = nextTok(p, '|');  g_srv.dUsed[g_srv.nDisks] = t ? atof(t) : 0.0f;
        t = nextTok(p, '|');  g_srv.dMax[g_srv.nDisks] = t ? atof(t) : 0.0f;
        g_srv.nDisks++;
      } else if (*cur == 'E') {
#if SCENES_ON
        sceneSetToken(cur + 2);   // "E|peek" и т.п. - демо-форс сцены/шапки
#endif
      }
    }
    cur = nl ? nl + 1 : nullptr;
  }
}

static bool statsFetch() {
  g_srv.valid = false;
  if (WiFi.status() != WL_CONNECTED || !targetIpValid) return false;
  String url = String("http://") + TARGET + ":8123/";
  WiFiClient wc;
  HTTPClient http;
  if (!http.begin(wc, url)) return false;
  http.setTimeout(4000);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    http.end();
    strlcpy(statsBuf, body.c_str(), sizeof(statsBuf));
    parseStats();
    Serial.print(F("[HTTP] stats ok: cpu="));
    Serial.print((int)(g_srv.cpuPct + 0.5f));
    Serial.print(F("% vms="));
    Serial.print((int)g_srv.nVms);
    Serial.print(F(" disks="));
    Serial.print((int)g_srv.nDisks);
    Serial.print(F(" night="));
    Serial.print(nightMode ? 1 : 0);
    Serial.print(F(" backup="));
    Serial.print(g_srv.backup ? 1 : 0);
    Serial.print(F(" doy="));
    Serial.println((int)g_srv.doy);
  } else {
    http.end();
    Serial.print(F("[HTTP] stats fail, code "));
    Serial.println(code);
  }
  return g_srv.valid;
}

// ---------- рендеры слайдов ----------
static void fmtG(float v, char* out, int n) {  // "12" или "12.3"
  int i = (int)v;
  int d = (int)((v - (float)i) * 10.0f + 0.5f);
  if (d >= 10) { i++; d = 0; }
  if (d == 0) snprintf(out, n, "%d", i);
  else snprintf(out, n, "%d.%d", i, d);
}

static void drawBar(int x, int y, int w, int h, float pct) {
  if (pct < 0) pct = 0; else if (pct > 1) pct = 1;
  display.drawRect(x, y, w, h, SSD1306_BLACK);
  if (w > 2 && h > 2) {
    int fw = (int)((w - 2) * pct + 0.5f);
    if (fw > 0) display.fillRect(x + 1, y + 1, fw, h - 2, SSD1306_BLACK);
  }
}

static void drawCheck(int x, int y) {
  display.drawLine(x, y + 3, x + 2, y + 6, SSD1306_BLACK);
  display.drawLine(x + 2, y + 6, x + 8, y, SSD1306_BLACK);
}

static void drawCross(int x, int y) {
  display.drawLine(x, y, x + 8, y + 7, SSD1306_BLACK);
  display.drawLine(x + 8, y, x, y + 7, SSD1306_BLACK);
}

static void slideHeader(const char* left, const char* right) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(0, 0);
  display.print(left);
  int rw = (int)strlen(right) * 6;
  display.setCursor(SCREEN_WIDTH - rw, 0);
  display.print(right);
  display.drawFastHLine(0, 9, SCREEN_WIDTH, SSD1306_BLACK);
}

static void slideFooter() {
  display.drawFastHLine(0, 51, SCREEN_WIDTH, SSD1306_BLACK);
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  char lbuf[24], rbuf[14];
  if (lastRtt >= 0) snprintf(lbuf, sizeof(lbuf), "ping:%dms", lastRtt);
  else strlcpy(lbuf, "no reply", sizeof(lbuf));
  unsigned long upS = g_srv.valid ? g_srv.uptime : (millis() - bootAt) / 1000UL;
  if (upS >= 86400UL) snprintf(rbuf, sizeof(rbuf), "up:%lud%02luh", upS / 86400UL, (upS % 86400UL) / 3600UL);
  else snprintf(rbuf, sizeof(rbuf), "up:%luh%02lum", upS / 3600UL, (upS % 3600UL) / 60UL);
  display.setCursor(0, 55);
  display.print(lbuf);
  int rw = (int)(strlen(rbuf) + 3) * 6;  // "up:" + значения
  display.setCursor(SCREEN_WIDTH - rw, 55);
  display.print(rbuf);
}

static void drawSlideServer() {
  display.fillScreen(SSD1306_WHITE);
  slideHeader("SERVER", g_srv.valid ? g_srv.node : "no data");
  char buf[18], a[8], b[8];
  if (g_srv.valid) {
    int cpu = (int)(g_srv.cpuPct + 0.5f);
    if (cpu > 999) cpu = 999;
    display.setTextSize(1);
    display.setCursor(2, 14);
    display.print(F("CPU"));
    display.setTextSize(2);
    display.setCursor(20, 10);
    snprintf(buf, sizeof(buf), "%d%%", cpu);
    display.print(buf);
    drawBar(84, 16, 40, 8, g_srv.cpuPct / 100.0f);

    display.setTextSize(1);
    display.setCursor(2, 30);
    display.print(F("RAM"));
    fmtG(g_srv.ramMiB / 1024.0f, a, sizeof(a));
    fmtG(g_srv.ramMaxMiB / 1024.0f, b, sizeof(b));
    snprintf(buf, sizeof(buf), "%s/%sG", a, b);
    display.setCursor(20, 30);
    display.print(buf);
    drawBar(84, 30, 40, 8, g_srv.ramMaxMiB ? (float)g_srv.ramMiB / g_srv.ramMaxMiB : 0.0f);

    int up = 0;
    for (int i = 0; i < g_srv.nVms; i++) if (g_srv.vms[i].up) up++;
    display.setCursor(2, 44);
    snprintf(buf, sizeof(buf), "VM %d/%d up", up, g_srv.nVms);
    display.print(buf);
    if (g_srv.backup) {
      display.setCursor(64, 44);
      display.print(F("BACKUP!"));
    }
  } else {
    display.setTextSize(2);
    display.setCursor((SCREEN_WIDTH - 8 * 12) / 2, 24);
    display.print(F("NO DATA!"));
  }
  slideFooter();
  display.display();
}

static void drawSlideVms() {
  display.fillScreen(SSD1306_WHITE);
  char rbuf[10];
  int up = 0;
  for (int i = 0; i < g_srv.nVms; i++) if (g_srv.vms[i].up) up++;
  snprintf(rbuf, sizeof(rbuf), "%d/%d", up, g_srv.nVms);
  slideHeader("VMs", rbuf);
  int step = g_srv.nVms > 3 ? 10 : 13;
  int y0 = g_srv.nVms > 3 ? 12 : 14;
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  for (int i = 0; i < g_srv.nVms; i++) {
    int y = y0 + step * i;
    const VmSlide& v = g_srv.vms[i];
    display.setCursor(2, y);
    display.print(v.id);
    display.setCursor(22, y);
    display.print(v.name);
    if (v.up) drawCheck(112, y + 1);
    else drawCross(112, y + 1);
  }
  slideFooter();
  display.display();
}

static void drawSlideDisks() {
  display.fillScreen(SSD1306_WHITE);
  slideHeader("DISKS", "GiB");
  char buf[18], a[8];
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  for (int i = 0; i < g_srv.nDisks; i++) {
    int y = 12 + 10 * i;
    display.setCursor(2, y);
    display.print(g_srv.dName[i]);
    fmtG(g_srv.dUsed[i], a, sizeof(a));
    snprintf(buf, sizeof(buf), "%s/%d", a, (int)(g_srv.dMax[i] + 0.5f));
    display.setCursor(80, y);
    display.print(buf);
    drawBar(52, y + 1, 26, 7, g_srv.dMax[i] > 0 ? g_srv.dUsed[i] / g_srv.dMax[i] : 0.0f);
  }
  slideFooter();
  display.display();
}

static void drawSlide(int idx) {
  if (!oledOk) return;
  switch (idx) {
    case 1:  drawSlideServer(); break;
    case 2:  drawSlideVms(); break;
    case 3:  drawSlideDisks(); break;
    default: showState(); break;  // слайд 0 - панель ОК/УПАЛ
  }
}

static int lidHalfWidth(int d, int r) {
  if (d <= 0) return 0;
  if (d >= r) return r;
  return (int)sqrtf((float)(r * r - (r - d) * (r - d)));
}

static void thickLine(int x0, int y0, int x1, int y1, int t) {
  for (int i = 0; i < t; i++) {
    display.drawLine(x0 + i, y0, x1 + i, y1, SSD1306_BLACK);
  }
}

static void drawEye(int ex, int ey, int eh, const Pupil& e, int lid, int lower) {
  ey += gEhOff;                              // v3: ��堭��
  int rr = EYE_W / 2;                        // радиус капсулы
  if (rr > eh / 2) rr = eh / 2;
  const int cy0 = ey + eh / 2;

  // зрачок: центр со сглаживанием + жёсткий кламп внутри глаза
  int pr = (int)(e.pr + 0.5f);
  int cx = (int)(ex + EYE_W / 2 + e.px + gAx + 0.5f);
  int cy = (int)(cy0 + e.py + gAy + 0.5f);
  int cxMin = ex + pr + 2, cxMax = ex + EYE_W - pr - 2;
  int cyMin = ey + pr + 3, cyMax = ey + eh - pr - 3;
  if (cx < cxMin) cx = cxMin;
  if (cx > cxMax) cx = cxMax;
  if (cy < cyMin) cy = cyMin;
  if (cy > cyMax) cy = cyMax;

  // белок
  display.fillRoundRect(ex, ey, EYE_W, eh, rr, SSD1306_WHITE);
  // зрачок + блик
  display.fillCircle(cx, cy, pr, SSD1306_BLACK);   // v3: ᯫ�譮� ��箪, ��� �����

  // верхнее веко
  if (lid > 0) {
    int hw = lidHalfWidth(lid, rr);
    display.fillRect(ex + EYE_W / 2 - hw, ey, hw * 2, lid, SSD1306_WHITE);
    display.fillRect(ex + EYE_W / 2 - hw, ey + lid - 2, hw * 2, 2, SSD1306_BLACK);
  }
  // нижнее веко (прищур)
  if (lower > 0) {
    int hw = lidHalfWidth(lower, rr);
    display.fillRect(ex + EYE_W / 2 - hw, ey + eh - lower, hw * 2, lower, SSD1306_WHITE);
    display.fillRect(ex + EYE_W / 2 - hw, ey + eh - lower, hw * 2, 2, SSD1306_BLACK);
  }
  // контур поверх век восстанавливает дуги
  display.drawRoundRect(ex, ey, EYE_W, eh, rr, SSD1306_BLACK);
  display.drawRoundRect(ex + 1, ey + 1, EYE_W - 2, eh - 2, (rr > 1) ? rr - 1 : 0, SSD1306_BLACK);   // v3: ������ 2px

  // брови
  // v2: бровь следует за глазом (ey-4); при ey<=12 это ровно BROW_Y - поведение v1 не меняется
  int by = ey - 4;
  if (by < BROW_Y) by = BROW_Y;
  if (brows == 1) {            // удивлённая/поднятая (только в спокойном режиме)
    thickLine(ex + 2, by - 3, ex + EYE_W - 4, by - 5, 2);
  } else if (brows == 2) {     // сердитая: наследает прямо на глаз сверху вниз к центру
    if (ex < SCREEN_WIDTH / 2) thickLine(ex + 2, ey - 2, ex + EYE_W - 2, ey + 4, 2);
    else                       thickLine(ex + 2, ey + 4, ex + EYE_W - 2, ey - 2, 2);
  } else {                     // спокойная с лёгким наклоном
    thickLine(ex + 2, by, ex + EYE_W - 4, by - 3, 2);
  }
}

// ===================== РОТ (только в злом режиме) =====================
// Мультяшный рот, который "матерится": 3 варианта, быстро сменяются (~170 мс)
static void drawMouth(int variant) {
  const int mx = MOUTH_X, my = MOUTH_Y;
  if (variant == 0) {
    // ОРЕТ: открытый рот, зубастая пасть
    display.fillRoundRect(mx, my, MOUTH_W, MOUTH_H, 10, SSD1306_BLACK);
    const int midy = my + MOUTH_H / 2; // 48
    for (int i = 0; i < 6; i++) {
      int x = mx + 4 + i * 8;
      if (i % 2 == 0) display.fillTriangle(x, midy - 5, x + 8, midy - 5, x + 4, midy + 1, SSD1306_WHITE);
      else            display.fillTriangle(x, midy + 5, x + 8, midy + 5, x + 4, midy - 1, SSD1306_WHITE);
    }
  } else if (variant == 1) {
    // СКРЕЖЕЩЕТ зубами: контур рта + зубы
    display.drawRoundRect(mx, my, MOUTH_W, MOUTH_H, 10, SSD1306_BLACK);
    display.drawRoundRect(mx + 1, my + 1, MOUTH_W - 2, MOUTH_H - 2, 9, SSD1306_BLACK);
    const int midy = my + MOUTH_H / 2;
    for (int i = 0; i < 6; i++) {
      int x = mx + 4 + i * 8;
      if (i % 2 == 0) display.fillTriangle(x, my + 2, x + 8, my + 2, x + 4, midy, SSD1306_BLACK);
      else            display.fillTriangle(x, my + MOUTH_H - 2, x + 8, my + MOUTH_H - 2, x + 4, midy, SSD1306_BLACK);
    }
  } else {
    // "ОООО!!!"
    display.fillCircle(64, my + MOUTH_H / 2, 10, SSD1306_BLACK);
    display.fillCircle(64, my + MOUTH_H / 2 - 2, 4, SSD1306_WHITE);
  }
}

// беда для глаз: гипервизор недоступен ИЛИ хоть одна VM не работает
static bool eyesInTrouble() {
  if (state != ST_OK) return true;
  if (!g_srv.valid) return false;
  for (int i = 0; i < g_srv.nVms; i++) if (!g_srv.vms[i].up) return true;
  return false;
}

#if SCENES_ON
static bool hatShown();
static void drawSantaHat();
static void sleepReset();
#endif

static void eyesDrawFrame(int lid, int lower, int mouth) {
  if (!oledOk) return;
  display.fillScreen(SSD1306_WHITE);
  bool trouble = eyesInTrouble();
  int ey, eh;
  if (!trouble)       { ey = EYE_OK_Y; eh = EYE_OK_H; }
  else                { ey = EYE_BAD_Y; eh = EYE_BAD_H; }
  drawEye(EYE1_X, ey, eh, eyeL, lid, lower);
  drawEye(EYE2_X, ey, eh, eyeR, lid, lower);
  if (trouble) {
    drawMouth(mouth);
  } else if (g_srv.backup) {
    // ПАНИКА: схватился за голову - лапки сверху + рот от ужаса "О"
    display.fillCircle(EYE1_X + 8, 4, 6, SSD1306_WHITE);
    display.drawCircle(EYE1_X + 8, 4, 6, SSD1306_BLACK);
    display.fillCircle(EYE2_X + 16, 4, 6, SSD1306_WHITE);
    display.drawCircle(EYE2_X + 16, 4, 6, SSD1306_BLACK);
    display.fillCircle(64, 57, 5, SSD1306_BLACK);
  } else if (nightMode) {
    // сон: летящий "Zz"
    if ((millis() / 4000UL) % 2UL == 0UL) {
      display.setTextSize(1);
      display.setTextColor(SSD1306_BLACK);
      display.setCursor(112, 2);
      display.print(F("Z"));
      display.setCursor(118, 9);
      display.print(F("z"));
    }
  }
#if SCENES_ON
  else if (hatShown()) {
    // новогодняя шапка в зеве между бровей (день, спокойный кадр)
    drawSantaHat();
  }
#endif
  display.display();
}

// ===================== ПОВЕДЕНИЕ ГЛАЗ =====================
static void setBothTargets(float tx, float ty, float tr) {
  eyeL.tx = eyeR.tx = tx;
  eyeL.ty = eyeR.ty = ty;
  eyeL.tr = eyeR.tr = tr;
}

// =============================================================
// СЦЕНЫ «ЖИВОЙ ДЕНЬ» v2 + НОЧНОЙ СОН (перенос из scenes_mock.py)
// =============================================================
#if SCENES_ON

enum SceneName { SC_NONE = 0, SC_PEEK, SC_BOOK, SC_TIKTOK, SC_PC, SC_SELF, SC_COFFEE };

static const uint8_t  SCENE_STEPS[7] = { 0, 4, 5, 5, 4, 5, 4 };
static const uint16_t SCENE_STEP_MS[7] = { 0, 950, 830, 800, 900, 800, 900 };
static const unsigned long SCENE_GAP_MS     = 130000UL; // пауза между сценами ~2-4 мин
static const unsigned long SCENE_GAP_VAR_MS = 150000UL;
static const char* const SCENE_NAMES[] = { "none", "peek", "book", "tiktok", "pc", "self", "coffee" };

static int  sceneName   = SC_NONE;
static int  sceneStep   = 0;
static unsigned long sceneStepAt = 0;
static unsigned long nextSceneAt = 0;      // авто-сцены: первая через 30с, дальше раз в 2-4 мин
static int  forceScene  = -1;              // демо-форс через E|<token>
static bool forceHat    = false;           // демо: E|hat
static bool sleepActive = false;           // активна ли фаза сна (для сброса при выходе из ночи)
static uint8_t  sleepPh  = 0;              // 0..3 засыпание, 4..5 облачко, 6+ сны
static unsigned long sleepPhAt = 0;
static uint8_t  dreamIdx = 0;              // 0..4: Zz, сервер, рыбка, кот, шапка
static uint8_t  dreamPh  = 0;              // фаза анимации сна
static unsigned long dreamAt = 0;

// первичные дугу/кривую/полутень в Adafruit_GFX рисуем сами
static void qcurve(int x0, int y0, int cx, int cy, int x1, int y1, int w) {
  int px_ = x0, py_ = y0;
  const int n = 12;
  for (int i = 1; i <= n; i++) {
    float t = i / (float)n;
    float u = 1.0f - t;
    int x = (int)(u * u * x0 + 2 * u * t * cx + t * t * x1 + 0.5f);
    int y = (int)(u * u * y0 + 2 * u * t * cy + t * t * y1 + 0.5f);
    display.drawLine(px_, py_, x, y, SSD1306_BLACK);
    if (w > 1) display.drawLine(px_, py_ + 1, x, y + 1, SSD1306_BLACK);
    px_ = x; py_ = y;
  }
}

static void arcThin(int cx, int cy, int rx, int ry, int a0, int a1, int w = 2) {
  const int n = 20;
  int px_ = -9999, py_ = -9999;
  if (a1 < a0) a1 += 360;
  for (int i = 0; i <= n; i++) {
    float a = (a0 + (a1 - a0) * i / (float)n) * PI / 180.0f;
    int x = (int)(cx + rx * cosf(a) + 0.5f);
    int y = (int)(cy + ry * sinf(a) + 0.5f);
    if (px_ != -9999) {
      for (int o = 0; o < w; o++) display.drawLine(px_, py_ + o, x, y + o, SSD1306_BLACK);
    }
    px_ = x; py_ = y;
  }
}

static void fillEll(int cx, int cy, int rx, int ry, uint16_t col) {
  if (rx < 1 || ry < 1) return;
  for (int yy = -ry; yy <= ry; yy++) {
    float k = 1.0f - (yy * yy) / (float)(ry * ry);
    if (k < 0) k = 0;
    int hw = (int)(rx * sqrtf(k) + 0.5f);
    display.drawFastHLine(cx - hw, cy + yy, hw * 2 + 1, col);
  }
}

static void ditherOval(int cx, int cy, int rx, int ry) {
  for (int yy = -ry; yy <= ry; yy++) {
    float k = 1.0f - (yy * yy) / (float)(ry * ry);
    if (k < 0) k = 0;
    int half = (int)(rx * sqrtf(k) + 0.5f);
    for (int xx = -half; xx <= half; xx++)
      if (((xx + yy) & 1) == 0) display.drawPixel(cx + xx, cy + yy, SSD1306_BLACK);
  }
}

static void spark(int cx, int cy) {
  display.drawLine(cx - 2, cy, cx + 2, cy, SSD1306_BLACK);
  display.drawLine(cx, cy - 2, cx, cy + 2, SSD1306_BLACK);
}

static void oledText(int x, int y, const char* s) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(x, y);
  display.print(s);
}

// ---------- реквизит сцен ----------

static void drawLN(int x0, int y0, int x1, int y1, int t) {
  display.drawLine(x0, y0, x1, y1, SSD1306_BLACK);
  if (t > 1) display.drawLine(x0, y0 + 1, x1, y1 + 1, SSD1306_BLACK);
}

// ---- v3 �ய: ��㦪�-ᨫ���, ��窠-"�㡫��", ���� ���� ----
static void drawMug(int dy) {
  const int top = 44 + dy, bot = 60 + dy;
  const int midy = (top + bot) / 2;
  display.fillRoundRect(55, top, 21, bot - top, 4, SSD1306_BLACK);
  display.fillCircle(54, midy, 5, SSD1306_BLACK);   // ��窠
  display.fillCircle(54, midy, 2, SSD1306_WHITE);
  display.fillRect(59, top + 4, 2, (bot - top) - 7, SSD1306_WHITE);   // ����
}

// ---- v3 �ய: ������ �����, ���� ��࠭��� + ������ 2px + ��ப� ----
static void drawBook(int dy) {
  const int top = 44 + dy, bot = 63 + dy;
  display.fillTriangle(35, top + 2, 63, top, 63, bot - 1, SSD1306_WHITE);
  display.fillTriangle(35, top + 2, 35, bot - 1, 63, bot - 1, SSD1306_WHITE);
  display.fillTriangle(93, top + 2, 65, top, 65, bot - 1, SSD1306_WHITE);
  display.fillTriangle(93, top + 2, 93, bot - 1, 65, bot - 1, SSD1306_WHITE);
  drawLN(35, top + 2, 63, top, 2);
  drawLN(35, bot - 1, 63, bot - 1, 2);
  drawLN(65, top, 93, top + 2, 2);
  drawLN(65, bot - 1, 93, bot - 1, 2);
  display.fillRect(34, top + 2, 2, bot - top - 3, SSD1306_BLACK);   // ���譨� �ࠩ
  display.fillRect(92, top + 2, 2, bot - top - 3, SSD1306_BLACK);
  display.fillRect(63, top, 3, bot - top - 1, SSD1306_BLACK);       // ���讪
  drawLN(40, 52 + dy, 58, 50 + dy, 2);                              // ��ப�
  drawLN(70, 50 + dy, 88, 52 + dy, 2);
}

// ---- v3 �ய: ⥫�䮭-ᨫ���, ���� ��࠭, play ----
static void drawPhoneV3(int ph) {
  display.fillRoundRect(53, ph, 23, 33, 4, SSD1306_BLACK);
  display.fillRect(56, ph + 4, 17, 21, SSD1306_WHITE);
  display.fillTriangle(61, ph + 9, 61, ph + 18, 68, ph + 13, SSD1306_BLACK);
  display.fillRect(60, ph + 27, 9, 2, SSD1306_WHITE);
}

static void drawHeartV3(float hy, float hs) {
  int rv = (int)(3.4f * hs + 0.5f), rw = (int)(3.6f * hs + 0.5f);
  int cy = (int)(hy + 0.5f);
  int tip = (int)(8.0f * hs + 0.5f);
  display.fillCircle(64 - rw, cy, rv, SSD1306_BLACK);
  display.fillCircle(64 + rw, cy, rv, SSD1306_BLACK);
  display.fillTriangle(63 - rw, cy - 1, 65 + rw, cy - 1, 64, cy + tip, SSD1306_BLACK);
}

// ---- v3: ᬥ�騥�� ����� (���騥 �㦪�) ----
static void drawHappyArcs(unsigned long now) {
  int dy = (int)roundf(0.8f * sinf(now * 0.0031f));
  arcThin(EYE1_X + 15, 32 + dy, 10, 8, 200, 340, 3);
  arcThin(EYE2_X + 15, 32 + dy, 10, 8, 200, 340, 3);
}

// ---- v3 �ய: ��������� - ���� ᯫ�譮� �� �� 6 ������ ----
static void drawKbdV3(unsigned long now, int pressedKey, bool shake) {
  int dx = shake ? (int)(1.5f * sinf(now * 0.045f)) : 0;
  for (int i = 0; i < 6; i++) {
    int kx = 38 + i * 9 + dx;
    bool flick = (pressedKey == i) || (shake && (((now / 150UL) + i) % 6 == 0));
    if (flick) {
      display.drawRect(kx, 52, 8, 7, SSD1306_BLACK);
      display.drawRect(kx + 1, 53, 6, 5, SSD1306_BLACK);
    } else {
      display.fillRect(kx, 52, 8, 7, SSD1306_BLACK);
    }
  }
}

static void drawMouseV3(int x, int y, bool click) {
  display.fillRoundRect(x, y, 9, 11, 3, SSD1306_BLACK);
  if (click) display.fillRect(x + 3, y + 1, 3, 4, SSD1306_WHITE);
}

static void drawCursorV3(int x, int y) {
  display.fillTriangle(x, y, x, y + 7, x + 5, y + 5, SSD1306_BLACK);
  display.fillTriangle(x + 3, y + 5, x + 6, y + 9, x + 8, y + 7, SSD1306_BLACK);
}

// ---- v3 �ய: ��᫥��� ����� 2px, �窨 1..3 � "!" ----
static void drawThoughtV3(int phase, float pop) {
  int rx = 8 + (int)(8.0f * pop + 0.5f);        // 10..16
  fillEll(64, 8, rx, 8, SSD1306_BLACK);
  if (rx > 3) fillEll(64, 8, rx - 2, 6, SSD1306_WHITE);
  fillEll(64, 17, 3, 3, SSD1306_BLACK);         // 墮�⨪
  fillEll(64, 17, 1, 1, SSD1306_WHITE);
  fillEll(58, 21, 2, 2, SSD1306_BLACK);
  fillEll(58, 21, 1, 1, SSD1306_WHITE);
  if (phase >= 1 && phase <= 3) {
    for (int i = 0; i < phase; i++) display.fillCircle(54 + i * 8, 8, 2, SSD1306_BLACK);
  } else if (phase == 4) {
    display.fillRect(62, 2, 3, 7, SSD1306_BLACK);   // "!"
    display.fillRect(62, 11, 3, 3, SSD1306_BLACK);
  }
}

static bool hatShown() {
  if (forceHat) return true;
  return g_srv.valid && g_srv.doy >= 1 && g_srv.doy <= 7;
}

static void drawSantaHat() {
  display.fillTriangle(54, 4, 74, 4, 66, 0, SSD1306_BLACK);  // конус вверх по зеву
  display.fillRect(52, 4, 24, 3, SSD1306_BLACK);             // бахрома
  display.fillCircle(66, 0, 2, SSD1306_BLACK);               // помпон (за краем наполовину)
}

// ---------- сон: закрытые глаза, облачко и мечты ----------

static void drawClosedEyes() {
  display.drawRoundRect(18, 30, 30, 26, 13, SSD1306_BLACK);
  display.drawRoundRect(19, 31, 28, 24, 12, SSD1306_BLACK);
  display.drawRoundRect(80, 30, 30, 26, 13, SSD1306_BLACK);
  display.drawRoundRect(81, 31, 28, 24, 12, SSD1306_BLACK);
  display.fillRect(19, 43, 28, 2, SSD1306_BLACK);
  display.fillRect(81, 43, 28, 2, SSD1306_BLACK);
}

static void drawCloud(int pop) {                             // pop 1..3: ���㢠���
  float s = pop / 3.0f;
  const int cx = 64, cyb = 26;
  int ccy = cyb - (int)(10.0f * s + 0.5f);
  int crx = (int)(13.0f * s + 0.5f), cry = (int)(10.0f * s + 0.5f);
  int scy = cyb - (int)(5.0f * s + 0.5f);
  int srx = (int)(9.0f * s + 0.5f), sry = (int)(6.0f * s + 0.5f);
  int sdx = (int)(12.0f * s + 0.5f);
  fillEll(cx, ccy, crx, cry, SSD1306_BLACK);
  fillEll(cx - sdx, scy, srx, sry, SSD1306_BLACK);
  fillEll(cx + sdx, scy, srx, sry, SSD1306_BLACK);
  if (crx > 3) fillEll(cx, ccy, crx - 2, cry - 2, SSD1306_WHITE);          // ���� 2px
  if (srx > 3) {
    fillEll(cx - sdx, scy, srx - 2, sry - 2, SSD1306_WHITE);
    fillEll(cx + sdx, scy, srx - 2, sry - 2, SSD1306_WHITE);
  }
  if (pop == 3) {
    fillEll(68, 30, 3, 3, SSD1306_BLACK);
    fillEll(68, 30, 1, 1, SSD1306_WHITE);
    fillEll(61, 34, 2, 2, SSD1306_BLACK);
    fillEll(61, 34, 1, 1, SSD1306_WHITE);
  }
}

static void drawDreamZzz() {
  oledText(52, 8 + (dreamPh ? 1 : 0), "Zz");
  oledText(64, 15, "z");
}

static void drawDreamServer(int ph) {
  display.fillRoundRect(52, 7, 25, 14, 2, SSD1306_BLACK);     // �����-ᨫ���
  for (int yy = 10; yy <= 18; yy += 4) display.fillRect(55, yy, 11, 2, SSD1306_WHITE);
  display.fillRect(69, (ph == 0) ? 10 : 14, 4, 3, SSD1306_WHITE);   // ������� �����
}

static void drawDreamFish(int ph) {
  int dy = ph ? -1 : 1;
  int y = 14 + dy;
  display.fillRoundRect(52, y - 4, 18, 9, 4, SSD1306_BLACK);  // ⥫�
  display.fillTriangle(70, y - 4, 77, y - 7 - dy, 77, y + 7 - dy, SSD1306_BLACK);
  display.fillRect(56, y - 1, 2, 2, SSD1306_WHITE);           // ����
  display.fillRect(60, y, 7, 1, SSD1306_WHITE);               // ��
}

static void drawDreamCat(int ph) {
  int ear = ph ? 1 : 0;
  display.fillRoundRect(52, 9, 23, 19, 9, SSD1306_BLACK);           // ������
  display.fillTriangle(54, 14, 50, 5 - ear, 61, 10, SSD1306_BLACK); // ��
  display.fillTriangle(66, 10, 76, 5 - ear, 72, 14, SSD1306_BLACK);
  display.fillRect(55, 18, 6, 2, SSD1306_WHITE);                    // ��騥 �����
  display.fillRect(65, 18, 6, 2, SSD1306_WHITE);
  display.fillTriangle(61, 21, 66, 21, 63, 23, SSD1306_WHITE);      // ��ᨪ
  display.fillRect(47, 20, 5, 2, SSD1306_BLACK);                    // ���
  display.fillRect(76, 20, 5, 2, SSD1306_BLACK);
  if (ph) qcurve(72, 25, 80, 29, 82, 22, 2);                        // 墮��
  else    qcurve(72, 25, 79, 30, 80, 17, 2);
}

static void drawDreamHat(int ph) {
  int dy = ph ? 1 : 0;
  display.fillTriangle(64, 6 + dy, 55, 22 + dy, 73, 22 + dy, SSD1306_BLACK);
  display.fillCircle(64, 5 + dy, 5, SSD1306_BLACK);           // ������
  display.fillRect(54, 21 + dy, 21, 4, SSD1306_BLACK);        // ������
  display.fillRect(56, 25 + dy, 17, 2, SSD1306_BLACK);
  if (ph) spark(50, 10);
  else    spark(78, 12);
}

static void drawDreams() {
  switch (dreamIdx) {
    case 0:  drawDreamZzz(); break;
    case 1:  drawDreamServer(dreamPh); break;
    case 2:  drawDreamFish(dreamPh); break;
    case 3:  drawDreamCat(dreamPh); break;
    default: drawDreamHat(dreamPh); break;
  }
}

// ---------- ��設� �業 ----------

static void sceneStart(int s) {
  sceneName = s;
  sceneStep = 0;
  sceneStepAt = millis() + SCENE_STEP_MS[s];
  // v3: �業� ��稭����� �� ������� (blink-⥫�����)
  blinkActive = true; blinkStart = millis();
  doubleBlink = false; blinkSecond = false;
  nextBlinkAt = millis() + 2200UL + random(0, 3500UL);
  Serial.print(F("[SCENE] start "));
  Serial.println(SCENE_NAMES[s]);
}

static void sceneClear() {
  if (sceneName != SC_NONE) {
    Serial.print(F("[SCENE] interrupted: "));
    Serial.println(SCENE_NAMES[sceneName]);
  }
  sceneName = SC_NONE;
  sceneStep = 0;
  forceScene = -1;
}

static void sceneSetToken(const char* tok) {
  if (!tok || !*tok) return;
  if (!strcmp(tok, "wake")) {          // v3: �� ᯠ�� 2 ������
    wakeUntil = millis() + 120000UL;
    sleepActive = false; sleepReset();
    dispMode = DM_EYES; lastFrame = 0;
    Serial.println(F("[SCENE] wake 2min"));
    return;
  }
  if (!strcmp(tok, "none")) { forceHat = false; sceneClear(); Serial.println(F("[SCENE] cleared")); return; }
  if (!strcmp(tok, "hat"))  { forceScene = -1; forceHat = true; sceneName = SC_NONE;
                              dispMode = DM_EYES; lastFrame = 0;
                              if (nextInfoAt < millis() + 60000UL) nextInfoAt = millis() + 60000UL;
                              Serial.println(F("[SCENE] hat forced")); return; }
  for (int i = SC_PEEK; i <= SC_COFFEE; i++) {
    if (!strcmp(tok, SCENE_NAMES[i])) {
      forceScene = i;
      sceneStart(i);
      forceScene = -1;         // форс одноразовый: иначе сцена зациклится
      dispMode = DM_EYES;      // форс сцены вернёт глаза из слайдов
      lastFrame = 0;
      if (nextInfoAt < millis() + 60000UL) nextInfoAt = millis() + 60000UL;
      return;
    }
  }
}

// ---------- USB-сериал: команды "E|<token>\n" (порт открывать без DTR/RTS - без ресета) ----------
static char serialxBuf[24];
static uint8_t serialxLen = 0;

static void serialCmdTick() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialxLen < 24) {
        serialxBuf[serialxLen] = 0;
        if (serialxLen >= 3 && serialxBuf[0] == 'E' && serialxBuf[1] == '|') {
          Serial.print(F("[SCENE] usb cmd: "));
          Serial.println(serialxBuf + 2);
          sceneSetToken(serialxBuf + 2);
        }
      }
      serialxLen = 0;
      continue;
    }
    if (serialxLen < 23) serialxBuf[serialxLen++] = c;
    else serialxLen = 24;   // переполнение - строку не рассматриваем
  }
}

// прогресс шага активной сцены: true = кадр сцены играет
static bool sceneStepTick(unsigned long now) {
  if (sceneName == SC_NONE) return false;
  if (now >= sceneStepAt) {
    sceneStep++;
    if (sceneStep >= SCENE_STEPS[sceneName]) {
      Serial.print(F("[SCENE] end "));
      Serial.println(SCENE_NAMES[sceneName]);
      sceneName = SC_NONE;
      sceneStep = 0;
      nextSceneAt = now + SCENE_GAP_MS + random(0, SCENE_GAP_VAR_MS);
      return false;
    }
    sceneStepAt = now + SCENE_STEP_MS[sceneName] + random(0, 350UL);
  }
  return true;
}

// true = сцена играет и рисуется собственным кадром
static bool sceneTick(unsigned long now) {
  if (sceneName == SC_NONE) {
    if (forceScene >= 0) {
      sceneStart(forceScene);
      forceScene = -1;
    } else if (now >= nextSceneAt) {
      sceneStart(random(1, SC_COFFEE + 1));
    }
    if (sceneName == SC_NONE) return false;
  }
  return sceneStepTick(now);
}

// зрачки на цели сцены (mock-значения px/py/r по шагам)
// v3: 楫� �業 (px, py, r) �� 蠣��
static void sceneTargets() {
  switch (sceneName) {
    case SC_PEEK: {
      static const int8_t tp[4][2] = { { 0, 6 }, { 3, -4 }, { -4, -4 }, { 0, 5 } };
      setBothTargets(tp[sceneStep][0], tp[sceneStep][1], 7);
      break; }
    case SC_BOOK: {
      static const int8_t tp[5][2] = { { -5, 1 }, { 5, 1 }, { -3, 3 }, { 3, 3 }, { 0, -2 } };
      static const uint8_t tr[5]   = { 6, 6, 6, 6, 9 };     // 9 = �ப�: "��!"
      setBothTargets(tp[sceneStep][0], tp[sceneStep][1], tr[sceneStep]);
      break; }
    case SC_TIKTOK: {
      static const int8_t tp[5][2] = { { 0, 4 }, { 0, 4 }, { 1, -1 }, { 2, -2 }, { 0, 4 } };
      static const uint8_t tr[5]   = { 6, 6, 7, 7, 6 };
      setBothTargets(tp[sceneStep][0], tp[sceneStep][1], tr[sceneStep]);
      break; }
    case SC_PC: {
      static const int8_t tp[4][2] = { { 5, -1 }, { -5, -1 }, { 4, 1 }, { 0, 2 } };
      setBothTargets(tp[sceneStep][0], tp[sceneStep][1], 7);
      break; }
    case SC_SELF: {
      static const int8_t tp[5][2] = { { -4, -4 }, { -5, -3 }, { -5, -3 }, { 3, 3 }, { 2, 1 } };
      setBothTargets(tp[sceneStep][0], tp[sceneStep][1], 7);
      break; }
    case SC_COFFEE: {
      setBothTargets(0, 2, 7);
      break; }
    default: break;
  }
}

// �ண��� ⥪�饣� 蠣� (�� �� ��� ��砫�); sceneStepAt - ��⪠ ���� 蠣�
static int stepElapsed(unsigned long now) {
  int rem = (int)(sceneStepAt - now);
  if (rem < 0) rem = 0;
  int el = (int)SCENE_STEP_MS[sceneName] - rem;
  if (el < 0) el = 0;
  return el;
}

static void sceneDrawPeek(int lid) {
  // ������뢠���: ����� ������� ᭨��, ���뢠����, ����, ����뢠����
  static const int8_t ge[4][4] = { { 33, 24, 0, 0 }, { 12, 38, 1, 0 }, { 12, 38, 1, 0 }, { 33, 24, 0, 0 } };
  brows = (uint8_t)ge[sceneStep][2];
  drawEye(EYE1_X, ge[sceneStep][0], ge[sceneStep][1], eyeL, lid, ge[sceneStep][3]);
  drawEye(EYE2_X, ge[sceneStep][0], ge[sceneStep][1], eyeR, lid, ge[sceneStep][3]);
}

static void sceneDrawBook(int lid, unsigned long now) {
  brows = (sceneStep >= 2) ? 1 : 0;
  drawEye(EYE1_X, EYE_OK_Y, EYE_OK_H, eyeL, lid, 0);
  drawEye(EYE2_X, EYE_OK_Y, EYE_OK_H, eyeR, lid, 0);
  int dy;
  if (sceneStep == 0) {
    dy = 22;                                            // ����� ��� �� ����堫�
  } else if (sceneStep == 1) {
    int el = stepElapsed(now);
    dy = (el >= 420) ? 0 : (int)(22.0f * (1.0f - el / 420.0f));
  } else if (sceneStep >= 4) {
    int el = stepElapsed(now);
    dy = (int)(24.0f * el / 750.0f);                    // ����� ������
    if (dy > 24) dy = 24;
  } else {
    dy = 0;
  }
  if (dy < 24) drawBook(dy);
}

static void sceneDrawTiktok(int lid, unsigned long now) {
  brows = 0;
  drawPhoneV3(16);
  if (sceneStep == 2 || sceneStep == 3) {
    // �थ窮 ���� �१ ��� 蠣� ������, ����� - �㦪�
    unsigned long el = (unsigned long)stepElapsed(now);
    unsigned long e1 = (sceneStep == 2) ? el : el + 400UL;
    if (e1 > 800UL) e1 = 800UL;
    float u = (float)e1 / 800.0f;
    float uu = u * u * (3.0f - 2.0f * u);
    drawHeartV3(34.0f - 26.0f * uu, 1.3f - 0.3f * uu);
    drawHappyArcs(now);
  } else {
    drawEye(EYE1_X, EYE_OK_Y, EYE_OK_H, eyeL, lid, 0);
    drawEye(EYE2_X, EYE_OK_Y, EYE_OK_H, eyeR, lid, 0);
  }
}

static void sceneDrawPc(int lid, unsigned long now) {
  switch (sceneStep) {
    case 0:
      brows = 0;
      drawKbdV3(now, (int)((now / 160UL) % 6UL), false);
      drawMouseV3(100, 51, false);
      break;
    case 1:
      brows = 1;
      drawKbdV3(now, (int)((now / 190UL) % 6UL), false);
      drawMouseV3(100, 51, (now - (sceneStepAt - SCENE_STEP_MS[sceneName])) < 240UL);
      break;
    case 2: {
      brows = 1;
      float u = (float)stepElapsed(now) / 850.0f;
      if (u > 1.0f) u = 1.0f;
      u = u * u * (3.0f - 2.0f * u);
      drawKbdV3(now, -1, false);
      drawMouseV3(100, 51, false);
      drawCursorV3(54 + (int)(10.0f * u), 40);
      break; }
    default:
      brows = 2;                                        // ������ �� ���᪥
      drawKbdV3(now, -1, true);
      drawMouseV3(100, 51, false);
      break;
  }
  drawEye(EYE1_X, EYE_OK_Y, EYE_OK_H, eyeL, lid, sceneStep >= 3 ? 4 : 0);
  drawEye(EYE2_X, EYE_OK_Y, EYE_OK_H, eyeR, lid, sceneStep >= 3 ? 4 : 0);
}

static void sceneDrawSelf(int lid, unsigned long now) {
  static const uint8_t PH[5] = { 1, 2, 3, 4, 0 };       // �窨, �窨, "!", �����
  brows = (sceneStep < 3) ? 1 : 0;
  if (sceneStep >= 3) {
    drawHappyArcs(now);
  } else {
    drawEye(EYE1_X, EYE_OK_Y, EYE_OK_H, eyeL, lid, 0);
    drawEye(EYE2_X, EYE_OK_Y, EYE_OK_H, eyeR, lid, 0);
  }
  float pop = 1.0f;
  if (sceneStep == 3) pop = 1.0f - 0.08f * fabsf(sinf(now * 0.021f));
  drawThoughtV3(PH[sceneStep], pop);
}

static void sceneDrawCoffee(int lid) {
  brows = 0;
  int lower = (sceneStep == 1 || sceneStep == 2) ? 11 : 8;   // �����-����
  drawEye(EYE1_X, EYE_OK_Y, EYE_OK_H, eyeL, lid, lower);
  drawEye(EYE2_X, EYE_OK_Y, EYE_OK_H, eyeR, lid, lower);
  drawMug((sceneStep >= 1 && sceneStep <= 2) ? -2 : 0);
}

static void sceneFrame(unsigned long now, int lid) {
  if (!oledOk) return;
  display.fillScreen(SSD1306_WHITE);
  sceneTargets();
  switch (sceneName) {
    case SC_PEEK:   sceneDrawPeek(lid);        break;
    case SC_BOOK:   sceneDrawBook(lid, now);   break;
    case SC_TIKTOK: sceneDrawTiktok(lid, now); break;
    case SC_PC:     sceneDrawPc(lid, now);     break;
    case SC_SELF:   sceneDrawSelf(lid, now);   break;
    case SC_COFFEE: sceneDrawCoffee(lid);      break;
    default: break;
  }
  display.display();
}

// ---------- ��設� ᭠ ----------

static void sleepReset() {
  sleepPh = 0;
  sleepPhAt = 0;
  dreamPh = 0;
  dreamAt = 0;   // dreamIdx не сбрасываем: сон продолжится с той же мечты
}

static void sleepTick(unsigned long now) {
  if (now < sleepPhAt) return;
  if (sleepPh <= 4) {
    sleepPh++;
    sleepPhAt = now + 450UL;               // сползание век + надувание облачка
  } else {
    if (dreamPh == 0) { dreamPh = 1; }
    else { dreamPh = 0; dreamIdx = (dreamIdx + 1) % 5; }
    sleepPhAt = now + 700UL;
  }
}

static void sleepFrame(unsigned long now) {
  if (!oledOk) return;
  display.fillScreen(SSD1306_WHITE);
  if (sleepPh <= 2) {                      // засыпание: глаза съезжают и закрываются
    static const int8_t sp[3][3] = { {16,34,12}, {22,30,22}, {27,26,26} };
    brows = 0;
    setBothTargets(0, 3, 6);
    int ey = sp[sleepPh][0], eh = sp[sleepPh][1], lv = sp[sleepPh][2];
    drawEye(EYE1_X, ey, eh, eyeL, lv, 0);
    drawEye(EYE2_X, ey, eh, eyeR, lv, 0);
  } else {
    drawClosedEyes();
    drawCloud(sleepPh <= 4 ? sleepPh - 3 : 3);   // надувание облачка 1..2, потом полное
    if (sleepPh > 4) drawDreams();
  }
  display.display();
}

#endif // SCENES_ON

static void eyesTick() {
  unsigned long now = millis();
  if (now - lastFrame < FRAME_MS) return;
  lastFrame = now;

  bool down = eyesInTrouble();   // злые глаза на любую беду: гип упал ИЛИ VM выключена
  int  eh   = down ? EYE_BAD_H : EYE_OK_H;
  bool panic = (!down && g_srv.backup);   // идёт бэкап: глаза за голову
  bool sleep_ = (nightMode && !down && !panic);  // ночной режим: сон

  // ---- моргание ----
  if (!blinkActive && now >= nextBlinkAt) {
    blinkActive = true;
    blinkStart = now;
    doubleBlink = (random(0, 10) < 2);
    blinkSecond = false;
  }
  int lid = 0;
  if (blinkActive) {
    float ph = (now - blinkStart) / 240.0f;
    if (ph >= 1.0f) {
      if (doubleBlink && !blinkSecond) { blinkSecond = true; blinkStart = now; ph = 0; }
      else { blinkActive = false; lid = 0; nextBlinkAt = now + 2000UL + random(0, 4000UL); }
    }
    if (blinkActive) lid = (int)((eh / 2 + 2) * sinf(PI * ph));
  }

  // ---- "жизнь": взгляды и особые действия ----
  if (down) {
    // SERVER УПАЛ: сердитые брови, зрачки быстро мечутся, рот матерится
#if SCENES_ON
    sceneClear();          // тревога важнее сцен и сна
#endif
    brows = 2;
    lowerLid = 4;
    setBothTargets(eyeL.tx, eyeL.ty, 6.0f);
    if (now >= nextLookAt) {
      float dx = (random(0, 201) - 100) / 100.0f * 5.5f;
      float dy = (random(0, 201) - 100) / 100.0f * 5.0f;
      setBothTargets(dx, dy, 6.0f);
      nextLookAt = now + 300UL + random(0, 600UL);
    }
  } else if (panic) {
    // БЭКАП: глаза охвачены паникой - зрачки мелкие, смотрят вверх, трясутся
#if SCENES_ON
    sceneClear();
#endif
    actMode = -1;
    brows = 1;
    lowerLid = 0;
    if (now >= nextLookAt) {
      float dx = (random(0, 141) - 70) / 100.0f;
      float dy = -(5.4f + (random(0, 21) - 10) / 100.0f);
      setBothTargets(dx, dy, 4.2f);
      nextLookAt = now + 140UL + random(0, 160UL);
    }
  } else if (sleep_ && now >= wakeUntil) {   // v3: E|wake ��ন� ����� �����묨
    // НОЧЬ: сонный режим - взгляд дрейфует вниз, редкие моргания
    actMode = -1;
    brows = 0;
    lowerLid = 0;
    if (!blinkActive && nextBlinkAt < now + 5000UL) {
      nextBlinkAt = now + 5000UL + random(0, 4000UL);
    }
    if (now >= nextLookAt) {
      float dx = (random(0, 101) - 50) / 100.0f * 1.5f;
      float dy = (random(0, 101) - 50) / 100.0f * 2.0f + 4.0f;
      setBothTargets(dx, dy, 5.5f);
      nextLookAt = now + 3000UL + random(0, 4000UL);
    }
#if SCENES_ON
    // демо-команда живёт и ночью: E|book запускает сцену поверх сна
    if (sceneName == SC_NONE && forceScene >= 0) sceneStart(forceScene);
    // фазы сна двигаются, только когда сцены нет
    if (sceneName == SC_NONE) {
      if (!sleepActive) { sleepActive = true; sleepReset(); }
      sleepTick(now);
    }
#endif
  } else {
#if SCENES_ON
    if (sceneTick(now)) {
      // сцена играет: кадр рисуется в конце тика, зрачки ведёт sceneTargets
      lowerLid = 0;
      actMode = -1;
    } else
#endif
    {
    lowerLid = 0;
    if (now < exhaleUntil) lowerLid = 6;   // v3: �뤮� ��᫥ �ॢ���/�������� ��᫥ ᭠
    if (actMode >= 0) {
      // идёт особое действие
      if (now >= actUntil) {
        actStep++;
        switch (actMode) {
          case 1: // влево-вправо
            if (actStep == 1) { setBothTargets(-5.5f, 0, PUPIL_R_BASE); actUntil = now + 700UL; }
            else if (actStep == 2) { setBothTargets(5.5f, 0, PUPIL_R_BASE); actUntil = now + 700UL; }
            else { actMode = -1; }
            break;
          case 2: { // круг
            const float wp[4][2] = { {5.5f, 0}, {0, 9}, {-5.5f, 0}, {0, -9} };
            if (actStep < 4) { setBothTargets(wp[actStep][0], wp[actStep][1], PUPIL_R_BASE); actUntil = now + 450UL; }
            else actMode = -1;
            break; }
          default:
            actMode = -1;
        }
        if (actMode == -1) { brows = 0; nextActAt = now + 8000UL + random(0, 12000UL); nextLookAt = now + 1000UL; }
      }
    } else {
      // обычные перемещения взгляда
      if (now >= nextLookAt) {
        float dx = (random(0, 201) - 100) / 100.0f * 5.5f;
        float dy = (random(0, 201) - 100) / 100.0f * 9.0f;
        setBothTargets(dx, dy, PUPIL_R_BASE);
        nextLookAt = now + 700UL + random(0, 2600UL);
      }
      // время особого действия
      if (now >= nextActAt) {
        actMode = random(0, 4);
        actStep = 0;
        switch (actMode) {
          case 0: // "замечает тебя": зрачки к центру и расширяются, брови вверх
            setBothTargets(0, 0, 9.5f);
            brows = 1;
            actUntil = now + 1400UL;
            break;
          case 1: // влево-вправо
            setBothTargets(-5.5f, 0, PUPIL_R_BASE);
            actUntil = now + 700UL;
            break;
          case 2: // круг
            setBothTargets(5.5f, 0, PUPIL_R_BASE);
            actUntil = now + 450UL;
            break;
          case 3: // прищур (осматривается с подозрением)
            lowerLid = 8;
            setBothTargets(2.0f, 3.0f, PUPIL_R_BASE);
            brows = 1;
            actUntil = now + 1600UL;
            break;
        }
      }
    }
    if (actMode == -1) brows = (now < exhaleUntil) ? 1 : 0;
    }
  }

  // ---- плавное движение ----
  // v3: ��㦨�� ����� lerp, ���� ᠪ���� � ��堭��; ���室� �ॢ���/ᮭ -> �뤮�
  v3Phase(0.033f, (down || panic) ? 0.3f : 1.0f);
  if (panicPrev && !panic)  exhaleUntil = now + 1600UL;
  if (sleepPrev && !sleep_) exhaleUntil = now + 1500UL;
  panicPrev = panic; sleepPrev = sleep_;

#if SCENES_ON
  // при выходе из ночи сон начнётся с засыпания заново
  if (!sleep_ && sleepActive) sleepActive = false;

  if (sleep_) {
#if SCENES_ON
    // демо-сцена поверх сна (E|book ночью работает)
    if (sceneName != SC_NONE && sceneStepTick(now)) { sceneFrame(now, lid); return; }
#endif
    sleepFrame(now);
#if SCENES_ON
    if (forceHat) drawSantaHat();   // E|hat и ночью: шапка поверх спящего
#endif
    return;
  }
  // сцена рисует свой кадр (глаза + реквизит)
  if (sceneName != SC_NONE) { sceneFrame(now, lid); return; }
#else
  // сонные глаза: нижнее веко почти до середины (как если спит)
  if (sleep_ && lid < eh / 2 + 3) lid = eh / 2 + 3;
#endif

  eyesDrawFrame(lid, lowerLid, (int)((now / 170UL) % 3UL));
}

// ===================== СТАТУС-ПАНЕЛЬ =====================
// Большие буквы рисуются примитивами (стандартный шрифт не умеет кириллицу):
//  O -> О   K -> К   U -> У   P -> П   A -> А   L -> Л
static void drawGlyph(char c, int x, int y, int w, int h) {
  const uint16_t col = SSD1306_BLACK;
  int t = h / 10; if (t < 2) t = 2;
  int mh = h / 2;
  int wb = w * 2 / 5;

  switch (c) {
    case 'O':
      display.fillRect(x, y, w, t, col);
      display.fillRect(x, y + h - t, w, t, col);
      display.fillRect(x, y, t, h, col);
      display.fillRect(x + w - t, y, t, h, col);
      break;
    case 'K':
      display.fillRect(x, y, t, h, col);
      thickLine(x + w - t, y, x + t, y + mh, t);
      thickLine(x + t, y + mh, x + w - t, y + h - 1, t);
      break;
    case 'U':
      thickLine(x, y, x + w / 2, y + mh, t);
      thickLine(x + w - t, y, x + w / 2, y + mh, t);
      thickLine(x + w / 2, y + mh, x + w / 2 - w / 6, y + h - 1, t);
      break;
    case 'P':
      display.fillRect(x, y, w, t, col);
      display.fillRect(x, y, t, h, col);
      display.fillRect(x + w - t, y, t, h, col);
      break;
    case 'A':
      thickLine(x + w / 2 - t / 2, y, x, y + h - 1, t);
      thickLine(x + w / 2 - t / 2, y, x + w - t, y + h - 1, t);
      display.fillRect(x + w / 4, y + h * 2 / 3, w / 2, t, col);
      break;
    case 'L':
      display.fillRect(x + w - t, y, t, h, col);
      display.fillRect(x, y + h - t, w, t, col);
      thickLine(x + w - wb, y, x, y + h - t, t);
      break;
  }
}

static void drawBigText(const char* word, int y, int h) {
  int n = strlen(word);
  int w  = h * 3 / 5;
  int gap = h / 12 + 2;
  int total = n * w + (n - 1) * gap;
  int x = (SCREEN_WIDTH - total) / 2;
  for (int i = 0; i < n; i++) {
    drawGlyph(word[i], x, y, w, h);
    x += w + gap;
  }
}

static void drawHeader() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(0, 0);
  display.print(F("MONITOR "));
  display.print(TARGET);
  display.drawFastHLine(0, 9, SCREEN_WIDTH, SSD1306_BLACK);
}

static void drawFooter(const char* left, const char* right) {
  display.drawFastHLine(0, 51, SCREEN_WIDTH, SSD1306_BLACK);
  display.setTextSize(1);
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(0, 55);
  display.print(left);
  int rw = strlen(right) * 6;
  display.setCursor(SCREEN_WIDTH - rw, 55);
  display.print(right);
}

void showState() {
  char lbuf[24], rbuf[24];
  unsigned long upMin = (millis() - bootAt) / 60000UL;

  if (!oledOk) return;

  display.fillScreen(SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  drawHeader();

  switch (state) {
    case ST_OK: {
      // большие буквы ОК (примитивы - стандартный шрифт не умеет кириллицу)
      drawBigText("OK", 14, 34);
      snprintf(lbuf, sizeof(lbuf), "ping:%dms", lastRtt);
      snprintf(rbuf, sizeof(rbuf), "up:%lum", upMin);
      drawFooter(lbuf, rbuf);
      break;
    }
    case ST_DOWN: {
      drawBigText("UPAL", 14, 34); // "УПАЛ"
      snprintf(rbuf, sizeof(rbuf), "up:%lum", upMin);
      drawFooter("no reply", rbuf);
      break;
    }
    case ST_NOWIFI: {
      const char* msg = "NO WIFI";
      int mw = (int)strlen(msg) * 18;
      display.setTextSize(3);
      display.setCursor((SCREEN_WIDTH - mw) / 2, 24);
      display.print(msg);
      break;
    }
  }
  display.display();
}

// ===================== ЛОГИКА =====================
void setStatus(State s) {
  if (s == state) return;
  state = s;
  if (state == ST_DOWN) {
    alertReset(); // включаем "противный" режим мигания
  } else {
    ledWrite(false);
    alertReset();
  }
  if (dispMode == DM_SLIDES) drawSlide(slideIdx);
  Serial.print(F("[STATE] "));
  if (state == ST_OK)        Serial.println(F("OK"));
  else if (state == ST_DOWN) Serial.println(F("DOWN (server not responding)"));
  else                       Serial.println(F("NO WIFI"));
}

void doPing() {
  lastPingAt = millis();

  if (WiFi.status() != WL_CONNECTED) {
    setStatus(ST_NOWIFI);
    return;
  }

  if (!targetIpValid) {
    Serial.println(F("[PING] bad target IP"));
    return;
  }

  bool ok = Ping.ping(targetIP, 1);
  if (ok) {
    lastRtt = Ping.averageTime();
    Serial.print(F("[PING] "));
    Serial.print(TARGET);
    Serial.print(F(" -> OK, "));
    Serial.print(lastRtt);
    Serial.println(F(" ms"));
    setStatus(ST_OK);
  } else {
    lastRtt = -1;
    Serial.print(F("[PING] "));
    Serial.print(TARGET);
    Serial.println(F(" -> FAIL"));
    setStatus(ST_DOWN);
  }
}

void monitorSetup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("[BOOT] Server Monitor starting"));

  randomSeed(RANDOM_REG32); // аппаратный генератор случайных чисел

  pinMode(PIN_LED, OUTPUT);
  analogWriteRange(255); // диапазон ШИМ 0..255 для плавного дыхания
  ledWrite(false);

  // пищалка (пассивный зуммер от ПК): GND + D3 (GPIO0)
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  Wire.begin(PIN_SDA, PIN_SCL); // SDA=D2(GPIO4), SCL=D1(GPIO5)
  Wire.setClock(400000);        // 400 кГц - быстрые кадры

  // --- диагностика: скан I2C-шины ---
  Serial.println(F("[I2C] scanning bus..."));
  int i2cFound = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C] device found at 0x%02X\n", a);
      i2cFound++;
    }
  }
  if (i2cFound == 0) Serial.println(F("[I2C] NO devices found (check wiring SCL=D1, SDA=D2)"));

  oledOk = false;
  if (i2cFound > 0 && display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledOk = true;
    Serial.println(F("[OLED] SSD1306 ready at 0x3C"));
  } else if (i2cFound > 0 && display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    oledOk = true;
    Serial.println(F("[OLED] SSD1306 ready at 0x3D"));
  } else {
    Serial.println(F("[OLED] display not usable, continuing without it"));
  }
  if (oledOk) {
    display.clearDisplay();
    display.display();
  }

  targetIpValid = targetIP.fromString(TARGET);
  if (!targetIpValid) {
    Serial.print(F("[BOOT] invalid target IP: "));
    Serial.println(TARGET);
  }

  // --- подключение к Wi-Fi (с таймаутом 25 сек) ---
  Serial.print(F("[WIFI] connecting to \""));
  Serial.print(WIFI_SSID);
  Serial.print(F("\""));
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 25000UL) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  bootAt = millis();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[WIFI] connected, IP="));
    Serial.print(WiFi.localIP());
    Serial.print(F(", RSSI="));
    Serial.print(WiFi.RSSI());
    Serial.println(F(" dBm"));
  } else {
    Serial.println(F("[WIFI] not connected yet, will retry"));
  }

  // при старте: панель статуса сразу, глаза, через 12 сек - первое слайд-шоу
  dispMode = DM_EYES;

  showState();
  doPing(); // первый пинг сразу при старте
  statsFetch(); // сразу забрать данные: время (ночь/день) + флаг бэкапа
  showState();
  nextInfoAt = millis() + INFO_FIRST_MS;

#if SCENES_ON
  nextSceneAt = millis() + 30000UL;   // первая сцена "живого дня" ~через 30 сек
  Serial.println(F("[SCENE] demo via USB: send line E|<name>"));
  Serial.println(F("[SCENE] names: peek | book | tiktok | pc | self | coffee | hat | none"));
#endif

  // короткий "я жив" бип - и проверка пищалки
  beepStart(BM_BOOT, PATTERN_BOOT, sizeof(PATTERN_BOOT) / sizeof(BeepStep), false);
}

void monitorLoop() {
  unsigned long now = millis();

#if SCENES_ON
  serialCmdTick();   // демо/серверные команды "E|<token>" по USB (без ресета: DtrEnable=false)
#endif

  // ---- ЭКРАН: инверсия (ночью - тёмный экран; при аварии - мигание) ----
  bool wantInvert;
  if (state != ST_OK) {
    if (now - downFlashAt >= 550UL) {
      downFlashAt = now;
      downInvert = !downInvert;
    }
    wantInvert = !downInvert;
  } else {
    downInvert = false;
    wantInvert = nightMode;
  }
  if (oledOk && wantInvert != screenInvert) {
    screenInvert = wantInvert;
    display.invertDisplay(wantInvert);
  }

  // ---- СВЕТОДИОД ----
  if (state == ST_OK) {
    // плавное расслабленное дыхание, как у спящего; ночью - почти не светит
    // (пока пищит пищалка, LED не трогаем: tone() и analogWrite() делят таймер 1)
    if (!beepPat) {
      unsigned long bt = now % BREATH_MS;
      float b = 0.5f - 0.5f * cosf(2.0f * PI * (float)bt / (float)BREATH_MS);
      if (nightMode) b *= 0.10f;
      analogWrite(PIN_LED, 255 - (int)(b * 250.0f + 0.5f)); // active-LOW: чем тише duty, тем ярче
    }
  } else {
    // авария: LED - аварийная "мигалка" на полную яркость даже ночью
    alertTick();
  }

  // восстановление Wi-Fi
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiTry >= WIFI_RETRY_MS) {
      lastWifiTry = now;
      Serial.println(F("[WIFI] reconnect attempt"));
      WiFi.reconnect();
    }
    if (state != ST_NOWIFI) setStatus(ST_NOWIFI);
  }

  // пинг раз в минуту + свежие данные для слайдов и тревог
  if (WiFi.status() == WL_CONNECTED && now - lastPingAt >= PING_INTERVAL) {
    doPing();
    if (state == ST_OK) statsFetch(); // гипервизор жив - обновляем VM/CPU (каждую минуту)
    if (dispMode == DM_SLIDES) drawSlide(slideIdx);
  }

  // ---- ЗВУКОВЫЕ ТРЕВОГИ (пищалка D3) ----
  bool hypAlarm = (state != ST_OK);
  int vmsDownCount = 0;
  if (g_srv.valid) {
    for (int i = 0; i < g_srv.nVms; i++) if (!g_srv.vms[i].up) vmsDownCount++;
  }
  bool vmAlarm = !hypAlarm && g_srv.valid && vmsDownCount > 0;
  if (hypAlarm) {
    beepStart(BM_SIREN, PATTERN_SIREN, sizeof(PATTERN_SIREN) / sizeof(BeepStep), true);
  } else if (vmAlarm && !nightMode) {
    beepStart(BM_VM, PATTERN_VM, sizeof(PATTERN_VM) / sizeof(BeepStep), true);
  } else if (beepPat && (beepPat == PATTERN_SIREN || beepPat == PATTERN_VM)) {
    // тревога кончилась - стоп + радостный "всё хорошо" (не ночью)
    beepStopNow(false);
    if (!nightMode) beepStart(BM_RECOVER, PATTERN_RECOVER, sizeof(PATTERN_RECOVER) / sizeof(BeepStep), false);
    Serial.print(F("[BEEP] stop: "));
    Serial.println(F("recovered"));
  }
  beepTick();

  // экран: глаза <-> слайд-шоу (каждые 2-3 минуты)
  if (dispMode == DM_EYES) {
    // слайды прячутся только при падении гипервизора; при VM-тревоге слайды показывают, кто именно упал
    if (state == ST_OK && now >= nextInfoAt) {
      dispMode = DM_SLIDES;
      slideIdx = 0;
      slideCount = g_srv.valid ? 4 : 2;   // данные уже свежие (обновляются каждую минуту)
      drawSlide(0);
      slideUntil = now + SLIDE_MS;
      Serial.println(F("[DISP] slides"));
    } else {
      eyesTick();
    }
  } else if (dispMode == DM_SLIDES) {
    if (now >= slideUntil) {
      slideIdx++;
      if (slideIdx >= slideCount) {
        dispMode = DM_EYES;
        nextInfoAt = now + INFO_PERIOD_MIN + random(0, INFO_PERIOD_VAR);
        lastFrame = 0;
        Serial.println(F("[DISP] eyes mode"));
      } else {
        drawSlide(slideIdx);
        slideUntil = now + SLIDE_MS;
      }
    }
  }
}

// Стандартные точки входа Arduino
void setup() { monitorSetup(); }
void loop()  { monitorLoop(); }
