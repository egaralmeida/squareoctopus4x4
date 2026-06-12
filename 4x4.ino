#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <FastLED.h>

// ============================
// Wi-Fi provisioning
// ============================
#define AP_SSID            "squareoctopus-4x4"
#define HOSTNAME           "squareoctopus-4x4"
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define NVS_NAMESPACE      "4x4"

Preferences prefs;
DNSServer dnsServer;
bool apMode = false;

// ============================
// LED configuration
// ============================
#define DATA_PIN     4
#define WIDTH        4
#define HEIGHT       4
#define VISIBLE_LEDS (WIDTH * HEIGHT)
#define NUM_LEDS     (VISIBLE_LEDS + 1)   // LED 0 is intentionally ignored
#define LED_TYPE     WS2812B
#define COLOR_ORDER  GRB
#define DEFAULT_BRIGHTNESS 96

CRGB leds[NUM_LEDS];

// ============================
// Server
// ============================
WebServer server(80);

// ============================
// Limits for uploaded sequences
// ============================
static const uint8_t MAX_SEQUENCE_FRAMES = 64;
static const uint8_t MAX_PIXELS_PER_FRAME = 16;

// ============================
// Types
// ============================
enum Mode : uint8_t {
  MODE_OFF = 0,
  MODE_IDLE,
  MODE_MANUAL,
  MODE_EFFECT,
  MODE_SEQUENCE
};

enum EffectType : uint8_t {
  EFFECT_NONE = 0,
  EFFECT_RAINBOW_FADE,
  EFFECT_BREATHE_WHITE,
  EFFECT_COMET_TRAIL,
  EFFECT_PLASMA,
  EFFECT_COLOR_WIPE_ROWS
};

