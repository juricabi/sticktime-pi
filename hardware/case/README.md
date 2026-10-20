# StickTime Pi case

A 3D-printed box for the whole setup: the Raspberry Pi 4, the 5.8 GHz VTX and a 40 mm fan. The VTX is
the hot part, so it gets the fan head-on; the Pi gets the same air on its way out. The VTX's power, its
SmartAudio wire and the fan's wires stay inside, next to the GPIO header; only the AV plug and the
antenna come out.

![The case: grill over the VTX, four screws, the name on the front, vents on the Pi's walls](images/case.png)

![Lid off and laid beside the case: the Pi, the wires from the GPIO header, the VTX on its frame, the fan on the lid](images/open.png)

![Close-up: the VTX on its open frame, its pads soldered to the jumpers and the AV cable's cores](images/detail.png)

![Exploded: base, Pi, VTX frame, VTX, fan, lid, screws](images/exploded.png)

## How it cools

The fan hangs under the lid's grill, over the VTX bay along the Pi's GPIO edge, and blows down on the
VTX. The VTX sits on an open frame: two thin rings and four spokes, with the standoffs on the inner
ring, so there is nothing under the board but air and both of its sides are in the stream. The bay's
walls are closed, so the air then has to cross the Pi to get out: over the GPIO header, over the SoC,
and out through the slots in the Pi's three walls above the ports. The VTX's watt or two warms that air
by about a degree, so the Pi still gets room-temperature air moving over it.

![The back: the Pi's ports, the vent slots above them, the AV opening up to the rim](images/back.png)

![The front](images/front.png)

## Parts

| Part | Count | Notes |
|---|---|---|
| `stl/base.stl` | 1 | print as is |
| `stl/lid.stl` | 1 | print as is (the STL is already upside down) |
| `stl/frame_<spacing>_<screw>.stl` | 1 | the frame for your VTX: spacing 16, 20, 25.5 or 30.5, screw M2, M2.5 or M3 (12 files) |
| 40x40x10 mm fan, 5 V, 2 wires | 1 | to GPIO pins 4 (5 V) and 6 (GND) |
| Heat-set inserts M3 x D5 x L4 ("Voron" inserts) | 12 | 4 lid, 4 fan, 4 frame; hole 4.4 mm (`insert_d`) |
| M3 x 8, button head or countersunk | 4 | lid; the heads sit flush in the lid |
| M3 x 14 | 4 | fan to the lid |
| M3 x 6 | 4 | frame to its posts |
| M2.5 x 6 | 4 | Pi (they cut their own thread in the standoffs) |
| M2, M2.5 or M3 x 6, to match the frame, or two small zip ties | 4 | VTX to the frame, see below |
| SMA bulkhead pigtail | 1 | only with `sma = true`, which adds a 6.5 mm hole in the bay's SD-card end wall; the VTX's own whip goes out through the notch otherwise |

PETG or PLA, 0.2 mm layers, 3 walls, no supports: nothing overhangs more than 45 degrees (the lid
bosses' undersides, the lid's chamfer). Outside: 92 x 110 x 42 mm.

## Assembly

1. **Inserts.** Press the 12 inserts in with a soldering iron: 4 in the lid's fan pad (from the
   inside), 4 in the base's frame posts, 4 in the tops of the bosses on the base's long walls.
2. **Fan into the lid.** Lid upside down on the bench, fan onto the pad with its label (the side the
   air comes out of) *away* from the lid, 4x M3 x 14.

   ![The lid upside down, the fan going onto its pad](images/step3.png)
3. **Pi into the base**, onto its four standoffs, 4x M2.5 x 6. The SD card goes in and out through the
   slot in the end wall.

   ![The Pi on its standoffs](images/step1.png)
4. **VTX onto the frame** (screws into the standoffs, or zip ties round the bars), **frame onto its
   posts**, 4x M3 x 6. The VTX's whip antenna goes out through the notch at the top of the USB-end wall
   (with `sma = true` the case has a hole for an SMA bulkhead in the other end wall instead).

   ![The frame with the VTX in the bay](images/step2.png)
5. **Wires.** The AV cable's plug goes into the jack from outside and the cable comes back in through
   the same opening (it runs up to the rim). From the header to the VTX: the SmartAudio wire (pin 7
   through 1 kΩ), and 5 V (pin 2) with ground (pin 9) if the VTX takes 5 V from the Pi. The fan goes on
   pins 4 and 6.
6. **Lid on**, 4x M3 x 8 into the bosses. The lip inside the rim locates it.

   ![The lid coming down onto the base](images/step4.png)

The lid has an optional slot over the GPIO header for wires to the outside (`gpio_slot = true`).

## VTX mounting

The frame has four standoffs at the VTX's hole spacing, 5 mm tall, on a ring that leaves the middle of
the board open. The screws cut their own thread in the standoffs. Measure your board's holes and pick
the frame with the screw that fits through them; the standoffs are as thin as the screw allows, so the
parts next to a small board's holes stay clear:

| Screw | Hole in the standoff | Standoff | Boards |
|---|---|---|---|
| M2 | 1.8 mm | 4.4 mm | 2.0 to 2.5 mm holes: most 16x16, 20x20 and 25.5x25.5 VTXes |
| M2.5 | 2.2 mm | 5.0 mm | 2.5 to 3 mm holes |
| M3 | 2.7 mm | 6.0 mm | 3 mm holes: most 30.5x30.5 VTXes, 20x20 boards with their rubber grommets taken out |

A VTX with odd holes, or none, is strapped to the frame's bars with two zip ties. There are 10 mm
between the board and the fan for a heatsink, and 10 mm of air under the board.

![The twelve frames: a row per spacing, a column per screw](images/frames.png)

Everything about the frame is a parameter in `sticktime_case.scad`:

| Parameter | What it is | Default |
|---|---|---|
| `vtx_pattern` | hole spacing | 16 |
| `vtx_hole_d` | hole in the standoffs: 1.8 (M2 self-tapping), 2.2 (M2.5), 2.7 (M3), or a clearance size with a nut | 1.8 |
| `vtx_nut_af` | nut trap in the top of each standoff, across flats (M2 4.0, M2.5 5.0, M3 5.5); 0 = none | 0 |
| `vtx_standoff_h` | the board's height above the frame | 5 |
| `vtx_standoff_d` | the standoffs' diameter (M2.5 wants 5, M3 wants 6) | 4.4 |

To render a frame for your VTX (OpenSCAD 2021.01 or newer; the name uses `host/fonts/Roboto-Bold.ttf`
from this repo, so render from a checkout):

```sh
openscad -D 'part="frame"' -D vtx_pattern=20 -o frame.stl sticktime_case.scad
```

The other parameters (`inner_h`, `bay_w`, `insert_d`, the logo, the openings) are at the top of the file
with a comment each. `part="assembly"` shows everything in place with a Pi, a fan and a VTX drawn in;
`exploded` pulls it apart.

The pictures above are rendered by `render.py` in Blender 4.1 or newer with Cycles: `blender -b -P
render.py`, with `openscad` on the PATH (or in `OPENSCAD`) to export the pieces. After `--`, shot names
render only those (`-- case open`), `--color "#c8102e"` shows another filament, `--acrylic` clear
acrylic, `--out DIR` writes somewhere other than `images/`, `--preview` renders small and fast. The
assembly steps are OpenSCAD previews.
