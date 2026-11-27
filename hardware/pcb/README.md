# Heart Beacon carrier rev C3

One single-sided board, 48 x 37 mm (rev B: 38.1 x 64.4), made to hide behind the 45 x 35 mm cell so the heart can
be 91 mm wide. Rev B's parts and nets with a new GPIO map and narrow socket pads. No jumpers.

```
../cage/.venv/Scripts/python revc.py place     # placement preview -> out/revC3_place.png
../cage/.venv/Scripts/python revc.py --own     # place, route, write the EAGLE files, convert to KiCad, fill, DRC (~10 s)
```

Outputs, next to the rev B files in the EAGLE project folder (nothing else there is touched):
`ESP32_heart_revC3.sch`, `ESP32_heart_revC3_build.scr`, `ESP32_heart_revC3_rules.dru`.
Here: `out/revC3_copper_side.png`, `out/revC3_from_the_back.png`, `out/kicad/ESP32_heart_revC3.kicad_pcb`, `out/kicad/drc.json`.

## In EAGLE

1. Open `ESP32_heart_revC3.sch` (there must be no `ESP32_heart_revC3.brd` yet).
2. File > Execute Script > `ESP32_heart_revC3_build.scr`. It runs `BOARD` (answer yes to "create from schematic"),
   deletes EAGLE's default outline, draws the outline (layers 20, 46, 22), moves and rotates every part, draws every
   track on layer 1, the VSYS wire pad, the GND polygon, and ends with `RATSNEST`.
3. In the board editor: DRC > Load `ESP32_heart_revC3_rules.dru`, then Check.
4. `RATSNEST` should report nothing to do. If it shows a GND airwire, the pour came out narrower in EAGLE than in
   KiCad at that spot: draw that one GND track by hand.

The schematic and the script were written by `revc.py` and checked against each other (same nets, same pads) and in
KiCad. They have not been opened in EAGLE.

## GPIO map

| Signal | Net | rev B | rev C3 |
|---|---|---|---|
| Button | A1 | GPIO0 | GPIO0 |
| LIS3DH INT1 | IMU_INT | GPIO1 | GPIO1 |
| LIS3DH SCL | SCL | GPIO9 | **GPIO2** |
| LIS3DH SDA | SDA | GPIO8 | GPIO8 |
| USB sense | USB_CHARGE | GPIO3 | GPIO3 |
| VBAT | VBAT_SENSE | GPIO4 | GPIO4 |
| LED rail enable | LED_EN (rev B: "7") | GPIO7 | **GPIO20** |
| LED data | LED_CTRL | GPIO10 | **GPIO21** |
| tied to GND | GND | GPIO5 | GPIO5 |

Free: GPIO6, 7, 9, 10. GPIO21 is the boot ROM's UART TX: it chatters at reset, but the LED rail is off then
(R8 holds the enable low), so the LEDs do not see it.

## Board

- Copper on EAGLE's Top, which faces the cell. SMD parts on that side. Every through-hole part (module sockets,
  LIS3DH sockets, S1, S3, headers) sits on the other side and is soldered on the copper side.
- Socket and header pads: oblong 1.3 mm along the row x 2.2 mm across, 0.8 mm drill (packages
  `MODULE_ESP32-C3_SUPERMINI_TH`, `MA06-1`, `MA03-1`, `1X02`, edited inside the schematic's libraries; the `.dru` sets
  restring 0.25 mm and long-pad elongation 69 % to match). One 0.4 mm track passes between two pins.
- The LIS3DH's unused pins (J1 5-6, J2 2-3) keep their pads; nothing routes to them.
- `S1` is unmirrored in EAGLE only so that its pin 1 lands top left; it mounts on the back like every other
  through-hole part.
- The VSYS wire pad (2 mm, 1 mm hole, net USB_OUT) is a via element as on rev B. It is at (41.6, 19.65), about
  10 mm from the module's PD1 corner.
- Layer 46 carries the outline cut with four 1.9 mm tabs. No mounting holes: the printed mount clips the board edges.

| Part | Where (copper side view, origin bottom left) |
|---|---|
| U1 SuperMini | top centre, USB-C flush with the top edge on the centre line |
| S1 button | below the module on the centre line |
| S3 slide switch | right edge, slider overhanging by 2.2 mm |
| LIS3DH (SV3, SV5) | left, SV3 next to the module's left pin row |
| USB sense divider | between the LIS3DH pin rows |
| VBAT divider | under the module; R21 between S1's pin rows |
| LED rail (Q5, R8, R7, Q6, R6, R9) | between S1's pin rows |
| SV1 LED header, C7 | bottom left |
| Power path (D2, Q2, C18, R5, JP1) | top right, next to S3 |
| Charger (U3 and passives), BATT header | bottom right |

## Rules and result

- 0.4 mm tracks, 0.35 mm clearance everywhere, 1.0 mm to the board edge, one layer, no jumpers.
- BATT+, USB_IN, VSYS, LED+ and the nets between them are 0.8 mm wherever 0.35 mm still holds, 0.4 mm elsewhere.
- GND: a polygon over the whole board (isolate 0.35, width 0.4, thermals on, orphans off), plus GND tracks where
  the pour alone would be too narrow.
- KiCad DRC at these rules, zone filled: 0 errors, 0 unconnected, 1 warning (the wire pad is a via on one layer).
- Routing: `router.py`, a grid router that places one connection at a time. The order and a handful of reserved
  lanes (`FIRST`, `LAST`, `PRELINKS` in `revc.py`) are what make it close without jumpers; change the placement and
  they may need another look. It can place 1210 jumpers (package `R1210J`, two tracks under one) when a connection
  is walled in; none were needed. Freerouting 2.1.0 can still be run with `revc.py` (without `--own`) but does not
  stop or save reliably on this PC.
