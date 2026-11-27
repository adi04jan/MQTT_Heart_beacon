# Minimal EAGLE .brd -> KiCad (v8 format, read by 9.0) for DRC + 3D. Layer 46 (Milling) is ignored on purpose.
# Footprints sit at angle 0; pads carry absolute orientation; custom 3D models are built in board orientation.
import sys, math, uuid, json, xml.etree.ElementTree as ET
brd = ET.parse(sys.argv[1]).getroot()
out = sys.argv[2]
modelmap = json.load(open(sys.argv[3], encoding='utf-8'))  # element name -> list of [path, [ox,oy,oz], rotz]
dnp = set(sys.argv[4].split(',')) if len(sys.argv) > 4 else set()
U = lambda: '"%s"' % uuid.uuid4()
f = lambda v: ('%.4f' % v).rstrip('0').rstrip('.') if abs(v) > 1e-9 else '0'

pk = {(l.get('name'), l.get('urn') or None, p.get('name')): p for l in brd.iter('library') for p in l.iter('package')}
def package(e):
    k = (e.get('library'), e.get('library_urn') or None, e.get('package'))
    if k in pk: return pk[k]
    c = [v for (ln, lu, pn), v in pk.items() if ln == e.get('library') and pn == e.get('package')]
    return c[0]

def tf(x, y, rot):
    r = rot or 'R0'; mir = 'M' in r; ang = float(r.lstrip('MSR') or 0)
    if mir: x = -x
    a = math.radians(ang)
    return x * math.cos(a) - y * math.sin(a), x * math.sin(a) + y * math.cos(a), mir, ang

nets = ['']
padnet = {}
for s in brd.iter('signal'):
    nets.append(s.get('name'))
    for c in s.findall('contactref'):
        padnet[(c.get('element'), c.get('pad'))] = s.get('name')
nid = {n: i for i, n in enumerate(nets)}
def netstr(n): return '(net %d "%s")' % (nid[n], n.replace('"', "'")) if n else ''

L = []
L.append('(kicad_pcb (version 20240108) (generator "hb_eagle2kicad") (generator_version "8.0")')
L.append(' (general (thickness 1.6) (legacy_teardrops no))')
L.append(' (paper "A4")')
L.append(' (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (32 "B.Adhes" user "B.Adhesive") (33 "F.Adhes" user "F.Adhesive")'
         ' (34 "B.Paste" user) (35 "F.Paste" user) (36 "B.SilkS" user "B.Silkscreen") (37 "F.SilkS" user "F.Silkscreen")'
         ' (38 "B.Mask" user) (39 "F.Mask" user) (40 "Dwgs.User" user "User.Drawings") (41 "Cmts.User" user "User.Comments")'
         ' (42 "Eco1.User" user "User.Eco1") (43 "Eco2.User" user "User.Eco2") (44 "Edge.Cuts" user) (45 "Margin" user)'
         ' (46 "B.CrtYd" user "B.Courtyard") (47 "F.CrtYd" user "F.Courtyard") (48 "B.Fab" user) (49 "F.Fab" user))')
L.append(' (setup (pad_to_mask_clearance 0) (allow_soldermask_bridges_in_footprints no))')
for i, n in enumerate(nets):
    L.append(' (net %d "%s")' % (i, n.replace('"', "'")))

MIL = 0.0254
def tht_dia(pad):
    dr = float(pad.get('drill'))
    # rev C3 rules: restring 0.25 mm (rlMinPadTop = rlMaxPadTop = 0.25mm), long pads 169 % (psElongationLong = 69)
    return max(dr + 0.5, float(pad.get('diameter') or 0)), dr

