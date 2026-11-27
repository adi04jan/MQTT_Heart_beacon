# Run with KiCad's python. Specctra bridge for Freerouting, and zone fill:
#   kicad_fr.py dsn board.kicad_pcb board.dsn
#   kicad_fr.py ses board.kicad_pcb board.ses tracks.json    (imports the session, dumps tracks in mm, y up)
#   kicad_fr.py fill board.kicad_pcb                         (fills the zones in place; kicad-cli drc does not refill)
import sys, json, wx
app = wx.App(False)
import pcbnew
mode, pcb = sys.argv[1], sys.argv[2]
b = pcbnew.LoadBoard(pcb)
if mode == "dsn":
    print("dsn", pcbnew.ExportSpecctraDSN(b, sys.argv[3]))
elif mode == "fill":
    ok = pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    for z in b.Zones(): z.CalculateFilledArea()
    print("fill", ok, round(sum(z.GetFilledArea() for z in b.Zones()) / 1e12, 1), "mm2")
    pcbnew.SaveBoard(pcb, b)
else:
    ok = pcbnew.ImportSpecctraSES(b, sys.argv[3])
    out = []
    for t in b.GetTracks():
        if t.Type() == pcbnew.PCB_VIA_T: continue
        a, e = t.GetStart(), t.GetEnd()
        out.append(dict(kind="seg", net=t.GetNetname(), layer=0 if t.GetLayer() == pcbnew.F_Cu else 1, w=t.GetWidth() / 1e6,
                        x1=a.x / 1e6, y1=-a.y / 1e6, x2=e.x / 1e6, y2=-e.y / 1e6))
    json.dump(out, open(sys.argv[4], "w"))
    print("ses", ok, len(out))
