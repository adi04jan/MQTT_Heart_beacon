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

- UTF-8, RFC 8259 JSON, at most **1024 bytes**.
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
| `mood` | beacon or app -> router; router -> phone | `k`: `heart` \| `sorry` \| `annoy`; `from` (router -> phone only) | tap = heart, double = sorry, triple = annoy. Its `id` is the event id. |
| `show` | router -> beacon | `k` | `id` = the originating `mood` id. Beacon dedupes on `id` (ring of 16 in RTC memory). |
| `shown` | beacon -> router | none | `id` = the `show` id. Delivery receipt. |
| `rcpt` | router -> sender's phone | `to` (did), `st`: `queued` \| `shown` \| `expired` | `id` = the event id. |
| `tele` | beacon -> router | `mv`, `pct`, `rssi`, `fw`, `hw`, `cfg`, `up_s`, `rst`, `heap` | every `tele_min`, and on each connect |
| `state` | beacon -> router (retained, LWT) | `st`: state name or `lost`; `path`: `wifi` \| `ble`; `wake` (epoch of next wake) | sent on every transition, before every deep sleep |
| `cfg` | router -> beacon (retained) | `n`, `b` (base64url config bytes, section 7), `s` (base64url ECDSA-P256 signature over `b`'s bytes) | rejected if signature bad, `did` mismatch, or `n` <= stored |
| `time` | router or phone -> beacon | `tz` (minutes east of UTC) | the envelope `ts` is the time |
| `sleep` | router -> beacon | `for_s` (0 = until schedule or button) | |
| `get` | either -> beacon | `what` | `get info` -> `resp` with `d`: `did`, `fw`, `hw`, `pv`, `min_pv`, `claimed` |
| `resp` | beacon -> requester | `re` (request id), `ok`, `err` (when not ok), `d` (data) | `err` values: `unknown_t`; later milestones add more |
| `claim` | phone -> beacon, BLE only | `host`, `mport`, `aport`, `user`, `pass` | accepted once; refused until factory reset |
| `wifi.scan`, `wifi.list`, `wifi.set`, `wifi.forget`, `wifi.use` | phone -> beacon, BLE only | `ssid`, `psk`, `on` | max 4 networks; `wifi.set` replies with join result and reason; PSKs never read back |
| `link` | phone -> beacon; phone -> router | `up` (phone has server link); `did`, `ble` (to router) | drives handover |
| `relay` | phone <-> router | `did`, `m` (inner envelope, a JSON object) | wrapper for anything carried over BLE |
| `reset` | phone -> beacon, BLE only | `what`: `wifi` \| `factory` | |

Those 20 values of `t` are the known set (`vectors.json` `envelope.types`).

## 4. Transport and handover

| Rule | Value |
|---|---|
| Uplink path | BLE if the link is encrypted and the last `link.up` is true; else WiFi. No ack in 5 s: resend the same `id` on the other path. |
| Enter NEAR | bonded link + `link up` held 30 s, then stop WiFi. |
| Leave NEAR | link lost: fast advertise 30 s, wait 60 s total, then start WiFi. A pending uplink skips the wait. |
| Phone upstream down 60 s | beacon starts WiFi while keeping BLE. |
| Downlink path (router) | most recently reported live path for that beacon. No `shown` in 10 s: try the other. |
| Dedupe | router: unique (`did`, `id`) per event. Beacon: `id` ring. |
| Ordering | none guaranteed, none needed. Effects are idempotent; the router orders by receive time. |
| Queue while asleep | router holds pending rows up to 24 h and flushes when `state` shows the beacon up. A burst arriving within 3 s plays each `k` once. |

## 5. MQTT

Mosquitto 2.x, MQTT 5, TLS on 8883 with the private CA. Keepalive 240 s for beacons, 120 s for phones.
Beacon client buffer 2048 B. QoS 1 everywhere.

| Topic | Publisher -> subscriber | Retain | Carries |
|---|---|---|---|
| `hb/v1/d/{did}/up` | beacon -> router | no | `mood`, `shown`, `tele`, `resp` |
| `hb/v1/d/{did}/state` | beacon -> router, LWT `st:lost` | yes | `state` |
| `hb/v1/d/{did}/down` | router -> beacon | no | `show`, `time`, `sleep`, `get` |
| `hb/v1/d/{did}/cfg` | router -> beacon | yes | `cfg` |
| `hb/v1/u/{uid}/up` | phone -> router | no | `mood`, `relay`, `link` |
| `hb/v1/u/{uid}/state` | phone -> router, LWT `on:false` | yes | `{"on":true}` (not an envelope) |
| `hb/v1/u/{uid}/down` | router -> phone | no | `mood`, `rcpt`, `relay`, device status |

| Client | Client id | Session | ACL (one dynsec role each, literal topics) |
|---|---|---|---|
| Beacon | `d-{did}` | clean start | publish `d/{did}/up`, `d/{did}/state`; subscribe `d/{did}/down`, `d/{did}/cfg` |
| Phone | `u-{uid}-{install}` | persistent, expiry 24 h | publish `u/{uid}/up`, `u/{uid}/state`; subscribe `u/{uid}/down` |
| Router | `router` | clean, 127.0.0.1:1883 only | `hb/v1/#` |

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

Signed by the server, published retained on `d/{did}/cfg` (as `cfg.b`), also handed to the phone for BLE
relay. Raw size <= 1024 B. Unknown keys at any level are ignored and dropped. Missing keys take the
compiled defaults (`vectors.json` `config.defaults`). Signature, `did` match and `n` monotonicity are
checked by the `cfg` handler, not by the parser.

```json
{
  "cv": 1, "n": 12, "did": "HB-3FA2C1",
  "srv": {"hosts": ["203.0.113.10"], "mqtt": [8883], "api": 8443},
  "name": "Desk",
  "led": {"bright": 60},
  "sched": {"on": true, "tz": 330, "days": "1111100", "start": "09:00", "stop": "19:00"},
  "pwr": {"idle_nap_min": 90, "nap_wake_min": 15, "batt_low_mv": 3450, "batt_off_mv": 3300, "batt_cal_ppm": 0},
  "ble": {"adv_fast_ms": 100, "adv_fast_s": 30, "adv_slow_ms": 500, "conn_ms": 500, "latency": 3, "sup_ms": 6000},
  "net": {"use_wifi": true, "near_hold_s": 30, "away_grace_s": 60, "tele_min": 30, "wifi": []},
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
| `sched.on` | bool | `false` (always on) | user, admin |
| `sched.tz` | int -720..840 (minutes east of UTC) | `0` | user, admin |
| `sched.days` | 7 chars of `0`/`1`, Monday first | `"1111100"` | user, admin |
| `sched.start`, `sched.stop` | `"HH:MM"`, 00:00..23:59 | `"09:00"`, `"19:00"` | user, admin |
| `pwr.idle_nap_min` | int 0..1440 (0 = never nap) | `90` | user, admin |
| `pwr.nap_wake_min` | int 1..1440 | `15` | admin |
| `pwr.batt_low_mv`, `pwr.batt_off_mv` | int 3000..4200 each, and `batt_off_mv < batt_low_mv` (error reported on `pwr.batt_off_mv`) | `3450`, `3300` | admin |
| `pwr.batt_cal_ppm` | int -200000..200000 (divider correction) | `0` | admin |
| `ble.adv_fast_ms`, `ble.adv_slow_ms` | int 20..10240 | `100`, `500` | admin |
| `ble.adv_fast_s` | int 0..600 | `30` | admin |
| `ble.conn_ms` | int 8..4000 | `500` | admin |
| `ble.latency` | int 0..499 | `3` | admin |
| `ble.sup_ms` | int 100..32000, and `sup_ms > 2 * (1 + latency) * conn_ms` | `6000` | admin |
| `net.use_wifi` | bool | `true` | user, admin |
| `net.near_hold_s`, `net.away_grace_s` | int 0..3600 | `30`, `60` | admin |
| `net.tele_min` | int 1..1440 | `30` | admin |
| `net.wifi` | list of 0..4 objects `{ssid: 1..32 bytes, psk: "" or 8..63 bytes, default ""}` | `[]` | admin (networks added in the app go over BLE and are never uploaded) |
| `fw.ver` | string 0..16 bytes (`""` = no target) | `""` | admin |
| `fw.path` | string 0..96 bytes | `""` | admin |
| `fw.sha256` | `""` or 64 lowercase hex | `""` | admin |
| `fw.min_mv` | int 3000..4200 | `3700` | admin |

A block (`srv`, `led`, ...) that is present but not an object is an error on the block name. Parse errors
name the first failing key, checked in table order: `size` (over 1024 B), `json` (not a JSON object), then
the dotted key path, e.g. `led.bright`. Any error in a list item of `net.wifi` is reported as `net.wifi`.

## 8. `vectors.json`

| Path | Each entry | A conforming implementation |
|---|---|---|
| `envelope.types` | known `t` values | has exactly this known set |
| `envelope.encode[]` | `t`, `id`, `ts`, `body`, `json` | encodes the fields to exactly `json`; decodes `json` back to the fields |
| `envelope.decode[]` | `json` and either `t`/`id`/`ts`/`body` or `err` | returns those fields, or fails with `err` |
| `envelope.unknown_t[]` | request `json`, `resp_id`, `resp_ts`, `resp_json` (null for a known `t`) | answers with exactly `resp_json`, or not at all |
| `envelope.did[]` | `mac` (12 hex), `did` | derives `did` |
| `frame.split[]` | `mtu`, `seq`, `msg`, `frames` (hex) | produces exactly `frames`; reassembling them yields `msg` |
| `frame.split_reject[]` | `mtu`, `seq`, `msg` | refuses to split |
| `frame.reassemble[]` | `steps[]`: `at` (ms), `frame` (hex), `out`, `msg` (when `complete`) | a fresh receiver fed the steps returns each `out` (`partial`, `complete`, `skip`, `oversize`, `empty`) |
| `config.parse[]` | `json` and either `cfg` (full result with defaults) or `err` | returns `cfg` or fails with `err` |

Config vectors are checked by the server now and by the firmware's `hb_config` from M3.
