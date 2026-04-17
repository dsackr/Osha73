#ifndef GLOBALS_H
#define GLOBALS_H

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

#define DEVICE_LOG_PATH "/device.log"

// ================= Pins =================
#define BUSY_Pin  3
#define RES_Pin   4
#define DC_Pin    1
#define CS_Pin    16
#define SCK_Pin   10
#define SDI_Pin   2

#define NEOPIXEL_PIN 23
#define STATUS_LED_PIN 8

extern const int SD_CS_PIN;

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

// ================= Display Modes =================
enum DisplayMode {
  MODE_URL = 0,
  MODE_SD  = 1
};

// ================= Session timing =================
extern const uint32_t ACTIVE_WINDOW_MS;
extern const uint32_t MAX_WINDOW_MS;
extern const uint32_t ONE_SHOT_DELAY_MS;

extern uint32_t sessionEndMs;

// ================= MAX17048 =================
extern const uint8_t I2C_SDA;
extern const uint8_t I2C_SCL;
extern const uint8_t MAX17048_ADDR;
extern const uint8_t REG_VCELL;
extern const uint8_t REG_SOC;
extern const uint8_t REG_CRATE;

extern bool fuelGaugeOk;

// ================= Low battery overlay =================
extern const int LOW_BAT_OVERWRITE_PCT;
extern const int ICON_MARGIN;
extern const int ICON_W;
extern const int ICON_H;

// ================= Globals =================
extern WebServer server;
extern DNSServer dnsServer;
extern Preferences preferences;

extern bool isAPMode;
extern String currentSSID;
extern String currentIP;

extern bool sdMounted;
extern bool sdSetupRequired;
extern bool wifiPowerSave;
extern File galleryFile;
extern String galleryFileName;
extern size_t galleryBytesWritten;

extern DisplayMode currentDisplayMode;
extern String currentSdImage;

extern volatile bool pendingDisplayRefresh;

extern String pullUrl;
extern uint32_t sleepHours;

extern volatile bool displayBusy;
extern bool uploadInProgress;
extern size_t imageBytesWritten;

extern File currentImageFile;

// ================= Common Helpers =================
void logWebRequest(const char *handlerName);
void appendDeviceLog(const char *fmt, ...);

#endif
