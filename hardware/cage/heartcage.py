"""Heart Beacon brass cage + jigs. One parametric source: params.toml -> out/*.stl, out/*.png, out/cage.json.

  python heartcage.py --check   asserts only (symmetry, triangles, valence, classes, clearance, openings)
  python heartcage.py           check, then STLs, renders and the cut list

Frame: x right, y up, z toward the viewer (front), origin at the heart's bounding-box centre, ring 2 at z = 0.
Rings: 0 front small, 1 front mid, 2 centre, 3 back mid, 4 back small. Front and back are the same shape,
so one former and one soldering fixture serve both halves.
"""
import json, math, sys, tomllib
from pathlib import Path
import numpy as np
import shapely, shapely.affinity
from shapely.geometry import Polygon, LineString, box as sbox
from scipy.optimize import least_squares

HERE = Path(__file__).parent
OUT = HERE / "out"
P = tomllib.loads((HERE / "params.toml").read_text())
H, BR, PCB, BAT, MNT, LED, JIG = (P[k] for k in ("heart", "brass", "pcb", "battery", "mount", "led", "jig"))
N = H["nodes_per_ring"]; M = N // 2
W = H["width"]; HT = W * H["height_ratio"]; D = W * H["depth_ratio"]
ZR = [D / 2, H["mid_z"], 0.0, -H["mid_z"], -D / 2]          # ring z
SHAPE = [2, 1, 0, 1, 2]                                      # ring -> shape index (0 centre, 1 mid, 2 small)
RR, SR = BR["ring_d"] / 2, BR["strut_d"] / 2
BANDS = ((2, 1), (1, 0), (2, 3), (3, 4))                     # (outer ring, inner ring); V apexes sit on the outer

# ---------------------------------------------------------------- outline (Cage.kt shape at d = 0)
def _shape(x, u):
    u = u + H["notch"] * np.exp(-x * x / 0.09) * np.maximum(0, u)
    a = x * x + u * u - 1
    return a ** 3 - x * x * u ** 3

def centre_outline(k=1440):
    """Ring 2 centreline, closed, from the notch clockwise (right side first)."""
    phi = np.arange(k) * 2 * np.pi / k
    dx, du = np.sin(phi), np.cos(phi)
    lo, hi = np.zeros(k), np.full(k, 0.02)
    for _ in range(200):                       # first sign change along each ray, as Cage.surfaceRadius does
        go = _shape(dx * hi, du * hi) < 0
        if not go.any(): break
        lo[go] = hi[go]; hi[go] += 0.02
    for _ in range(40):
        m = (lo + hi) / 2; ins = _shape(dx * m, du * m) < 0
        lo[ins] = m[ins]; hi[~ins] = m[~ins]
    r = (lo + hi) / 2
    xy = np.c_[dx * r, du * r]
    mn, mx = xy.min(0), xy.max(0)
    return (xy - (mn + mx) / 2) * np.array([W, HT]) / (mx - mn)

def right_half(poly):
    """Polygon -> dense right-half polyline from the notch (x=0, top) to the tip (x=0, bottom)."""
    ext = poly.exterior
    cut = ext.intersection(LineString([(0, -HT), (0, HT)]))
    ys = sorted(p.y for p in cut.geoms)
    right = ext.intersection(sbox(0, -HT, W, HT))
    if right.geom_type != "LineString": right = shapely.line_merge(right)
    c = np.array(right.coords)
    if c[0, 1] < c[-1, 1]: c = c[::-1]
    c[0] = (0, ys[-1]); c[-1] = (0, ys[0])
    s = np.r_[0, np.cumsum(np.hypot(*np.diff(c, axis=0).T))]
    t = np.linspace(0, s[-1], int(s[-1] / 0.5) + 2)
    return np.c_[np.interp(t, s, c[:, 0]), np.interp(t, s, c[:, 1])]

class Half:
    """Right half of a closed mirror-symmetric curve, parametrised 0..1 by arc length."""
    def __init__(self, pts):
        self.pts = pts
        self.s = np.r_[0, np.cumsum(np.hypot(*np.diff(pts, axis=0).T))]
        self.len = self.s[-1]
    def at(self, u):
        t = np.atleast_1d(u) * self.len
        return np.c_[np.interp(t, self.s, self.pts[:, 0]), np.interp(t, self.s, self.pts[:, 1])]
    def closed(self):
        return np.r_[self.pts, self.pts[-2:0:-1] * [-1, 1]]

