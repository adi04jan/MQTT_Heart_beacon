"""hb_sim <-> an MQTT broker: plays the beacon's radios so a simulated beacon talks to a real router.

    hb_sim --port 7000 --did HB-000001 &
    HB_MQTT_PASS=... python fw/sim/bridge.py --sim 127.0.0.1:7000 --broker 203.0.113.10:8883 --ca ca.crt

WiFi always joins; MQTT is a paho-mqtt (>= 2.0) MQTT 5 session with the client id, login and LWT the sim names,
clean start, keepalive 240 s, QoS 1. Every PUBACK of a mood goes back to the sim as `puback`. The clock is
real time in ms. Type tap, tap2, tap3, hold or quit on stdin. fw/sim/README.md has the line protocol.
"""
import argparse
import json
import os
import queue
import socket
import sys
import threading
import time

import paho.mqtt.client as mqtt


class Bridge:
    def __init__(self, args):
        host, port = args.sim.rsplit(":", 1)
        self.sock = socket.create_connection((host, int(port)))
        self.f = self.sock.makefile("r", encoding="utf-8", newline="\n")
        self.broker = args.broker.rsplit(":", 1)
        self.ca = args.ca
        self.t0 = time.monotonic()
        self.q = queue.Queue()  # commands from paho's thread and stdin; "{t}" becomes the clock when sent
        self.cli = self.last = None
        self.moods = {}  # paho mid -> envelope id of a mood publish
        self.asleep, self.wake_at = False, None

    def now(self):
        return int((time.monotonic() - self.t0) * 1000) + 300

    def send(self, line):
        self.sock.sendall((line.replace("{t}", str(self.now())) + "\n").encode())
        out = []
        while True:
            got = self.f.readline().rstrip("\n")
            if not got:
                raise SystemExit("hb_sim closed the connection")
            if got == "ok":
                break
            if got.startswith("err"):
                print("sim", got, file=sys.stderr)
                break
            out.append(got)
        for got in out:
            self.act(got)

    def act(self, line):
        verb, _, rest = line.partition(" ")
        if verb == "link":
            what, _, args = rest.partition(" ")
            if what in ("fast", "scan"):
                self.q.put("wifi {t} up -55")
            elif what == "mqtt":
                self.connect(*args.split(" ", 3))
            elif what == "down":
                self.disconnect()
        elif verb == "sub" and self.cli:
            self.cli.subscribe(rest, qos=1)
        elif verb == "pub" and self.cli:
            topic, retain, payload = rest.split(" ", 2)
            info = self.last = self.cli.publish(topic, payload, qos=1, retain=retain == "1")
            if json.loads(payload).get("t") == "mood":
                self.moods[info.mid] = json.loads(payload)["id"]
            print("->", topic, payload)
        elif verb == "sleep":
            if self.cli and self.last:
                self.last.wait_for_publish(2)  # the retained `state` first, bounded like the firmware's flush
            self.disconnect()  # then a clean DISCONNECT, so no LWT
            self.asleep = True
            self.wake_at = time.monotonic() + int(rest) if int(rest) else None
            print("sim sleep", rest, "s (tap wakes it)")
        elif verb in ("state", "cfg", "ble"):
            print("sim", line)

    def connect(self, client_id, user, will_topic, will):
        c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id, protocol=mqtt.MQTTv5)
        c.username_pw_set(user, os.environ.get("HB_MQTT_PASS") or None)
        c.will_set(will_topic, will, qos=1, retain=True)
        if self.ca:
            c.tls_set(ca_certs=self.ca)
        c.on_connect = lambda cl, u, fl, rc, pr: self.q.put("mqtt {t} " + ("down" if rc.is_failure else "up"))
        c.on_disconnect = lambda cl, u, fl, rc, pr: self.q.put("mqtt {t} down")
        c.on_message = lambda cl, u, m: self.q.put(f"rx {{t}} {m.topic} {m.payload.decode()}")
        c.on_publish = lambda cl, u, mid, rc, pr: mid in self.moods and self.q.put(f"puback {{t}} {self.moods.pop(mid)}")
        c.connect_async(self.broker[0], int(self.broker[1]), keepalive=240, clean_start=True)
        c.loop_start()
        self.cli = c

    def disconnect(self):
        if self.cli:
            self.cli.disconnect()
            self.cli.loop_stop()
            self.cli = None

    def stdin(self):
        taps = {"tap": 1, "tap2": 2, "tap3": 3}
        for line in sys.stdin:
            w = line.strip()
            if w in taps or w == "hold":
                for _ in range(taps.get(w, 1)):
                    self.q.put("press {t}")
                    time.sleep(1.0 if w == "hold" else 0.1)
                    self.q.put("release {t}")
                    time.sleep(0.15)
            elif w == "quit":
                self.q.put("quit")
                return

    def run(self):
        threading.Thread(target=self.stdin, daemon=True).start()
        self.send("boot {t} poweron claimed=1 wifi=1 cached=1")
        while True:
            try:
                line = self.q.get(timeout=0.05)
            except queue.Empty:
                line = None
            if line == "quit":
                break
            if self.asleep:
                if line and line.startswith("press"):  # the button wakes it
                    self.asleep = False
                    self.send("boot {t} button pressed=1 cached=1")
                elif self.wake_at and time.monotonic() >= self.wake_at:
                    self.asleep = False
                    self.send("boot {t} timer cached=1")
                continue
            self.send(line or "run {t}")
        self.disconnect()
        self.sock.close()


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sim", default="127.0.0.1:7000", help="hb_sim host:port")
    ap.add_argument("--broker", required=True, help="MQTT broker host:port")
    ap.add_argument("--ca", help="CA certificate (PEM) for TLS; without it the connection is plain")
    Bridge(ap.parse_args()).run()
