"""Meshes, STLs and renders for heartcage.py (run that, not this). Everything is derived from the checked geometry."""
import json, math
import numpy as np
import shapely
from shapely.geometry import Polygon, LineString, Point, box as sbox
from shapely.ops import unary_union
from manifold3d import Manifold, CrossSection, OpType
import trimesh
from PIL import Image
import heartcage as hc
from heartcage import N, M, ZR, RR, SR, BR, PCB, BAT, MNT, LED, JIG, HALF, OUT, b2c

CLR = BR["clearance"]
RAIL_R = BR["rail_d"] / 2
BRASS, PETG, GREEN, DARK, BLUE, WHITE, ORANGE, TAN = ((0.80, 0.62, 0.25), (0.55, 0.62, 0.70), (0.13, 0.42, 0.25),
                                                       (0.16, 0.16, 0.18), (0.20, 0.36, 0.80), (0.96, 0.96, 0.92),
                                                       (0.95, 0.50, 0.12), (0.62, 0.45, 0.33))

# ---------------------------------------------------------------- solid helpers
def cs(poly):
    loops = []
    for g in ([poly] if poly.geom_type == "Polygon" else list(poly.geoms)):
        g = shapely.geometry.polygon.orient(g)
        loops.append(np.array(g.exterior.coords)[:-1])
        loops += [np.array(i.coords)[:-1] for i in g.interiors]
    return CrossSection(loops)

def prism(poly, z0, z1):
    return cs(poly).extrude(z1 - z0).translate([0, 0, z0])

def union(parts):
    return Manifold.batch_boolean(list(parts), OpType.Add)

_ball = {}
def ball(p, r):
    if r not in _ball: _ball[r] = Manifold.sphere(r, 16)
    return _ball[r].translate([float(v) for v in p])

def capsule(a, b, r):
    return Manifold.batch_hull([ball(a, r), ball(b, r)])

def ringpoly(k): return Polygon(HALF[k].closed())
def closed_line(half): return LineString(np.r_[half.closed(), half.closed()[:1]])
def vf(man):
    m = man.to_mesh()
    return np.asarray(m.vert_properties)[:, :3].astype(float), np.asarray(m.tri_verts)

def rods(segs, sides=10):
    """Plain cylinders for rendering and the reference STL: (V, F)."""
    ang = np.arange(sides) * 2 * np.pi / sides
    V, F = [], []
    for a, b, r in segs:
        d = b - a; L = np.linalg.norm(d)
        if L < 1e-9: continue
        d = d / L
        u = np.cross(d, [0, 0, 1.0] if abs(d[2]) < 0.9 else [1.0, 0, 0]); u /= np.linalg.norm(u)
        v = np.cross(d, u)
        circ = (np.cos(ang)[:, None] * u + np.sin(ang)[:, None] * v) * r
        n0 = sum(len(x) for x in V)
        V.append(np.r_[a - d * 0.3 + circ, b + d * 0.3 + circ])
        i = np.arange(sides); j = (i + 1) % sides
        F.append(np.r_[np.c_[i, j, j + sides], np.c_[i, j + sides, i + sides]] + n0)
    return np.concatenate(V), np.concatenate(F)

# ---------------------------------------------------------------- parts
def former():
    """Stepped bending former: ring 2 on the base, ring 1/3 on the first step, ring 0/4 on the second."""
    step = 6.0; base_t = 3.0
    parts = [prism(ringpoly(0).buffer(8), 0, base_t)]
    cuts = []
    for k in range(3):
        z = base_t + step * k
        poly = ringpoly(k)
        parts.append(prism(poly.buffer(-RR), z, z + step))
        pockets = [g for g in getattr(poly.convex_hull.difference(poly), "geoms", [poly.convex_hull.difference(poly)])
                   if g.area > 3]
        zone = unary_union(pockets).buffer(5)                       # concave stretches need an outer fence
        fence = poly.buffer(RR + CLR + 3.2).difference(poly.buffer(RR + CLR)).intersection(zone)
        parts.append(prism(fence, z - 0.01, z + 4))
        marks = poly.buffer(RR + 2.0).exterior
        for j in range(N):                                           # node marks on the ledge next to the wire
            q = marks.interpolate(marks.project(Point(*hc_cage.xy[k][j])))
            cuts.append(prism(Point(q.x, q.y).buffer(0.8, resolution=6), z - 0.7, z + 5))
    cuts.append(prism(ringpoly(2).buffer(-9), -1, base_t + 3 * step + 1))
    return union(parts) - union(cuts)

