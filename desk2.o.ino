#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <Preferences.h>
#include "time.h"

// ===== Hardware Pin Configuration for Waveshare ESP32-S3-Zero =====
// Having GPIO 10, 11, 12, 13, 14 sequentially in a row makes wiring extremely clean.
constexpr int SPI_SCLK_PIN = 12; // GP12 (SCK / SCL)
constexpr int SPI_MOSI_PIN = 11; // GP11 (MOSI / SDA)
constexpr int SPI_MISO_PIN = -1; // Not used
constexpr int SPI_CS_PIN   = 10; // GP10 (CS)
constexpr int SPI_DC_PIN   = 14; // GP14 (DC / A0)
constexpr int SPI_RST_PIN  = 13; // GP13 (RES / RST)
constexpr int TOUCH_PIN    = 1;  // GP1 (Touch sensor digital input)

// ===== Display configuration for ESP32-S3 Zero + ST7789 240x240 =====
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI      _bus;

public:
  LGFX() {
    { // SPI bus config
      auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;     // FSPI on ESP32S3
      cfg.spi_mode   = 3;
      cfg.freq_write = 40000000;      // 40 MHz write
      cfg.spi_3wire  = false;
      cfg.use_lock   = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;

      cfg.pin_sclk = SPI_SCLK_PIN;
      cfg.pin_mosi = SPI_MOSI_PIN;
      cfg.pin_miso = SPI_MISO_PIN;
      cfg.pin_dc   = SPI_DC_PIN;

      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    { // Panel config
      auto cfg = _panel.config();
      cfg.pin_cs   = SPI_CS_PIN;
      cfg.pin_rst  = SPI_RST_PIN;
      cfg.pin_busy = -1;

      cfg.memory_width  = 240;
      cfg.memory_height = 240;
      cfg.panel_width   = 240;
      cfg.panel_height  = 240;
      cfg.offset_x      = 0;
      cfg.offset_y      = 0;
      cfg.offset_rotation = 0;
      cfg.readable      = false;
      cfg.invert        = true;       // many ST7789 modules need invert
      cfg.rgb_order     = false;

      _panel.config(cfg);
    }

    setPanel(&_panel);
  }
};

LGFX lcd;

// ===== Eye layout (pair near bottom of 240x240 display) =====
const int16_t CX_LEFT  = 70;
const int16_t CX_RIGHT = 170;
const int16_t CY       = 110;

// Sprite dimensions for rendering
const int16_t EYE_SPRITE_W = 100;
const int16_t EYE_SPRITE_H = 100;

const int16_t PUPIL_SIZE = 36;
const int16_t SCLERA_PAD = 6;

const uint16_t BLACK  = 0x0000;
const uint16_t WHITE  = 0xFFFF;

// Eye colour palette (double-tap cycles through these); also used for UI text
#define NUM_EYE_COLOURS 9
const uint16_t EYE_COLOURS[NUM_EYE_COLOURS] = {
  0xF81F,  // violet
  0x07FF,  // cyan
  0xFFE0,  // yellow
  0x001F,  // blue
  0x07E0,  // green
  0xF800,  // red
  0xFD20,  // orange
  0xFE19,  // pink
  0xFFFF   // white
};
int currentEyeColourIndex = 0;

// ===== Mood & Physics Animation Engine =====
#define MOOD_NORMAL    0
#define MOOD_HAPPY     1
#define MOOD_SURPRISED 2
#define MOOD_SLEEPY    3
#define MOOD_ANGRY     4
#define MOOD_SAD       5
#define MOOD_EXCITED   6
#define MOOD_LOVE      7
#define MOOD_SUSPICIOUS 8
#define MOOD_HEART     9
int currentMood = MOOD_NORMAL;

struct Eye {
  float x, y;
  float w, h;
  float targetX, targetY, targetW, targetH;

  float pupilX, pupilY;
  float targetPupilX, targetPupilY;

  float velX, velY, velW, velH;
  float pVelX, pVelY;
  float k = 0.12f;
  float d = 0.60f;
  float pk = 0.08f;
  float pd = 0.50f;

  bool blinking;
  unsigned long lastBlink;
  unsigned long nextBlinkTime;

  void init(float _x, float _y, float _w, float _h) {
    x = targetX = _x;
    y = targetY = _y;
    w = targetW = _w;
    h = targetH = _h;
    pupilX = targetPupilX = 0;
    pupilY = targetPupilY = 0;
    nextBlinkTime = millis() + random(1000, 4000);
  }

