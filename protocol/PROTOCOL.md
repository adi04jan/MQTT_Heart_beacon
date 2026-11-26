# Heart Beacon wire protocol v1

Normative contract between the beacon firmware (`fw/`), the server (`server/`) and the Android app
(`android/`). Where another document disagrees with this file, this file wins. `vectors.json` beside it
holds the golden vectors; the C, Kotlin and Python test suites all check them (`./test.sh`).
MUST / MUST NOT / MAY are used in the RFC 2119 sense.

## 1. Identifiers

| Name | Format | Rule |
|---|---|---|
| `did` | `HB-` + 6 uppercase hex | Last 3 bytes of the factory base MAC (`esp_efuse_mac_get_default`, the WiFi STA MAC). `a0:76:4e:3f:a2:c1` -> `HB-3FA2C1`. Also the BLE name. |
| `id` | 16 lowercase hex (64 bits) | Random per message, except where section 3 says it is reused. |
| `uid` | server-assigned string | User id in MQTT topics. |

## 2. Envelope

One JSON object per message, the same bytes on MQTT and BLE:

```
{"v":1,"t":"<type>","id":"<16 hex>","ts":<epoch s, 0 if clock unknown>, ...body}
```

### 2.1 Encoding (senders)

- UTF-8, RFC 8259 JSON, at most **1024 bytes**. A message the **beacon** originates is at most **896 bytes**,
  so the `relay` the phone wraps around it (at most 82 bytes of overhead: `ts` 4294967295) always fits in 1024.
  Encoders check their limit and refuse to send above it; receivers accept up to 1024 (2.2) whoever sent it.
- Canonical form, which the vectors check byte for byte: no whitespace; keys `v`, `t`, `id`, `ts` first and
  in that order, then the body keys in the order the sender lists them; non-ASCII characters written as raw
  UTF-8, not `\u` escapes; `/` not escaped.
- Integers are written without fraction or exponent. Strings in messages carry no control characters.

### 2.2 Decoding (receivers)

Checks run in this order; the first failure is the result and the message is dropped.

| # | Check | Error |
|---|---|---|
| 1 | Raw length <= 1024 bytes | `too_big` |
| 2 | Parses as one JSON value that is an object; only JSON whitespace (space, tab, CR, LF) after it; no `NaN` / `Infinity` | `bad_json` |
| 3 | `v` is the integer 1 | `bad_v` |
| 4 | `t` is a non-empty string | `bad_t` |
| 5 | `id` is a string of exactly 16 characters `[0-9a-f]` | `bad_id` |
| 6 | `ts` is an integer, 0 <= `ts` <= 4294967295 | `bad_ts` |

- Key order is free. Unknown keys MUST be ignored (they are not an error). The body is every key except
  `v`, `t`, `id`, `ts`.
- A message that passes 1-6 but whose `t` is not in the message table below MUST be answered with
  `{"v":1,"t":"resp","id":<new id>,"ts":<now>,"re":<request id>,"ok":false,"err":"unknown_t"}`. A message
  that fails 1-6 is never answered (there may be no `id` to answer).
- Unspecified (no vector, receivers MAY differ): invalid UTF-8, a byte-order mark, duplicate keys,
  `1.0` or `1e3` where an integer is expected, leading zeros.

### 2.3 Versioning

Additive changes (new keys, new `t`) stay `v:1`. A breaking change is `v:2` on topic prefix `hb/v2`.

## 3. Messages

