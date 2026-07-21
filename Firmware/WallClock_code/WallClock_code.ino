/*
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║       NeoPixel Smart Wall Clock  —  v2  (NTP Edition)           ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  All original features kept. New additions:                     ║
 * ║  ✦ AP mode  (SSID: WallClock  |  Pass: clockwifi)              ║
 * ║  ✦ Web UI at 192.168.4.1 / wallclock.local                     ║
 * ║    • Color pickers for Morning / Noon / Evening / Night         ║
 * ║    • Min / Max brightness sliders                               ║
 * ║    • Wi-Fi credentials + timezone selector → NTP auto-sync     ║
 * ║    • Manual "Sync Now" button                                   ║
 * ║    • Auto-reconnect + periodic re-sync every 6 hours           ║
 * ║    • OTA firmware upload with progress bar                      ║
 * ║    • Live clock on the page with NTP badge                      ║
 * ║  ✦ All settings saved to EEPROM (survive power cycles)         ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  Required libraries (Arduino Library Manager):                  ║
 * ║    Adafruit NeoPixel  |  RTClib                                 ║
 * ║  Built into the ESP8266 core — no install needed:              ║
 * ║    ESP8266WiFi  ESP8266WebServer  ESP8266mDNS  DNSServer        ║
 * ║    EEPROM  time.h (SNTP)                                        ║
 * ╚══════════════════════════════════════════════════════════════════╝
 */

#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <RTClib.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <time.h>       // SNTP / configTime() / time()

// ═══════════════════════════════════════════════════════════
//  Pin & Segment Constants  (unchanged)
// ═══════════════════════════════════════════════════════════
#define LED_PIN       D5
#define NUM_SEGMENTS  31
#define LEN_NORMAL     5
#define LEN_COLON      1
#define LDR_PIN       A0

// ═══════════════════════════════════════════════════════════
//  Access-Point Credentials
// ═══════════════════════════════════════════════════════════
const char* AP_SSID = "WallClock";
const char* AP_PASS = "clockwifi";     // must be ≥ 8 chars

// ═══════════════════════════════════════════════════════════
//  EEPROM Layout
//  MAGIC changed to 0xAD → forces factory-reset on first boot
//  after flashing so new WiFi / timezone fields are initialised.
// ═══════════════════════════════════════════════════════════
#define EEPROM_SIZE  128
#define MAGIC_BYTE   0xAD

struct ClockSettings {
  uint8_t  magic;
  // ── colors ──
  uint8_t  mrR, mrG, mrB;   // Morning
  uint8_t  noR, noG, noB;   // Noon
  uint8_t  evR, evG, evB;   // Evening
  uint8_t  niR, niG, niB;   // Night
  // ── brightness ──
  uint8_t  minBr, maxBr;
  // ── network ──
  int16_t  tzOffset;         // UTC offset in minutes  (IST = 330)
  char     wifiSSID[33];     // max 32 chars + '\0'
  char     wifiPass[65];     // max 64 chars + '\0'
};  // 115 bytes — safely within EEPROM_SIZE=128

ClockSettings cfg;

// ═══════════════════════════════════════════════════════════
//  Runtime State
// ═══════════════════════════════════════════════════════════
int           currentBrightness  = 120;

// WiFi state machine
bool          wifiConnecting     = false;
bool          wifiConnected      = false;
unsigned long wifiConnectStart   = 0;
unsigned long lastWifiRetry      = 0;
#define WIFI_CONNECT_TIMEOUT_MS  15000UL   // 15 s per attempt
#define WIFI_RETRY_INTERVAL_MS   60000UL   // retry every 60 s on failure

// NTP state machine
bool          ntpSyncing         = false;
bool          ntpSynced          = false;
unsigned long ntpSyncStart       = 0;
unsigned long lastNTPSync        = 0;
char          lastSyncTimeStr[9] = "--:--:--";   // "HH:MM:SS"
#define NTP_SYNC_TIMEOUT_MS      20000UL   // 20 s to get first response
#define NTP_RESYNC_INTERVAL_MS   (6UL * 3600UL * 1000UL)   // 6 hours

// ═══════════════════════════════════════════════════════════
//  Hardware Objects
// ═══════════════════════════════════════════════════════════
RTC_DS3231         rtc;
Adafruit_NeoPixel* pStrip = nullptr;
ESP8266WebServer   server(80);
DNSServer          dnsServer;

// ═══════════════════════════════════════════════════════════
//  Segment Mapping  (unchanged from original)
// ═══════════════════════════════════════════════════════════
const char* segmentName[NUM_SEGMENTS] = {
  "a","b","c","d","e","f","g",
  "h","i","j","k","l","m","n",
  "o","p",
  "q","r","s","t","u","v","w",
  "x","y","z","A","B","C","D","E"
};
const uint8_t digitSegments[4][7] = {
  { 0, 1, 2, 3, 4, 5, 6},
  { 7, 8, 9,10,11,12,13},
  {16,17,18,19,20,21,22},
  {23,24,25,26,27,28,29}
};
const uint8_t digitPattern[10][7] = {
  {1,1,1,0,1,1,1},{0,0,1,0,0,0,1},{0,1,1,1,1,1,0},
  {0,1,1,1,0,1,1},{1,0,1,1,0,0,1},{1,1,0,1,0,1,1},
  {1,1,0,1,1,1,1},{0,1,1,0,0,0,1},{1,1,1,1,1,1,1},
  {1,1,1,1,0,1,1}
};
uint16_t segLen[NUM_SEGMENTS];
uint32_t segStart[NUM_SEGMENTS];
uint16_t TOTAL_LEDS = 0;