def fixture(cage, rails):
    """Soldering dome for one half: rings 2, 1, 0 at height, a groove per strut, a relief pocket per joint."""
    zb = -RR
    parts = [prism(ringpoly(0).buffer(10), zb - JIG["base_t"], zb),
             prism(Polygon([cage.node(0, j)[:2] for j in range(N)]), zb, ZR[0])]
    cuts = []
    for outer, inner in ((2, 1), (1, 0)):
        for j in range(N):
            a, b = cage.node(outer, j), cage.node(inner, j)
            cuts.append(capsule(a, b, SR + CLR / 2))
            for jn in cage.diag_to(j):
                a2, b2 = cage.node(outer, jn), cage.node(inner, jn)
                cuts.append(capsule(a, b2, SR + CLR / 2))
                for tri in ((a, b, b2), (a, a2, b2)):
                    low = np.array(tri); low[:, 2] = zb
                    parts.append(Manifold.hull_points(np.r_[np.array(tri), low]))
    for r in (2, 1, 0):
        slot = closed_line(HALF[hc.SHAPE[r]]).buffer(RR + CLR / 2)
        cuts.append(prism(slot, ZR[r] - RR, ZR[r] + RR + 1.5))
        cuts += [ball(cage.node(r, j), JIG["relief_r"]) for j in range(N)]
    floor = rails.z - RAIL_R                                          # the LED rail assembly rests here, LEDs up
    cuts.append(prism(ringpoly(2).buffer(-2.5), floor, ZR[0] + 2))
    cuts.append(prism(ringpoly(2).buffer(-16), zb - JIG["base_t"] - 1, ZR[0] + 2))
    for a, b in rails.spokes:
        cuts += [ball(a, 3.0), ball(b, 3.0), capsule(a, b, RAIL_R + CLR / 2)]
    return (union(parts) - union(cuts)).translate([0, 0, JIG["base_t"] - zb])

def stop_ring():
    """Stands in for ring 2 when the back half is built: the loose strut ends rest against it."""
    return prism(ringpoly(0).buffer(2.5).difference(ringpoly(0).buffer(-0.3)), 0, RR + 0.3)

def rail_jig(rails):
    """LEDs lens-down in pockets, both rails across the pads, caps between the rails. Built mirrored."""
    ch = 0.8 * BR["rail_d"]; T = 2.0 + LED["body_t"] + ch; zc = T - ch
    mid = Polygon(rails.mid.closed())
    body = prism(mid.buffer(10).difference(mid.buffer(-10)), 0, T)
    cuts = [prism(closed_line(h).buffer(RAIL_R + CLR / 2), zc, T + 1) for h in (rails.gnd, rails.vcc)]
    s = (LED["body"] + 0.2) / 2
    for k, (c, t, n) in enumerate(rails.leds):
        sq = shapely.affinity.translate(shapely.affinity.rotate(sbox(-s, -s, s, s), rails.led_angle(k),
                                                                origin=(0, 0), use_radians=True), *c)
        gnd_mark = c + (rails.pad(k, "VSS") - c) * 1.45              # ear on the relief = the LED's notched corner
        cuts += [prism(sq, zc - LED["body_t"], T + 1), prism(Point(*c).buffer(4.0), zc - 0.5, T + 1),
                 prism(Point(*gnd_mark).buffer(1.0), zc - 0.5, T + 1)]
    L = LED["pitch"] + BR["rail_d"] + CLR
    for c, t, n in rails.caps:                                       # 1206 across the rails
        a, b = c - n * L / 2, c + n * L / 2
        cuts.append(prism(LineString([a, b]).buffer(0.95, cap_style="flat"), zc - 0.3, T + 1))
    return (body - union(cuts)).mirror([1, 0, 0])