| `t` | Direction | Body | Notes |
|---|---|---|---|
| `mood` | beacon or app -> router; router -> phone | `k`: `heart` \| `sorry` \| `annoy` \| `custom` \| `together`; with `custom` also `rgb` (`"#rrggbb"`) and `pat` (0 pulses, 1 breaths); `from` (router -> phone only): the sender's display name (the user's `name`, not the `uid`) | tap = heart, double = sorry, triple = annoy, tap then hold = custom (the sender's `mood.rgb` / `mood.pat`), hold let go between 2 s and 5 s = together. Its `id` is the event id. |
| `show` | router -> beacon | `k`: a mood `k`, or `echo` (your heart just played on another desk), `celebrate` (the circle's anniversary); `custom` carries `rgb` and `pat`; `together` carries `at` (epoch s the shared breathing starts) when both pressed within 10 s, none for a one-sided invitation | `id` = the originating `mood` id (a fresh id for `echo`, `celebrate` and a synced `together`). Beacon dedupes on `id` (ring of 16 in RTC memory): a `show` whose `id` is already in the ring is answered with `shown` again and its effect is **not** replayed. |
| `shown` | beacon -> router | none | `id` = the `show` id. Delivery receipt, sent for every `show` the beacon accepts, duplicates included. |
| `rcpt` | router -> sender's phone | `to` (did), `st`: `queued` \| `shown` \| `expired` | `id` = the event id. |
| `tele` | beacon -> router; router -> owner's phone (in `relay`) | `mv`, `pct`, `rssi`, `fw`, `hw`, `cfg`, `up_s`, `rst`, `heap` | every `tele_min`, and on each connect |
| `state` | beacon -> router (retained, LWT); router -> owner's phone (in `relay`) | `st`: `pairing` \| `near` \| `away` \| `offline` \| `nap` \| `sleep` \| `lowbatt`, or `lost` (LWT only); `path`: `wifi` \| `ble`; `wake` (epoch of next wake) | sent on every transition, before every deep sleep. `st` is lowercase and exact (`vectors.json` `envelope.states`): a receiver drops a `state` with any other `st`. |
| `cfg` | router -> beacon (retained); phone -> beacon over BLE | `n`, `b` (base64url config bytes, section 7), `s` (signature over `b`'s bytes, section 7.1) | rejected if signature bad, `did` mismatch, or `n` <= stored. Never inside a `relay`. |
| `time` | router or phone -> beacon | `tz` (minutes east of UTC) | the envelope `ts` is the time. `tz` is informational: the schedule runs on the config's `sched.tz`. |
| `sleep` | router -> beacon | `for_s` (0 = until schedule or button) | |
| `get` | either -> beacon | `what` | `get info` -> `resp` with `d`: `did`, `fw`, `hw`, `pv`, `min_pv`, `claimed` |
| `resp` | beacon -> requester; phone -> beacon (relay ack) | `re` (request id), `ok`, `err` (when not ok), `d` (data) | `err` values: `unknown_t`; later milestones add more. The phone's relay ack is `{re: <beacon message id>, ok: true}` (section 4). |
| `claim` | phone -> beacon, BLE only | `host`, `mport`, `aport`, `user`, `pass` | from a bonded phone at any time; a new claim replaces the stored server credentials (the server issues a new password on every claim) |
| `wifi.scan`, `wifi.list`, `wifi.set`, `wifi.forget`, `wifi.use` | phone -> beacon, BLE only | `ssid`, `psk`, `on` | max 4 networks; `wifi.set` replies with join result and reason; PSKs never read back. The only way WiFi reaches a beacon. |
| `link` | phone -> beacon; phone -> router | to the beacon: exactly `{up}` (phone has its server link); to the router: exactly `{did, ble}` (BLE link to that beacon is up) | drives handover |
| `relay` | phone <-> router | `did`, `m` (inner envelope, a JSON object) | wrapper for anything carried over BLE, and router -> phone for the beacon's `state` and `tele` (section 4). Never carries `cfg`. |
| `reset` | phone -> beacon, BLE only | `what`: `wifi` \| `factory` | |
| `streak` | router -> beacon; router -> phone | `n`: days in a row on which every member of the circle sent a heart | on every change and on each beacon connect; `n` > 0 lights the ember pixel |
| `presence` | router -> beacon; router -> phone | `uid`, `near` (bool): another member's phone is near their beacon | on every change; the beacon shows it only with `led.presence` |
| `batt` | router -> phone | `did`, `name` (owner's display name), `pct` | once when another member's beacon reports `pct` <= 15 (again after it was above 30) |

Those 23 values of `t` are the known set (`vectors.json` `envelope.types`).

## 4. Transport and handover

| Rule | Value |
|---|---|
| Uplink path | BLE if the link is encrypted and the last `link.up` is true; else WiFi. No ack in 5 s: resend the same `id` on the other path. |
| Uplink ack | WiFi: the MQTT PUBACK of that publish. BLE: the phone's `resp{re: <id>, ok: true}`, sent over BLE once the broker has PUBACKed the phone's `relay` of that message (none while the phone's server link is down, so the beacon's 5 s rule moves the message to WiFi). The phone acks every beacon message it relays; the beacon ignores an ack for an `id` it is not waiting on. |
| Enter NEAR | bonded link + `link up` held 30 s, then stop WiFi. |
| Leave NEAR | link lost: fast advertise 30 s, wait 60 s total, then start WiFi. A pending uplink skips the wait. |
| Phone upstream down 60 s | beacon starts WiFi while keeping BLE. |
| Downlink path (router) | most recently reported live path for that beacon. No `shown` in 10 s: try the other. |
| Device status (router -> phone) | every `state` and `tele` the router accepts from a beacon, on either path, goes to the owner's phone as `relay{did, m}` on `u/{uid}/down`. The phone shows it and never writes it to the beacon. |
| Dedupe | router: unique (`did`, `id`) per event. Beacon: `id` ring of 16; a duplicate `show` gets `shown` again without replaying the effect. |
| Ordering | none guaranteed, none needed. Effects are idempotent; the router orders by receive time. |
| Queue while asleep | router holds pending rows up to 24 h and flushes when `state` shows the beacon up. A burst arriving within 3 s plays each `k` once. |
| Echo | when a delivery becomes `shown`, the router sends `show{k:"echo"}` (fresh id, best effort, not tracked) to the sender's own beacons. |
| Together | a `together` mood is delivered as `show{k:"together"}` without `at` (an invitation: 3 breaths). If another member sends `together` within 10 s, every beacon of both gets `show{k:"together", at: <now + 2 s>}` and breathes 15 times in step (each beacon aligns its breath to `at` on its own clock). |
| Good morning | the beacon keeps the last mood it played; at the first schedule start after it, it replays it once at 30 % brightness. |

## 5. MQTT

MQTT 5, TLS on 8883 with the private CA. Keepalive 240 s for beacons, 120 s for phones. Beacon client
buffer 2048 B. QoS 1 everywhere.

| Topic | Publisher -> subscriber | Retain | Carries |
|---|---|---|---|
| `hb/v1/d/{did}/up` | beacon -> router | no | `mood`, `shown`, `tele`, `resp` |
| `hb/v1/d/{did}/state` | beacon -> router, LWT `st:lost` | yes | `state` |
| `hb/v1/d/{did}/down` | router -> beacon | no | `show`, `time`, `sleep`, `get` |
| `hb/v1/d/{did}/cfg` | router -> beacon | yes | `cfg` |
| `hb/v1/u/{uid}/up` | phone -> router | no | `mood`, `relay`, `link` |
| `hb/v1/u/{uid}/state` | phone -> router, LWT `on:false` | yes | `{"on":true}` (not an envelope) |
| `hb/v1/u/{uid}/down` | router -> phone | no | `mood`, `rcpt`, `relay` (downlinks for BLE delivery; the beacon's `state` / `tele` as device status) |

| Client | Login (`%u`) | Client id | Session | ACL |
|---|---|---|---|---|
| Beacon | its `did` | `d-{did}` | clean start | `pattern write hb/v1/d/%u/up`, `.../state`; `pattern read hb/v1/d/%u/down`, `.../cfg` |
| Phone | its `uid` | `u-{uid}-{install}` | persistent, expiry 24 h | `pattern write hb/v1/u/%u/up`, `.../state`; `pattern read hb/v1/u/%u/down` |
| Router | `router` | `router` | clean, plain listener 127.0.0.1:1884 only | `user router` / `topic readwrite hb/v1/#` |

### 5.1 Broker

A **separate Mosquitto 1.6.9 instance** for v2 (unit `hb-mosquitto`, config `/etc/mosquitto-hb/`, shipped as
`deploy/mosquitto-hb.conf` and `deploy/acl`), beside the shared broker it never touches. 1.6.9 has MQTT 5 but
no dynamic security, so:

- Authentication: `allow_anonymous false` and a `password_file` (mosquitto_passwd 1.6 format) that the server
  rewrites from its database on every user, claim or unclaim change, then signals the broker to re-read
  (SIGHUP). A beacon's password is created at claim and deleted at unclaim.
- Authorization: one static `acl_file` of `pattern` lines, `%u` = the MQTT username, which is the `did` for a
  beacon and the `uid` for a phone (table above), plus the router's `topic` line.
- Pattern lines apply to every login, so each login also gets the *other* class's subtree under its own name:
  a beacon may write `hb/v1/u/HB-XXXXXX/up`, a phone may read `hb/v1/d/{uid}/down`. That is harmless: `uid`s
  are `[a-z][a-z0-9_]{0,31}` and `did`s are `HB-` + 6 uppercase hex, so no real client owns those topics, and
  the router ignores them (it acts only on `d/{did}` of claimed beacons and `u/{uid}` of known users).

### 5.2 Claim rule

Only the user the admin assigned to a `did` may claim it (`POST /api/v1/devices/claim`): anyone else gets
`403 not_assigned`, the same answer as for an unregistered `did`. A `did` claimed by another user needs an
admin release first (`409 claimed`). The `did` is public (it is the BLE name), so the admin's assignment is the
proof of ownership. Optional future work: a per-unit BLE claim code, readable from the beacon only inside its
pairing window and checked by the server, so a claim also proves physical access.

## 6. BLE GATT

| Item | Value |
|---|---|
| Service | `48420001-6265-6163-6F6E-763200000000` |
| RX (phone -> beacon) | `48420002-6265-6163-6F6E-763200000000`, write with response, encryption required |
| TX (beacon -> phone) | `48420003-6265-6163-6F6E-763200000000`, notify, encryption required |
| MTU | both sides request 247; any MTU >= 23 works |
| Security | LE Secure Connections, bonding, Just Works. New bonds only inside the 2 min pairing window (5 s button hold). An unencrypted connection is dropped after 5 s outside the window. Max 3 bonds, 1 connection. Public static address. |
| Advertising | ADV: flags + service UUID. Scan response: name = `did`. Fast 100 ms for 30 s after boot, button or disconnect; slow per config. |
| Connection parameters | phone defaults for 5 s, then the beacon requests `ble.conn_ms` interval, `ble.latency`, `ble.sup_ms` (re-request once if refused) |
| Versioning | `get info` returns `pv` (this document: 1) and `min_pv`; the app refuses to operate below `min_pv` |

### 6.1 Frames

Each RX write and TX notification carries one frame of at most `MTU - 3` bytes:

```
byte 0     bit 7 = MORE (another fragment of this message follows)
           bits 0-6 = message sequence number, mod 128
bytes 1..  the next bytes of the UTF-8 envelope (may split a multi-byte character)
```

No CRC: the encrypted link carries a MIC.

**Sending.** Each direction keeps its own counter; every fragment of a message carries the same sequence
number and the next message uses the next number (127 wraps to 0). The message (1..1024 bytes) is cut into
chunks of `MTU - 4` bytes; every frame but the last has MORE set. MTU < 23, an empty message, a message over
1024 bytes, or a sequence number outside 0..127 is a caller error.

**Receiving.** Time `now` is an injected millisecond counter; gaps are computed as `(now - last) mod 2^32`.
State is IDLE, COLLECT(seq) or SKIP(seq), plus `last` (time of the previous accepted frame) and the buffer.

1. A zero-length frame: result `empty`, state unchanged.
2. If not IDLE and the gap is over 2000 ms: a COLLECT drops its partial. A frame with the **same** seq is
   the tail of that message: state becomes SKIP(seq). Otherwise state becomes IDLE.
3. If not IDLE and seq differs: drop any partial, state becomes IDLE (the sender abandoned that message).
4. SKIP(seq): result `skip`; `last = now`; if MORE is clear, state becomes IDLE.
5. IDLE: start COLLECT(seq) with an empty buffer.
6. If buffer + payload would exceed 1024 bytes: drop the partial, result `oversize`; state becomes
   SKIP(seq) if MORE is set, else IDLE.
7. Append the payload, `last = now`. MORE set: result `partial`. MORE clear: result `complete` with the
   buffer as the message, state becomes IDLE.

A disconnect resets the receiver to IDLE. Sequence numbers do not dedupe: two complete messages with the
same seq are two messages (dedupe is by envelope `id`). The receiver keeps no timer; a stale partial is
dropped by the next frame or by reset.

## 7. Config schema v1

Signed by the server and published retained on `d/{did}/cfg` (as `cfg.b`). The phone gets the same signed
`cfg` envelope only in the API's claim and settings responses and writes it to the beacon over BLE as a plain
`cfg` message; it never travels inside a `relay`. Raw size <= 1024 B, and the `cfg` envelope around it must
also fit in 1024 B. Unknown keys at any level are ignored and dropped. Missing keys take the compiled defaults
(`vectors.json` `config.defaults`). Signature, `did` match and `n` monotonicity are checked by the `cfg`
handler, not by the parser.

WiFi networks are **not** part of the config: PSKs must not travel in anything the phone or the server sees.
They reach the beacon only over BLE (`wifi.*`). A `net.wifi` key is an unknown key and is dropped.

```json
{
  "cv": 1, "n": 12, "did": "HB-3FA2C1",
  "srv": {"hosts": ["203.0.113.10"], "mqtt": [8883], "api": 8443},
  "name": "Desk",
  "led": {"bright": 60},
  "sched": {"on": true, "tz": 330, "days": "1111100", "start": "09:00", "stop": "19:00"},
  "pwr": {"idle_nap_min": 90, "nap_wake_min": 15, "batt_low_mv": 3450, "batt_off_mv": 3300, "batt_cal_ppm": 0},
  "ble": {"adv_fast_ms": 100, "adv_fast_s": 30, "adv_slow_ms": 500, "conn_ms": 180, "latency": 0, "sup_ms": 6000},
  "net": {"use_wifi": true, "near_hold_s": 30, "away_grace_s": 60, "tele_min": 30},
  "fw": {"ver": "2.0.0", "path": "/fw/hb-2.0.0.bin", "sha256": "", "min_mv": 3700}
}
```

"int" means a JSON integer (booleans are not integers); string lengths are UTF-8 bytes.

| Key | Type and range | Default | Who edits |
|---|---|---|---|
| `cv` | int, must be 1 | required | server |
| `n` | int 1..2147483647 | required | server, bumped on every change |
| `did` | `HB-` + 6 uppercase hex | required | server |
| `srv.hosts` | list of 0..4 strings, 1..64 bytes each | `[]` (use the claim's `host`) | admin |
| `srv.mqtt` | list of 1..4 ints 1..65535 | `[8883]` | admin |
| `srv.api` | int 1..65535 | `8443` | admin |
| `name` | string 0..32 bytes | `""` | user, admin |
| `led.bright` | int 0..100 (percent) | `60` | user, admin |
| `led.presence` | bool: a faint pixel while another member's phone is near their beacon | `false` | user, admin |
| `mood.rgb` | `"#rrggbb"` (7 bytes, lowercase hex): the custom mood's colour | `"#ff4fa3"` | user, admin |
| `mood.pat` | int 0..1 (0 = 3 pulses, 1 = 3 slow breaths) | `0` | user, admin |
| `sched.on` | bool | `false` (always on) | user, admin |
| `sched.tz` | int -720..840 (minutes east of UTC); the zone the schedule runs in (`time.tz` is informational) | `0` | user, admin |
| `sched.days` | 7 chars of `0`/`1`, Monday first: the days a window **starts** on | `"1111100"` | user, admin |
| `sched.start`, `sched.stop` | `"HH:MM"`, 00:00..23:59; any pair is valid (section 7.2) | `"09:00"`, `"19:00"` | user, admin |
| `pwr.idle_nap_min` | int 0..1440 (0 = never nap) | `0` (NAP off) | user, admin |
| `pwr.nap_wake_min` | int 1..1440 | `15` | admin |
| `pwr.batt_low_mv`, `pwr.batt_off_mv` | int 3000..4200 each, and `batt_off_mv < batt_low_mv` (error reported on `pwr.batt_off_mv`) | `3450`, `3300` | admin |
| `pwr.batt_cal_ppm` | int -200000..200000 (divider correction) | `0` | admin |
| `ble.adv_fast_ms`, `ble.adv_slow_ms` | int 20..10240 | `100`, `500` | admin |
| `ble.adv_fast_s` | int 0..600 | `30` | admin |
| `ble.conn_ms` | int 8..4000 | `180` | admin |
| `ble.latency` | int 0..499 | `0` | admin |
| `ble.sup_ms` | int 100..32000, and `sup_ms > 2 * (1 + latency) * conn_ms` | `6000` | admin |
| `net.use_wifi` | bool | `true` | user, admin |
| `net.near_hold_s`, `net.away_grace_s` | int 0..3600 | `30`, `60` | admin |
| `net.tele_min` | int 1..1440 | `30` | admin |
| `fw.ver` | string 0..16 bytes (`""` = no target) | `""` | admin |
| `fw.path` | string 0..96 bytes | `""` | admin |
| `fw.sha256` | `""` or 64 lowercase hex | `""` | admin |
| `fw.min_mv` | int 3000..4200 | `3700` | admin |

A block (`srv`, `led`, ...) that is present but not an object is an error on the block name. Parse errors
name the first failing key, checked in table order: `size` (over 1024 B), `json` (not a JSON object), then
the dotted key path, e.g. `led.bright`.

### 7.1 Signature

`cfg.s` is **ECDSA P-256 (secp256r1) with SHA-256 over the raw config bytes**, i.e. exactly the bytes `cfg.b`
decodes to (the server signs the compact JSON it publishes; nobody re-serializes before verifying). The
signature is **DER-encoded** (`SEQUENCE {r INTEGER, s INTEGER}`, 70-72 bytes, not the raw 64-byte `r || s`),
then **base64url without padding**; `cfg.b` is base64url without padding as well. The verifying key is the
public half of the server's config key, pinned in the firmware. `vectors.json` `config.signature` holds a test
public key (SubjectPublicKeyInfo DER, hex) and signatures that must and must not verify.

### 7.2 Schedule

With `sched.on`, the beacon is in hours during `[start, stop)` local time in `sched.tz`, on the days `sched.days`
enables. A window belongs to the day it **starts** on: `stop <= start` runs past midnight into the next day
(22:00-06:00 on a Friday covers Saturday 05:59, whatever Saturday's bit says), and `start == stop` means 24 h
on from `start`. `sched.on` false, or an unknown clock (`ts` 0), counts as in hours. Out of hours the beacon
sleeps until the next window start (`vectors.json` `config.sched`: `in_hours`, and `next_start_s` = seconds to
the next start strictly after `epoch`, 0 when no day is enabled).

## 8. `vectors.json`

| Path | Each entry | A conforming implementation |
|---|---|---|
| `envelope.types` | known `t` values | has exactly this known set |
| `envelope.states`, `envelope.lwt_state` | the `state.st` values; `lost` (LWT only) | the beacon names its states exactly so; the router accepts exactly these |
| `envelope.encode[]` | `t`, `id`, `ts`, `body`, `json` | encodes the fields to exactly `json`; decodes `json` back to the fields |
| `envelope.size[]` | `t`, `id`, `ts`, `body`, `bytes`, `beacon_ok`, `ok` | encodes to `bytes` bytes when `ok`, refuses when not; a beacon encoder (896 B cap) accepts exactly when `beacon_ok` |
| `envelope.decode[]` | `json` and either `t`/`id`/`ts`/`body` or `err` | returns those fields, or fails with `err` |
| `envelope.unknown_t[]` | request `json`, `resp_id`, `resp_ts`, `resp_json` (null for a known `t`) | answers with exactly `resp_json`, or not at all |
| `envelope.did[]` | `mac` (12 hex), `did` | derives `did` |
| `frame.split[]` | `mtu`, `seq`, `msg`, `frames` (hex) | produces exactly `frames`; reassembling them yields `msg` |
| `frame.split_reject[]` | `mtu`, `seq`, `msg` | refuses to split |
| `frame.reassemble[]` | `steps[]`: `at` (ms), `frame` (hex), `out`, `msg` (when `complete`) | a fresh receiver fed the steps returns each `out` (`partial`, `complete`, `skip`, `oversize`, `empty`) |
| `config.parse[]` | `json` and either `cfg` (full result with defaults) or `err` | returns `cfg` or fails with `err` |
| `config.sched[]` | `sched` block, `epoch`, `in_hours`, `next_start_s` (absent when the schedule is off or the clock unknown) | agrees with section 7.2 |
| `config.signature` | `pub_spki_hex`; `verify[]`: `b`, `s`, `ok` | verifies `s` over the bytes of `b` exactly when `ok` |

Config parse vectors are checked by the server now and by the firmware's `hb_config` from M3; schedule vectors
by the firmware; signature vectors by the server and the app's JVM code now, by the firmware from M3.

## 9. Change log

Every entry keeps `v:1`: these are clarifications and additive rules, not wire breaks.

| Date | Change |
|---|---|
| 2026-11-23 | First version (M0). |
| 2026-11-24 | Round 2. `state.st` values are exact lowercase names (`envelope.states`); receivers drop others. Schedule windows: `stop <= start` runs past midnight and belongs to the start day, `start == stop` is 24 h; `sched.tz` governs, `time.tz` is informational (7.2, `config.sched`). `pwr.idle_nap_min` defaults to 0 (NAP off). The phone acks every relayed beacon uplink with `resp{re, ok:true}` once the broker PUBACKs the `relay`; that is the BLE ack for the 5 s resend rule. Beacon-originated messages capped at 896 B so a `relay` always fits (`envelope.size`). `link` to the router is exactly `{did, ble}`. Router forwards the beacon's `state` and `tele` to the owner's phone in a `relay`. `mood.from` is the sender's display name. A duplicate `show` is answered with `shown` again without replaying. Config signature format written down (7.1, `config.signature`). `net.wifi` removed from the config; signed `cfg` reaches the phone only through the API, never in a `relay`. Broker section: Mosquitto 1.6.9 instance with `password_file` + `pattern` ACL (5.1); claim rule (5.2). |
| 2026-11-26 | M4 bench. The beacon also sends `state` and `tele` whenever the phone says `link{up:true}` (a new app session on a kept connection gets the device status). `wifi.set` tries the network before storing it; its `resp` `err` is short text for a person (`wrong password`, `network not found`, `no answer`, `busy`, ...). Request field errors are `bad_<key>`; a bonded phone may `claim` again (it replaces the credentials); `wifi.use` answers `unsupported` (`net.use_wifi` stays in the signed config). |
| 2026-11-26 | Config defaults `ble.conn_ms` 500 -> 180 and `ble.latency` 3 -> 0 (`config.defaults`, parse vectors): a phone's heart reaches the beacon in about 0.2 s instead of up to 2 s. The beacon's RX characteristic also takes write-without-response; senders may write every frame but a message's last one without response. |
| 2026-11-26 | Review ideas, all additive (`v:1`): moods `custom` (`rgb`, `pat`) and `together`; shows `echo`, `celebrate`, synced `together` (`at`); new types `streak`, `presence`, `batt` (23 known); config `led.presence`, `mood.rgb`, `mood.pat`; router rules Echo and Together; the beacon's good-morning replay. |