struct PixelUpdate {
  uint8_t x;
  uint8_t y;
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct SequenceFrame {
  uint16_t durationMs = 100;
  bool clearFirst = true;
  bool clearAfter = false;
  uint8_t pixelCount = 0;
  PixelUpdate pixels[MAX_PIXELS_PER_FRAME];
};

struct SequenceState {
  bool loaded = false;
  bool running = false;
  uint8_t frameCount = 0;
  uint8_t currentFrame = 0;
  uint32_t repeat = 1;          // 0 = endless
  uint32_t cyclesCompleted = 0;
  uint32_t frameStartedAt = 0;
  SequenceFrame frames[MAX_SEQUENCE_FRAMES];
};

struct EffectState {
  EffectType type = EFFECT_NONE;
  bool running = false;
  uint16_t durationMs = 1000;   // one cycle duration
  uint32_t repeat = 1;          // 0 = endless
  uint32_t cyclesCompleted = 0;
  uint32_t cycleStartedAt = 0;
  uint8_t hue = 0;              // used by comet trail
};

struct DeviceState {
  Mode mode = MODE_IDLE;
  bool power = true;
  uint8_t brightness = DEFAULT_BRIGHTNESS;
  uint16_t rotation = 0; // 0, 90, 180, 270 degrees
} deviceState;

SequenceState sequenceState;
EffectState effectState;

// Used by manual mode / pixels endpoint
bool dirtyShow = false;

// ============================
// Forward declarations
// ============================
uint16_t XY(uint8_t x, uint8_t y);
void showNow();
void clearMatrix(bool show);
void stopAllPlayback(bool keepPixels);
void startEffect(EffectType type, uint16_t durationMs, uint32_t repeat);
void startSequence(uint32_t repeat);
void tickPlayback();
void tickEffect(uint32_t now);
void tickSequence(uint32_t now);
void renderEffectFrame(EffectType type, uint32_t elapsedInCycle);
void handleNotFound();
void handleGetState();
void handlePostState();
void handlePostPixels();
void handlePostEffect();
void handlePostSequence();
bool parseJsonBody(JsonDocument& doc);
void sendJsonError(int code, const char* message);
void sendJsonOk(const String& payload);
const char* modeName(Mode mode);
const char* effectName(EffectType effect);
EffectType parseEffectName(const char* name);
void handlePostWifiReset();
void drawStatusBar(uint8_t progress, CRGB color);
void showNoWifiFeedback();
void tickAPModeFeedback();
bool connectToWiFi();
void startAPMode();
void startApiServer();
void handlePortalRoot();
void handlePortalSave();
void handlePortalRedirect();

// ============================
// XY mapper
// Visible matrix maps to LED 1..16
// LED 0 is ignored on purpose
// Applies the configured rotation before the serpentine mapping
// ============================
uint16_t XY(uint8_t x, uint8_t y) {
  if (x >= WIDTH || y >= HEIGHT) return 0;

  uint8_t rx = x, ry = y;
  switch (deviceState.rotation) {
    case 90:
      rx = HEIGHT - 1 - y;
      ry = x;
      break;
    case 180:
      rx = WIDTH - 1 - x;
      ry = HEIGHT - 1 - y;
      break;
    case 270:
      rx = y;
      ry = WIDTH - 1 - x;
      break;
    default:
      break;
  }
  x = rx;
  y = ry;

  uint16_t physicalIndex;
  if ((y % 2) == 0) {
    physicalIndex = y * WIDTH + x;
  } else {
    physicalIndex = y * WIDTH + (WIDTH - 1 - x);
  }

  return physicalIndex + 1; // +1 because LED 0 is the ignored hack LED
}

// ============================
// Helpers
// ============================
void showNow() {
  if (!deviceState.power) {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
  }
  FastLED.show();
}

void clearMatrix(bool show) {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  leds[0] = CRGB::Black; // ignored adapter LED stays black too
  if (show) showNow();
}

void stopAllPlayback(bool keepPixels) {
  effectState.running = false;
  effectState.type = EFFECT_NONE;
  effectState.cyclesCompleted = 0;

  sequenceState.running = false;
  sequenceState.currentFrame = 0;
  sequenceState.cyclesCompleted = 0;

  if (!keepPixels) {
    clearMatrix(true);
  }
}

const char* modeName(Mode mode) {
  switch (mode) {
    case MODE_OFF: return "off";
    case MODE_IDLE: return "idle";
    case MODE_MANUAL: return "manual";
    case MODE_EFFECT: return "effect";
    case MODE_SEQUENCE: return "sequence";
    default: return "unknown";
  }
}

const char* effectName(EffectType effect) {
  switch (effect) {
    case EFFECT_RAINBOW_FADE: return "rainbowFade";
    case EFFECT_BREATHE_WHITE: return "breatheWhite";
    case EFFECT_COMET_TRAIL: return "cometTrail";
    case EFFECT_PLASMA: return "plasma";
    case EFFECT_COLOR_WIPE_ROWS: return "colorWipeRows";
    default: return "none";
  }
}

EffectType parseEffectName(const char* name) {
  if (!name) return EFFECT_NONE;
  if (strcmp(name, "rainbowFade") == 0) return EFFECT_RAINBOW_FADE;
  if (strcmp(name, "breatheWhite") == 0) return EFFECT_BREATHE_WHITE;
  if (strcmp(name, "cometTrail") == 0) return EFFECT_COMET_TRAIL;
  if (strcmp(name, "plasma") == 0) return EFFECT_PLASMA;
  if (strcmp(name, "colorWipeRows") == 0) return EFFECT_COLOR_WIPE_ROWS;
  return EFFECT_NONE;
}

bool parseJsonBody(JsonDocument& doc) {
  if (!server.hasArg("plain")) return false;
  String body = server.arg("plain");
  DeserializationError err = deserializeJson(doc, body);
  return !err;
}

void sendJsonError(int code, const char* message) {
  StaticJsonDocument<128> doc;
  doc["ok"] = false;
  doc["error"] = message;
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

void sendJsonOk(const String& payload) {
  server.send(200, "application/json", payload);
}

// ============================
// Status feedback
// Horizontal bar on the middle row, drawn through XY()
// so it honors the configured rotation.
// ============================
void drawStatusBar(uint8_t progress, CRGB color) {
  if (progress > WIDTH) progress = WIDTH;
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  for (uint8_t x = 0; x < progress; x++) {
    leds[XY(x, 1)] = color;
  }
  FastLED.show();
}

void showNoWifiFeedback() {
  // Full red bar blinking for ~2 seconds
  for (uint8_t i = 0; i < 4; i++) {
    drawStatusBar(WIDTH, CRGB::Red);
    delay(250);
    drawStatusBar(0, CRGB::Black);
    delay(250);
  }
}

void tickAPModeFeedback() {
  static uint32_t lastFrame = 0;
  uint32_t now = millis();
  if (now - lastFrame < 33) return;
  lastFrame = now;

  // Slowly pulsing orange bar
  uint8_t v = beatsin8(12, 30, 255);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  for (uint8_t x = 0; x < WIDTH; x++) {
    leds[XY(x, 1)] = CHSV(28, 255, v);
  }
  FastLED.show();
}

// ============================
// Effect engine
// ============================
void startEffect(EffectType type, uint16_t durationMs, uint32_t repeat) {
  stopAllPlayback(true);

  effectState.type = type;
  effectState.running = true;
  effectState.durationMs = max<uint16_t>(durationMs, 1);
  effectState.repeat = repeat;
  effectState.cyclesCompleted = 0;
  effectState.cycleStartedAt = millis();
  effectState.hue = 0;

  deviceState.mode = MODE_EFFECT;
  deviceState.power = true;
}

void renderEffectFrame(EffectType type, uint32_t elapsedInCycle) {
  uint8_t t8 = millis() / 8;
  uint16_t t16 = millis() / 4;

  switch (type) {
    case EFFECT_RAINBOW_FADE:
      for (uint8_t y = 0; y < HEIGHT; y++) {
        for (uint8_t x = 0; x < WIDTH; x++) {
          uint8_t hue = t8 + x * 16 + y * 24;
          uint8_t val = 180 + sin8((millis() / 4) + x * 32 + y * 40) / 3;
          leds[XY(x, y)] = CHSV(hue, 255, val);
        }
      }
      showNow();
      break;

    case EFFECT_BREATHE_WHITE: {
      uint8_t breath = beatsin8(18, 20, 255);
      for (uint16_t i = 1; i < NUM_LEDS; i++) {
        leds[i] = CRGB(breath, breath, breath);
      }
      showNow();
      break;
    }

    case EFFECT_COMET_TRAIL: {
      fadeToBlackBy(leds + 1, VISIBLE_LEDS, 40);
      uint8_t pos = ((elapsedInCycle / 90) % VISIBLE_LEDS) + 1;
      leds[pos] += CHSV(effectState.hue, 255, 255);

      uint8_t side = ((pos - 1 + 1) % VISIBLE_LEDS) + 1;
      leds[side] += CHSV(effectState.hue + 32, 180, 120);

      effectState.hue++;
      showNow();
      break;
    }

    case EFFECT_PLASMA:
      for (uint8_t y = 0; y < HEIGHT; y++) {
        for (uint8_t x = 0; x < WIDTH; x++) {
          uint8_t v1 = sin8(x * 40 + t16);
          uint8_t v2 = cos8(y * 50 - t16 / 2);
          uint8_t hue = ((uint16_t)v1 + (uint16_t)v2) / 2 + t16 / 8;
          uint8_t val = 120 + (((uint16_t)v1 + (uint16_t)v2) / 4);
          leds[XY(x, y)] = CHSV(hue, 220, val);
        }
      }
      showNow();
      break;

    case EFFECT_COLOR_WIPE_ROWS: {
      uint32_t totalSteps = WIDTH * HEIGHT;
      uint32_t stepDuration = max<uint32_t>(effectState.durationMs / totalSteps, 1);
      uint32_t step = min<uint32_t>(elapsedInCycle / stepDuration, totalSteps);

      clearMatrix(false);
      for (uint8_t y = 0; y < HEIGHT; y++) {
        CRGB c = CHSV(y * 64, 255, 255);
        for (uint8_t x = 0; x < WIDTH; x++) {
          uint32_t idx = y * WIDTH + x;
          if (idx < step) {
            leds[XY(x, y)] = c;
          }
        }
      }
      showNow();
      break;
    }

    default:
      break;
  }
}

void tickEffect(uint32_t now) {
  if (!effectState.running || deviceState.mode != MODE_EFFECT || !deviceState.power) return;

  uint32_t elapsed = now - effectState.cycleStartedAt;

  if (elapsed >= effectState.durationMs) {
    effectState.cyclesCompleted++;

    if (effectState.repeat != 0 && effectState.cyclesCompleted >= effectState.repeat) {
      effectState.running = false;
      effectState.type = EFFECT_NONE;
      deviceState.mode = MODE_IDLE;
      return;
    }

    effectState.cycleStartedAt = now;
    elapsed = 0;

    if (effectState.type == EFFECT_COMET_TRAIL) {
      clearMatrix(false);
    }
  }

  renderEffectFrame(effectState.type, elapsed);
}

// ============================
// Sequence engine
// ============================
void startSequence(uint32_t repeat) {
  stopAllPlayback(true);

  sequenceState.repeat = repeat;
  sequenceState.running = true;
  sequenceState.currentFrame = 0;
  sequenceState.cyclesCompleted = 0;
  sequenceState.frameStartedAt = 0;

  deviceState.mode = MODE_SEQUENCE;
  deviceState.power = true;
}

void tickSequence(uint32_t now) {
  if (!sequenceState.running || deviceState.mode != MODE_SEQUENCE || !deviceState.power) return;
  if (!sequenceState.loaded || sequenceState.frameCount == 0) return;

  if (sequenceState.frameStartedAt == 0) {
    sequenceState.currentFrame = 0;
    SequenceFrame& frame = sequenceState.frames[sequenceState.currentFrame];

    if (frame.clearFirst) {
      clearMatrix(false);
    }

    for (uint8_t i = 0; i < frame.pixelCount; i++) {
      PixelUpdate& p = frame.pixels[i];
      if (p.x < WIDTH && p.y < HEIGHT) {
        leds[XY(p.x, p.y)] = CRGB(p.r, p.g, p.b);
      }
    }
    showNow();
    sequenceState.frameStartedAt = now;
    return;
  }

  SequenceFrame& current = sequenceState.frames[sequenceState.currentFrame];
  if ((now - sequenceState.frameStartedAt) < current.durationMs) return;

  if (current.clearAfter) {
    clearMatrix(true);
  }

  sequenceState.currentFrame++;

  if (sequenceState.currentFrame >= sequenceState.frameCount) {
    sequenceState.cyclesCompleted++;

    if (sequenceState.repeat != 0 && sequenceState.cyclesCompleted >= sequenceState.repeat) {
      sequenceState.running = false;
      deviceState.mode = MODE_IDLE;
      return;
    }

    sequenceState.currentFrame = 0;
  }

  SequenceFrame& next = sequenceState.frames[sequenceState.currentFrame];

  if (next.clearFirst) {
    clearMatrix(false);
  }

  for (uint8_t i = 0; i < next.pixelCount; i++) {
    PixelUpdate& p = next.pixels[i];
    if (p.x < WIDTH && p.y < HEIGHT) {
      leds[XY(p.x, p.y)] = CRGB(p.r, p.g, p.b);
    }
  }

  showNow();
  sequenceState.frameStartedAt = now;
}

void tickPlayback() {
  uint32_t now = millis();

  if (!deviceState.power) return;

  switch (deviceState.mode) {
    case MODE_EFFECT:
      tickEffect(now);
      break;
    case MODE_SEQUENCE:
      tickSequence(now);
      break;
    case MODE_MANUAL:
    case MODE_IDLE:
    case MODE_OFF:
    default:
      if (dirtyShow) {
        showNow();
        dirtyShow = false;
      }
      break;
  }
}

// ============================
// HTTP handlers
// ============================
void handleGetState() {
  StaticJsonDocument<768> doc;
  doc["ok"] = true;
  doc["power"] = deviceState.power;
  doc["brightness"] = deviceState.brightness;
  doc["rotation"] = deviceState.rotation;
  doc["mode"] = modeName(deviceState.mode);

  JsonObject effect = doc.createNestedObject("effect");
  effect["running"] = effectState.running;
  effect["name"] = effectName(effectState.type);
  effect["durationMs"] = effectState.durationMs;
  effect["repeat"] = effectState.repeat;
  effect["cyclesCompleted"] = effectState.cyclesCompleted;

  JsonObject sequence = doc.createNestedObject("sequence");
  sequence["loaded"] = sequenceState.loaded;
  sequence["running"] = sequenceState.running;
  sequence["frameCount"] = sequenceState.frameCount;
  sequence["currentFrame"] = sequenceState.currentFrame;
  sequence["repeat"] = sequenceState.repeat;
  sequence["cyclesCompleted"] = sequenceState.cyclesCompleted;

  String out;
  serializeJson(doc, out);
  sendJsonOk(out);
}

void handlePostState() {
  StaticJsonDocument<512> doc;
  if (!parseJsonBody(doc)) {
    sendJsonError(400, "Invalid or missing JSON body");
    return;
  }

  if (doc["brightness"].is<int>()) {
    int b = doc["brightness"].as<int>();
    if (b < 0 || b > 255) {
      sendJsonError(400, "brightness must be 0..255");
      return;
    }
    deviceState.brightness = (uint8_t)b;
    FastLED.setBrightness(deviceState.brightness);
    dirtyShow = true;
  }

  if (doc["rotation"].is<int>()) {
    int rot = doc["rotation"].as<int>();
    if (rot != 0 && rot != 90 && rot != 180 && rot != 270) {
      sendJsonError(400, "rotation must be 0, 90, 180 or 270");
      return;
    }
    deviceState.rotation = (uint16_t)rot;
    prefs.putUShort("rot", deviceState.rotation);
    dirtyShow = true;
  }

  if (doc["stop"].is<bool>() && doc["stop"].as<bool>()) {
    stopAllPlayback(true);
    if (deviceState.power) {
      deviceState.mode = MODE_IDLE;
    }
  }

  if (doc["clear"].is<bool>() && doc["clear"].as<bool>()) {
    clearMatrix(true);
    if (deviceState.power) {
      deviceState.mode = MODE_IDLE;
    }
  }

  if (doc["power"].is<bool>()) {
    bool p = doc["power"].as<bool>();
    deviceState.power = p;

    if (!p) {
      stopAllPlayback(false);
      deviceState.mode = MODE_OFF;
    } else {
      if (deviceState.mode == MODE_OFF) {
        deviceState.mode = MODE_IDLE;
      }
      dirtyShow = true;
    }
  }

  handleGetState();
}

void handlePostPixels() {
  StaticJsonDocument<2048> doc;
  if (!parseJsonBody(doc)) {
    sendJsonError(400, "Invalid or missing JSON body");
    return;
  }

  JsonArray pixels = doc["pixels"].as<JsonArray>();
  if (pixels.isNull()) {
    sendJsonError(400, "pixels array is required");
    return;
  }

  bool clearFirst = doc["clearFirst"] | false;
  bool show = doc["show"] | true;

  stopAllPlayback(true);

  if (!deviceState.power) {
    deviceState.power = true;
  }

  if (clearFirst) {
    clearMatrix(false);
  }

  for (JsonObject p : pixels) {
    if (!p["x"].is<int>() || !p["y"].is<int>() ||
        !p["r"].is<int>() || !p["g"].is<int>() || !p["b"].is<int>()) {
      sendJsonError(400, "each pixel needs x,y,r,g,b");
      return;
    }

    int x = p["x"].as<int>();
    int y = p["y"].as<int>();
    int r = p["r"].as<int>();
    int g = p["g"].as<int>();
    int b = p["b"].as<int>();

    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
      sendJsonError(400, "pixel coordinate out of range");
      return;
    }
    if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
      sendJsonError(400, "r,g,b must be 0..255");
      return;
    }

    leds[XY((uint8_t)x, (uint8_t)y)] = CRGB((uint8_t)r, (uint8_t)g, (uint8_t)b);
  }