// ═══════════════════════════════════════════════════════════
//  EEPROM  Helpers
// ═══════════════════════════════════════════════════════════
void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, cfg);
  if (cfg.magic != MAGIC_BYTE) {
    // First boot after flash — write factory defaults
    cfg.magic    = MAGIC_BYTE;
    cfg.mrR=255; cfg.mrG=240; cfg.mrB=180;   // Morning  : warm yellow-white
    cfg.noR=  0; cfg.noG=100; cfg.noB=255;   // Noon     : soft blue
    cfg.evR=255; cfg.evG=120; cfg.evB= 40;   // Evening  : orange
    cfg.niR=180; cfg.niG=140; cfg.niB=255;   // Night    : lavender
    cfg.minBr    =  10;
    cfg.maxBr    = 150;
    cfg.tzOffset = 330;                       // IST default
    memset(cfg.wifiSSID, 0, sizeof(cfg.wifiSSID));
    memset(cfg.wifiPass, 0, sizeof(cfg.wifiPass));
    EEPROM.put(0, cfg);
    EEPROM.commit();
    Serial.println(F("[EEPROM] Factory defaults written"));
  }
}

void saveSettings() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
  Serial.println(F("[EEPROM] Saved"));
}

// ═══════════════════════════════════════════════════════════
//  Color Utilities
// ═══════════════════════════════════════════════════════════
String rgbToHex(uint8_t r, uint8_t g, uint8_t b) {
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
  return String(buf);
}
void hexToRGB(const String& hex, uint8_t& r, uint8_t& g, uint8_t& b) {
  const char* h = hex.c_str() + (hex[0] == '#' ? 1 : 0);
  uint32_t v    = strtoul(h, nullptr, 16);
  r = (v >> 16) & 0xFF;
  g = (v >>  8) & 0xFF;
  b =  v        & 0xFF;
}

// ═══════════════════════════════════════════════════════════
//  Lightweight JSON field extractor (no ArduinoJson needed)
//  Finds "key":"value"  or  "key":number
// ═══════════════════════════════════════════════════════════
String jsonStr(const String& body, const char* key) {
  String k = String('"') + key + "\":\"";
  int i = body.indexOf(k);
  if (i < 0) return "";
  i += k.length();
  int e = body.indexOf('"', i);
  return (e > i) ? body.substring(i, e) : "";
}
long jsonNum(const String& body, const char* key) {
  String k = String('"') + key + "\":";
  int i = body.indexOf(k);
  if (i < 0) return 0;
  return body.substring(i + k.length()).toInt();
}

// ═══════════════════════════════════════════════════════════
//  Display Logic  (unchanged behaviour — colors now from cfg)
// ═══════════════════════════════════════════════════════════
void computeSegmentLayout() {
  TOTAL_LEDS = 0;
  for (uint8_t s = 0; s < NUM_SEGMENTS; s++) {
    bool col    = (segmentName[s][0]=='o' || segmentName[s][0]=='p');
    segLen[s]   = col ? LEN_COLON : LEN_NORMAL;
    segStart[s] = TOTAL_LEDS;
    TOTAL_LEDS += segLen[s];
  }
}
void lightSegment(uint8_t idx, uint32_t color) {
  for (uint16_t i = 0; i < segLen[idx]; i++)
    pStrip->setPixelColor(segStart[idx] + i, color);
}
void clearAll() {
  for (uint16_t i = 0; i < TOTAL_LEDS; i++) pStrip->setPixelColor(i, 0);
}
uint32_t getTimeColor(uint8_t hour) {
  if      (hour >=  5 && hour < 10) return pStrip->Color(cfg.mrR,cfg.mrG,cfg.mrB);
  else if (hour >= 10 && hour < 16) return pStrip->Color(cfg.noR,cfg.noG,cfg.noB);
  else if (hour >= 16 && hour < 19) return pStrip->Color(cfg.evR,cfg.evG,cfg.evB);
  else                               return pStrip->Color(cfg.niR,cfg.niG,cfg.niB);
}
void displayDigit(uint8_t didx, uint8_t num, uint32_t color) {
  for (uint8_t s = 0; s < 7; s++)
    if (digitPattern[num][s]) lightSegment(digitSegments[didx][s], color);
}
void displayTime(uint8_t hour, uint8_t minute, bool colonOn) {
  clearAll();
  uint32_t color = getTimeColor(hour);
  displayDigit(0, hour   / 10, color);
  displayDigit(1, hour   % 10, color);
  displayDigit(2, minute / 10, color);
  displayDigit(3, minute % 10, color);
  if (colonOn) {
    lightSegment(14, pStrip->Color(255,255,255));
    lightSegment(15, pStrip->Color(255,255,255));
  }
  pStrip->show();
}

// ═══════════════════════════════════════════════════════════
//  NTP  —  non-blocking state machine
// ═══════════════════════════════════════════════════════════

