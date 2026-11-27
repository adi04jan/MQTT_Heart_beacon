"""Heart Beacon carrier rev C3: one single-sided board, 48 x 37 mm, sized to hide behind the 45 x 35 mm cell.

Rev B's circuit with a new GPIO map (PINMAP), narrow oblong socket pads so one track passes between two pins, and
0 ohm 1210 jumpers where two nets still have to cross. One copper layer (EAGLE Top, the milled side), no vias.
Through-hole parts sit on the other side and are soldered on the copper side. GND is routed and then poured.

  python revc.py place    placement preview only (out/revC3_place.png)
  python revc.py          place, autoroute, write ESP32_heart_revC3.sch / _build.scr / _rules.dru, KiCad DRC
  python revc.py --reuse  skip the autorouter, reuse out/kicad/tracks.json

The maker's EAGLE builds the board itself from the schematic and the script, so the two stay consistent.
Reads the rev B files as the source of parts and nets; writes only ESP32_heart_revC3* next to them.
Importing this module is cheap (cage/heartcage.py reads OUTLINE and PLACE from it).
"""
import copy, json, math, shutil, subprocess, sys
import xml.etree.ElementTree as ET
from pathlib import Path
from shapely.geometry import Polygon, LineString, Point, box as sbox
from shapely import affinity
from shapely.ops import unary_union

HERE = Path(__file__).parent
OUT = HERE / "out"
EAGLE = Path.home() / "OneDrive/Documents/EAGLE/projects/ES32_heart"
SRC_SCH, SRC_BRD = EAGLE / "ESP32_heart_revB.sch", EAGLE / "ESP32_heart_revB.brd"
NAME = "ESP32_heart_revC3"
DST_SCH, DST_SCR, DST_DRU = EAGLE / f"{NAME}.sch", EAGLE / f"{NAME}_build.scr", EAGLE / f"{NAME}_rules.dru"
JAVA = Path("C:/Program Files/Eclipse Adoptium/jdk-21.0.13.9-hotspot/bin/java.exe")
FREEROUTING = Path.home() / "AppData/Local/Temp/hb-hw/tools/freerouting-2.1.0.jar"
FR_SETTINGS = Path.home() / "AppData/Local/Temp/freerouting/freerouting.json"
KICAD = Path("C:/Program Files/KiCad/9.0/bin")
HDR = '<?xml version="1.0" encoding="utf-8"?>\n<!DOCTYPE eagle SYSTEM "eagle.dtd">\n'

# ---------------------------------------------------------------- board definition (mm, copper side view, y up)
BW, BH = 48.0, 37.0
AX = 24.0                                  # the module's USB-C and S1 sit on this line (the heart's axis)
CH = dict(bl=3.0, br=3.0, tl=2.0, tr=2.0)  # corner chamfers
OUTLINE = [(CH["bl"], 0), (BW - CH["br"], 0), (BW, CH["br"]), (BW, BH - CH["tr"]), (BW - CH["tr"], BH),
           (CH["tl"], BH), (0, BH - CH["tl"]), (0, CH["bl"])]
TABS = [(12.0, 0.0), (36.0, 0.0), (10.0, BH), (38.0, BH)]      # 1.9 mm gaps in the layer-46 cut = holding tabs
TRACK, CLEAR, EDGE = 0.4, 0.35, 1.0        # the maker's rules: 0.4 mm lines, 0.35 mm clearance everywhere
POWER, POWER_WIDE = 0.55, 0.8              # power nets are routed at 0.55 and widened to 0.8 wherever 0.35 still holds
POWER_NETS = {"BATT+", "USB_IN", "USB_OUT", "N$15", "N$18", "N$6"}   # cell, USB, VSYS, LED+, and the path between them
JUMPER_PKG = dict(name="R1210J", pad=(1.0, 2.6), pitch=3.0)   # 1210 on narrow pads: 2.0 mm between them = two tracks
LANDING = ("USB_OUT", 41.6, 19.65)          # 2 mm pad for the module's VSYS (PD1) wire, next to that corner as on rev B
LANDING_D = 2.0

# GPIO map: net -> module pad. The firmware's pin map is per-board Kconfig, so signals sit where they route best.
PINMAP = {"A1": "0", "IMU_INT": "1", "SCL": "2", "SDA": "8", "VBAT_SENSE": "4", "USB_CHARGE": "3",
          "LED_EN": "20", "LED_CTRL": "21"}
FIRST = ("N$18", "USB_IN")                              # nets the completion router takes before the rest
LAST = ("BATT+", "GND")                    # GND is a pour: it takes what is left
PRELINKS = [("GND", ("S3", "MT1"), ("S3", "MT2")), ("GND", ("C18", "2"), ("C17", "2")), ("BATT+", ("S3", "3"), ("C6", "1")), ("BATT+", ("R21", "2"), ("R6", "2")), ("BATT+", ("R6", "2"), ("Q6", "2")),
            ("LED_DIN", ("R9", "2"), ("SV1", "2")), ("N$15", ("Q6", "3"), ("SV1", "1"))]
RENAME = {"7": "LED_EN"}                   # rev B called the LED enable net "7" after its pin
# Socket and header pads: oblong, 1.3 mm along the row, 2.2 mm across, 0.8 mm drill: one 0.4 mm track fits between pins
NARROW = {"MODULE_ESP32-C3_SUPERMINI_TH": "R0", "MA06-1": "R90", "MA03-1": "R90", "1X02": "R90"}
NPAD, NDRILL, ELONG = 1.3, 0.8, 2.2 / 1.3

