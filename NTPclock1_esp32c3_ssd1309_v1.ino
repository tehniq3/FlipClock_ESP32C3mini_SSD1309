// =============================================================================
// FLIP NTP Clock — ESP32-C3 + 2.42" OLED SSD1309 (SPI)
// original: https://www.eelectronicparts.com/blogs/news/diy-esp32-s3-mini-flip-clock-animated-ntp-time-on-a-0-96-oled-display
// =============================================================================

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h> // Folosim SSD1306, dar merge perfect pe SSD1309
#include <Fonts/FreeSansBold24pt7b.h>
#include <WiFi.h>
#include <time.h>

// ================== USER SETTINGS ==================
const char* WIFI_SSID = "bbk2"; 
const char* WIFI_PASS = "internet2"; 

#define TZ_EASTERN   "EST5EDT,M3.2.0,M11.1.0"
#define TZ_CENTRAL   "CST6CDT,M3.2.0,M11.1.0"
#define TZ_MOUNTAIN  "MST7MDT,M3.2.0,M11.1.0"
#define TZ_PACIFIC   "PST8PDT,M3.2.0,M11.1.0" 
#define TZ_ROMANIA   "EET-2EEST,M3.5.0/3,M10.5.0/4"
const char* TIMEZONE   = TZ_ROMANIA;
const char* NTP_SERVER = "pool.ntp.org";

#define USE_12_HOUR        true
const unsigned long ANIM_MS = 350;
// ===================================================

#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64

// ================== PINI SPI (ESP32-C3) ==================
#define SPI_CLK  4
#define SPI_MOSI 6
#define SPI_MISO -1 
#define SPI_CS   7

#define OLED_DC   3
#define OLED_RST  10
// ========================================================

// Inițializare obiect SPI
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_DC, OLED_RST, SPI_CS);

// =================================================================
//                             LAYOUT
// =================================================================
#define CARD_W    28
#define CARD_H    46
#define CARD_Y     0       
#define HALF_H    (CARD_H / 2)       // 23
#define MID_Y     (CARD_Y + HALF_H)  // 23 

const int16_t CARD_X[4] = { 2, 32, 68, 98 };

#define COLON_X        62
#define COLON_TOP_Y    11  
#define COLON_BOT_Y    35  
#define COLON_SIZE      3

// =================================================================
//                  DIGIT GLYPH PRE-RENDERING
// =================================================================
static GFXcanvas1* digitCanvas[10] = { nullptr };

void renderDigitCanvases() {
  for (int d = 0; d < 10; d++) {
    GFXcanvas1* c = new GFXcanvas1(CARD_W, CARD_H);
    c->fillScreen(0);
    c->setFont(&FreeSansBold24pt7b);
    c->setTextColor(1);
    char s[2] = { (char)('0' + d), 0 };
    int16_t x1, y1; uint16_t w, h;
    c->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    int16_t cx = (CARD_W - (int16_t)w) / 2 - x1;
    int16_t cy = CARD_H / 2 - y1 - (int16_t)h / 2;
    c->setCursor(cx, cy);
    c->print(s);
    digitCanvas[d] = c;
  }
}

// =================================================================
//                   HALF-DIGIT FLAP RENDERING
// =================================================================
static inline void drawTopHalf(int16_t cardX, int16_t cardY, int digit, int drawnH) {
  if (drawnH <= 0 || digit < 0 || digit > 9) return;
  GFXcanvas1* c = digitCanvas[digit];
  const int16_t topY = cardY + HALF_H - drawnH;
  for (int16_t ty = 0; ty < drawnH; ty++) {
    int16_t sy0 = (int32_t)ty       * HALF_H / drawnH;
    int16_t sy1 = (int32_t)(ty + 1) * HALF_H / drawnH;
    if (sy1 <= sy0)     sy1 = sy0 + 1;
    if (sy1 > HALF_H)   sy1 = HALF_H;
    int16_t screenY = topY + ty;
    for (int16_t sx = 0; sx < CARD_W; sx++) {
      bool lit = false;
      for (int16_t sy = sy0; sy < sy1; sy++) {
        if (c->getPixel(sx, sy)) { lit = true; break; }
      }
      if (lit) display.drawPixel(cardX + sx, screenY, SSD1306_WHITE);
    }
  }
}

static inline void drawBottomHalf(int16_t cardX, int16_t cardY, int digit, int drawnH) {
  if (drawnH <= 0 || digit < 0 || digit > 9) return;
  GFXcanvas1* c = digitCanvas[digit];
  const int16_t botY0 = cardY + HALF_H;
  for (int16_t ty = 0; ty < drawnH; ty++) {
    int16_t sy0 = HALF_H + (int32_t)ty       * HALF_H / drawnH;
    int16_t sy1 = HALF_H + (int32_t)(ty + 1) * HALF_H / drawnH;
    if (sy1 <= sy0)     sy1 = sy0 + 1;
    if (sy1 > CARD_H)   sy1 = CARD_H;
    int16_t screenY = botY0 + ty;
    for (int16_t sx = 0; sx < CARD_W; sx++) {
      bool lit = false;
      for (int16_t sy = sy0; sy < sy1; sy++) {
        if (c->getPixel(sx, sy)) { lit = true; break; }
      }
      if (lit) display.drawPixel(cardX + sx, screenY, SSD1306_WHITE);
    }
  }
}