_C = Polygon(centre_outline())
def inset(d):
    return _C.buffer(-d, join_style="round", resolution=32) if d else _C
HALF = [Half(right_half(inset(d))) for d in (0.0, H["mid_inset"], H["small_inset"])]

# ---------------------------------------------------------------- nodes and struts
def lengths(u):
    """u: 3 x (M-1) interior node parameters -> strut lengths of bands A (0->1) and B (1->2), right half."""
    pts = [np.r_[[h.pts[0]], h.at(ui), [h.pts[-1]]] for h, ui in zip(HALF, u)]
    out = []
    for a, b, dz in ((0, 1, ZR[1] - ZR[2]), (1, 2, ZR[0] - ZR[1])):
        out += [math.hypot(*(pts[a][j] - pts[b][j]), dz) for j in range(M + 1)]          # meridians
        out += [math.hypot(*(pts[a][j] - pts[b][j + 1]), dz) for j in range(M)]          # diagonals
    return np.array(out), pts

def kmeans1d(x, k):
    c = np.quantile(x, (np.arange(k) + 0.5) / k)
    for _ in range(100):
        lab = np.abs(x[:, None] - c[None]).argmin(1)
        c = np.array([x[lab == i].mean() if (lab == i).any() else c[i] for i in range(k)])
    return lab, c

def solve_nodes():
    """Slide nodes along their rings until every strut falls into <= max_classes length classes."""
    u = np.tile(np.arange(1, M) / M, (3, 1))
    L, _ = lengths(u)
    lab, _ = kmeans1d(L, H["max_classes"])
    for _ in range(6):
        def res(v):
            uu = v.reshape(3, M - 1)
            L, _ = lengths(uu)
            cm = np.array([L[lab == i].mean() if (lab == i).any() else 0 for i in range(H["max_classes"])])
            sp = np.diff(np.c_[np.zeros(3), uu, np.ones(3)], axis=1)
            return np.r_[L - cm[lab], H["even_weight"] * M * (sp - 1 / M).ravel()]
        u = least_squares(res, u.ravel()).x.reshape(3, M - 1)
        L, _ = lengths(u)
        lab, _ = kmeans1d(L, H["max_classes"])
    assert (np.diff(np.c_[np.zeros(3), u, np.ones(3)], axis=1) > 0.3 / M).all(), "nodes bunched up"
    return u