for e in brd.iter('element'):
    name = e.get('name')
    p = package(e)
    ex, ey = float(e.get('x')), float(e.get('y'))
    _, _, mir, ang = tf(0, 0, e.get('rot'))
    side = 'B' if mir else 'F'
    has_tht = p.find('pad') is not None
    fp = [' (footprint "EAGLE:%s" (layer "%s.Cu") (uuid %s) (at %s %s)' % (e.get('package'), side, U(), f(ex), f(-ey))]
    for prop, val, lay in (('Reference', name, '%s.Fab' % side), ('Value', e.get('value') or e.get('package'), '%s.Fab' % side)):
        fp.append('  (property "%s" "%s" (at 0 0 0) (layer "%s") (hide yes) (uuid %s) (effects (font (size 0.8 0.8) (thickness 0.12))%s))'
                  % (prop, val.replace('"', "'"), lay, U(), ' (justify mirror)' if mir else ''))
    attr = 'through_hole' if has_tht else 'smd'
    fp.append('  (attr %s%s)' % (attr, ' dnp' if name in dnp else ''))
    for c in p:
        if c.tag == 'wire' and c.get('layer') in ('21', '22'):
            a = tf(float(c.get('x1')), float(c.get('y1')), e.get('rot')); b = tf(float(c.get('x2')), float(c.get('y2')), e.get('rot'))
            lay = ('F.SilkS' if c.get('layer') == '21' else 'B.SilkS') if not mir else ('B.SilkS' if c.get('layer') == '21' else 'F.SilkS')
            fp.append('  (fp_line (start %s %s) (end %s %s) (stroke (width %s) (type solid)) (layer "%s") (uuid %s))'
                      % (f(a[0]), f(-a[1]), f(b[0]), f(-b[1]), f(max(0.1, float(c.get('width')))), lay, U()))
        if c.tag in ('pad', 'smd'):
            x, y, _, _ = tf(float(c.get('x')), float(c.get('y')), e.get('rot'))
            prot = float((c.get('rot') or 'R0').lstrip('MSR') or 0)
            a = (ang + prot) % 360
            net = padnet.get((name, c.get('name')))
            ns = (' ' + netstr(net)) if net else ''
            if c.tag == 'smd':
                lay = '"F.Cu" "F.Paste" "F.Mask"' if not mir else '"B.Cu" "B.Paste" "B.Mask"'
                fp.append('  (pad "%s" smd rect (at %s %s %s) (size %s %s) (layers %s)%s (uuid %s))'
                          % (c.get('name'), f(x), f(-y), f(a), f(float(c.get('dx'))), f(float(c.get('dy'))), lay, ns, U()))
            else:
                d, dr = tht_dia(c)
                shape = c.get('shape') or 'round'
                if shape == 'long':
                    fp.append('  (pad "%s" thru_hole oval (at %s %s %s) (size %s %s) (drill %s) (layers "*.Cu" "*.Mask")%s (uuid %s))'
                              % (c.get('name'), f(x), f(-y), f(a), f(2.2 / 1.3 * d), f(d), f(dr), ns, U()))
                else:
                    kind = 'rect' if shape == 'square' else 'circle'
                    fp.append('  (pad "%s" thru_hole %s (at %s %s %s) (size %s %s) (drill %s) (layers "*.Cu" "*.Mask")%s (uuid %s))'
                              % (c.get('name'), kind, f(x), f(-y), f(a), f(d), f(d), f(dr), ns, U()))
        if c.tag == 'hole':
            x, y, _, _ = tf(float(c.get('x')), float(c.get('y')), e.get('rot'))
            dr = float(c.get('drill'))
            fp.append('  (pad "" np_thru_hole circle (at %s %s) (size %s %s) (drill %s) (layers "*.Cu" "*.Mask") (uuid %s))' % (f(x), f(-y), f(dr), f(dr), f(dr), U()))
    for path, off, rz in modelmap.get(name, []):
        fp.append('  (model "%s" (offset (xyz %s %s %s)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 %s)))' % (path, f(off[0]), f(off[1]), f(off[2]), f(rz)))
    fp.append(' )')
    L += fp