  void update() {
    float ax = (targetX - x) * k;
    float ay = (targetY - y) * k;
    float aw = (targetW - w) * k;
    float ah = (targetH - h) * k;

    velX = (velX + ax) * d;
    velY = (velY + ay) * d;
    velW = (velW + aw) * d;
    velH = (velH + ah) * d;

    x += velX;
    y += velY;
    w += velW;
    h += velH;

    float pax = (targetPupilX - pupilX) * pk;
    float pay = (targetPupilY - pupilY) * pk;
    pVelX = (pVelX + pax) * pd;
    pVelY = (pVelY + pay) * pd;
    pupilX += pVelX;
    pupilY += pVelY;
  }
};

Eye leftEye, rightEye;

// ===== Config (WiFi, OpenWeather, timezone) via web portal =====
#define PREF_NAMESPACE   "cb"
#define CONFIG_AP_SSID   "DeskBuddy-Setup"
#define CONFIG_AP_PASS   "12345678"
#define MAX_SSID_LEN     32
#define MAX_PASS_LEN     64
#define MAX_CITY_LEN     64
#define MAX_COUNTRY_LEN  8
#define MAX_APIKEY_LEN   64

String wifi_ssid;
String wifi_pass;
String ow_city;
String ow_country;
String ow_api_key;
long   gmtOffsetSec = 19800;  // default IST (5.5h), can be changed in portal

Preferences prefs;
WebServer server(80);
DNSServer dnsServer;
bool configMode = false;       // true = running AP + config portal

const char* NTP_SERVER = "pool.ntp.org";
const int   DAYLIGHT_OFFSET_SEC = 0;

// Weather data
float currentTemp   = 0.0f;
int   currentHum    = 0;
String weatherMain  = "Loading";
String weatherDesc  = "Wait...";

// Simple 3-point forecast (temp + label only)
struct SimpleForecast {
  String label;
  int    temp;
};
SimpleForecast forecast[3];

unsigned long lastWeatherUpdate = 0;
const unsigned long WEATHER_INTERVAL_MS = 10UL * 60UL * 1000UL; // 10 minutes

// Page state
// 0 = eyes only, 1 = clock, 2 = weather, 3 = forecast
int currentPage  = 0;
int lastPage     = -1;

// Touch debounce & gestures
bool roundEyeMode       = false;  // triple-tap on eyes page toggles round eyes

// Info-page redraw throttle
unsigned long lastInfoRedrawMs = 0;
int lastClockMin = -1;
unsigned long lastWeatherPageDrawMs = 0;
unsigned long lastForecastPageDrawMs = 0;

// Single sprite reused for both eyes
lgfx::LGFX_Sprite eyeSprite(&lcd);

// ===== Config load/save (Preferences) =====
void loadConfig() {
  prefs.begin(PREF_NAMESPACE, true);
  wifi_ssid    = prefs.getString("wifi_ssid", "");
  wifi_pass    = prefs.getString("wifi_pass", "");
  ow_city      = prefs.getString("ow_city", "IDUKKI");
  ow_country   = prefs.getString("ow_country", "IN");
  ow_api_key   = prefs.getString("ow_apikey", "");
  gmtOffsetSec = prefs.getLong("gmt_offset", 19800);
  currentEyeColourIndex = prefs.getUChar("eye_colour", 0);
  if (currentEyeColourIndex >= NUM_EYE_COLOURS) currentEyeColourIndex = 0;
  prefs.end();

  if (ow_city.length() == 0) ow_city = "IDUKKI";
  if (ow_country.length() == 0) ow_country = "IN";
}

void saveConfig() {
  prefs.begin(PREF_NAMESPACE, false);
  prefs.putString("wifi_ssid", wifi_ssid);
  prefs.putString("wifi_pass", wifi_pass);
  prefs.putString("ow_city", ow_city);
  prefs.putString("ow_country", ow_country);
  prefs.putString("ow_apikey", ow_api_key);
  prefs.putLong("gmt_offset", gmtOffsetSec);
  prefs.putUChar("eye_colour", (uint8_t)currentEyeColourIndex);
  prefs.end();
}

void saveEyeColour() {
  prefs.begin(PREF_NAMESPACE, false);
  prefs.putUChar("eye_colour", (uint8_t)currentEyeColourIndex);
  prefs.end();
}

