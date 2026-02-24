/*/***************************************************************************************
 * ESP32-C6 + 7.3" 6-color E-Paper (800x480)
 * VERSION: SD-backed gallery and OSHA counter, LittleFS fallback for UI
 *
 * SETUP INSTRUCTIONS:
 * 1. Insert a FAT32-formatted micro SD card and ensure it has at least a few megabytes of free space.
 * 2. On first boot with Wi-Fi configured, the firmware will automatically download the default UI (ui.html and ap.html) and background assets to the SD card if they are missing.
 * 3. Optionally, you can still use the “ESP32 Sketch Data Upload” plugin to upload ui.html and ap.html into LittleFS by placing them in a `data` folder next to this .ino file and running the upload tool.
 * 4. Upload the sketch using a partition scheme with at least 2 MB of flash for the application (e.g. “No OTA” or “Huge APP”) to accommodate the firmware size.
 *
 * FEATURES IN THIS VERSION:
 * - On-device OSHA incident mode with secure NVS storage of the API token and SD-backed configuration.
 * - SD-backed image gallery with client-side thumbnail generation and asynchronous display refresh.
 * - Automatic SD card setup for /gallery and /osha directories, background image storage, and layout
 * - Wi-Fi STA + AP configuration pages with reconnect/clear options, deep sleep scheduling, and battery overlay.
  ***************************************************************************************/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SD.h>
#include <LittleFS.h>
#include <esp_sleep.h>
#include <ctype.h>
#include <stdarg.h>
#include <vector>
#include <algorithm>
#include <time.h>

#define DEVICE_LOG_PATH "/osha/device.log"

// ================= Pins =================
#define BUSY_Pin  3
#define RES_Pin   4
#define DC_Pin    1
#define CS_Pin    16
#define SCK_Pin   10
#define SDI_Pin   2

#define NEOPIXEL_PIN 23
#define STATUS_LED_PIN 8  // SparkFun ESP32-C6 status LED (if present)

const int SD_CS_PIN = 18;  // SparkFun ESP32-C6 Thing Plus micro-SD CS pin

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
bool sdSetupRequired = false;
bool wifiPowerSave = true;
File galleryFile;
String galleryFileName;
size_t galleryBytesWritten = 0;

bool oshaEnabled = false;
String oshaToken = "";
String oshaBaseUrl = "https://api.incident.io/v2/incidents";

int oshaDays = 0;
int oshaPrior = 0;
String oshaIncident = "";
String oshaDate = "";
String oshaReason = "";

volatile bool pendingDisplayRefresh = false;

struct OshaState {
  int days;
  int prior;
  String incident;
  String date;
  String reason;
};

struct OshaLayout {
  String backgroundPath;
  int daysX;
  int daysY;
  int daysScale;
  int priorX;
  int priorY;
  int priorScale;
  int incidentX;
  int incidentY;
  int incidentScale;
  int dateX;
  int dateY;
  int dateScale;
  int deployBoxX;
  int deployBoxY;
  int changeBoxX;
  int changeBoxY;
  int missedBoxX;
  int missedBoxY;
  int boxSize;
};

OshaState currentOshaState = {0,0,"","",""};



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
bool ensureDirectory(const char *path);
bool sdCardNeedsSetup();
bool downloadFileToSd(const char *url, const char *destPath);
bool configureSdCardDefaults(String &errorOut);
void logSdSetup(const char *fmt, ...);
void logWebRequest(const char *handlerName);
void handleSdSetupPage();
void handleSdSetupRun();
String sanitizeGalleryFileName(const String &name);
void handleImagesUploadStream();
void handleImagesUploadDone();
void handleImagesList();
void handleImagesThumb();
void handleImagesRaw();
void handleImagesDelete();
void handleDisplayShow();
void handleOshaRefresh();
void handleOshaConfig();
void handleLogs();
void handleSdList();
void handleSdUploadStream();
void handleSdUploadDone();
void handleSdDelete();
void handleSdMkdir();
void handleUiRefresh();
bool refreshOshaAndMaybeDisplay(bool forceDisplay);
bool renderOshaDisplay(const OshaState &state);
void processPendingDisplayRefresh();
void appendDeviceLog(const char *fmt, ...);
bool sanitizeSdPath(const String &input, String &outPath);
bool removeSdPathRecursive(const String &path);
bool saveIncidentDebugFile(const String &name, const String &content);
bool refreshDefaultUiFromGithub(String &errorOut);
bool readHttpBodyWithTimeout(HTTPClient &http, String &bodyOut, unsigned long stallTimeoutMs);

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
void writeDisplayRefreshSequence();
void setOshaModeEnabled(bool enabled);
void displayOshaSetupMessage();
bool ensureOshaConfigJson();
bool loadOshaLayout(OshaLayout &layout);
bool loadOshaStateFromSd(OshaState &state);
bool saveOshaStateToSd(const OshaState &state);
bool fetchOshaState(OshaState &stateOut);
String normalizeOshaCategory(const String &raw);
bool parseIsoToEpoch(const String &iso, time_t &epochOut);
String epochToNyDate(time_t utcEpoch);
String nyTodayDate();
int daysBetweenDates(const String &fromDate, const String &toDate);

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
bool streamHtmlFromStorage(const char *path);

void logWebRequest(const char *handlerName) {
  String remote = server.client().remoteIP().toString();
  String uri = server.uri();
  const char *method = "UNKNOWN";
  switch (server.method()) {
    case HTTP_GET: method = "GET"; break;
    case HTTP_POST: method = "POST"; break;
    case HTTP_PUT: method = "PUT"; break;
    case HTTP_DELETE: method = "DELETE"; break;
    default: break;
  }
  Serial.printf("WEB: %s [%s %s] from %s\n", handlerName, method, uri.c_str(), remote.c_str());
}

void appendDeviceLog(const char *fmt, ...) {
  char message[384];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  Serial.printf("LOG: %s\n", message);
  if (!sdMounted) return;
  if (!SD.exists("/osha")) SD.mkdir("/osha");

  File f = SD.open(DEVICE_LOG_PATH, FILE_APPEND);
  if (!f) f = SD.open(DEVICE_LOG_PATH, FILE_WRITE);
  if (!f) return;

  unsigned long s = millis() / 1000UL;
  f.printf("[%lu] %s\n", s, message);
  f.close();
}

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

String sanitizeGalleryFileName(const String &name) {
  String clean = sanitizeFileName(name);
  if (clean.length() == 0) clean = "image" + String(millis()) + ".bin";
  return clean;
}

void ensureGalleryDir() {
  if (!sdMounted) return;
  if (!SD.exists("/gallery")) SD.mkdir("/gallery");
  if (!SD.exists("/gallery/.thumbs")) SD.mkdir("/gallery/.thumbs");
  if (!SD.exists("/osha")) SD.mkdir("/osha");
}

bool ensureDirectory(const char *path) {
  if (!sdMounted) return false;
  if (SD.exists(path)) return true;
  return SD.mkdir(path);
}

bool sdCardNeedsSetup() {
  if (!sdMounted) return true;

  if (!SD.exists("/gallery")) return true;
  if (!SD.exists("/gallery/.thumbs")) return true;
  if (!SD.exists("/osha")) return true;

  if (!SD.exists("/osha/background.bin")) return true;
  if (!SD.exists("/background.png")) return true;
  if (!SD.exists("/ui.html")) return true;
  if (!SD.exists("/ap.html")) return true;

  return false;
}

