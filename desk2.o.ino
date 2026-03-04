#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <Preferences.h>
#include "time.h"

// ===== Display configuration for XIAO ESP32S3 + ST7789 240x240 =====
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

      // XIAO ESP32S3 default SPI pins
      cfg.pin_sclk = 7;               // D8  (SCK)
      cfg.pin_mosi = 9;               // D10 (MOSI)
      cfg.pin_miso = -1;              // not used
      cfg.pin_dc   = 4;               // D4  (DC)

      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    { // Panel config
      auto cfg = _panel.config();
      cfg.pin_cs   = 2;               // D2 (CS)
      cfg.pin_rst  = 3;               // D3 (RST)
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

const int16_t EYE_W      = 80;
const int16_t EYE_H      = 80;
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

// Touch input
const int TOUCH_PIN = 1;          // GPIO 1 touch sensor
const unsigned long DOUBLE_TAP_MS = 1000;  // max time between taps to count as double-tap
unsigned long lastTapTime = 0;

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
bool lastTouchState      = false;
unsigned long lastTouchTime = 0;
const unsigned long TOUCH_DEBOUNCE_MS = 200;

const unsigned long LONG_PRESS_MS = 800;
bool roundEyeMode       = false;  // long-press on eyes page toggles round eyes
unsigned long touchDownTime = 0;
bool touchDownValid     = false;

// Info-page redraw throttle
unsigned long lastInfoRedrawMs = 0;

// Animation
float angle = 0.0f;

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

// ===== Eye drawing =====
// roundEye = true -> circular eye, false -> rounded-rect eye
void drawEyeToSprite(int16_t offX, int16_t offY, bool roundEye) {
  eyeSprite.fillScreen(BLACK);
  uint16_t eyeColour = EYE_COLOURS[currentEyeColourIndex];

  int16_t halfW = EYE_W / 2;
  int16_t halfH = EYE_H / 2;

  if (roundEye) {
    int16_t radius = halfW;   // 40, fits in 80x80
    eyeSprite.fillCircle(halfW, halfH, radius, eyeColour);
  } else {
    eyeSprite.fillRoundRect(0, 0, EYE_W, EYE_H, 10, eyeColour);
  }

  // Pupil center & clamp
  int16_t cx = halfW + offX;
  int16_t cy = halfH + offY;
  int16_t minCX = SCLERA_PAD + PUPIL_SIZE / 2;
  int16_t maxCX = EYE_W - SCLERA_PAD - PUPIL_SIZE / 2;
  int16_t minCY = SCLERA_PAD + PUPIL_SIZE / 2;
  int16_t maxCY = EYE_H - SCLERA_PAD - PUPIL_SIZE / 2;

  if (cx < minCX) cx = minCX;
  if (cx > maxCX) cx = maxCX;
  if (cy < minCY) cy = minCY;
  if (cy > maxCY) cy = maxCY;

  int16_t pupilR = PUPIL_SIZE / 2;  // 18

  if (roundEye) {
    eyeSprite.fillCircle(cx, cy, pupilR, BLACK);
  } else {
    eyeSprite.fillRoundRect(cx - pupilR, cy - pupilR, PUPIL_SIZE, PUPIL_SIZE, 6, BLACK);
  }
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
  eyeSprite.createSprite(EYE_W, EYE_H);

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

  angle += 0.02f;          // adjust for speed; lower = slower
  if (angle > TWO_PI) angle -= TWO_PI;

  float maxOffsetX = (EYE_W / 2.0f) - SCLERA_PAD - (PUPIL_SIZE / 2.0f);
  float maxOffsetY = (EYE_H / 2.0f) - SCLERA_PAD - (PUPIL_SIZE / 2.0f);

  int16_t offX = (int16_t)(cosf(angle) * maxOffsetX);
  int16_t offY = (int16_t)(sinf(angle * 0.5f) * maxOffsetY);

  // Web config: handle requests when connected
  server.handleClient();

  // Periodically refresh weather
  unsigned long nowMs = millis();
  if (nowMs - lastWeatherUpdate > WEATHER_INTERVAL_MS) {
    fetchWeather();
    lastWeatherUpdate = nowMs;
  }

  // Touch: on DOWN record time; on UP decide long-press vs double-tap vs single tap
  bool touch = digitalRead(TOUCH_PIN);
  if (touch && !lastTouchState && (nowMs - lastTouchTime > TOUCH_DEBOUNCE_MS)) {
    touchDownTime = nowMs;
    touchDownValid = true;
  }
  if (!touch && lastTouchState && touchDownValid) {
    unsigned long duration = nowMs - touchDownTime;
    if (duration >= LONG_PRESS_MS && currentPage == 0) {
      // Long-press on eyes page: toggle round eye animation
      roundEyeMode = !roundEyeMode;
    } else {
      // Short release = tap: double-tap = cycle colour, else advance page
      if (lastTapTime != 0 && (touchDownTime - lastTapTime) <= DOUBLE_TAP_MS) {
        currentEyeColourIndex = (currentEyeColourIndex + 1) % NUM_EYE_COLOURS;
        saveEyeColour();
      } else {
        currentPage = (currentPage + 1) % 4;
      }
      lastTapTime = touchDownTime;
    }
    lastTouchTime = nowMs;
    touchDownValid = false;
  }
  lastTouchState = touch;

  // Draw current page content
  if (currentPage == 0) {
    // Eyes page: round or rounded-rect eyes depending on long-press
    if (lastPage != 0) {
      lcd.clear(BLACK);
    }
    drawEyeToSprite(offX, offY, roundEyeMode);
    eyeSprite.pushSprite(CX_LEFT - EYE_W / 2, CY - EYE_H / 2);
    drawEyeToSprite(offX, offY, roundEyeMode);
    eyeSprite.pushSprite(CX_RIGHT - EYE_W / 2, CY - EYE_H / 2);
  } else if (currentPage == 1) {
    // Clock
    if (currentPage != lastPage) {
      lcd.clear(BLACK);
    }
    if (currentPage != lastPage || nowMs - lastInfoRedrawMs > 1000) {
      drawClockPage();
      lastInfoRedrawMs = nowMs;
    }
  } else if (currentPage == 2) {
    // Weather
    if (currentPage != lastPage || nowMs - lastInfoRedrawMs > 2000) {
      drawWeatherPage();
      lastInfoRedrawMs = nowMs;
    }
  } else {
    // Forecast
    if (currentPage != lastPage || nowMs - lastInfoRedrawMs > 5000) {
      drawForecastPage();
      lastInfoRedrawMs = nowMs;
    }
  }

  lastPage = currentPage;
  delay(20);
}