// Kick off an NTP sync. configTime() is non-blocking; the SNTP
// client runs in the background and time() returns the result.
void beginNTPSync() {
  long tzSec = (long)cfg.tzOffset * 60L;
  configTime(tzSec, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  ntpSyncStart = millis();
  ntpSyncing   = true;
  ntpSynced    = false;
  Serial.println(F("[NTP] Sync started"));
}

// Called every loop iteration while ntpSyncing == true.
// Once time() returns a valid Unix timestamp, updates the RTC.
void checkNTPSync() {
  if (!ntpSyncing) return;

  time_t t = time(nullptr);
  if (t > 1000000000UL) {           // sanity: year > ~2001
    struct tm* ti = localtime(&t);  // already in local TZ from configTime
    rtc.adjust(DateTime(
      (uint16_t)(ti->tm_year + 1900),
      (uint8_t) (ti->tm_mon  + 1),
      (uint8_t)  ti->tm_mday,
      (uint8_t)  ti->tm_hour,
      (uint8_t)  ti->tm_min,
      (uint8_t)  ti->tm_sec
    ));
    ntpSyncing = false;
    ntpSynced  = true;
    lastNTPSync = millis();
    // Record the RTC time-of-sync as a readable string
    snprintf(lastSyncTimeStr, sizeof(lastSyncTimeStr),
             "%02d:%02d:%02d", ti->tm_hour, ti->tm_min, ti->tm_sec);
    Serial.printf("[NTP] RTC updated → %s\n", lastSyncTimeStr);

  } else if (millis() - ntpSyncStart > NTP_SYNC_TIMEOUT_MS) {
    ntpSyncing = false;
    Serial.println(F("[NTP] Sync timed out — will retry later"));
  }
}

// Periodic re-sync: every 6 hours if connected
void checkPeriodicNTPSync() {
  if (!wifiConnected || wifiConnecting || ntpSyncing) return;
  if (!ntpSynced) return;   // initial sync hasn't succeeded yet
  if (millis() - lastNTPSync >= NTP_RESYNC_INTERVAL_MS) {
    Serial.println(F("[NTP] Periodic re-sync"));
    beginNTPSync();
  }
}

// ═══════════════════════════════════════════════════════════
//  Wi-Fi STA  —  non-blocking state machine
// ═══════════════════════════════════════════════════════════

// Initiate a STA connection attempt (AP stays up in WIFI_AP_STA mode)
void startWifiConnect() {
  if (strlen(cfg.wifiSSID) == 0) return;
  Serial.printf("[WiFi] Connecting to '%s'...\n", cfg.wifiSSID);
  WiFi.begin(cfg.wifiSSID, cfg.wifiPass);
  wifiConnecting   = true;
  wifiConnected    = false;
  wifiConnectStart = millis();
}

// Called every loop iteration — resolves the connection attempt
void checkWiFiConnect() {
  if (!wifiConnecting) return;

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnecting = false;
    wifiConnected  = true;
    lastWifiRetry  = millis();  // reset back-off timer
    Serial.print(F("[WiFi] Connected! IP: "));
    Serial.println(WiFi.localIP());
    beginNTPSync();             // kick off NTP immediately

  } else if (millis() - wifiConnectStart > WIFI_CONNECT_TIMEOUT_MS) {
    wifiConnecting = false;
    wifiConnected  = false;
    lastWifiRetry  = millis();  // wait WIFI_RETRY_INTERVAL before next attempt
    WiFi.disconnect();
    Serial.println(F("[WiFi] Timed out — will retry in 60 s"));
  }
}

// Detects if a connected link has dropped, and auto-retries
void checkWiFiHealth() {
  if (wifiConnecting) return;  // already in progress

  // Detect dropped link
  if (wifiConnected && WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    ntpSynced     = false;
    lastWifiRetry = millis();
    Serial.println(F("[WiFi] Link lost — will retry in 60 s"));
  }

  // Auto-retry if credentials are saved but not connected
  if (!wifiConnected && strlen(cfg.wifiSSID) > 0) {
    if (millis() - lastWifiRetry >= WIFI_RETRY_INTERVAL_MS) {
      Serial.println(F("[WiFi] Retrying..."));
      startWifiConnect();
    }
  }
}