  deviceState.mode = MODE_MANUAL;
  if (show) {
    showNow();
  } else {
    dirtyShow = true;
  }

  handleGetState();
}

void handlePostEffect() {
  StaticJsonDocument<512> doc;
  if (!parseJsonBody(doc)) {
    sendJsonError(400, "Invalid or missing JSON body");
    return;
  }

  const char* name = doc["name"];
  if (!name) {
    sendJsonError(400, "effect name is required");
    return;
  }

  EffectType type = parseEffectName(name);
  if (type == EFFECT_NONE) {
    sendJsonError(400, "unknown effect");
    return;
  }

  uint16_t durationMs = doc["durationMs"] | 5000;
  uint32_t repeat = doc["repeat"] | 1;

  startEffect(type, durationMs, repeat);
  handleGetState();
}

void handlePostSequence() {
  StaticJsonDocument<12288> doc;
  if (!parseJsonBody(doc)) {
    sendJsonError(400, "Invalid or missing JSON body");
    return;
  }

  JsonArray frames = doc["frames"].as<JsonArray>();
  if (frames.isNull()) {
    sendJsonError(400, "frames array is required");
    return;
  }

  size_t frameCount = frames.size();
  if (frameCount == 0) {
    sendJsonError(400, "frames array cannot be empty");
    return;
  }
  if (frameCount > MAX_SEQUENCE_FRAMES) {
    sendJsonError(400, "too many frames");
    return;
  }

  sequenceState.loaded = false;
  sequenceState.frameCount = 0;

  uint8_t frameIndex = 0;
  for (JsonObject frameObj : frames) {
    if (frameIndex >= MAX_SEQUENCE_FRAMES) break;

    SequenceFrame& frame = sequenceState.frames[frameIndex];
    frame.durationMs = frameObj["durationMs"] | 100;
    frame.clearFirst = frameObj["clearFirst"] | true;
    frame.clearAfter = frameObj["clearAfter"] | false;
    frame.pixelCount = 0;

    JsonArray pixels = frameObj["pixels"].as<JsonArray>();
    if (pixels.isNull()) {
      sendJsonError(400, "each frame needs a pixels array");
      return;
    }
    if (pixels.size() > MAX_PIXELS_PER_FRAME) {
      sendJsonError(400, "too many pixels in a frame");
      return;
    }

    uint8_t pixelIndex = 0;
    for (JsonObject p : pixels) {
      if (!p["x"].is<int>() || !p["y"].is<int>() ||
          !p["r"].is<int>() || !p["g"].is<int>() || !p["b"].is<int>()) {
        sendJsonError(400, "each pixel needs x,y,r,g,b");
        return;
      }

      int x = p["x"].as<int>();
      int y = p["y"].as<int>();
      int r = p["r"].as<int>();
      int g = p["g"].as<int>();
      int b = p["b"].as<int>();

      if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
        sendJsonError(400, "pixel coordinate out of range");
        return;
      }
      if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) {
        sendJsonError(400, "r,g,b must be 0..255");
        return;
      }

      frame.pixels[pixelIndex].x = (uint8_t)x;
      frame.pixels[pixelIndex].y = (uint8_t)y;
      frame.pixels[pixelIndex].r = (uint8_t)r;
      frame.pixels[pixelIndex].g = (uint8_t)g;
      frame.pixels[pixelIndex].b = (uint8_t)b;
      pixelIndex++;
    }

    frame.pixelCount = pixelIndex;
    frameIndex++;
  }

  sequenceState.frameCount = frameIndex;
  sequenceState.loaded = true;

  uint32_t repeat = doc["repeat"] | 1;
  startSequence(repeat);

  handleGetState();
}