// ===== Web config: HTML form and handlers =====
static const char CONFIG_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>DESKBUDDY 2.0</title>
<style>
body{font-family:sans-serif;max-width:360px;margin:20px auto;padding:16px;
background:#000000;color:#87CEEB;min-height:100vh;box-sizing:border-box;text-align:center;}
h1{color:#87CEEB;font-size:1.3em;}
.wrap{text-align:left;}
label{display:block;margin:10px 0 4px;color:#87CEEB;}
input{width:100%;box-sizing:border-box;padding:10px;border:1px solid #87CEEB;
border-radius:6px;background:#000000;color:#87CEEB;}
button{background:#000000;color:#87CEEB;border:1px solid #87CEEB;padding:14px 20px;margin-top:20px;
cursor:pointer;font-weight:bold;border-radius:6px;}
button:hover{background:#111111;}
.msg{color:#87CEEB;margin-top:12px;}
.sec{margin-top:18px;font-weight:bold;color:#87CEEB;}
.hint{font-size:0.8em;color:#87CEEB;}
</style></head><body>
<h1>DESKBUDDY 2.0</h1>
<div class="wrap">
<form method="POST" action="/save">
<div class="sec">WiFi</div>
<label>WiFi SSID</label><input name="wifi_ssid" value="%s" maxlength="32" placeholder="Network name">
<label>WiFi Password</label><input type="password" name="wifi_pass" value="%s" maxlength="64" placeholder="Leave blank to keep current">

<div class="sec">Weather (OpenWeather)</div>
<label>API Key <span class="hint">(required for weather)</span></label><input name="ow_apikey" value="%s" maxlength="64" placeholder="Paste key from openweathermap.org">
<label>City</label><input name="ow_city" value="%s" maxlength="64" placeholder="e.g. London">
<label>Country code</label><input name="ow_country" value="%s" maxlength="4" placeholder="e.g. IN">

<div class="sec">Time &amp; Clock</div>
<label>Timezone offset (hours)</label>
<input name="tz" value="%s" maxlength="6" placeholder="e.g. 5.5 for IST, 1 for CET, -5 for EST">

<button type="submit">Save &amp; Reboot Device</button>
</form>
</div>
<p class="msg">%s</p></body></html>
)rawliteral";

void serveConfigForm(bool isPostOk) {
  String msg = isPostOk ? "Saved. Rebooting..." : "";
  char html[2200];
  String tzStr = String(gmtOffsetSec / 3600.0f, 1);  // e.g. "5.5"
  // Don't pre-fill password in form (security); leave blank = keep current when saving
  snprintf(html, sizeof(html), CONFIG_HTML,
           wifi_ssid.c_str(), "",
           ow_api_key.c_str(),
           ow_city.c_str(), ow_country.c_str(),
           tzStr.c_str(),
           msg.c_str());
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  wifi_ssid = server.arg("wifi_ssid").substring(0, MAX_SSID_LEN);
  String newPass = server.arg("wifi_pass");
  if (newPass.length() > 0) {
    wifi_pass = newPass.substring(0, MAX_PASS_LEN);
  }
  ow_api_key = server.arg("ow_apikey").substring(0, MAX_APIKEY_LEN);
  ow_city    = server.arg("ow_city").substring(0, MAX_CITY_LEN);
  ow_country = server.arg("ow_country").substring(0, MAX_COUNTRY_LEN);

  String tzIn = server.arg("tz");
  float tzHours = tzIn.toFloat();  // can be negative or fractional
  gmtOffsetSec = (long)(tzHours * 3600.0f);

  if (ow_city.length() == 0) ow_city = "IDUKKI";
  if (ow_country.length() == 0) ow_country = "IN";

  saveConfig();
  serveConfigForm(true);
  delay(500);
  ESP.restart();
}

void handleConfigGet() {
  serveConfigForm(false);
}

void startConfigAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(CONFIG_AP_SSID, CONFIG_AP_PASS);
  IPAddress apIp = WiFi.softAPIP();
  dnsServer.start(53, "*", apIp);
  server.on("/", HTTP_GET, handleConfigGet);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", "");
  });
  server.begin();
  configMode = true;
}

// ===== Eye drawing & masking =====
void drawEyelidMask(float x, float y, float w, float h, int mood, bool isLeft) {
  if (mood == MOOD_HEART) return; // Full heart symbol visible

  int ix = (int)x;
  int iy = (int)y;
  int iw = (int)w;
  int ih = (int)h;

  if (mood == MOOD_ANGRY) {
    if (isLeft) {
      for (int i = 0; i < 30; i++) {
        eyeSprite.drawLine(ix - 5, iy + i - 15, ix + iw + 5, iy + i - 30, BLACK);
      }
    } else {
      for (int i = 0; i < 30; i++) {
        eyeSprite.drawLine(ix - 5, iy + i - 30, ix + iw + 5, iy + i - 15, BLACK);
      }
    }
  }
  else if (mood == MOOD_SAD) {
    if (isLeft) {
      for (int i = 0; i < 30; i++) {
        eyeSprite.drawLine(ix - 5, iy + i - 30, ix + iw + 5, iy + i - 15, BLACK);
      }
    } else {
      for (int i = 0; i < 30; i++) {
        eyeSprite.drawLine(ix - 5, iy + i - 15, ix + iw + 5, iy + i - 30, BLACK);
      }
    }
  }
  else if (mood == MOOD_HAPPY || mood == MOOD_LOVE || mood == MOOD_EXCITED) {
    eyeSprite.fillRect(ix - 5, iy + ih - 24, iw + 10, 30, BLACK);
    eyeSprite.fillCircle(ix + iw / 2, iy + ih + 12, iw / 1.3f, BLACK);
  }
  else if (mood == MOOD_SLEEPY) {
    eyeSprite.fillRect(ix - 5, iy - 5, iw + 10, ih / 2 + 4, BLACK);
  }
  else if (mood == MOOD_SUSPICIOUS) {
    if (isLeft) {
      eyeSprite.fillRect(ix - 5, iy - 5, iw + 10, ih / 2 - 4, BLACK);
    } else {
      eyeSprite.fillRect(ix - 5, iy + ih - 16, iw + 10, 20, BLACK);
    }
  }
}

void drawHeartToSprite(int x, int y, int size, uint16_t color) {
  int r = size / 4;
  eyeSprite.fillCircle(x - r, y - r/2, r, color);
  eyeSprite.fillCircle(x + r, y - r/2, r, color);
  eyeSprite.fillTriangle(x - 2 * r, y - r/2, x + 2 * r, y - r/2, x, y + size/2, color);
}

void drawEyeToSprite(Eye& e, bool isLeft, bool roundEye) {
  eyeSprite.fillScreen(BLACK);
  uint16_t eyeColour = EYE_COLOURS[currentEyeColourIndex];

  int ix = (int)e.x;
  int iy = (int)e.y;
  int iw = (int)e.w;
  int ih = (int)e.h;

  int r = 16;
  if (iw < 40) r = 6;

  // 1. Draw Sclera
  if (currentMood == MOOD_HEART) {
    drawHeartToSprite(ix + iw / 2, iy + ih / 2, iw, eyeColour);
  } else if (roundEye) {
    eyeSprite.fillEllipse(ix + iw / 2, iy + ih / 2, iw / 2, ih / 2, eyeColour);
  } else {
    eyeSprite.fillRoundRect(ix, iy, iw, ih, r, eyeColour);
  }

  // 2. Draw Pupil (Skip for MOOD_HEART)
  if (currentMood != MOOD_HEART) {
    int cx = ix + iw / 2;
    int cy = iy + ih / 2;
    int pw = (int)(iw / 2.2f);
    int ph = (int)(ih / 2.2f);
    int px = cx + (int)e.pupilX - (pw / 2);
    int py = cy + (int)e.pupilY - (ph / 2);

    int minPX = ix + SCLERA_PAD;
    int maxPX = ix + iw - SCLERA_PAD - pw;
    int minPY = iy + SCLERA_PAD;
    int maxPY = iy + ih - SCLERA_PAD - ph;
    if (px < minPX) px = minPX;
    if (px > maxPX) px = maxPX;
    if (py < minPY) py = minPY;
    if (py > maxPY) py = maxPY;

    if (roundEye) {
      eyeSprite.fillEllipse(px + pw / 2, py + ph / 2, pw / 2, ph / 2, BLACK);
    } else {
      eyeSprite.fillRoundRect(px, py, pw, ph, r / 2, BLACK);
    }

    // 3. Draw Reflection Highlight
    if (iw > 30 && ih > 30) {
      eyeSprite.fillCircle(px + pw - 8, py + 8, 4, WHITE);
    }
  } else {
    // 3. Draw Reflection Highlight on Heart Eyeball
    if (iw > 30 && ih > 30) {
      eyeSprite.fillCircle(ix + iw / 2 + iw / 5, iy + ih / 2 - ih / 5, iw / 10, WHITE);
    }
  }

  // 4. Draw Eyelids
  drawEyelidMask(e.x, e.y, e.w, e.h, currentMood, isLeft);
}

// ===== Floating Particles & Helpers =====
void drawHeart(int x, int y, int size, uint16_t color) {
  int r = size / 4;
  lcd.fillCircle(x - r, y - r, r, color);
  lcd.fillCircle(x + r, y - r, r, color);
  lcd.fillTriangle(x - 2 * r, y - r/2, x + 2 * r, y - r/2, x, y + size/2, color);
}

void drawZZZ(int x, int y, int step) {
  lcd.setTextColor(WHITE, BLACK);
  lcd.setTextSize(2);
  lcd.setCursor(x, y);
  lcd.print("z");
  lcd.setTextSize(3);
  lcd.setCursor(x + 15, y - 12);
  lcd.print("Z");
  lcd.setTextSize(4);
  lcd.setCursor(x + 35, y - 28);
  lcd.print("Z");
}

void drawAngerMark(int x, int y, int r, uint16_t color) {
  lcd.drawFastHLine(x - r, y - r/2, 2*r, color);
  lcd.drawFastHLine(x - r, y + r/2, 2*r, color);
  lcd.drawFastVLine(x - r/2, y - r, 2*r, color);
  lcd.drawFastVLine(x + r/2, y - r, 2*r, color);
}

// Saccade & physics globals
unsigned long lastSaccade = 0;
unsigned long saccadeInterval = 3000;
float breathVal = 0.0f;

void updatePhysicsAndMood() {
  unsigned long now = millis();
  breathVal = sinf(now / 800.0f) * 3.0f;

  if (now > leftEye.nextBlinkTime) {
    leftEye.blinking  = true;
    leftEye.lastBlink = now;
    rightEye.blinking = true;
    leftEye.nextBlinkTime = now + random(2000, 6000);
  }
  if (leftEye.blinking) {
    leftEye.targetH  = 4;
    rightEye.targetH = 4;
    if (now - leftEye.lastBlink > 120) {
      leftEye.blinking  = false;
      rightEye.blinking = false;
    }
  }

  static float lx = 0.0f, ly = 0.0f;
  if (!leftEye.blinking && now - lastSaccade > saccadeInterval) {
    lastSaccade    = now;
    saccadeInterval = random(500, 3000);
    int dir = random(0, 10);
    lx = 0; ly = 0;
    if      (dir == 4) { lx = -6; ly = -4; }
    else if (dir == 5) { lx = 6;  ly = -4; }
    else if (dir == 6) { lx = -6; ly = 4; }
    else if (dir == 7) { lx = 6;  ly = 4; }
    else if (dir == 8) { lx = 8;  ly = 0; }
    else if (dir == 9) { lx = -8; ly = 0; }
    
    leftEye.targetPupilX  = lx * 2.0f;
    leftEye.targetPupilY  = ly * 2.0f;
    rightEye.targetPupilX = lx * 2.0f;
    rightEye.targetPupilY = ly * 2.0f;
  }

  if (!leftEye.blinking) {
    float baseW = 80.0f;
    float baseH = 80.0f + breathVal;
    switch (currentMood) {
      case MOOD_NORMAL:
        leftEye.targetW  = baseW;  leftEye.targetH  = baseH;
        rightEye.targetW = baseW;  rightEye.targetH = baseH;
        break;
      case MOOD_HAPPY:
      case MOOD_LOVE:
        leftEye.targetW  = 90.0f;  leftEye.targetH  = 70.0f;
        rightEye.targetW = 90.0f;  rightEye.targetH = 70.0f;
        break;
      case MOOD_SURPRISED:
        leftEye.targetW  = 68.0f;  leftEye.targetH  = 100.0f;
        rightEye.targetW = 68.0f;  rightEye.targetH = 100.0f;
        break;
      case MOOD_SLEEPY:
        leftEye.targetW  = 84.0f;  leftEye.targetH  = 66.0f;
        rightEye.targetW = 84.0f;  rightEye.targetH = 66.0f;
        break;
      case MOOD_ANGRY:
        leftEye.targetW  = 76.0f;  leftEye.targetH  = 70.0f;
        rightEye.targetW = 76.0f;  rightEye.targetH = 70.0f;
        break;
      case MOOD_SAD:
        leftEye.targetW  = 76.0f;  leftEye.targetH  = 90.0f;
        rightEye.targetW = 76.0f;  rightEye.targetH = 90.0f;
        break;
      case MOOD_EXCITED:
        leftEye.targetW  = 94.0f;  leftEye.targetH  = 94.0f;
        rightEye.targetW = 94.0f;  rightEye.targetH = 94.0f;
        break;
      case MOOD_SUSPICIOUS:
        leftEye.targetW  = 80.0f;  leftEye.targetH  = 44.0f;
        rightEye.targetW = 80.0f;  rightEye.targetH = 94.0f;
        break;
      case MOOD_HEART:
        leftEye.targetW  = 86.0f;  leftEye.targetH  = 86.0f + breathVal;
        rightEye.targetW = 86.0f;  rightEye.targetH = 86.0f + breathVal;
        break;
    }
  }

  leftEye.targetX  = (100.0f - leftEye.targetW) / 2.0f + lx * 0.7f;
  leftEye.targetY  = (100.0f - leftEye.targetH) / 2.0f + ly * 0.7f;
  rightEye.targetX = (100.0f - rightEye.targetW) / 2.0f + lx * 0.7f;
  rightEye.targetY = (100.0f - rightEye.targetH) / 2.0f + ly * 0.7f;

  leftEye.update();
  rightEye.update();
}

void updateMoodBasedOnWeather() {
  if      (weatherMain == "Clear")                              currentMood = MOOD_HAPPY;
  else if (weatherMain == "Rain" || weatherMain == "Drizzle")   currentMood = MOOD_SAD;
  else if (weatherMain == "Thunderstorm")                       currentMood = MOOD_SURPRISED;
  else if (currentTemp > 35.0f)                                 currentMood = MOOD_EXCITED;
  else if (currentTemp < 5.0f)                                  currentMood = MOOD_SLEEPY;
  else                                                          currentMood = MOOD_NORMAL;
}


// ===== Networking & data =====
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED || ow_api_key.length() == 0) return;

  HTTPClient http;

  // Current weather
  String url = "http://api.openweathermap.org/data/2.5/weather?q=";
  url += ow_city;
  url += ",";
  url += ow_country;
  url += "&appid=";
  url += ow_api_key;
  url += "&units=metric";

  http.begin(url);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    JSONVar obj = JSON.parse(payload);
    if (JSON.typeof(obj) != "undefined") {
      currentTemp  = double(obj["main"]["temp"]);
      currentHum   = int(obj["main"]["humidity"]);
      weatherMain  = (const char*)obj["weather"][0]["main"];
      weatherDesc  = (const char*)obj["weather"][0]["description"];
      if (weatherDesc.length() > 0) {
        weatherDesc[0] = toupper(weatherDesc[0]);
      }
      updateMoodBasedOnWeather();
    }
  }
  http.end();

  // Simple forecast (next 3 time slots, ~today+)
  url = "http://api.openweathermap.org/data/2.5/forecast?q=";
  url += ow_city;
  url += ",";
  url += ow_country;
  url += "&appid=";
  url += ow_api_key;
  url += "&units=metric";

  http.begin(url);
  code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    JSONVar fo = JSON.parse(payload);
    if (JSON.typeof(fo) != "undefined") {
      int indices[3] = { 1, 3, 7 }; // ~next few periods
      const char* labels[3] = { "Soon", "Later", "Tomorrow" };
      for (int i = 0; i < 3; i++) {
        int idx = indices[i];
        forecast[i].temp  = (int)double(fo["list"][idx]["main"]["temp"]);
        forecast[i].label = labels[i];
      }
    }
  }
  http.end();
}

