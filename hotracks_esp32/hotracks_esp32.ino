// HotRacks ESP32 Firmware — Reference Implementation
//
// Features:
// - Direct WS2812B LED driving via FastLED
// - Dual Mode Wi-Fi: Station Mode (Home LAN) + Access Point Fallback (Captive Portal)
// - Onboard Captive Portal: Automatically launches SoftAP ("HotRacks-Setup" / "HotRacks123")
//   when not connected to Wi-Fi. Users can connect, scan nearby networks, and save credentials.
// - NVS Storage: Stores Wi-Fi SSID, password, and Device ID across reboots via Preferences.
// - HTTP REST API: /status, /led, /effect, /off for seamless HotRacks web app integration.
// - mDNS: Advertises as <device_id>.local on the local network.

#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <FastLED.h>
#include <Preferences.h>
#include <WiFi.h>

#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#include <ElegantOTA.h>

#include "config.h"

// ---- State & Hardware ----
CRGB leds[MAX_LEDS];
CLEDController *ledController = nullptr;
int activeNumLeds = NUM_LEDS; // set from NVS in setup(), changeable at runtime via POST /config
AsyncWebServer server(80);
DNSServer dnsServer;
Preferences preferences;

const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
IPAddress apNetMsk(255, 255, 255, 0);

bool inApMode = false;
String activeDeviceId = DEVICE_ID;

// ---- Effect Animation State ----
enum EffectType {
  EFFECT_NONE,
  EFFECT_RAINBOW,
  EFFECT_BREATHE,
  EFFECT_CHASE,
  EFFECT_SPARKLE,
  EFFECT_FLAME,
  EFFECT_AURORA,
  EFFECT_SCANNER,
  EFFECT_STROBE
};

struct GlobalEffect {
  EffectType type = EFFECT_NONE;
  CRGB color = CRGB::White;
  uint8_t speed = 50; // 1-100
  bool bySlot = true; // step per hex slot instead of per raw LED (see "Slot Map" below)
} globalEffect;

struct PixelEffect {
  EffectType type = EFFECT_NONE;
  CRGB color = CRGB::White;
  uint8_t speed = 50;
};
PixelEffect pixelEffects[MAX_LEDS];

// ---- Color Palettes ----
// Real multi-color gradients (like WLED's palettes), not just a single flat color.
// Selected by the same palette ids used in the frontend (wledPalettes.ts).
CRGBPalette16 activePalette = CRGBPalette16(CRGB::White);
bool paletteActive = false; // false = "default"/no palette, use the flat color instead

CRGBPalette16 paletteFromId(const String &id) {
  if (id == "rainbow") return RainbowColors_p;
  if (id == "party") return PartyColors_p;
  if (id == "cloud") return CloudColors_p;
  if (id == "lava") return LavaColors_p;
  if (id == "ocean") return OceanColors_p;
  if (id == "forest") return ForestColors_p;
  if (id == "fire") return HeatColors_p;
  if (id == "sunset") return CRGBPalette16(CRGB(255, 94, 0), CRGB(255, 0, 110), CRGB(131, 0, 255), CRGB(255, 94, 0));
  if (id == "cyberpunk") return CRGBPalette16(CRGB(255, 0, 229), CRGB(0, 229, 255), CRGB(255, 0, 229), CRGB(0, 229, 255));
  return CRGBPalette16(CRGB::White); // "default" and unknown ids
}

void setActivePalette(const String &id) {
  paletteActive = id.length() > 0 && id != "default";
  if (paletteActive) activePalette = paletteFromId(id);
}

// Samples the active palette at a point in time (or a fixed point for a static
// fill), falling back to the plain color when no palette is selected.
CRGB colorForPhase(CRGB fallback, uint32_t p) {
  if (paletteActive) return ColorFromPalette(activePalette, uint8_t(p), 255, LINEARBLEND);
  return fallback;
}