void handleNotFound() {
  sendJsonError(404, "Not found");
}

void handlePostWifiReset() {
  prefs.remove("ssid");
  prefs.remove("pass");
  server.send(200, "application/json", "{\"ok\":true,\"message\":\"WiFi credentials cleared, rebooting into setup mode\"}");
  delay(500);
  ESP.restart();
}

// ============================
// Wi-Fi provisioning portal
// ============================
static const char PORTAL_HTML_HEAD[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>4x4 Matrix Setup &middot; @squareoctopus</title>
<style>
  :root { --bg:#0e1116; --card:#161b24; --border:#2a3140; --text:#e6e9ef; --muted:#8b93a3; --accent:#ff7849; }
  * { box-sizing:border-box; margin:0; padding:0; }
  body { background:var(--bg); color:var(--text); min-height:100vh; display:flex; align-items:center; justify-content:center; padding:24px;
         font-family:-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; }
  .card { background:var(--card); border:1px solid var(--border); border-radius:16px; padding:32px 28px; width:100%; max-width:380px;
          box-shadow:0 8px 40px rgba(0,0,0,.45); }
  .brand { font-size:14px; letter-spacing:.12em; text-transform:uppercase; color:var(--accent); font-weight:700; margin-bottom:6px; }
  h1 { font-size:22px; font-weight:600; margin-bottom:4px; }
  .sub { color:var(--muted); font-size:14px; margin-bottom:24px; }
  label { display:block; font-size:13px; color:var(--muted); margin:16px 0 6px; }
  select, input { width:100%; background:#0b0e13; border:1px solid var(--border); border-radius:8px; color:var(--text);
                  padding:11px 12px; font-size:15px; outline:none; }
  select:focus, input:focus { border-color:var(--accent); }
  button { width:100%; margin-top:24px; background:var(--accent); color:#1a0e07; border:none; border-radius:8px;
           padding:13px; font-size:15px; font-weight:700; cursor:pointer; }
  button:active { filter:brightness(.9); }
  .foot { margin-top:22px; text-align:center; font-size:12px; color:var(--muted); }
  .foot span { color:var(--accent); }
</style>
</head>
<body>
<div class="card">
  <div class="brand">@squareoctopus</div>
  <h1>4x4 Matrix Setup</h1>
  <p class="sub">Connect your LED matrix to a Wi-Fi network.</p>
  <form method="POST" action="/save">
    <label for="networks">Available networks</label>
    <select id="networks" onchange="document.getElementById('ssid').value=this.value">
      <option value="">Select a network&hellip;</option>
)rawliteral";

static const char PORTAL_HTML_TAIL[] PROGMEM = R"rawliteral(    </select>
    <label for="ssid">Network name (SSID)</label>
    <input id="ssid" name="ssid" type="text" maxlength="32" required autocomplete="off">
    <label for="pass">Password</label>
    <input id="pass" name="pass" type="password" maxlength="64" autocomplete="off" placeholder="Leave empty for open networks">
    <button type="submit">Connect</button>
  </form>
  <div class="foot">made with care by <span>@squareoctopus</span></div>
</div>
</body>
</html>
)rawliteral";

