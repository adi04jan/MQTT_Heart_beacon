# Heart Beacon brass cage and jigs

A hand-soldered brass wire heart, 91 x 88 x 46 mm, around the rev C3 carrier board and a 45 x 35 x 5 mm cell, plus
the printed jigs that make it repeatable. Everything regenerates from `params.toml` and the board generator in
`../pcb/revc.py` (board outline and part positions are read from there, so the two cannot drift apart).

```
python -m venv .venv && .venv/Scripts/python -m pip install numpy scipy shapely trimesh manifold3d mapbox_earcut pillow
.venv/Scripts/python heartcage.py --check     # asserts only, ~5 s
.venv/Scripts/python heartcage.py             # check, then out/*.stl, *.3mf, *.png, cage.json, cutlist.txt (~30 s)
```

- `params.toml`: every dimension. Lines marked `MEASURE` or `ASSUMED` are not measured.
- `heartcage.py`: geometry and the self-check. `build.py`: solids, STLs, 3MF plates, renders.
- `out/cutlist.txt`: ring lengths, strut classes, and every strut by node. `out/cage.json`: the same as data.

## Design

- One brass gauge for everything: 1.02 mm (19 AWG).
- Five planar rings front to back: ring 2 (centre, the silhouette), rings 1/3 (inset 5 mm, z = +-12), rings 0/4
  (inset 14 mm, z = +-22.5). Front and back halves are the same shape, so one former and one fixture do both.
- The silhouette is the app twin's (`Cage.kt` `shape()` at d = 0), with its 1 : 0.97 : 0.5 proportions.
- 24 nodes per ring. Between neighbouring rings each outer node carries a V of two struts (one straight in, one
  diagonal toward the tip): 189 struts, 188 triangles, four strut lengths.
- What keeps 1.02 mm rings in shape: nothing but the triangles, and that is enough. A bare ring is floppy, so a
  ring never leaves the former or the fixture alone: it is bent on the former, moved straight into its fixture
  groove, and comes off the fixture only as part of a soldered half, where every node already has four or more
  rods meeting at it. The centre ring is the last free edge; it gets its second set of struts at closing.
- One window: the V at the back notch is left out so the USB-C plug reaches the module.
- LED rails sit 2.5 mm behind ring 0: outer rail GND (tied to ring 0 by six spokes, so the whole cage is GND),
  inner rail LED+. Pixel 0 at the tip, chain clockwise seen from the front (up the left lobe), as in `Cage.kt`.
- Inside, front to back: mount plate (snaps onto ring 1 and is the backdrop behind the LEDs), the cell, the board
  with its copper and SMD side toward the cell, then module, LIS3DH, button and slide switch facing the open back.
  The electronics are visible from the back; from the front you see the plate.
- Reach: USB-C through the back notch window; button and slide switch through the open back ring (the slider
  sticks out of the board's right edge as seen from the front, the left as seen from the back).

## Print (Bambu Lab A1 mini, 180 x 180 bed)

Open the plates in Bambu Studio; parts are already arranged and oriented.

| Plate | Parts | Settings |
|---|---|---|
| `plate_PETG_1.3mf` | `solder_fixture`, `led_rail_jig`, `button_extender` | 0.16 mm layers (for the LED pockets), 3 walls, 15 % infill, no supports |
| `plate_PETG_2.3mf` | `fixture_stop_ring`, `pcb_battery_mount` | 0.2 mm, 100 % infill, no supports; white or black filament (it is the LED backdrop) |
| `plate_PLA_1.3mf` | `ring_former`, `cut_gauge` | 0.2 mm, 4 walls, 20 % infill, no supports |

PETG for everything that sees the iron. Channels are rod + 0.25 mm; every cage joint has a 4 mm relief pocket.
The single STLs are in `out/` too. Largest part: the fixture, 110 x 107 mm.

## Build

1. **Cut struts** (about 45 min). Push the rod into a slot of the cut gauge to the stop, cut flush with the open
   edge. Dots next to a slot = class (1 dot = A). Quantities in `out/cutlist.txt`; cut a few spare.
2. **Bend rings** (about 30 min). Start with the middle of the rod in the notch fence of the former, wrap both
   sides down to the tip, cut, solder the tip. Ring 2 on the base step, rings 1 and 3 on the middle step, rings 0
   and 4 on the top step. Mark the nodes from the dimples with a pen. Move each ring straight to the fixture.
3. **LED rails** (about 60 min). Bend both rails on the LED jig's channels. LEDs go lens down, the notched
   corner (GND) toward the ear on the pocket, which is the outer rail. Rails across the pads, 1206 caps in their
   slots, solder. Add the data links (0.4 mm enamelled wire, DOUT to the next DIN, along the middle between the
   rails, over the caps) and three flying leads at pixel 0 (LED+, DIN, GND) for SV1. Test it on the board now.
4. **Front half** (about 2 h). On the fixture: ring 2 on the base, ring 1 on the shoulder, ring 0 on top. Lay
   struts in their grooves (they slide down against the lower ring) and solder both ends. Drop the rail assembly
   on the top floor, LEDs up, and solder the six spokes.
5. **Back half** (about 2 h). Lay the stop ring where ring 2 was. Rings 3 and 4 as before. Ring 3 to ring 4
   struts get both ends soldered; ring 2 to ring 3 struts only their ring 3 end (the free ends rest on the stop
   ring). Leave out the three struts at the notch (USB window).
6. **Inside** (about 20 min). Cell against the plate between the fences (leads out the side), board onto the four
   posts copper side toward the cell until the hooks click. Trim solder joints on the copper side to under 2.5 mm
   first. Snap the plate's four ears onto ring 1 from behind, with the front half lying face down. Plug the LED
   leads into SV1. The button extender is optional: it presses onto S1's actuator.
7. **Close** (about 30 min). Seat the back half on ring 2 and solder the 24 centre nodes, two strut ends each.
   These joints are 12 mm from the nearest plastic and from the cell; work fast with a hot, clean tip.

About 7-8 hours of bench time, plus roughly 9 hours of printing.

## Checks (`--check`)

1. Mirror symmetry of nodes and struts. 2. Triangle count. 3. Valence >= 3. 4. Every strut within its class
+-0.5 mm. 5. >= 2 mm from every rod to the board, cell, module, LIS3DH, switch and header volumes, and those
volumes inside the cage. 6. USB-C plug path, slide switch and button reachable. 7. LED chain order, mirror
symmetry and pads landing on the right rail. 8. Every printed part fits the 180 x 180 x 180 mm printer.
