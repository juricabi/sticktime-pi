"""Renders the case in Blender (4.1 or newer): the pictures in images/.

    blender -b -P render.py

The printed parts come from STL files in build/parts/ (base, lid_place, frame_place), exported with
OpenSCAD; this script does that itself when `openscad` is on the PATH (or in the OPENSCAD environment
variable) and one is missing. The Pi 4, the VTX, the fan, the screws, the antenna and the cables are
built here from their dimensions. All printed parts are one colour, like a single-colour printer makes
them. Units in the helpers are mm; the scene is in metres.
"""
import math
import os
import random
import shutil
import subprocess
import sys

import bpy
from mathutils import Vector

# names after "--" pick the shots to render: blender -b -P render.py -- open front
WANTED = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []

HERE = os.path.dirname(os.path.abspath(__file__))
PARTS = os.path.join(HERE, "build", "parts")
OUT = os.path.join(HERE, "images")
SCAD = os.path.join(HERE, "sticktime_case.scad")
S = 0.001

PIECES = ["base", "lid_place", "frame_place"]
PATTERNS = ["16", "20", "25.5", "30.5"]
SCREWS = ["M2", "M2.5", "M3"]

# the case's numbers, as in sticktime_case.scad
FLOOR, STANDOFF, PCB = 2.4, 3, 1.6
Z_PCB = FLOOR + STANDOFF
Z_PTOP = Z_PCB + PCB
Z_TOP = FLOOR + 37
LID_T, PAD_T, FAN_T = 3, 4, 10
CX = (-1.5 + 86) / 2
BAY = (CX, 56 + 1.5 + 46 / 2)
Z_FRAME = FLOOR + 5
Z_VTX = Z_FRAME + 2.5 + 5
BOSSES = [(CX - 32, 2.0), (CX + 32, 2.0), (CX - 32, 100.0), (CX + 32, 100.0)]
PI_HOLES = [(3.5, 3.5), (61.5, 3.5), (3.5, 52.5), (61.5, 52.5)]
SMA = (-3.9, BAY[1], Z_VTX + 3)
CENTER = Vector((CX * S, 51 * S, 20 * S))

M = {}
GROUPS = {}   # name -> objects, for lifting parts in the open and exploded views


def export_pieces():
    os.makedirs(PARTS, exist_ok=True)
    openscad = os.environ.get("OPENSCAD") or shutil.which("openscad")
    for p in PIECES:
        path = os.path.join(PARTS, p + ".stl")
        if os.path.exists(path):
            continue
        if not openscad:
            raise SystemExit(f"{path} is missing and openscad was not found")
        subprocess.run([openscad, "-o", path, "--export-format", "binstl", "-D", f'part="{p}"', SCAD], check=True)


# ---- helpers (mm in, metres out) ----
def material(name, color, rough=0.5, metal=0.0, emit=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    bsdf = m.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*color, 1)
    bsdf.inputs["Roughness"].default_value = rough
    bsdf.inputs["Metallic"].default_value = metal
    if emit:
        bsdf.inputs["Emission Color"].default_value = (*color, 1)
        bsdf.inputs["Emission Strength"].default_value = emit
    return m


def group(name, obj):
    if name != "cutters":   # cutters are deleted after their boolean
        obj["rest_z"] = obj.location.z
        GROUPS.setdefault(name, []).append(obj)
    return obj


def smooth(obj, angle=30):
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    try:
        bpy.ops.object.shade_smooth_by_angle(angle=math.radians(angle))
    except Exception:
        bpy.ops.object.shade_smooth()
    obj.select_set(False)


def box(name, size, at, mat, grp, bevel=0):
    """A box of size (mm) with its minimum corner at `at` (mm)."""
    bpy.ops.mesh.primitive_cube_add(size=1, location=[(a + s / 2) * S for a, s in zip(at, size)])
    o = bpy.context.active_object
    o.name = name
    o.scale = [s * S for s in size]
    bpy.ops.object.transform_apply(scale=True)
    o.data.materials.append(mat)
    if bevel:
        mod = o.modifiers.new("bevel", "BEVEL")
        mod.width = bevel * S
        mod.segments = 3
        smooth(o, 40)
    return group(grp, o)


