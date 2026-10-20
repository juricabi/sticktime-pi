// StickTime Pi case: a Raspberry Pi 4, a 5.8 GHz VTX and a 40 mm fan in one box.
//
// The VTX is the hot part, so it gets the fan head-on: the fan hangs under the lid's grill over the VTX
// bay and blows down on the VTX, which sits on an open frame (two thin rings and four spokes, nothing
// under the board but air). The bay's walls are closed, so the air has to cross the Pi to get out:
// over the GPIO header, over the SoC, and out through the slots in the Pi's three walls. The VTX's
// power, its SmartAudio wire and the fan's wires stay inside: the GPIO header is right next to the bay.
//
// The frame fits a 16x16, 20x20, 25.5x25.5 or 30.5x30.5 board (a parameter), with the holes, standoff
// height and nut traps as parameters too, and swaps with four screws. The lid screws down with four M3
// into heat-set inserts (Voron style, M3 x D5 x L4) in bosses that grow out of the long walls; the fan
// and the frame use the same inserts; the Pi screws onto the base's standoffs with M2.5.
//
// Parts (set `part` below, or on the command line: openscad -D 'part="base"' -o base.stl sticktime_case.scad):
//   base      the box with the Pi standoffs, the frame posts and the lid bosses; printed as is
//   lid       the top with the grill and the fan pad; printed upside down (the STL is already flipped)
//   frame     the VTX frame; printed as is
//   assembly  everything in place, with a Pi, a fan and a VTX drawn in
//   exploded, step1, step2, step3, step4: the README's pictures
//
// Everything prints without supports: the raised name on a vertical wall, the bosses' 45 degree
// undersides, the chamfer and the flared foot. Units are mm. x runs along the Pi's long edge from the
// SD card end, y across the board from the HDMI side, z up from the bottom of the case. The Pi's board
// corner is at (0, 0); its ports and holes are placed from the official mechanical drawing.

use <../../host/fonts/Roboto-Bold.ttf>

part = "assembly";
explode = 25;           // spacing of the exploded view

/* [Raspberry Pi 4] */
pi_x = 85; pi_y = 56; pi_r = 3; pi_pcb = 1.6;
pi_holes = [[3.5, 3.5], [61.5, 3.5], [3.5, 52.5], [61.5, 52.5]];
pi_standoff = 3;        // board underside above the floor
pi_hole_d = 2.2;        // M2.5 screws cut their own thread; 2.7 for a clearance hole

/* [Case] */
wall = 2.4; floor_t = 2.4; lid_t = 3;
clr = 1.5;              // Pi board edge to the inner walls
inner_h = 37;           // floor top to lid underside: 10 mm between the VTX board and the fan for a heatsink
bay_w = 46;             // VTX bay along the GPIO edge, inner width: the fan's pad fits over it
corner_r = 5;
flare = 1.5;            // the base's foot widens by this much at the bottom
chamfer = 1.5;          // on the lid's top edge
insert_d = 4.4;         // hole for an M3 x D5 x L4 heat-set insert (4.0 if yours want a tighter press)
insert_h = 6;
m3 = 3.4;               // M3 clearance hole
head_d = 6.2; head_h = 1.6;      // the lid screws' heads sit in these (button head or countersunk M3)
boss_w = 9; boss_d = 7; // the lid bosses: along the wall, and how far they come out of it
vent_w = 2.5;           // width of the vent slots
font = "Roboto:style=Bold";
logo = "StickTime";     // raised on the bay's outer wall; "" for none
logo_size = 12; logo_h = 1;
gpio_slot = false;      // slot in the lid over the GPIO header, for wires to the outside
sma = true;             // 6.5 mm hole for an SMA bulkhead in the bay's SD-card end wall
ant_notch = true;       // notch at the top of the bay's USB-end wall, for a whip antenna or a cable

/* [Fan] */
fan = 40; fan_t = 10; fan_pitch = 32; fan_open = 38;
pad = 42; pad_t = 4;    // the thick square under the lid that the fan screws into