CLIPS = [(8.0, hc.BRD.BH, 1), (40.0, hc.BRD.BH, 1), (14.0, 0.0, -1), (34.0, 0.0, -1)]   # board x, edge y, outward

def mount(cage, st):
    """Front plate behind the LEDs: cell against its back, board clipped on four posts behind the cell,
    four ears that snap onto ring 1. Returned in the cage frame."""
    zf, zb = st.plate_front, st.plate_back
    w = MNT["post_w"] / 2
    core = st.bat.buffer(BAT["gap"] + 1.6, join_style="mitre")
    parts, pads = [], []
    for xb, yb, d in CLIPS:
        x, y = b2c(xb, yb)
        bar = lambda a, b: sbox(x - w, min(y + d * a, y + d * b), x + w, max(y + d * a, y + d * b))
        pads.append(bar(-2.0, 2.6))
        parts += [prism(bar(0.2, 2.6), st.bk - 1.4, zb + 0.01),            # post outside the board edge
                  prism(bar(-0.8, 0.2), st.bf, zb + 0.01),                 # ledge the copper side rests on
                  prism(bar(-0.7, 0.2), st.bk - 1.4, st.bk - 0.2)]         # hook over the back face
    inside = cage.section(zf).buffer(-1.2)
    assert all(inside.contains(q) for q in pads), "a board clip post does not fit inside ring 1"
    core = unary_union([core.intersection(inside)] + pads)          # trimmed under the notch
    mids = np.r_[0, cage.u[1], 1]; mids = (mids[:-1] + mids[1:]) / 2      # ring 1 points halfway between nodes
    pts = HALF[1].at(mids)
    bx0, by0, bx1, by1 = st.bat.bounds
    ears = []
    for anchor in ((bx1, by1), (bx1, by0)):
        q = pts[np.argmin(np.linalg.norm(pts - anchor, axis=1))]
        d = (q - anchor) / np.linalg.norm(q - anchor)
        ears.append((np.array(anchor) - d * 4, q + d * 3.0, q))
    ears += [(a * [-1, 1], b * [-1, 1], q * [-1, 1]) for a, b, q in ears]
    plate = unary_union([core] + [LineString([a, b]).buffer(3.2) for a, b, _ in ears])
    parts.append(prism(plate, zb, zf))
    g = BAT["gap"]
    fence = st.bat.buffer(g + 1.6, join_style="mitre").difference(st.bat.buffer(g, join_style="mitre"))
    parts.append(prism(fence.intersection(sbox(-200, (by0 + by1) / 2 - 7, 200, (by0 + by1) / 2 + 7)), zb - 3.0, zb + 0.01))
    cuts = []
    ring1 = cage.ring[1]
    for _, _, q in ears:                                             # snap groove: ring 1 sits 0.3 d inside the front face
        near = np.where(np.linalg.norm(ring1[:, :2] - q, axis=1) < 7)[0]
        if (np.diff(near) > 1).any():
            cut = int(np.argmax(np.diff(near) > 1)) + 1; near = np.r_[near[cut:], near[:cut]]
        cuts += [capsule(ring1[i], ring1[j], RR + 0.05) for i, j in zip(near[::2][:-1], near[::2][1:])]
    return union(parts) - union(cuts)

def cap_r(): return PCB["actuator_d"] / 2 + 1.0