def cyl(name, d, h, at, mat, grp, axis="Z", verts=48):
    """A cylinder of diameter d, length h (mm), starting at `at` (mm) and running along `axis`."""
    rot = {"Z": (0, 0, 0), "X": (0, math.pi / 2, 0), "Y": (math.pi / 2, 0, 0)}[axis]
    centre = list(at)
    centre["XYZ".index(axis)] += h / 2
    bpy.ops.mesh.primitive_cylinder_add(vertices=verts, radius=d / 2 * S, depth=h * S,
                                        location=[c * S for c in centre], rotation=rot)
    o = bpy.context.active_object
    o.name = name
    o.data.materials.append(mat)
    smooth(o)
    return group(grp, o)


def wire(name, points, d, mat, grp):
    cu = bpy.data.curves.new(name, "CURVE")
    cu.dimensions = "3D"
    cu.bevel_depth = d / 2 * S
    cu.bevel_resolution = 4
    cu.use_fill_caps = True
    sp = cu.splines.new("BEZIER")
    sp.bezier_points.add(len(points) - 1)
    for bp, p in zip(sp.bezier_points, points):
        bp.co = [c * S for c in p]
        bp.handle_left_type = bp.handle_right_type = "AUTO"
    cu.materials.append(mat)
    o = bpy.data.objects.new(name, cu)
    bpy.context.collection.objects.link(o)
    return group(grp, o)


def screw(name, d, length, at, grp, down=True):
    """A button head screw: head at `at` (mm), shaft going down (or up), with a hex socket."""
    x, y, z = at
    head_d, head_h = d * 1.9, d * 0.55
    shaft = cyl(name + "_shaft", d, length, (x, y, z - length if down else z), M["screw"], grp, verts=24)
    head = cyl(name + "_head", head_d, head_h, (x, y, z if down else z - head_h), M["screw"], grp, verts=48)
    mod = head.modifiers.new("round", "BEVEL")
    mod.width = head_h * 0.45 * S
    mod.segments = 4
    bpy.context.view_layer.objects.active = head
    bpy.ops.object.modifier_apply(modifier=mod.name)
    zs = z + head_h * 0.4 if down else z - head_h
    sock = cyl(name + "_socket", d * 0.75, head_h, (x, y, zs), M["screw"], "cutters", verts=6)
    cut = head.modifiers.new("socket", "BOOLEAN")
    cut.operation = "DIFFERENCE"
    cut.object = sock
    bpy.context.view_layer.objects.active = head
    bpy.ops.object.modifier_apply(modifier=cut.name)
    bpy.data.objects.remove(sock)
    smooth(head, 35)
    return head


def load(name, path, mat, grp):
    bpy.ops.wm.stl_import(filepath=path, global_scale=S)
    o = bpy.context.selected_objects[0]
    o.name = name
    o.data.materials.append(mat)
    smooth(o)
    return group(grp, o)


