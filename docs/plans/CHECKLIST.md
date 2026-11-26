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
- [x] Bridge quieter (2026-11-24, deployed): non-admins get only real hearts, admin skips routine `BATT_STATUS`, `-R` drops the
  stale retained message the daily restart replayed at midnight. Still open: sender allow-list

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
- [x] Size gate: 953,696 B with TLS + esp-mqtt (M3); 1,161,952 B with NimBLE (M4) against the new 1.9 MB gate (v1 layout dropped 2026-11-26)
- [ ] Bench (needs deploy + user OK to flash COM6, a flash backup first): hearts both ways via the server; AP power-cycle; 24 h soak

### M4 firmware BLE + handover
- [x] NimBLE service (RX/TX), LE SC bonding window (5 s hold), claim + WiFi setup over BLE: bench 2026-11-26 on COM6 with
  the real app (Add flow: `wifi.set` joined, claim, cfg 2 and 3 verified with the deployed key) and a bleak probe from the PC
- [~] Phone-near -> WiFi off after the 30 s hold (bench: `link down`, no hang); phone-away -> WiFi after 60 s: host-tested only.
  The real app stands in for `tools/ble_cli.py`
- [x] Router -> phone -> BLE -> beacon delivery: a heart sent as the other circle member played, `shown` back in 0.6 s (2026-11-26)
- [ ] Beacon button -> BLE -> phone relay -> router (needs a button press on COM6)
- [ ] Bench: 20 forced handovers, no lost/doubled heart; NEAR current measured

### M5 Android app MVP
- [x] `:core` relay/handover logic + codec tests (30 tests, 2026-11-23)
- [~] Foreground service (`connectedDevice`), CDM presence, autoConnect GATT, MQTT: run on the phone 2026-11-26 (CDM chooser, bonding, relay both ways). Boot restart not tried
- [x] Real data layer: `HttpApi` (pinned CA, IP-SAN check, 401/403/409/429 messages), Keystore-sealed creds, `RealRepo` + MQTT retry; error-state screens (41 core + 29 app tests, lead-verified 2026-11-24)
- [x] Real `ca.crt` in `hb_ca.pem`, server address from the gitignored `local.properties`; first on-phone run (Paho TLS on Android, bonding), 2026-11-25/26
- [x] Home = the beacon twin (2026-11-24, 802c694): 3D brass cage from the real build's photos, firmware LED effects byte-exact
  (`fx` vectors shared by C and Kotlin), tap/double/triple/hold gestures like the button, dashboard over the left lobe,
  settings below. Design polish pass in progress (impeccable)
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

## D. Review 2026-11-26 (Fable review of fw, app, server, design)

Every finding and idea from the review, in work order. Tick only after its check passed (host test, bench, or
phone). `[u]` = needs the user (a decision, hardware, or a server deploy OK).

### D1 Firmware bugs (host-tested in `fw/test/host/test_core.c`)
- [x] R1 A mood whose WiFi session drops before its PUBACK is parked forever: `pump()` re-arms a `sends == 2` mood
  once `due` passes while a path is live, and never fails over to a path that is not live (host: 2 tests, incl. the review scenario)
- [x] R2 24.86-day `millis` wrap: expired deadlines (`pair_until`, `grace_until`, and any other `hb_due` deadline)
  are clamped to `now` so PAIRING never reopens and schedule sleep is never blocked by a wrapped deadline (host: 60 days of uptime, red without the fix)