// ===== Drawing pages =====
void drawClockPage() {
  lcd.clear(BLACK);
  uint16_t uiColour = EYE_COLOURS[currentEyeColourIndex];
  lcd.setTextColor(uiColour, BLACK);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    lcd.setCursor(40, 110);
    lcd.print("Syncing time...");
    return;
  }

  int hour24 = timeinfo.tm_hour;
  int hour12 = hour24 % 12;
  if (hour12 == 0) hour12 = 12;
  int min = timeinfo.tm_min;
  char buf[6];
  sprintf(buf, "%02d:%02d", hour12, min);

  lcd.setFont(&fonts::Font7);
  lcd.setTextDatum(middle_center);
  lcd.setTextSize(1.5);
  lcd.drawString(buf, 120, 100);

  lcd.setFont(nullptr);
  lcd.setTextSize(4);
  lcd.setTextDatum(top_center);
  char dateBuf[20];
  strftime(dateBuf, sizeof(dateBuf), "%a %d %b", &timeinfo);
  lcd.drawString(dateBuf, 120, 150);
}

void drawWeatherPage() {
  lcd.clear(BLACK);
  uint16_t uiColour = EYE_COLOURS[currentEyeColourIndex];
  lcd.setTextColor(uiColour, BLACK);

  lcd.setTextSize(3);
  String loc = ow_city + ", " + ow_country;
  uint8_t len = loc.length();
  uint8_t charW = 4;
  int16_t textW = len * charW;
  int16_t x = 30;
  int16_t y = 10;
  lcd.setCursor(x, y);
  lcd.print(loc);

  String tStr = String((int)currentTemp) + "C";
  len = tStr.length();
  uint8_t sizeTemp = 6;
  textW = len * charW * sizeTemp;
  x = 65;
  y = 70;
  lcd.setTextSize(sizeTemp);
  lcd.setCursor(x, y);
  lcd.print(tStr);

  lcd.setTextSize(2);
  String descLine = weatherMain + String(" - ") + weatherDesc;
  len = descLine.length();
  textW = len * charW;
  x = 10;
  y = 150;
  lcd.setCursor(x, y);
  lcd.print(descLine);

  String humStr = String("Hum ") + String(currentHum) + "%";
  len = humStr.length();
  textW = len * charW;
  x = 60;
  y = 190;
  lcd.setCursor(x, y);
  lcd.print(humStr);
}

