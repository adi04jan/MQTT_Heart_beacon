"""Single-layer completion router for the rev C3 board.

Freerouting does the bulk; this finishes what it leaves open and routes GND, one connection at a time on a
0.1 mm grid, against everything already on the board (hard clearances, no rip-up). What it cannot connect is
returned as an airwire list.
"""
import math
import numpy as np
import shapely
from shapely.geometry import Polygon, Point, LineString
from shapely.ops import unary_union, nearest_points
from scipy import ndimage
from scipy.sparse import csr_matrix
from scipy.sparse.csgraph import dijkstra

G = 0.1              # grid pitch, mm
SLACK = 0.01         # added to every clearance (shapes are tested exactly at cell centres)

def complete(pads, tracks, outline, bw, bh, width, clear, edge, order, jump=None, pour=(), log=print):
    """pads: [dict(ref, pad, net, xy, shape)], tracks: [dict(net, w, x1, y1, x2, y2)] (kept), width: net -> track width.
    Routes the nets in `order`. jump = dict(pitch, pad=(along, across)) lets a walled-in connection hop once over
    the tracks in its way with a 0 ohm jumper. Nets in `pour` are copper pours: pads the pour already joins to the
    piece holding the net's first pad need no track. Returns (new tracks, hops [dict(net, x1, y1, x2, y2)], airwires)."""
    NX, NY = int(round(bw / G)) + 1, int(round(bh / G)) + 1
    XS, YS = np.meshgrid(np.arange(NX) * G, np.arange(NY) * G)
    board = Polygon(outline)

    def cells(shape):
        x0, y0, x1, y1 = shape.bounds
        i0, i1 = max(0, int(x0 / G)), min(NX, int(x1 / G) + 2); j0, j1 = max(0, int(y0 / G)), min(NY, int(y1 / G) + 2)
        m = np.zeros((NY, NX), bool)
        if i1 > i0 and j1 > j0: m[j0:j1, i0:i1] = shapely.contains_xy(shape, XS[j0:j1, i0:i1], YS[j0:j1, i0:i1])
        return m

    J, I = np.mgrid[0:NY, 0:NX]
    src, dst, ln = [], [], []
    for dj, di in ((0, 1), (0, -1), (1, 0), (-1, 0), (1, 1), (1, -1), (-1, 1), (-1, -1)):
        ok = (J + dj >= 0) & (J + dj < NY) & (I + di >= 0) & (I + di < NX)
        src.append(J[ok] * NX + I[ok]); dst.append((J[ok] + dj) * NX + I[ok] + di); ln.append(np.full(ok.sum(), 1.4142 if dj and di else 1.0))
    src, dst, ln = map(np.concatenate, (src, dst, ln))
    g = csr_matrix((ln, (src, dst)), shape=(NX * NY, NX * NY)); g.sort_indices()
    cols = g.indices.astype(np.int64); base = g.data.copy()

    geo = lambda t: LineString([(t["x1"], t["y1"]), (t["x2"], t["y2"])])
    tracks = [dict(t) for t in tracks]
    hop_pads = []                                                 # jumper pads placed so far: obstacles for every other net
    new, air, hops = [], [], []
    for net in order:
        w = width[net]
        mine = [p for p in pads if p["net"] == net]
        if len(mine) < 2: continue
        # pieces of this net's copper that are already joined
        items = [("pad", p, p["shape"]) for p in mine] + [("trk", t, geo(t).buffer(t["w"] / 2)) for t in tracks if t["net"] == net]
        par = list(range(len(items)))
        def find(i):
            while par[i] != i: par[i] = par[par[i]]; i = par[i]
            return i
        for a in range(len(items)):
            for b in range(a + 1, len(items)):
                if items[a][2].distance(items[b][2]) < 1e-6: par[find(a)] = find(b)
        other = [p["shape"] for p in pads if p["net"] != net] + [geo(t).buffer(t["w"] / 2) for t in tracks if t["net"] != net]
        other += [h["shape"] for h in hop_pads if h["net"] != net]
        hard = ~shapely.contains_xy(board.buffer(-(edge + w / 2 + SLACK)), XS, YS) | cells(unary_union(other).buffer(clear + w / 2 + SLACK))
        def piece_cells(root):
            m = np.zeros((NY, NX), bool)
            for i, it in enumerate(items):
                if find(i) != root: continue
                m |= cells(it[2].buffer(-0.08)) if it[0] == "pad" else cells(geo(it[1]).buffer(0.08))
                if it[0] == "pad": m |= cells(Point(*it[1]["xy"]).buffer(0.08))
            return m
        if net in pour:                                               # what the pour reaches from the first pad is already connected
            # stricter than a track: only count pour that is comfortably wide, the rest gets a real track
            wide = ~shapely.contains_xy(board.buffer(-(edge + 0.5)), XS, YS) | cells(unary_union(other).buffer(clear + 0.5))
            lab, _ = ndimage.label(~wide, structure=np.ones((3, 3), bool))
            got = set(np.unique(lab[piece_cells(find(0))])) - {0}
            grew = True
            while grew:
                grew = False
                for r in sorted({find(i) for i in range(len(items)) if items[i][0] == "pad"}):
                    if r == find(0): continue
                    ids = set(np.unique(lab[piece_cells(r)])) - {0}
                    if ids & got: par[r] = find(0); got |= ids; grew = True
        while True:
            roots = sorted({find(i) for i in range(len(items)) if items[i][0] == "pad"})
            if len(roots) < 2: break
            masks = {r: piece_cells(r) for r in roots}
            # grow from the piece with the most pads
            main = max(roots, key=lambda r: sum(1 for i in range(len(items)) if find(i) == r and items[i][0] == "pad"))
            cost = np.where(hard, 1e9, 1.0)
            for m in masks.values(): cost[m] = 1.0
            g.data = base * cost.ravel()[cols]
            d, pred, _ = dijkstra(g, directed=True, indices=np.flatnonzero(masks[main].ravel()), min_only=True, return_predecessors=True)
            best = min((r for r in roots if r != main), key=lambda r: d[masks[r].ravel()].min())
            cs = np.flatnonzero(masks[best].ravel()); t = int(cs[np.argmin(d[cs])])
            jc = None
            if d[t] >= 1e8 and jump:                                  # walled in: allow one hop with a 0 ohm jumper
                JC = int(round(jump["pitch"] / G)); pa_, pc_ = jump["pad"]
                solid = cells(unary_union([p["shape"] for p in pads] + [geo(t_).buffer(t_["w"] / 2) for t_ in tracks if t_["net"] != net]
                                          + [h["shape"] for h in hop_pads]))
                solid |= ~shapely.contains_xy(board.buffer(-edge), XS, YS)
                def pad_ok(ax, ay):                                   # a jumper pad centred here keeps `clear` to all of that
                    kx, ky = int(np.ceil((ax + clear + SLACK) / G)), int(np.ceil((ay + clear + SLACK) / G))
                    return ~ndimage.maximum_filter(solid, size=(2 * ky + 1, 2 * kx + 1))
                padonly = cells(unary_union([p["shape"] for p in pads]))
                es, ed = [], []
                for dj, di in ((0, JC), (0, -JC), (JC, 0), (-JC, 0)):
                    okp = pad_ok(pa_ / 2, pc_ / 2) if di else pad_ok(pc_ / 2, pa_ / 2)
                    body = ~ndimage.maximum_filter(padonly, size=(int(pc_ / G) + 1, int((jump["pitch"] - pa_) / G) + 1) if di
                                                   else (int((jump["pitch"] - pa_) / G) + 1, int(pc_ / G) + 1))
                    ok = (J + dj >= 0) & (J + dj < NY) & (I + di >= 0) & (I + di < NX)
                    jj, ii = J[ok], I[ok]
                    good = okp[jj, ii] & okp[jj + dj, ii + di] & body[jj + dj // 2, ii + di // 2] & ~hard[jj, ii] & ~hard[jj + dj, ii + di]
                    es.append(jj[good] * NX + ii[good]); ed.append((jj[good] + dj) * NX + ii[good] + di)
                es, ed = np.concatenate(es), np.concatenate(ed)
                g2 = (g + csr_matrix((np.full(len(es), 3000.0), (es, ed)), shape=g.shape)).tocsr()
                d, pred, _ = dijkstra(g2, directed=True, indices=np.flatnonzero(masks[main].ravel()), min_only=True, return_predecessors=True)
                best = min((r for r in roots if r != main), key=lambda r: d[masks[r].ravel()].min())
                cs = np.flatnonzero(masks[best].ravel()); t = int(cs[np.argmin(d[cs])])
                jc = JC
            if d[t] >= 1e8 or (jc and d[t] >= 6000):                  # still walled in (or two hops needed): an airwire
                pa = [items[i][1] for i in range(len(items)) if find(i) == main and items[i][0] == "pad"][0]
                pb = [items[i][1] for i in range(len(items)) if find(i) == best and items[i][0] == "pad"][0]
                air.append((net, (pa["ref"], pa["pad"]), (pb["ref"], pb["pad"])))
                par[find(best)] = find(main)
                continue
            path = [t]
            while pred[path[-1]] >= 0: path.append(int(pred[path[-1]]))
            pts = [((c % NX) * G, (c // NX) * G) for c in path[::-1]]
            def anchor(pt, root):                                     # end exactly on a pad centre or on a track centre line
                best_q, best_d = None, 1e9
                for i, it in enumerate(items):
                    if find(i) != root: continue
                    q = it[1]["xy"] if it[0] == "pad" and it[2].contains(Point(*pt)) else None
                    if it[0] == "trk":
                        n = nearest_points(geo(it[1]), Point(*pt))[0]; q = (n.x, n.y)
                    if q is not None and math.dist(q, pt) < best_d: best_q, best_d = tuple(q), math.dist(q, pt)
                return best_q
            a, b = anchor(pts[0], main), anchor(pts[-1], best)
            cells_path = path[::-1]
            cut = [q for q in range(len(cells_path) - 1) if jc and abs(cells_path[q + 1] - cells_path[q]) in (jc, jc * NX)]
            runs = [([a] if a else []) + pts + ([b] if b else [])]
            if cut:
                q = cut[0] + (1 if a else 0)
                full = runs[0]; runs = [full[:q + 1], full[q + 1:]]
                h = dict(net=net, x1=full[q][0], y1=full[q][1], x2=full[q + 1][0], y2=full[q + 1][1])
                hops.append(h)
                horiz = abs(h["y1"] - h["y2"]) < 1e-9
                pw, ph = (jump["pad"][0], jump["pad"][1]) if horiz else (jump["pad"][1], jump["pad"][0])
                for (x, y) in ((h["x1"], h["y1"]), (h["x2"], h["y2"])):
                    hop_pads.append(dict(net=net, shape=Polygon([(x - pw / 2, y - ph / 2), (x + pw / 2, y - ph / 2), (x + pw / 2, y + ph / 2), (x - pw / 2, y + ph / 2)])))
            for run in runs:
                for sg in _segs(run, net, w):
                    tracks.append(sg); new.append(sg)
                    items.append(("trk", sg, geo(sg).buffer(w / 2))); par.append(len(par))
                    par[find(len(items) - 1)] = find(main)
            par[find(best)] = find(main)
        log(f"  {net}: done")
    return new, hops, air

def _segs(run, net, w):
    if len(run) < 2: return []
    out = [run[0]]
    for a, b in zip(run[1:], run[2:]):
        p = out[-1]
        if abs((a[0] - p[0]) * (b[1] - a[1]) - (a[1] - p[1]) * (b[0] - a[0])) > 1e-9: out.append(a)
    out.append(run[-1])
    return [dict(kind="seg", net=net, layer=0, w=w, x1=a[0], y1=a[1], x2=b[0], y2=b[1]) for a, b in zip(out, out[1:]) if a != b]
