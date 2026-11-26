# hb_sim

The beacon's `hb_core` + `hb_wire` (the same library target the host tests link, called the way
`fw/main/main.c` calls it) behind a line-based TCP socket. The client plays the radios, the button and the
clock; the sim answers with what the firmware would do. No IDF, no hardware.

Build and test (part of the host suite and `./test.sh`):

```bash
cmake -S fw/test/host -B fw/test/host/build -G Ninja && cmake --build fw/test/host/build
(cd fw/test/host/build && ctest --output-on-failure)        # test_sim drives a heart through the socket
fw/test/host/build/sim/hb_sim --port 7000 --did HB-000001  # prints "listening 7000 HB-000001"
```

Options: `--port` (0 = any free port, printed), `--did` (default `HB-000001`), `--key` (config signing key,
SPKI PEM or DER; default `fw/test/certs/cfg-sign.pub`, the test key; the firmware pins the deployed one). It listens on
127.0.0.1 only, serves one client at a time, and every connection starts a fresh beacon (asleep until `boot`).

## Line protocol

One command per line. Every command but `boot` first runs the beacon up to `<ms>` exactly as the firmware's
main loop does (step at each `next`, button level held, battery read answered with the last `mv`), then
applies its event. `<ms>` is the injected clock in milliseconds and never goes backwards. The reply is any
number of action lines, then `next <ms>` (step again within this long) and `ok`, or a single `err <why>`.

| Command | Event |
|---|---|
| `boot <ms> poweron\|button\|timer [claimed=1] [mv=3900] [epoch=0] [wifi=1] [cached=0] [pressed=0] [seed=]` | power-on or wake; `wifi`: a network and server are provisioned; `cached`: an AP's BSSID + channel is cached; `seed`: message ids (default: from the wall clock, so runs never repeat ids) |
| `run <ms>` | nothing: just time |
| `press <ms>` / `release <ms>` | button level |
| `batt <ms> <mv>` | battery reading (also used for later reads) |
| `wifi <ms> up [rssi]` / `wifi <ms> down` | got an IP / join failed or association lost |
| `mqtt <ms> up` / `mqtt <ms> down` | CONNACK / session failed or lost |
| `puback <ms> <id>` | the PUBACK of the mood publish with this envelope id |
| `rx <ms> <topic> <json>` | a message the broker delivered (rest of the line is the payload) |
| `quit` | exit |

| Action line | Meaning |
|---|---|
| `led <54 hex>` | new frame, 9 pixels r g b |
| `link fast` / `link scan` | WiFi on: join the cached AP / scan and join; answer with `wifi ... up` or `down` |
| `link mqtt <client-id> <user> <will-topic> <will-json>` | start MQTT 5, clean start, keepalive 240 s, LWT retained QoS 1; answer with `mqtt ... up` or `down` |
| `link down` | stop MQTT and WiFi |
| `sub <topic>` | subscribe, QoS 1 (printed on `mqtt up`) |
| `pub <topic> <retain 0\|1> <json>` | publish, QoS 1; for a `mood`, report its PUBACK with `puback` |
| `ble <json>` | an uplink the core put on BLE (no BLE before M4: not sent, the firmware drops it too) |
| `cfg ok <n>` / `cfg refused <why>` | a `cfg` was accepted (kept for the next `boot`, as NVS) / refused (`sig`, `did`, `n`, ...) |
| `state <name>` | the power state changed (or the link just came up) |
| `sleep <s>` | deep sleep, timer `s` (0 = button only); nothing runs until the next `boot` |

Example (`>` is the client):

```
> boot 300 poweron epoch=1795400000
link scan
state offline
> wifi 1300 up -60
link mqtt d-HB-000001 HB-000001 hb/v1/d/HB-000001/state {"v":1,"t":"state","id":"...","ts":1795400001,"st":"lost"}
> mqtt 1500 up
sub hb/v1/d/HB-000001/down
sub hb/v1/d/HB-000001/cfg
pub hb/v1/d/HB-000001/state 1 {"v":1,"t":"state","id":"...","ts":1795400001,"st":"away","path":"wifi"}
pub hb/v1/d/HB-000001/up 0 {"v":1,"t":"tele",...}
state away
> press 5000
> release 5100
> run 6000
pub hb/v1/d/HB-000001/up 0 {"v":1,"t":"mood","id":"657dd9308bc6740c","ts":1795400005,"k":"heart"}
```

## Bridge to a broker

`bridge.py` (Python, `pip install paho-mqtt`) connects a running `hb_sim` to a real broker: WiFi always joins,
MQTT is a paho session with the client id, login and LWT the sim names, PUBACKs go back as `puback`, the
clock is real time, and stdin takes `tap`, `tap2`, `tap3`, `hold`, `quit`. The beacon password comes from
`HB_MQTT_PASS` (never the command line).

```bash
fw/test/host/build/sim/hb_sim --port 7000 --did HB-000001 &
HB_MQTT_PASS=... python fw/sim/bridge.py --sim 127.0.0.1:7000 --broker <server-ip>:8883 --ca ca.crt
python fw/sim/bridge.py --sim 127.0.0.1:7000 --broker 127.0.0.1:1883   # a local test broker, plain
```

`fw/test/certs/` holds **test-only** files: a throwaway CA from `deploy/mkca.sh 127.0.0.1` (private key not
kept) and the `protocol/vectors.json` config test key. The firmware pins the deployed `ca.crt` and
`cfg-sign.pub` from `fw/main/certs/`.