class Cage:
    def __init__(self):
        self.u = solve_nodes()
        _, hp = lengths(self.u)
        # full ring node lists: j = 0 notch, 1..M-1 right, M tip, M+1..N-1 left (mirror of N-j)
        self.xy = [np.r_[p, p[-2:0:-1] * [-1, 1]] for p in hp]
        self.ring = [np.c_[HALF[SHAPE[r]].closed(), np.full(len(HALF[SHAPE[r]].closed()), ZR[r])] for r in range(5)]
        # the one window: the V at the back notch is left out so the USB-C plug can reach the module
        self.removed = {((2, 0), (3, 0)), ((2, 0), (3, 1)), ((2, 0), (3, N - 1))}
        self.struts, self.tris = [], []
        for outer, inner in BANDS:
            for j in range(N):
                e_m = ((outer, j), (inner, j))
                for jn in self.diag_to(j):
                    e_d = ((outer, j), (inner, jn))
                    e_m2 = ((outer, jn), (inner, jn))
                    for e1, e2 in ((e_m, e_d), (e_d, e_m2)):
                        if e1 not in self.removed and e2 not in self.removed: self.tris.append((e1, e2))
                    if e_d not in self.removed: self.struts.append(e_d)
                if e_m not in self.removed: self.struts.append(e_m)
        self.L = np.array([self.length(s) for s in self.struts])
        # classes: greedy over the sorted lengths, each class spanning <= 1.6 x tol
        self.cls_len, first = [], None
        for l in np.sort(self.L):
            if first is None or l - first > 1.6 * H["class_tol"]:
                first = l; self.cls_len.append([l, l])
            self.cls_len[-1][1] = l
        self.cls_len = [round(float(a + b) / 2, 1) for a, b in self.cls_len]
        self.cls = [int(np.argmin(np.abs(np.array(self.cls_len) - l))) for l in self.L]
    @staticmethod
    def diag_to(j):
        """Inner-ring nodes that outer node j's diagonals run to (always toward the tip; the notch node has two)."""
        return [1, N - 1] if j == 0 else [] if j == M else [j + 1] if j < M else [j - 1]
    def node(self, r, j): return np.r_[self.xy[SHAPE[r]][j % N], ZR[r]]
    def p(self, rj): return self.node(*rj)
    def length(self, s): return float(np.linalg.norm(self.p(s[0]) - self.p(s[1])))
    def segments(self):
        """Every rod as (a, b, radius): ring polyline pieces (2 mm) and struts."""
        segs = []
        for r in range(5):
            q = self.ring[r][::4]
            segs += [(a, b, RR) for a, b in zip(q, np.roll(q, -1, axis=0))]
        return segs + [(self.p(a), self.p(b), SR) for a, b in self.struts]
    def section(self, z):
        """Cross-section polygon of the strut surface at height z."""
        for outer, inner in BANDS:
            z0, z1 = ZR[outer], ZR[inner]
            if min(z0, z1) - 1e-9 <= z <= max(z0, z1) + 1e-9:
                t = (z - z0) / (z1 - z0)
                at = lambda j, k: self.node(outer, j)[:2] + t * (self.node(inner, k)[:2] - self.node(outer, j)[:2])
                pts = []
                for j in range(N):
                    if j > M: pts.append(at(j, j - 1))
                    pts.append(at(j, j))
                    if j < M: pts.append(at(j, j + 1))
                pts.append(at(0, N - 1))
                return Polygon(pts)
        raise ValueError(z)

# ---------------------------------------------------------------- what goes inside
sys.path.insert(0, str(HERE.parent / "pcb"))
import revc as BRD                       # rev C board: outline and part positions come from the board generator

def b2c(x, y):
    """Board (EAGLE, copper side view) -> cage xy. The copper side faces the front, the USB edge is up."""
    return (x - BRD.AX, y - BRD.BH + PCB["usb_edge_y"])
def brect(x0, y0, x1, y1):
    (ax, ay), (bx, by) = b2c(x0, y0), b2c(x1, y1)
    return sbox(min(ax, bx), min(ay, by), max(ax, bx), max(ay, by))

