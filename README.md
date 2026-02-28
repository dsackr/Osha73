# OSHA 7.3" 6-Color E-Paper Controller (ESP32-C6)

This firmware targets an ESP32-C6 driving an 800x480 7.3" 6-color e-paper panel.

Current code supports:
- **Photo mode** (manual image upload + SD library management)
- **OSHA mode** (Google Sheets CSV polling + on-device overlay rendering)
- **Wi-Fi STA/AP flows** with captive setup fallback
- **SD-card-first web UI/assets** with LittleFS fallback for HTML pages
- **Timed Wi-Fi session + deep sleep** behavior

## Install (Arduino)

1. Clone the repo and open it:

   ```bash
   git clone https://github.com/dsackr/OSHA-7.3eink.git
   cd OSHA-7.3eink
   ```

2. Open `OSHA-7.3eink.ino` in the Arduino IDE.
3. Select your ESP32-C6 board and port.
4. Use a partition scheme with enough app space (for example, **No OTA** or **Huge APP**).
5. Flash as usual.

## Storage behavior (SD + LittleFS)

- Firmware mounts **LittleFS** and **SD** on boot.
- `ui.html` / `ap.html` are served with a fallback order that prefers LittleFS for those pages.
- SD is still required for gallery files and OSHA data/background.
- If STA mode is active and SD layout is incomplete, the device redirects to `/sd/setup`.

## SD card layout

Required paths/files:
- `/gallery/`
- `/gallery/.thumbs/`
- `/osha/`
- `/osha/background.bin`
- `/background.png`
- `/ui.html`
- `/ap.html`

Auto-created/managed by firmware:
- `/osha/config.json` (OSHA text/checkbox layout)
- `/osha/state.json` (last OSHA computed state)
- `/osha/device.log` (device event log)

### One-click SD setup

When Wi-Fi STA is connected, `POST /sd/setup/run` will:
- create required directories
- download default files from this repo (`background.bin`, `data/background.png`, `data/ui.html`, `data/ap.html`)

## Modes

## Photo mode

- `POST /display/upload` streams a full packed frame to the panel and schedules refresh.
- Optional `save=<name>` query can store uploaded frame on SD root as `<name>.bin`.
- `POST /images/upload?name=<name>` uploads a packed frame to `/gallery/<name>.bin`.
- `GET /images/list` lists `.bin` images in `/gallery`.
- `POST /images/delete?name=<name>` deletes a gallery image.
- `POST /display/show?name=<name>` renders a gallery image to the display.
- Entering photo mode disables OSHA mode.

Expected packed payload size: `192000` bytes (`800*480/2`).

## OSHA mode

- Config is stored in `Preferences` (NVS):
  - `osha_enabled`
  - `osha_sheet_url`
- Source is a **Google Sheets CSV URL** (`sheet_url`).
- Parser behavior:
  - skips header row
  - expects at least 4 comma-separated columns
  - uses incident/date/reason fields from columns 2/3/4
  - date input format is parsed as `M/D/YYYY`
  - deduplicates incidents by date
- Normalized reason categories:
  - contains `deploy` → `Deploy`
  - contains `change` → `Change`
  - contains `miss` → `Missed Task`
- Computed outputs:
  - `osha_days`
  - `osha_prior`
  - `osha_incident` (digits extracted from incident id)
  - `osha_date`
  - `osha_reason`

When OSHA mode is enabled, scheduled sleep is capped at 24 hours.

## HTTP endpoints

- `GET /` and `GET /ui` — main UI (or AP page in AP mode)
- `GET|POST /wifi` — Wi-Fi settings save/apply
- `POST /wifi/clear` — clear saved Wi-Fi credentials
- `GET /status` — system status JSON (battery, mode, OSHA fields, SD flags)
- `GET /session` — remaining Wi-Fi session window
- `POST /extend?minutes=5` — extend session window (bounded)
- `POST /sleepconfig` — set sleep hours (1-168; max 24 in OSHA mode)
- `GET /shutdown` — immediate deep sleep
- `GET /sd/setup` — setup page when SD layout is incomplete
- `POST /sd/setup/run` — create/download required SD defaults
- `POST /ui/refresh` — refresh `/ui.html` and `/ap.html` from GitHub
- `POST /display/upload` — upload frame for immediate display update
- `POST /images/upload?name=<safeName>` — save library image
- `GET /images/list` — list library images
- `POST /images/delete?name=<safeName>` — delete library image
- `POST /display/show?name=<safeName>` — render selected library image
- `POST /osha/config` — set `enabled` and optional `sheet_url`
- `POST /osha/refresh` — force OSHA fetch + render

## Web UI

`data/ui.html` includes controls for:
- session extension + shutdown
- sleep interval configuration
- OSHA mode toggle and Sheet URL configuration
- immediate OSHA refresh
- client-side image conversion + dithering upload to library
- library list / show / delete actions

## Notes

- AP fallback SSID is `EPaper-Setup`.
- Timezone is configured to `EST5EDT` and NTP is requested during boot.
- Low battery overlay icon is drawn onto outgoing frame data when battery is low.