// ═══════════════════════════════════════════════════════════
//  Web Page Builder
// ═══════════════════════════════════════════════════════════
String buildPage() {
  String p;
  p.reserve(7200);

  // ── Head & Styles ──────────────────────────────────────
  p += F("<!DOCTYPE html><html lang='en'><head>"
         "<meta charset='UTF-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Wall Clock Config</title><style>"
         "*{box-sizing:border-box;margin:0;padding:0}"
         "body{background:#0f0f1a;color:#ddd;"
         "font-family:'Segoe UI',Arial,sans-serif;"
         "padding:14px;max-width:460px;margin:auto}"

         "h1{color:#ffd700;font-size:1.55em;text-align:center;margin-bottom:3px}"
         "h2{font-size:.95em;color:#888;margin-bottom:10px;"
         "border-bottom:1px solid #2a2a3e;padding-bottom:5px}"
         ".sub{text-align:center;color:#555;font-size:.78em;margin-bottom:16px}"
         ".card{background:#1a1a2e;border-radius:12px;"
         "padding:14px;margin:10px 0;box-shadow:0 2px 10px #0006}"

         // live clock
         ".live{text-align:center;font-size:2.4em;font-weight:700;"
         "color:#ffd700;letter-spacing:.2em;padding:6px 0}"
         ".ntpbadge{display:inline-block;font-size:.3em;vertical-align:super;"
         "background:#143314;color:#6fff6f;padding:2px 6px;"
         "border-radius:8px;letter-spacing:0;margin-left:6px}"
         ".ntpbadge.off{background:#331414;color:#ff7070}"

         // color rows
         ".row{display:flex;align-items:center;gap:10px;margin-bottom:9px}"
         ".row label,.frow label{flex:1;font-size:.88em;line-height:1.4}"
         ".tag{display:inline-block;padding:1px 6px;border-radius:9px;"
         "font-size:.72em;margin-left:4px;vertical-align:middle}"
         ".mr{background:#332a00;color:#ffd96a}"
         ".no{background:#001f40;color:#6ab8ff}"
         ".ev{background:#331500;color:#ff9a4a}"
         ".ni{background:#180030;color:#c4a0ff}"
         "input[type=color]{width:46px;height:32px;"
         "border:2px solid #333;border-radius:7px;"
         "padding:2px;cursor:pointer;background:transparent;flex-shrink:0}"

         // sliders
         "input[type=range]{flex:1;accent-color:#ffd700;min-width:90px}"
         ".val{color:#ffd700;font-size:.88em;min-width:24px;text-align:right}"

         // text / select inputs
         ".frow{display:flex;align-items:center;gap:10px;margin-bottom:9px}"
         ".inp{flex:1;background:#111128;border:1px solid #2a2a4a;"
         "color:#ddd;padding:7px 10px;border-radius:8px;font-size:.88em;"
         "outline:none}"
         ".inp:focus{border-color:#ffd700}"
         "select.inp{cursor:pointer}"

         // net status badge
         ".netbadge{padding:7px 12px;border-radius:8px;font-size:.82em;"
         "margin-bottom:10px;display:block}"
         ".nb-gray{background:#222;color:#888}"
         ".nb-yellow{background:#332200;color:#ffd700}"
         ".nb-green{background:#103010;color:#6fff6f}"
         ".nb-red{background:#2a0808;color:#ff7070}"

         // buttons
         ".btn{display:block;width:100%;padding:11px;"
         "border:none;border-radius:10px;"
         "font-size:.97em;font-weight:700;cursor:pointer;margin-top:6px}"
         ".save{background:#ffd700;color:#0f0f1a}"
         ".save:hover{background:#e6c200}"
         ".conn{background:#1565c0;color:#fff}"
         ".conn:hover{background:#1976d2}"
         ".syncp{background:#4a148c;color:#fff;margin-top:6px}"
         ".syncp:hover:not(:disabled){background:#6a1aaf}"
         ".syncp:disabled{background:#2a1a3e;color:#555;cursor:not-allowed}"
         ".ota{background:#e94560;color:#fff;margin-top:8px}"
         ".ota:hover{background:#c03050}"
         ".ota:disabled{background:#555;cursor:not-allowed}"

         // status messages
         ".status{padding:8px 12px;border-radius:8px;"
         "margin-top:7px;font-size:.83em;display:none}"
         ".ok{background:#143314;color:#6fff6f;display:block}"
         ".er{background:#331414;color:#ff7070;display:block}"

         // OTA progress
         "#pww{background:#222;border-radius:3px;"
         "overflow:hidden;margin-top:8px;display:none;height:7px}"
         "#pw{height:7px;background:#ffd700;border-radius:3px;"
         "width:0;transition:width .2s}"
         "input[type=file]{width:100%;color:#888;font-size:.83em;padding:3px 0}"

         // eye-icon button
         ".eyebtn{background:#1a1a3a;border:1px solid #2a2a4a;color:#888;"
         "padding:7px 10px;border-radius:8px;cursor:pointer;font-size:.9em}"
         ".eyebtn:hover{color:#ddd}"

         "code{background:#222;padding:1px 5px;border-radius:4px;font-size:.85em}"
         "a{color:#ffd700}"
         "</style></head><body>");

  // ── Header ─────────────────────────────────────────────
  p += F("<h1>&#x1F553; Wall Clock</h1>"
         "<p class='sub'>Join Wi-Fi &nbsp;<b>WallClock</b>&nbsp;"
         "(pass: <b>clockwifi</b>)&nbsp; then open &nbsp;"
         "<a href='http://192.168.4.1'>192.168.4.1</a>"
         " &nbsp;or&nbsp; "
         "<a href='http://wallclock.local'>wallclock.local</a></p>");

  // ── Live Clock ─────────────────────────────────────────
  p += F("<div class='card'>"
         "<div class='live' id='clk'>--:--:--"
         "<span class='ntpbadge off' id='ntpbadge'>NTP</span>"
         "</div></div>");

  // ── Time-Based Colors ──────────────────────────────────
  p += F("<div class='card'><h2>&#x1F3A8;&nbsp; Time-Based Colors</h2>");

  p += F("<div class='row'><label>&#x1F305; Morning"
         "<span class='tag mr'>05:00–10:00</span></label>"
         "<input type='color' id='c_mr' value='");
  p += rgbToHex(cfg.mrR,cfg.mrG,cfg.mrB);
  p += F("'></div>");

  p += F("<div class='row'><label>&#x2600;&#xFE0F; Noon"
         "<span class='tag no'>10:00–16:00</span></label>"
         "<input type='color' id='c_no' value='");
  p += rgbToHex(cfg.noR,cfg.noG,cfg.noB);
  p += F("'></div>");

  p += F("<div class='row'><label>&#x1F306; Evening"
         "<span class='tag ev'>16:00–19:00</span></label>"
         "<input type='color' id='c_ev' value='");
  p += rgbToHex(cfg.evR,cfg.evG,cfg.evB);
  p += F("'></div>");

  p += F("<div class='row'><label>&#x1F319; Night"
         "<span class='tag ni'>19:00–05:00</span></label>"
         "<input type='color' id='c_ni' value='");
  p += rgbToHex(cfg.niR,cfg.niG,cfg.niB);
  p += F("'></div></div>");

  // ── Brightness ─────────────────────────────────────────
  p += F("<div class='card'><h2>&#x1F4A1;&nbsp; Auto-Brightness (LDR)</h2>");

  p += F("<div class='row'><label>&#x1F315; Max brightness"
         "&nbsp;<span style='color:#555;font-size:.8em'>(bright room)</span></label>"
         "<input type='range' id='br_max' min='50' max='255' value='");
  p += cfg.maxBr;
  p += F("' oninput=\"document.getElementById('v_max').innerText=this.value\">"
         "<span class='val' id='v_max'>");
  p += cfg.maxBr;
  p += F("</span></div>");

  p += F("<div class='row'><label>&#x1F311; Min brightness"
         "&nbsp;<span style='color:#555;font-size:.8em'>(dark room)</span></label>"
         "<input type='range' id='br_min' min='1' max='100' value='");
  p += cfg.minBr;
  p += F("' oninput=\"document.getElementById('v_min').innerText=this.value\">"
         "<span class='val' id='v_min'>");
  p += cfg.minBr;
  p += F("</span></div></div>");

  // ── Save Button  (colors + brightness) ────────────────
  p += F("<button class='btn save' onclick='doSave()'>&#x1F4BE;&nbsp; Save Colors &amp; Brightness</button>"
         "<div id='ss' class='status'></div>");

  // ── Wi-Fi & NTP Card ───────────────────────────────────
  p += F("<div class='card' style='margin-top:14px'>"
         "<h2>&#x1F4F6;&nbsp; Wi-Fi &amp; NTP Time Sync</h2>"
         "<span id='nb' class='netbadge nb-gray'>&#x274C; Not connected</span>"

         "<div class='frow'><label>Network (SSID)</label>"
         "<input class='inp' type='text' id='ssid' maxlength='32' "
         "placeholder='Your home Wi-Fi name' value='");
  p += String(cfg.wifiSSID);          // pre-fill saved SSID
  p += F("'></div>"

         "<div class='frow'><label>Password</label>"
         "<div style='display:flex;gap:6px;flex:1'>"
         "<input class='inp' type='password' id='wpass' maxlength='64' "
         "placeholder='Leave blank to keep current'>"
         "<button class='eyebtn' onclick='togglePw()' title='Show/hide'>&#x1F441;</button>"
         "</div></div>"

         "<div class='frow'><label>Timezone</label>"
         "<select class='inp' id='tz'></select></div>"

         "<button class='btn conn' onclick='doWifi()'>&#x1F4F6;&nbsp; Connect &amp; Sync</button>"
         "<button class='btn syncp' id='ntpBtn' onclick='doNTPSync()' disabled>"
         "&#x231A;&nbsp; Sync Time Now</button>"
         "<div id='ws' class='status'></div></div>");

  // ── OTA Card ───────────────────────────────────────────
  p += F("<div class='card' style='margin-top:14px'>"
         "<h2>&#x26A1;&nbsp; OTA Firmware Update</h2>"
         "<p style='font-size:.8em;color:#555;margin-bottom:10px'>"
         "In Arduino IDE: <code>Sketch &rarr; Export Compiled Binary</code>, "
         "then upload the <code>.bin</code> file below. "
         "The clock restarts automatically after flashing.</p>"
         "<input type='file' id='fw' accept='.bin' onchange='fwChosen()'>"
         "<div id='pww'><div id='pw'></div></div>"
         "<button class='btn ota' id='flashBtn' onclick='doOTA()' disabled>"
         "&#x1F4E5;&nbsp; Flash Firmware</button>"
         "<div id='os' class='status'></div></div>");

  // ── JavaScript ─────────────────────────────────────────
  // Inject saved timezone as a JS variable so the select can pre-select it
  p += F("<script>const SAVED_TZ=");
  p += cfg.tzOffset;
  p += F(";");

  // Timezone list → select options
  p += F("(function(){"
         "const TZ=["
         "[-720,'UTC\u221212:00'],[-660,'UTC\u221211:00'],[-600,'UTC\u221210:00 (Hawaii)'],"
         "[-540,'UTC\u22129:00 (Alaska)'],[-480,'UTC\u22128:00 (PST)'],"
         "[-420,'UTC\u22127:00 (MST)'],[-360,'UTC\u22126:00 (CST)'],"
         "[-300,'UTC\u22125:00 (EST)'],[-240,'UTC\u22124:00 (AST)'],"
         "[-210,'UTC\u22123:30 (Newfoundland)'],[-180,'UTC\u22123:00 (Brazil)'],"
         "[-120,'UTC\u22122:00'],[-60,'UTC\u22121:00'],"
         "[0,'UTC\u00B10:00 (London / Lisbon)'],"
         "[60,'UTC+1:00 (Paris / Berlin)'],"
         "[120,'UTC+2:00 (Cairo / Athens)'],"
         "[180,'UTC+3:00 (Moscow / Riyadh)'],"
         "[210,'UTC+3:30 (Tehran)'],"
         "[240,'UTC+4:00 (Dubai / Baku)'],"
         "[270,'UTC+4:30 (Kabul)'],"
         "[300,'UTC+5:00 (Karachi / Tashkent)'],"
         "[330,'UTC+5:30 \u2605 IST (India / Sri Lanka)'],"
         "[345,'UTC+5:45 (Kathmandu)'],"
         "[360,'UTC+6:00 (Dhaka / Almaty)'],"
         "[390,'UTC+6:30 (Yangon)'],"
         "[420,'UTC+7:00 (Bangkok / Jakarta)'],"
         "[480,'UTC+8:00 (Singapore / Beijing / Perth)'],"
         "[525,'UTC+8:45 (Eucla)'],"
         "[540,'UTC+9:00 (Tokyo / Seoul)'],"
         "[570,'UTC+9:30 (Darwin / Adelaide)'],"
         "[600,'UTC+10:00 (Sydney / Brisbane)'],"
         "[630,'UTC+10:30'],"
         "[660,'UTC+11:00 (Noumea)'],"
         "[720,'UTC+12:00 (Auckland)'],"
         "[780,'UTC+13:00 (Samoa)'],"
         "[840,'UTC+14:00 (Kiribati)']];"
         "const sel=document.getElementById('tz');"
         "TZ.forEach(([v,l])=>{"
         "const o=new Option(l,v);"
         "if(v===SAVED_TZ)o.selected=true;"
         "sel.add(o);});"
         "})();");

  // Live clock — polls /time every second; shows NTP badge
  p += F("function fetchTime(){"
         "fetch('/time').then(r=>r.json()).then(d=>{"
         "const z=n=>String(n).padStart(2,'0');"
         "document.getElementById('clk').childNodes[0].nodeValue="
         "z(d.h)+':'+z(d.m)+':'+z(d.s);"
         "const b=document.getElementById('ntpbadge');"
         "if(d.ntp){b.className='ntpbadge';b.title='NTP synced at '+d.ls;}"
         "else{b.className='ntpbadge off';b.title='NTP not yet synced';}"
         "}).catch(()=>{});}"
         "setInterval(fetchTime,1000);fetchTime();");

  // Net status poller — every 3 s
  p += F("function pollNet(){"
         "fetch('/netstatus').then(r=>r.json()).then(d=>{"
         "const nb=document.getElementById('nb');"
         "const wb=document.getElementById('ws');"
         "const nb2=document.getElementById('ntpBtn');"
         "nb2.disabled=!d.connected;"
         "if(d.connecting){"
         "nb.className='netbadge nb-yellow';"
         "nb.innerHTML='&#x1F504; Connecting to <b>'+d.ssid+'</b>...';}"
         "else if(d.ntpSyncing){"
         "nb.className='netbadge nb-yellow';"
         "nb.innerHTML='&#x1F504; Syncing time via NTP...';}"
         "else if(d.connected&&d.ntpSynced){"
         "nb.className='netbadge nb-green';"
         "nb.innerHTML='\u2705 Connected &middot; <b>'+d.ip+"
         "'</b> &middot; NTP synced at '+d.lastSync;}"
         "else if(d.connected){"
         "nb.className='netbadge nb-yellow';"
         "nb.innerHTML='&#x1F4F6; Connected ('+d.ip+') &middot; NTP pending';}"
         "else{"
         "nb.className='netbadge nb-red';"
         "nb.innerHTML='\u274C Not connected';}}).catch(()=>{});}"
         "setInterval(pollNet,3000);pollNet();");

  // Toggle password visibility
  p += F("function togglePw(){"
         "const i=document.getElementById('wpass');"
         "i.type=i.type==='password'?'text':'password';}");

  // Save colors & brightness
  p += F("function doSave(){"
         "const b=JSON.stringify({"
         "morning:document.getElementById('c_mr').value,"
         "noon:document.getElementById('c_no').value,"
         "evening:document.getElementById('c_ev').value,"
         "night:document.getElementById('c_ni').value,"
         "minbr:document.getElementById('br_min').value,"
         "maxbr:document.getElementById('br_max').value});"
         "fetch('/save',{method:'POST',"
         "headers:{'Content-Type':'application/json'},body:b})"
         ".then(r=>r.text()).then(t=>{"
         "const s=document.getElementById('ss');"
         "const ok=t==='OK';"
         "s.className='status '+(ok?'ok':'er');"
         "s.innerText=ok?'\u2705 Settings saved!':'\u274C Save failed: '+t;"
         "setTimeout(()=>s.style.display='none',3500);"
         "}).catch(e=>{"
         "const s=document.getElementById('ss');"
         "s.className='status er';"
         "s.innerText='\u274C Network error: '+e;});}");

  // Connect to Wi-Fi + NTP
  p += F("function doWifi(){"
         "const ssid=document.getElementById('ssid').value.trim();"
         "if(!ssid){alert('Enter an SSID.');return;}"
         "const b=JSON.stringify({"
         "ssid:ssid,"
         "pass:document.getElementById('wpass').value,"
         "tz:Number(document.getElementById('tz').value)});"
         "const nb=document.getElementById('nb');"
         "nb.className='netbadge nb-yellow';"
         "nb.innerHTML='&#x1F504; Connecting...';"
         "fetch('/wifi',{method:'POST',"
         "headers:{'Content-Type':'application/json'},body:b})"
         ".then(r=>r.json()).then(d=>{"
         "const s=document.getElementById('ws');"
         "s.className='status ok';"
         "s.innerText='\u2705 Credentials saved — connecting in background...';"
         "setTimeout(()=>s.style.display='none',4000);"
         "pollNet();"
         "}).catch(e=>{"
         "const s=document.getElementById('ws');"
         "s.className='status er';"
         "s.innerText='\u274C '+e;});}");

  // Manual NTP sync
  p += F("function doNTPSync(){"
         "fetch('/ntpsync',{method:'POST'})"
         ".then(r=>r.json()).then(d=>{"
         "const s=document.getElementById('ws');"
         "if(d.ok){"
         "s.className='status ok';"
         "s.innerText='\u231A Sync requested — check badge for result...';}"
         "else{"
         "s.className='status er';"
         "s.innerText='\u274C '+d.msg;}"
         "setTimeout(()=>s.style.display='none',4000);"
         "}).catch(e=>{"
         "const s=document.getElementById('ws');"
         "s.className='status er';s.innerText='\u274C '+e;});}");

  // Enable Flash button only when a .bin is chosen
  p += F("function fwChosen(){"
         "document.getElementById('flashBtn').disabled="
         "!document.getElementById('fw').files.length;}");

  // OTA flash via XHR with progress bar
  p += F("function doOTA(){"
         "const f=document.getElementById('fw').files[0];"
         "if(!f){alert('Pick a .bin file first.');return;}"
         "const fd=new FormData();fd.append('firmware',f);"
         "const x=new XMLHttpRequest();"
         "x.open('POST','/update',true);"
         "document.getElementById('pww').style.display='block';"
         "document.getElementById('flashBtn').disabled=true;"
         "x.upload.onprogress=e=>{"
         "if(e.lengthComputable)"
         "document.getElementById('pw').style.width=(e.loaded/e.total*100)+'%';};"
         "x.onload=()=>{"
         "const s=document.getElementById('os');"
         "const ok=x.responseText.trim()==='OK';"
         "s.className='status '+(ok?'ok':'er');"
         "s.innerText=ok?"
         "'\u2705 Flash complete! Rebooting in 3 s...':'\u274C Flash failed: '+x.responseText;};"
         "x.onerror=()=>{"
         "const s=document.getElementById('os');"
         "s.className='status er';"
         "s.innerText='\u274C Upload error — check connection.';};"
         "x.send(fd);}"
         "</script></body></html>");

  return p;
}