/* [VTX frame] */
vtx_pattern = 16;       // hole spacing: 16, 20, 25.5 or 30.5
vtx_hole_d = 2.2;       // 2.2: M2.5 cuts its own thread; 1.7: M2; 2.7 or 3.4: clearance, with a nut below
vtx_nut_af = 0;         // nut trap in the top of each standoff, across flats (M2 4.0, M2.5 5.0, M3 5.5); 0 = none
vtx_nut_t = 2;
vtx_standoff_h = 5;     // the VTX board sits this high above the frame; what is under the board hangs free
vtx_standoff_d = 5.5;
vtx_board = [26, 26];   // the VTX board's outline, only drawn in the assembly
frame_size = [48, 36]; frame_t = 2.5; frame_bar = 4;
frame_post_h = 5;       // frame underside above the floor
frame_screws = [[-21, -15], [21, -15], [-21, 15], [21, 15]];

$fa = 3; $fs = 0.5;

// ---- derived ----
ix0 = -clr; ix1 = pi_x + 1;             // the USB ports overhang the board; their faces end up in the wall
iy0 = -clr; iy1 = pi_y + clr + bay_w;
ox0 = ix0 - wall; ox1 = ix1 + wall; oy0 = iy0 - wall; oy1 = iy1 + wall;
z_pcb = floor_t + pi_standoff;          // Pi board underside
z_ptop = z_pcb + pi_pcb;                // Pi board top
z_top = floor_t + inner_h;              // lid underside = height of the base
z_frame = floor_t + frame_post_h;
cx = (ix0 + ix1) / 2;                   // the case's middle along x
bay_c = [cx, pi_y + clr + bay_w / 2];   // the fan and the VTX sit here
fan_c = bay_c;
// the lid bosses: [x, y of the screw, z where the boss leaves the wall]. On the HDMI side they start
// above the USB-C opening and above the USB 2 port; in the bay they start just above the floor.
bosses = [[cx - 32, iy0 + boss_d / 2, z_ptop + 6.5], [cx + 32, iy0 + boss_d / 2, z_ptop + 17],
          [cx - 32, iy1 - boss_d / 2, floor_t + 6], [cx + 32, iy1 - boss_d / 2, floor_t + 6]];
sma_z = z_frame + frame_t + vtx_standoff_h + 3;   // the SMA hole, level with the VTX board
notch_y = bay_c[1];
fan_screws = [for (i = [-1, 1], j = [-1, 1]) [fan_c[0] + i * fan_pitch / 2, fan_c[1] + j * fan_pitch / 2]];
logo_z = (floor_t + z_top - 2.5) / 2;
z_vent1 = z_top - 4.5;                  // the vent slots stop under the lid's lip

module rrect(x0, y0, x1, y1, r) translate([x0 + r, y0 + r]) offset(r = r) square([x1 - x0 - 2 * r, y1 - y0 - 2 * r]);
module outline2d() rrect(ox0, oy0, ox1, oy1, corner_r);
module cavity2d() rrect(ix0, iy0, ix1, iy1, corner_r - wall);

// the outline extruded h high, its top edge chamfered in small steps (45 degrees, prints without supports)
module plate(h, steps = 6) {
    linear_extrude(h - chamfer) outline2d();
    for (i = [0 : steps - 1]) translate([0, 0, h - chamfer + i * chamfer / steps])
        linear_extrude(chamfer / steps + 0.01) offset(r = -(i + 1) * chamfer / steps) outline2d();
}

// the base's outer shape: the foot flares out at 45 degrees, then the walls go straight up
module shell(h, steps = 6) {
    for (i = [0 : steps - 1]) translate([0, 0, i * flare / steps])
        linear_extrude(flare / steps + 0.01) offset(r = flare - i * flare / steps) outline2d();
    translate([0, 0, flare]) linear_extrude(h - flare) outline2d();
}

// a lid boss on the inside of a long wall: its underside slopes out of the wall at 45 degrees
module boss(b) {
    s = b[1] < pi_y / 2 ? 1 : -1;       // grows into the cavity: +y from the HDMI wall, -y from the bay wall
    yw = b[1] - s * boss_d / 2;         // the wall's inner face
    hull() {
        translate([b[0] - boss_w / 2, min(yw, yw - s), b[2]]) cube([boss_w, 1, z_top - b[2]]);
        translate([b[0] - boss_w / 2, min(yw, yw + s * boss_d), b[2] + boss_d]) cube([boss_w, boss_d, z_top - b[2] - boss_d]);
    }
}
function near_boss(x) = [for (b = bosses) if (abs(x - b[0]) < boss_w / 2 + vent_w + 0.5) 1] != [];

