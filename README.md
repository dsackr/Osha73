# OSHA 7.3" 6-Color E-Paper Controller (ESP32-C6)

Firmware and web UI for driving an **800x480 7.3-inch 6-color e-paper panel** from an ESP32-C6.

This project provides:
- A Wi-Fi setup flow with AP fallback.
- A browser-based image conversion + upload tool.
- Optional periodic one-shot pull from a remote URL.
- Battery telemetry via MAX17048.
- Deep sleep scheduling for low-power operation.

## Hardware + platform assumptions

- Board: ESP32-C6 (SparkFun ESP32-C6 pinout is assumed by LED references).
- Display: 7.3" 6-color e-paper, 800x480, 4-bit packed pixel stream (2 pixels/byte).
- Battery gauge: MAX17048 on I2C.
- Arduino environment with ESP32 core.

## Pin mapping used in firmware

From `7in3.ino`:

- E-paper control/data
  - `BUSY_Pin` = GPIO 3
  - `RES_Pin` = GPIO 4
  - `DC_Pin` = GPIO 1
  - `CS_Pin` = GPIO 16
  - `SCK_Pin` = GPIO 10
  - `SDI_Pin` = GPIO 2
- LEDs
  - `NEOPIXEL_PIN` = GPIO 23
  - `STATUS_LED_PIN` = GPIO 8
- I2C (MAX17048)
  - SDA = GPIO 6
  - SCL = GPIO 7

## Repository layout

- `7in3.ino` — main firmware (display driver, Wi-Fi/server logic, battery, sleep).
- `data/ui.html` — main control panel served when connected in STA mode.
- `data/ap.html` — AP-mode credential setup page.

## Setup and flashing

1. Open `7in3.ino` in Arduino IDE 2.x.
2. Install required libraries/board support (ESP32 core and dependencies used by the sketch).
3. Keep the `data/` directory next to the sketch with:
   - `data/ap.html`
   - `data/ui.html`
4. Upload firmware.
5. Upload LittleFS assets using **Tools → ESP32 Sketch Data Upload**.

If LittleFS fails to mount or files are missing, firmware serves minimal fallback HTML so setup remains possible.

## Boot behavior and operating modes

### 1) Station mode (saved Wi-Fi exists and connects)

- Device connects to saved SSID.
- Starts HTTP server.
- Enables a finite “Wi-Fi session window” (default 10 minutes).
- Optionally performs one-shot pull from configured URL shortly after boot.
- If session expires and display is idle, enters deep sleep.

### 2) Access Point mode (no credentials or STA connect fails)

- Starts AP:
  - SSID: `EPaper-Setup`
  - Password: `epaper123`
  - IP: `192.168.4.1`
- Starts DNS server for captive-style redirection.
- Shows AP info on e-paper.
- Remains awake (no auto-shutdown timer in AP mode).

## Web endpoints

All endpoints are provided by the firmware HTTP server on port 80.

- `GET /`
  - Serves `ap.html` in AP mode or `ui.html` in STA mode (from LittleFS when present).
- `GET /ui`
  - Alias to `/`.
- `POST /save`
  - Legacy endpoint to save Wi-Fi credentials.
- `GET /wifi`
  - Wi-Fi management page (works in AP and STA).
- `POST /wifi`
  - Save Wi-Fi credentials and reboot.
- `POST /wifi/clear`
  - Remove saved credentials and reboot into AP mode.
- `GET /status`
  - JSON status including IP, SSID, battery, pull URL, sleep interval, busy flag.
- `GET /session`
  - Returns remaining session time in ms (`ms_left`).
- `POST /extend?minutes=N`
  - Extends STA Wi-Fi session by 1–30 minutes per request (capped by max window).
- `POST /pullurl`
  - Saves one-shot image pull URL (`url` form field).
- `POST /sleepconfig`
  - Saves deep sleep wake interval (`hours`, valid 1–168).
- `GET /shutdown`
  - Immediate deep sleep.
- `POST /display/upload[?save=name]`
  - Streams packed image bytes to display; optional SD save with sanitized filename.

Unknown paths redirect to `/`.

## Image format and upload path

The display expects exactly:

- Resolution: **800x480**
- Packed bytes: **192000 bytes** (`800*480/2`)
- 2 pixels per byte (high nibble then low nibble)

### Browser UI conversion (`data/ui.html`)

The UI performs client-side preprocessing:

1. Loads selected image.
2. Crops/fills to 800x480.
   - Portrait images are rotated to better fill landscape display.
3. Applies Floyd–Steinberg dithering.
4. Quantizes to e-paper palette.
5. Packs nibbles into binary stream.
6. Uploads via `multipart/form-data` to `/display/upload`.

Palette used by the UI:

- `0x0` black
- `0x1` white
- `0x2` yellow
- `0x3` red
- `0x5` blue
- `0x6` green
- `0x7` clean/white fallback

## Battery telemetry and low-battery overlay

When MAX17048 is detected:

- `/status` reports:
  - voltage
  - percent (rounded SOC)
  - C-rate (%/hr)
  - inferred state (`charging`, `discharging`, `idle`)

If battery percentage is <= 10%, firmware overlays a red low-battery icon in the top-right corner during image writes (manual upload and one-shot pull paths).

## Power behavior

- STA mode starts with a default 10-minute Wi-Fi session (`DEFAULT_WINDOW_MS`).
- Session can be extended from the UI via `/extend`.
- Firmware caps the total session to max 60 minutes from “now”.
- On timeout (and when not actively writing display), device enters deep sleep.
- Deep sleep wake timer is configurable via `/sleepconfig` (1–168 hours, default 24).

## Persistence (NVS Preferences)

Namespace: `epaper`

Stored keys:
- `ssid` / `password` — Wi-Fi credentials
- `pullurl` — one-shot URL
- `wps` — Wi-Fi power save flag (read by firmware)
- `sleepHours` — deep sleep wake interval

## SD card behavior

- Firmware attempts SD init at boot.
- If `/display/upload` includes query parameter `save=...`, it sanitizes filename to `[A-Za-z0-9_-]` and appends `.bin` if missing.
- Uploaded packed display bytes are saved to SD as they are streamed to panel.

## Typical first-time workflow

1. Flash sketch and upload LittleFS files.
2. Power board; connect phone/laptop to `EPaper-Setup`.
3. Open `http://192.168.4.1` and submit Wi-Fi credentials.
4. Device reboots into STA mode.
5. Open `http://<device-ip>/ui`.
6. Configure optional pull URL and wake interval.
7. Upload an image manually to validate display pipeline.

## Troubleshooting

- **Only fallback pages appear**
  - LittleFS likely missing files; re-run Sketch Data Upload.
- **No STA connection after saving Wi-Fi**
  - Wrong credentials or AP out of range; clear creds via `/wifi/clear` (or boot will eventually fall back to AP mode).
- **Image appears wrong colors**
  - Ensure uploader uses this project’s quantization and packed nibble format.
- **Battery fields stay zero/unknown**
  - MAX17048 not detected on I2C pins or wiring issue.
- **Unexpected sleep behavior**
  - Check `/status` and `/session` values; verify `sleep_hours` and STA/AP mode.
