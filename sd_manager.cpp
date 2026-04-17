#include "sd_manager.h"

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
}

bool sdCardNeedsSetup() {
  if (!sdMounted) return false;
  return !SD.exists("/gallery");
}

void logSdSetup(const char *fmt, ...) {
  char message[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  Serial.printf("SD: %s\n", message);
}

bool downloadFileToSd(const char *url, const char *destPath) {
  logSdSetup("download %s", destPath);
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(url)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return false; }

  if (SD.exists(destPath)) SD.remove(destPath);
  File out = SD.open(destPath, FILE_WRITE);
  if (!out) { http.end(); return false; }

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[512];
  size_t total = 0;
  while (http.connected()) {
    int avail = stream->available();
    if (avail <= 0) {
      if (!http.connected()) break;
      delay(5); continue;
    }
    int n = stream->readBytes(buf, (size_t)min(avail, (int)sizeof(buf)));
    if (n > 0) {
      out.write(buf, n);
      total += n;
    }
  }
  out.close();
  http.end();
  logSdSetup("done: %u bytes", total);
  return true;
}

bool sanitizeSdPath(const String &input, String &outPath) {
  String p = input;
  p.trim();
  if (p.length() == 0) p = "/";
  if (!p.startsWith("/")) p = "/" + p;
  if (p.indexOf("..") >= 0) return false;
  outPath = p;
  return true;
}
