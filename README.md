# 4x4 LED Matrix — by @squareoctopus

Wi-Fi controlled 4x4 WS2812B LED matrix running on an ESP32, exposing a simple HTTP JSON API for pixels, effects, and frame sequences.

https://youtube.com/shorts/MrOqMmSNaX8

## Hardware

- **Board:** ESP32-C3 Super Mini (select **ESP32C3 Dev Module** in the boards list) — this is the board the default case is designed for
- **LEDs:** WS2812B strip/matrix, 17 LEDs total, data on **GPIO 4**
- **Layout:** serpentine (zig-zag) rows, 4 wide × 4 tall

### Why LED 0 is ignored

The strip has **17** LEDs but only **16** are visible. LED 0 is a *sacrificial level shifter*: WS2812B data nominally needs 5 V logic, but the ESP32 outputs 3.3 V. The first LED sits close to the ESP32 and (powered appropriately) still reads the 3.3 V signal, then **regenerates the data line at full logic level** for the rest of the strip. It is kept permanently black and is invisible to the API — coordinates `(0,0)`–`(3,3)` map to physical LEDs 1–16.

## Deploying

### Requirements

- [Arduino IDE](https://www.arduino.cc/en/software) or [arduino-cli](https://arduino.github.io/arduino-cli/)
- **ESP32 board support** (Boards Manager: `esp32` by Espressif)
- Libraries (Library Manager):
  - **FastLED**
  - **ArduinoJson**
  - (`WiFi`, `WebServer`, `DNSServer`, `ESPmDNS`, `Preferences` ship with the ESP32 core)

### Flash

Arduino IDE: open `4x4.ino`, select your ESP32 board and port, Upload.

arduino-cli:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32c3 .
arduino-cli upload --fqbn esp32:esp32:esp32c3 -p COM5 .   # adjust port
```

## First-time Wi-Fi setup

The device stores Wi-Fi credentials in flash (NVS). On boot:

1. If credentials are stored, it tries to connect for ~20 seconds.
2. If there are no credentials, or the connection fails, it starts its own access point.

To provision:

1. Connect your phone/laptop to the open Wi-Fi network **`squareoctopus-4x4`**.
2. A captive portal should open automatically (otherwise browse to `http://192.168.4.1`).
3. Pick your network from the list (or type the SSID), enter the password, hit **Connect**.
4. The device reboots and joins your network.

### Finding the device

- **mDNS:** the device is reachable at **`http://squareoctopus-4x4.local`** — no IP needed (works out of the box on macOS, iOS, Android and most Linux distros; on Windows it works on recent versions or with Bonjour installed).
- **Router:** it appears as `squareoctopus-4x4` in your router's DHCP client list.
- **Serial monitor** (115200 baud): the IP is printed on boot.

### Status bar (middle row of the matrix)

| Display | Meaning |
|---|---|
| 🔵 Blue sweeping bar | Searching for / connecting to Wi-Fi |
| 🔴 Red blinking bar | Stored network not found / connection failed |
| 🟠 Orange pulsing bar | Setup (AP) mode — connect to `squareoctopus-4x4` |

The status bar follows the configured `rotation`.

To re-provision (e.g. new router), `POST /wifi/reset` — or just take the device out of range and it will fall back to AP mode after the timeout.

## API reference

All endpoints accept/return JSON. Base URL: `http://squareoctopus-4x4.local` (or `http://<device-ip>`).

Coordinates: `x` = column (0–3, left→right), `y` = row (0–3, top→bottom), at rotation 0. The `rotation` setting rotates the whole logical coordinate space.

Every successful call returns the current device state:

```json
{
  "ok": true,
  "power": true,
  "brightness": 96,
  "rotation": 0,
  "mode": "idle",
  "effect":   { "running": false, "name": "none", "durationMs": 1000, "repeat": 1, "cyclesCompleted": 0 },
  "sequence": { "loaded": false, "running": false, "frameCount": 0, "currentFrame": 0, "repeat": 1, "cyclesCompleted": 0 }
}
```

Errors return `{"ok": false, "error": "<message>"}` with an HTTP 4xx code.

### `GET /state`

Returns the state object above.

### `POST /state`

Set global options. All fields optional; any combination works.

```json
{
  "power": true,
  "brightness": 128,
  "rotation": 90,
  "stop": true,
  "clear": true
}
```

| Field | Type | Notes |
|---|---|---|
| `power` | bool | `false` stops playback and blanks the matrix |
| `brightness` | int 0–255 | Global brightness |
| `rotation` | int | One of `0`, `90`, `180`, `270` (degrees, clockwise). **Persisted across reboots.** |
| `stop` | bool | Stop any running effect/sequence, keep pixels |
| `clear` | bool | Blank the matrix |

### `POST /pixels`

Set individual pixels (manual mode). Stops any running playback.

```json
{
  "clearFirst": true,
  "show": true,
  "pixels": [
    { "x": 0, "y": 0, "r": 255, "g": 0, "b": 0 },
    { "x": 3, "y": 3, "r": 0, "g": 0, "b": 255 }
  ]
}
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `pixels` | array | required | Each needs `x`, `y`, `r`, `g`, `b` |
| `clearFirst` | bool | `false` | Blank before applying |
| `show` | bool | `true` | `false` buffers without displaying |

### `POST /effect`

Run a built-in effect.

```json
{ "name": "plasma", "durationMs": 5000, "repeat": 0 }
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `name` | string | required | One of `rainbowFade`, `breatheWhite`, `cometTrail`, `plasma`, `colorWipeRows` |
| `durationMs` | int | `5000` | Duration of one cycle |
| `repeat` | int | `1` | Number of cycles; `0` = run forever |

### `POST /sequence`

Upload and play a frame-by-frame animation. Limits: **64 frames**, **16 pixels per frame**.

```json
{
  "repeat": 0,
  "frames": [
    {
      "durationMs": 200,
      "clearFirst": true,
      "clearAfter": false,
      "pixels": [ { "x": 1, "y": 1, "r": 0, "g": 255, "b": 0 } ]
    },
    {
      "durationMs": 200,
      "pixels": [ { "x": 2, "y": 2, "r": 0, "g": 255, "b": 0 } ]
    }
  ]
}
```

| Frame field | Type | Default | Notes |
|---|---|---|---|
| `durationMs` | int | `100` | How long the frame stays on screen |
| `clearFirst` | bool | `true` | Blank before drawing the frame |
| `clearAfter` | bool | `false` | Blank after the frame's duration |
| `pixels` | array | required | Same pixel format as `/pixels` |

`repeat` (top level, default `1`): full sequence loops; `0` = endless.

### `POST /wifi/reset`

Clears the stored Wi-Fi credentials and reboots into setup (AP) mode. No body required.

```sh
curl -X POST http://squareoctopus-4x4.local/wifi/reset
```

---

made with care by **@squareoctopus**
