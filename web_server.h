#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "globals.h"

void handleRoot();
void handleUi();
void handleSdSetupPage();
void handleSdSetupRun();
void handleSaveWiFi();
void handleClearWiFi();
void handleWifiPage();
void handleLogs();
void handleSdList();
void handleSdUploadStream();
void handleSdUploadDone();
void handleSdDelete();
void handleSdMkdir();
void handleStatus();
void handleSession();
void handleExtend();
void handlePullUrl();
void handleSleepConfig();
void handleShutdown();
void handleUploadStream();
void handleUploadDone();
void handleUiRefresh();
void handleOtaStream();
void handleOtaDone();
void handleUiUpdateMagic();
void handleImagesUploadStream();
void handleImagesUploadDone();
void handleImagesList();
void handleImagesThumb();
void handleImagesRaw();
void handleImagesDelete();
void handleDisplayShow();
bool streamHtmlFromStorage(const char *path);
void setupWebServer();

#endif