# ---- the things in the case ----
def build_pi():
    g = "pi"
    bpy.ops.wm.stl_import(filepath=os.path.join(PARTS, "pi_board.stl"), global_scale=S)
    board = bpy.context.selected_objects[0]
    board.name = "pi_board"
    board.data.materials.append(M["pcb"])
    group(g, board)
    zt = Z_PTOP
    # the ports, from the Pi 4 drawing
    box("usb_c", (8.9, 7.4, 3.2), (11.2 - 4.45, -1.3, zt), M["metal"], g, bevel=0.6)
    box("usb_c_in", (7.4, 0.6, 2.0), (11.2 - 3.7, -1.5, zt + 0.6), M["black"], g)
    for i, x in enumerate((26, 39.5)):
        box(f"hdmi{i}", (6.9, 7.5, 2.9), (x - 3.45, -0.8, zt), M["metal"], g, bevel=0.4)
        box(f"hdmi{i}_in", (5.8, 0.6, 1.6), (x - 2.9, -1.0, zt + 0.6), M["black"], g)
    box("av_base", (7, 12.5, 5), (54 - 3.5, 0, zt), M["black"], g)
    cyl("av_jack", 6, 14.5, (54, -2, zt + 3), M["black"], g, axis="Y")
    cyl("av_hole", 3.6, 1, (54, -2.5, zt + 3), M["dark"], g, axis="Y")
    for i, y in enumerate((9, 27)):
        box(f"usb{i}", (17.4, 13.1, 15.6), (70.1, y - 6.55, zt), M["metal"], g, bevel=0.5)
        for j in range(2):
            box(f"usb{i}_in{j}", (0.6, 11.6, 5.0), (87.1, y - 5.8, zt + 1.4 + j * 7.8), M["black"], g)
            box(f"usb{i}_tab{j}", (0.7, 9.6, 1.4), (87.0, y - 4.8, zt + 1.4 + j * 7.8 + 3.1),
                M["usb_blue"] if i == 1 else M["dark"], g)
    box("eth", (21.5, 16, 13.5), (66, 45.75 - 8, zt), M["metal"], g, bevel=0.5)
    box("eth_in", (0.6, 11.8, 9.0), (87.1, 45.75 - 5.9, zt + 1.6), M["black"], g)
    box("eth_led_g", (0.6, 2, 1.2), (87.1, 45.75 - 7.6, zt + 0.8), M["led_green"], g)
    box("eth_led_y", (0.6, 2, 1.2), (87.1, 45.75 + 5.6, zt + 0.8), M["led_amber"], g)
    # the chips
    box("soc", (15, 15, 1.2), (22, 24, zt), M["black"], g)
    box("soc_lid", (12, 12, 0.4), (23.5, 25.5, zt + 1.2), M["metal"], g, bevel=0.2)
    box("ram", (10, 15, 1.0), (39.5, 24, zt), M["black"], g)
    box("wifi", (10, 11, 1.5), (7, 36, zt), M["metal"], g, bevel=0.3)
    box("vl805", (5, 5, 0.9), (57, 20, zt), M["black"], g)
    box("pmic", (4, 4, 0.8), (8.5, 8.5, zt), M["black"], g)
    box("csi", (3.0, 22, 5.5), (44.5, 2, zt), M["conn"], g)
    box("dsi", (3.0, 22, 5.5), (2.5, 21, zt), M["conn"], g)
    box("poe", (5.2, 5.2, 2.5), (56, 44, zt), M["black"], g)
    for i in range(2):
        for j in range(2):
            cyl(f"poe_pin{i}{j}", 0.64, 8, (57.3 + i * 2.54, 45.3 + j * 2.54, zt + 2.5), M["gold"], g, verts=8)
    # the GPIO header, 2 x 20
    box("gpio_base", (50.8, 5.08, 2.5), (7.1, 50, zt), M["black"], g)
    for i in range(20):
        for row, y in enumerate((51.27, 53.81)):
            cyl(f"pin{i}_{row}", 0.64, 6, (8.37 + i * 2.54, y, zt + 2.5), M["gold"], g, verts=8)
    # LEDs, SD card, small parts
    box("led_pwr", (1.6, 0.8, 0.6), (1.0, 7.0, zt), M["led_red"], g)
    box("led_act", (1.6, 0.8, 0.6), (1.0, 10.0, zt), M["led_green"], g)
    box("sd", (15, 11, 1.0), (-2.4, 24, Z_PCB - 2.2), M["black"], g, bevel=0.3)
    for h in PI_HOLES:
        screw(f"pi_screw_{h[0]}_{h[1]}", 2.5, 6, (h[0], h[1], zt), g)
    rnd = random.Random(7)
    for i in range(70):
        x, y = rnd.uniform(5, 80), rnd.uniform(5, 48)
        if 20 < x < 52 and 22 < y < 41:
            continue
        box(f"smd{i}", (rnd.choice((1.0, 1.6, 2.0)), rnd.choice((0.5, 0.8, 1.2)), 0.5), (x, y, zt),
            rnd.choice((M["black"], M["smd"], M["smd"])), g)


