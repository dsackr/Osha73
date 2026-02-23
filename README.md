# OSHA 7.3" 6-Color E-Paper Controller (ESP32-C6)

Updated firmware adds **on-device OSHA incident mode**, **SD-backed gallery management**, and **asynchronous display upload refresh** while preserving AP fallback, deep sleep, manual upload, one-shot pull, and battery overlay behavior.

## Install (ESP32-C6 + Arduino)

These steps assume you already know your ESP32-C6 Arduino setup and hardware wiring.

1. Clone the repo and open it:

   ```bash
   git clone https://github.com/<your-org-or-user>/OSHA-7.3eink.git
   cd OSHA-7.3eink
   ```

2. Open `7in3.ino` in the Arduino IDE.
3. Select your ESP32-C6 board/port and flash as usual.

## Prepare the SD card

Use a FAT32 SD card and copy these files/folders from the repo root:

- `background.bin` → `/osha/background.bin`
- `data/background.png` → `/background.png`
- `data/ui.html` → `/ui.html`
- `data/ap.html` → `/ap.html`

Create these folders if they do not exist:

- `/gallery/`
- `/gallery/.thumbs/`
- `/osha/`

After first boot, firmware auto-creates:

- `/osha/config.json`
- `/osha/state.json`

If your workflow includes uploading web assets/FS content from Arduino tools, keep the same final paths shown above on the SD card.

## SD card layout

- `/gallery/` — saved packed `.bin` images
- `/gallery/.thumbs/` — JPEG thumbnails (`<name>.jpg`) served by `/images/thumb`
- `/osha/background.bin` — 800x480 packed 4bpp OSHA background
- `/osha/config.json` — OSHA overlay coordinates/scales (auto-created with defaults)
- `/osha/state.json` — last computed OSHA state for change detection

## OSHA mode

- Config stored in NVS (`Preferences`):
  - `osha_enabled` (bool)
  - `osha_token` (Bearer token)
  - `osha_url` (API base URL)
- Token is never written to SD and is not echoed in API responses.
- Incident fetch source: `https://api.incident.io/v2/incidents`
- Pagination follows `next_cursor` until complete.
- Classification parsing uses custom field name `RCA Classification` (case-insensitive match of field name).
- Excluded classes: `non-procedural incident`, `not classified`.
- Normalized categories:
  - contains `deploy` → `Deploy`
  - contains `change` → `Change`
  - contains `miss` → `Missed Task`
- Incident date is converted to America/New_York (`TZ=EST5EDT`) before date comparisons.
- Same-day incidents are deduplicated.
- Computed fields:
  - `osha_days`
  - `osha_prior`
  - `osha_incident`
  - `osha_date`
  - `osha_reason`
- If newly computed OSHA state equals `/osha/state.json`, e-paper refresh is skipped.

## New/updated endpoints

- `POST /osha/config`
  - Form fields: `enabled=0|1`, optional `token`
- `POST /osha/refresh`
  - Forces fetch/compute/render now
- `GET /status`
  - Now includes: `osha_enabled`, `osha_days`, `osha_prior`, `osha_incident`, `osha_date`, `osha_reason`

- `POST /images/upload[?name=<safeName>]`
  - Upload exactly `192000` bytes packed image to `/gallery/<name>.bin`
  - Returns immediately and does **not** refresh display
- `GET /images/list`
  - Lists gallery `.bin` files
- `GET /images/thumb?name=<name>`
  - Streams `/gallery/.thumbs/<name>.jpg`
- `POST /images/delete?name=<name>`
  - Deletes both `.bin` and thumbnail
- `POST /display/show?name=<name>`
  - Streams selected gallery image to display and refreshes

- `POST /display/upload`
  - Upload response now returns immediately after data write; display refresh completes asynchronously.

## OSHA background/config workflow

1. Put packed 4bpp background at `/osha/background.bin`.
2. Boot firmware once; it auto-creates `/osha/config.json` defaults if missing.
3. Tune coordinates/scales in `/osha/config.json` as needed.
4. Configure token + enable OSHA from UI (`/ui`) or `POST /osha/config`.
5. Trigger `POST /osha/refresh` to force immediate render.

## Manual testing checklist

1. Upload packed image through UI `/display/upload`; verify immediate HTTP success and eventual panel refresh.
2. Upload gallery image through `/images/upload?name=test`; verify `/images/list` contains `test.bin`.
3. Call `/display/show?name=test`; verify display updates.
4. Call `/images/delete?name=test`; verify file removed.
5. Set OSHA token with `/osha/config`, run `/osha/refresh`, confirm `/status` OSHA fields populate.
6. Re-run `/osha/refresh` without data change; verify no unnecessary render when state unchanged.
