// FDDSynthPro rev C: a 3D-printable sled that fits the board in a 3.5-inch drive bay.
// docs/pcb-revc.md explains the layout; every dimension below is a parameter.
//
// Axes: X across the bay (0 at the left, seen from the front), Y front to back (0 at the
// bezel's face), Z up (0 at the bay's floor). The board runs front to back: its left edge
// (the HDD Synth MK5's speaker end) faces the bezel, J1 and the power header face the rear.
//
// Print in PETG as modelled: the bay's floor (z = 0) on the bed, the bezel standing up. No supports:
// the side holes are 4 mm bores, which bridge. fddsynthpro_bay.3mf is this file exported by OpenSCAD
// (openscad -o fddsynthpro_bay.3mf fddsynthpro_bay.scad); export it again after changing a parameter.
// Check the hole positions against your case (or an old floppy drive) before printing:
// floppy drives don't always follow SFF-8301.

/* [Bay (SFF-8301)] */
bay_w = 101.6;          // A3: width
bay_h = 25.4;           // the bezel's height (26.1 is SFF-8301's maximum for a drive)
side_hole_z = 6.35;     // A10: side holes above the bottom
side_hole_y = [28.5, 130.1];  // A8 from the front, then A9 (101.6) further back
bottom_hole_x = 3.18;   // A5: in from each side
bottom_hole_y = [41.28, 85.73];  // A7, and A6 (44.45) further back; check your case
bottom_holes = true;
bottom_block_h = 4.8;   // the blocks round the bottom holes

/* [Fixings] */
insert_d = 4.0;         // hole for an M3 heat-set insert (or 2.6 for a self-tapping M3)
insert_depth = 5.0;
board_screw_d = 2.6;    // M3 into plastic, through the board's M3 holes

/* [Sled] */
bezel_t = 3.0;
pcb_y0 = 15.0;          // the board's front edge, behind the bezel: keeps the microSD card's and
                        // USB-C's openings (on the board's long edges) clear of the front side holes
wall_t = 6.0;           // the side walls (room for the inserts)
wall_h = 12.0;          // around the side holes only; the middle stays open for the card and USB-C
wall_len = 18.0;        // each wall block, centred on its side hole
rail_len = 138.0;       // past the rear side hole, within SFF-8301's 147 mm
tie_w = 6.0;            // the low rails joining the wall blocks
board_z = 3.0;          // the board's underside: clears its through-hole pins
boss_d = 7.0;           // the board's standoffs, on its four M3 holes (nothing else touches its underside)

/* [The board (hardware/revc)] */
pcb_l = 100.33;         // front to back
pcb_w = 69.914;         // across
pcb_t = 1.6;
pcb_holes = [[9.76, 4.124], [86.36, 4.124], [9.76, 65.844], [86.36, 65.844]];  // MK5 M3 holes, board mm
speaker = [0.0, 20.3, 41.0, 59.95];   // board mm: x0, y0, x1, y1 (on top, 20 mm tall)
led = [1.6, 13.0];                    // the activity LED at the board's front edge

/* [Bezel features] */
grille = true;          // slots in front of the speaker
led_hole_d = 3.2;       // a 3 mm light pipe, or just a window onto the LED
sd_slot = true;         // a slot for a microSD extender's card end
sd_slot_x = 8.0;        // from the bezel's left edge
sd_slot_z = 14.0;
encoder = false;        // stretch goal: an EC11 encoder's 7 mm bush
encoder_x = 88.0;
encoder_z = 12.7;
button = false;         // the action button: a 12 mm panel push button
button_x = 72.0;
button_z = 12.7;

$fn = 48;

// Board mm to sled coordinates: the board is centred across the bay, front edge on the bezel.
pcb_x0 = (bay_w - pcb_w) / 2;
function bx(board_y) = pcb_x0 + board_y;   // the board's y runs across the bay
function by(board_x) = bezel_t + pcb_y0 + board_x;  // the board's x runs front to back