static const char PORTAL_HTML_SAVED[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Connecting&hellip; &middot; @squareoctopus</title>
<style>
  body { background:#0e1116; color:#e6e9ef; min-height:100vh; display:flex; align-items:center; justify-content:center; padding:24px;
         font-family:-apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; text-align:center; }
  .brand { font-size:14px; letter-spacing:.12em; text-transform:uppercase; color:#ff7849; font-weight:700; margin-bottom:10px; }
  p { color:#8b93a3; margin-top:8px; font-size:14px; line-height:1.6; }
</style>
</head>
<body>
<div>
  <div class="brand">@squareoctopus</div>
  <h1>Credentials saved</h1>
  <p>The matrix is rebooting and will join your network.<br>
  A blue bar means it is connecting. If it pulses orange again,<br>
  the password was wrong &mdash; reconnect to this access point and retry.</p>
</div>
</body>
</html>
)rawliteral";

String scannedNetworkOptions;

void scanNetworksForPortal() {
  scannedNetworkOptions = "";
  int16_t n = WiFi.scanNetworks();
  for (int16_t i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    // basic HTML escaping for the SSID
    ssid.replace("&", "&amp;");
    ssid.replace("<", "&lt;");
    ssid.replace(">", "&gt;");
    ssid.replace("\"", "&quot;");
    scannedNetworkOptions += "      <option value=\"" + ssid + "\">" + ssid +
                             " (" + String(WiFi.RSSI(i)) + " dBm)</option>\n";
  }
  WiFi.scanDelete();
}

void handlePortalRoot() {
  String page = FPSTR(PORTAL_HTML_HEAD);
  page += scannedNetworkOptions;
  page += FPSTR(PORTAL_HTML_TAIL);
  server.send(200, "text/html", page);
}

void handlePortalSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();

  if (ssid.length() == 0) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }

  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);

  server.send(200, "text/html", FPSTR(PORTAL_HTML_SAVED));
  delay(1500);
  ESP.restart();
}