// ═══════════════════════════════════════════════════════════
//  HTTP Handlers
// ═══════════════════════════════════════════════════════════

// GET /
void handleRoot() {
  server.send(200, "text/html", buildPage());
}

// GET /time  →  {"h":14,"m":30,"s":45,"ntp":true,"ls":"14:30:00"}
void handleGetTime() {
  DateTime now = rtc.now();
  char buf[80];
  snprintf(buf, sizeof(buf),
    "{\"h\":%d,\"m\":%d,\"s\":%d,\"ntp\":%s,\"ls\":\"%s\"}",
    now.hour(), now.minute(), now.second(),
    ntpSynced ? "true" : "false",
    lastSyncTimeStr);
  server.send(200, "application/json", buf);
}

// GET /netstatus  →  connection + NTP state for the page poller
void handleNetStatus() {
  char buf[220];
  snprintf(buf, sizeof(buf),
    "{\"connecting\":%s,\"connected\":%s,\"ip\":\"%s\","
    "\"ntpSyncing\":%s,\"ntpSynced\":%s,"
    "\"lastSync\":\"%s\",\"ssid\":\"%s\"}",
    wifiConnecting ? "true"  : "false",
    wifiConnected  ? "true"  : "false",
    wifiConnected  ? WiFi.localIP().toString().c_str() : "",
    ntpSyncing     ? "true"  : "false",
    ntpSynced      ? "true"  : "false",
    lastSyncTimeStr,
    cfg.wifiSSID);
  server.send(200, "application/json", buf);
}

