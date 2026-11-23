# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A battery-powered ESP32-C3 "heart beacon" (9 NeoPixels + one button) and a Linux-side Telegram <-> MQTT bridge.
Pressing the button on one beacon lights up every beacon on the same topic and notifies Telegram users; Telegram
commands ("miss", "sorry", "annoy", "details", "go_deep_sleep") light up or control the beacons.

- `Code/MQTT_Heart_Beacon.ino`: the whole firmware (single Arduino sketch).
- `Code/credential.h`: all config: MQTT broker/user/pass/topic, message strings, OTA URLs, WiFi SSID/password lists.
  The committed copy holds **placeholders only**; keep it that way. Real values live only on the dev machine.
- `Telegram_server_service/`: laid out as filesystem paths for the server
  (`/usr/local/bin/mqtt_to_telegram.sh`, `/etc/systemd/system/telegram_heart.service`). The script also holds
  placeholder credentials (bot token, Telegram user IDs, MQTT creds).

No tests, no CI, no build system beyond arduino-cli / Arduino IDE.

## Build / flash

Board: MakerGO ESP32-C3 SuperMini. Toolchain: arduino-cli 1.4.1, core `esp32:esp32` 3.2.1 (IDF 5.4.2),
FQBN `esp32:esp32:makergo_c3_supermini` (USB CDC on boot, so `Serial` is the native USB port). The real
`credential.h` is kept outside the repo; drop it into the build copy, never into `Code/`.
Libraries: Adafruit NeoPixel, PubSubClient 2.8, ArduinoJson 7.x.

arduino-cli requires the sketch folder name to match the `.ino` name, and the folder here is `Code/`, so
`arduino-cli compile Code` fails ("main file missing from sketch"). Compile a copy (top-level files only;
`Code/test/` is host-only):

```bash
S=$(mktemp -d)/MQTT_Heart_Beacon && mkdir -p "$S" && cp Code/*.ino Code/*.h "$S/"   # then drop in real credential.h
arduino-cli compile --fqbn esp32:esp32:makergo_c3_supermini --warnings all --build-path "$S/../build" "$S"
arduino-cli upload  --fqbn esp32:esp32:makergo_c3_supermini -p COMx --input-dir "$S/../build" "$S"
```

App size is ~1.18 MB of the 1.25 MB OTA slot (90%); watch growth.

Host tests (pure logic in `Code/beacon_logic.h`: button classifier, battery mapping, version compare, timers):

```bash
g++ -std=c++17 -Wall -Wextra -Werror -o /tmp/test_logic Code/test/test_logic.cpp && /tmp/test_logic
```

Bench testing without disturbing users: append `#undef MQTT_TOPIC` / `#define MQTT_TOPIC "heart_beacon/bench_com6"`
to the build copy's `credential.h`. The Telegram bridge and other beacons only listen on the real topic. Every boot
on the real topic publishes `Online` + details, which the bridge forwards to Telegram users.

## OTA

Runs on every boot after WiFi connects. Flow: GET `<base_url>version.txt`, `version_is_newer()` against
`CURRENT_FIRMWARE_VERSION` (upgrade only; a server behind the device is ignored), then `HttpsOTA` fetches
`<base_url>firmware_<version>.bin` and restarts. To release: bump `CURRENT_FIRMWARE_VERSION`, export the binary,
publish it as `firmware_<ver>.bin` and update `version.txt`. Rolling back means shipping a higher version number.

OTA is plain HTTP (`base_url` is `http://`) and unsigned. It works because esp_https_ota only demands that *a*
cert is configured; with an http URL the client ignores it. The embedded `server_certificate` (expired 2021 Let's
Encrypt X3) is therefore just a placeholder that keeps that check happy; don't "fix" it without testing OTA.
`url` in `credential.h` is unused.

## MQTT protocol (single shared topic)

All devices and the bridge publish and subscribe on one topic (`MQTT_TOPIC`).

- Device -> topic: `{"id": <WiFi MAC>, "message": <msg>, "volt": <battery V>}`
- Bridge -> topic: `{"id": <tg first name>, "message": <msg>, "raw": <original tg text>, "user": <tg user id>}`
- The `raw` field is the loop breaker: the bridge forwards device messages to Telegram and ignores anything with `raw`.
- Devices receive their own publishes back. That echo is the design: a button press flashes the presser's
  beacon too.

Message strings are defined twice, in `credential.h` (`MQTT_MSG_*`) and in the script (`MQTT_MSG_*`), and
must match exactly. The device accepts both capitalisations for Sorry/Annoy/Details via `_1`/`_2` variants.