// openings through the walls: y-side walls take x ranges, x-side walls take y ranges
module cut_hdmi_side(x0, x1, z0, z1) translate([x0, oy0 - flare - 1, z0]) cube([x1 - x0, wall + flare + 2, z1 - z0]);
module cut_sd_end(y0, y1, z0, z1) translate([ox0 - flare - 1, y0, z0]) cube([wall + flare + 2, y1 - y0, z1 - z0]);
module cut_usb_end(y0, y1, z0, z1) translate([ix1 - 1, y0, z0]) cube([wall + flare + 2, y1 - y0, z1 - z0]);

module insert_hole(z_top_of_hole) translate([0, 0, z_top_of_hole - insert_h]) cylinder(d = insert_d, h = insert_h + 1);

// the name, raised on the bay's outer wall, read from outside
module logo3d() translate([cx, oy1 - 0.5, logo_z]) rotate([90, 0, 180]) linear_extrude(logo_h + 0.5)
    text(logo, size = logo_size, font = font, halign = "center", valign = "center", $fn = 24);

// the grill over the fan: ring slots with six spokes
module grill2d() difference() {
    for (r = [[3.6, 6.4], [7.6, 10.4], [11.6, 14.4], [15.6, 18.4]]) difference() { circle(r = r[1]); circle(r = r[0]); }
    for (a = [0 : 60 : 179]) rotate(a) square([fan, 1.6], center = true);
}

module base() {
    difference() {
        union() {
            difference() {
                shell(z_top);
                translate([0, 0, floor_t]) linear_extrude(inner_h + 1) cavity2d();
            }
            for (b = bosses) boss(b);
            for (h = pi_holes) translate([h[0], h[1], 0]) cylinder(d = 6, h = z_pcb);
            for (s = frame_screws) translate([bay_c[0] + s[0], bay_c[1] + s[1], 0]) cylinder(d = 7, h = z_frame);
            if (logo != "") logo3d();
        }
        for (b = bosses) translate([b[0], b[1], 0]) insert_hole(z_top);
        for (h = pi_holes) translate([h[0], h[1], -1]) cylinder(d = pi_hole_d, h = z_pcb + 2);
        for (s = frame_screws) translate([bay_c[0] + s[0], bay_c[1] + s[1], 0]) insert_hole(z_frame);

        // HDMI side: USB-C, the two micro HDMI (one opening: plugs are fat), the AV jack. The AV opening runs
        // up to the rim so the VTX cable can come back inside over the plug.
        cut_hdmi_side(11.2 - 6.75, 11.2 + 6.75, z_ptop - 2.5, z_ptop + 5.5);
        cut_hdmi_side(19.5, 46, z_ptop - 2.5, z_ptop + 5.5);
        cut_hdmi_side(48, 60, z_ptop - 1.6, z_top + 1);
        // USB end: USB 2, USB 3, Ethernet
        cut_usb_end(9 - 7.3, 9 + 7.3, z_pcb, z_ptop + 16.5);
        cut_usb_end(27 - 7.3, 27 + 7.3, z_pcb, z_ptop + 16.5);
        cut_usb_end(45.75 - 8.6, 45.75 + 8.6, z_pcb, z_ptop + 14);
        // SD card end: the card (it sits under the board), and the two LEDs next to the USB-C corner
        cut_sd_end(18, 42, floor_t, z_pcb + 0.6);
        cut_sd_end(5, 12.5, z_ptop, z_ptop + 3);
        // the way out, on the Pi's three walls: above the ports, under the lid's lip
        for (x = [2 : 5 : 82]) if (!near_boss(x + vent_w / 2) && (x + vent_w < 47 || x > 61))
            cut_hdmi_side(x, x + vent_w, z_ptop + 7.5, z_vent1);
        for (y = [2 : 5 : 52]) cut_sd_end(y, y + vent_w, z_ptop + 7.5, z_vent1);
        for (y = [2 : 5 : 52]) cut_usb_end(y, y + vent_w, z_ptop + 18.5, z_vent1);
        // floor slots under the SoC and the RAM
        for (y = [22 : 5 : 37]) translate([20, y, -1]) cube([30, vent_w, floor_t + 2]);
        // the antenna: an SMA bulkhead in the SD-card end wall, or a whip through a notch at the other end
        if (sma) translate([ox0 - flare - 1, bay_c[1], sma_z]) rotate([0, 90, 0]) cylinder(d = 6.5, h = wall + flare + 2);
        if (ant_notch) cut_usb_end(notch_y - 5, notch_y + 5, z_top - 8, z_top + 1);
    }
}

