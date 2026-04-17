#include "display.h"

const unsigned char font5x7[][5] PROGMEM = {
  {0,0,0,0,0}, {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
  {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41},
  {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
  {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
  {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
  {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E},
  {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
  {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
  {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
  {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46},
  {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
  {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03}, {0x36,0x49,0x49,0x49,0x36},
  {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00}, {0x14,0x14,0x14,0x14,0x14},
  {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02}
};

void SPI_Write(unsigned char value) {
  for (int i = 0; i < 8; i++) {
    digitalWrite(SCK_Pin, LOW);
    digitalWrite(SDI_Pin, (value & 0x80) ? HIGH : LOW);
    value <<= 1;
    delayMicroseconds(2);
    digitalWrite(SCK_Pin, HIGH);
    delayMicroseconds(2);
  }
}

void Epaper_Write_Command(unsigned char command) {
  digitalWrite(CS_Pin, LOW);
  digitalWrite(DC_Pin, LOW);
  SPI_Write(command);
  digitalWrite(CS_Pin, HIGH);
}

void Epaper_Write_Data(unsigned char data) {
  digitalWrite(CS_Pin, LOW);
  digitalWrite(DC_Pin, HIGH);
  SPI_Write(data);
  digitalWrite(CS_Pin, HIGH);
}

void Epaper_READBUSY(void) {
  unsigned long start = millis();
  while (!digitalRead(BUSY_Pin)) {
    delay(50);
    if (millis() - start > 30000) {
      Serial.println("BUSY TIMEOUT");
      return;
    }
  }
}

void EPD_Init(void) {
  digitalWrite(RES_Pin, HIGH); delay(50);
  digitalWrite(RES_Pin, LOW);  delay(50);
  digitalWrite(RES_Pin, HIGH); delay(200);

  Epaper_Write_Command(0xAA);
  Epaper_Write_Data(0x49); Epaper_Write_Data(0x55); Epaper_Write_Data(0x20);
  Epaper_Write_Data(0x08); Epaper_Write_Data(0x09); Epaper_Write_Data(0x18);

  Epaper_Write_Command(PWRR); Epaper_Write_Data(0x3F);
  Epaper_Write_Command(PSR);  Epaper_Write_Data(0x5F); Epaper_Write_Data(0x69);

  Epaper_Write_Command(POFS);
  Epaper_Write_Data(0x00); Epaper_Write_Data(0x54);
  Epaper_Write_Data(0x00); Epaper_Write_Data(0x44);

  Epaper_Write_Command(BTST1);
  Epaper_Write_Data(0x40); Epaper_Write_Data(0x1F);
  Epaper_Write_Data(0x1F); Epaper_Write_Data(0x2C);

  Epaper_Write_Command(BTST2);
  Epaper_Write_Data(0x6F); Epaper_Write_Data(0x1F);
  Epaper_Write_Data(0x17); Epaper_Write_Data(0x49);

  Epaper_Write_Command(BTST3);
  Epaper_Write_Data(0x6F); Epaper_Write_Data(0x1F);
  Epaper_Write_Data(0x1F); Epaper_Write_Data(0x22);

  Epaper_Write_Command(PLL); Epaper_Write_Data(0x08);
  Epaper_Write_Command(CDI); Epaper_Write_Data(0x3F);

  Epaper_Write_Command(TCON);
  Epaper_Write_Data(0x02); Epaper_Write_Data(0x00);

  Epaper_Write_Command(TRES);
  Epaper_Write_Data(0x03); Epaper_Write_Data(0x20);
  Epaper_Write_Data(0x01); Epaper_Write_Data(0xE0);

  Epaper_Write_Command(T_VDCS); Epaper_Write_Data(0x01);
  Epaper_Write_Command(PWS);    Epaper_Write_Data(0x2F);

  Epaper_Write_Command(PON);
  Epaper_READBUSY();
}

void EPD_DeepSleep(void) {
  Epaper_Write_Command(0x02);
  Epaper_Write_Data(0x00);
  Epaper_READBUSY();
}

int getCharIndex(char c) {
  if (c == ' ') return 0;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 1;
  if (c >= 'a' && c <= 'z') return c - 'a' + 1;
  if (c >= '0' && c <= '9') return c - '0' + 27;
  if (c == ':') return 37;
  if (c == '-') return 38;
  if (c == '.') return 39;
  if (c == '/') return 40;
  return 0;
}

void displayTextScreen(const char* l1, const char* l2, const char* l3, const char* l4) {
  EPD_Init();
  Epaper_Write_Command(DTM);

  int lineY[] = {160, 200, 240, 280};
  const char* lines[] = {l1, l2, l3, l4};

  for (int y = 0; y < EPD_HEIGHT; y++) {
    for (int x = 0; x < EPD_WIDTH / 2; x++) {
      unsigned char pixelPair = 0x11;

      for (int lineNum = 0; lineNum < 4; lineNum++) {
        if (!lines[lineNum]) continue;

        int lineStartY = lineY[lineNum];
        if (y < lineStartY || y >= lineStartY + 14) continue;

        int textLen = strlen(lines[lineNum]);
        int textWidthPixels = textLen * 12;
        int startX = (EPD_WIDTH - textWidthPixels) / 2;

        int px1 = x * 2;
        int px2 = x * 2 + 1;

        unsigned char c1 = EPD_7IN3F_WHITE;
        unsigned char c2 = EPD_7IN3F_WHITE;

        auto drawPixel = [&](int px, unsigned char &out) {
          if (px < startX || px >= startX + textWidthPixels) return;
          int charIdx = (px - startX) / 12;
          int charCol = ((px - startX) % 12) / 2;
          int charRow = (y - lineStartY) / 2;
          if (charIdx >= textLen || charCol >= 5 || charRow >= 7) return;
          int fontIdx = getCharIndex(lines[lineNum][charIdx]);
          unsigned char col = pgm_read_byte(&font5x7[fontIdx][charCol]);
          if (col & (1 << charRow)) out = EPD_7IN3F_BLACK;
        };

        drawPixel(px1, c1);
        drawPixel(px2, c2);

        pixelPair = (c1 << 4) | c2;
        break;
      }

      Epaper_Write_Data(pixelPair);
    }
  }

  Epaper_Write_Command(DRF);
  Epaper_Write_Data(0x00);
  Epaper_READBUSY();
  EPD_DeepSleep();
}

void displayAPInfo(void) {
  displayTextScreen("AP MODE", "SSID: " AP_SSID, "OPEN: 192.168.4.1", "SET WIFI IN BROWSER");
}

void displayConnectedInfo(void) {
  char ipLine[32];
  snprintf(ipLine, sizeof(ipLine), "IP: %s", currentIP.c_str());
  displayTextScreen("CONNECTED", currentSSID.c_str(), ipLine, "OPEN /ui");
}