def build_vtx():
    g = "vtx"
    x0, y0 = BAY[0] - 13, BAY[1] - 13
    z = Z_VTX
    board = box("vtx_pcb", (26, 26, 1.0), (x0, y0, z), M["vtx_pcb"], g, bevel=0.5)
    for i in (-1, 1):
        for j in (-1, 1):
            screw(f"vtx_screw{i}{j}", 2, 5, (BAY[0] + i * 8, BAY[1] + j * 8, z + 1.0), g)
    box("vtx_can", (12, 9, 2.6), (BAY[0] - 2, BAY[1] + 2, z + 1), M["metal"], g, bevel=0.3)
    box("vtx_chip", (5, 5, 1.0), (BAY[0] - 9, BAY[1] - 6, z + 1), M["black"], g)
    box("vtx_button", (3, 3, 1.8), (BAY[0] + 6, BAY[1] - 10, z + 1), M["black"], g)
    box("vtx_led", (1.2, 1.2, 0.6), (BAY[0] + 3, BAY[1] - 10, z + 1), M["led_blue"], g)
    cyl("vtx_ufl", 3.2, 2.6, (BAY[0] + 11.5, BAY[1] + 7, z + 1), M["gold"], g, verts=24)
    for i, name in enumerate(("v", "g", "a", "s")):
        box(f"vtx_pad_{name}", (1.6, 1.6, 0.1), (BAY[0] - 8 + i * 3.5, y0 + 1.2, z + 1), M["gold"], g)
    rnd = random.Random(3)
    for i in range(18):
        x, y = rnd.uniform(x0 + 3, x0 + 23), rnd.uniform(y0 + 4, y0 + 23)
        box(f"vtx_smd{i}", (1.0, 0.6, 0.5), (x, y, z + 1), M["smd"], g)
    return board


def build_fan():
    g = "lid"
    zb = Z_TOP - PAD_T - FAN_T
    cx, cy = BAY
    frame = box("fan_frame", (40, 40, 10), (cx - 20, cy - 20, zb), M["fan"], g, bevel=2.5)
    bore = cyl("fan_bore", 38, 12, (cx, cy, zb - 1), M["fan"], "cutters", verts=96)
    cut = frame.modifiers.new("bore", "BOOLEAN")
    cut.operation = "DIFFERENCE"
    cut.object = bore
    bpy.context.view_layer.objects.active = frame
    bpy.ops.object.modifier_apply(modifier=cut.name)
    bpy.data.objects.remove(bore)
    for i in (-1, 1):
        for j in (-1, 1):
            hole = cyl("fan_hole", 4.3, 12, (cx + i * 16, cy + j * 16, zb - 1), M["fan"], "cutters", verts=24)
            cut = frame.modifiers.new("hole", "BOOLEAN")
            cut.operation = "DIFFERENCE"
            cut.object = hole
            bpy.context.view_layer.objects.active = frame
            bpy.ops.object.modifier_apply(modifier=cut.name)
            bpy.data.objects.remove(hole)
    smooth(frame, 40)
    cyl("fan_hub", 19, 8, (cx, cy, zb + 1), M["fan"], g)
    cyl("fan_label", 16, 0.2, (cx, cy, zb + 0.9), M["label"], g)
    for k in range(7):
        a = k * 2 * math.pi / 7
        bpy.ops.mesh.primitive_cube_add(size=1, location=((cx + 12.5 * math.cos(a)) * S, (cy + 12.5 * math.sin(a)) * S, (zb + 5) * S))
        b = bpy.context.active_object
        b.name = f"fan_blade{k}"
        b.scale = (12 * S, 7.5 * S, 0.9 * S)
        b.rotation_euler = (math.radians(38), 0, a + math.pi / 2)
        b.data.materials.append(M["fan"])
        group(g, b)
    for k in range(3):
        a = k * 2 * math.pi / 3 + 0.4
        bpy.ops.mesh.primitive_cube_add(size=1, location=((cx + 14 * math.cos(a)) * S, (cy + 14 * math.sin(a)) * S, (zb + 0.8) * S))
        s = bpy.context.active_object
        s.name = f"fan_strut{k}"
        s.scale = (12 * S, 2.2 * S, 1.6 * S)
        s.rotation_euler = (0, 0, a)
        s.data.materials.append(M["fan"])
        group(g, s)