module lid() {   // in place, on top of the base
    difference() {
        union() {
            translate([0, 0, z_top]) plate(lid_t);
            // a lip that sits inside the walls and locates the lid
            translate([0, 0, z_top - 2.5]) linear_extrude(2.5)
                difference() { offset(r = -0.3) cavity2d(); offset(r = -1.5) cavity2d(); }
            // the fan pad: thick enough for the inserts, the fan sits flat against it
            translate([fan_c[0] - pad / 2, fan_c[1] - pad / 2, z_top - pad_t]) linear_extrude(pad_t + 1)
                offset(r = 3) offset(r = -3) square(pad);
        }
        // the lip keeps clear of the bosses, the AV cable's opening and the antenna notch
        for (b = bosses) translate([b[0] - boss_w / 2 - 0.3, b[1] - boss_d / 2 - 0.3, z_top - 3]) cube([boss_w + 0.6, boss_d + 0.6, 3]);
        translate([47, oy0 - 1, z_top - 3]) cube([14, wall + 3, 3]);
        if (ant_notch) translate([ix1 - 2, notch_y - 6, z_top - 3]) cube([wall + 3, 12, 3]);
        // the lid screws, their heads sunk flush
        for (b = bosses) translate([b[0], b[1], 0]) {
            translate([0, 0, z_top - 1]) cylinder(d = m3, h = lid_t + 2);
            translate([0, 0, z_top + lid_t - head_h]) cylinder(d = head_d, h = head_h + 1);
        }
        // inserts for the fan, the grill
        for (f = fan_screws) translate([f[0], f[1], z_top - pad_t - 1]) cylinder(d = insert_d, h = insert_h + 0.5);
        translate([fan_c[0], fan_c[1], z_top - pad_t - 1]) linear_extrude(pad_t + lid_t + 2) grill2d();
        if (gpio_slot) translate([5.5, 49.7, z_top - 1]) cube([54, 5.6, lid_t + 2]);
    }
}

// the VTX frame, in place on its posts: an outer ring on the posts, an inner ring through the VTX's
// holes with the standoffs on it, four spokes between them. Zip ties go round any bar.
module frame() {
    p = vtx_pattern / 2;
    translate([bay_c[0], bay_c[1], z_frame]) difference() {
        union() {
            linear_extrude(frame_t) {
                difference() {
                    offset(r = 3) offset(r = -3) square(frame_size, center = true);
                    square([frame_size[0] - 2 * frame_bar, frame_size[1] - 2 * frame_bar], center = true);
                }
                difference() { square(vtx_pattern + frame_bar, center = true); square(vtx_pattern - frame_bar, center = true); }
                for (i = [-1, 1], j = [-1, 1]) hull() {
                    translate([i * p, j * p]) circle(d = frame_bar);
                    translate([i * (frame_size[0] / 2 - frame_bar / 2), j * (frame_size[1] / 2 - frame_bar / 2)]) circle(d = frame_bar);
                }
            }
            for (i = [-1, 1], j = [-1, 1]) translate([i * p, j * p, 0]) cylinder(d = vtx_standoff_d, h = frame_t + vtx_standoff_h);
        }
        for (s = frame_screws) translate([s[0], s[1], -1]) cylinder(d = m3, h = frame_t + 2);
        for (i = [-1, 1], j = [-1, 1]) translate([i * p, j * p, -1]) {
            cylinder(d = vtx_hole_d, h = frame_t + vtx_standoff_h + 2);
            if (vtx_nut_af > 0) translate([0, 0, 1 + frame_t + vtx_standoff_h - vtx_nut_t])
                cylinder(d = vtx_nut_af / cos(30), h = vtx_nut_t + 1, $fn = 6);
        }
    }
}