U1Y = BH - 12.7                            # USB-C flush with the top edge, as on rev B
IMU_T = BH - 5.34                          # y of the LIS3DH's pin 1 (GND): level with the module's G pin
PLACE = {   # name: (x, y, rot)   M = body on the Bottom (through-hole, soldered on the copper side)
    "U1": (AX, U1Y, "MR0"),
    "S1": (AX, 7.2, "R180"),               # on the back like the rest; unmirrored so pin 1 (button) is top left
    "S3": (44.19, 28.0, "MR90"),           # slider overhangs the right edge by 2.2 mm
    "SV3": (13.0, IMU_T - 6.35, "MR90"),   # LIS3DH J1 next to the module: GND, 3V3, SDA, SCL from the top
    "SV5": (2.84, IMU_T - 7.62, "MR270"),  # LIS3DH J2: pin 1 (INT1) lowest
    "SV1": (6.0, 3.2, "MR0"),              # LED: 1 LED+ (right), 2 DIN, 3 GND
    "BATT": (43.0, 3.4, "MR0"),
    "JP1": (40.0, 17.8, "MR180"),
    # power path, top right next to the slide switch
    "D2": (37.0, 28.6, "R90"), "Q2": (40.9, 28.0, "R270"), "C18": (39.3, 23.9, "R0"), "R5": (39.3, 21.3, "R180"),
    # charger, bottom right
    "U3": (40.0, 12.0, "R90"), "C17": (36.8, 15.2, "R0"), "C6": (44.6, 13.6, "R270"),
    "R3": (36.8, 10.7, "R0"), "R4": (36.8, 8.0, "R0"), "R2": (36.8, 5.8, "R180"), "LED1": (41.6, 5.8, "R0"),
    # battery divider under the module, USB sense divider between the LIS3DH pin rows
    "R22": (20.8, 25.6, "R180"), "C2": (20.8, 22.0, "R0"), "R21": (20.5, 12.2, "R0"),
    "R1": (8.0, 28.2, "R180"), "R14": (8.0, 25.3, "R0"), "C32": (8.1, 22.6, "R180"),
    # LED rail between S1's pin rows; only LED+ and DIN go on to the header
    "R8": (24.3, 16.4, "R0"), "Q5": (24.3, 13.4, "R180"), "R7": (24.3, 9.2, "R270"), "Q6": (25.3, 5.0, "R270"),
    "R6": (21.6, 6.5, "R180"), "R9": (27.6, 12.6, "R270"), "C7": (6.0, 6.4, "R180"),
}

# ---------------------------------------------------------------- EAGLE helpers
def el(tag, text=None, **kw):
    e = ET.Element(tag)
    for k, v in kw.items(): e.set(k.rstrip("_"), v if isinstance(v, str) else ("%.4f" % v).rstrip("0").rstrip("."))
    e.text = text; e.tail = "\n"
    return e

def save(root, path):
    assert path.name.startswith("ESP32_heart_revC") or OUT in path.parents, path     # never the maker's files
    path.write_text(HDR + ET.tostring(root, encoding="unicode") + "\n", encoding="utf-8", newline="\n")

def tf(x, y, rot):
    r = rot or "R0"; mir = "M" in r; ang = float(r.lstrip("MSR") or 0)
    if mir: x = -x
    a = math.radians(ang)
    return x * math.cos(a) - y * math.sin(a), x * math.sin(a) + y * math.cos(a), mir, ang

def tht_dia(pad):                                   # restring 0.25 mm, as set in the .dru and in eagle2kicad.py
    return max(float(pad.get("drill")) + 0.5, float(pad.get("diameter") or 0))

def narrow_pads(libraries):
    """Rewrite the socket / header pads in a file's libraries (board or schematic)."""
    n = 0
    for lib in libraries:
        for pk in lib.iter("package"):
            if pk.get("name") in NARROW:
                for pad in pk.findall("pad"):
                    pad.set("drill", "%g" % NDRILL); pad.set("diameter", "%g" % NPAD); pad.set("shape", "long")
                    pad.set("rot", NARROW[pk.get("name")]); n += 1
    return n

def jumper_package():
    w, h = JUMPER_PKG["pad"]; p = JUMPER_PKG["pitch"] / 2
    pk = el("package", name=JUMPER_PKG["name"]); pk.text = "\n"
    pk.append(el("description", "1210 0 ohm jumper on narrow pads: 2.0 mm between the pads for two 0.4 mm tracks"))
    pk.append(el("smd", name="1", x=-p, y=0.0, dx=w, dy=h, layer="1"))
    pk.append(el("smd", name="2", x=p, y=0.0, dx=w, dy=h, layer="1"))
    for x1, y1, x2, y2 in ((-1.6, 1.25, 1.6, 1.25), (-1.6, -1.25, 1.6, -1.25)):
        pk.append(el("wire", x1=x1, y1=y1, x2=x2, y2=y2, width=0.127, layer="51"))
    pk.append(el("text", ">NAME", x=-1.6, y=1.6, size=1.0, layer="25"))
    return pk