// ---- Slot Map ----
// The physical hex layout (LED range per hex slot), pushed from the app via POST
// /slots whenever hex cells change. Lets chase/scanner/etc. advance one hex at a
// time instead of one raw LED at a time, so a multi-LED hex moves as a single unit
// matching the rack's physical layout — like WLED segments, but per hex.
#define MAX_SLOTS 128
struct Slot {
  uint16_t index;
  uint16_t count;
};
Slot slots[MAX_SLOTS];
int numSlots = 0;

uint32_t animPhase = 0;
uint32_t lastTick = 0;
const uint16_t TICK_MS = 30;

// AsyncWebServer request callbacks run on AsyncTCP's own task, not the loop() task.
// FastLED.show() bit-bangs timing-critical WS2812 data and must only ever be called
// from one task, so handlers just flip this flag and loop() does the actual show().
// Calling it directly from a request handler is what caused the flaky/partial LED
// updates (some pixels not applying, "off" not fully clearing, slow one-by-one lighting).
volatile bool ledsDirty = false;
void requestShow() { ledsDirty = true; }

// ---- Persisted LED State ----
// Saves whatever's currently lit so a power cycle resumes the same look instead of
// coming back dark. Debounced (only flushed to flash a couple seconds after the last
// change) so rapid clicking in the app doesn't hammer the NVS flash sectors with wear.
volatile bool stateDirty = false;
volatile uint32_t lastStateChangeMs = 0;
const uint32_t STATE_SAVE_DEBOUNCE_MS = 2000;
void markStateDirty() {
  stateDirty = true;
  lastStateChangeMs = millis();
}

void saveStateToNvs() {
  preferences.putUChar("eff_type", (uint8_t)globalEffect.type);
  preferences.putUChar("eff_speed", globalEffect.speed);
  preferences.putUChar("eff_r", globalEffect.color.r);
  preferences.putUChar("eff_g", globalEffect.color.g);
  preferences.putUChar("eff_b", globalEffect.color.b);
  // Only the static per-pixel buffer needs saving when no global effect is running —
  // an active effect regenerates the whole strip from globalEffect every tick anyway.
  // (Per-hex "lit only" animated effects aren't captured here and won't survive a
  // power cycle — only whole-rack effects and static per-hex colors do.)
  if (globalEffect.type == EFFECT_NONE) {
    preferences.putBytes("pixels", leds, activeNumLeds * sizeof(CRGB));
  }
}

void restoreStateFromNvs() {
  uint8_t type = preferences.getUChar("eff_type", EFFECT_NONE);
  if (type != EFFECT_NONE) {
    globalEffect.type = (EffectType)type;
    globalEffect.speed = preferences.getUChar("eff_speed", 50);
    globalEffect.color = CRGB(
        preferences.getUChar("eff_r", 255),
        preferences.getUChar("eff_g", 255),
        preferences.getUChar("eff_b", 255));
    return;
  }
  size_t expected = activeNumLeds * sizeof(CRGB);
  if (preferences.getBytesLength("pixels") == expected) {
    preferences.getBytes("pixels", leds, expected);
    requestShow();
  }
}

void saveSlotsToNvs() {
  preferences.putInt("num_slots", numSlots);
  preferences.putBytes("slots", slots, numSlots * sizeof(Slot));
}

void restoreSlotsFromNvs() {
  int saved = preferences.getInt("num_slots", 0);
  if (saved > MAX_SLOTS) saved = MAX_SLOTS;
  size_t expected = saved * sizeof(Slot);
  if (saved > 0 && preferences.getBytesLength("slots") == expected) {
    preferences.getBytes("slots", slots, expected);
    numSlots = saved;
  }
}

