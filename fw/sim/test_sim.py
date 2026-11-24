"""hb_sim over TCP: boot, WiFi + MQTT up, a tap publishes the heart, a `show` is answered with `shown`, a signed
`cfg` is accepted once. Stdlib only; run by ctest: python test_sim.py <hb_sim> <protocol/vectors.json>."""
import json
import os
import socket
import subprocess
import sys
import tempfile

DID = "HB-3FA2C1"  # the did the config.signature vectors were signed for
UP, STATE, DOWN, CFG = (f"hb/v1/d/{DID}/{leaf}" for leaf in ("up", "state", "down", "cfg"))
EPOCH = 1795400000


class Sim:
    def __init__(self, exe, key):
        self.p = subprocess.Popen([exe, "--port", "0", "--did", DID, "--key", key], stdout=subprocess.PIPE, text=True)
        port = int(self.p.stdout.readline().split()[1])
        self.s = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.f = self.s.makefile("r", encoding="utf-8", newline="\n")

    def cmd(self, line):
        """Sends one command, returns its output lines (without the final ok); fails on err."""
        self.s.sendall((line + "\n").encode())
        lines = []
        while True:
            got = self.f.readline().rstrip("\n")
            assert got, f"hb_sim closed during {line!r}"
            if got == "ok":
                return lines
            assert not got.startswith("err"), f"{line!r} -> {got}"
            lines.append(got)

    def close(self):
        try:
            self.s.sendall(b"quit\n")
            self.p.wait(5)
        finally:
            self.s.close()
            if self.p.poll() is None:
                self.p.kill()


def pubs(lines, topic):
    """The JSON of every `pub <topic> <retain> <json>` line on topic, as (retain, raw json, parsed)."""
    out = []
    for ln in lines:
        if ln.startswith(f"pub {topic} "):
            _, _, retain, raw = ln.split(" ", 3)
            out.append((retain == "1", raw, json.loads(raw)))
    return out


def main(exe, vectors):
    sig = json.load(open(vectors, encoding="utf-8"))["config"]["signature"]
    good = next(v for v in sig["verify"] if v["name"] == "good")
    fd, key = tempfile.mkstemp(suffix=".der")
    os.write(fd, bytes.fromhex(sig["pub_spki_hex"]))
    os.close(fd)
    sim = Sim(exe, key)
    try:
        sim.s.sendall(b"run 10\n")  # nothing runs before the first boot
        assert sim.f.readline().startswith("err asleep")
        out = sim.cmd(f"boot 300 poweron claimed=1 wifi=1 cached=0 epoch={EPOCH}")
        assert "link scan" in out and "state offline" in out, out

        out = sim.cmd("wifi 1300 up -60")
        mq = [ln for ln in out if ln.startswith("link mqtt ")]
        assert len(mq) == 1, out
        _, _, client, user, will_topic, will = mq[0].split(" ", 5)
        assert (client, user, will_topic) == (f"d-{DID}", DID, STATE), mq
        assert json.loads(will)["st"] == "lost" and json.loads(will)["t"] == "state", will

        out = sim.cmd("mqtt 1500 up")
        assert f"sub {DOWN}" in out and f"sub {CFG}" in out, out
        (retain, _, st), = pubs(out, STATE)
        assert retain and st["t"] == "state" and st["st"] == "away" and st["path"] == "wifi", st
        (retain, _, tele), = pubs(out, UP)
        assert not retain and tele["t"] == "tele" and tele["rssi"] == -60 and tele["mv"] == 3900, tele

        # A tap: 100 ms press, then the 600 ms double-tap gap passes.
        out = sim.cmd("press 5000") + sim.cmd("release 5100") + sim.cmd("run 6000")
        (retain, raw, mood), = pubs(out, UP)
        assert not retain and mood["t"] == "mood" and mood["k"] == "heart", mood
        assert raw == json.dumps({"v": 1, "t": "mood", "id": mood["id"], "ts": mood["ts"], "k": "heart"},
                                 separators=(",", ":")), raw  # canonical key order, compact
        assert EPOCH <= mood["ts"] <= EPOCH + 10, mood
        assert any(ln.startswith("led ") and ln != "led " + "00" * 27 for ln in out), "the heart plays locally"
        sim.cmd(f"puback 6100 {mood['id']}")
        assert not [p for p in pubs(sim.cmd("run 30000"), UP) if p[2]["t"] == "mood"], "acked: never resent"

        show = json.dumps({"v": 1, "t": "show", "id": "00000000000000aa", "ts": 0, "k": "annoy"})
        out = sim.cmd(f"rx 31000 {DOWN} {show}")
        (_, _, shown), = pubs(out, UP)
        assert shown["t"] == "shown" and shown["id"] == "00000000000000aa", shown

        cfg = json.dumps({"v": 1, "t": "cfg", "id": "0123456789abcdef", "ts": 0, "n": 1, "b": good["b"],
                          "s": good["s"]})
        assert "cfg ok 1" in sim.cmd(f"rx 32000 {CFG} {cfg}")
        assert "cfg refused n" in sim.cmd(f"rx 33000 {CFG} {cfg}")  # the retained copy on the next connect

        sim.s.sendall(b"run 100\n")  # time never goes backwards
        assert sim.f.readline().startswith("err time went backwards")
    finally:
        sim.close()
        os.unlink(key)
    print("test_sim: OK")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