def placed_board(jumpers=(), inset=0.0, landing_via=True):
    """rev B board with every element moved, old copper removed, new outline, and the jumpers added.
    jumpers: [dict(name, x, y, rot, net1, net2, moved=[(element, pad)])]. Returns (root, pads)."""
    root = ET.parse(SRC_BRD).getroot()
    board = root.find("drawing/board")
    ada = [l for l in root.iter("library") if l.get("name") == "adafruit" and not l.get("urn")][0]
    ada.find("packages").append(jumper_package())
    narrow_pads(root.iter("library"))
    pk = {(l.get("name"), l.get("urn") or None, p.get("name")): p for l in root.iter("library") for p in l.iter("package")}
    def package(e):
        k = (e.get("library"), e.get("library_urn") or None, e.get("package"))
        return pk[k] if k in pk else [v for (ln, lu, pn), v in pk.items() if ln == e.get("library") and pn == e.get("package")][0]
    elements, signals = board.find("elements"), board.find("signals")
    E = {e.get("name"): e for e in elements}
    assert set(E) == set(PLACE), set(E) ^ set(PLACE)
    place = dict(PLACE)
    for sg in signals:
        if sg.get("name") in RENAME: sg.set("name", RENAME[sg.get("name")])
        for c in list(sg):
            if c.tag in ("wire", "via", "polygon"): sg.remove(c)
    S = {sg.get("name"): sg for sg in signals}
    for net, pad in PINMAP.items():                               # the new GPIO map
        old = [c for c in S[net].findall("contactref") if c.get("element") == "U1"]
        assert len(old) == 1, net
        S[net].remove(old[0])
    for net, pad in PINMAP.items():
        for sg in signals:
            for c in sg.findall("contactref"):
                if c.get("element") == "U1" and c.get("pad") == pad: sg.remove(c)
        S[net].insert(0, el("contactref", element="U1", pad=pad))
    for j in jumpers:
        e = el("element", name=j["name"], library="adafruit", package=JUMPER_PKG["name"], value="0", x=j["x"], y=j["y"], rot=j["rot"])
        elements.append(e); E[j["name"]] = e; place[j["name"]] = (j["x"], j["y"], j["rot"])
        new = el("signal", name=j["net2"]); new.text = "\n"
        signals.append(new); S[j["net2"]] = new
    for j in jumpers:                                             # after all new signals exist
        S[j["net1"]].insert(0, el("contactref", element=j["name"], pad="1"))
        S[j["net2"]].insert(0, el("contactref", element=j["name"], pad="2"))
        for ref, pad in j["moved"]:
            c = [c for s in signals for c in s.findall("contactref") if c.get("element") == ref and c.get("pad") == pad]
            assert len(c) == 1, (ref, pad)
            [s for s in signals if c[0] in list(s)][0].remove(c[0])
            S[j["net2"]].append(c[0])
    padnet = {(c.get("element"), c.get("pad")): s.get("name") for s in signals for c in s.findall("contactref")}
    pads = []
    for name, (x, y, rot) in place.items():
        e = E[name]
        e.set("x", "%g" % x); e.set("y", "%g" % y); e.set("rot", rot)
        e.attrib.pop("smashed", None)
        for a in e.findall("attribute"):
            if a.get("name") in ("NAME", "VALUE"): e.remove(a)
            else:
                for k in ("x", "y", "rot"): a.attrib.pop(k, None)
        for c in package(e):
            if c.tag not in ("pad", "smd"): continue
            px, py, mir, ang = tf(float(c.get("x")), float(c.get("y")), rot)
            a = (ang + float((c.get("rot") or "R0").lstrip("MSR") or 0)) % 180
            if c.tag == "smd":
                assert not mir, name
                w, h = float(c.get("dx")), float(c.get("dy"))
                shape = sbox(-w / 2, -h / 2, w / 2, h / 2)
            else:
                d = tht_dia(c)
                e2 = d * (ELONG - 1) / 2
                shape = LineString([(-e2, 0), (e2, 0)]).buffer(d / 2) if c.get("shape") == "long" else Point(0, 0).buffer(d / 2)
            shape = affinity.translate(affinity.rotate(shape, a, origin=(0, 0)), x + px, y + py)
            pads.append(dict(ref=name, pad=c.get("name"), net=padnet.get((name, c.get("name"))), xy=(x + px, y + py),
                             shape=shape, tht=c.tag == "pad"))
    plain = board.find("plain")
    for c in list(plain): plain.remove(c)
    # inset > 0 only for the autorouter, which knows one clearance: it gets a smaller board instead of an edge rule
    edge = OUTLINE if not inset else list(Polygon(OUTLINE).buffer(-inset, join_style="mitre").exterior.coords)[:-1]
    for a, b in zip(edge, edge[1:] + edge[:1]):
        plain.append(el("wire", x1=a[0], y1=a[1], x2=b[0], y2=b[1], width=0.0, layer="20"))
    for (x1, y1), (x2, y2) in zip(OUTLINE, OUTLINE[1:] + OUTLINE[:1]):
        cuts = [0.0, 1.0]                                              # layer 46: the cut, with gaps for the tabs
        L = math.hypot(x2 - x1, y2 - y1)
        for tx, ty in TABS:
            t = ((tx - x1) * (x2 - x1) + (ty - y1) * (y2 - y1)) / L / L
            if 0 < t < 1 and abs((tx - x1) * (y2 - y1) - (ty - y1) * (x2 - x1)) < 1e-6:
                cuts += [t - 0.95 / L, t + 0.95 / L]
        cuts.sort()
        for a, b in zip(cuts[::2], cuts[1::2]):
            plain.append(el("wire", x1=x1 + a * (x2 - x1), y1=y1 + a * (y2 - y1), x2=x1 + b * (x2 - x1), y2=y1 + b * (y2 - y1),
                            width=1.2, layer="46"))
    # LIS3DH outline on the bottom silk (22.86 x 12.7, pin 1 marked)
    x0, x1_, y0, y1_ = PLACE["SV5"][0] - 1.27, PLACE["SV3"][0] + 1.27, IMU_T - 19.05, IMU_T + 3.81
    for a, b in (((x0, y0), (x1_, y0)), ((x1_, y0), (x1_, y1_)), ((x1_, y1_), (x0, y1_)), ((x0, y1_), (x0, y0))):
        plain.append(el("wire", x1=a[0], y1=a[1], x2=b[0], y2=b[1], width=0.1524, layer="22"))
    plain.append(el("circle", x=PLACE["SV3"][0] - 2.4, y=IMU_T, radius=0.254, width=0.508, layer="22"))
    # the VSYS wire landing: a drilled 2 mm pad for one wire, kept as a via element exactly as rev B has it
    if landing_via:
        S[LANDING[0]].append(el("via", x=LANDING[1], y=LANDING[2], extent="1-16", drill=1.0, diameter=LANDING_D))
    pads.append(dict(ref="LAND", pad="1", net=LANDING[0], xy=LANDING[1:], shape=Point(*LANDING[1:]).buffer(LANDING_D / 2), tht=True))
    return root, pads

