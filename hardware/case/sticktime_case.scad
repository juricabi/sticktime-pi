// StickTime Pi case: a Raspberry Pi 4, a 5.8 GHz VTX and a 40 mm fan in one box.
//
// The fan sits on the lid over the Pi's SoC and blows down. The air leaves through the VTX bay along
// the GPIO edge, over and under the VTX, and out of the slots in the bay's walls. The VTX sits on its
// own tray (a small plate on four posts): a 16x16, 20x20, 25.5x25.5 or 30.5x30.5 board is a parameter
// change, and zip-tie slots take boards with odd holes. The fan's and the VTX's wires stay inside: the
// GPIO header is right next to the bay. All case screws are M3 into heat-set inserts (Voron style,
// M3 x D5 x L4); README.md lists the parts and the screws.
//
// Parts (set `part` below, or on the command line: openscad -D 'part="base"' -o base.stl sticktime_case.scad):
//   base      the box with the Pi standoffs and the tray posts; printed as is
//   lid       the top with the fan opening; printed upside down (the STL is already flipped)
//   tray      the VTX plate; printed as is
//   guard     a finger guard for the top of the fan; printed as is
//   assembly  everything in place, with a Pi, a fan and a VTX drawn in
//
// Units are mm. x runs along the Pi's long edge from the SD card end, y across the board from the HDMI
// side, z up from the bottom of the case. The Pi's board corner is at (0, 0); its ports and holes are
// placed from the official mechanical drawing.

part = "assembly";

/* [Raspberry Pi 4] */
pi_x = 85; pi_y = 56; pi_r = 3; pi_pcb = 1.6;
pi_holes = [[3.5, 3.5], [61.5, 3.5], [3.5, 52.5], [61.5, 52.5]];
pi_standoff = 3;        // board underside above the floor
pi_hole_d = 2.2;        // M2.5 screws cut their own thread; 2.7 for a clearance hole

/* [Case] */
wall = 2; floor_t = 2; lid_t = 2.5;
clr = 1.5;              // Pi board edge to the inner walls
inner_h = 28;           // floor top to lid underside: the USB ports end at 22.6, wires on the GPIO header fit
bay_w = 38;             // VTX bay along the GPIO edge, inner width (a 36 mm board fits)
corner_r = 4;
insert_d = 4.4;         // hole for an M3 x D5 x L4 heat-set insert (4.0 if yours want a tighter press)
insert_h = 6;
m3 = 3.4;               // M3 clearance hole
vent_w = 2.5;           // width of the vent slots
gpio_slot = false;      // slot in the lid over the GPIO header, for wires to the outside
sma = true;             // 6.5 mm hole for an SMA bulkhead in the bay's SD-card end wall
ant_notch = true;       // notch at the top of the bay's outer wall, for a whip antenna or a cable

/* [Fan] */
fan = 40; fan_t = 10; fan_pitch = 32; fan_open = 37;
fan_c = [33, 31];       // over the SoC and the RAM

/* [VTX tray] */
vtx_pattern = 16;       // hole spacing: 16, 20, 25.5 or 30.5
vtx_hole_d = 2.2;       // 2.2: M2.5 cuts its own thread; 1.7: M2; 2.7 or 3.4: clearance, with a nut below
vtx_nut_af = 0;         // nut trap in the top of each standoff, across flats (M2 4.0, M2.5 5.0, M3 5.5); 0 = none
vtx_nut_t = 2;
vtx_standoff_h = 4;     // the VTX board sits this high above the tray (0: flat on the tray, held by zip ties)
vtx_standoff_d = 5;
vtx_board = [26, 26];   // the VTX board's outline; the zip-tie slots sit just outside it
tie_slots = true;
tray_size = [48, 36]; tray_t = 2.5;
tray_post_h = 7;        // tray underside above the floor (air passes under it too)
tray_cx = 34;           // tray center along x; the SD-card end of the bay is left free for the antenna
tray_screws = [[-21, -15], [21, -15], [-21, 15], [21, 15]];

$fa = 3; $fs = 0.5;

// ---- derived ----
ix0 = -clr; ix1 = pi_x + 1;             // the USB ports overhang the board; their faces end up in the wall
iy0 = -clr; iy1 = pi_y + clr + bay_w;
ox0 = ix0 - wall; ox1 = ix1 + wall; oy0 = iy0 - wall; oy1 = iy1 + wall;
z_pcb = floor_t + pi_standoff;          // Pi board underside
z_ptop = z_pcb + pi_pcb;                // Pi board top
z_top = floor_t + inner_h;              // lid underside = height of the base
z_tray = floor_t + tray_post_h;
tray_cy = pi_y + clr + bay_w / 2;
tray_c = [tray_cx, tray_cy];
ear_r = 5;
ears = [[ox0 - 0.5, oy0 - 0.5], [ox1 + 0.5, oy0 - 0.5]];   // lid screws outside the HDMI-side corners
posts = [[ix0 + 4.5, iy1 - 4.5], [ix1 - 4.5, iy1 - 4.5]];  // lid screws inside the bay's corners
sma_pos = [64, 18];                     // [y, z] of the SMA hole in the SD-card end wall of the bay
notch_x = 66;                           // antenna notch center along the bay's outer wall
fan_screws = [for (i = [-1, 1], j = [-1, 1]) [fan_c[0] + i * fan_pitch / 2, fan_c[1] + j * fan_pitch / 2]];

