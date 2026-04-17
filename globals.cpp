#include "globals.h"

const int SD_CS_PIN = 18;

const uint32_t ACTIVE_WINDOW_MS = 5UL * 60UL * 1000UL;
const uint32_t MAX_WINDOW_MS = 60UL * 60UL * 1000UL;
const uint32_t ONE_SHOT_DELAY_MS = 1500;

uint32_t sessionEndMs = 0;

const uint8_t I2C_SDA = 6;
const uint8_t I2C_SCL = 7;
const uint8_t MAX17048_ADDR = 0x36;
const uint8_t REG_VCELL = 0x02;
const uint8_t REG_SOC   = 0x04;
const uint8_t REG_CRATE = 0x16;

bool fuelGaugeOk = false;

const int LOW_BAT_OVERWRITE_PCT = 10;
const int ICON_MARGIN = 6;
const int ICON_W = 26;
const int ICON_H = 12;

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

DisplayMode currentDisplayMode = MODE_URL;
String currentSdImage = "";

volatile bool pendingDisplayRefresh = false;

String pullUrl = "";
uint32_t sleepHours = 24;

volatile bool displayBusy = false;
bool uploadInProgress = false;
size_t imageBytesWritten = 0;

File currentImageFile;

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

  File f = SD.open(DEVICE_LOG_PATH, FILE_APPEND);
  if (!f) f = SD.open(DEVICE_LOG_PATH, FILE_WRITE);
  if (!f) return;

  unsigned long s = millis() / 1000UL;
  f.printf("[%lu] %s\n", s, message);
  f.close();
}