// =================================================================
//                       ANIMATION STATE
// =================================================================
struct DigitSlot {
  int           currentDigit;
  int           previousDigit;
  unsigned long animStart;
};
DigitSlot slots[4];
bool slotsInitialized = false;

void getTargetDigits(const struct tm& t, int out[4], bool& outIsPM) {
  int h = t.tm_hour;
  outIsPM = (h >= 12);
#if USE_12_HOUR
  h = h % 12; if (h == 0) h = 12;
#endif
  int hT = h / 10;
  out[0] = (hT == 0) ? -1 : hT;
  out[1] = h % 10;
  out[2] = t.tm_min / 10;
  out[3] = t.tm_min % 10;
}

void updateSlots(const int target[4], unsigned long now) {
  if (!slotsInitialized) {
    for (int i = 0; i < 4; i++) {
      slots[i].previousDigit = target[i];
      slots[i].currentDigit  = target[i];
      slots[i].animStart     = 0;
    }
    slotsInitialized = true;
    return;
  }
  for (int i = 0; i < 4; i++) {
    if (target[i] != slots[i].currentDigit) {
      slots[i].previousDigit = slots[i].currentDigit;
      slots[i].currentDigit  = target[i];
      slots[i].animStart     = now;
    } else if (slots[i].animStart != 0 &&
               (now - slots[i].animStart) >= ANIM_MS) {
      slots[i].animStart     = 0;
      slots[i].previousDigit = slots[i].currentDigit;
    }
  }
}

// =================================================================
//                         CARD RENDERING
// =================================================================
void drawCard(int idx, unsigned long now) {
  int16_t xc = CARD_X[idx];
  int16_t yc = CARD_Y;

  display.drawRoundRect(xc, yc, CARD_W, CARD_H, 3, SSD1306_WHITE);

  int  oldD   = slots[idx].previousDigit;
  int  newD   = slots[idx].currentDigit;
  unsigned long aStart = slots[idx].animStart;

  bool animating = (aStart != 0) && (oldD != newD);
  float t = 0.0f;
  if (animating) {
    unsigned long elapsed = now - aStart;
    if (elapsed >= ANIM_MS) animating = false;
    else t = (float)elapsed / (float)ANIM_MS;
  }

  if (!animating) {
    drawTopHalf(xc, yc, newD, HALF_H);
    drawBottomHalf(xc, yc, newD, HALF_H);
  } else if (t < 0.5f) {
    drawTopHalf(xc, yc, newD, HALF_H);
    drawBottomHalf(xc, yc, oldD, HALF_H);
    int drawnH = (int)(HALF_H * (1.0f - 2.0f * t));
    if (drawnH > 0) {
      display.fillRect(xc + 1, yc + HALF_H - drawnH, CARD_W - 2, drawnH, SSD1306_BLACK);
      drawTopHalf(xc, yc, oldD, drawnH);
    }
  } else {
    drawTopHalf(xc, yc, newD, HALF_H);
    drawBottomHalf(xc, yc, oldD, HALF_H);
    int drawnH = (int)(HALF_H * (2.0f * t - 1.0f));
    if (drawnH > 0) {
      display.fillRect(xc + 1, yc + HALF_H, CARD_W - 2, drawnH, SSD1306_BLACK);
      drawBottomHalf(xc, yc, newD, drawnH);
    }
  }

  display.drawFastHLine(xc + 1, yc + HALF_H, CARD_W - 2, SSD1306_BLACK);
}

// =================================================================
//                       CHROME (colon, status)
// =================================================================
void drawColon() {
  display.fillRect(COLON_X, COLON_TOP_Y, COLON_SIZE, COLON_SIZE, SSD1306_WHITE);
  display.fillRect(COLON_X, COLON_BOT_Y, COLON_SIZE, COLON_SIZE, SSD1306_WHITE);
}