void logSdSetup(const char *fmt, ...) {
  char message[220];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  Serial.printf("SD_SETUP: %s\n", message);
}

bool downloadFileToSd(const char *url, const char *destPath) {
  logSdSetup("download start %s -> %s", url, destPath);
  HTTPClient http;
  http.setTimeout(12000);
  if (!http.begin(url)) {
    logSdSetup("download begin failed for %s", url);
    return false;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    logSdSetup("download http status %d for %s", code, url);
    http.end();
    return false;
  }

  int expectedLength = http.getSize();
  logSdSetup("download http 200 for %s (content-length=%d)", destPath, expectedLength);

  if (SD.exists(destPath)) SD.remove(destPath);
  File out = SD.open(destPath, FILE_WRITE);
  if (!out) {
    logSdSetup("download open failed for %s", destPath);
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t totalWritten = 0;
  unsigned long startedAt = millis();
  unsigned long lastProgressLog = startedAt;
  unsigned long lastDataAt = startedAt;
  const unsigned long STALL_TIMEOUT_MS = 15000;

  while (http.connected()) {
    if (expectedLength > 0 && (int)totalWritten >= expectedLength) {
      break;
    }

    int avail = stream->available();
    if (avail <= 0) {
      unsigned long now = millis();
      if (now - lastDataAt > STALL_TIMEOUT_MS) {
        logSdSetup("download stalled for %s after %lu ms (wrote=%u bytes)",
                   destPath, now - startedAt, (unsigned int)totalWritten);
        out.close();
        SD.remove(destPath);
        http.end();
        return false;
      }

      if (now - lastProgressLog > 3000) {
        logSdSetup("download waiting for data %s (wrote=%u bytes so far)",
                   destPath, (unsigned int)totalWritten);
        lastProgressLog = now;
      }

      if (!http.connected()) break;
      delay(2);
      continue;
    }

    int n = stream->readBytes(buf, (size_t)min(avail, (int)sizeof(buf)));
    if (n <= 0) break;
    out.write(buf, (size_t)n);
    totalWritten += (size_t)n;

    unsigned long now = millis();
    lastDataAt = now;
    if (now - lastProgressLog > 2000) {
      if (expectedLength > 0) {
        logSdSetup("download progress %s: %u/%d bytes", destPath,
                   (unsigned int)totalWritten, expectedLength);
      } else {
        logSdSetup("download progress %s: %u bytes", destPath, (unsigned int)totalWritten);
      }
      lastProgressLog = now;
    }
  }

  if (expectedLength > 0 && (int)totalWritten != expectedLength) {
    logSdSetup("download size mismatch for %s (expected=%d, got=%u)",
               destPath, expectedLength, (unsigned int)totalWritten);
    out.close();
    SD.remove(destPath);
    http.end();
    return false;
  }

  if (!out) {
    logSdSetup("download write failed for %s", destPath);
    out.close();
    SD.remove(destPath);
    http.end();
    return false;
  }

  out.close();
  http.end();
  logSdSetup("download complete %s (%u bytes in %lu ms)",
             destPath, (unsigned int)totalWritten, millis() - startedAt);
  return true;
}

bool refreshDefaultUiFromGithub(String &errorOut) {
  if (!sdMounted) {
    errorOut = "sd unavailable";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    errorOut = "wifi not connected";
    return false;
  }

  if (!downloadFileToSd("https://raw.githubusercontent.com/dsackr/OSHA-7.3eink/main/data/ui.html", "/ui.html")) {
    errorOut = "failed to download /ui.html";
    return false;
  }
  if (!downloadFileToSd("https://raw.githubusercontent.com/dsackr/OSHA-7.3eink/main/data/ap.html", "/ap.html")) {
    errorOut = "failed to download /ap.html";
    return false;
  }
  errorOut = "";
  return true;
}

bool readHttpBodyWithTimeout(HTTPClient &http, String &bodyOut, unsigned long stallTimeoutMs) {
  WiFiClient *stream = http.getStreamPtr();
  if (!stream) return false;

  bodyOut = "";
  int expectedLength = http.getSize();
  if (expectedLength > 0) bodyOut.reserve((size_t)expectedLength + 1);

  unsigned long lastDataAt = millis();
  uint8_t buf[1024];

  while (http.connected() || stream->available() > 0) {
    int avail = stream->available();
    if (avail <= 0) {
      if (millis() - lastDataAt > stallTimeoutMs) {
        return false;
      }
      delay(2);
      continue;
    }

    int n = stream->readBytes(buf, (size_t)min(avail, (int)sizeof(buf)));
    if (n <= 0) continue;

    bodyOut.concat((const char*)buf, (size_t)n);
    lastDataAt = millis();
  }

  if (expectedLength > 0 && bodyOut.length() != (size_t)expectedLength) {
    return false;
  }
  return true;
}

bool configureSdCardDefaults(String &errorOut) {
  logSdSetup("configure requested (wifi=%d, sdMounted=%d)",
             WiFi.status() == WL_CONNECTED ? 1 : 0,
             sdMounted ? 1 : 0);
  if (!sdMounted) {
    logSdSetup("configure aborted: sd unavailable");
    errorOut = "sd unavailable";
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    logSdSetup("configure aborted: wifi not connected");
    errorOut = "wifi not connected";
    return false;
  }

  logSdSetup("ensuring required directories");
  if (!ensureDirectory("/gallery")) { errorOut = "failed to create /gallery"; return false; }
  logSdSetup("directory ready: /gallery");
  if (!ensureDirectory("/gallery/.thumbs")) { errorOut = "failed to create /gallery/.thumbs"; return false; }
  logSdSetup("directory ready: /gallery/.thumbs");
  if (!ensureDirectory("/osha")) { errorOut = "failed to create /osha"; return false; }
  logSdSetup("directory ready: /osha");

  logSdSetup("starting required asset downloads");
  if (!downloadFileToSd("https://raw.githubusercontent.com/dsackr/OSHA-7.3eink/main/background.bin", "/osha/background.bin")) {
    errorOut = "failed to download /osha/background.bin";
    logSdSetup("configure failed: %s", errorOut.c_str());
    return false;
  }
  if (!downloadFileToSd("https://raw.githubusercontent.com/dsackr/OSHA-7.3eink/main/data/background.png", "/background.png")) {
    errorOut = "failed to download /background.png";
    logSdSetup("configure failed: %s", errorOut.c_str());
    return false;
  }
  if (!downloadFileToSd("https://raw.githubusercontent.com/dsackr/OSHA-7.3eink/main/data/ui.html", "/ui.html")) {
    errorOut = "failed to download /ui.html";
    logSdSetup("configure failed: %s", errorOut.c_str());
    return false;
  }
  if (!downloadFileToSd("https://raw.githubusercontent.com/dsackr/OSHA-7.3eink/main/data/ap.html", "/ap.html")) {
    errorOut = "failed to download /ap.html";
    logSdSetup("configure failed: %s", errorOut.c_str());
    return false;
  }

  sdSetupRequired = sdCardNeedsSetup();
  errorOut = "";
  logSdSetup("configure complete (sdSetupRequired=%d)", sdSetupRequired ? 1 : 0);
  return !sdSetupRequired;
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
  oshaEnabled = preferences.getBool("osha_enabled", false);
  oshaToken = preferences.getString("osha_token", "");
  oshaBaseUrl = preferences.getString("osha_url", "https://api.incident.io/v2/incidents");
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
  if (!isAPMode && sdSetupRequired) {
    server.sendHeader("Location", "/sd/setup", true);
    server.send(302, "text/plain", "");
    return;
  }

  if (isAPMode) {
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    if (streamHtmlFromStorage("/ap.html")) {
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

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
  if (streamHtmlFromStorage("/ui.html")) {
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

bool streamHtmlFromStorage(const char *path) {
  String requestPath(path);
  bool preferLittleFs = (requestPath == "/ui.html" || requestPath == "/ap.html");

  if (preferLittleFs && LittleFS.exists(path)) {
    File file = LittleFS.open(path, "r");
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
      return true;
    }
  }

  if (sdMounted && SD.exists(path)) {
    File file = SD.open(path, "r");
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
      return true;
    }
  }

  if (!preferLittleFs && LittleFS.exists(path)) {
    File file = LittleFS.open(path, "r");
    if (file) {
      server.streamFile(file, "text/html");
      file.close();
      return true;
    }
  }

  return false;
}

void handleUi() {
  if (!isAPMode && sdSetupRequired) {
    server.sendHeader("Location", "/sd/setup", true);
    server.send(302, "text/plain", "");
    return;
  }
  handleRoot(); // Same as root for now
}

void handleSdSetupPage() {
  logWebRequest("handleSdSetupPage");
  if (isAPMode) {
    server.send(400, "text/html", "<html><body><h3>SD setup requires WiFi STA mode.</h3></body></html>");
    return;
  }
  if (!sdMounted) {
    server.send(500, "text/html", "<html><body><h3>SD card not detected.</h3></body></html>");
    return;
  }

  String html =
    "<html><body style='font-family:sans-serif;max-width:640px;'>"
    "<h2>SD Card Setup Required</h2>"
    "<p>The SD card is mounted but missing one or more required files/folders.</p>"
    "<ul>"
    "<li>/gallery/</li>"
    "<li>/gallery/.thumbs/</li>"
    "<li>/osha/</li>"
    "<li>/osha/background.bin</li>"
    "<li>/background.png</li>"
    "<li>/ui.html</li>"
    "<li>/ap.html</li>"
    "</ul>"
    "<p>Click configure to create missing folders and download default files from GitHub.</p>"
    "<form action='/sd/setup/run' method='POST'>"
    "<button type='submit'>Configure SD Card</button>"
    "</form>"
    "</body></html>";

  server.send(200, "text/html", html);
}

void handleSdSetupRun() {
  logWebRequest("handleSdSetupRun");
  logSdSetup("HTTP /sd/setup/run invoked from %s", server.client().remoteIP().toString().c_str());
  String error;
  bool ok = configureSdCardDefaults(error);
  if (!ok) {
    logSdSetup("HTTP /sd/setup/run failed: %s", error.c_str());
    String json = String("{\"status\":\"error\",\"message\":\"") + error + "\"}";
    server.send(500, "application/json", json);
    return;
  }
  logSdSetup("HTTP /sd/setup/run succeeded");
  server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"sd configured\"}");
}

// Save WiFi creds (works in AP or STA mode)
void handleSaveWiFi() {
  logWebRequest("handleSaveWiFi");
  String ssid = server.arg("ssid");
  String pass = server.arg("password");
  if (ssid.length() == 0) { server.send(400, "text/html", "SSID required"); return; }

  preferences.begin("epaper", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", pass);
  preferences.end();
  appendDeviceLog("WiFi credentials updated for SSID: %s", ssid.c_str());

  server.send(200, "text/html",
    "<html><body><h3>Saved.</h3><p>Restarting...</p></body></html>");
  delay(800);
  ESP.restart();
}

// Clear saved WiFi creds (forces AP mode next boot)
void handleClearWiFi() {
  logWebRequest("handleClearWiFi");
  preferences.begin("epaper", false);
  preferences.remove("ssid");
  preferences.remove("password");
  preferences.end();
  appendDeviceLog("WiFi credentials cleared");

  server.send(200, "text/html",
    "<html><body><h3>WiFi cleared.</h3><p>Restarting into AP mode...</p></body></html>");
  delay(800);
  ESP.restart();
}

// WiFi config page (available in both STA and AP mode)
void handleWifiPage() {
  logWebRequest("handleWifiPage");
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

bool sanitizeSdPath(const String &input, String &outPath) {
  String p = input;
  p.trim();
  if (p.length() == 0) p = "/";
  if (!p.startsWith("/")) p = "/" + p;
  while (p.indexOf("//") >= 0) p.replace("//", "/");
  if (p.indexOf("..") >= 0) return false;
  outPath = p;
  return true;
}

bool removeSdPathRecursive(const String &path) {
  if (!SD.exists(path.c_str())) return true;
  File node = SD.open(path.c_str(), FILE_READ);
  if (!node) return false;
  bool isDir = node.isDirectory();
  node.close();
  if (!isDir) return SD.remove(path.c_str());

  File dir = SD.open(path.c_str(), FILE_READ);
  if (!dir) return false;
  while (true) {
    File child = dir.openNextFile();
    if (!child) break;
    String childPath = String(child.name());
    bool childIsDir = child.isDirectory();
    child.close();
    if (childIsDir) {
      if (!removeSdPathRecursive(childPath)) { dir.close(); return false; }
    } else {
      if (!SD.remove(childPath.c_str())) { dir.close(); return false; }
    }
  }
  dir.close();
  return SD.rmdir(path.c_str());
}

void handleLogs() {
  if (!sdMounted || !SD.exists(DEVICE_LOG_PATH)) {
    server.send(200, "text/plain", "No logs yet.\n");
    return;
  }
  File f = SD.open(DEVICE_LOG_PATH, FILE_READ);
  if (!f) { server.send(500, "text/plain", "Unable to open log file\n"); return; }
  server.streamFile(f, "text/plain");
  f.close();
}

void handleSdList() {
  if (!sdMounted) { server.send(500, "application/json", "{\"error\":\"sd unavailable\"}"); return; }
  String path;
  if (!sanitizeSdPath(server.hasArg("path") ? server.arg("path") : "/", path)) {
    server.send(400, "application/json", "{\"error\":\"invalid path\"}");
    return;
  }
  File dir = SD.open(path.c_str(), FILE_READ);
  if (!dir || !dir.isDirectory()) { server.send(404, "application/json", "{\"error\":\"not found\"}"); return; }
  DynamicJsonDocument d(16384);
  JsonArray arr = d.createNestedArray("entries");
  while (true) {
    File e = dir.openNextFile();
    if (!e) break;
    JsonObject item = arr.createNestedObject();
    String full = String(e.name());
    String name = full;
    int slash = full.lastIndexOf('/');
    if (slash >= 0) name = full.substring(slash + 1);
    item["name"] = name;
    item["path"] = full;
    item["is_dir"] = e.isDirectory();
    item["size"] = (uint32_t)e.size();
    e.close();
  }
  dir.close();
  d["path"] = path;
  String out;
  serializeJson(d, out);
  server.send(200, "application/json", out);
}

String sdUploadTargetPath = "";
File sdUploadFile;

void handleSdUploadStream() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (!sdMounted) return;
    String base;
    if (!sanitizeSdPath(server.hasArg("path") ? server.arg("path") : "/", base)) return;
    if (!base.endsWith("/")) base += "/";
    String name = sanitizeFileName(up.filename);
    sdUploadTargetPath = base + name;
    if (sdUploadFile) sdUploadFile.close();
    sdUploadFile = SD.open(sdUploadTargetPath.c_str(), FILE_WRITE);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (sdUploadFile) sdUploadFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (sdUploadFile) sdUploadFile.close();
  }
}

void handleSdUploadDone() {
  if (!sdMounted) { server.send(500, "application/json", "{\"error\":\"sd unavailable\"}"); return; }
  if (sdUploadTargetPath.length() == 0) { server.send(400, "application/json", "{\"error\":\"upload failed\"}"); return; }
  appendDeviceLog("SD upload: %s", sdUploadTargetPath.c_str());
  String out = String("{\"status\":\"ok\",\"path\":\"") + sdUploadTargetPath + "\"}";
  server.send(200, "application/json", out);
  sdUploadTargetPath = "";
}

void handleSdDelete() {
  if (!sdMounted || !server.hasArg("path")) { server.send(400, "application/json", "{\"error\":\"missing path\"}"); return; }
  String path;
  if (!sanitizeSdPath(server.arg("path"), path) || path == "/") { server.send(400, "application/json", "{\"error\":\"invalid path\"}"); return; }
  if (!SD.exists(path.c_str())) { server.send(404, "application/json", "{\"error\":\"not found\"}"); return; }
  bool ok = removeSdPathRecursive(path);
  if (!ok) { server.send(500, "application/json", "{\"error\":\"delete failed\"}"); return; }
  appendDeviceLog("SD delete: %s", path.c_str());
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleSdMkdir() {
  if (!sdMounted || !server.hasArg("path")) { server.send(400, "application/json", "{\"error\":\"missing path\"}"); return; }
  String path;
  if (!sanitizeSdPath(server.arg("path"), path) || path == "/") { server.send(400, "application/json", "{\"error\":\"invalid path\"}"); return; }
  if (SD.exists(path.c_str()) || SD.mkdir(path.c_str())) {
    appendDeviceLog("SD mkdir: %s", path.c_str());
    server.send(200, "application/json", "{\"status\":\"ok\"}");
    return;
  }
  server.send(500, "application/json", "{\"error\":\"mkdir failed\"}");
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
  json += "\"busy\":" + String(displayBusy ? "true" : "false") + ",";
  json += "\"sd_mounted\":" + String(sdMounted ? "true" : "false") + ",";
  json += "\"sd_setup_required\":" + String(sdSetupRequired ? "true" : "false") + ",";
  json += "\"osha_enabled\":" + String(oshaEnabled ? "true" : "false") + ",";
  json += "\"osha_token_configured\":" + String(oshaToken.length() > 0 ? "true" : "false") + ",";
  json += "\"osha_days\":" + String(oshaDays) + ",";
  json += "\"osha_prior\":" + String(oshaPrior) + ",";
  json += "\"osha_incident\":\"" + oshaIncident + "\",";
  json += "\"osha_date\":\"" + oshaDate + "\",";
  json += "\"osha_reason\":\"" + oshaReason + "\"";
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
  logWebRequest("handleExtend");
  if (isAPMode) { server.send(200, "application/json", "{\"status\":\"ok\",\"ap_mode\":true}"); return; }
  int addMin = server.hasArg("minutes") ? server.arg("minutes").toInt() : 5;
  addMin = constrain(addMin, 1, 30);

  uint32_t now = millis();
  uint32_t newEnd = max(sessionEndMs, now) + (uint32_t)addMin * 60UL * 1000UL;
  uint32_t maxEnd = now + MAX_WINDOW_MS;
  sessionEndMs = min(newEnd, maxEnd);
  appendDeviceLog("Session extended by %d minute(s)", addMin);

  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handlePullUrl() {
  logWebRequest("handlePullUrl");
  if (!server.hasArg("url")) { server.send(400, "application/json", "{\"error\":\"missing url\"}"); return; }
  pullUrl = server.arg("url");
  Serial.printf("WEB: Updated pull URL to: %s\n", pullUrl.c_str());
  preferences.begin("epaper", false);
  preferences.putString("pullurl", pullUrl);
  preferences.end();
  appendDeviceLog("Pull URL updated to %s", pullUrl.c_str());
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleSleepConfig() {
  logWebRequest("handleSleepConfig");
  if (!server.hasArg("hours")) { server.send(400, "application/json", "{\"error\":\"missing hours\"}"); return; }

  int hours = server.arg("hours").toInt();
  if (hours < 1 || hours > 168) {
    server.send(400, "application/json", "{\"error\":\"hours must be between 1 and 168\"}");
    return;
  }

  if (oshaEnabled && hours > 24) {
    server.send(400, "application/json", "{\"error\":\"hours must be <= 24 when OSHA mode is enabled\"}");
    return;
  }

  sleepHours = hours;
  preferences.begin("epaper", false);
  preferences.putUInt("sleepHours", sleepHours);
  preferences.end();
  appendDeviceLog("Sleep interval updated to %d hour(s)", hours);
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleShutdown() {
  logWebRequest("handleShutdown");
  appendDeviceLog("Manual shutdown requested from UI");
  server.send(200, "text/plain", "Shutting down");
  delay(150);
  shutdownForever();
}

// ================= Binary upload =================
void handleUploadStream() {
  HTTPUpload &up = server.upload();

  if (up.status == UPLOAD_FILE_START) {
    logWebRequest("handleUploadStream:UPLOAD_FILE_START");
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
    Serial.println("WEB: display upload aborted");
    uploadInProgress = false;
    displayBusy = false;
    if (currentImageFile) currentImageFile.close();
  }
}

void handleUploadDone() {
  logWebRequest("handleUploadDone");
  if (!uploadInProgress) { server.send(500, "application/json", "{\"error\":\"no upload\"}"); return; }

  while (imageBytesWritten < EPD_BUFFER_SIZE) {
    Epaper_Write_Data(0x11);
    if (currentImageFile) currentImageFile.write(0x11);
    imageBytesWritten++;
  }

  if (currentImageFile) currentImageFile.close();

  setOshaModeEnabled(false);
  appendDeviceLog("Manual override image uploaded and displayed");
  Serial.println("WEB: Display upload complete; picture mode enabled and refresh queued");
  uploadInProgress = false;
  pendingDisplayRefresh = true;

  server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Upload complete, refreshing\"}");
}

void writeDisplayRefreshSequence() {
  Epaper_Write_Command(DRF);
  Epaper_Write_Data(0x00);
  Epaper_READBUSY();
  EPD_DeepSleep();
  displayBusy = false;
}

void processPendingDisplayRefresh() {
  if (!pendingDisplayRefresh) return;
  pendingDisplayRefresh = false;
  writeDisplayRefreshSequence();
}

// ================= OSHA helpers =================
bool ensureOshaConfigJson() {
  if (!sdMounted) return false;
  if (SD.exists("/osha/config.json")) return true;

  DynamicJsonDocument d(1024);
  d["background_path"] = "/osha/background.bin";
  d["days_x"] = 70; d["days_y"] = 120; d["days_scale"] = 10;
  d["prior_x"] = 520; d["prior_y"] = 120; d["prior_scale"] = 4;
  d["incident_x"] = 520; d["incident_y"] = 190; d["incident_scale"] = 4;
  d["date_x"] = 520; d["date_y"] = 250; d["date_scale"] = 3;
  d["deploy_box_x"] = 520; d["deploy_box_y"] = 320;
  d["change_box_x"] = 520; d["change_box_y"] = 360;
  d["missed_box_x"] = 520; d["missed_box_y"] = 400;
  d["box_size"] = 18;

  File f = SD.open("/osha/config.json", FILE_WRITE);
  if (!f) return false;
  serializeJsonPretty(d, f);
  f.close();
  return true;
}

bool loadOshaLayout(OshaLayout &layout) {
  if (!ensureOshaConfigJson()) return false;
  File f = SD.open("/osha/config.json", FILE_READ);
  if (!f) return false;
  DynamicJsonDocument d(2048);
  if (deserializeJson(d, f)) { f.close(); return false; }
  f.close();
  layout.backgroundPath = d["background_path"] | "/osha/background.bin";
  layout.daysX = d["days_x"] | 70; layout.daysY = d["days_y"] | 120; layout.daysScale = d["days_scale"] | 10;
  layout.priorX = d["prior_x"] | 520; layout.priorY = d["prior_y"] | 120; layout.priorScale = d["prior_scale"] | 4;
  layout.incidentX = d["incident_x"] | 520; layout.incidentY = d["incident_y"] | 190; layout.incidentScale = d["incident_scale"] | 4;
  layout.dateX = d["date_x"] | 520; layout.dateY = d["date_y"] | 250; layout.dateScale = d["date_scale"] | 3;
  layout.deployBoxX = d["deploy_box_x"] | 520; layout.deployBoxY = d["deploy_box_y"] | 320;
  layout.changeBoxX = d["change_box_x"] | 520; layout.changeBoxY = d["change_box_y"] | 360;
  layout.missedBoxX = d["missed_box_x"] | 520; layout.missedBoxY = d["missed_box_y"] | 400;
  layout.boxSize = d["box_size"] | 18;
  return true;
}

bool parseIsoToEpoch(const String &iso, time_t &epochOut) {
  if (iso.length() < 19) return false;
  struct tm t = {};
  t.tm_year = iso.substring(0,4).toInt() - 1900;
  t.tm_mon = iso.substring(5,7).toInt() - 1;
  t.tm_mday = iso.substring(8,10).toInt();
  t.tm_hour = iso.substring(11,13).toInt();
  t.tm_min = iso.substring(14,16).toInt();
  t.tm_sec = iso.substring(17,19).toInt();
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t local = mktime(&t);
  setenv("TZ", "EST5EDT", 1);
  tzset();
  int sign = 1, offH = 0, offM = 0;
  if (iso.endsWith("Z")) sign = 0;
  else {
    int p = iso.lastIndexOf('+');
    if (p < 0) p = iso.lastIndexOf('-');
    if (p > 0 && p + 5 < (int)iso.length()) {
      sign = (iso.charAt(p) == '+') ? 1 : -1;
      offH = iso.substring(p+1,p+3).toInt();
      offM = iso.substring(p+4,p+6).toInt();
    }
  }
  long offSec = (sign == 0) ? 0 : sign * (offH * 3600 + offM * 60);
  epochOut = local - offSec;
  return true;
}

String epochToNyDate(time_t utcEpoch) {
  struct tm tmny;
  localtime_r(&utcEpoch, &tmny);
  char buf[11];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &tmny);
  return String(buf);
}

String nyTodayDate() {
  time_t now = time(nullptr);
  struct tm tmny;
  localtime_r(&now, &tmny);
  char buf[11];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &tmny);
  return String(buf);
}

int daysBetweenDates(const String &fromDate, const String &toDate) {
  if (fromDate.length() < 10 || toDate.length() < 10) return 0;
  struct tm a = {};
  a.tm_year = fromDate.substring(0,4).toInt() - 1900;
  a.tm_mon = fromDate.substring(5,7).toInt() - 1;
  a.tm_mday = fromDate.substring(8,10).toInt();
  a.tm_hour = 12;
  struct tm b = {};
  b.tm_year = toDate.substring(0,4).toInt() - 1900;
  b.tm_mon = toDate.substring(5,7).toInt() - 1;
  b.tm_mday = toDate.substring(8,10).toInt();
  b.tm_hour = 12;
  time_t ta = mktime(&a);
  time_t tb = mktime(&b);
  if (ta <= 0 || tb <= 0) return 0;
  return max(0, (int)((tb - ta) / 86400));
}

String normalizeOshaCategory(const String &rawIn) {
  String raw = rawIn;
  raw.toLowerCase();
  if (raw.indexOf("deploy") >= 0) return "Deploy";
  if (raw.indexOf("change") >= 0) return "Change";
  if (raw.indexOf("miss") >= 0) return "Missed Task";
  return "";
}

bool loadOshaStateFromSd(OshaState &state) {
  if (!sdMounted || !SD.exists("/osha/state.json")) return false;
  File f = SD.open("/osha/state.json", FILE_READ);
  if (!f) return false;
  DynamicJsonDocument d(512);
  if (deserializeJson(d, f)) { f.close(); return false; }
  f.close();
  state.days = d["days"] | 0;
  state.prior = d["prior"] | 0;
  state.incident = String((const char*)(d["incident"] | ""));
  state.date = String((const char*)(d["date"] | ""));
  state.reason = String((const char*)(d["reason"] | ""));
  return true;
}

bool saveOshaStateToSd(const OshaState &state) {
  if (!sdMounted) return false;
  File f = SD.open("/osha/state.json", FILE_WRITE);
  if (!f) return false;
  DynamicJsonDocument d(512);
  d["days"] = state.days; d["prior"] = state.prior; d["incident"] = state.incident;
  d["date"] = state.date; d["reason"] = state.reason;
  serializeJson(d, f);
  f.close();
  return true;
}

bool saveIncidentDebugFile(const String &name, const String &content) {
  if (!sdMounted) return false;
  if (!SD.exists("/osha")) SD.mkdir("/osha");
  String path = "/osha/" + name;
  File f = SD.open(path.c_str(), FILE_WRITE);
  if (!f) return false;
  f.print(content);
  f.close();
  return true;
}

bool fetchOshaState(OshaState &stateOut) {
  if (!oshaEnabled) {
    Serial.println("OSHA: fetch skipped; OSHA mode disabled");
    appendDeviceLog("OSHA poll skipped: mode disabled");
    return false;
  }
  if (oshaToken.length() == 0) {
    Serial.println("OSHA: fetch skipped; missing token");
    appendDeviceLog("OSHA poll skipped: token missing");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("OSHA: fetch skipped; WiFi not connected");
    appendDeviceLog("OSHA poll skipped: WiFi disconnected");
    return false;
  }
  std::vector<std::pair<time_t, String>> offenses;
  String cursor = "";

  while (true) {
    String url = oshaBaseUrl;
    if (cursor.length() > 0) url += "?cursor=" + cursor;

    WiFiClientSecure secureClient;
    secureClient.setInsecure();

    HTTPClient http;
    http.useHTTP10(true);
    http.setConnectTimeout(15000);
    http.setTimeout(30000);
    Serial.printf("OSHA: fetching incidents from %s\n", url.c_str());
    if (!http.begin(secureClient, url)) {
      Serial.println("OSHA: failed to start HTTPS client");
      appendDeviceLog("OSHA poll failed: HTTPS begin failed");
      return false;
    }
    http.addHeader("Authorization", "Bearer " + oshaToken);
    http.addHeader("Accept", "application/json");
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
      String err = http.errorToString(code);
      String body = http.getString();
      if (body.length() > 300) body = body.substring(0, 300) + "...";
      Serial.printf("OSHA: API GET failed with code %d (%s)\n", code, err.c_str());
      appendDeviceLog("OSHA poll failed: HTTP %d (%s)", code, err.c_str());
      if (body.length() > 0) {
        Serial.printf("OSHA: API error body: %s\n", body.c_str());
        saveIncidentDebugFile("incident-last-error.txt", body);
      }
      http.end();
      return false;
    }

    String payload;
    if (!readHttpBodyWithTimeout(http, payload, 20000)) {
      Serial.println("OSHA: API body read failed or timed out");
      appendDeviceLog("OSHA poll failed: timed out reading API response body");
      http.end();
      return false;
    }
    Serial.printf("OSHA: API page payload bytes=%u\n", (unsigned int)payload.length());
    saveIncidentDebugFile("incident-last.json", payload);
    if (payload.length() == 0) {
      Serial.println("OSHA: API returned empty payload");
      http.end();
      return false;
    }

    StaticJsonDocument<1024> filter;
    filter["incidents"][0]["id"] = true;
    filter["incidents"][0]["incident_date"] = true;
    filter["incidents"][0]["custom_field_entries"][0]["custom_field"]["name"] = true;
    filter["incidents"][0]["custom_field_entries"][0]["value_catalog_entry"]["label"] = true;
    filter["incidents"][0]["custom_field_entries"][0]["value"]["label"] = true;
    filter["incidents"][0]["custom_field_entries"][0]["value"] = true;
    filter["pagination"]["next_cursor"] = true;
    filter["next_cursor"] = true;

    DynamicJsonDocument d(32768);
    auto err = deserializeJson(d, payload, DeserializationOption::Filter(filter));
    http.end();
    if (err) {
      Serial.printf("OSHA: JSON parse failed: %s\n", err.c_str());
      saveIncidentDebugFile("incident-parse-error.txt", payload);
      appendDeviceLog("OSHA poll failed: JSON parse error %s", err.c_str());
      return false;
    }

    JsonArray incidents = d["incidents"].as<JsonArray>();
    for (JsonVariant iv : incidents) {
      String classification = "";
      JsonArray entries = iv["custom_field_entries"].as<JsonArray>();
      for (JsonVariant ev : entries) {
        String fieldName = String((const char*)(ev["custom_field"]["name"] | ""));
        String lower = fieldName; lower.toLowerCase();
        if (lower == "rca classification") {
          classification = String((const char*)(ev["value_catalog_entry"]["label"] | ev["value"]["label"] | ev["value"] | ""));
          break;
        }
      }
      String cat = normalizeOshaCategory(classification);
      String cl = classification; cl.toLowerCase();
      if (cl == "non-procedural incident" || cl == "not classified" || cat.length() == 0) continue;

      String dateIso = String((const char*)(iv["incident_date"] | ""));
      time_t ep = 0;
      if (!parseIsoToEpoch(dateIso, ep)) continue;
      offenses.push_back({ep, cat + "|" + String((const char*)(iv["id"] | ""))});
    }

    String nextCursor = String((const char*)(d["pagination"]["next_cursor"] | d["next_cursor"] | ""));
    if (nextCursor.length() == 0) break;
    cursor = nextCursor;
  }

  if (offenses.empty()) {
    Serial.println("OSHA: no qualifying offenses found");
    appendDeviceLog("OSHA poll success: no qualifying offenses");
    stateOut = {0,0,"","",""};
    return true;
  }

  std::sort(offenses.begin(), offenses.end(), [](const auto &a, const auto &b){ return a.first > b.first; });

  std::vector<String> uniqueDates;
  time_t latestEp = 0;
  String latestCat = "";
  String latestId = "";

  for (auto &off : offenses) {
    String nyDate = epochToNyDate(off.first);
    bool seen = false;
    for (auto &d : uniqueDates) if (d == nyDate) { seen = true; break; }
    if (seen) continue;
    uniqueDates.push_back(nyDate);
    int sep = off.second.indexOf('|');
    String cat = off.second.substring(0, sep);
    String iid = off.second.substring(sep + 1);
    if (uniqueDates.size() == 1) {
      latestEp = off.first;
      latestCat = cat;
      latestId = iid;
    }
  }

  String latestDate = epochToNyDate(latestEp);
  String todayDate = nyTodayDate();

  String digits = "";
  for (size_t i = 0; i < latestId.length(); i++) if (isdigit(latestId[i])) digits += latestId[i];

  stateOut.days = daysBetweenDates(latestDate, todayDate);
  stateOut.prior = (int)uniqueDates.size() - 1;
  stateOut.incident = digits;
  stateOut.date = latestDate;
  stateOut.reason = latestCat;
  Serial.printf("OSHA: computed state days=%d prior=%d incident=%s date=%s reason=%s\n",
                stateOut.days, stateOut.prior, stateOut.incident.c_str(),
                stateOut.date.c_str(), stateOut.reason.c_str());
  return true;
}

bool renderOshaDisplay(const OshaState &state) {
  if (!sdMounted) return false;
  OshaLayout layout;
  if (!loadOshaLayout(layout)) return false;
  File bg = SD.open(layout.backgroundPath.c_str(), FILE_READ);
  if (!bg) return false;

  auto textPixel = [&](int x, int y, int tx, int ty, const String &txt, int scale)->bool {
    int charW = 6 * scale;
    int charH = 8 * scale;
    int rx = x - tx, ry = y - ty;
    if (rx < 0 || ry < 0) return false;
    int idx = rx / charW;
    if (idx < 0 || idx >= (int)txt.length()) return false;
    int cx = (rx % charW) / scale;
    int cy = (ry % charH) / scale;
    if (cx >= 5 || cy >= 7) return false;
    int fi = getCharIndex(txt[idx]);
    uint8_t col = pgm_read_byte(&font5x7[fi][cx]);
    return (col & (1 << cy));
  };

  auto boxPixel = [&](int x, int y, int bx, int by, bool tick)->bool {
    int s = layout.boxSize;
    int rx = x - bx, ry = y - by;
    if (rx < 0 || ry < 0 || rx >= s || ry >= s) return false;
    if (rx == 0 || ry == 0 || rx == s-1 || ry == s-1) return true;
    if (tick && ((rx == ry) || (rx == ry-1) || (rx == ry+1))) return true;
    return false;
  };

  EPD_Init();
  Epaper_Write_Command(DTM);

  size_t written = 0;
  while (written < EPD_BUFFER_SIZE) {
    int b = bg.read();
    if (b < 0) b = 0x11;
    int y = written / BYTES_PER_ROW;
    int xb = written % BYTES_PER_ROW;
    int x1 = xb * 2, x2 = x1 + 1;

    uint8_t hi = (b >> 4) & 0x0F;
    uint8_t lo = b & 0x0F;

    String daysTxt = String(state.days);
    if (textPixel(x1,y,layout.daysX,layout.daysY,daysTxt,layout.daysScale)) hi = EPD_7IN3F_BLACK;
    if (textPixel(x2,y,layout.daysX,layout.daysY,daysTxt,layout.daysScale)) lo = EPD_7IN3F_BLACK;
    if (textPixel(x1,y,layout.priorX,layout.priorY,String(state.prior),layout.priorScale)) hi = EPD_7IN3F_BLACK;
    if (textPixel(x2,y,layout.priorX,layout.priorY,String(state.prior),layout.priorScale)) lo = EPD_7IN3F_BLACK;
    if (textPixel(x1,y,layout.incidentX,layout.incidentY,state.incident,layout.incidentScale)) hi = EPD_7IN3F_BLACK;
    if (textPixel(x2,y,layout.incidentX,layout.incidentY,state.incident,layout.incidentScale)) lo = EPD_7IN3F_BLACK;
    if (textPixel(x1,y,layout.dateX,layout.dateY,state.date,layout.dateScale)) hi = EPD_7IN3F_BLACK;
    if (textPixel(x2,y,layout.dateX,layout.dateY,state.date,layout.dateScale)) lo = EPD_7IN3F_BLACK;

    bool dep = state.reason == "Deploy";
    bool chg = state.reason == "Change";
    bool mis = state.reason == "Missed Task";
    if (boxPixel(x1,y,layout.deployBoxX,layout.deployBoxY,dep)) hi = EPD_7IN3F_BLACK;
    if (boxPixel(x2,y,layout.deployBoxX,layout.deployBoxY,dep)) lo = EPD_7IN3F_BLACK;
    if (boxPixel(x1,y,layout.changeBoxX,layout.changeBoxY,chg)) hi = EPD_7IN3F_BLACK;
    if (boxPixel(x2,y,layout.changeBoxX,layout.changeBoxY,chg)) lo = EPD_7IN3F_BLACK;
    if (boxPixel(x1,y,layout.missedBoxX,layout.missedBoxY,mis)) hi = EPD_7IN3F_BLACK;
    if (boxPixel(x2,y,layout.missedBoxX,layout.missedBoxY,mis)) lo = EPD_7IN3F_BLACK;

    hi = applyLowBatteryOverlayNibble(x1,y,hi);
    lo = applyLowBatteryOverlayNibble(x2,y,lo);

    Epaper_Write_Data((hi << 4) | lo);
    written++;
  }
  bg.close();
  Epaper_Write_Command(DRF);
  Epaper_Write_Data(0x00);
  Epaper_READBUSY();
  EPD_DeepSleep();
  return true;
}

bool refreshOshaAndMaybeDisplay(bool forceDisplay) {
  Serial.printf("OSHA: refresh started (forceDisplay=%s)\n", forceDisplay ? "true" : "false");
  appendDeviceLog("OSHA refresh started (forceDisplay=%s)", forceDisplay ? "true" : "false");
  OshaState fetched;
  if (!fetchOshaState(fetched)) {
    Serial.println("OSHA: refresh failed while fetching state");
    return false;
  }

  OshaState old = {0,0,"","",""};
  bool hasOld = loadOshaStateFromSd(old);
  bool changed = !hasOld || old.days != fetched.days || old.prior != fetched.prior || old.incident != fetched.incident || old.date != fetched.date || old.reason != fetched.reason;
  Serial.printf("OSHA: previous state %s, changed=%s\n", hasOld ? "loaded" : "missing", changed ? "true" : "false");
  if (forceDisplay || changed) {
    Serial.println("OSHA: rendering display");
    if (!renderOshaDisplay(fetched)) {
      Serial.println("OSHA: render failed");
      return false;
    }
    saveOshaStateToSd(fetched);
    Serial.println("OSHA: state saved to SD");
  }

  currentOshaState = fetched;
  oshaDays = fetched.days;
  oshaPrior = fetched.prior;
  oshaIncident = fetched.incident;
  oshaDate = fetched.date;
  oshaReason = fetched.reason;
  Serial.println("OSHA: refresh complete");
  appendDeviceLog("OSHA refresh complete: days=%d prior=%d incident=%s", fetched.days, fetched.prior, fetched.incident.c_str());
  return true;
}

void handleOshaConfig() {
  logWebRequest("handleOshaConfig");
  int enabled = server.hasArg("enabled") ? server.arg("enabled").toInt() : (oshaEnabled ? 1 : 0);
  bool enablingOsha = (enabled == 1);
  setOshaModeEnabled(enablingOsha);
  preferences.begin("epaper", false);
  if (server.hasArg("token") && server.arg("token").length() > 0) {
    oshaToken = server.arg("token");
    preferences.putString("osha_token", oshaToken);
    Serial.println("WEB: OSHA token updated");
    appendDeviceLog("OSHA API token updated");
  }
  if (enablingOsha && sleepHours > 24) {
    sleepHours = 24;
    preferences.putUInt("sleepHours", sleepHours);
  }
  preferences.putString("osha_url", oshaBaseUrl);
  preferences.end();
  Serial.printf("WEB: OSHA mode %s, sleepHours=%u\n", oshaEnabled ? "enabled" : "disabled", sleepHours);
  appendDeviceLog("OSHA mode set to %s", oshaEnabled ? "enabled" : "disabled");
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}


void handleOshaRefresh() {
  logWebRequest("handleOshaRefresh");
  if (!oshaEnabled) {
    Serial.println("WEB: OSHA refresh rejected: OSHA mode disabled");
    server.send(400, "application/json", "{\"error\":\"osha disabled\"}");
    return;
  }
  bool ok = refreshOshaAndMaybeDisplay(true);
  if (!ok) {
    Serial.println("WEB: OSHA refresh failed");
    server.send(500, "application/json", "{\"error\":\"refresh failed; check serial logs for details\"}");
    return;
  }
  Serial.println("WEB: OSHA refresh succeeded");
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleUiRefresh() {
  logWebRequest("handleUiRefresh");
  String error;
  if (!refreshDefaultUiFromGithub(error)) {
    Serial.printf("WEB: UI refresh failed: %s\n", error.c_str());
    appendDeviceLog("UI refresh failed: %s", error.c_str());
    String response = String("{\"error\":\"") + error + "\"}";
    server.send(500, "application/json", response);
    return;
  }

  Serial.println("WEB: UI refresh complete");
  appendDeviceLog("UI refresh complete: /ui.html and /ap.html updated from GitHub");
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleImagesUploadStream() {
  if (server.upload().status == UPLOAD_FILE_START) {
    logWebRequest("handleImagesUploadStream:UPLOAD_FILE_START");
  }
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (!sdMounted) return;
    galleryBytesWritten = 0;
    galleryFileName = sanitizeGalleryFileName(server.hasArg("name") ? server.arg("name") : "");
    if (galleryFile) galleryFile.close();
    galleryFile = SD.open(("/gallery/" + galleryFileName).c_str(), FILE_WRITE);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!galleryFile) return;
    for (size_t i = 0; i < up.currentSize && galleryBytesWritten < EPD_BUFFER_SIZE; i++) {
      galleryFile.write(up.buf[i]);
      galleryBytesWritten++;
    }
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (galleryFile) galleryFile.close();
  }
}

void handleImagesUploadDone() {
  logWebRequest("handleImagesUploadDone");
  if (!sdMounted) { server.send(500, "application/json", "{\"error\":\"sd unavailable\"}"); return; }
  if (galleryBytesWritten != EPD_BUFFER_SIZE) { server.send(400, "application/json", "{\"error\":\"invalid size\"}"); return; }
  String json = String("{\"status\":\"ok\",\"name\":\"") + galleryFileName + "\"}";
  server.send(200, "application/json", json);
}

void handleImagesList() {
  logWebRequest("handleImagesList");
  if (!sdMounted) { server.send(200, "application/json", "[]"); return; }
  File dir = SD.open("/gallery");
  DynamicJsonDocument d(4096);
  JsonArray arr = d.to<JsonArray>();
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;
    String n = String(f.name());
    if (!f.isDirectory() && n.endsWith(".bin")) arr.add(n.substring(n.lastIndexOf('/') + 1));
    f.close();
  }
  String out; serializeJson(arr, out);
  server.send(200, "application/json", out);
}

void handleImagesThumb() {
  logWebRequest("handleImagesThumb");
  if (!sdMounted || !server.hasArg("name")) { server.send(400, "text/plain", "missing name"); return; }
  String name = sanitizeGalleryFileName(server.arg("name"));
  name.replace(".bin", ".jpg");
  String p = "/gallery/.thumbs/" + name;
  if (!SD.exists(p.c_str())) { server.send(404, "text/plain", "not found"); return; }
  File f = SD.open(p.c_str(), FILE_READ);
  server.streamFile(f, "image/jpeg");
  f.close();
}

void handleImagesRaw() {
  logWebRequest("handleImagesRaw");
  if (!sdMounted) { server.send(404, "application/json", "{\"error\":\"not found\"}"); return; }
  if (!server.hasArg("name")) { server.send(400, "application/json", "{\"error\":\"missing name\"}"); return; }

  String name = sanitizeGalleryFileName(server.arg("name"));
  String path = "/gallery/" + name;
  if (!SD.exists(path.c_str())) { server.send(404, "application/json", "{\"error\":\"not found\"}"); return; }

  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) { server.send(404, "application/json", "{\"error\":\"not found\"}"); return; }
  server.streamFile(f, "application/octet-stream");
  f.close();
}

