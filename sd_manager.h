#ifndef SD_MANAGER_H
#define SD_MANAGER_H

#include "globals.h"

String sanitizeFileName(const String &name);
String sanitizeGalleryFileName(const String &name);
String urlEncodeComponent(const String &value);
void ensureGalleryDir();
bool ensureDirectory(const char *path);
bool sdCardNeedsSetup();
void logSdSetup(const char *fmt, ...);
bool downloadFileToSd(const char *url, const char *destPath);
bool refreshDefaultUiFromGithub(String &errorOut);
bool readHttpBodyWithTimeout(HTTPClient &http, String &bodyOut, const char *sdPath,
                             unsigned long idleTimeoutMs, unsigned long hardTimeoutMs,
                             bool &completeOut, size_t &bytesReadTotalOut,
                             bool &bodyBufferedCompleteOut);
String escapedPreview(const String &input, size_t maxLen);
void logHttpResponseMeta(const String &url, HTTPClient &http, int code, const String &readMode,
                         unsigned long startedAt, unsigned long firstByteAt, bool tlsInsecure);
bool configureSdCardDefaults(String &errorOut);
void maybeOpenSDForSave();
bool sanitizeSdPath(const String &input, String &outPath);
bool removeSdPathRecursive(const String &path);

#endif