class Stack:
    """Front to back: mount plate (snaps onto ring 1), cell, SMD gap, board, then module / LIS3DH / switches."""
    def __init__(self):
        self.plate_front = ZR[1] + 0.3 * BR["ring_d"]            # ring 1 snaps into the plate's ears
        self.plate_back = self.plate_front - MNT["plate_t"]
        self.bat_front = self.plate_back - BAT["gap"]
        self.bat_back = self.bat_front - BAT["t"]
        self.bf = self.bat_back - BAT["gap"] - PCB["smd_h"]      # board front = copper side
        self.bk = self.bf - PCB["thickness"]                     # board back: sockets, switches
        self.usb_z = self.bk - 5.5 - (PCB["top_h"] - 5.5) / 2
        cy = PCB["usb_edge_y"] - BRD.BH / 2 + BAT["dy"]
        self.bat = sbox(-BAT["l"] / 2, cy - BAT["w"] / 2, BAT["l"] / 2, cy + BAT["w"] / 2)
        self.board = Polygon([b2c(*q) for q in BRD.OUTLINE])
        P = BRD.PLACE; ax = BRD.AX
        s1, s3 = P["S1"], P["S3"]
        self.s1 = brect(s1[0] - 6, s1[1] - 6, s1[0] + 6, s1[1] + 6)
        self.s1_xy = b2c(s1[0], s1[1])
        self.slider = brect(s3[0] + 1.9, s3[1] - 4.5, BRD.BW + PCB["slider_past_edge"], s3[1] + 4.5)   # sticks out of the right edge
        hdr = lambda n, half: brect(P[n][0] - half, P[n][1] - 1.3, P[n][0] + half, P[n][1] + 1.3)
        self.items = [   # name, xy polygon, z0, z1
            ("board", self.board, self.bk, self.bf),
            ("SMD side", self.board, self.bf, self.bf + PCB["smd_h"]),
            ("battery", self.bat, self.bat_back, self.bat_front),
            ("module", brect(ax - 9, BRD.BH - 24.05, ax + 9, BRD.BH), self.bk - PCB["top_h"], self.bk),
            ("LIS3DH", brect(P["SV5"][0] - 1.27, BRD.IMU_T - 19.05, P["SV3"][0] + 1.27, BRD.IMU_T + 3.81), self.bk - 8.5, self.bk),
            ("S1 body", self.s1, self.bk - 4.0, self.bk),
            ("S1 actuator", brect(s1[0] - 3.7, s1[1] - 3.7, s1[0] + 3.7, s1[1] + 3.7), self.bk - PCB["switch_h"], self.bk),
            ("slide switch", brect(s3[0] - 1.9, s3[1] - 5.2, s3[0] + 1.9, s3[1] + 5.2), self.bk - PCB["bottom_h"], self.bk),
            ("slider", self.slider, self.bk - PCB["bottom_h"], self.bk),
            ("SV1", hdr("SV1", 3.9), self.bk - 8.5, self.bk),
            ("BATT", hdr("BATT", 2.6), self.bk - 8.5, self.bk),
            ("JP1", hdr("JP1", 2.6), self.bk - 8.5, self.bk),
        ]
        uw, ut = PCB["usb_plug"]
        self.usb_sweep = (sbox(-uw / 2, PCB["usb_edge_y"], uw / 2, HT), self.usb_z - ut / 2, self.usb_z + ut / 2)
        self.sw_sweep = (self.slider, -D, self.bk - PCB["bottom_h"])    # reached from the back, through ring 4
        self.btn_sweep = (shapely.Point(*self.s1_xy).buffer(5.5), -D, self.bk - PCB["switch_h"])

def rod_gap(cage, poly, z0, z1):
    """Smallest surface-to-surface distance from any rod to the prism poly x [z0, z1]."""
    best = 1e9
    for a, b, r in cage.segments():
        k = max(2, int(np.linalg.norm(b - a) / 0.5) + 1)
        q = a + (b - a) * np.linspace(0, 1, k)[:, None]
        dxy = shapely.distance(poly, shapely.points(q[:, :2]))
        dz = np.maximum(0, np.maximum(z0 - q[:, 2], q[:, 2] - z1))
        best = min(best, float(np.hypot(dxy, dz).min()) - r)
    return best

# ---------------------------------------------------------------- LED rails (front face, inside ring 0)
# WS2812B pads, lens toward the viewer (datasheet top view): 1 VDD, 2 DOUT, 3 VSS (notched corner), 4 DIN.
PADS = {"VDD": (-2.45, 1.6), "DOUT": (-2.45, -1.6), "VSS": (2.45, -1.6), "DIN": (2.45, 1.6)}

class Rails:
    """Two bare rails (outer GND, inner LED+), 9 LEDs bridging them, chain running clockwise from the tip."""
    def __init__(self):
        mid = Polygon(HALF[2].closed()).buffer(-LED["inset"], resolution=32)
        mid = mid.buffer(-6).buffer(6)                            # blunt tip: 1 mm wire bends, LED 0 sits flat
        self.z = ZR[0] - LED["recess"]
        self.mid = Half(right_half(mid))
        self.gnd = Half(right_half(mid.buffer(LED["pitch"] / 2)))
        self.vcc = Half(right_half(mid.buffer(-LED["pitch"] / 2)))
        self.leds = [self.frame(k / 9) for k in range(9)]         # (centre, tangent, outward normal)
        self.caps = [self.frame((g + 0.5) / 9) for g in LED["cap_gaps"]]
        ring0 = LineString(HALF[2].closed())
        self.spokes = []
        for g in LED["spoke_gaps"]:
            c, t, n = self.frame((g + 0.5) / 9)
            a = c + n * LED["pitch"] / 2
            q = ring0.interpolate(ring0.project(shapely.Point(a)))
            self.spokes.append((np.r_[a, self.z], np.r_[q.x, q.y, ZR[0]]))
    def point(self, f):
        """Centre-line point at fraction f of the loop, from the tip clockwise as seen from the front (left side first)."""
        f = f % 1.0
        return self.mid.at(1 - 2 * f)[0] * [-1, 1] if f < 0.5 else self.mid.at(2 * f - 1)[0]
    def frame(self, f):
        c = self.point(f)
        t = self.point(f + 1e-3) - self.point(f - 1e-3); t /= np.linalg.norm(t)
        return c, t, np.array([-t[1], t[0]])                      # clockwise travel: outward is to the left of travel
    def led_angle(self, k):
        """Rotation of LED k (radians): DIN/DOUT land on the centre line, VDD inward, VSS outward."""
        t = self.leds[k][1]
        return math.atan2(-t[1], -t[0]) - math.atan2(1.6, 2.45)
    def pad(self, k, name):
        a = self.led_angle(k); x, y = PADS[name]
        return self.leds[k][0] + [x * math.cos(a) - y * math.sin(a), x * math.sin(a) + y * math.cos(a)]