def build_antenna():
    # a whoop-style whip: the VTX's u.fl pigtail runs under the fan, out through the notch at the top
    # of the USB-end wall, and ends in a short heat-shrunk dipole
    g = "cables"
    ux, uy, uz = BAY[0] + 11.5, BAY[1] + 7, Z_VTX + 3.6
    wire("pigtail", [(ux, uy, uz), (ux + 6, uy + 6, uz + 2), (72, 94, 22), (80, 89, 29), (86, BAY[1], 35.5),
                     (96, BAY[1] - 1, 37), (106, BAY[1] - 3, 41)], 1.3, M["black"], g)
    wire("whip", [(105, BAY[1] - 2.8, 40.6), (118, BAY[1] - 5, 45.5), (132, BAY[1] - 7.5, 51)], 2.6, M["black"], g)


def build_cables():
    g = "cables"
    zt = Z_PTOP
    # the AV plug and its cable back in through the opening
    cyl("av_plug_tip", 3.5, 14, (54, -16, zt + 3), M["metal"], g, axis="Y")
    cyl("av_plug", 7.5, 14, (54, -30, zt + 3), M["black"], g, axis="Y")
    wire("av_cable", [(54, -30, zt + 3), (54, -40, zt + 7), (56, -30, 30), (54, -3, 37.5), (52, 12, 36),
                      (46, 45, 28), (40, 62, 20), (BAY[0] - 4, BAY[1] - 11.5, Z_VTX + 1.5)], 3.0, M["black"], g)
    # power: a USB-C lead
    box("usbc_plug", (12, 20, 6.4), (11.2 - 6, -24, zt + 1.6 - 3.2), M["white"], g, bevel=2)
    box("usbc_tip", (8.3, 4, 2.6), (11.2 - 4.15, -4, zt + 1.6 - 1.3), M["metal"], g)
    wire("usbc_cable", [(11.2, -24, zt + 1.6), (11.2, -45, zt + 1), (-5, -90, 2), (-40, -130, 2), (-120, -150, 2)],
         3.8, M["white"], g)
    # Dupont housings on pins 2, 4, 6 (outer row), 7, 9 (inner row), and their wires
    pins = {2: (8.37, 53.81), 4: (10.91, 53.81), 6: (13.45, 53.81), 7: (15.99, 51.27), 9: (18.53, 51.27)}
    for n, (x, y) in pins.items():
        box(f"dupont{n}", (2.5, 2.5, 14), (x - 1.25, y - 1.25, zt + 2.6), M["black"], g, bevel=0.3)
    top = zt + 2.6 + 14
    cx, cy = BAY
    zb = Z_TOP - PAD_T - FAN_T
    wire("fan_red", [(pins[4][0], pins[4][1], top), (14, 60, top + 8), (cx - 19, cy - 19, zb + 2)], 1.0, M["red"], g)
    wire("fan_black", [(pins[6][0], pins[6][1], top), (16.5, 60.5, top + 7), (cx - 18, cy - 19.5, zb + 2)], 1.0, M["black"], g)
    pad = lambda i: (BAY[0] - 8 + i * 3.5, BAY[1] - 13 + 1.2, Z_VTX + 1.1)
    wire("vtx_red", [(pins[2][0], pins[2][1], top), (12, 62, top + 2), (24, 66, 20), pad(0)], 1.0, M["red"], g)
    wire("vtx_black", [(pins[9][0], pins[9][1], top), (20, 60, top + 1), (28, 65, 19), pad(1)], 1.0, M["black"], g)
    wire("vtx_sa", [(pins[7][0], pins[7][1], top), (18, 61, top + 3), (30, 66, 18), pad(3)], 1.0, M["yellow"], g)
    box("sa_resistor", (3.4, 1.6, 1.6), (26, 63, 18), M["resistor"], g, bevel=0.6)