module bezel() {
    difference() {
        cube([bay_w, bezel_t, bay_h]);
        if (grille) {
            // Vertical slots in front of the speaker's square, above the board.
            x0 = bx(speaker[1]) + 4;
            x1 = bx(speaker[3]) - 4;
            for (x = [x0 : 4 : x1])
                translate([x, -1, board_z + pcb_t + 3]) cube([2, bezel_t + 2, bay_h - board_z - pcb_t - 7]);
        }
        translate([bx(led[1]), -1, board_z + pcb_t + 0.6]) rotate([-90, 0, 0]) cylinder(d = led_hole_d, h = bezel_t + 2);
        if (sd_slot) translate([sd_slot_x, -1, sd_slot_z]) cube([12.5, bezel_t + 2, 2.2]);
        if (encoder) translate([encoder_x, -1, encoder_z]) rotate([-90, 0, 0]) cylinder(d = 7.2, h = bezel_t + 2);
        if (button) translate([button_x, -1, button_z]) rotate([-90, 0, 0]) cylinder(d = 12.2, h = bezel_t + 2);
    }
}

// One side: a low rail the length of the sled, wall blocks round the bay's side holes, and arms
// out to the board's standoffs. Only the standoffs touch the board's underside, so they keep
// clear of its through-hole pins.
module rail(left) {
    wx = left ? 0 : bay_w - wall_t;
    difference() {
        union() {
            translate([left ? 0 : bay_w - tie_w, 0, 0]) cube([tie_w, rail_len, board_z]);
            for (y = side_hole_y)
                translate([wx, max(0, y - wall_len / 2), 0]) cube([wall_t, wall_len, wall_h]);
            if (bottom_holes)  // low, so they stay under the microSD card's and USB-C plug's path
                for (y = bottom_hole_y)
                    translate([wx, y - 5, 0]) cube([wall_t, 10, bottom_block_h]);
            for (h = pcb_holes)
                if ((left && h[1] < pcb_w / 2) || (!left && h[1] > pcb_w / 2)) {
                    translate([bx(h[1]), by(h[0]), 0]) cylinder(d = boss_d, h = board_z);
                    translate([left ? 0 : bx(h[1]), by(h[0]) - boss_d / 2, 0])
                        cube([left ? bx(h[1]) : bay_w - bx(h[1]), boss_d, board_z]);
                }
        }
        // Side holes, from the outside, for heat-set inserts.
        for (y = side_hole_y)
            translate([left ? -1 : bay_w + 1, y, side_hole_z])
                rotate([0, left ? 90 : -90, 0]) cylinder(d = insert_d, h = insert_depth + 1);
        if (bottom_holes)
            for (y = bottom_hole_y)
                translate([left ? bottom_hole_x : bay_w - bottom_hole_x, y, -1]) cylinder(d = insert_d, h = min(insert_depth, bottom_block_h - 0.8) + 1);
        // The board's screws.
        for (h = pcb_holes)
            if ((left && h[1] < pcb_w / 2) || (!left && h[1] > pcb_w / 2))
                translate([bx(h[1]), by(h[0]), -1]) cylinder(d = board_screw_d, h = board_z + 2);
    }
}

// A low cross-bar behind the board, under the cables, joining the rails.
module rear_bar() {
    translate([0, by(pcb_l) + 2, 0]) cube([bay_w, 6, board_z]);
}

module sled() {
    bezel();
    rail(true);
    rail(false);
    rear_bar();
}

// The board and speaker, for checking fit (not printed): set show_board = true in the customizer.
show_board = false;
module board_ghost() {
    color("green", 0.5) translate([pcb_x0, bezel_t, board_z]) cube([pcb_w, pcb_l, pcb_t]);
    color("black", 0.5) translate([bx(speaker[1]), by(speaker[0]), board_z + pcb_t]) cube([40, 40, 20]);
}

sled();
if (show_board) board_ghost();
