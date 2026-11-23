# Heart Beacon checklist

Tracks the v1 fixes in flight and the v2 build. Details live in
[`2026-11-23-heart-beacon-v2-plan.md`](2026-11-23-heart-beacon-v2-plan.md) (section names in brackets).
Tick a box only after its bench check passed.

## Your requests -> where they are handled

| Request | Where |
|---|---|
| More user-friendly, optimised, low-power firmware | v1 now: section A. v2: [Power], M1, M3, M4, M6 |
| Own MQTT server over a secured line, data + config, no Telegram | [Decisions 5-7], [MQTT topics], M2, M8 |
| Hosted JSON config: provision devices, settings, WiFi, BLE | [Config schema v1], [Claim flow], M2, M3, M6 |
| Public IP of our server only, not the old address | [Assumptions A1], private CA pinned (Decision 5) |
| Admin panel just for me | [Admin panel], M2 (minimal), M6 (complete) |
| Android app: always-on BLE, background, auto + fast connect | [Android app], M4, M5 |
| App: add/remove device, WiFi connect/disconnect, auto on/off timer, phone details, minimal settings | [Android app] screens table, M5, M6 |
| Office desk toy: save power after hours / by usage | [Power states] SLEEP/NAP, [Decisions 8], M6 |
| Messages from other devices still arrive after long idle or sleep | v1 now: A3. v2: router queue + coalescing, M6 |

## A. v1 firmware fixes (Arduino, branch `feature/wifi-power-fixes`)

### A1. 0.1.0: bugs + WiFi + power (bench-verified on COM6)
- [x] Rescan after WiFi drop (sticky `WIFI_STATUS` removed)
- [x] No reboot loop: offline -> deep sleep, 15 min timer + button wake
- [x] Fast reconnect from RTC-cached BSSID/channel: 1.1 s vs 3.9 s scan (measured)
- [x] CPU 80 MHz, `WIFI_PS_MAX_MODEM`, MQTT keepalive 60 s, WiFi config not written to flash (power: **not measured yet**)
- [x] Publishes no longer retained (verified `retain=0` on broker)
- [x] OTA upgrade-only (no downgrade by `version.txt`)
- [x] MQTT buffer 1024 B (600 B bridge message received)
- [x] Button: debounced tap = heart, hold = battery, wake press swallowed; heart queued if offline
- [x] Host tests `Code/test/test_logic.cpp` (button, battery, version, timers)

### A2. 0.1.1: one-flash connect + queued delivery (bench-verified on COM6)
- [x] Connect animation: 1 white flash instead of 3
- [x] Persistent MQTT session + QoS 1 subscribe: heart sent while beacon was offline arrived on reconnect
- [x] Stale `GO_DEEP_SLEEP` within 60 s of connect ignored (verified)
- [x] Own `Online` echo no longer flashes green
- [x] Clean MQTT disconnect before deep sleep (broker starts queueing at once)

### A3. To finish v1 (needs you)
- [ ] Broker: `queue_qos0_messages true` (beacons can only publish QoS 0; probe showed QoS 0 is dropped for offline beacons today)
- [ ] OK to flash COM6 with the real topic (boot sends `Online` + details -> Telegram forwards as "Miss you")
- [ ] Measure current on COM6 (meter in the battery lead): connected idle, deep sleep
- [ ] Clear stale retained message on the live topic after all beacons run >= 0.1.0 (`mosquitto_pub -r -n -t <topic>`)
- [ ] Commit branch + PR
- [ ] Release to other beacons by OTA: `firmware_0.1.1.bin` + `version.txt`
- [ ] Optional bridge fixes while Telegram lives: sender allow-list, forward only `Miss you` to non-admins

## B. v2 decisions (open questions, plan end)
- [ ] 1. v1 fleet size/location; can owners install the app
- [ ] 2. Who gets the app; Android versions/brands; any iPhone
- [ ] 3. Office WiFi type; outbound 8883/8443 open
- [ ] 4. LED power switch (respin or bodge); battery capacity; divider values
- [ ] 5. Firebase push allowed as Doze fallback
- [ ] 6. Circles: one per user enough
- [ ] 7. Missed hearts: play on wake (default 24 h, coalesced) or drop
- [ ] 8. Server/VPS and IP; backups; SSH-tunnel-only admin OK

## C. v2 milestones

### M0 contract + skeletons
- [ ] `protocol/PROTOCOL.md` + `vectors.json` (envelope, BLE framing, topics, config schema)
- [ ] Codecs in C / Kotlin / Python pass the same vectors
- [ ] Root `test.sh` / `test.ps1` runs every host test
- [ ] Check stock bootloader rollback support (sdkconfig read)

### M1 firmware core, radios off (ESP-IDF)
- [ ] Pure `hb_core`: button gestures, LED effects, battery w/ hysteresis, schedule, power policy + host tests
- [ ] Drivers: RMT LEDs, ADC, sleep, NVS; PM + tickless in `sdkconfig.defaults`
- [ ] Bench: measured light/deep-sleep current recorded in the plan

### M2 server MVP (VPS, public IP)
- [ ] Private CA + leaf cert with IP SAN; Mosquitto 8883 + dynsec per-client roles
- [ ] FastAPI + SQLite: device registry, claim, signed JSON config, router (queue, dedupe, receipts)
- [ ] Minimal admin (devices, config edit) on 127.0.0.1 via SSH tunnel; backups
- [ ] Bench: TLS fails without CA; two simulators exchange a heart over the public IP

### M3 firmware WiFi + MQTT
- [ ] Fast connect + backoff (no reboot), outbox, dedupe, signed-config apply
- [ ] Size gate <= 1.18 MB
- [ ] Bench: hearts both ways via VPS; AP power-cycle; 24 h soak

### M4 firmware BLE + handover
- [ ] NimBLE service (RX/TX), LE SC bonding window (5 s hold), claim + WiFi setup over BLE
- [ ] Phone-near -> WiFi off; phone-away -> WiFi; `tools/ble_cli.py` stands in for the app
- [ ] Bench: 20 forced handovers, no lost/doubled heart; NEAR current measured

### M5 Android app MVP
- [ ] `:core` relay/handover logic + codec tests (can start right after M0)
- [ ] Foreground service (`connectedDevice`), CDM presence, autoConnect GATT, MQTT, boot restart
- [ ] Screens: Login, Home (beacon card, Heart/Sorry/Annoy, events), Add device
- [ ] Bench: reconnect p95 < 10 s; force-stop + phone reboot recovery; overnight Doze delay log

### M6 schedule, smart power, settings, admin
- [ ] Auto on/off timer, nap when idle, brightness, WiFi add/forget, remove device
- [ ] App settings: notifications, phone details, log out
- [ ] Queued delivery on wake with coalescing
- [ ] Full admin panel (devices, users/circles, firmware, events, system)
- [ ] Bench: 3 schedule cycles; heart during SLEEP plays once on wake; 48 h soak

### M7 signed OTA + rollback
- [ ] Signed images, pinned CA, self-test then mark valid
- [ ] Bench: good update, failed self-test rolls back, power cut mid-download, bad signature refused

### M8 migrate v1, retire Telegram
- [ ] Rehearse v1 -> v2 OTA on a bench unit, then the fleet
- [ ] Stop the bridge, remove old OTA path and shared credentials, revoke bot token

### M9 hardware rev B (independent)
- [ ] LED rail load switch on GPIO10 (or bodge), 1 MΩ/1 MΩ divider
- [ ] Bench: sleep current < 0.1 mA measured