def checks(pads):
    over = [(a["ref"], a["pad"], b["ref"], b["pad"], round(a["shape"].distance(b["shape"]), 3)) for i, a in enumerate(pads)
            for b in pads[i + 1:] if a["net"] != b["net"] and a["shape"].distance(b["shape"]) < CLEAR - 1e-6]
    assert not over, f"pads closer than {CLEAR}: {over}"
    board = Polygon(OUTLINE)
    close = [(p["ref"], p["pad"]) for p in pads if p["shape"].distance(board.exterior) < EDGE or not board.contains(p["shape"])]
    assert not close, f"pads within {EDGE} of the edge: {close}"
    P = {(p["ref"], p["pad"]): p["xy"] for p in pads}        # the LIS3DH now plugs in from the back: mirrored pattern
    a, b, c = P[("SV3", "1")], P[("SV3", "6")], P[("SV5", "1")]
    cross = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    assert abs(cross + 12.7 * 10.16) < 1e-6 and abs(math.dist(a, c) - math.hypot(10.16, 10.16)) < 1e-6, "LIS3DH pin pattern"

# ---------------------------------------------------------------- jumpers: split each net where it hops
def split_nets(pads, tracks, hops):
    """hops: router jumpers [dict(net, x1, y1, x2, y2)]. Returns (jumper parts, track list with final net names)."""
    w, h = JUMPER_PKG["pad"]
    jumpers, k = [], 0
    track_net = {id(t): t["net"] for t in tracks}
    for net in sorted({j["net"] for j in hops}):
        items = [("pad", p, p["shape"]) for p in pads if p["net"] == net]
        items += [("trk", t, LineString([(t["x1"], t["y1"]), (t["x2"], t["y2"])]).buffer(t["w"] / 2)) for t in tracks if t["net"] == net]
        ends = []
        for j in [j for j in hops if j["net"] == net]:
            horiz = abs(j["y1"] - j["y2"]) < 1e-9
            a, b = sorted([(j["x1"], j["y1"]), (j["x2"], j["y2"])])
            pw, ph = (w, h) if horiz else (h, w)
            for q in (a, b):
                items.append(("jp", (len(ends) // 2, len(ends) % 2), sbox(q[0] - pw / 2, q[1] - ph / 2, q[0] + pw / 2, q[1] + ph / 2)))
                ends.append((a, b, horiz))
        par = list(range(len(items)))
        def find(i):
            while par[i] != i: par[i] = par[par[i]]; i = par[i]
            return i
        for i in range(len(items)):
            for q in range(i + 1, len(items)):
                if items[i][2].distance(items[q][2]) < 1e-6: par[find(i)] = find(q)
        comp = [find(i) for i in range(len(items))]
        jp = {it[1]: comp[i] for i, it in enumerate(items) if it[0] == "jp"}
        # The router hops from the main side (x1, y1) to the far side (x2, y2): the far piece gets the new name.
        # Everything else keeps the net's name (for GND that includes pads joined only by the pour).
        name = {}
        for q, j in enumerate([j for j in hops if j["net"] == net]):
            a, b, horiz = ends[2 * q]
            flip = (j["x1"], j["y1"]) != a                            # pad 1 is the lower-left end; is that the main side?
            near, far = (jp[(q, 1)], jp[(q, 0)]) if flip else (jp[(q, 0)], jp[(q, 1)])
            if near == far or far in name: continue                   # a hop that joins nothing new
            k += 1
            name[far] = f"{net}_J{k}"
            rot = ("R180" if flip else "R0") if horiz else ("R270" if flip else "R90")
            moved = [(it[1]["ref"], it[1]["pad"]) for i, it in enumerate(items) if it[0] == "pad" and comp[i] == far and it[1]["ref"] != "LAND"]
            jumpers.append(dict(name=f"RJ{k}", x=(a[0] + b[0]) / 2, y=(a[1] + b[1]) / 2, rot=rot, net1=name.get(near, net), net2=name[far], moved=moved))
        for i, it in enumerate(items):
            if it[0] == "trk": track_net[id(it[1])] = name.get(comp[i], net)
    return jumpers, [dict(t, net=track_net[id(t)]) for t in tracks]

def widen(pads, tracks):
    """Power tracks go to POWER_WIDE wherever that still leaves CLEAR to everything else."""
    board = Polygon(OUTLINE)
    geo = lambda t, w: LineString([(t["x1"], t["y1"]), (t["x2"], t["y2"])]).buffer(w / 2)
    base = lambda n: n.split("_J")[0]
    n_wide = 0
    for t in tracks:
        if base(t["net"]) not in POWER_NETS: continue
        g = geo(t, POWER_WIDE)
        others = [p["shape"] for p in pads if p["net"] != t["net"]] + [geo(o, o["w"]) for o in tracks if o["net"] != t["net"]]
        if board.buffer(-EDGE).contains(g) and all(g.distance(o) >= CLEAR + 1e-3 for o in others):
            t["w"] = POWER_WIDE; n_wide += 1
    return n_wide

# ---------------------------------------------------------------- schematic: add the jumpers
def write_schematic(jumpers):
    root = ET.parse(SRC_SCH).getroot()
    sch = root.find("drawing/schematic")
    # R8 4.7k: LED enable is GPIO20 (U0RXD) on rev C, whose reset-time weak pull-up (~45k) against 10k would put
    # 0.6 V on Q5's gate (Vth 0.5-1.0 V) during the ROM/bootloader window; 4.7k keeps it at 0.3 V.
    [p for p in sch.find("parts") if p.get("name") == "R8"][0].set("value", "4.7K")
    libs = {(l.get("name"), l.get("urn") or None): l for l in sch.find("libraries")}
    ada = libs[("adafruit", None)]
    ada.find("packages").append(jumper_package())
    ds = [d for d in ada.find("devicesets") if d.get("name") == "R-US_"][0]
    dev = copy.deepcopy([d for d in ds.find("devices") if d.get("name") == "M1406"][0])
    dev.set("name", "1210J"); dev.set("package", JUMPER_PKG["name"])
    ds.find("devices").append(dev)
    parts = {p.get("name"): p for p in sch.find("parts")}
    sheet = sch.find("sheets/sheet")
    inst = {(i.get("part"), i.get("gate")): i for i in sheet.find("instances")}
    nets = sheet.find("nets")
    N = {n.get("name"): n for n in nets}

    def pin_xy(part, gate, pin):
        p = parts[part]
        lib = libs[(p.get("library"), p.get("library_urn") or None)]
        dset = [d for d in lib.find("devicesets") if d.get("name") == p.get("deviceset")][0]
        sym = [g.get("symbol") for g in dset.find("gates") if g.get("name") == gate][0]
        pn = [q for s in lib.find("symbols") if s.get("name") == sym for q in s.findall("pin") if q.get("name") == pin][0]
        i = inst[(part, gate)]
        dx, dy, mir, ang = tf(float(pn.get("x")), float(pn.get("y")), i.get("rot"))
        px, py = float(i.get("x")) + dx, float(i.get("y")) + dy
        ox, oy, _, _ = tf(-1.0, 0.0, pn.get("rot"))                 # a pin points into its symbol: outward is the other way
        ox, oy, _, _ = tf(ox, oy, i.get("rot"))
        return px, py, ox, oy

    def stub(net, part, gate, pin):
        px, py, ox, oy = pin_xy(part, gate, pin)
        seg = el("segment"); seg.text = "\n"
        seg.append(el("pinref", part=part, gate=gate, pin=pin))
        seg.append(el("wire", x1=px, y1=py, x2=px + 5.08 * ox, y2=py + 5.08 * oy, width=0.1524, layer="91"))
        seg.append(el("label", x=px + 2.54 * ox + (0 if abs(ox) > 0.5 else 0.5), y=py + 2.54 * oy + (0.5 if abs(ox) > 0.5 else 0), size=1.27, layer="95"))
        net.append(seg)

    def pad_pin(part, pad):                                         # board pad -> schematic (gate, pin)
        p = parts[part]
        lib = libs[(p.get("library"), p.get("library_urn") or None)]
        dset = [d for d in lib.find("devicesets") if d.get("name") == p.get("deviceset")][0]
        dv = [d for d in dset.find("devices") if d.get("name") == (p.get("device") or "")][0]
        return [(c.get("gate"), c.get("pin")) for c in dv.find("connects") if pad in c.get("pad").split()]

    narrow_pads(sch.find("libraries"))
    for n in nets:
        if n.get("name") in RENAME: n.set("name", RENAME[n.get("name")])
    N = {n.get("name"): n for n in nets}

    def drop_empty():
        for n in list(nets):
            if not n.findall("segment"): nets.remove(n)

    def detach(part, gate, pin):
        """Take a pin off whatever net it is on. Other pins that shared its segment get their own labelled stubs."""
        for n in list(nets):
            for seg in n.findall("segment"):
                refs = seg.findall("pinref")
                if any((r.get("part"), r.get("gate"), r.get("pin")) == (part, gate, pin) for r in refs):
                    n.remove(seg)
                    for r in refs:
                        if (r.get("part"), r.get("gate"), r.get("pin")) != (part, gate, pin):
                            stub(n, r.get("part"), r.get("gate"), r.get("pin"))
        drop_empty()

    pad2pin = {pad: pin for (pin, pad) in [(c.get("pin"), c.get("pad")) for c in
               [d for l in libs.values() for ds_ in l.find("devicesets") for d in ds_.find("devices")
                if ds_.get("name") == parts["U1"].get("deviceset")][0].find("connects")]}
    old_pin = {}
    for net in PINMAP:                                              # the new GPIO map: free every pin involved, then connect
        for seg in N[net].findall("segment"):
            for r in seg.findall("pinref"):
                if r.get("part") == "U1": old_pin[net] = r.get("pin")
    for net, pad in PINMAP.items():
        if old_pin[net] != pad2pin[pad]:
            detach("U1", "G$1", old_pin[net]); detach("U1", "G$1", pad2pin[pad])
    for net, pad in PINMAP.items():
        if old_pin[net] != pad2pin[pad]:
            if net not in {n.get("name") for n in nets}:
                nn = el("net", name=net, class_="0"); nn.text = "\n"; nets.append(nn)
            stub([n for n in nets if n.get("name") == net][0], "U1", "G$1", pad2pin[pad])
    N = {n.get("name"): n for n in nets}

    for q, j in enumerate(jumpers):
        sch.find("parts").append(el("part", name=j["name"], library="adafruit", deviceset="R-US_", device="1210J", value="0"))
        parts[j["name"]] = sch.find("parts")[-1]
        i = el("instance", part=j["name"], gate="G$1", x=220.98, y=30.48 + 12.7 * q)
        sheet.find("instances").append(i); inst[(j["name"], "G$1")] = i
        new = el("net", name=j["net2"], class_="0"); new.text = "\n"
        nets.append(new); N[j["net2"]] = new
    for j in jumpers:
        stub(N[j["net1"]], j["name"], "G$1", "1")
        stub(N[j["net2"]], j["name"], "G$1", "2")
        for part, pad in j["moved"]:
            for gate, pin in pad_pin(part, pad):
                refs = [(seg, r) for n in nets for seg in n.findall("segment") for r in seg.findall("pinref")
                        if (r.get("part"), r.get("gate"), r.get("pin")) == (part, gate, pin)]
                assert len(refs) == 1, (part, gate, pin, len(refs))
                seg, r = refs[0]
                seg.remove(r)
                px, py, _, _ = pin_xy(part, gate, pin)
                for wv in seg.findall("wire"):                      # no old-net wire may keep touching the moved pin
                    if any(abs(float(wv.get(a)) - px) < 1e-3 and abs(float(wv.get(b)) - py) < 1e-3 for a, b in (("x1", "y1"), ("x2", "y2"))):
                        seg.remove(wv)
                if not seg.findall("pinref") and not seg.findall("wire"):
                    [n for n in nets if seg in list(n)][0].remove(seg)
                stub(N[j["net2"]], part, gate, pin)
    save(root, DST_SCH)

# ---------------------------------------------------------------- KiCad / Freerouting
KPY = [str(KICAD / "python.exe"), str(HERE / "kicad_fr.py")]

def to_kicad(brd, pcb):
    (pcb.parent / "modelmap.json").write_text("{}")
    subprocess.run([sys.executable, str(HERE / "eagle2kicad.py"), str(brd), str(pcb), str(pcb.parent / "modelmap.json")],
                   check=True, capture_output=True)
    pro = json.loads((HERE / "rules.kicad_pro").read_text())                 # the rev B reviewer's rule set, tightened
    rules = pro["board"]["design_settings"]["rules"]
    rules.update(min_clearance=CLEAR, min_track_width=TRACK, min_copper_edge_clearance=EDGE, min_resolved_spokes=1)
    pro["net_settings"]["classes"][0].update(track_width=TRACK, clearance=CLEAR)
    pcb.with_suffix(".kicad_pro").write_text(json.dumps(pro, indent=1))

def autoroute(k, jumpers=(), seconds=240):
    """Placed board -> single-layer Specctra DSN -> Freerouting -> SES -> tracks. Returns (tracks, finished)."""
    root, pads = placed_board(jumpers, inset=EDGE - CLEAR, landing_via=False)
    save(root, k / "route.brd")
    to_kicad(k / "route.brd", k / "route.kicad_pcb")
    # the wire landing as a one-pad footprint, so the router sees a pin
    import re, uuid
    lines = (k / "route.kicad_pcb").read_text(encoding="utf-8").rstrip().split("\n")
    net = re.search(r'\(net (\d+) "%s"\)' % re.escape(LANDING[0]), "\n".join(lines)).group(1)
    U = lambda: '"%s"' % uuid.uuid4()
    prop = '  (property "%s" "%s" (at 0 0 0) (layer "F.Fab") (hide yes) (uuid %s) (effects (font (size 0.8 0.8) (thickness 0.12))))'
    fp = [' (footprint "EAGLE:LAND" (layer "F.Cu") (uuid %s) (at %g %g)' % (U(), LANDING[1], -LANDING[2]),
          prop % ("Reference", "LAND", U()), prop % ("Value", "landing", U()), '  (attr through_hole)',
          '  (pad "1" thru_hole circle (at 0 0) (size %g %g) (drill 1) (layers "*.Cu" "*.Mask") (net %s "%s") (uuid %s))'
          % (LANDING_D, LANDING_D, net, LANDING[0], U()), ' )']
    assert lines[-1] == ")"
    # every footprint on the front for the router: a back-side part would carry its pads to the layer that is not there
    lines = [l.replace('(layer "B.Cu")', '(layer "F.Cu")') if l.startswith(" (footprint") else l for l in lines]
    (k / "route.kicad_pcb").write_text("\n".join(lines[:-1] + fp + [")"]) + "\n", encoding="utf-8")
    subprocess.run(KPY + ["dsn", str(k / "route.kicad_pcb"), str(k / "route.dsn")], check=True, capture_output=True, timeout=120)
    # one signal layer: drop B.Cu from the layer list and from every padstack
    dsn = (k / "route.dsn").read_text()
    dsn, n = re.subn(r'\n    \(layer B\.Cu\n      \(type signal\)\n      \(property\n        \(index 1\)\n      \)\n    \)', "", dsn)
    assert n == 1, "B.Cu layer block not found in the DSN"
    dsn = "\n".join(l for l in dsn.split("\n") if "B.Cu" not in l)
    dsn = re.sub(r"\(clearance [\d.]+ \(type smd_smd\)\)", "(clearance %d (type smd_smd))" % (CLEAR * 1000), dsn)
    # GND is the leftover copper (a pour), so the router does not get it: its pads stay as obstacles
    dsn, n = re.subn(r"\n    \(net GND\n      \(pins [^)]*\)\n    \)", "", dsn)
    assert n == 1, "GND net not found in the DSN"
    dsn = dsn.replace(" GND IMU_INT", " IMU_INT")
    (k / "route.dsn").write_text(dsn)
    if FR_SETTINGS.exists():                                                 # no GUI, no analytics
        st = json.loads(FR_SETTINGS.read_text())
        st["profile"].update(allow_telemetry=False, allow_contact=False); st["gui"]["enabled"] = False
        st["usage_and_diagnostic_data"]["disable_analytics"] = True
        FR_SETTINGS.write_text(json.dumps(st, indent=2))
    (k / "route.ses").unlink(missing_ok=True)
    try:                                                                     # it only stops by itself when nothing is left unrouted
        log = subprocess.run([str(JAVA), "-jar", str(FREEROUTING), "-de", str(k / "route.dsn"), "-do", str(k / "route.ses")],
                             capture_output=True, text=True, timeout=seconds)
        text = log.stdout + log.stderr
    except subprocess.TimeoutExpired as e:
        text = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
    (k / "freerouting.log").write_text(text)
    best = re.findall(r"\((\d+) unrouted\)", text)
    if not (k / "route.ses").exists():
        return [], False, min(map(int, best)) if best else None
    subprocess.run(KPY + ["ses", str(k / "route.kicad_pcb"), str(k / "route.ses"), str(k / "tracks.json")],
                   check=True, capture_output=True, timeout=120)
    return json.loads((k / "tracks.json").read_text()), True, min(map(int, best)) if best else 0

def drc(pcb):
    subprocess.run(KPY + ["fill", str(pcb)], check=True, capture_output=True, timeout=120)
    rpt = pcb.with_name("drc.json")
    subprocess.run([str(KICAD / "kicad-cli.exe"), "pcb", "drc", "--format", "json", "--severity-all", "-o", str(rpt), str(pcb)],
                   check=True, capture_output=True, timeout=120)
    return json.loads(rpt.read_text())

def preview(pads, tracks, path, mirror=False, scale=30):
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (int(BW * scale) + 40, int(BH * scale) + 40), (250, 250, 248)); d = ImageDraw.Draw(im)
    T = lambda x, y: (20 + ((BW - x) if mirror else x) * scale, 20 + (BH - y) * scale)
    d.polygon([T(*p) for p in OUTLINE], outline=(60, 60, 60), fill=(238, 242, 236))
    cu = (176, 106, 40) if not mirror else (214, 190, 160)
    for t in tracks:
        a, b = T(t["x1"], t["y1"]), T(t["x2"], t["y2"]); r = t["w"] * scale / 2
        col = (120, 150, 90) if t["net"].startswith("GND") else cu
        d.line([a, b], fill=col, width=max(1, int(round(t["w"] * scale))))
        for q in (a, b): d.ellipse([q[0] - r, q[1] - r, q[0] + r, q[1] + r], fill=col)
    for p in pads:
        jumper = p["ref"].startswith("RJ")
        col = (40, 110, 200) if jumper else (120, 150, 90) if (p["net"] or "").startswith("GND") else cu if p["net"] else (170, 170, 170)
        d.polygon([T(*q) for q in p["shape"].exterior.coords], fill=col, outline=(70, 45, 10))
        if p["tht"]: x, y = T(*p["xy"]); d.ellipse([x - 0.45 * scale, y - 0.45 * scale, x + 0.45 * scale, y + 0.45 * scale], fill=(250, 250, 248))
    for name, (x, y, rot) in PLACE.items(): d.text(T(x, y), name, fill=(20, 20, 20), anchor="mm")
    im.save(path)

def write_scr_dru(root):
    """The EAGLE side: a script that rebuilds this board on the board EAGLE creates from the schematic, and the rules."""
    board = root.find("drawing/board")
    out = ["# Generated by hardware/pcb/revc.py. Open %s.sch, then File > Execute Script > this file." % NAME,
           "BOARD;", "GRID MM;", "SET WIRE_BEND 2;", "SET OPTIMIZING OFF;",
           "DISPLAY NONE 20;", "GROUP ALL;", "DELETE (>0 0);", "DISPLAY ALL;"]          # EAGLE's default outline
    for layer in ("20", "46", "22"):
        out.append(f"LAYER {layer};")
        for c in board.find("plain"):
            if c.get("layer") != layer: continue
            if c.tag == "wire":
                out.append(f"WIRE {c.get('width')} ({c.get('x1')} {c.get('y1')}) ({c.get('x2')} {c.get('y2')});")
            elif c.tag == "circle":
                x, y, r = (float(c.get(q)) for q in ("x", "y", "radius"))
                out.append(f"CIRCLE {c.get('width')} ({x:g} {y:g}) ({x + r:g} {y:g});")
    for e in board.iter("element"):
        out.append(f"MOVE '{e.get('name')}' ({e.get('x')} {e.get('y')});")
        out.append(f"ROTATE ={e.get('rot') or 'R0'} '{e.get('name')}';")
    out.append("LAYER 1;")
    for sg in board.iter("signal"):
        for v in sg.findall("via"):
            out += [f"CHANGE DRILL {v.get('drill')};", f"VIA '{sg.get('name')}' {v.get('diameter')} ROUND ({v.get('x')} {v.get('y')});"]
        for w in sg.findall("wire"):
            out.append(f"WIRE '{sg.get('name')}' {w.get('width')} ({w.get('x1')} {w.get('y1')}) ({w.get('x2')} {w.get('y2')});")
    for sg in board.iter("signal"):
        for pg in sg.findall("polygon"):
            out += [f"CHANGE ISOLATE {pg.get('isolate')};", "CHANGE ORPHANS OFF;", "CHANGE THERMALS ON;", "CHANGE POUR SOLID;", "CHANGE RANK 1;"]
            pts = " ".join(f"({v.get('x')} {v.get('y')})" for v in pg.findall("vertex"))
            first = pg.find("vertex")
            out.append(f"POLYGON '{sg.get('name')}' {pg.get('width')} {pts} ({first.get('x')} {first.get('y')});")
    out += ["SET OPTIMIZING ON;", "RATSNEST;", "WINDOW FIT;"]
    DST_SCR.write_text("\n".join(out) + "\n", encoding="ascii", newline="\r\n")
    rules = {q.get("name"): q.get("value") for q in board.find("designrules")}
    for q in ("mdWireWire", "mdWirePad", "mdWireVia", "mdPadPad", "mdPadVia", "mdViaVia", "mdSmdPad", "mdSmdVia", "mdSmdSmd"):
        rules[q] = "%gmm" % CLEAR
    rules.update(msWidth="%gmm" % TRACK, mdCopperDimension="%gmm" % EDGE, msDrill="0.7mm", rvPadTop="0.25", rvPadBottom="0.25",
                 rlMinPadTop="0.25mm", rlMaxPadTop="0.25mm", rlMinPadBottom="0.25mm", rlMaxPadBottom="0.25mm",
                 psElongationLong="%d" % round((ELONG - 1) * 100))
    lines = [f"description[en] = <b>Heart beacon {NAME}</b>: {TRACK} mm min width, {CLEAR} mm clearance, single sided (CNC V-bit)"]
    lines += [f"{q} = {v}" for q, v in rules.items() if q]
    DST_DRU.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\r\n")
    return sum(1 for l in out if l.startswith("WIRE '"))

if __name__ == "__main__":
    k = OUT / "kicad"; k.mkdir(parents=True, exist_ok=True)
    root, pads = placed_board()
    checks(pads)
    if "place" in sys.argv:
        preview(pads, [], OUT / "revC3_place.png"); sys.exit(0)
    jumpers = []
    if "--own" in sys.argv:
        tracks = []                                               # no autorouter: the completion router does it all
    elif "--reuse" in sys.argv:
        tracks = json.loads((k / "tracks.json").read_text())
    else:
        tracks, finished, best = autoroute(k, jumpers, int(sys.argv[sys.argv.index("--seconds") + 1]) if "--seconds" in sys.argv else 240)
        print(f"freerouting: finished {finished}, fewest unrouted seen {best}", flush=True)
        if not finished: sys.exit(1)
    root, pads = placed_board(jumpers)
    checks(pads)
    import router                                                 # finish what the autorouter left open, then GND
    nets = sorted({p["net"] for p in pads if p["net"]})
    hp = lambda n: (lambda q: (max(x for x, _ in q) - min(x for x, _ in q)) + (max(y for _, y in q) - min(y for _, y in q)))([p["xy"] for p in pads if p["net"] == n])
    first = [n for n in FIRST if n in nets]                       # order found by trying: short nets, then GND, BATT+ last
    last = [n for n in LAST if n in nets]
    order = first + sorted([n for n in nets if n not in first and n not in last], key=hp) + last
    pre, _, miss = router.complete(pads, tracks, OUTLINE, BW, BH, {n: TRACK for n in nets}, CLEAR, EDGE, first, log=lambda *a: None)
    assert not miss, miss
    tracks = tracks + pre
    for net, a, b in PRELINKS:                                    # connections that get their lane before the rest
        two = [dict(p, net=(net if (p["ref"], p["pad"]) in (a, b) else None) if p["net"] == net else p["net"]) for p in pads]
        pre, _, miss = router.complete(two, tracks, OUTLINE, BW, BH, {n: TRACK for n in nets}, CLEAR, EDGE, [net], log=lambda *a: None)
        if miss: print(f"pre-link {net} {a} {b} does not route", flush=True)
        tracks = tracks + pre
    more, hops, air = router.complete(pads, tracks, OUTLINE, BW, BH, {n: TRACK for n in nets}, CLEAR, EDGE, order,
                                      jump=JUMPER_PKG, pour=("GND",), log=lambda *a: None)
    tracks = tracks + more
    if hops:                                                      # each hop is a 1210 jumper and splits its net
        jumpers, tracks = split_nets(pads, tracks, hops)
        root, pads = placed_board(jumpers)
        checks(pads)
    print(f"completion router: {len(more)} segments added; airwires left: {air}", flush=True)
    (k / "tracks_final.json").write_text(json.dumps(tracks))
    n_wide = widen(pads, tracks)
    S = {sg.get("name"): sg for sg in root.find("drawing/board/signals")}
    for t in tracks:
        S[t["net"]].append(el("wire", x1=t["x1"], y1=t["y1"], x2=t["x2"], y2=t["y2"], width=t["w"], layer="1"))
    pg = el("polygon", width=TRACK, layer="1", isolate=CLEAR, thermals="yes", orphans="no"); pg.text = "\n"
    for x, y in OUTLINE: pg.append(el("vertex", x=x, y=y))                # GND is the leftover copper
    S["GND"].append(pg)
    save(root, OUT / f"{NAME}_check.brd")
    n_wires = write_scr_dru(root)
    write_schematic(jumpers)
    to_kicad(OUT / f"{NAME}_check.brd", k / f"{NAME}.kicad_pcb")
    r = drc(k / f"{NAME}.kicad_pcb")
    preview(pads, tracks, OUT / "revC3_copper_side.png")
    preview(pads, tracks, OUT / "revC3_from_the_back.png", mirror=True)
    viol, unc = r.get("violations", []), r.get("unconnected_items", [])
    errs = [v for v in viol if v["severity"] == "error"]
    length = sum(math.hypot(t["x2"] - t["x1"], t["y2"] - t["y1"]) for t in tracks)
    print(f"board {BW} x {BH} mm; {n_wires} track segments, {length:.0f} mm; jumpers {len(jumpers)}; widened to {POWER_WIDE}: {n_wide}")
    print("DRC errors", len(errs), "warnings", len(viol) - len(errs), "unconnected", len(unc))
    for v in viol + unc:
        print("  ", v["severity"], v["type"], v["description"], [(round(i["pos"]["x"], 2), round(-i["pos"]["y"], 2)) for i in v["items"]][:2])
    (OUT / "revC3.json").write_text(json.dumps(dict(size=[BW, BH], copper_mm=round(length), rules=dict(track=TRACK, clearance=CLEAR, edge=EDGE),
        pinmap=PINMAP, jumpers=jumpers, airwires=air, drc_errors=len(errs), drc_warnings=[v["type"] for v in viol if v["severity"] != "error"],
        drc_unconnected=len(unc)), indent=1))