module rrect(x0, y0, x1, y1, r) translate([x0 + r, y0 + r]) offset(r = r) square([x1 - x0 - 2 * r, y1 - y0 - 2 * r]);
module outline2d() { rrect(ox0, oy0, ox1, oy1, corner_r); for (e = ears) translate(e) circle(r = ear_r); }
module cavity2d() rrect(ix0, iy0, ix1, iy1, corner_r - wall);

// a lid post in the bay's corner, with the corner between it and the walls filled; g grows it (clearance)
module post2d(p, g = 0) {
    translate(p) circle(r = 4.5 + g);
    left = p[0] < pi_x / 2;
    x0 = left ? ix0 - 1 : p[0] - g; x1 = left ? p[0] + g : ix1 + 1;
    translate([x0, p[1] - g]) square([x1 - x0, iy1 + 1 - (p[1] - g)]);
}

// openings through the walls: y-side walls take x ranges, x-side walls take y ranges
module cut_hdmi_side(x0, x1, z0, z1) translate([x0, oy0 - 1, z0]) cube([x1 - x0, wall + 2, z1 - z0]);
module cut_bay_side(x0, x1, z0, z1) translate([x0, iy1 - 1, z0]) cube([x1 - x0, wall + 2, z1 - z0]);
module cut_sd_end(y0, y1, z0, z1) translate([ox0 - 1, y0, z0]) cube([wall + 2, y1 - y0, z1 - z0]);
module cut_usb_end(y0, y1, z0, z1) translate([ix1 - 1, y0, z0]) cube([wall + 2, y1 - y0, z1 - z0]);

module insert_hole(z_top_of_hole) translate([0, 0, z_top_of_hole - insert_h]) cylinder(d = insert_d, h = insert_h + 1);

module base() {
    difference() {
        union() {
            difference() {
                linear_extrude(z_top) outline2d();
                translate([0, 0, floor_t]) linear_extrude(inner_h + 1) cavity2d();
            }
            for (p = posts) linear_extrude(z_top) post2d(p);
            for (h = pi_holes) translate([h[0], h[1], 0]) cylinder(d = 6, h = z_pcb);
            for (s = tray_screws) translate([tray_c[0] + s[0], tray_c[1] + s[1], 0]) cylinder(d = 7, h = z_tray);
        }
        for (h = pi_holes) translate([h[0], h[1], -1]) cylinder(d = pi_hole_d, h = z_pcb + 2);
        for (s = tray_screws) translate([tray_c[0] + s[0], tray_c[1] + s[1], 0]) insert_hole(z_tray);
        for (p = concat(ears, posts)) translate([p[0], p[1], 0]) insert_hole(z_top);

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
        // the VTX bay: antenna out of the SD-card end wall, vents in its three outer walls
        if (sma) translate([ox0 - 1, sma_pos[0], sma_pos[1]]) rotate([0, 90, 0]) cylinder(d = 6.5, h = wall + 2);
        if (ant_notch) cut_bay_side(notch_x - 5, notch_x + 5, z_top - 8, z_top + 1);
        for (x = [10 : 5 : 75]) cut_bay_side(x, x + vent_w, 8, 24);
        for (y = [71 : 5 : 81]) cut_sd_end(y, y + vent_w, 8, 24);
        for (y = [61 : 5 : 81]) cut_usb_end(y, y + vent_w, 8, 24);
        // floor slots under the SoC and the RAM
        for (y = [22 : 5 : 37]) translate([20, y, -1]) cube([30, vent_w, floor_t + 2]);
    }
}

