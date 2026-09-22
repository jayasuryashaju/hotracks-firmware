#pragma once

// ---- Wi-Fi ----
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ---- Device identity ----
// Must match the Device "id" you create in the HotRacks web app (e.g. RACK-01).
// The board will be reachable at http://<DEVICE_ID, lowercased>.local
#define DEVICE_ID "RACK-01"

// ---- LED strip ----
#define LED_PIN     4     // WS2812B data pin — physical wiring, so this stays compile-time
#define MAX_LEDS    300   // upper bound of the pixel buffer; must cover the largest rack this
                          // binary will ever run on, since raising it later needs a reflash
#define NUM_LEDS    32    // default *active* LED count, only used the first time this board boots
                          // (no saved value in NVS yet). After that, the active count is set at
                          // runtime via POST /config {"num_leds": N} and persisted in flash, so
                          // the same firmware image works across racks of different sizes.
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB
#define MAX_BRIGHTNESS 255 // 0-255, keep headroom on the power supply

// ---- OTA Updates ----
// After the first USB flash, browse to http://<device_id>.local/update to push
// future firmware builds from the browser — no cable needed again. No login: this
// is only reachable from inside your LAN, same trust model as the rest of the API.
