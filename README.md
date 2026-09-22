Reference firmware for one physical rack section. Drives your addressable LED strip,
advertises itself over mDNS, provides an onboard Wi-Fi captive portal for effortless
first-time network setup, and exposes a high-performance REST API to the HotRacks web app.

HotRacks supports two controller options:
1. **HotRacks Custom Firmware** (this firmware): Optimized for hex racks, featuring SoftAP Wi-Fi provisioning and per-hex addressable animations.
2. **Existing WLED Devices**: Off-the-shelf WLED controllers (flashed on ESP32/ESP8266 or pre-built USB 3.0 controllers), controlled via the WLED JSON API.

## Hardware

- Any ESP32 dev board.
- A WS2812B strip/string with one LED per hex cell (or a short run of LEDs
  per cell if you want a brighter glow — set `led_count` per hex in the
  Django admin/API accordingly).
- 5V power supply sized for your LED count (roughly 60 mA per LED at full
  white; a 10-hex rack at moderate brightness comfortably runs off a 2A 5V
  supply, but size up if you'll run all-white/all-on effects).

### Wiring

- **Data**: ESP32 `LED_PIN` (default GPIO 5, see `config.h`) → a
  **300–500 Ω resistor** → strip DIN. The resistor protects the first LED
  from data-line ringing.
- **Power**: feed 5V and GND to the strip directly from your power supply,
  not through the ESP32's onboard regulator. Tie the ESP32's GND to the
  strip's GND (common ground) — this is required for the data signal to be
  read correctly.
- **Bulk capacitor**: a 1000 µF capacitor across the strip's 5V/GND at the
  injection point smooths inrush current.
- For rows longer than ~60 LEDs, inject 5V/GND at both ends of the run to
  avoid voltage droop dimming/yellowing the far end.
- `led_index` in the HotRacks app must match each hex's physical position
  in the wired chain (LED 0 is the first one after the data pin, etc.) —
  wire the strip in the same order you plan to lay out the hexes, or just
  record the mapping and set each `HexCell.led_index` to match.

## Software & Wi-Fi Provisioning
 
1. Arduino IDE → Boards Manager → install "esp32" (Espressif).
2. Library Manager → install: **FastLED**, **ArduinoJson**, **ElegantOTA** (by Ayush Sharma),
   and the actively-maintained **ESP32Async** forks of **ESP Async WebServer** and **AsyncTCP**
   (the original `me-no-dev` versions don't build against current esp32 core/mbedtls releases —
   make sure no old copies of either are also installed under your Arduino `libraries/` folder).
3. Flash `hotracks_esp32.ino` to your ESP32 board over USB (one-time — see OTA Updates below for every flash after this).
4. **First-Time Wi-Fi Setup (Captive Portal)**:
   - When first powered on (or disconnected from Wi-Fi), the ESP32 automatically broadcasts its own Wi-Fi Access Point:
     - **Network (SSID)**: `HotRacks-Setup`
     - **Default Password**: `HotRacks123`
     - **Portal IP**: `192.168.4.1`
   - Connect your phone, tablet, or laptop to `HotRacks-Setup`.
   - The captive portal screen will pop up automatically (or navigate to `http://192.168.4.1` in your browser).
   - Click **Scan** to view local Wi-Fi networks, select your home 2.4GHz Wi-Fi, enter your Wi-Fi password, and hit **Save & Reboot Controller**.
   - The ESP32 saves credentials to non-volatile flash storage (`Preferences`) and reboots directly onto your local network.
5. In the HotRacks web app:
   - Open **Connection Setup** in the top navigation.
   - Click **Scan Local Network** — HotRacks will automatically detect the controller on your subnet!
   - Or connect your off-the-shelf WLED controller by selecting **Existing WLED Device**.

## OTA Updates

After the first USB flash, every later firmware update can be pushed over Wi-Fi:

1. In Arduino IDE, Sketch → Export Compiled Binary (or just keep building normally).
2. Browse to `http://<device_id lowercased>.local/update` (e.g. `http://rack-01.local/update`).
3. Log in with the `OTA_USERNAME`/`OTA_PASSWORD` set in `config.h` (defaults to `admin` /
   `hotracks-ota` — change the password before leaving this on a shared network).
4. Upload the new `.bin` from the Arduino build output (or Sketch → Export Compiled Binary's
   output folder) through the page.

No cable needed again unless the board stops booting or loses Wi-Fi entirely.

## Distributing precompiled firmware (no Arduino IDE needed)

The `docs/` folder is a self-contained browser flashing tool (like WLED's
install.wled.me) built on [ESP Web Tools](https://esphome.github.io/esp-web-tools/):
a person can plug in a blank ESP32-C3 and flash it from Chrome/Edge alone, no Arduino
IDE or drivers beyond the board's USB-serial chip.

To cut a release:

1. In Arduino IDE, with board = **ESP32C3 Dev Module**: **Sketch → Export Compiled
   Binary**. Look in the sketch's `build/<fqbn>/` folder for a **merged** binary
   (bootloader + partition table + app combined at their real flash offsets, often
   named `hotracks_esp32.ino.merged.bin`; recent esp32 core versions produce this
   automatically). If your core version doesn't produce one, merge them yourself:
   ```bash
   esptool.py --chip esp32c3 merge_bin -o hotracks_esp32.merged.bin \
     0x0     bootloader.bin \
     0x8000  partitions.bin \
     0x10000 hotracks_esp32.ino.bin
   ```
2. Create a GitHub Release on the `hotracks-firmware` repo (tag it, e.g. `v1.0.1`) and
   attach that file **named exactly `hotracks_esp32.merged.bin`** — `docs/manifest.json`
   points at `.../releases/latest/download/hotracks_esp32.merged.bin`, which always
   resolves to the newest release's asset with that name, so the install page never
   needs editing between releases.
3. Enable GitHub Pages on the repo, serving from the `main` branch's `/docs` folder.
   The install page is then live at `https://<username>.github.io/hotracks-firmware/`.

## API contract

| Method | Path       | Body                                              | Notes                                    |
|--------|-----------|----------------------------------------------------|-------------------------------------------|
| GET    | `/status`  | —                                                  | `{device_id, led_count, max_leds, uptime_s, rssi}`. `led_count` is the currently *active* LED count (runtime-configurable, see below); `max_leds` is the compiled-in buffer size (`MAX_LEDS` in `config.h`). |
| POST   | `/config`  | `{num_leds}`                                       | Sets the active LED count (1..`max_leds`), persists it to flash, and clears the strip. No reflash needed to resize a rack — the app can call this whenever the device's LED count changes. |
| POST   | `/led`     | `{index, r, g, b}`                                 | Sets one LED immediately, cancels any effect running on it. `index` must be `< led_count`. |
| POST   | `/leds`    | `{pixels: [{index, r, g, b}, ...]}`                | Sets many LEDs in a single request/single strip refresh. Prefer this over N `/led` calls for a multi-LED hex — separate concurrent requests can be applied out of order and each one forces its own refresh, which is what caused the "lights up one-by-one" / partial on-off behavior. |
| POST   | `/off`     | —                                                   | Clears every LED and any running effect  |
| POST   | `/effect`  | `{name, color?, speed?, index?, count?}`           | `name`: `solid`/`breathe`/`rainbow`/`chase`/`sparkle`/`flame`/`aurora`/`scanner`/`strobe`/`off`. Omit `index` to target the whole strip; when targeting one hex, pass `count` (its `led_count`) so the whole hex animates, not just its first LED. |

State (whole-strip effect, or the static per-hex colors when no effect is running) is
persisted to flash a couple seconds after the last change, and restored on boot — a
power cycle resumes the same look instead of coming back dark. Per-hex effects applied
only to a "lit" subset aren't captured by this and won't survive a power cycle.
Wi-Fi reconnects automatically if the connection drops mid-session (checked every 5s);
a full captive-portal re-provision is only needed if the saved credentials themselves
stop working.

CORS is wide open (`Access-Control-Allow-Origin: *`) since this is a
trusted local-network device with no auth — don't expose it past your LAN.

## Testing without the web app

```bash
curl http://rack-01.local/status
curl -X POST http://rack-01.local/config -d '{"num_leds":48}'
curl -X POST http://rack-01.local/led -d '{"index":0,"r":255,"g":80,"b":0}'
curl -X POST http://rack-01.local/leds -d '{"pixels":[{"index":0,"r":255,"g":80,"b":0},{"index":1,"r":255,"g":80,"b":0}]}'
curl -X POST http://rack-01.local/effect -d '{"name":"rainbow","speed":60}'
curl -X POST http://rack-01.local/off
```

This firmware has not been flashed or run on real hardware from this
session — there was no ESP32 attached — so treat it as a solid starting
point to verify on your bench, not as pre-tested.