module lid() {   // in place, on top of the base
    difference() {
        union() {
            translate([0, 0, z_top]) linear_extrude(lid_t) outline2d();
            // a lip that sits inside the walls and locates the lid
            translate([0, 0, z_top - 2.5]) linear_extrude(2.5)
                difference() { offset(r = -0.3) cavity2d(); offset(r = -1.5) cavity2d(); }
            for (f = fan_screws) translate([f[0], f[1], z_top - 5]) cylinder(d = 9, h = 5 + lid_t);
        }
        // the lip keeps clear of the posts, the AV cable's opening and the antenna notch
        for (p = posts) translate([0, 0, z_top - 3]) linear_extrude(3) post2d(p, 0.3);
        translate([47, oy0 - 1, z_top - 3]) cube([14, wall + 3, 3]);
        if (ant_notch) translate([notch_x - 6, iy1 - 2, z_top - 3]) cube([12, wall + 3, 3]);
        // lid screws, countersunk
        for (p = concat(ears, posts)) translate([p[0], p[1], 0]) {
            translate([0, 0, z_top - 3]) cylinder(d = m3, h = lid_t + 4);
            translate([0, 0, z_top + lid_t - 1.6]) cylinder(d1 = m3, d2 = 6.6, h = 1.61);
        }
        // the fan: opening, inserts, and a slot for its wires next to the frame's corner nearest the GPIO header
        translate([fan_c[0], fan_c[1], z_top - 6]) cylinder(d = fan_open, h = lid_t + 7, $fa = 1);
        for (f = fan_screws) translate([f[0], f[1], 0]) insert_hole(z_top + lid_t);
        translate([fan_c[0] + fan / 2 - 7, fan_c[1] + fan / 2 + 0.5, z_top - 1]) cube([7, 3, lid_t + 2]);
        if (gpio_slot) translate([5.5, 49.7, z_top - 1]) cube([54, 5.6, lid_t + 2]);
    }
}

module tray() {   // in place, on its posts
    translate([tray_c[0], tray_c[1], z_tray]) difference() {
        union() {
            linear_extrude(tray_t) offset(r = 3) offset(r = -3) square(tray_size, center = true);
            if (vtx_standoff_h > 0) for (i = [-1, 1], j = [-1, 1])
                translate([i * vtx_pattern / 2, j * vtx_pattern / 2, 0]) cylinder(d = vtx_standoff_d, h = tray_t + vtx_standoff_h);
        }
        for (s = tray_screws) translate([s[0], s[1], -1]) cylinder(d = m3, h = tray_t + 2);
        for (i = [-1, 1], j = [-1, 1]) translate([i * vtx_pattern / 2, j * vtx_pattern / 2, -1]) {
            cylinder(d = vtx_hole_d, h = tray_t + vtx_standoff_h + 2);
            if (vtx_nut_af > 0) translate([0, 0, 1 + tray_t + vtx_standoff_h - vtx_nut_t])
                cylinder(d = vtx_nut_af / cos(30), h = vtx_nut_t + 1, $fn = 6);
        }
        if (tie_slots) for (i = [-1, 1], j = [-1, 1]) {
            translate([i * (vtx_board[0] / 2 + 2.5), j * vtx_board[1] / 4, tray_t / 2]) cube([2, 5, tray_t + 2], center = true);
            translate([i * vtx_board[0] / 4, j * (vtx_board[1] / 2 + 2.5), tray_t / 2]) cube([5, 2, tray_t + 2], center = true);
        }
    }
}

module guard() {   // on the bed; the assembly lifts it onto the fan
    difference() {
        union() {
            linear_extrude(2) difference() { offset(r = 4) offset(r = -4) square(fan, center = true); circle(d = fan - 3); }
            for (r = [6, 12, 18]) linear_extrude(2) difference() { circle(r = r + 0.8); circle(r = r - 0.8); }
            for (a = [0 : 60 : 179]) rotate(a) translate([0, 0, 1]) cube([fan - 2, 1.6, 2], center = true);
            cylinder(d = 8, h = 2);
        }
        for (i = [-1, 1], j = [-1, 1]) translate([i * fan_pitch / 2, j * fan_pitch / 2, -1]) cylinder(d = m3, h = 4);
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

module fan40() color("dimgray") translate([fan_c[0], fan_c[1], z_top + lid_t]) difference() {
    linear_extrude(fan_t) offset(r = 4) offset(r = -4) square(fan, center = true);
    translate([0, 0, -1]) cylinder(d = fan_open, h = fan_t + 2);
    for (f = fan_screws) translate([f[0] - fan_c[0], f[1] - fan_c[1], -1]) cylinder(d = 4.3, h = fan_t + 2);
}

module vtx() color("royalblue") translate([tray_c[0], tray_c[1], z_tray + tray_t + vtx_standoff_h]) {
    linear_extrude(1) difference() {
        square(vtx_board, center = true);
        for (i = [-1, 1], j = [-1, 1]) translate([i * vtx_pattern / 2, j * vtx_pattern / 2]) circle(d = 2.5);
    }
    translate([-3, -3, 1]) cube([6, 6, 3]);
}

if (part == "base") base();
else if (part == "lid") translate([0, 0, z_top + lid_t]) mirror([0, 0, 1]) lid();
else if (part == "tray") translate([-tray_c[0], -tray_c[1], -z_tray]) tray();
else if (part == "guard") guard();
else if (part == "lid_in_place") lid();
else {
    color("lightgray") base();
    color("slategray", 0.85) lid();
    color("orange") tray();
    color("dimgray") translate([fan_c[0], fan_c[1], z_top + lid_t + fan_t]) guard();
    fan40();
    pi4();
    vtx();
}
