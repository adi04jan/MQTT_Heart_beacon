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
- [x] Broker: `queue_qos0_messages true` (live 2026-11-23 via reload; probe delivered a queued QoS 0 message)
- [x] COM6 flashed with 0.1.1 on the real topic (verified `retain=0` on the live broker)
- [ ] Measure current on COM6 (meter in the battery lead): connected idle, deep sleep
- [ ] Clear stale retained message on the live topic after all beacons run >= 0.1.0 (`mosquitto_pub -r -n -t <topic>`)
- [x] Commit branch (`feature/v1.2-wifi-power-queue`, local) - [ ] push + PR
- [ ] 0.1.2 office-hours schedule (Mon-Sat 09:00-19:00 IST): host tests pass; bench test blocked until COM6 is back on USB
- [ ] Release to other beacons by OTA: `firmware_0.1.1.bin` + `version.txt`
- [ ] Optional bridge fixes while Telegram lives: sender allow-list, forward only `Miss you` to non-admins

## B. v2 decisions (answered 2026-11-23, see plan "Answers from the user")
- [x] 1. Fleet: 2 beacons, both owners on Android
- [x] 2. Phones: all Android 14+
- [x] 3. Office WiFi: plain password, ports open
- [x] 4. Hardware: no change for now (rev A, LED floor stays)
- [x] 5. FCM: decide after M5 measurement
- [x] NAP: off by default (`pwr.idle_nap_min` 0), user can enable per beacon (2026-11-24)
- [x] 6. Circles: one circle, everyone receives
- [x] 7. Missed hearts: play on wake, coalesced, 24 h
- [x] 8. Server: existing home server via public IP (SSH-tunnel admin)
- [x] Follow-up: home public IP is static (2026-11-24); port forwards 8883/8443 live

## C. v2 milestones

### M0 contract + skeletons
- [x] `protocol/PROTOCOL.md` + `vectors.json` (envelope, BLE framing, topics, config schema)
- [x] Codecs in C / Kotlin / Python pass the same vectors (envelope + frames in all three; config vectors in Python now, C `hb_config` in M3)
- [x] Root `test.sh` / `test.ps1` runs every host test (green from a clean clone, 2026-11-23)
- [x] Check stock bootloader rollback support (sdkconfig read): `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`; v2 must mark itself valid (plan, Risks)

### M1 firmware core, radios off (ESP-IDF)
- [x] Pure `hb_core`: button gestures, LED effects, battery w/ hysteresis, schedule, power policy + host tests (31 + 10, 2026-11-23)
- [x] Drivers: RMT LEDs, ADC, sleep, NVS; PM + tickless in `sdkconfig.defaults`; arduino 4 MB partitions; rollback mark-valid; IDF build 245,424 B (19 % of gate)
- [ ] Bench (needs COM6 on USB + current meter + user OK): light/deep-sleep current, button + timer wake, LED colours/timing

### M2 server MVP (home server, public IP)
- [x] Private CA + leaf cert with IP SAN (`deploy/mkca.sh`, chain + TLS handshake checked locally); separate Mosquitto 1.6.9 instance, password_file + pattern ACL (replaces dynsec)
- [x] FastAPI + SQLite: device registry, claim (admin-assigned owner only), signed JSON config, router (queue, dedupe, receipts, coalesce) - 177 tests
- [x] Minimal admin (devices, users, register/assign, config edit) on 127.0.0.1; `backup.sh` restore checked in WSL (age branch untested)
- [x] Real-broker ACL test: runs in WSL against apt Mosquitto 1.6.9 (`deploy/e2e-local.sh`), 2026-11-24
- [x] Deploy kit: `install.sh` (dry run by default, `--apply`, refuses to touch the shared broker, no root writes in hb-owned dirs), `uninstall.sh`, `hb-admin`, `RUNBOOK.md`; local e2e `smoke.py` SMOKE PASS 15/15 (lead-verified 2026-11-24)
- [x] Deployed on the ThinkCentre 2026-11-24 (2d20277): `hb-mosquitto` + `hb-server` enabled, shared broker / bridge / logger untouched (same start time), CA key moved off-box, smoke users + `HB-000001` registered; smoke.py 15/15 PASS through an ssh tunnel (TLS still verified against the public IP)
- [x] Router forwards TCP 8883 and 8443 to the server (live 2026-11-24; 8444 and 1884 closed from outside)
- [x] Real users `aditya` (Aditya) and `bulbul` (BulBul), circle `home`; both log in over the public IP (2026-11-24)
- [x] COM6 beacon `HB-756268` registered to `aditya`
- [x] Other beacon `HB-4F17E4` (MAC `A0:85:E3:4F:17:E4`, from the bridge journal) registered to `bulbul`. It runs 0.0.9 on
  `Airtel_Mohnish` (-84 dBm) and has not connected since at least 2026-10-28; the OTA server still offers 0.0.9
- [x] Bench: smoke.py from outside the LAN (office) against the public IP: SMOKE PASS 15/15 (2026-11-24)

### M3 firmware WiFi + MQTT
- [x] Fast connect + backoff (no reboot), outbox, dedupe, signed-config apply (hb_link / hb_config / hb_wire, host-tested; `hb_sim` + `fw/sim/bridge.py`)
- [x] Size gate <= 1.18 MB: 953,696 B (77 %) with TLS + esp-mqtt (lead-verified). Watch: NimBLE in M4 adds ~150-250 KB
- [ ] Bench (needs deploy + user OK to flash COM6, a flash backup first): hearts both ways via the server; AP power-cycle; 24 h soak

### M4 firmware BLE + handover
- [ ] NimBLE service (RX/TX), LE SC bonding window (5 s hold), claim + WiFi setup over BLE
- [ ] Phone-near -> WiFi off; phone-away -> WiFi; `tools/ble_cli.py` stands in for the app
- [ ] Bench: 20 forced handovers, no lost/doubled heart; NEAR current measured

### M5 Android app MVP
- [x] `:core` relay/handover logic + codec tests (30 tests, 2026-11-23)
- [~] Foreground service (`connectedDevice`), CDM presence, autoConnect GATT, MQTT, boot restart: written, compiles, not run on hardware
- [x] Real data layer: `HttpApi` (pinned CA, IP-SAN check, 401/403/409/429 messages), Keystore-sealed creds, `RealRepo` + MQTT retry; error-state screens (41 core + 29 app tests, lead-verified 2026-11-24)
- [ ] Drop the real `ca.crt` into `app/src/main/assets/hb_ca.pem` and the IP into `DEFAULT_HOST` after deploy; first on-phone run (Paho TLS on Android, bonding)
- [x] Nav-bar fix: scrolling lists end at the navigation bar (Home, Device settings, App settings; doubled inset dropped on Add device); emulator before/after, 3-button nav, 2026-11-24
- [x] Protocol round 2 (2026-11-24): phone `resp` ack for relayed uplinks, 896 B beacon cap, `net.wifi` out of cfg, lowercase state names, schedule past midnight, `mood.from` = display name, signature format, Mosquitto 1.6.9 ACL text, NAP off by default, `link` to router `{did, ble}`, state/tele to the phone, duplicate `show` -> `shown`. Spec + vectors, C / Python / Kotlin tests green (`./test.sh`); beacon outbox is logic only until M3/M4 radios
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