# ---------------------------------------------------------------- self-check
def check(cage, st, rails):
    nodes = {(r, j): cage.node(r, j) for r in range(5) for j in range(N)}
    # 1 mirror symmetry
    for (r, j), q in nodes.items():
        assert np.allclose(q * [-1, 1, 1], nodes[(r, (N - j) % N)], atol=1e-6), f"node {r},{j} has no mirror"
    sset = {frozenset(s) for s in cage.struts}
    for a, b in cage.struts:
        assert frozenset({(a[0], (N - a[1]) % N), (b[0], (N - b[1]) % N)}) in sset, f"strut {a}-{b} has no mirror"
    print(f"ok  mirror symmetry: {len(nodes)} nodes, {len(cage.struts)} struts")
    # 2 triangles
    want = 8 * N - 4
    assert len(cage.tris) == want, (len(cage.tris), want)
    assert 170 <= want <= 200, "triangle count drifted from the current heart's 180"
    print(f"ok  triangles: {want} (current heart: 180)")
    # 3 valence
    val = {k: 2 for k in nodes}
    for a, b in cage.struts: val[a] += 1; val[b] += 1
    assert min(val.values()) >= 3, min(val.values())
    print(f"ok  valence: min {min(val.values())}, max {max(val.values())}")
    # 4 length classes
    assert len(cage.cls_len) <= H["max_classes"]
    dev = max(abs(l - cage.cls_len[c]) for l, c in zip(cage.L, cage.cls))
    assert dev <= H["class_tol"], f"strut {dev:.2f} mm off its class"
    print(f"ok  strut classes: {len(cage.cls_len)} classes, worst deviation {dev:.2f} mm (limit {H['class_tol']})")
    # 5 clearance and containment
    worst = 1e9
    for name, poly, z0, z1 in st.items:
        g = rod_gap(cage, poly, z0, z1)
        assert g >= 2.0, f"{name}: only {g:.2f} mm to a rod"
        for z in np.linspace(z0, z1, 5):
            assert cage.section(z).buffer(-2.0).contains(poly), f"{name} pokes through the cage at z={z:.1f}"
        worst = min(worst, g)
    print(f"ok  clearance: closest rod is {worst:.2f} mm from the PCB/battery volumes (limit 2)")
    # 6 openings
    g_usb = rod_gap(cage, *st.usb_sweep); g_sw = rod_gap(cage, *st.sw_sweep); g_btn = rod_gap(cage, *st.btn_sweep)
    assert g_usb >= 0.3, f"USB plug path blocked ({g_usb:.2f})"
    assert g_sw >= 0.3, f"slide switch path blocked ({g_sw:.2f})"
    assert g_btn >= 0.3, f"button path blocked ({g_btn:.2f})"
    print(f"ok  openings: USB plug path clear by {g_usb:.2f} mm, slide switch (from the back) by {g_sw:.2f} mm, "
          f"button by {g_btn:.2f} mm")
    # 7 LED chain: pixel 0 on the axis, pixel k mirrors pixel 9-k, pads land on the right rail
    assert abs(rails.leds[0][0][0]) < 1e-6
    for k in range(1, 9):
        assert np.allclose(rails.leds[k][0] * [-1, 1], rails.leds[9 - k][0], atol=1e-6), f"LED {k} not mirrored"
    assert rails.leds[1][0][0] < 0, "chain must run up the left lobe first (Cage.kt order)"
    gnd, vcc = LineString(rails.gnd.closed()), LineString(rails.vcc.closed())
    worst_on, worst_off = 0, 1e9
    for k in range(9):
        pt = lambda n: shapely.Point(rails.pad(k, n))
        worst_on = max(worst_on, vcc.distance(pt("VDD")), gnd.distance(pt("VSS")))
        worst_off = min(worst_off, *(r.distance(pt(n)) for r in (gnd, vcc) for n in ("DIN", "DOUT")))
    assert worst_on <= 0.75, f"a power pad sits {worst_on:.2f} mm off its rail"
    assert worst_off >= 1.5, f"a data pad is {worst_off:.2f} mm from a rail"
    assert Polygon(HALF[2].closed()).buffer(-RR - 1).contains(Polygon(rails.gnd.closed())), "rails do not fit in ring 0"
    print(f"ok  LED rails: chain order and mirror symmetry hold; power pads within {worst_on:.2f} mm of their rail, "
          f"data pads {worst_off:.2f} mm clear")
    return dict(clearance=round(worst, 2), usb=round(g_usb, 2), switch=round(g_sw, 2), button=round(g_btn, 2), class_dev=round(dev, 2))

