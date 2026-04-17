#include "image_fetcher.h"
#include "display.h"
#include "sd_manager.h"
#include "battery.h"

bool fetchAndDisplayOneShot() {
  Serial.printf("PULL: starting pull from %s\n", pullUrl.c_str());
  if (pullUrl.length() == 0) {
    Serial.println("PULL: No URL configured");
    return false;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("PULL: WiFi not connected");
    return false;
  }

  WiFi.setSleep(false); // Disable power save for stability

  HTTPClient http;
  http.setTimeout(15000);
  
  if (!http.begin(pullUrl)) {
    Serial.println("PULL: HTTP begin failed");
    WiFi.setSleep(wifiPowerSave);
    return false;
  }

  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("User-Agent", "ESP32-Epaper-Client");
  http.addHeader("Connection", "close");

  int code = http.GET();
  Serial.printf("PULL: HTTP Code %d\n", code);

  if (code != HTTP_CODE_OK) {
    Serial.printf("PULL: Failed, code %d\n", code);
    http.end();
    WiFi.setSleep(wifiPowerSave);
    return false;
  }

  int len = http.getSize();
  if (len > 0 && len != EPD_BUFFER_SIZE) {
    Serial.printf("PULL: WARNING! Content-Length mismatch. Expected %d, got %d.\n", EPD_BUFFER_SIZE, len);
  }

  WiFiClient *stream = http.getStreamPtr();
  if (!stream) {
    Serial.println("PULL: Null stream");
    http.end();
    WiFi.setSleep(wifiPowerSave);
    return false;
  }

  // Peek to check for wrong formats (PNG, JPG, BMP)
  uint8_t header[4] = {0};
  size_t peeked = stream->readBytes(header, 4);
  if (peeked >= 2) {
    if (header[0] == 0x89 && header[1] == 0x50) {
      Serial.println("PULL: ERROR: Detected PNG header!");
      http.end(); WiFi.setSleep(wifiPowerSave); return false;
    }
    if (header[0] == 0xFF && header[1] == 0xD8) {
      Serial.println("PULL: ERROR: Detected JPEG header!");
      http.end(); WiFi.setSleep(wifiPowerSave); return false;
    }
    if (header[0] == 0x42 && header[1] == 0x4D) {
      Serial.println("PULL: ERROR: Detected BMP header!");
      http.end(); WiFi.setSleep(wifiPowerSave); return false;
    }
  }

  Serial.println("PULL: Writing to display...");
  displayBusy = true;
  EPD_Init();
  Epaper_Write_Command(DTM);

  size_t written = 0;
  // Process peeked bytes first
  for(size_t i=0; i<peeked && written < EPD_BUFFER_SIZE; i++) {
    uint8_t byteIn = header[i];
    int y = (int)written / BYTES_PER_ROW;
    int xb = (int)written % BYTES_PER_ROW;
    uint8_t hi = applyLowBatteryOverlayNibble(xb * 2, y, (byteIn >> 4) & 0x0F);
    uint8_t lo = applyLowBatteryOverlayNibble(xb * 2 + 1, y, byteIn & 0x0F);
    Epaper_Write_Data((hi << 4) | lo);
    written++;
  }

  uint8_t buf[1024];
  unsigned long lastData = millis();
  while (written < EPD_BUFFER_SIZE) {
    int avail = stream->available();
    if (avail <= 0) {
      if (!http.connected() || (millis() - lastData > 5000)) break;
      delay(10); continue;
    }
    lastData = millis();
    int n = stream->readBytes(buf, (size_t)min(avail, (int)sizeof(buf)));
    for (int i = 0; i < n && written < EPD_BUFFER_SIZE; i++) {
      uint8_t byteIn = buf[i];
      int y = (int)written / BYTES_PER_ROW;
      int xb = (int)written % BYTES_PER_ROW;
      uint8_t hi = applyLowBatteryOverlayNibble(xb * 2, y, (byteIn >> 4) & 0x0F);
      uint8_t lo = applyLowBatteryOverlayNibble(xb * 2 + 1, y, byteIn & 0x0F);
      Epaper_Write_Data((hi << 4) | lo);
      written++;
    }
  }

  while (written < EPD_BUFFER_SIZE) {
    Epaper_Write_Data(0x11); // Pad white
    written++;
  }

  Epaper_Write_Command(DRF);
  Epaper_Write_Data(0x00);
  Epaper_READBUSY();
  EPD_DeepSleep();

  displayBusy = false;
  http.end();
  WiFi.setSleep(wifiPowerSave);
  Serial.printf("PULL: Finished. Bytes written: %d\n", written);
  return true;
}

bool displaySdImage(const String &fileName) {
  if (!sdMounted) return false;
  String path = "/gallery/" + fileName;
  if (!SD.exists(path.c_str())) return false;
  
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) return false;

  displayBusy = true;
  EPD_Init();
  Epaper_Write_Command(DTM);

  size_t written = 0;
  while (written < EPD_BUFFER_SIZE) {
    int b = f.read();
    if (b < 0) break;
    int y = (int)written / BYTES_PER_ROW;
    int xb = (int)written % BYTES_PER_ROW;
    uint8_t hi = applyLowBatteryOverlayNibble(xb * 2, y, (b >> 4) & 0x0F);
    uint8_t lo = applyLowBatteryOverlayNibble(xb * 2 + 1, y, b & 0x0F);
    Epaper_Write_Data((hi << 4) | lo);
    written++;
  }
  
  while (written < EPD_BUFFER_SIZE) {
    Epaper_Write_Data(0x11);
    written++;
  }
  
  f.close();
  Epaper_Write_Command(DRF);
  Epaper_Write_Data(0x00);
  Epaper_READBUSY();
  EPD_DeepSleep();
  displayBusy = false;
  return true;
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

void shutdownForever() {
  server.stop();
  dnsServer.stop();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  pinMode(NEOPIXEL_PIN, OUTPUT);
  digitalWrite(NEOPIXEL_PIN, LOW);
  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  gpio_reset_pin((gpio_num_t)NEOPIXEL_PIN);
  gpio_reset_pin((gpio_num_t)STATUS_LED_PIN);
  uint64_t sleepTimeMicros = (uint64_t)sleepHours * 60ULL * 60ULL * 1000000ULL;
  esp_sleep_enable_timer_wakeup(sleepTimeMicros);
  Serial.printf("SHUTDOWN: Sleeping for %u hours\n", (unsigned int)sleepHours);
  delay(100);
  esp_deep_sleep_start();
}