def build_case():
    load("base", os.path.join(PARTS, "base.stl"), M["print"], "static")
    load("lid", os.path.join(PARTS, "lid_place.stl"), M["print"], "lid")
    load("frame", os.path.join(PARTS, "frame_place.stl"), M["print"], "frame")
    for i, (x, y) in enumerate(BOSSES):
        screw(f"lid_screw{i}", 3, 8, (x, y, Z_TOP + LID_T - 1.6), "lid")
    for i, (dx, dy) in enumerate(((-21, -15), (21, -15), (-21, 15), (21, 15))):
        screw(f"frame_screw{i}", 3, 6, (BAY[0] + dx, BAY[1] + dy, Z_FRAME + 2.5), "frame")


def setup():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    prefs = bpy.context.preferences.addons["cycles"].preferences
    scene.cycles.device = "CPU"
    for kind in ("OPTIX", "CUDA", "HIP", "ONEAPI", "METAL"):
        try:
            prefs.compute_device_type = kind
            prefs.get_devices()
            if any(d.type == kind for d in prefs.devices):
                for d in prefs.devices:
                    d.use = d.type in (kind, "CPU")
                scene.cycles.device = "GPU"
                break
        except Exception:
            continue
    scene.cycles.samples = 256
    scene.cycles.use_denoising = True
    scene.render.resolution_x = 2000
    scene.render.resolution_y = 1500
    scene.render.image_settings.file_format = "PNG"
    try:
        scene.view_settings.view_transform = "AgX"
        scene.view_settings.look = "AgX - Medium High Contrast"
    except Exception:
        pass
    scene.view_settings.exposure = 0.0

    world = bpy.data.worlds.new("studio")
    scene.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs[0].default_value = (0.78, 0.80, 0.83, 1)
    bg.inputs[1].default_value = 0.35

    bpy.ops.mesh.primitive_plane_add(size=8, location=(CENTER.x, CENTER.y, 0))
    bpy.context.active_object.name = "floor"
    bpy.context.active_object.data.materials.append(material("floor", (0.50, 0.51, 0.54), rough=0.85))

    def light(name, offset, power, size):
        bpy.ops.object.light_add(type="AREA", location=CENTER + Vector(offset))
        lamp = bpy.context.active_object
        lamp.name = name
        lamp.data.energy = power
        lamp.data.size = size
        aim(lamp, CENTER)
    # a few watts is a lot at 0.6 m: Blender's sun at 1 W/m2 lights a white wall to 0.3
    light("key", (0.30, -0.20, 0.50), 14, 0.7)
    light("fill", (-0.55, 0.25, 0.30), 5, 1.2)
    light("rim", (0.05, 0.60, 0.40), 8, 0.4)

    bpy.ops.object.camera_add(location=(0, 0, 1))
    cam = bpy.context.active_object
    cam.name = "camera"
    cam.data.sensor_width = 36
    scene.camera = cam
    return scene, cam


def aim(obj, target):
    d = Vector(target) - obj.location
    obj.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()