void drawBottomStatus(const struct tm& t) {
  static const char* const days[]   = { "Sunday","Monday","Tuesday","Wednesday",
                                        "Thursday","Friday","Saturday" };
  static const char* const months[] = { "Jan","Feb","Mar","Apr","May","Jun",
                                        "Jul","Aug","Sep","Oct","Nov","Dec" };
  char buf[40];
  snprintf(buf, sizeof(buf), "%s %s %d %d",
           days[t.tm_wday], months[t.tm_mon], t.tm_mday, t.tm_year + 1900);

  display.setFont(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  int16_t x = (SCREEN_WIDTH - (int16_t)w) / 2;
  if (x < 0) x = 0;
  
  display.setCursor(x, 56); 
  display.print(buf);
}

static int wifiBarsFromRssi(int rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

void drawWifiIcon(int16_t xLeft, int16_t baseY) {
  int bars = 0;
  if (WiFi.status() == WL_CONNECTED) bars = wifiBarsFromRssi(WiFi.RSSI());

  const int heights[] = { 2, 4, 5, 7 };
  for (int i = 0; i < 4; i++) {
    int16_t x = xLeft + i * 2;
    if (i < bars) {
      display.drawFastVLine(x, baseY - heights[i] + 1, heights[i], SSD1306_WHITE);
    } else {
      display.drawPixel(x, baseY, SSD1306_WHITE);
    }
  }
}

static const char* greetingFor(int hour) {
  if (hour >=  5 && hour < 12) return "Morning";
  if (hour >= 12 && hour < 17) return "Afternoon";
  if (hour >= 17 && hour < 22) return "Evening";
  return "Night";
}

void drawMiddleStatus(const struct tm& t, bool isPM) {
  const int16_t ROW_Y = 48;       
  const int16_t BAR_BASE_Y = 54;  

  display.setFont(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);

  char tzBuf[8];
  strftime(tzBuf, sizeof(tzBuf), "%Z", &t);
  const char* tz = (tzBuf[0]) ? tzBuf : "---";
  display.setCursor(2, ROW_Y);
  display.print(tz);
  int16_t tzW = (int16_t)strlen(tz) * 6;
  int16_t wifiX = 2 + tzW + 3;

  drawWifiIcon(wifiX, BAR_BASE_Y);
  int16_t wifiEndX = wifiX + 6;

  char ampmSec[16];
  snprintf(ampmSec, sizeof(ampmSec), "%s:%02d",
           isPM ? "PM" : "AM", t.tm_sec);
  int16_t ampmSecW = (int16_t)strlen(ampmSec) * 6;
  int16_t ampmX    = SCREEN_WIDTH - ampmSecW - 1;
  display.setCursor(ampmX, ROW_Y);
  display.print(ampmSec);

  const char* g = greetingFor(t.tm_hour);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(g, 0, 0, &x1, &y1, &w, &h);
  int16_t gX   = (SCREEN_WIDTH - (int16_t)w) / 2;
  int16_t gapL = wifiEndX + 3;
  int16_t gapR = ampmX - 3;
  if (gX + (int16_t)w > gapR) gX = gapR - (int16_t)w;
  if (gX < gapL) gX = gapL;
  display.setCursor(gX, ROW_Y);
  display.print(g);
}

// =================================================================
//                          MAIN FRAME
// =================================================================
void drawFrame(unsigned long now) {
  struct tm t;
  bool haveTime = getLocalTime(&t, 5);

  display.clearDisplay();

  if (!haveTime) {
    display.setFont(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
    display.setCursor(12, 28);
    display.print("Waiting for NTP...");
    display.display();
    return;
  }

  int  target[4];
  bool isPM;
  getTargetDigits(t, target, isPM);
  updateSlots(target, now);

  for (int i = 0; i < 4; i++) drawCard(i, now);
  drawColon();
  drawMiddleStatus(t, isPM);        
  drawBottomStatus(t);        

  display.display();
}

// =================================================================
//                       STARTUP MESSAGES
// =================================================================
void showMessage(const String& l1, const String& l2 = "") {
  display.clearDisplay();
  display.setFont();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 14); display.println(l1);
  if (l2.length()) { display.setCursor(0, 30); display.println(l2); }
  display.display();
}

// =================================================================
//                        SETUP / LOOP
// =================================================================
void setup() {
  Serial.begin(115200); delay(200);

  // 1. Inițializăm SPI cu pinii tăi personalizați
  SPI.begin(SPI_CLK, SPI_MISO, SPI_MOSI, SPI_CS);

  // 2. Pornim ecranul
  if (!display.begin(SSD1306_SWITCHCAPVCC)) {
    Serial.println("SSD1309 (SPI) not found");
    while (true) delay(1000);
  }
  display.setTextWrap(false);
  display.setRotation(2); // Rotirea pentru ecran galben/albastru

  showMessage("FLIP NTP Clock", "Rendering glyphs...");
  renderDigitCanvases();

  showMessage("Connecting WiFi", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setTxPower(WIFI_POWER_8_5dBm); 
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    showMessage("WiFi FAILED", "Check credentials");
    while (true) delay(1000);
  }
  Serial.print("\nWiFi OK, IP: "); Serial.println(WiFi.localIP());
  showMessage("WiFi connected", WiFi.localIP().toString());
  delay(400);

  configTzTime(TIMEZONE, NTP_SERVER);
  showMessage("Syncing NTP...", NTP_SERVER);
  struct tm t;
  unsigned long ntpStart = millis();
  while (!getLocalTime(&t, 100) && millis() - ntpStart < 10000) {
    delay(200);
  }

  showMessage("Time synced", "Enjoy the flip!");
  delay(500);
}

void loop() {
  static unsigned long lastWifiCheck = 0;
  static unsigned long lastFrame     = 0;
  unsigned long now = millis();

  if (now - lastWifiCheck > 30000) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
  }

  const unsigned long FRAME_MS = 30;
  if (now - lastFrame >= FRAME_MS) {
    lastFrame = now;
    drawFrame(now);
  }
}