def summary(cage, st):
    ring_len = [float(np.linalg.norm(np.diff(np.r_[q, q[:1]], axis=0), axis=1).sum()) for q in cage.ring]
    qty = [sum(1 for c in cage.cls if c == i) for i in range(len(cage.cls_len))]
    cut = [round(l - BR["ring_d"], 1) for l in cage.cls_len]      # struts butt against the ring wires
    return dict(
        size=dict(W=W + BR["ring_d"], H=round(HT + BR["ring_d"], 1), D=D + BR["ring_d"]),
        rings=[dict(ring=r, z=ZR[r], w=round(float(np.ptp(cage.ring[r][:, 0])), 1),
                    h=round(float(np.ptp(cage.ring[r][:, 1])), 1), developed=round(ring_len[r], 1)) for r in range(5)],
        nodes=5 * N, struts=len(cage.struts), rods=len(cage.struts) + 5, triangles=len(cage.tris),
        classes=[dict(cls=chr(65 + i), node_to_node=cage.cls_len[i], cut=cut[i], qty=qty[i]) for i in range(len(qty))],
        brass=dict(ring_mm=round(sum(ring_len)), strut_mm=round(sum(c * q for c, q in zip(cut, qty)))),
        z=dict(plate_front=round(st.plate_front, 2), battery=[round(st.bat_back, 2), round(st.bat_front, 2)],
               board=[round(st.bk, 2), round(st.bf, 2)], usb_centre=round(st.usb_z, 2)))

if __name__ == "__main__":
    cage, st, rails = Cage(), Stack(), Rails()
    info = summary(cage, st)
    print(json.dumps(info, indent=1))
    info["check"] = check(cage, st, rails)
    import build
    parts = build.make_parts(cage, st, rails, info)             # 8 every printed part fits the bed
    for name, (man, _, _) in parts.items():
        x, y, z = build.extent(man)
        assert min(max(x, y), 180) <= P["printer"]["bed"] and min(x, y) <= P["printer"]["bed"] and max(x, y) <= P["printer"]["bed"] \
            and z <= P["printer"]["height"], f"{name} {x:.0f} x {y:.0f} x {z:.0f} does not fit the bed"
    big = max(parts, key=lambda n: max(build.extent(parts[n][0])[:2]))
    print(f"ok  bed: all {len(parts)} parts fit {P['printer']['bed']} x {P['printer']['bed']} mm; largest is {big} at "
          f"{build.extent(parts[big][0])[0]:.0f} x {build.extent(parts[big][0])[1]:.0f} mm")
    if "--check" in sys.argv: sys.exit(0)
    build.run(cage, st, rails, info, parts)