def materials():
    M["print"] = material("print", (0.16, 0.17, 0.19), rough=0.42)
    M["pcb"] = material("pcb", (0.02, 0.16, 0.06), rough=0.45)
    M["vtx_pcb"] = material("vtx_pcb", (0.02, 0.02, 0.03), rough=0.45)
    M["metal"] = material("metal", (0.80, 0.80, 0.82), rough=0.3, metal=1.0)
    M["gold"] = material("gold", (0.85, 0.65, 0.25), rough=0.35, metal=1.0)
    M["screw"] = material("screw", (0.50, 0.51, 0.53), rough=0.4, metal=1.0)
    M["black"] = material("black", (0.02, 0.02, 0.02), rough=0.55)
    M["dark"] = material("dark", (0.08, 0.08, 0.09), rough=0.7)
    M["smd"] = material("smd", (0.55, 0.52, 0.45), rough=0.5)
    M["conn"] = material("conn", (0.85, 0.82, 0.70), rough=0.6)
    M["fan"] = material("fan", (0.05, 0.05, 0.06), rough=0.55)
    M["label"] = material("label", (0.35, 0.35, 0.38), rough=0.6)
    M["usb_blue"] = material("usb_blue", (0.05, 0.25, 0.75), rough=0.5)
    M["white"] = material("white", (0.85, 0.85, 0.85), rough=0.5)
    M["red"] = material("red", (0.65, 0.04, 0.03), rough=0.5)
    M["yellow"] = material("yellow", (0.80, 0.65, 0.05), rough=0.5)
    M["resistor"] = material("resistor", (0.55, 0.65, 0.75), rough=0.5)
    M["led_red"] = material("led_red", (1.0, 0.05, 0.02), emit=6)
    M["led_green"] = material("led_green", (0.1, 1.0, 0.2), emit=6)
    M["led_amber"] = material("led_amber", (1.0, 0.6, 0.05), emit=4)
    M["led_blue"] = material("led_blue", (0.1, 0.3, 1.0), emit=8)


def lift(offsets, hide=()):
    for name, objs in GROUPS.items():
        for o in objs:
            o.location.z = o["rest_z"] + offsets.get(name, 0) * S
            o.hide_render = name in hide


def shot(scene, cam, name, offset, target=None, lens=50):
    if WANTED and name not in WANTED:
        return
    cam.location = CENTER + Vector(offset)
    aim(cam, target if target is not None else CENTER)
    cam.data.lens = lens
    scene.render.filepath = os.path.join(OUT, name + ".png")
    bpy.ops.render.render(write_still=True)
    print("rendered", scene.render.filepath)


def main():
    export_pieces()
    scene, cam = setup()
    materials()
    build_case()
    build_pi()
    build_vtx()
    build_fan()
    build_antenna()
    build_cables()

    shot(scene, cam, "case", (0.19, 0.22, 0.15))
    shot(scene, cam, "front", (0.04, 0.34, 0.10), target=CENTER + Vector((0, 0, 0.005)), lens=60)
    shot(scene, cam, "back", (-0.21, -0.23, 0.16))
    shot(scene, cam, "top", (0.0, 0.001, 0.40), lens=55)
    lift({}, hide=("lid",))   # lid off: the Pi, the VTX on its frame, the wires
    shot(scene, cam, "open", (0.19, 0.21, 0.19), target=CENTER + Vector((0, 0, 0.0)))
    lift({"pi": 12, "vtx": 45, "frame": 30, "lid": 90}, hide=("cables",))
    shot(scene, cam, "exploded", (0.27, 0.31, 0.27), target=CENTER + Vector((0, 0, 0.05)))

    # the twelve frames in a grid: a row per spacing, a column per screw
    for objs in GROUPS.values():
        for o in objs:
            o.hide_render = True
    for r, pattern in enumerate(PATTERNS):
        for c, screw_size in enumerate(SCREWS):
            f = load(f"frame_{pattern}_{screw_size}", os.path.join(HERE, "stl", f"frame_{pattern}_{screw_size}.stl"),
                     M["print"], "grid")
            f.location = (CENTER.x + (c - 1) * 0.062, CENTER.y + (1.5 - r) * 0.050, 0)
    shot(scene, cam, "frames", (0.0, -0.21, 0.30), target=Vector((CENTER.x, CENTER.y + 0.01, 0)), lens=50)


main()