void drawForecastPage() {
  lcd.clear(BLACK);
  uint16_t uiColour = EYE_COLOURS[currentEyeColourIndex];
  lcd.setTextColor(uiColour, BLACK);
  lcd.setTextSize(3);

  String title = "FORECAST";
  uint8_t len = title.length();
  uint8_t charW = 6;
  int16_t textW = len * charW;
  int16_t x = 60;
  int16_t y = 40;
  lcd.setCursor(x, y);
  lcd.print(title);

  y = 90;
  for (int i = 0; i < 3; i++) {
    String line = forecast[i].label + String(": ") + String(forecast[i].temp) + "C";
    len = line.length();
    textW = len * charW * 2;
    x = (240 - textW) / 2;
    lcd.setTextSize(2);
    lcd.setCursor(x, y);
    lcd.print(line);
    y += 35;
  }
}

// ===== Setup & loop =====
void setup() {
  lcd.init();
  lcd.setRotation(1);
  lcd.clear(BLACK);

  eyeSprite.setColorDepth(16);
  eyeSprite.createSprite(100, 100);

  leftEye.init(10, 10, 80, 80);
  rightEye.init(10, 10, 80, 80);

  pinMode(TOUCH_PIN, INPUT);

  // Startup splash: ESC-Labs for 1 second
  lcd.setFont(nullptr);
  lcd.setTextSize(3);
  lcd.setTextDatum(middle_center);
  lcd.setTextColor(WHITE, BLACK);
  lcd.clear(BLACK);
  lcd.drawString("ESC-Labs", 120, 120);
  delay(1000);

  // DESK-BUDDY cycling through all colours for 2 seconds total
  lcd.clear(BLACK);
  const unsigned long DESK_BUDDY_MS = 2000;
  const unsigned long perColourMs = DESK_BUDDY_MS / NUM_EYE_COLOURS;
  for (int i = 0; i < NUM_EYE_COLOURS; i++) {
    lcd.setTextColor(EYE_COLOURS[i], BLACK);
    lcd.drawString("DESK-BUDDY", 120, 120);
    delay(perColourMs);
  }

  // Connecting... screen
  lcd.clear(BLACK);
  lcd.setTextColor(WHITE, BLACK);
  lcd.drawString("Connecting...", 120, 120);

  loadConfig();

  // Try to connect to saved WiFi (or skip if no SSID saved)
  if (wifi_ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(200);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    // No config or connection failed: start AP + captive portal
    uint16_t uiColour = EYE_COLOURS[currentEyeColourIndex];
    lcd.clear(BLACK);
    lcd.setTextSize(2);
    lcd.setTextColor(uiColour, BLACK);
    lcd.setTextDatum(middle_center);
    lcd.drawString("Config mode", 120, 80);
    lcd.setTextDatum(top_left);
    lcd.setCursor(10, 120);
    lcd.print("Connect to:");
    lcd.setCursor(10, 140);
    lcd.print(CONFIG_AP_SSID);
    lcd.setCursor(10, 160);
    lcd.print("192.168.4.1");
    startConfigAP();
    return;
  }

  // Connected: NTP + weather, and serve config page at http://<ip>/
  configTime(gmtOffsetSec, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  fetchWeather();
  lastWeatherUpdate = millis();

  server.on("/", HTTP_GET, handleConfigGet);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

void loop() {
  // Config AP mode: only run web server until user saves and device reboots
  if (configMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(10);
    return;
  }

  // Web config: handle requests when connected
  server.handleClient();

  unsigned long nowMs = millis();

  // Periodically refresh weather
  if (nowMs - lastWeatherUpdate > WEATHER_INTERVAL_MS) {
    fetchWeather();
    lastWeatherUpdate = nowMs;
  }

  // Gesture handling: single tap, double tap, triple tap, and long press
  static int tapCounter = 0;
  static unsigned long lastTapTime = 0;
  static bool lastPinState = false;
  static unsigned long pressStartTime = 0;
  static bool isLongPressHandled = false;

  const unsigned long LONG_PRESS_TIME = 800;
  const unsigned long DOUBLE_TAP_DELAY = 400;

  bool currentPinState = digitalRead(TOUCH_PIN);

  if (currentPinState && !lastPinState) {
    pressStartTime = nowMs;
    isLongPressHandled = false;
  } else if (currentPinState && lastPinState) {
    if ((nowMs - pressStartTime > LONG_PRESS_TIME) && !isLongPressHandled) {
      if (currentPage == 0) {
        currentMood = (currentMood + 1) % 10;
        lastSaccade = 0; // Look straight initially on mood change
      }
      isLongPressHandled = true;
    }
  } else if (!currentPinState && lastPinState) {
    if ((nowMs - pressStartTime < LONG_PRESS_TIME) && !isLongPressHandled) {
      tapCounter++;
      lastTapTime = nowMs;
    }
  }
  lastPinState = currentPinState;

  if (tapCounter > 0) {
    if (nowMs - lastTapTime > DOUBLE_TAP_DELAY) {
      if (tapCounter == 1) {
        // Single tap: cycle page
        currentPage = (currentPage + 1) % 4;
      } else if (tapCounter == 2) {
        // Double tap: cycle color
        currentEyeColourIndex = (currentEyeColourIndex + 1) % NUM_EYE_COLOURS;
        saveEyeColour();
      } else if (tapCounter >= 3) {
        // Triple tap: toggle round vs rounded-rect mode
        roundEyeMode = !roundEyeMode;
      }
      tapCounter = 0;
    }
  }

  // Draw current page content
  if (currentPage == 0) {
    // Eyes page with smooth physics animations
    if (lastPage != 0) {
      lcd.clear(BLACK);
    }
    
    // Update physics variables
    updatePhysicsAndMood();

    // Draw floating particles if applicable
    if (currentMood == MOOD_LOVE) {
      drawHeart(120, 45, 24, 0xF800); // Center red heart
      drawHeart(50, 45, 12, 0xF800);  // Left red heart
      drawHeart(190, 45, 12, 0xF800); // Right red heart
    } else if (currentMood == MOOD_SLEEPY) {
      drawZZZ(110, 45, 0); // ZZZ in the middle
    } else if (currentMood == MOOD_ANGRY) {
      drawAngerMark(45, 40, 8, 0xF800);  // Anger mark left
      drawAngerMark(195, 40, 8, 0xF800); // Anger mark right
    }

    // Render left eye
    drawEyeToSprite(leftEye, true, roundEyeMode);
    eyeSprite.pushSprite(CX_LEFT - 50, CY - 50);

    // Render right eye
    drawEyeToSprite(rightEye, false, roundEyeMode);
    eyeSprite.pushSprite(CX_RIGHT - 50, CY - 50);

  } else if (currentPage == 1) {
    // Clock
    struct tm timeinfo;
    bool hasTime = getLocalTime(&timeinfo);
    if (currentPage != lastPage || 
        (hasTime && timeinfo.tm_min != lastClockMin) || 
        (!hasTime && nowMs - lastInfoRedrawMs > 1000)) {
      drawClockPage();
      lastInfoRedrawMs = nowMs;
      if (hasTime) {
        lastClockMin = timeinfo.tm_min;
      }
    }
  } else if (currentPage == 2) {
    // Weather
    if (currentPage != lastPage || lastWeatherPageDrawMs != lastWeatherUpdate) {
      drawWeatherPage();
      lastWeatherPageDrawMs = lastWeatherUpdate;
    }
  } else {
    // Forecast
    if (currentPage != lastPage || lastForecastPageDrawMs != lastWeatherUpdate) {
      drawForecastPage();
      lastForecastPageDrawMs = lastWeatherUpdate;
    }
  }

  lastPage = currentPage;
  delay(16); // ~60fps target animation loop rate
}
