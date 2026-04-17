#include "globals.h"
#include "display.h"
#include "battery.h"
#include "sd_manager.h"
#include "wifi_manager.h"
#include "image_fetcher.h"
#include "web_server.h"

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

  setupWiFi(); // Loads preferences including mode, url, sd_image, etc.
  sdSetupRequired = (!isAPMode) ? sdCardNeedsSetup() : false;
  setupWebServer();

  if (!isAPMode) {
    delay(ONE_SHOT_DELAY_MS);
    if (currentDisplayMode == MODE_URL) {
      if (pullUrl.length() > 0) {
        (void)fetchAndDisplayOneShot();
      }
    } else if (currentDisplayMode == MODE_SD) {
      if (currentSdImage.length() > 0) {
        (void)displaySdImage(currentSdImage);
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

  // Power management: Check if we've exceeded the active window
  if (!displayBusy && !uploadInProgress && !pendingDisplayRefresh) {
    int32_t msLeft = (int32_t)(sessionEndMs - millis());
    if (msLeft <= 0) {
      Serial.println("IDLE: Active window expired, shutting down.");
      shutdownForever();
    }
  }
  delay(5);
}