void handleImagesDelete() {
  logWebRequest("handleImagesDelete");
  if (!sdMounted || !server.hasArg("name")) { server.send(400, "application/json", "{\"error\":\"missing name\"}"); return; }
  String name = sanitizeGalleryFileName(server.arg("name"));
  String p = "/gallery/" + name;
  String t = "/gallery/.thumbs/" + name;
  t.replace(".bin", ".jpg");
  if (SD.exists(p.c_str())) SD.remove(p.c_str());
  if (SD.exists(t.c_str())) SD.remove(t.c_str());
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleDisplayShow() {
  logWebRequest("handleDisplayShow");
  if (!sdMounted || !server.hasArg("name")) { server.send(400, "application/json", "{\"error\":\"missing name\"}"); return; }
  String name = sanitizeGalleryFileName(server.arg("name"));
  File f = SD.open(("/gallery/" + name).c_str(), FILE_READ);
  if (!f) { server.send(404, "application/json", "{\"error\":\"not found\"}"); return; }

  displayBusy = true;
  EPD_Init();
  Epaper_Write_Command(DTM);
  size_t written = 0;
  while (written < EPD_BUFFER_SIZE) {
    int b = f.read();
    if (b < 0) b = 0x11;
    int y = written / BYTES_PER_ROW;
    int xb = written % BYTES_PER_ROW;
    int x1 = xb * 2, x2 = x1 + 1;
    uint8_t hi = applyLowBatteryOverlayNibble(x1, y, (b >> 4) & 0x0F);
    uint8_t lo = applyLowBatteryOverlayNibble(x2, y, b & 0x0F);
    Epaper_Write_Data((hi << 4) | lo);
    written++;
  }
  f.close();
  Epaper_Write_Command(DRF);
  Epaper_Write_Data(0x00);
  Epaper_READBUSY();
  EPD_DeepSleep();
  displayBusy = false;
  setOshaModeEnabled(false);
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void setOshaModeEnabled(bool enabled) {
  oshaEnabled = enabled;
  preferences.begin("epaper", false);
  preferences.putBool("osha_enabled", oshaEnabled);
  preferences.end();
}

void displayOshaSetupMessage() {
  String line2 = "Open: http://" + currentIP + "/ui";
  displayTextScreen(
    "OSHA MODE READY",
    "Missing incident.io API key",
    line2.c_str(),
    "Set key or upload image"
  );
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
  server.on("/logs", HTTP_GET, handleLogs);
  server.on("/sd/setup", HTTP_GET, handleSdSetupPage);
  server.on("/sd/setup/run", HTTP_POST, handleSdSetupRun);
  server.on("/session", HTTP_GET, handleSession);
  server.on("/extend", HTTP_POST, handleExtend);
  server.on("/pullurl", HTTP_POST, handlePullUrl);
  server.on("/sleepconfig", HTTP_POST, handleSleepConfig);
  server.on("/shutdown", HTTP_GET, handleShutdown);
  server.on("/display/upload", HTTP_POST, handleUploadDone, handleUploadStream);
  server.on("/images/upload", HTTP_POST, handleImagesUploadDone, handleImagesUploadStream);
  server.on("/images/list", HTTP_GET, handleImagesList);
  server.on("/images/thumb", HTTP_GET, handleImagesThumb);
  server.on("/images/raw", HTTP_GET, handleImagesRaw);
  server.on("/images/delete", HTTP_POST, handleImagesDelete);
  server.on("/display/show", HTTP_POST, handleDisplayShow);
  server.on("/sd/list", HTTP_GET, handleSdList);
  server.on("/sd/upload", HTTP_POST, handleSdUploadDone, handleSdUploadStream);
  server.on("/sd/delete", HTTP_POST, handleSdDelete);
  server.on("/sd/mkdir", HTTP_POST, handleSdMkdir);
  server.on("/osha/config", HTTP_POST, handleOshaConfig);
  server.on("/osha/refresh", HTTP_POST, handleOshaRefresh);
  server.on("/ui/refresh", HTTP_POST, handleUiRefresh);

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
  uint32_t effectiveSleepHours = sleepHours;
  if (oshaEnabled && effectiveSleepHours > 24) effectiveSleepHours = 24;
  uint64_t sleepTimeMicros = (uint64_t)effectiveSleepHours * 60ULL * 60ULL * 1000000ULL;
  esp_sleep_enable_timer_wakeup(sleepTimeMicros);

  Serial.printf("Going to sleep for %u hours...\n", effectiveSleepHours);
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

  sdMounted = SD.begin(SD_CS_PIN);
  if (!sdMounted) {
    Serial.println("SD card mount failed; check card and wiring");
  } else {
    Serial.println("SD card mounted successfully");
    appendDeviceLog("Device boot: SD card mounted");
  }
  setenv("TZ", "EST5EDT", 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  if (sdMounted) ensureGalleryDir();
  if (sdMounted) (void)ensureOshaConfigJson();

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
  sdSetupRequired = (!isAPMode) ? sdCardNeedsSetup() : false;
  setupWebServer();

  if (!isAPMode) {
    delay(ONE_SHOT_DELAY_MS);
    if (oshaEnabled) {
      if (oshaToken.length() == 0) {
        displayOshaSetupMessage();
      } else {
        (void)refreshOshaAndMaybeDisplay(false);
      }
    }
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
  processPendingDisplayRefresh();

  if (!displayBusy && !uploadInProgress && !pendingDisplayRefresh) {
    int32_t msLeft = (int32_t)(sessionEndMs - millis());
    if (msLeft <= 0) shutdownForever();
  }
  delay(5);
}
