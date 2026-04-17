#ifndef DISPLAY_H
#define DISPLAY_H

#include "globals.h"

extern const unsigned char font5x7[][5];

void SPI_Write(unsigned char value);
void Epaper_Write_Command(unsigned char command);
void Epaper_Write_Data(unsigned char data);
void Epaper_READBUSY(void);
void EPD_Init(void);
void EPD_DeepSleep(void);
void displayTextScreen(const char* l1, const char* l2, const char* l3, const char* l4);
void displayAPInfo(void);
void displayConnectedInfo(void);
int getCharIndex(char c);

#endif