def button_cap(st):
    """Optional extender: presses onto S1's actuator and ends 3 mm inside the back ring. Cage frame."""
    c = Point(*st.s1_xy)
    z_front = st.bk - PCB["switch_h"] + 3.0                           # 3 mm socket over the actuator
    z_end = ZR[4] + 3.0
    body = prism(c.buffer(cap_r()), z_end, z_front) + prism(c.buffer(cap_r() + 0.8), z_front - 1.0, z_front)
    return body - prism(c.buffer(PCB["actuator_d"] / 2 + 0.1), z_front - 3.0, z_front + 1)

def cut_gauge(info):
    """One blind slot per strut class: push the rod to the stop, cut flush with the open edge. Dots = class."""
    cls = info["classes"]; pitch = 8.0; w = BR["strut_d"] + CLR
    Lmax = max(c["cut"] for c in cls)
    body = prism(sbox(0, 0, Lmax + 5, pitch * len(cls) + 2), 0, 4.0)
    cuts = []
    for i, c in enumerate(cls):
        y = 1 + pitch * (i + 0.5)
        cuts.append(prism(sbox(-1, y - w / 2, c["cut"], y + w / 2), 4.0 - BR["strut_d"] - 0.4, 5))
        cuts += [prism(Point(2.0 + 2.2 * k, y + 2.4).buffer(0.7, resolution=6), 3.3, 5) for k in range(i + 1)]
    return body - union(cuts)

# ---------------------------------------------------------------- renderer (z-buffer, flat shaded)
def render(objs, path, yaw=0, pitch=0, up="y", size=1100, ss=2):
    """objs: [(V, F, rgb)]. Orthographic. yaw about the vertical, pitch about the horizontal (degrees)."""
    cy, sy, cp, sp = (f(math.radians(a)) for a in (yaw, pitch) for f in (math.cos, math.sin))
    R = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]]) @ np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    if up == "z": R = R @ np.array([[1, 0, 0], [0, 0, 1], [0, -1, 0]])
    cam = [(V @ R.T, F, c) for V, F, c in objs]
    allv = np.concatenate([v for v, _, _ in cam])
    mn, mx = allv.min(0), allv.max(0)
    n = size * ss
    k = (n * 0.9) / max(mx[0] - mn[0], mx[1] - mn[1])
    off = np.array([n / 2 - k * (mn[0] + mx[0]) / 2, n / 2 + k * (mn[1] + mx[1]) / 2])
    zbuf = np.full((n, n), -1e9); img = np.ones((n, n, 3)) * np.array([0.94, 0.94, 0.95])
    light = np.array([-0.35, 0.55, 0.76]); light /= np.linalg.norm(light)
    for V, F, col in cam:
        T = V[F]
        nrm = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])
        ln = np.linalg.norm(nrm, axis=1); keep = (ln > 1e-12) & (nrm[:, 2] > 0)
        T, nrm = T[keep], nrm[keep] / ln[keep, None]
        shade = 0.35 + 0.65 * np.clip(nrm @ light, 0, 1)
        X = T[:, :, 0] * k + off[0]; Y = -T[:, :, 1] * k + off[1]; Z = T[:, :, 2]
        x0 = np.clip(np.floor(X.min(1)).astype(int), 0, n - 1); x1 = np.clip(np.ceil(X.max(1)).astype(int), 0, n - 1)
        y0 = np.clip(np.floor(Y.min(1)).astype(int), 0, n - 1); y1 = np.clip(np.ceil(Y.max(1)).astype(int), 0, n - 1)
        col = np.array(col)
        for i in range(len(T)):
            xs = np.arange(x0[i], x1[i] + 1) + 0.5; ys = (np.arange(y0[i], y1[i] + 1) + 0.5)[:, None]
            ax, ay, bx, by, cx, cy_ = X[i, 0], Y[i, 0], X[i, 1], Y[i, 1], X[i, 2], Y[i, 2]
            area = (bx - ax) * (cy_ - ay) - (by - ay) * (cx - ax)
            if abs(area) < 1e-9: continue
            w0 = ((bx - xs) * (cy_ - ys) - (by - ys) * (cx - xs)) / area
            w1 = ((cx - xs) * (ay - ys) - (cy_ - ys) * (ax - xs)) / area
            w2 = 1 - w0 - w1
            z = w0 * Z[i, 0] + w1 * Z[i, 1] + w2 * Z[i, 2]
            zb = zbuf[y0[i]:y1[i] + 1, x0[i]:x1[i] + 1]
            hit = (w0 >= 0) & (w1 >= 0) & (w2 >= 0) & (z > zb)
            zb[hit] = z[hit]
            img[y0[i]:y1[i] + 1, x0[i]:x1[i] + 1][hit] = col * shade[i]
    zz = np.where(zbuf < -1e8, mn[2] - 5, zbuf)                       # depth-step outlines
    edge = np.zeros((n, n), bool)
    for ax in (0, 1):
        d = np.abs(np.diff(zz, axis=ax)) > 1.2
        if ax == 0: edge[1:] |= d; edge[:-1] |= d
        else: edge[:, 1:] |= d; edge[:, :-1] |= d
    img[edge] *= 0.45
    img = img.reshape(size, ss, size, ss, 3).mean((1, 3))
    Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(path)

