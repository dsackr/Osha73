/***************************************************************************************
 * ESP32-C6 + 7.3" 6-color E-Paper (800x480)
 * VERSION: LittleFS - HTML files stored in flash
 *
 * SETUP INSTRUCTIONS:
 * 1. Install "ESP32 Sketch Data Upload" plugin for Arduino IDE 2.x
 * 2. Create a folder called "data" next to this .ino file
 * 3. Place ap.html and ui.html in the data folder
 * 4. Upload sketch
 * 5. Use Tools -> ESP32 Sketch Data Upload to upload the HTML files
 *
 * CHANGES IN THIS VERSION:
 * - Add /wifi page to reconfigure WiFi in BOTH STA + AP mode
 * - Add /wifi/clear to forget saved WiFi and reboot into AP mode
 * - Log mode + IP address to Serial on boot
 ***************************************************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <FS.h>
#include <SD.h>
#include <LittleFS.h>
#include <esp_sleep.h>
#include <ctype.h>

// ================= Pins =================
#define BUSY_Pin  3
#define RES_Pin   4
#define DC_Pin    1
#define CS_Pin    16
#define SCK_Pin   10
#define SDI_Pin   2

#define NEOPIXEL_PIN 23
#define STATUS_LED_PIN 8  // SparkFun ESP32-C6 status LED (if present)

const int SD_CS_PIN = -1;

// ================= Display =================
#define EPD_WIDTH   800
#define EPD_HEIGHT  480
#define EPD_BUFFER_SIZE (EPD_WIDTH * EPD_HEIGHT / 2)
#define BYTES_PER_ROW (EPD_WIDTH / 2)

#define EPD_7IN3F_BLACK   0x0
#define EPD_7IN3F_WHITE   0x1
#define EPD_7IN3F_YELLOW  0x2
#define EPD_7IN3F_RED     0x3
#define EPD_7IN3F_BLUE    0x5
#define EPD_7IN3F_GREEN   0x6
#define EPD_7IN3F_CLEAN   0x7

#define PSR         0x00
#define PWRR        0x01
#define POFS        0x03
#define PON         0x04
#define BTST1       0x05
#define BTST2       0x06
#define BTST3       0x08
#define DTM         0x10
#define DRF         0x12
#define PLL         0x30
#define CDI         0x50
#define TCON        0x60
#define TRES        0x61
#define T_VDCS      0x84
#define PWS         0xE3

// ================= WiFi =================
#define AP_SSID "EPaper-Setup"
#define AP_PASSWORD "epaper123"
#define WIFI_CONNECT_TIMEOUT 15000
#define DNS_PORT 53

// ================= Session timing =================
static const uint32_t DEFAULT_WINDOW_MS = 10UL * 60UL * 1000UL;
static const uint32_t MAX_WINDOW_MS     = 60UL * 60UL * 1000UL;
static const uint32_t ONE_SHOT_DELAY_MS = 1500;

uint32_t sessionEndMs = 0;

// ================= MAX17048 =================
static const uint8_t I2C_SDA = 6;
static const uint8_t I2C_SCL = 7;
static const uint8_t MAX17048_ADDR = 0x36;
static const uint8_t REG_VCELL = 0x02;
static const uint8_t REG_SOC   = 0x04;
static const uint8_t REG_CRATE = 0x16;

bool fuelGaugeOk = false;

// ================= Low battery overlay =================
static const int LOW_BAT_OVERWRITE_PCT = 10;
static const int ICON_MARGIN = 6;
static const int ICON_W = 26;
static const int ICON_H = 12;

// ================= Globals =================
WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

bool isAPMode = false;
String currentSSID = "";
String currentIP = "";

bool sdMounted = false;
bool wifiPowerSave = true;
File galleryFile;
String galleryFileName;
size_t galleryBytesWritten = 0;

bool oshaEnabled = false;
String oshaToken = "";
String oshaBaseUrl = "https://api.incident.io/v2/incidents";



String pullUrl = "";
uint32_t sleepHours = 24;  // Default: wake once per day

volatile bool displayBusy = false;
static bool uploadInProgress = false;
static size_t imageBytesWritten = 0;

File currentImageFile;

// ================= Forward declarations =================
void SPI_Write(unsigned char value);
void Epaper_Write_Command(unsigned char command);
void ensureGalleryDir();
String sanitizeGalleryFileName(const String &name);
void handleImagesUploadStream();
void handleImagesUploadDone();
void handleImagesList();
void handleImagesThumb();
void handleImagesDelete();
void handleDisplayShow();
void handleOshaRefresh();

void Epaper_Write_Data(unsigned char data);
void Epaper_READBUSY(void);
void EPD_Init(void);
void EPD_DeepSleep(void);
void displayTextScreen(const char* l1, const char* l2, const char* l3, const char* l4);
void displayAPInfo(void);
void displayConnectedInfo(void);
void setupWiFi(void);
void setupWebServer(void);

bool max17048Read16(uint8_t reg, uint16_t &out);
float batteryVoltage();
float batterySOC();
float batteryCRatePctPerHour();
String batteryStateString(float cratePctPerHr);

bool shouldOverwriteLowBattery();
uint8_t applyLowBatteryOverlayNibble(int x, int y, uint8_t nibble);

String sanitizeFileName(const String &name);
void maybeOpenSDForSave();
bool fetchAndDisplayOneShot();
void shutdownForever();

// Web handlers
void handleRoot();
void handleUi();
void handleSaveWiFi();
void handleClearWiFi();
void handleWifiPage();
void handleStatus();
void handleSession();
void handleExtend();
void handlePullUrl();
void handleSleepConfig();
void handleShutdown();
void handleUploadStream();
void handleUploadDone();

// ================= SPI =================
void SPI_Write(unsigned char value) {
  for (int i = 0; i < 8; i++) {
    digitalWrite(SCK_Pin, LOW);
    digitalWrite(SDI_Pin, (value & 0x80) ? HIGH : LOW);
    value <<= 1;
    delayMicroseconds(2);
    digitalWrite(SCK_Pin, HIGH);
    delayMicroseconds(2);
  }
}

void Epaper_Write_Command(unsigned char command) {
  digitalWrite(CS_Pin, LOW);
  digitalWrite(DC_Pin, LOW);
  SPI_Write(command);
  digitalWrite(CS_Pin, HIGH);
}

void Epaper_Write_Data(unsigned char data) {
  digitalWrite(CS_Pin, LOW);
  digitalWrite(DC_Pin, HIGH);
  SPI_Write(data);
  digitalWrite(CS_Pin, HIGH);
}

void Epaper_READBUSY(void) {
  unsigned long start = millis();
  while (!digitalRead(BUSY_Pin)) {
    delay(50);
    if (millis() - start > 30000) {
      Serial.println("BUSY TIMEOUT");
      return;
    }
  }
}

// ================= EPD Init =================
void EPD_Init(void) {
  digitalWrite(RES_Pin, HIGH); delay(50);
  digitalWrite(RES_Pin, LOW);  delay(50);
  digitalWrite(RES_Pin, HIGH); delay(200);

  Epaper_Write_Command(0xAA);
  Epaper_Write_Data(0x49); Epaper_Write_Data(0x55); Epaper_Write_Data(0x20);
  Epaper_Write_Data(0x08); Epaper_Write_Data(0x09); Epaper_Write_Data(0x18);

  Epaper_Write_Command(PWRR); Epaper_Write_Data(0x3F);
  Epaper_Write_Command(PSR);  Epaper_Write_Data(0x5F); Epaper_Write_Data(0x69);

  Epaper_Write_Command(POFS);
  Epaper_Write_Data(0x00); Epaper_Write_Data(0x54);
  Epaper_Write_Data(0x00); Epaper_Write_Data(0x44);

  Epaper_Write_Command(BTST1);
  Epaper_Write_Data(0x40); Epaper_Write_Data(0x1F);
  Epaper_Write_Data(0x1F); Epaper_Write_Data(0x2C);

  Epaper_Write_Command(BTST2);
  Epaper_Write_Data(0x6F); Epaper_Write_Data(0x1F);
  Epaper_Write_Data(0x17); Epaper_Write_Data(0x49);

  Epaper_Write_Command(BTST3);
  Epaper_Write_Data(0x6F); Epaper_Write_Data(0x1F);
  Epaper_Write_Data(0x1F); Epaper_Write_Data(0x22);

  Epaper_Write_Command(PLL); Epaper_Write_Data(0x08);
  Epaper_Write_Command(CDI); Epaper_Write_Data(0x3F);

  Epaper_Write_Command(TCON);
  Epaper_Write_Data(0x02); Epaper_Write_Data(0x00);

  Epaper_Write_Command(TRES);
  Epaper_Write_Data(0x03); Epaper_Write_Data(0x20);
  Epaper_Write_Data(0x01); Epaper_Write_Data(0xE0);

  Epaper_Write_Command(T_VDCS); Epaper_Write_Data(0x01);
  Epaper_Write_Command(PWS);    Epaper_Write_Data(0x2F);

  Epaper_Write_Command(PON);
  Epaper_READBUSY();
}

void EPD_DeepSleep(void) {
  Epaper_Write_Command(0x02);
  Epaper_Write_Data(0x00);
  Epaper_READBUSY();
}

// ================= Text renderer =================
const unsigned char font5x7[][5] PROGMEM = {
  {0,0,0,0,0}, {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
  {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41},
  {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
  {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
  {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
  {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E},
  {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
  {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
  {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
  {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46},
  {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
  {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, {0x36,0x49,0x49,0x49,0x36},
  {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00}, {0x14,0x14,0x14,0x14,0x14},
  {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02}
};

int getCharIndex(char c) {
  if (c == ' ') return 0;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
  if (c >= 'a' && c <= 'z') return c - 'a' + 1;
  if (c >= '0' && c <= '9') return c - '0' + 27;
  if (c == ':') return 37;
  if (c == '-') return 38;
  if (c == '.') return 39;
  if (c == '/') return 40;
  return 0;
}

void displayTextScreen(const char* l1, const char* l2, const char* l3, const char* l4) {
  EPD_Init();
  Epaper_Write_Command(DTM);

  int lineY[] = {160, 200, 240, 280};
  const char* lines[] = {l1, l2, l3, l4};

  for (int y = 0; y < EPD_HEIGHT; y++) {
    for (int x = 0; x < EPD_WIDTH / 2; x++) {
      unsigned char pixelPair = 0x11;

      for (int lineNum = 0; lineNum < 4; lineNum++) {
        if (!lines[lineNum]) continue;

        int lineStartY = lineY[lineNum];
        if (y < lineStartY || y >= lineStartY + 14) continue;

        int textLen = strlen(lines[lineNum]);
        int textWidthPixels = textLen * 12;
        int startX = (EPD_WIDTH - textWidthPixels) / 2;

        int px1 = x * 2;
        int px2 = x * 2 + 1;

        unsigned char c1 = EPD_7IN3F_WHITE;
        unsigned char c2 = EPD_7IN3F_WHITE;

        auto drawPixel = [&](int px, unsigned char &out) {
          if (px < startX || px >= startX + textWidthPixels) return;
          int charIdx = (px - startX) / 12;
          int charCol = ((px - startX) % 12) / 2;
          int charRow = (y - lineStartY) / 2;
          if (charIdx >= textLen || charCol >= 5 || charRow >= 7) return;
          int fontIdx = getCharIndex(lines[lineNum][charIdx]);
          unsigned char col = pgm_read_byte(&font5x7[fontIdx][charCol]);
          if (col & (1 << charRow)) out = EPD_7IN3F_BLACK;
        };

        drawPixel(px1, c1);
        drawPixel(px2, c2);

        pixelPair = (c1 << 4) | c2;
        break;
      }

      Epaper_Write_Data(pixelPair);
    }
  }

  Epaper_Write_Command(DRF);
  Epaper_Write_Data(0x00);
  Epaper_READBUSY();
  EPD_DeepSleep();
}

void displayAPInfo(void) {
  displayTextScreen("AP MODE", "SSID: " AP_SSID, "OPEN: 192.168.4.1", "SET WIFI IN BROWSER");
}

void displayConnectedInfo(void) {
  char ipLine[32];
  snprintf(ipLine, sizeof(ipLine), "IP: %s", currentIP.c_str());
  displayTextScreen("CONNECTED", currentSSID.c_str(), ipLine, "OPEN /ui");
}

// ================= MAX17048 =================
bool max17048Read16(uint8_t reg, uint16_t &out) {
  Wire.beginTransmission(MAX17048_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)MAX17048_ADDR, 2) != 2) return false;
  uint8_t msb = Wire.read();
  uint8_t lsb = Wire.read();
  out = ((uint16_t)msb << 8) | lsb;
  return true;
}

float batteryVoltage() {
  if (!fuelGaugeOk) return 0.0f;
  uint16_t raw = 0;
  if (!max17048Read16(REG_VCELL, raw)) return 0.0f;
  return (float)raw * 78.125f / 1000000.0f;
}

float batterySOC() {
  if (!fuelGaugeOk) return 0.0f;
  uint16_t raw = 0;
  if (!max17048Read16(REG_SOC, raw)) return 0.0f;
  return (float)raw / 256.0f;
}

float batteryCRatePctPerHour() {
  if (!fuelGaugeOk) return 0.0f;
  uint16_t rawU = 0;
  if (!max17048Read16(REG_CRATE, rawU)) return 0.0f;
  int16_t raw = (int16_t)rawU;
  return (float)raw * 0.208f;
}

String batteryStateString(float cratePctPerHr) {
  if (!fuelGaugeOk) return "unknown";
  if (cratePctPerHr > 0.8f) return "charging";
  if (cratePctPerHr < -0.8f) return "discharging";
  return "idle";
}

// ================= Low battery icon =================
bool shouldOverwriteLowBattery() {
  if (!fuelGaugeOk) return false;
  int pct = (int)(batterySOC() + 0.5f);
  return pct <= LOW_BAT_OVERWRITE_PCT;
}

static inline bool iconPixelOn(int lx, int ly) {
  if (lx >= 0 && lx <= 21 && (ly == 0 || ly == ICON_H - 1)) return true;
  if ((lx == 0 || lx == 21) && ly >= 0 && ly < ICON_H) return true;
  if (lx >= 22 && lx <= 25 && (ly == 4 || ly == 7)) return true;
  if (lx == 25 && ly >= 4 && ly <= 7) return true;
  if (lx == 10 && ly >= 3 && ly <= 8) return true;
  if (lx == 10 && ly == 10) return true;
  return false;
}

uint8_t applyLowBatteryOverlayNibble(int x, int y, uint8_t nibble) {
  if (!shouldOverwriteLowBattery()) return nibble;

  int x0 = EPD_WIDTH - ICON_MARGIN - ICON_W;
  int y0 = ICON_MARGIN;

  int lx = x - x0;
  int ly = y - y0;

  if (lx < 0 || ly < 0 || lx >= ICON_W || ly >= ICON_H) return nibble;
  if (iconPixelOn(lx, ly)) return EPD_7IN3F_RED;
  return nibble;
}

// ================= SD helpers =================
String sanitizeFileName(const String &name) {
  String clean = "";
  for (size_t i = 0; i < name.length(); i++) {
    char c = name.charAt(i);
    if (isalnum(c) || c == '_' || c == '-') clean += c;
  }
  if (clean.length() == 0) return clean;
  if (!clean.endsWith(".bin")) clean += ".bin";
  return clean;
}

void maybeOpenSDForSave() {
  if (!sdMounted) return;
  if (!server.hasArg("save")) return;

  String requested = sanitizeFileName(server.arg("save"));
  if (requested.length() == 0) return;

  if (currentImageFile) currentImageFile.close();
  currentImageFile = SD.open(("/" + requested).c_str(), FILE_WRITE);
}

// ================= WiFi setup =================
void setupWiFi(void) {
  preferences.begin("epaper", false);
  String savedSSID = preferences.getString("ssid", "");
  String savedPass = preferences.getString("password", "");

  pullUrl  = preferences.getString("pullurl", "");
  wifiPowerSave = preferences.getBool("wps", true);
  sleepHours = preferences.getUInt("sleepHours", 24);
  preferences.end();

  if (savedSSID.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());

    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < WIFI_CONNECT_TIMEOUT) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      currentSSID = savedSSID;
      currentIP = WiFi.localIP().toString();
      isAPMode = false;

      WiFi.setSleep(wifiPowerSave);
      sessionEndMs = millis() + DEFAULT_WINDOW_MS;

      // Log IP on boot
      Serial.printf("BOOT: STA connected to '%s' | IP: %s\n",
                    currentSSID.c_str(), currentIP.c_str());

      // Don't display info screen - just connect silently
      return;
    }
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  currentSSID = AP_SSID;
  currentIP = WiFi.softAPIP().toString();
  isAPMode = true;

  // Log IP on boot
  Serial.printf("BOOT: AP mode '%s' | IP: %s\n",
                currentSSID.c_str(), currentIP.c_str());

  displayAPInfo();
}

// ================= Web handlers =================
void handleRoot() {
  if (isAPMode) {
    // Try to serve from LittleFS first
    if (LittleFS.exists("/ap.html")) {
      File file = LittleFS.open("/ap.html", "r");
      server.streamFile(file, "text/html");
      file.close();
      return;
    }
    // Fallback minimal HTML
    server.send(200, "text/html",
      "<html><body><h1>AP Mode</h1>"
      "<p><a href='/wifi'>WiFi settings</a></p>"
      "<form action='/save' method='POST'>"
      "<label>SSID:</label><input name='ssid' required><br>"
      "<label>Password:</label><input type='password' name='password'><br>"
      "<input type='submit' value='Connect'>"
      "</form></body></html>");
    return;
  }

  // Try to serve from LittleFS
  if (LittleFS.exists("/ui.html")) {
    File file = LittleFS.open("/ui.html", "r");
    server.streamFile(file, "text/html");
    file.close();
    return;
  }

  // Fallback
  server.send(200, "text/html",
    "<html><body><h1>E-Paper</h1>"
    "<p>Upload via POST to /display/upload</p>"
    "<p><a href='/status'>Status JSON</a></p>"
    "<p><a href='/wifi'>WiFi settings</a></p>"
    "</body></html>");
}

void handleUi() {
  handleRoot(); // Same as root for now
}

// Save WiFi creds (works in AP or STA mode)
void handleSaveWiFi() {
  String ssid = server.arg("ssid");
  String pass = server.arg("password");
  if (ssid.length() == 0) { server.send(400, "text/html", "SSID required"); return; }

  preferences.begin("epaper", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", pass);
  preferences.end();

  server.send(200, "text/html",
    "<html><body><h3>Saved.</h3><p>Restarting...</p></body></html>");
  delay(800);
  ESP.restart();
}

// Clear saved WiFi creds (forces AP mode next boot)
void handleClearWiFi() {
  preferences.begin("epaper", false);
  preferences.remove("ssid");
  preferences.remove("password");
  preferences.end();

  server.send(200, "text/html",
    "<html><body><h3>WiFi cleared.</h3><p>Restarting into AP mode...</p></body></html>");
  delay(800);
  ESP.restart();
}

// WiFi config page (available in both STA and AP mode)
void handleWifiPage() {
  // Optional: if you later add /wifi.html to LittleFS, it will be used.
  if (LittleFS.exists("/wifi.html")) {
    File file = LittleFS.open("/wifi.html", "r");
    server.streamFile(file, "text/html");
    file.close();
    return;
  }

  String modeStr = isAPMode ? "AP" : "STA";
  String html =
    "<html><body style='font-family: sans-serif; max-width: 520px;'>"
    "<h2>WiFi Settings</h2>"
    "<p><b>Mode:</b> " + modeStr + "<br>"
    "<b>SSID:</b> " + currentSSID + "<br>"
    "<b>IP:</b> " + currentIP + "</p>"
    "<hr>"
    "<form action='/wifi' method='POST'>"
    "<label>SSID</label><br><input name='ssid' required style='width: 100%;'><br><br>"
    "<label>Password</label><br><input type='password' name='password' style='width: 100%;'><br><br>"
    "<button type='submit'>Save & Restart</button>"
    "</form>"
    "<hr>"
    "<form action='/wifi/clear' method='POST' onsubmit='return confirm(\"Clear saved WiFi and reboot into AP mode?\")'>"
    "<button type='submit' style='color: #b00;'>Forget WiFi (AP mode)</button>"
    "</form>"
    "<p><a href='/'>Back</a></p>"
    "</body></html>";

  server.send(200, "text/html", html);
}

void handleStatus() {
  float v = batteryVoltage();
  float soc = batterySOC();
  float crate = batteryCRatePctPerHour();
  String state = batteryStateString(crate);

  int32_t msLeft = 0;
  if (!isAPMode) {
    msLeft = (int32_t)(sessionEndMs - millis());
    if (msLeft < 0) msLeft = 0;
  }

  String json = "{";
  json += "\"ip\":\"" + currentIP + "\",";
  json += "\"ssid\":\"" + currentSSID + "\",";
  json += "\"battery_voltage\":" + String(v, 3) + ",";
  json += "\"battery_percent\":" + String((int)(soc + 0.5f)) + ",";
  json += "\"battery_crate_pct_per_hr\":" + String(crate, 2) + ",";
  json += "\"battery_state\":\"" + state + "\",";
  json += "\"wifi_powersave\":" + String(wifiPowerSave ? "true" : "false") + ",";
  json += "\"pull_url\":\"" + pullUrl + "\",";
  json += "\"sleep_hours\":" + String(sleepHours) + ",";
  json += "\"ms_left\":" + String(msLeft) + ",";
  json += "\"busy\":" + String(displayBusy ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleSession() {
  if (isAPMode) { server.send(200, "application/json", "{\"ms_left\":0,\"ap_mode\":true}"); return; }
  int32_t msLeft = (int32_t)(sessionEndMs - millis());
  if (msLeft < 0) msLeft = 0;
  server.send(200, "application/json", String("{\"ms_left\":") + String(msLeft) + "}");
}

void handleExtend() {
  if (isAPMode) { server.send(200, "application/json", "{\"status\":\"ok\",\"ap_mode\":true}"); return; }
  int addMin = server.hasArg("minutes") ? server.arg("minutes").toInt() : 5;
  addMin = constrain(addMin, 1, 30);

  uint32_t now = millis();
  uint32_t newEnd = max(sessionEndMs, now) + (uint32_t)addMin * 60UL * 1000UL;
  uint32_t maxEnd = now + MAX_WINDOW_MS;
  sessionEndMs = min(newEnd, maxEnd);

  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handlePullUrl() {
  if (!server.hasArg("url")) { server.send(400, "application/json", "{\"error\":\"missing url\"}"); return; }
  pullUrl = server.arg("url");
  preferences.begin("epaper", false);
  preferences.putString("pullurl", pullUrl);
  preferences.end();
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleSleepConfig() {
  if (!server.hasArg("hours")) { server.send(400, "application/json", "{\"error\":\"missing hours\"}"); return; }

  int hours = server.arg("hours").toInt();
  if (hours < 1 || hours > 168) { // 1 hour to 1 week
    server.send(400, "application/json", "{\"error\":\"hours must be between 1 and 168\"}");
    return;
  }

  sleepHours = hours;
  preferences.begin("epaper", false);
  preferences.putUInt("sleepHours", sleepHours);
  preferences.end();
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleShutdown() {
  server.send(200, "text/plain", "Shutting down");
  delay(150);
  shutdownForever();
}

// ================= Binary upload =================
void handleUploadStream() {
  HTTPUpload &up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    if (displayBusy) return;

    displayBusy = true;
    uploadInProgress = true;
    imageBytesWritten = 0;

    maybeOpenSDForSave();

    EPD_Init();
    Epaper_Write_Command(DTM);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!uploadInProgress) return;

    const uint8_t *buf = up.buf;
    size_t n = up.currentSize;

    for (size_t i = 0; i < n && imageBytesWritten < EPD_BUFFER_SIZE; i++) {
      uint8_t byteIn = buf[i];

      int byteIndex = (int)imageBytesWritten;
      int y = byteIndex / BYTES_PER_ROW;
      int xb = byteIndex % BYTES_PER_ROW;
      int x1 = xb * 2;
      int x2 = x1 + 1;

      uint8_t hi = (byteIn >> 4) & 0x0F;
      uint8_t lo = (byteIn >> 0) & 0x0F;

      hi = applyLowBatteryOverlayNibble(x1, y, hi);
      lo = applyLowBatteryOverlayNibble(x2, y, lo);

      uint8_t byteOut = (hi << 4) | lo;

      Epaper_Write_Data(byteOut);
      if (currentImageFile) currentImageFile.write(byteOut);

      imageBytesWritten++;
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    uploadInProgress = false;
    displayBusy = false;
    if (currentImageFile) currentImageFile.close();
  }
}

void handleUploadDone() {
  if (!uploadInProgress) { server.send(500, "application/json", "{\"error\":\"no upload\"}"); return; }

  while (imageBytesWritten < EPD_BUFFER_SIZE) {
    Epaper_Write_Data(0x11);
    if (currentImageFile) currentImageFile.write(0x11);
    imageBytesWritten++;
  }

  Epaper_Write_Command(DRF);
  Epaper_Write_Data(0x00);
  Epaper_READBUSY();
  EPD_DeepSleep();

  if (currentImageFile) currentImageFile.close();

  uploadInProgress = false;
  displayBusy = false;

  server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Display updated\"}");
}

// ================= One-shot pull =================
bool fetchAndDisplayOneShot() {
  if (pullUrl.length() == 0) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setTimeout(9000);
  if (!http.begin(pullUrl)) return false;

  // Don't use ETag - always pull to update daily counter
  int code = http.GET();
  if (code == HTTP_CODE_NOT_MODIFIED || code == 204) { http.end(); return false; }
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  WiFiClient *stream = http.getStreamPtr();

  displayBusy = true;
  EPD_Init();
  Epaper_Write_Command(DTM);

  uint8_t buf[1024];
  size_t written = 0;

  while (http.connected() && written < EPD_BUFFER_SIZE) {
    int avail = stream->available();
    if (avail <= 0) { delay(2); continue; }

    int n = stream->readBytes(buf, (size_t)min(avail, (int)sizeof(buf)));
    for (int i = 0; i < n && written < EPD_BUFFER_SIZE; i++) {
      uint8_t byteIn = buf[i];

      int byteIndex = (int)written;
      int y = byteIndex / BYTES_PER_ROW;
      int xb = byteIndex % BYTES_PER_ROW;
      int x1 = xb * 2;
      int x2 = x1 + 1;

      uint8_t hi = (byteIn >> 4) & 0x0F;
      uint8_t lo = (byteIn >> 0) & 0x0F;

      hi = applyLowBatteryOverlayNibble(x1, y, hi);
      lo = applyLowBatteryOverlayNibble(x2, y, lo);

      uint8_t byteOut = (hi << 4) | lo;

      Epaper_Write_Data(byteOut);
      written++;
    }
  }

  while (written < EPD_BUFFER_SIZE) {
    Epaper_Write_Data(0x11);
    written++;
  }

  Epaper_Write_Command(DRF);
  Epaper_Write_Data(0x00);
  Epaper_READBUSY();
  EPD_DeepSleep();

  displayBusy = false;
  http.end();
  return true;
}

// ================= Web server setup =================
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/ui", HTTP_GET, handleUi);

  // Legacy AP save endpoint still supported
  server.on("/save", HTTP_POST, handleSaveWiFi);

  // New WiFi endpoints (work in both STA and AP mode)
  server.on("/wifi", HTTP_GET, handleWifiPage);
  server.on("/wifi", HTTP_POST, handleSaveWiFi);
  server.on("/wifi/clear", HTTP_POST, handleClearWiFi);

  server.on("/status", HTTP_GET, handleStatus);
  server.on("/session", HTTP_GET, handleSession);
  server.on("/extend", HTTP_POST, handleExtend);
  server.on("/pullurl", HTTP_POST, handlePullUrl);
  server.on("/sleepconfig", HTTP_POST, handleSleepConfig);
  server.on("/shutdown", HTTP_GET, handleShutdown);
  server.on("/display/upload", HTTP_POST, handleUploadDone, handleUploadStream);

  server.onNotFound([]() {
    server.sendHeader("Location", "/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
}

// ================= Shutdown =================
void shutdownForever() {
  server.stop();
  dnsServer.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Aggressively turn off all LEDs
  pinMode(NEOPIXEL_PIN, OUTPUT);
  digitalWrite(NEOPIXEL_PIN, LOW);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  // Disable pull-ups/pull-downs on LED pins
  gpio_reset_pin((gpio_num_t)NEOPIXEL_PIN);
  gpio_reset_pin((gpio_num_t)STATUS_LED_PIN);

  // Wake up after configured sleep duration (default 24 hours)
  uint64_t sleepTimeMicros = (uint64_t)sleepHours * 60ULL * 60ULL * 1000000ULL;
  esp_sleep_enable_timer_wakeup(sleepTimeMicros);

  Serial.printf("Going to sleep for %u hours...\n", sleepHours);
  delay(100); // Give serial time to send

  esp_deep_sleep_start();
}

// ================= Setup / Loop =================
void setup() {
  Serial.begin(115200);
  delay(300);

  // Initialize LittleFS for HTML files
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed - will use fallback HTML");
  } else {
    Serial.println("LittleFS mounted successfully");
  }

  sdMounted = (SD_CS_PIN >= 0) ? SD.begin(SD_CS_PIN) : SD.begin();

  pinMode(NEOPIXEL_PIN, OUTPUT);
  digitalWrite(NEOPIXEL_PIN, LOW);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  pinMode(BUSY_Pin, INPUT_PULLUP);
  pinMode(RES_Pin, OUTPUT);
  pinMode(DC_Pin, OUTPUT);
  pinMode(CS_Pin, OUTPUT);
  pinMode(SCK_Pin, OUTPUT);
  pinMode(SDI_Pin, OUTPUT);

  digitalWrite(CS_Pin, HIGH);
  digitalWrite(DC_Pin, LOW);
  digitalWrite(SCK_Pin, LOW);
  digitalWrite(SDI_Pin, LOW);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  uint16_t tmp = 0;
  fuelGaugeOk = max17048Read16(REG_VCELL, tmp);

  setupWiFi();
  setupWebServer();

  if (!isAPMode) {
    delay(ONE_SHOT_DELAY_MS);
    (void)fetchAndDisplayOneShot();
  }
}

void loop() {
  if (isAPMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(5);
    return;
  }

  server.handleClient();

  if (!displayBusy && !uploadInProgress) {
    int32_t msLeft = (int32_t)(sessionEndMs - millis());
    if (msLeft <= 0) shutdownForever();
  }
  delay(5);
}
