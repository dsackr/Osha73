#include "wifi_manager.h"
#include "display.h"

void setupWiFi(void) {
  preferences.begin("epaper", false);
  String savedSSID = preferences.getString("ssid", "");
  String savedPass = preferences.getString("password", "");

  pullUrl  = preferences.getString("pullurl", "");
  wifiPowerSave = preferences.getBool("wps", true);
  sleepHours = preferences.getUInt("sleepHours", 24);
  currentDisplayMode = (DisplayMode)preferences.getInt("mode", (int)MODE_URL);
  currentSdImage = preferences.getString("sd_image", "");
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
      sessionEndMs = millis() + ACTIVE_WINDOW_MS;

      // Log IP on boot
      Serial.printf("BOOT: STA connected to '%s' | IP: %s\n",
                    currentSSID.c_str(), currentIP.c_str());

      // Don't display info screen - just connect silently
      return;
    }
  }

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