plain = brd.find('drawing/board/plain')
for c in plain:
    lay = {'20': 'Edge.Cuts', '21': 'F.SilkS', '22': 'B.SilkS'}.get(c.get('layer'))
    if c.tag == 'wire' and lay:
        L.append(' (gr_line (start %s %s) (end %s %s) (stroke (width %s) (type solid)) (layer "%s") (uuid %s))'
                 % (f(float(c.get('x1'))), f(-float(c.get('y1'))), f(float(c.get('x2'))), f(-float(c.get('y2'))),
                    f(max(0.05, float(c.get('width')))) if lay != 'Edge.Cuts' else '0.05', lay, U()))
    if c.tag == 'circle' and lay:
        x, y, r = float(c.get('x')), float(c.get('y')), float(c.get('radius'))
        L.append(' (gr_circle (center %s %s) (end %s %s) (stroke (width %s) (type solid)) (fill none) (layer "%s") (uuid %s))'
                 % (f(x), f(-y), f(x + r), f(-y), f(float(c.get('width'))), lay, U()))
    if c.tag == 'text' and lay and c.text:
        L.append(' (gr_text "%s" (at %s %s) (layer "%s") (uuid %s) (effects (font (size %s %s) (thickness 0.15)) (justify left bottom)))'
                 % (c.text, f(float(c.get('x'))), f(-float(c.get('y'))), lay, U(), c.get('size'), c.get('size')))
    if c.tag == 'hole':
        x, y, dr = float(c.get('x')), float(c.get('y')), float(c.get('drill'))
        L.append(' (footprint "EAGLE:HOLE" (layer "F.Cu") (uuid %s) (at %s %s)' % (U(), f(x), f(-y)))
        L.append('  (property "Reference" "H%d" (at 0 0 0) (layer "F.Fab") (hide yes) (uuid %s) (effects (font (size 0.8 0.8) (thickness 0.12))))' % (len(L), U()))
        L.append('  (property "Value" "hole" (at 0 0 0) (layer "F.Fab") (hide yes) (uuid %s) (effects (font (size 0.8 0.8) (thickness 0.12))))' % U())
        L.append('  (attr exclude_from_pos_files exclude_from_bom)')
        L.append('  (pad "" np_thru_hole circle (at 0 0) (size %s %s) (drill %s) (layers "*.Cu" "*.Mask") (uuid %s))' % (f(dr), f(dr), f(dr), U()))
        L.append(' )')

for s in brd.iter('signal'):
    n = s.get('name')
    for w in s.findall('wire'):
        lay = {'1': 'F.Cu', '16': 'B.Cu'}.get(w.get('layer'))
        if not lay: continue
        assert not w.get('curve'), 'curved track'
        L.append(' (segment (start %s %s) (end %s %s) (width %s) (layer "%s") %s (uuid %s))'
                 % (f(float(w.get('x1'))), f(-float(w.get('y1'))), f(float(w.get('x2'))), f(-float(w.get('y2'))), f(float(w.get('width'))), lay, '(net %d)' % nid[n], U()))
    for v in s.findall('via'):
        dr = float(v.get('drill'))
        d = max(float(v.get('diameter') or 0), dr + 2 * max(8 * MIL, min(20 * MIL, 0.25 * dr)) if not v.get('diameter') else dr + 2 * 8 * MIL)
        L.append(' (via (at %s %s) (size %s) (drill %s) (layers "F.Cu" "B.Cu") %s (uuid %s))' % (f(float(v.get('x'))), f(-float(v.get('y'))), f(d), f(dr), '(net %d)' % nid[n], U()))
    for pg in s.findall('polygon'):
        lay = {'1': 'F.Cu', '16': 'B.Cu'}[pg.get('layer')]
        pts = ' '.join('(xy %s %s)' % (f(float(v.get('x'))), f(-float(v.get('y')))) for v in pg.findall('vertex'))
        L.append(' (zone %s (net_name "%s") (layer "%s") (uuid %s) (hatch edge 0.5) (connect_pads (clearance %s)) (min_thickness %s)'
                 ' (filled_areas_thickness no) (fill yes (thermal_gap 0.35) (thermal_bridge_width %s)) (polygon (pts %s)))'
                 % (netstr(n).replace('(net %d "%s")' % (nid[n], n), '(net %d)' % nid[n]), n, lay, U(), pg.get('isolate') or '0.3048', pg.get('width'), pg.get('width'), pts))
L.append(')')
open(out, 'w', encoding='utf-8').write('\n'.join(L) + '\n')
print('wrote', out, 'footprints', sum(1 for x in L if x.startswith(' (footprint')), 'nets', len(nets) - 1)
