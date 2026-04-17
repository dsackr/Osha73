#ifndef IMAGE_FETCHER_H
#define IMAGE_FETCHER_H

#include "globals.h"

bool fetchAndDisplayOneShot();
bool displaySdImage(const String &fileName);
void shutdownForever();
void writeDisplayRefreshSequence();
void processPendingDisplayRefresh();

#endif