// ---- Captive Portal HTML (PROGMEM) ----
const char CAPTIVE_PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>HotRacks Display — Wi-Fi Setup</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body { background: #030712; color: #f8fafc; display: flex; justify-content: center; align-items: center; min-height: 100vh; padding: 1.5rem; }
    .card { background: #0f172a; border: 1px solid #1e293b; border-radius: 1rem; width: 100%; max-width: 440px; padding: 2rem; box-shadow: 0 25px 50px -12px rgba(6, 182, 212, 0.15); }
    .header { text-align: center; margin-bottom: 1.75rem; }
    .badge { display: inline-block; padding: 0.25rem 0.75rem; background: rgba(6, 182, 212, 0.15); color: #22d3ee; border: 1px solid rgba(6, 182, 212, 0.4); border-radius: 9999px; font-size: 0.75rem; font-weight: 700; text-transform: uppercase; letter-spacing: 0.05em; margin-bottom: 0.75rem; }
    h1 { font-size: 1.5rem; font-weight: 800; color: #ffffff; letter-spacing: -0.025em; }
    p.desc { font-size: 0.85rem; color: #94a3b8; margin-top: 0.35rem; line-height: 1.4; }
    .form-group { margin-bottom: 1.25rem; }
    label { display: block; font-size: 0.75rem; font-weight: 700; color: #cbd5e1; text-transform: uppercase; letter-spacing: 0.05em; margin-bottom: 0.4rem; }
    select, input { width: 100%; background: #030712; border: 1px solid #334155; border-radius: 0.5rem; padding: 0.75rem; color: #ffffff; font-size: 0.9rem; outline: none; transition: border-color 0.2s; }
    select:focus, input:focus { border-color: #22d3ee; }
    .scan-btn { background: #1e293b; border: 1px solid #334155; color: #22d3ee; font-size: 0.75rem; font-weight: 600; padding: 0.4rem 0.75rem; border-radius: 0.375rem; cursor: pointer; float: right; margin-top: -1.75rem; }
    .scan-btn:hover { background: #334155; }
    .btn-submit { width: 100%; background: linear-gradient(135deg, #06b6d4, #14b8a6); border: none; border-radius: 0.5rem; padding: 0.85rem; font-size: 0.95rem; font-weight: 700; color: #020617; cursor: pointer; transition: opacity 0.2s, transform 0.1s; margin-top: 0.5rem; }
    .btn-submit:hover { opacity: 0.95; }
    .btn-submit:active { transform: scale(0.99); }
    .footer { text-align: center; margin-top: 1.5rem; font-size: 0.75rem; color: #64748b; }
  </style>
</head>
<body>
  <div class="card">
    <div class="header">
      <div class="badge">HotRacks Setup Mode</div>
      <h1>Connect Rack to Wi-Fi</h1>
      <p class="desc">Select your local 2.4GHz Wi-Fi network and enter the password to connect your display rack.</p>
    </div>

    <form method="POST" action="/save_wifi">
      <div class="form-group">
        <label for="ssid">Nearby Wi-Fi Network</label>
        <button type="button" class="scan-btn" onclick="refreshScan()">Scan</button>
        <select id="ssid_select" onchange="onSelectChange(this)">
          <option value="">-- Scanning networks... --</option>
        </select>
        <input type="text" id="ssid" name="ssid" placeholder="Or enter SSID manually" style="margin-top: 0.5rem;" required>
      </div>

      <div class="form-group">
        <label for="password">Wi-Fi Password</label>
        <input type="password" id="password" name="password" placeholder="Enter network password">
      </div>

      <div class="form-group">
        <label for="device_id">Device Rack ID</label>
        <input type="text" id="device_id" name="device_id" value="RACK-01" required>
      </div>

      <button type="submit" class="btn-submit">Save &amp; Reboot Controller</button>
    </form>

    <div class="footer">
      Access Point IP: 192.168.4.1 · HotRacks Hardware
    </div>
  </div>

  <script>
    async function refreshScan() {
      const sel = document.getElementById('ssid_select');
      sel.innerHTML = '<option value="">Scanning...</option>';
      try {
        const res = await fetch('/scan_wifi');
        const list = await res.json();
        sel.innerHTML = '<option value="">-- Select your Wi-Fi --</option>';
        list.forEach(net => {
          const opt = document.createElement('option');
          opt.value = net.ssid;
          opt.text = `${net.ssid} (${net.rssi} dBm)${net.secure ? ' 🔒' : ''}`;
          sel.appendChild(opt);
        });
      } catch (err) {
        sel.innerHTML = '<option value="">Scan failed (enter SSID below)</option>';
      }
    }
    function onSelectChange(sel) {
      if (sel.value) document.getElementById('ssid').value = sel.value;
    }
    window.addEventListener('DOMContentLoaded', refreshScan);
  </script>
</body>
</html>
)rawliteral";

// ---- Runtime LED Count ----
void applyActiveLedCount(int count) {
  activeNumLeds = constrain(count, 1, MAX_LEDS);
  if (ledController) {
    ledController->setLeds(leds, activeNumLeds);
  }
  fill_solid(leds, MAX_LEDS, CRGB::Black);
  for (int i = 0; i < MAX_LEDS; i++) pixelEffects[i] = PixelEffect();
  requestShow();
}

// ---- CORS Helpers ----
void addCors(AsyncWebServerResponse *res) {
  res->addHeader("Access-Control-Allow-Origin", "*");
  res->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  res->addHeader("Access-Control-Allow-Headers", "Content-Type");
}

void sendJson(AsyncWebServerRequest *request, int code, const JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  AsyncWebServerResponse *res = request->beginResponse(code, "application/json", out);
  addCors(res);
  request->send(res);
}

// ---- Effect Rendering ----
CRGB renderEffectAt(EffectType type, CRGB color, uint8_t speed, uint32_t phase) {
  uint32_t p = phase * (1 + speed / 10);
  switch (type) {
    case EFFECT_RAINBOW: {
      CHSV hsv(uint8_t(p), 255, 255);
      CRGB out;
      hsv2rgb_rainbow(hsv, out);
      return out;
    }
    case EFFECT_BREATHE: {
      CRGB base = colorForPhase(color, p);
      uint8_t b = sin8(uint8_t(p));
      return base.nscale8_video(b);
    }
    case EFFECT_SPARKLE: {
      CRGB base = colorForPhase(color, p);
      return (random8() > 200) ? base : CRGB::Black;
    }
    case EFFECT_STROBE: {
      CRGB base = colorForPhase(color, p);
      return (p % 128 < 24) ? base : CRGB::Black;
    }
    case EFFECT_FLAME: {
      CRGB base = colorForPhase(color, p);
      uint8_t heat = inoise8(uint16_t(p) * 3, millis() / 8);
      base.nscale8_video(heat);
      return base;
    }
    case EFFECT_AURORA: {
      uint8_t hueShift = inoise8(uint16_t(p), millis() / 20);
      CHSV hsv(hueShift, 200, sin8(uint8_t(p)));
      CRGB out;
      hsv2rgb_rainbow(hsv, out);
      return out;
    }
    default:
      return color;
  }
}

// Lights every LED of slot `s` to `c` (bounds-checked against the live buffer size).
void paintSlot(int s, CRGB c) {
  for (uint16_t i = 0; i < slots[s].count; i++) {
    uint16_t led = slots[s].index + i;
    if (led < activeNumLeds) leds[led] = c;
  }
}

void renderChaseToStrip(CRGB color, uint8_t speed, uint32_t phase, bool bySlot) {
  CRGB c = colorForPhase(color, phase);
  fadeToBlackBy(leds, activeNumLeds, 40);
  if (bySlot && numSlots > 0) {
    uint16_t pos = (phase * (1 + speed / 8) / 4) % numSlots;
    for (uint16_t s = pos; s < numSlots; s += 3) paintSlot(s, c);
    return;
  }
  uint16_t pos = (phase * (1 + speed / 8) / 4) % activeNumLeds;
  for (uint16_t i = pos; i < activeNumLeds; i += 3) leds[i] = c;
}

void renderScannerToStrip(CRGB color, uint8_t speed, uint32_t phase, bool bySlot) {
  CRGB c = colorForPhase(color, phase);
  fadeToBlackBy(leds, activeNumLeds, 60);
  int units = (bySlot && numSlots > 0) ? numSlots : activeNumLeds;
  if (units < 2) {
    if (units == 1) {
      if (bySlot && numSlots > 0) paintSlot(0, c);
      else if (activeNumLeds == 1) leds[0] = c;
    }
    return;
  }
  uint16_t span = (units - 1) * 2;
  uint16_t pos = (phase * (1 + speed / 8) / 2) % span;
  uint16_t unit = (pos < units) ? pos : span - pos;
  if (bySlot && numSlots > 0) paintSlot(unit, c);
  else leds[unit] = c;
}

void tickAnimations() {
  uint32_t now = millis();
  if (now - lastTick < TICK_MS) return;
  lastTick = now;
  animPhase++;

  // In AP setup mode: gentle cyan/amber breathe indicator
  if (inApMode) {
    uint8_t b = sin8(uint8_t(animPhase * 2));
    CRGB setupColor = CRGB::Cyan;
    setupColor.nscale8_video(b);
    for (int i = 0; i < activeNumLeds; i++) leds[i] = setupColor;
    FastLED.show();
    return;
  }

  if (globalEffect.type != EFFECT_NONE) {
    if (globalEffect.type == EFFECT_CHASE) {
      renderChaseToStrip(globalEffect.color, globalEffect.speed, animPhase, globalEffect.bySlot);
    } else if (globalEffect.type == EFFECT_SCANNER) {
      renderScannerToStrip(globalEffect.color, globalEffect.speed, animPhase, globalEffect.bySlot);
    } else if (globalEffect.bySlot && numSlots > 0) {
      // Every LED in a slot shares one color per frame — a 5-LED hex animates as
      // one unit instead of each of its LEDs running the effect independently.
      for (int s = 0; s < numSlots; s++) {
        CRGB c = renderEffectAt(globalEffect.type, globalEffect.color, globalEffect.speed, animPhase + s * 3);
        paintSlot(s, c);
      }
    } else {
      for (int i = 0; i < activeNumLeds; i++) {
        leds[i] = renderEffectAt(globalEffect.type, globalEffect.color, globalEffect.speed, animPhase + i * 3);
      }
    }
    FastLED.show();
    return;
  }

  bool any = false;
  for (int i = 0; i < activeNumLeds; i++) {
    if (pixelEffects[i].type != EFFECT_NONE) {
      leds[i] = renderEffectAt(pixelEffects[i].type, pixelEffects[i].color, pixelEffects[i].speed, animPhase);
      any = true;
    }
  }
  if (any) FastLED.show();
}

EffectType parseEffectName(const String &name) {
  if (name == "rainbow") return EFFECT_RAINBOW;
  if (name == "breathe") return EFFECT_BREATHE;
  if (name == "chase") return EFFECT_CHASE;
  if (name == "sparkle") return EFFECT_SPARKLE;
  if (name == "flame") return EFFECT_FLAME;
  if (name == "aurora") return EFFECT_AURORA;
  if (name == "scanner") return EFFECT_SCANNER;
  if (name == "strobe") return EFFECT_STROBE;
  return EFFECT_NONE; // "solid"/"off"/unknown names fall through to a solid fill
}

void clearAll() {
  globalEffect.type = EFFECT_NONE;
  for (int i = 0; i < activeNumLeds; i++) pixelEffects[i] = PixelEffect();
  fill_solid(leds, activeNumLeds, CRGB::Black);
  requestShow();
  markStateDirty();
}

// ---- Station Mode HTTP Handlers ----
void handleStatus(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["device_id"] = activeDeviceId;
  doc["led_count"] = activeNumLeds;
  doc["max_leds"] = MAX_LEDS;
  doc["uptime_s"] = millis() / 1000;
  doc["rssi"] = WiFi.RSSI();
  sendJson(request, 200, doc);
}

void handleConfigBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) {
    request->send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  if (!doc["num_leds"].is<int>()) {
    request->send(400, "application/json", "{\"error\":\"missing num_leds\"}");
    return;
  }
  int requested = doc["num_leds"];
  if (requested < 1 || requested > MAX_LEDS) {
    request->send(400, "application/json", "{\"error\":\"num_leds out of range\"}");
    return;
  }

  applyActiveLedCount(requested);
  preferences.putInt("num_leds", activeNumLeds);
  markStateDirty();

  JsonDocument res;
  res["ok"] = true;
  res["led_count"] = activeNumLeds;
  sendJson(request, 200, res);
}

void handleLedBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) {
    request->send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  int index = doc["index"] | -1;
  if (index < 0 || index >= activeNumLeds) {
    request->send(400, "application/json", "{\"error\":\"index out of range\"}");
    return;
  }
  pixelEffects[index] = PixelEffect();
  leds[index] = CRGB(doc["r"] | 0, doc["g"] | 0, doc["b"] | 0);
  requestShow();
  markStateDirty();

  JsonDocument res;
  res["ok"] = true;
  sendJson(request, 200, res);
}

// Sets many pixels in one request/one FastLED.show() instead of one HTTP round-trip
// per LED. A multi-LED hex sent as N separate /led calls was the main cause of
// "lights up one-by-one" and partial on/off — concurrent requests could be applied
// out of order, and each one triggered its own show().
void handleLedsBatchBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) {
    request->send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  JsonArray pixels = doc["pixels"].as<JsonArray>();
  if (pixels.isNull()) {
    request->send(400, "application/json", "{\"error\":\"missing pixels\"}");
    return;
  }

  for (JsonObject px : pixels) {
    int index = px["index"] | -1;
    if (index < 0 || index >= activeNumLeds) continue;
    pixelEffects[index] = PixelEffect();
    leds[index] = CRGB(px["r"] | 0, px["g"] | 0, px["b"] | 0);
  }
  requestShow();
  markStateDirty();

  JsonDocument res;
  res["ok"] = true;
  sendJson(request, 200, res);
}

// Pushes the physical hex layout (LED range per hex) so slot-based effects know
// where each hex's LEDs are. The app calls this whenever the device's hex cells
// change (layout edits, leds_per_hex changes, reallocation).
void handleSlotsBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) {
    request->send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  JsonArray arr = doc["slots"].as<JsonArray>();
  if (arr.isNull()) {
    request->send(400, "application/json", "{\"error\":\"missing slots\"}");
    return;
  }

  numSlots = 0;
  for (JsonObject s : arr) {
    if (numSlots >= MAX_SLOTS) break;
    int idx = s["index"] | -1;
    int cnt = s["count"] | 1;
    if (idx < 0 || cnt < 1) continue;
    slots[numSlots].index = idx;
    slots[numSlots].count = cnt;
    numSlots++;
  }
  saveSlotsToNvs();

  JsonDocument res;
  res["ok"] = true;
  res["num_slots"] = numSlots;
  sendJson(request, 200, res);
}

void handleEffectBody(AsyncWebServerRequest *request, uint8_t *data, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) {
    request->send(400, "application/json", "{\"error\":\"bad json\"}");
    return;
  }
  String name = doc["name"] | "off";
  uint8_t speed = doc["speed"] | 50;
  const char *colorHex = doc["color"] | "#ffffff";
  long colorVal = strtol(colorHex + 1, nullptr, 16);
  CRGB color((colorVal >> 16) & 0xFF, (colorVal >> 8) & 0xFF, colorVal & 0xFF);
  // Step per hex slot by default (matches the physical rack layout); the app can
  // send bySlot:false to fall back to raw per-LED stepping instead.
  bool bySlot = doc["bySlot"] | true;

  if (doc["palette"].is<const char *>()) {
    setActivePalette(String((const char *)doc["palette"]));
  } else {
    setActivePalette("");
  }

  bool hasIndex = doc["index"].is<int>();
  EffectType type = parseEffectName(name);

  if (name == "off") {
    clearAll();
  } else if (hasIndex) {
    int index = doc["index"];
    // A hex with more than 1 LED (leds_per_hex > 1) sends its full LED range via
    // "count" — without this, only the hex's first LED would animate/light up
    // and the rest would sit at whatever they were previously.
    int count = doc["count"] | 1;
    if (count < 1) count = 1;
    CRGB staticColor = colorForPhase(color, 128);
    for (int i = index; i < index + count; i++) {
      if (i < 0 || i >= activeNumLeds) continue;
      if (type == EFFECT_NONE) {
        pixelEffects[i] = PixelEffect();
        leds[i] = staticColor;
      } else {
        pixelEffects[i] = { type, color, speed };
      }
    }
    if (type == EFFECT_NONE) requestShow();
    markStateDirty();
  } else {
    if (type == EFFECT_NONE) {
      globalEffect.type = EFFECT_NONE;
      fill_solid(leds, activeNumLeds, colorForPhase(color, 128));
      requestShow();
    } else {
      globalEffect = { type, color, speed, bySlot };
    }
    markStateDirty();
  }

  JsonDocument res;
  res["ok"] = true;
  sendJson(request, 200, res);
}

// ---- Captive Portal AP Mode Setup ----
void startCaptivePortal() {
  inApMode = true;
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, apNetMsk);

  // AP SSID: HotRacks-Setup, Password: HotRacks123
  String apSsid = "HotRacks-Setup";
  WiFi.softAP(apSsid.c_str(), "HotRacks123");

  Serial.println("\n==========================================");
  Serial.printf("HotRacks Captive Portal Launched!\n");
  Serial.printf("SSID: %s\n", apSsid.c_str());
  Serial.printf("Password: HotRacks123\n");
  Serial.printf("Portal IP: %s\n", apIP.toString().c_str());
  Serial.println("==========================================\n");

  // DNS redirect for captive portal detection
  dnsServer.start(DNS_PORT, "*", apIP);

  // Serve Captive Portal Page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", CAPTIVE_PORTAL_HTML);
  });

  // Captive portal probes from Apple, Google, Microsoft
  server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", CAPTIVE_PORTAL_HTML);
  });
  server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->redirect("http://192.168.4.1/");
  });
  server.on("/gen_204", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->redirect("http://192.168.4.1/");
  });
  server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Microsoft NCSI");
  });

  // Dynamic Wi-Fi Scan endpoint
  server.on("/scan_wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
    int n = WiFi.scanNetworks();
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n; ++i) {
      JsonObject obj = arr.add<JsonObject>();
      obj["ssid"] = WiFi.SSID(i);
      obj["rssi"] = WiFi.RSSI(i);
      obj["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    sendJson(request, 200, doc);
  });

  // Save Wi-Fi credentials endpoint
  server.on("/save_wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
    String newSsid = "";
    String newPass = "";
    String newDevId = activeDeviceId;

    if (request->hasParam("ssid", true)) {
      newSsid = request->getParam("ssid", true)->value();
    }
    if (request->hasParam("password", true)) {
      newPass = request->getParam("password", true)->value();
    }
    if (request->hasParam("device_id", true)) {
      newDevId = request->getParam("device_id", true)->value();
    }

    if (newSsid.length() > 0) {
      preferences.putString("ssid", newSsid);
      preferences.putString("pass", newPass);
      preferences.putString("dev_id", newDevId);

      String responseHtml = String(F("<html><body style='background:#030712;color:#f8fafc;font-family:sans-serif;text-align:center;padding:3rem;'>"
                              "<h1 style='color:#22d3ee;'>Credentials Saved!</h1>"
                              "<p style='color:#94a3b8;margin-top:1rem;'>Rebooting controller to connect to <strong>")) +
                            newSsid +
                            F("</strong>...</p><p style='color:#64748b;font-size:0.85rem;'>Please reconnect your device to your home Wi-Fi.</p></body></html>");

      request->send(200, "text/html", responseHtml);

      Serial.println("Saved Wi-Fi credentials to NVS! Restarting ESP32 in 1 second...");
      delay(1200);
      ESP.restart();
    } else {
      request->send(400, "text/plain", "Missing SSID");
    }
  });

  server.begin();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  ledController = &FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, MAX_LEDS);
  FastLED.setBrightness(MAX_BRIGHTNESS);

  // Initialize NVS storage
  preferences.begin("hotracks", false);
  String savedSsid = preferences.getString("ssid", WIFI_SSID);
  String savedPass = preferences.getString("pass", WIFI_PASSWORD);
  activeDeviceId = preferences.getString("dev_id", DEVICE_ID);
  applyActiveLedCount(preferences.getInt("num_leds", NUM_LEDS));
  restoreStateFromNvs();
  restoreSlotsFromNvs();

  Serial.printf("HotRacks Controller Booting. Device ID: %s\n", activeDeviceId.c_str());

  // Attempt to connect to saved Wi-Fi network
  bool connected = false;
  if (savedSsid.length() > 0 && savedSsid != "YOUR_WIFI_SSID") {
    WiFi.mode(WIFI_STA);
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    Serial.printf("Connecting to Wi-Fi SSID '%s'", savedSsid.c_str());

    uint8_t attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 25) {
      delay(400);
      Serial.print(".");
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      Serial.printf("\nWi-Fi Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
      Serial.println("\nFailed to connect to configured Wi-Fi network.");
    }
  } else {
    Serial.println("No configured Wi-Fi credentials found.");
  }

  // Fallback to Captive Portal AP Mode if connection failed
  if (!connected) {
    startCaptivePortal();
    return;
  }

  // Station Mode: Start mDNS and Runtime APIs
  String mdnsName = activeDeviceId;
  mdnsName.toLowerCase();
  if (MDNS.begin(mdnsName.c_str())) {
    Serial.printf("mDNS Responder ready: http://%s.local\n", mdnsName.c_str());
  }

  server.on("/status", HTTP_GET, handleStatus);
  server.on(
      "/config", HTTP_POST, [](AsyncWebServerRequest *request) {},
      nullptr,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
        handleConfigBody(request, data, len);
      });
  server.onNotFound([](AsyncWebServerRequest *request) {
    if (request->method() == HTTP_OPTIONS) {
      AsyncWebServerResponse *res = request->beginResponse(204);
      addCors(res);
      request->send(res);
    } else {
      request->send(404);
    }
  });
  server.on(
      "/led", HTTP_POST, [](AsyncWebServerRequest *request) {},
      nullptr,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
        handleLedBody(request, data, len);
      });
  server.on(
      "/leds", HTTP_POST, [](AsyncWebServerRequest *request) {},
      nullptr,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
        handleLedsBatchBody(request, data, len);
      });
  server.on(
      "/effect", HTTP_POST, [](AsyncWebServerRequest *request) {},
      nullptr,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
        handleEffectBody(request, data, len);
      });
  server.on(
      "/slots", HTTP_POST, [](AsyncWebServerRequest *request) {},
      nullptr,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t) {
        handleSlotsBody(request, data, len);
      });
  server.on("/off", HTTP_POST, [](AsyncWebServerRequest *request) {
    clearAll();
    JsonDocument res;
    res["ok"] = true;
    sendJson(request, 200, res);
  });

  ElegantOTA.begin(&server); // no auth — only reachable from inside the trusted LAN anyway

  server.begin();
  Serial.println("HotRacks HTTP Controller API online on port 80.");
  Serial.printf("OTA update page: http://%s.local/update\n", mdnsName.c_str());
}

uint32_t lastWifiCheckMs = 0;
const uint32_t WIFI_CHECK_INTERVAL_MS = 5000;

void loop() {
  if (inApMode) {
    dnsServer.processNextRequest();
  } else {
    // WiFi.begin() only runs once in setup(); if the router reboots or the signal
    // drops mid-session, reconnect instead of needing a manual power cycle.
    uint32_t now = millis();
    if (now - lastWifiCheckMs > WIFI_CHECK_INTERVAL_MS) {
      lastWifiCheckMs = now;
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi disconnected, reconnecting...");
        WiFi.reconnect();
      }
    }
  }

  tickAnimations();
  if (ledsDirty) {
    FastLED.show();
    ledsDirty = false;
  }
  if (stateDirty && millis() - lastStateChangeMs > STATE_SAVE_DEBOUNCE_MS) {
    saveStateToNvs();
    stateDirty = false;
  }
}