def obj(man, col): return (*vf(man), col)

# ---------------------------------------------------------------- run
hc_cage = None

def extent(man):
    V, _ = vf(man)
    return tuple(float(v) for v in np.ptp(V, axis=0))

def flip(man, z):
    """Cage frame -> print frame with the face at height z on the bed (a half turn about x, not a mirror)."""
    return man.rotate([180.0, 0.0, 0.0]).translate([0, 0, z])

def make_parts(cage, st, rails, info):
    """Every printed part in its print orientation: name -> (solid, orientation, material)."""
    global hc_cage
    hc_cage = cage
    z_front = st.bk - PCB["switch_h"] + 3.0
    return {
        "solder_fixture": (fixture(cage, rails), "base down", "PETG"),
        "fixture_stop_ring": (stop_ring(), "flat", "PETG"),
        "led_rail_jig": (rail_jig(rails), "flat, pockets up", "PETG"),
        "pcb_battery_mount": (flip(mount(cage, st), st.plate_front), "plate (front face) down, posts up", "PETG"),
        "button_extender": (flip(button_cap(st), z_front), "socket end down, upright", "PETG"),
        "ring_former": (former(), "flat, steps up", "PLA"),
        "cut_gauge": (cut_gauge(info), "flat, slots up", "PLA"),
    }

def write_3mf(path, meshes):
    """Plain 3MF (core spec): one object per part, already positioned on the plate. meshes: [(name, V, F)]."""
    import zipfile
    obj, items = [], []
    for i, (name, V, F) in enumerate(meshes, 1):
        vs = "".join('<vertex x="%.4f" y="%.4f" z="%.4f"/>' % tuple(v) for v in V)
        ts = "".join('<triangle v1="%d" v2="%d" v3="%d"/>' % tuple(t) for t in F)
        obj.append('<object id="%d" name="%s" type="model"><mesh><vertices>%s</vertices><triangles>%s</triangles></mesh></object>'
                   % (i, name, vs, ts))
        items.append('<item objectid="%d"/>' % i)
    model = ('<?xml version="1.0" encoding="UTF-8"?>' + chr(10) +
             '<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">'
             '<resources>%s</resources><build>%s</build></model>' % ("".join(obj), "".join(items)))
    types = ('<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
             '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
             '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>')
    rels = ('<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
            '<Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>'
            '</Relationships>')
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", types); z.writestr("_rels/.rels", rels); z.writestr("3D/3dmodel.model", model)