- [x] R3 Re-claim from a bonded phone (the bond is the proof); the app shows the beacon's real `err` instead of
  "did not answer" (host test; app shows the beacon's `err`)
- [x] R4 NAP check-in: the 10 s stay counts from MQTT up (capped 45 s), so a check-in always fetches queued hearts (host test)
- [~] R5 Brownout reset goes to LOWBATT sleep (after 2 in a row, `RTC_NOINIT_ATTR` count), never the battery-bar /
  WiFi start loop (core host-tested: RESET wake keeps the outbox; port written, bench pending)
- [~] R6 Before deep sleep in NEAR, the final `state` (with `wake`) is flushed over BLE (wait for NOTIFY_TX, bounded
  1.5 s), then the link is terminated (written, builds; bench pending)
- [~] R7 Task watchdog panics (main task subscribed): a hung loop reboots instead of draining the battery (config + main subscribed, builds; bench pending)
- [~] R8 Hardware factory reset: hold 15 s, white flash, everything erased incl. bonds (15 s hold: white flash, then factory reset on release; host tests for the gesture and the action; bench pending)
- [~] R9 Core dump to the `coredump` partition; `tele` carries a flag when one is waiting (config + `dump erase` console command, builds; bench pending)
- [~] R10 Console polls stdin only while a USB host is attached (no 10 Hz wakes on battery) (written, builds; bench pending)
- [x] R11 Stale size comments (`fw/sdkconfig.defaults` 1.18 MB, CLAUDE.md size gate text) (sdkconfig.defaults, CLAUDE.md)
- [x] R12 Beacon falls back to WiFi after N min without a `link{up}` from the phone (the phone may be in Doze); depends
  on A1's result (host test; the app sends `time` every 4 min, RelayTest)
### D2 Power and latency (firmware + config defaults)
- [~] P1 BT modem sleep: `CONFIG_BT_CTRL_MODEM_SLEEP`, mode 1, main XTAL LP clock, XTAL powered in light sleep; NEAR
  light sleeps (config, builds; bench + P2 pending)
- [ ] P2 [u] Measure NEAR / AWAY / sleep current with a meter in the battery lead (P1 before and after)
- [x] P3 Faster BLE: `ble.conn_ms` 180, `ble.latency` 0 defaults (PROTOCOL.md 7 table + vectors + server defaults +
  all three codecs) (vectors, PROTOCOL.md, C / Python / Kotlin defaults; all suites green. The live beacon keeps cfg 3 until the server re-signs it, S7)
- [~] P4 App writes every frame but the last of a message without response (`GattLink`), one interval per message (app writes MORE frames without response only if RX advertises it; firmware RX advertises it; bench pending)
### D3 App bugs and reliability
- [ ] A1 Doze: force-idle the phone and send a heart; if it stalls, keep the MQTT session alive (AlarmManager ping or
  a beacon-side heartbeat timeout, R12)
- [~] A2 `near(false)` falls back to the last router state / "Last seen" instead of a stuck "Near" (written; phone check pending)
- [~] A3 Lost bond (CCCD write fails): retry the GATT after a delay and show "Hold the button 5 s to pair again" (written: GATT reopened after 30 s, `needsPair` for U3; phone check pending)
- [~] A4 `mqttErrorOccurred` / refused publish trigger a reconnect, not just "server down" (written; phone check pending)
- [x] A5 PUBACK watchdog backs off 5 s, 15 s, 60 s (no TLS thrash on slow mobile data) (RelayTest)
### D4 Server and deploy
- [x] S1 Router pings (`get info`) a beacon after two unanswered `show`s instead of waiting up to `tele_min` (router test; WSL e2e SMOKE PASS)
- [x] S2 Session-table expiry sweep (API test)
- [x] S3 Broker 8883 listener: `tls_version tlsv1.2`, `max_connections` (WSL e2e with the shipped config: SMOKE PASS)
- [x] S4 Cert expiry check (`openssl x509 -checkend`, cron) with a warning; leaf lasts to about 2029-02 (startup warning + admin banner under 60 days, admin test; renewal is RUNBOOK "leaf")
- [ ] S5 [u] A DDNS name as a second SAN and in `srv.hosts` (survives an ISP IP change); needs a name from the user
- [x] S6 [u] Unattended security upgrades check for the public Mosquitto 1.6.9 (read-only report, user applies) (2026-11-26: unattended-upgrades enabled and ran today; mosquitto 1.6.9 ESM and openssl ESM patched)
- [ ] S7 [u] Deploy S1-S4 and the protocol/server parts of the ideas below to the home server (user OK per deploy)

### D5 Product ideas
- [~] I1 Phone-only partner mode: hearts arrive as notifications with the app closed and no beacon (a light
  foreground service, or FCM after A1) (app: BeaconService stays up signed in without a beacon, "Listening for hearts"; phone check pending)
- [~] I2 Echo: when the partner's beacon actually plays your heart (`rcpt shown`), your cage gives one soft pulse (router: show echo to the sender's beacons on `shown`; fw plays ECHO; tests. App/twin: pending) App: the twin plays the echo when one of your hearts turns Shown.
- [~] I3 Hold-together: both press within 10 s and both cages breathe in sync for a minute (2-5 s hold = together; router syncs within 10 s with `at`; fw breath in phase; tests. App button/twin: pending) App: Together key, notification reply, twin invite breath.
- [x] I4 Goodnight / good-morning: the last heart before schedule stop replays dimmed at schedule start (fw: last shown mood replays at 30 % on the first schedule wake; host test)
- [~] I5 Streak ember: one faint LED on days you both sent a heart; the hold bar can show the streak (router streak from event history, `streak{n}` on change and on connect; fw ember pixel; tests. App display: pending. The hold bar keeps showing the battery) App: the twin draws the ember.
- [~] I6 Custom mood: a fourth gesture (tap then hold) with colour and pattern chosen in the app, carried in the
  signed cfg (tap then hold = custom; cfg `mood.rgb`/`mood.pat` in vectors + 3 codecs; router carries the colour; fw plays it; tests. App colour picker: pending) App: Colour key sends your colour; settings "Your colour" swatches + Breathe.