// ---- things that are not printed, drawn in the assembly ----
module pi4() {
    color("darkgreen") translate([0, 0, z_pcb]) linear_extrude(pi_pcb) difference() {
        rrect(0, 0, pi_x, pi_y, pi_r);
        for (h = pi_holes) translate(h) circle(d = 2.7);
    }
    color("silver") translate([0, 0, z_ptop]) {
        translate([11.2 - 4.5, -1.3, 0]) cube([9, 8.8, 3.2]);                      // USB-C
        for (x = [26, 39.5]) translate([x - 3.5, -0.8, 0]) cube([7, 7.5, 3]);      // micro HDMI
        translate([54, 12, 3]) rotate([90, 0, 0]) cylinder(d = 6, h = 14);         // AV jack
        for (y = [9, 27]) translate([72, y - 6.6, 0]) cube([15.5, 13.2, 16]);      // USB 2, USB 3
        translate([66, 45.75 - 8, 0]) cube([21.5, 16, 13.5]);                      // Ethernet
        translate([22, 24, 0]) cube([15, 15, 2.4]);                                // SoC
    }
    color("black") translate([7.1, 50, z_ptop]) cube([50.8, 5.08, 8.5]);            // GPIO header
    color("black") translate([-2.5, 24, z_pcb - 2.5]) cube([15, 11, 1]);            // SD card
}

module fan40() color("dimgray") translate([fan_c[0], fan_c[1], z_top - pad_t - fan_t]) difference() {
    linear_extrude(fan_t) offset(r = 4) offset(r = -4) square(fan, center = true);
    translate([0, 0, -1]) cylinder(d = fan_open, h = fan_t + 2);
    for (f = fan_screws) translate([f[0] - fan_c[0], f[1] - fan_c[1], -1]) cylinder(d = 4.3, h = fan_t + 2);
}

module vtx() color("royalblue") translate([bay_c[0], bay_c[1], z_frame + frame_t + vtx_standoff_h]) {
    linear_extrude(1) difference() {
        square(vtx_board, center = true);
        for (i = [-1, 1], j = [-1, 1]) translate([i * vtx_pattern / 2, j * vtx_pattern / 2]) circle(d = 2.5);
    }
    translate([-4, -4, 1]) cube([8, 8, 4]);
    translate([-3, -3, -3]) cube([6, 6, 3]);
}

// a button head screw, head up, its tip at z = 0
module screw(d, l) color("silver") { translate([0, 0, -l]) cylinder(d = d, h = l); cylinder(d1 = d + 2.8, d2 = d + 1, h = head_h); }

// the whole thing; ex lifts the inner parts apart, ex_lid the lid and the fan, with its screws above it
module scene(ex = 0, ex_lid = 0, pi = true, fr = true, top = true, alpha = 1, screws = true) {
    color("lightgray") base();
    if (pi) translate([0, 0, ex * 0.5]) pi4();
    if (fr) {
        translate([0, 0, ex]) color("orange") frame();
        translate([0, 0, ex * 1.5]) vtx();
    }
    if (top) {
        translate([0, 0, ex_lid]) color("slategray", alpha) lid();
        translate([0, 0, ex_lid * 0.8]) fan40();
        if (screws) for (b = bosses) translate([b[0], b[1], z_top + lid_t - head_h + ex_lid * 1.3]) screw(3, 8);
    }
}

if (part == "base") base();
else if (part == "lid") translate([0, 0, z_top + lid_t]) mirror([0, 0, 1]) lid();
else if (part == "frame") translate([-bay_c[0], -bay_c[1], -z_frame]) frame();
else if (part == "exploded") scene(explode, explode * 2.2);
else if (part == "step1") scene(pi = true, fr = false, top = false);
else if (part == "step2") scene(pi = true, fr = true, top = false);
else if (part == "step3") {   // the lid on the bench, upside down, the fan going in from above
    translate([0, 0, z_top + lid_t]) mirror([0, 0, 1]) { lid(); translate([0, 0, -explode]) fan40(); }
}
else if (part == "step4") scene(ex_lid = explode * 1.5);
else scene(alpha = 0.85);