def plates(parts, bed, gap=6.0, margin=5.0):
    """Shelf-pack each material's parts onto bed-sized plates and write one 3MF per plate."""
    written = []
    for mat in sorted({m for _, _, m in parts.values()}):
        todo = sorted((n for n, v in parts.items() if v[2] == mat), key=lambda n: -extent(parts[n][0])[1])
        k = 0
        while todo:
            k += 1; meshes = []; x = y = margin; row = 0.0; left = []
            for n in todo:
                V, F = vf(parts[n][0]); w, h, _ = np.ptp(V, axis=0)
                if x + w > bed - margin: x = margin; y += row + gap; row = 0.0
                if y + h > bed - margin or x + w > bed - margin: left.append(n); continue
                meshes.append((n, V - [V[:, 0].min() - x, V[:, 1].min() - y, V[:, 2].min()], F))
                x += w + gap; row = max(row, h)
            assert len(left) < len(todo), "a part is larger than the bed"
            for _, V, _ in meshes: assert V[:, :2].min() >= 0 and V[:, :2].max() <= bed
            name = f"plate_{mat}_{k}.3mf"
            write_3mf(OUT / name, meshes)
            written.append((name, mat, [n for n in todo if n not in left]))
            todo = left
    return written

def run(cage, st, rails, info, parts):
    OUT.mkdir(exist_ok=True)
    for f in list(OUT.glob("*.png")) + list(OUT.glob("*.stl")) + list(OUT.glob("*.3mf")): f.unlink()
    # brass
    cage_vf = rods(cage.segments())
    rail_segs = []
    for h in (rails.gnd, rails.vcc):
        q = np.c_[h.closed(), np.full(len(h.closed()), rails.z)][::3]
        rail_segs += [(a, b, RAIL_R) for a, b in zip(q, np.roll(q, -1, axis=0))]
    rail_segs += [(a, b, RAIL_R) for a, b in rails.spokes]
    rails_vf = rods(rail_segs, 8)
    s = LED["body"] / 2
    leds = union([prism(shapely.affinity.translate(shapely.affinity.rotate(sbox(-s, -s, s, s), rails.led_angle(k), origin=(0, 0),
                  use_radians=True), *rails.leds[k][0]), rails.z + RAIL_R, rails.z + RAIL_R + LED["body_t"]) for k in range(9)])
    caps = union([prism(LineString([c - n * 1.6, c + n * 1.6]).buffer(0.8, cap_style="flat"), rails.z - 0.4, rails.z + 0.6)
                  for c, t, n in rails.caps])
    for name, (man, orient, mat) in parts.items():
        V, F = vf(man)
        tm = trimesh.Trimesh(V, F, process=False)
        assert tm.is_watertight and man.volume() > 0, name
        tm.export(OUT / f"{name}.stl")
        info.setdefault("parts", {})[name] = dict(size=[round(e, 1) for e in extent(man)], orientation=orient, material=mat,
                                                  volume_cm3=round(man.volume() / 1000, 1))
    info["plates"] = [dict(file=f, material=m, parts=p) for f, m, p in plates(parts, hc.P["printer"]["bed"])]
    trimesh.Trimesh(*cage_vf, process=False).export(OUT / "cage_reference.stl")
    # the inside, for the assembly renders
    inside = []
    for name, poly, z0, z1 in st.items:
        if name == "SMD side": continue
        col = GREEN if name == "board" else BLUE if name == "battery" else DARK
        inside.append(obj(prism(poly, z0 + (0.01 if name != "board" else 0), z1), col))
    inside += [obj(mount(cage, st), PETG), obj(button_cap(st), ORANGE)]
    front = [(*rails_vf, BRASS), obj(leds, WHITE), obj(caps, TAN)]
    scene = [(*cage_vf, BRASS)] + inside + front
    render(scene, OUT / "assembly_front.png")
    render(scene, OUT / "assembly_side.png", yaw=-90)
    render(scene, OUT / "assembly_iso.png", yaw=-32, pitch=22)
    render(scene, OUT / "assembly_back.png", yaw=180)
    render(scene, OUT / "assembly_back_iso.png", yaw=148, pitch=22)
    render(inside, OUT / "inside_back_iso.png", yaw=148, pitch=22)
    for name, (man, _, _) in parts.items():
        render([obj(man, PETG)], OUT / f"{name}.png", yaw=-30, pitch=38, up="z", size=900)
    lift = np.array([0, 0, JIG["base_t"] + RR])
    half = [(a + lift, b + lift, r) for a, b, r in cage.segments() if min(a[2], b[2]) > -1e-6]
    render([obj(parts["solder_fixture"][0], PETG), (*rods(half), BRASS)], OUT / "solder_fixture_loaded.png",
           yaw=-30, pitch=38, up="z", size=900)
    zc = 2.0 + LED["body_t"] + RAIL_R
    jr = [(a * [-1, 1, 0] + [0, 0, zc], b * [-1, 1, 0] + [0, 0, zc], r) for a, b, r in rail_segs[:-len(rails.spokes)]]
    render([obj(parts["led_rail_jig"][0], PETG), (*rods(jr, 8), BRASS)], OUT / "led_rail_jig_loaded.png",
           yaw=-30, pitch=50, up="z", size=900)
    # numbers
    info["rails"] = dict(gnd_mm=round(rails.gnd.len * 2, 1), vcc_mm=round(rails.vcc.len * 2, 1), z=rails.z,
                         spokes_mm=[round(float(np.linalg.norm(a - b)), 1) for a, b in rails.spokes],
                         led_xy=[[round(float(v), 1) for v in l[0]] for l in rails.leds])
    (OUT / "cage.json").write_text(json.dumps(info, indent=1))
    d = BR["ring_d"]
    lines = ["Heart Beacon cage cut list (generated by heartcage.py; lengths in mm)", "",
             f"Overall {info['size']['W']} x {info['size']['H']} x {info['size']['D']} over the wire",
             f"{info['nodes']} nodes, {info['struts']} struts + 5 rings, {info['triangles']} triangles",
             f"All brass is {d} mm rod", "",
             "Rings (add ~10 mm each for the tip joint):"]
    lines += [f"  ring {r['ring']}  z {r['z']:+6.1f}  {r['w']} x {r['h']}  developed {r['developed']}" for r in info["rings"]]
    lines += ["", "Struts (cut = node-to-node minus the rod diameter):"]
    lines += [f"  class {c['cls']}  slot {i + 1}  cut {c['cut']:5.1f}  node-to-node {c['node_to_node']:5.1f}  qty {c['qty']}"
              for i, c in enumerate(info["classes"])]
    lines += ["", f"LED rails: GND (outer) {info['rails']['gnd_mm']}, LED+ (inner) {info['rails']['vcc_mm']}, "
              f"{len(rails.spokes)} spokes of about {max(info['rails']['spokes_mm'])} (cut to fit)", "",
              f"Brass total: rings {info['brass']['ring_mm']} + struts {info['brass']['strut_mm']} + rails "
              f"{round(info['rails']['gnd_mm'] + info['rails']['vcc_mm'] + sum(info['rails']['spokes_mm']))} mm", "",
              "Struts per band and class (band = outer ring > inner ring):"]
    for outer, inner in hc.BANDS:
        cnt = {}
        for (a, b), c in zip(cage.struts, cage.cls):
            if a[0] == outer and b[0] == inner: cnt[chr(65 + c)] = cnt.get(chr(65 + c), 0) + 1
        lines.append(f"  ring {outer} > ring {inner}: " + ", ".join(f"{k} x{v}" for k, v in sorted(cnt.items())))
    lines += ["", "Strut by strut (ring.node, node 0 = notch, counting clockwise seen from the front):"]
    for (a, b), c, L in zip(cage.struts, cage.cls, cage.L):
        lines.append(f"  {a[0]}.{a[1]:02d} - {b[0]}.{b[1]:02d}  {chr(65 + c)}  {L:5.1f}")
    (OUT / "cutlist.txt").write_text(chr(10).join(lines) + chr(10))
    print("wrote", OUT)