| Firmware constant | Value | Sent by | Device reaction |
|---|---|---|---|
| `MQTT_MSG_1` | `Miss you` | button tap, bridge "miss" | red heartbeat |
| `MQTT_MSG_2` | `Online` | device on every MQTT (re)connect | green heartbeat |
| `MQTT_MSG_3` | `BATT_STATUS` | device, periodic (~30 min) | none |
| `MQTT_MSG_4` | `DEEP SLEEP` | device, right before deep sleep | none |
| `MQTT_GO_SLEEP` | `GO_DEEP_SLEEP` | bridge | purple pulse, then deep sleep |
| `MQTT_MSG_SORRY_*` / `ANNOY_*` | `Sorry` / `Annoy` | bridge | orange / cyan heartbeat |
| `MQTT_MSG_ONLINE_*` | `Details` | bridge | replies `"<fw ver> \| SSID: .. \| RSSI: .."` |

Publishes must use the `(topic, const uint8_t*, len, retained)` overload: `publish(topic, char*, n)` silently
binds to `publish(topic, payload, retained = n != 0)`. Firmware <= 0.0.9 retained every message that way, so the
live topic still holds a stale retained device message until someone clears it (`mosquitto_pub -r -n -t <topic>`).
MQTT buffer is 1024 B (`setBufferSize`); longer bridge messages are still dropped.

## Firmware runtime model (0.1.1)

- `setup()`: CPU 80 MHz, low-battery check, battery bar (skipped on timer wake), WiFi, OTA check, MQTT config.
  WiFi: `WIFI_PS_MAX_MODEM`, `persistent(false)`. `connect_wifi()` first tries the last AP's BSSID + channel from
  RTC memory (`cached_*`, survives deep sleep), then one scan trying each known SSID once.
- Offline handling never reboots: no WiFi at boot (3 rounds, 1 on timer wake) or no MQTT for 5 min in `loop()` ->
  deep sleep with a 15 min timer + button wake. A timer wake is "quiet" (no animations, one attempt).
- `loop()`: 10 ms tick; periodic work uses `every()` on `millis()` (battery check 15 s, `BATT_STATUS` 30 min).
  MQTT reconnect is one attempt per 5 s, so the button keeps working while offline.
- Commands from `callback()` set `STATUS_FLAG` / `SLEEP_FLAG`; `loop()` acts on them. LED effects run inline
  inside `callback()` (~3 s blocking).
- Button (GPIO0, active low) via `Button` in `beacon_logic.h`: tap (< 600 ms) = send `Miss you` (queued in RTC
  memory as `pending_heart` if offline); hold = battery bar. The wake-up press is swallowed.
- Deep sleep: on `GO_DEEP_SLEEP` or battery <= 3.3 V (button wake only), or offline (timer + button).
- MQTT: stable client ID `heart-<MAC without colons>`, persistent session (cleanSession=false), QoS 1 subscribe,
  keepalive 60 s, clean `disconnect()` before deep sleep, so the broker queues messages while the beacon is away.
  Beacons publish QoS 0 (PubSubClient limit), so the broker needs `queue_qos0_messages true` for beacon-to-beacon
  hearts to be queued. A `GO_DEEP_SLEEP` within 60 s of connect is treated as a stale queued command and ignored;
  the beacon's own `Online` echo plays no effect.

Pins: button GPIO0, NeoPixel data GPIO2 (`NEO_KHZ400`), battery ADC `A3` (GPIO3) through a 1:2 divider,
GPIO10 free (the plan's LED power-switch pin).

Battery: `check_batt_voltage()` keeps a 10-sample moving window shared by **every** caller (each call shifts
it) and returns mV already multiplied by the divider ratio (`sum / 5`). Thresholds: `MIN_MV` 3300,
`LOW_BATT_MV` 3450, `MAX_MV` 4150. The 9 WS2812s draw several mA even when dark and are not power-gated, so
deep sleep is not µA on this hardware.

## v2

`docs/plans/2026-11-23-heart-beacon-v2-plan.md`: ESP-IDF rewrite, own TLS MQTT + config server, Android BLE app.

## Bridge service (server)

`mqtt_to_telegram.sh` runs two loops: `mqtt_listener` (background, `mosquitto_sub` piped into `jq`) and
`telegram_poller` (foreground, `getUpdates` long-poll + 10 s sleep). Admin vs. regular users is decided by the
`USERS` map plus a hard-coded admin ID inside `mqtt_listener`; keep those consistent when editing.

Deploy (server needs `mosquitto-clients`, `jq`, `curl`):

```bash
sudo install -m 755 Telegram_server_service/usr/local/bin/mqtt_to_telegram.sh /usr/local/bin/
sudo install -m 644 Telegram_server_service/etc/systemd/system/telegram_heart.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now telegram_heart
journalctl -u telegram_heart -f
```

## Repo conventions

Feature branches (`feature/...`) merged into `main` via GitHub PRs. Existing commits carry no Co-Authored-By
or Claude trailers.
