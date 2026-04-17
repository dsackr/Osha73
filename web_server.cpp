#include "web_server.h"
#include <Update.h>
#include "sd_manager.h"
#include "display.h"
#include "battery.h"
#include "image_fetcher.h"

void keepAwake() {
  if (!isAPMode) {
    sessionEndMs = millis() + ACTIVE_WINDOW_MS;
  }
}

void handleRoot() {
  keepAwake();
  if (isAPMode) {
    if (streamHtmlFromStorage("/ap.html")) return;
    server.send(200, "text/html", "<html><body><h1>AP Mode</h1><form action='/save' method='POST'>SSID: <input name='ssid'><br>Pass: <input name='password' type='password'><br><input type='submit' value='Connect'></form></body></html>");
    return;
  }
  if (streamHtmlFromStorage("/ui.html")) return;

  String html = "<html><body style='font-family:sans-serif;padding:20px;background:#121212;color:#eee;'>";
  html += "<h1>System Recovery</h1><p>ui.html not found on SD card.</p>";
  html += "<button onclick=\"fetch('/update/ui',{method:'POST'}).then(r=>alert('Sync Started... wait 10s then refresh'))\" style='padding:10px;background:#2196f3;color:white;border:none;border-radius:5px;'>Sync UI from GitHub to SD Card</button>";
  html += "<hr><form action='/pullurl' method='POST'>Pull URL: <input name='url' value='"+pullUrl+"'><input type='submit' value='Save'></form>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleUi() { handleRoot(); }

void handleUiUpdateMagic() {
  keepAwake();
  if (!sdMounted) { server.send(500, "text/plain", "SD not mounted"); return; }
  if (downloadFileToSd("https://raw.githubusercontent.com/dsackr/OSHA-7.3eink/main/data/ui.html", "/ui.html")) {
    server.send(200, "text/plain", "UI Updated. Refresh in 5s.");
  } else {
    server.send(500, "text/plain", "Download failed.");
  }
}

void handleSaveWiFi() {
  keepAwake();
  String ssid = server.arg("ssid");
  String pass = server.arg("password");
  preferences.begin("epaper", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", pass);
  preferences.end();
  server.send(200, "text/html", "Saved. Restarting...");
  delay(800); ESP.restart();
}

void handleStatus() {
  keepAwake();
  uint32_t now = millis();
  int32_t msLeft = (sessionEndMs > now) ? (int32_t)(sessionEndMs - now) : 0;
  String json = "{";
  json += "\"ip\":\"" + currentIP + "\",";
  json += "\"battery_percent\":" + String((int)(batterySOC() + 0.5f)) + ",";
  json += "\"pull_url\":\"" + pullUrl + "\",";
  json += "\"sleep_hours\":" + String(sleepHours) + ",";
  json += "\"ms_left\":" + String(msLeft) + ",";
  json += "\"mode\":" + String((int)currentDisplayMode) + "";
  json += "}";
  server.send(200, "application/json", json);
}

void handleExtend() {
  keepAwake();
  sessionEndMs = millis() + 5UL * 60UL * 1000UL;
  server.send(200, "application/json", "{\"ms_left\":300000}");
}

void handlePullUrl() {
  keepAwake();
  if (server.hasArg("url")) {
    pullUrl = server.arg("url");
    preferences.begin("epaper", false);
    preferences.putString("pullurl", pullUrl);
    preferences.end();
    if (currentDisplayMode == MODE_URL) fetchAndDisplayOneShot();
  }
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleSleepConfig() {
  keepAwake();
  if (server.hasArg("hours")) {
    sleepHours = server.arg("hours").toInt();
    preferences.begin("epaper", false);
    preferences.putUInt("sleepHours", sleepHours);
    preferences.end();
  }
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleShutdown() {
  server.send(200, "text/plain", "Sleeping...");
  delay(200); shutdownForever();
}

void handleOtaStream() {
  keepAwake();
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadInProgress = true;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) Serial.println("OTA OK");
    uploadInProgress = false;
  }
}

void handleOtaDone() {
  keepAwake();
  if (Update.hasError()) server.send(500, "text/plain", "OTA Failed");
  else {
    server.send(200, "text/plain", "OK. Rebooting...");
    delay(1000); ESP.restart();
  }
}

void handleImagesUploadStream() {
  keepAwake();
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    if (!sdMounted) return;
    galleryFileName = sanitizeGalleryFileName(server.hasArg("name") ? server.arg("name") : up.filename);
    if (galleryFile) galleryFile.close();
    galleryFile = SD.open(("/gallery/" + galleryFileName).c_str(), FILE_WRITE);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (galleryFile) galleryFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (galleryFile) galleryFile.close();
  }
}

void handleImagesUploadDone() {
  keepAwake();
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleImagesList() {
  keepAwake();
  String json = "[";
  if (sdMounted) {
    File dir = SD.open("/gallery");
    bool first = true;
    while (true) {
      File f = dir.openNextFile();
      if (!f) break;
      if (!f.isDirectory()) {
        if (!first) json += ",";
        json += "\"" + String(f.name()).substring(String(f.name()).lastIndexOf('/') + 1) + "\"";
        first = false;
      }
      f.close();
    }
    dir.close();
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleImagesDelete() {
  keepAwake();
  if (server.hasArg("name")) {
    String p = "/gallery/" + sanitizeGalleryFileName(server.arg("name"));
    if (SD.exists(p.c_str())) SD.remove(p.c_str());
  }
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleModeUpdate() {
  keepAwake();
  if (server.hasArg("mode")) {
    currentDisplayMode = (DisplayMode)server.arg("mode").toInt();
    preferences.begin("epaper", false);
    preferences.putInt("mode", (int)currentDisplayMode);
    preferences.end();
  }
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleSdSelect() {
  keepAwake();
  if (server.hasArg("name")) {
    currentSdImage = server.arg("name");
    preferences.begin("epaper", false);
    preferences.putString("sd_image", currentSdImage);
    preferences.end();
    if (currentDisplayMode == MODE_SD) displaySdImage(currentSdImage);
  }
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

bool streamHtmlFromStorage(const char *path) {
  if (sdMounted && SD.exists(path)) {
    File file = SD.open(path, "r");
    if (file) { server.streamFile(file, "text/html"); file.close(); return true; }
  }
  if (LittleFS.exists(path)) {
    File file = LittleFS.open(path, "r");
    if (file) { server.streamFile(file, "text/html"); file.close(); return true; }
  }
  return false;
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/ui", HTTP_GET, handleUi);
  server.on("/save", HTTP_POST, handleSaveWiFi);
  server.on("/wifi", HTTP_GET, [](){ keepAwake(); if(!streamHtmlFromStorage("/wifi.html")) server.send(200,"text/html","WiFi Page Missing"); });
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/session", HTTP_GET, [](){ keepAwake(); handleStatus(); });
  server.on("/extend", HTTP_POST, handleExtend);
  server.on("/pullurl", HTTP_POST, handlePullUrl);
  server.on("/sleepconfig", HTTP_POST, handleSleepConfig);
  server.on("/shutdown", HTTP_GET, handleShutdown);
  server.on("/update", HTTP_POST, handleOtaDone, handleOtaStream);
  server.on("/update/ui", HTTP_POST, handleUiUpdateMagic);
  server.on("/images/upload", HTTP_POST, handleImagesUploadDone, handleImagesUploadStream);
  server.on("/images/list", HTTP_GET, handleImagesList);
  server.on("/images/delete", HTTP_POST, handleImagesDelete);
  server.on("/config/mode", HTTP_POST, handleModeUpdate);
  server.on("/sd/select", HTTP_POST, handleSdSelect);
  server.onNotFound([]() { server.sendHeader("Location", "/", true); server.send(302, "text/plain", ""); });
  server.begin();
}