// POST /save  —  colors + brightness (unchanged behaviour)
void handleSave() {
  if (!server.hasArg("plain")) { server.send(400,"text/plain","No body"); return; }
  String body = server.arg("plain");

  String mr = jsonStr(body,"morning");
  String no = jsonStr(body,"noon");
  String ev = jsonStr(body,"evening");
  String ni = jsonStr(body,"night");
  String mn = jsonStr(body,"minbr");
  String mx = jsonStr(body,"maxbr");

  if (mr.length()==7) hexToRGB(mr, cfg.mrR,cfg.mrG,cfg.mrB);
  if (no.length()==7) hexToRGB(no, cfg.noR,cfg.noG,cfg.noB);
  if (ev.length()==7) hexToRGB(ev, cfg.evR,cfg.evG,cfg.evB);
  if (ni.length()==7) hexToRGB(ni, cfg.niR,cfg.niG,cfg.niB);
  if (mn.length())    cfg.minBr = (uint8_t)constrain(mn.toInt(), 1, 100);
  if (mx.length())    cfg.maxBr = (uint8_t)constrain(mx.toInt(),50, 255);

  saveSettings();
  server.send(200, "text/plain", "OK");
}

// POST /wifi  —  save credentials + kick off non-blocking connection
void handleWifi() {
  if (!server.hasArg("plain")) { server.send(400,"text/plain","No body"); return; }
  String body = server.arg("plain");

  String ssid = jsonStr(body,"ssid");
  String pass = jsonStr(body,"pass");
  long   tz   = jsonNum(body,"tz");

  if (ssid.length() == 0) { server.send(400,"text/plain","Empty SSID"); return; }

  ssid.toCharArray(cfg.wifiSSID, sizeof(cfg.wifiSSID));
  if (pass.length() > 0)            // blank = keep existing password
    pass.toCharArray(cfg.wifiPass, sizeof(cfg.wifiPass));
  cfg.tzOffset = (int16_t)constrain(tz, -720, 840);

  saveSettings();

  // Restart STA connection with new credentials
  WiFi.disconnect();
  delay(100);
  wifiConnected = false;
  ntpSynced     = false;
  startWifiConnect();

  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /ntpsync  —  trigger a manual NTP sync
void handleNTPSync() {
  if (!wifiConnected) {
    server.send(503, "application/json", "{\"ok\":false,\"msg\":\"Not connected to Wi-Fi\"}");
    return;
  }
  beginNTPSync();
  server.send(200, "application/json", "{\"ok\":true}");
}

// POST /update (response after upload) — reboot after OTA
void handleOTA() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
  delay(200);
  ESP.restart();
}