void handlePortalRedirect() {
  // Captive portal: send every unknown request to the setup page
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/");
  server.send(302, "text/plain", "");
}

// ============================
// Wi-Fi connection / AP mode
// ============================
bool connectToWiFi() {
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  if (ssid.length() == 0) {
    Serial.println("No stored Wi-Fi credentials");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME); // shows up under this name in the router's client list
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("Connecting to '%s'", ssid.c_str());

  uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startedAt > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println(" timed out");
      return false;
    }
    // Blue sweeping progress bar while searching
    drawStatusBar(((millis() - startedAt) / 250) % (WIDTH + 1), CRGB::Blue);
    Serial.print(".");
    delay(50);
  }

  WiFi.setAutoReconnect(true);
  Serial.println();
  Serial.print("Connected. IP: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: http://" HOSTNAME ".local");
  } else {
    Serial.println("mDNS failed to start");
  }

  clearMatrix(true);
  return true;
}

void startAPMode() {
  apMode = true;

  // Scan while still in STA mode so the portal can list networks
  WiFi.mode(WIFI_STA);
  scanNetworksForPortal();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  delay(100);

  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, handlePortalRoot);
  server.on("/save", HTTP_POST, handlePortalSave);
  server.onNotFound(handlePortalRedirect);
  server.begin();

  Serial.print("AP mode. Connect to '");
  Serial.print(AP_SSID);
  Serial.print("' and open http://");
  Serial.println(WiFi.softAPIP());
}

void startApiServer() {
  server.on("/state", HTTP_GET, handleGetState);
  server.on("/state", HTTP_POST, handlePostState);
  server.on("/pixels", HTTP_POST, handlePostPixels);
  server.on("/effect", HTTP_POST, handlePostEffect);
  server.on("/sequence", HTTP_POST, handlePostSequence);
  server.on("/wifi/reset", HTTP_POST, handlePostWifiReset);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started");
}

// ============================
// Setup
// ============================
void setup() {
  Serial.begin(115200);
  delay(1000);

  prefs.begin(NVS_NAMESPACE, false);

  deviceState.power = true;
  deviceState.brightness = DEFAULT_BRIGHTNESS;
  deviceState.rotation = prefs.getUShort("rot", 0);
  deviceState.mode = MODE_IDLE;

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(deviceState.brightness);
  clearMatrix(true);

  if (connectToWiFi()) {
    startApiServer();
  } else {
    showNoWifiFeedback();
    startAPMode();
  }
}

// ============================
// Main loop
// ============================
void loop() {
  if (apMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    tickAPModeFeedback();
    return;
  }

  server.handleClient();
  tickPlayback();
}