- [~] I7 Presence glow: an optional dim pixel while the partner's phone is near their beacon (router `presence` on near changes; fw pixel 4 with `led.presence`; tests. App toggle: pending) App: "When they're near" switch; the twin draws the pixel.
- [~] I8 Quiet reply: Heart / Sorry actions on the arrival notification (app: Heart / Sorry (Together for a together) actions on mood notifications, ReplyReceiver; phone check pending) Phone 2026-11-26 16:32: a heart sent as BulBul from the PC posted "BulBul sent a Heart" with Heart and Sorry actions (dumpsys); tapping one is for the user.
- [~] I9 Anniversary: the server plays a one-off special effect on both beacons at a set time (admin form per circle, setting table; router plays `celebrate` once a year on the minute; tests)
- [~] I10 Battery nudge: the partner's app says when your beacon's battery is low (router `batt` to the other members once at <= 15 % (again after > 30 %); test. App notification: pending) App: "<name>'s heart is running low" notification.
### D6 App UX and design (impeccable, look at the phone before ticking)
- [~] U1 Settings preview on the twin: the brightness slider lights the cage live; the schedule toggle shows the
  asleep look (brightness: the cage lights steady red at the dragged value; the schedule preview is not done; phone look pending)
- [~] U2 Delivery line under the heart: "On its way", "Glowing on <name>'s desk", "Waiting till 09:00" (needs R6's
  `wake`) instead of a bare "Queued" (marks read "On its way" / "Glowing on their desk" / "Didn't reach them"; the partner's wake time is not known to this phone, so no "till 09:00"; phone look pending)
- [~] U3 Re-pair plate: the cage draws the blue pair pulse with "Hold the button 5 s" (with A3) (Network readout "Pair again / Hold its button 5 s", the twin pulses blue while `needsPair`; phone look pending)
- [ ] U4 Signal as a three-bead solder meter; raw did / dBm / fw in a "Workshop" drawer Phone 2026-11-26: Workshop row shows the did (rssi 0 = WiFi off in NEAR is hidden); beads only show on WiFi.
- [ ] U5 Night stage: the felt darkens with a moonlit rim while the beacon sleeps; cooler paper cast in light theme
- [~] U6 Haptic heartbeat on send and on arrival, timed from `Fx` (vibration ticks at each pulse peak on arrival, a slow swell for together, touch-feedback usage; phone check pending)
- [ ] U7 Near / Away transition as a brass thread drawing between phone and cage, not a text flip
- [~] U8 First-run coach mark: a brass tag tied to the cage, "tap me", gone after the first send ("Tap the heart" tag under the cage until the first send; skipped if the history has your sends; phone look pending) Phone: correctly absent for a user with sends in the history.

Effect engine (shared by R8 and the ideas): `reset`, `echo`, `breath`, `celebrate`, `custom`, per-effect `level`,
idle overlay; 42 new `fx` vectors generated from hb_fx.c, Kotlin Fx matches all 97. Protocol: PROTOCOL.md 3/4/7 and the
change log; 23 known types. Firmware, server and app suites green (2026-11-26).

Still open in D (2026-11-26 16:15): phone looks and checks (A1-A4, I1, I8, U2, U3, U6, the new Colour / Together keys and
"Your colour" settings), U1 U4 U5 U7 U8 design work, firmware bench on COM6 (it is off USB: R5-R10, P1, P2, P4, R8), S5 and
S7 need the user, and none of this is committed yet.

Phone pass 2026-11-26 16:20: the Colour and Together keys sit in the key row (Together's bar made longer than Battery's);
"Your colour" swatches + Breathe + "When they're near" render (a doubled label removed). Found and fixed: the app never
read the beacon's networks (now `wifi.list` when NEAR: the office network shows as "Saved on the beacon"), and `rssi` 0 showed as
"0 dBm".

Bench 2026-11-26 16:50 (new firmware on COM6, user at the beacon): Remove in the app (server unclaim + factory reset
over BLE), a long button hold for the pairing window, Add again (bond, Wi-Fi, claim, cfg 6), back to `near` over BLE with
relayed state/tele. Boot showed the `reset` wake and BT modem sleep on the main XTAL; a stale v1 core dump (other image
SHA) was found by the new coredump check and erased.

### D8 Updates from the cloud (user request 2026-11-26)
- [ ] O1 App self-update: the server hosts signed APKs (version + sha256 in an endpoint); the app checks on start and
  daily, downloads, verifies, and installs through PackageInstaller (REQUEST_INSTALL_PACKAGES, a user confirm each time)
- [ ] O2 Beacon OTA over BLE from the app (with M7): the app fetches the image named by the signed `fw` config block,
  checks its sha256 and streams it over the BLE link in chunks with acks; the beacon writes the passive slot, verifies
  the signature, reboots, self-tests, marks valid (rollback otherwise). WiFi HTTPS OTA stays the default path
- [ ] O3 Release flow: one command publishes a new app or firmware to the server (admin), so both update from the cloud

### D7 Hardware (M9, the user's call: "no hardware change for now")
- [ ] H1 [u] Rev B: LED rail load switch on GPIO10 and a 32 kHz crystal (dark draw mA -> uA, BLE on the RTC clock)