// POST /update (upload data handler)
void handleOTAUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] Start: %s\n", upload.filename.c_str());
    WiFiUDP::stopAll();
    uint32_t maxSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSpace)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
      Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true))
      Serial.printf("[OTA] Done: %u bytes\n", upload.totalSize);
    else
      Update.printError(Serial);
  }
  yield();
}

// ═══════════════════════════════════════════════════════════
//  Setup
// ═══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n=== NeoPixel Wall Clock v2 Booting ==="));

  loadSettings();   // EEPROM (writes factory defaults on first boot)

  // ── LED Strip ──────────────────────────────────────────
  computeSegmentLayout();
  pStrip = new Adafruit_NeoPixel(TOTAL_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);
  pStrip->begin();
  pStrip->setBrightness(currentBrightness);
  pStrip->show();

  // ── RTC ────────────────────────────────────────────────
  if (!rtc.begin()) {
    Serial.println(F("RTC not found — halting"));
    while (1) yield();
  }
  if (rtc.lostPower()) {
    Serial.println(F("RTC lost power — setting compile time"));
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // ── Wi-Fi: AP + STA mode ───────────────────────────────
  // AP_STA keeps the config hotspot alive while connecting to home router
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("[WiFi] AP: %s  IP: %s\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());

  // Captive portal DNS — any DNS query → 192.168.4.1
  dnsServer.start(53, "*", WiFi.softAPIP());

  // mDNS — http://wallclock.local
  if (MDNS.begin("wallclock")) {
    MDNS.addService("http","tcp",80);
    Serial.println(F("[mDNS] http://wallclock.local"));
  }

  // ── HTTP Routes ────────────────────────────────────────
  server.on("/",          HTTP_GET,  handleRoot);
  server.on("/time",      HTTP_GET,  handleGetTime);
  server.on("/netstatus", HTTP_GET,  handleNetStatus);
  server.on("/save",      HTTP_POST, handleSave);
  server.on("/wifi",      HTTP_POST, handleWifi);
  server.on("/ntpsync",   HTTP_POST, handleNTPSync);
  server.on("/update",    HTTP_POST, handleOTA, handleOTAUpload);
  server.onNotFound([]() {
    server.sendHeader("Location","/");
    server.send(302,"text/plain","");
  });
  server.begin();
  Serial.println(F("[HTTP] Server ready on port 80"));

  // ── Boot-time Wi-Fi connect (non-blocking) ─────────────
  lastWifiRetry = millis();   // prevent immediate retry-check false-fire
  startWifiConnect();         // no-op if wifiSSID is empty

  Serial.println(F("=== Boot complete ===\n"));
}

// ═══════════════════════════════════════════════════════════
//  Loop
// ═══════════════════════════════════════════════════════════
void loop() {
  // ── Network service (must run every iteration) ─────────
  dnsServer.processNextRequest();
  server.handleClient();
  MDNS.update();

  // ── Wi-Fi + NTP state machines ─────────────────────────
  checkWiFiConnect();       // resolve ongoing connection attempt
  checkWiFiHealth();        // detect drops + auto-retry
  checkNTPSync();           // detect NTP response + update RTC
  checkPeriodicNTPSync();   // 6-hour re-sync

  // ── Clock timers ───────────────────────────────────────
  static bool          colonState       = false;
  static unsigned long lastColonToggle  = 0;
  static unsigned long lastBrightUpdate = 0;
  static unsigned long lastDisplayUpdate= 0;
  unsigned long now = millis();

  // Smooth brightness from LDR, respecting user-set min/max
  if (now - lastBrightUpdate >= 50) {
    lastBrightUpdate = now;
    int ldr    = analogRead(LDR_PIN);                         // 0–1023
    int target = map(ldr, 0, 1023, cfg.maxBr, cfg.minBr);   // bright → dim
    target     = constrain(target, (int)cfg.minBr, (int)cfg.maxBr);
    if (abs(target - currentBrightness) > 1) {
      currentBrightness += (target > currentBrightness) ? 1 : -1;
      pStrip->setBrightness(currentBrightness);
    }
  }

  // Blink colon every 1 s
  if (now - lastColonToggle >= 1000) {
    lastColonToggle = now;
    colonState = !colonState;
  }

  // Refresh display every 500 ms
  if (now - lastDisplayUpdate >= 500) {
    lastDisplayUpdate = now;
    DateTime t = rtc.now();
    displayTime(t.hour(), t.minute(), colonState);
  }
}
