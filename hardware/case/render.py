"""Renders the case in Blender (4.1 or newer, Cycles): the pictures in images/.

    blender -b -P render.py                      # every shot
    blender -b -P render.py -- case open         # only these
    blender -b -P render.py -- --color "#c8102e" # another filament colour (sRGB hex)
    blender -b -P render.py -- --color "#2e3136,#2e3136,#ff8a00"   # base, lid, frame each their own
    blender -b -P render.py -- --preview         # small and quick, for checking
    blender -b -P render.py -- --acrylic --out build/renders/acrylic   # clear acrylic, elsewhere

The printed parts come from STL files in build/parts/ (base, lid_place, frame_place, pi_board),
exported with OpenSCAD; this script does that itself when `openscad` is on the PATH (or in the
OPENSCAD environment variable) and one is missing. The Pi 4, the VTX, the fan, the screws, the
antenna and the cables are built here from their dimensions. Each printed part is one colour, as a
single-colour printer makes it; by default StickTime's: a navy base, an orange lid and frame. The helpers
take mm; the scene is in metres.
"""
import math
import os
import random
import shutil
import subprocess
import sys

import bmesh
import bpy
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
PARTS = os.path.join(HERE, "build", "parts")
OUT = os.path.join(HERE, "images")
SCAD = os.path.join(HERE, "sticktime_case.scad")
FONT_PATH = os.path.join(HERE, "..", "..", "host", "fonts", "Roboto-Bold.ttf")
S = 0.001

ARGS = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
PREVIEW = "--preview" in ARGS
ACRYLIC = "--acrylic" in ARGS
# StickTime's colours (the game's panel navy, its orange accent): the base navy, the lid and the frame orange
COLOR = ARGS[ARGS.index("--color") + 1] if "--color" in ARGS else ("#f4f8fa" if ACRYLIC else "#1b2130,#ff8a00,#ff8a00")
if "--out" in ARGS:
    OUT = os.path.abspath(ARGS[ARGS.index("--out") + 1])
WANTED = [a for a in ARGS if not a.startswith("--") and a != COLOR and os.path.abspath(a) != OUT]

PIECES = ["base", "lid_place", "frame_place", "pi_board"]
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
Z_VTX = Z_FRAME + 2.5 + 5                 # the VTX board's underside
Z_FAN = Z_TOP - PAD_T - FAN_T             # the fan's underside
BOSSES = [(CX - 32, 2.0), (CX + 32, 2.0), (CX - 32, 100.0), (CX + 32, 100.0)]
PI_HOLES = [(3.5, 3.5), (61.5, 3.5), (3.5, 52.5), (61.5, 52.5)]
CENTER = Vector((CX * S, 51 * S, 20 * S))
# the VTX: a 20 x 20 mm board with its 16 x 16 holes 2 mm in from the corners; its pads along the edge
# facing the GPIO header, between the holes (x offsets from the bay's middle)
VTX = 20
PADS = {"5v": -5, "sa": -2.5, "gnd": 0, "vid": 2.5, "gnd2": 5}
PAD_Y = BAY[1] - VTX / 2 + 1.0
PAD_Z = Z_VTX + 1.0
UFL = (BAY[0] + 7.2, BAY[1] + 2.5)
# GPIO header: pin n sits at column (n - 1) // 2; odd pins on the inner row, even on the edge row
def pin(n):
    return (8.37 + (n - 1) // 2 * 2.54, 51.27 if n % 2 else 53.81)
Z_DUPONT = Z_PTOP + 2.6                   # Dupont housings sit on the header's plastic, 14 mm tall
# the lid turned over and laid next to the case (the open shot): x -> 2 XP - x, z -> 2 ZP - z
XP, ZP = -11.45, (Z_TOP + LID_T + 0.02) / 2

M = {}
RIGS = {}
FONT = []
PRINT_LINES = []   # (texture node, k, base period mm) of the print material: Scale = k / period, per shot


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


def srgb(hexstr):
    h = hexstr.lstrip("#")
    c = [int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    return tuple(v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4 for v in c)


# ---- materials ----
def mat(name, color, rough=0.5, metal=0.0, coat=0.0, aniso=0.0, emit=0.0, bevel=0.0, spec=0.5, sheen=0.0,
        thread=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = (*color, 1)
    b.inputs["Roughness"].default_value = rough
    b.inputs["Metallic"].default_value = metal
    b.inputs["Specular IOR Level"].default_value = spec
    if coat:
        b.inputs["Coat Weight"].default_value = coat
        b.inputs["Coat Roughness"].default_value = 0.08
    if aniso:
        b.inputs["Anisotropic"].default_value = aniso
    if sheen:
        b.inputs["Sheen Weight"].default_value = sheen
    if emit:
        b.inputs["Emission Color"].default_value = (*color, 1)
        b.inputs["Emission Strength"].default_value = emit
    normal = None
    if bevel:
        bv = nt.nodes.new("ShaderNodeBevel")
        bv.samples = 8
        bv.inputs["Radius"].default_value = bevel * S
        normal = bv.outputs["Normal"]
    if thread:   # lines across the object's z axis: layer lines, screw threads
        tc = nt.nodes.new("ShaderNodeTexCoord")
        wave = nt.nodes.new("ShaderNodeTexWave")
        wave.wave_type = "BANDS"
        wave.bands_direction = "Z"
        wave.wave_profile = "SIN"
        wave.inputs["Scale"].default_value = (2 * math.pi / 20) / (thread * S)
        nt.links.new(tc.outputs["Object"], wave.inputs["Vector"])
        bump = nt.nodes.new("ShaderNodeBump")
        bump.inputs["Strength"].default_value = 0.3
        bump.inputs["Distance"].default_value = thread * 0.1 * S
        nt.links.new(wave.outputs["Fac"], bump.inputs["Height"])
        if normal:
            nt.links.new(normal, bump.inputs["Normal"])
        normal = bump.outputs["Normal"]
    if normal:
        nt.links.new(normal, b.inputs["Normal"])
    return m


def print_mat(color, layer=0.2, grain=0.35):
    """PLA as it comes off the printer: satin, softly rounded edges, layer lines on the walls, the fine
    grain of a textured bed sheet on the flat faces, and a faint colour change from layer to layer."""
    m = mat("print", color, rough=0.48, bevel=0.3, sheen=0.2, spec=0.4)
    nt = m.node_tree
    b = nt.nodes["Principled BSDF"]
    b.inputs["Sheen Tint"].default_value = (*color, 1)   # a white sheen would wash out a coloured filament
    bevel = [n for n in nt.nodes if n.bl_idname == "ShaderNodeBevel"][0]
    tc = nt.nodes.new("ShaderNodeTexCoord")
    node = nt.nodes.new
    link = nt.links.new

    def math_node(op, *inputs):
        n = node("ShaderNodeMath")
        n.operation = op
        for i, v in enumerate(inputs):
            if isinstance(v, (int, float)):
                n.inputs[i].default_value = v
            else:
                link(v, n.inputs[i])
        return n.outputs[0]

    w = node("ShaderNodeTexWave")
    w.wave_type = "BANDS"
    w.bands_direction = "Z"
    w.wave_profile = "SIN"
    w.inputs["Scale"].default_value = 2 * math.pi / (20 * layer * S)
    PRINT_LINES.append((w, 2 * math.pi / 20, layer))
    link(tc.outputs["Object"], w.inputs["Vector"])
    walls = math_node("POWER", w.outputs["Fac"], 0.6)   # rounder ridges, narrow grooves
    n = node("ShaderNodeTexNoise")
    n.inputs["Scale"].default_value = 1 / (grain * S)
    n.inputs["Detail"].default_value = 3
    PRINT_LINES.append((n, 1, grain))
    link(tc.outputs["Object"], n.inputs["Vector"])
    tops = math_node("MULTIPLY", n.outputs["Fac"], 0.6)
    nrm = node("ShaderNodeSeparateXYZ")
    link(tc.outputs["Normal"], nrm.inputs["Vector"])
    flat = node("ShaderNodeMapRange")
    flat.inputs["From Min"].default_value = 0.85
    flat.inputs["From Max"].default_value = 0.97
    link(math_node("ABSOLUTE", nrm.outputs["Z"]), flat.inputs["Value"])
    height = math_node("MULTIPLY_ADD", math_node("SUBTRACT", tops, walls), flat.outputs["Result"], walls)
    bump = node("ShaderNodeBump")
    bump.inputs["Strength"].default_value = 0.6
    bump.inputs["Distance"].default_value = 0.12 * layer * S
    PRINT_LINES.append((bump, None, layer))
    link(height, bump.inputs["Height"])
    link(bevel.outputs["Normal"], bump.inputs["Normal"])
    link(bump.outputs["Normal"], b.inputs["Normal"])
    # roughness wanders a little, and the colour shifts slightly from layer to layer
    pos = node("ShaderNodeSeparateXYZ")
    link(tc.outputs["Object"], pos.inputs["Vector"])
    zonly = node("ShaderNodeCombineXYZ")
    link(pos.outputs["Z"], zonly.inputs["Z"])
    band = node("ShaderNodeTexNoise")
    band.inputs["Scale"].default_value = 250
    link(zonly.outputs["Vector"], band.inputs["Vector"])
    tint = node("ShaderNodeMapRange")
    tint.inputs["To Min"].default_value = 0.88
    tint.inputs["To Max"].default_value = 1.1
    link(band.outputs["Fac"], tint.inputs["Value"])
    col = node("ShaderNodeVectorMath")
    col.operation = "MULTIPLY"
    col.inputs[0].default_value = color
    link(tint.outputs["Result"], col.inputs[1])
    groove = node("ShaderNodeMapRange")   # the grooves between layers sit a little in shadow
    groove.inputs["To Min"].default_value = 0.8
    groove.inputs["To Max"].default_value = 1.0
    link(height, groove.inputs["Value"])
    col2 = node("ShaderNodeVectorMath")
    col2.operation = "MULTIPLY"
    link(col.outputs["Vector"], col2.inputs[0])
    link(groove.outputs["Result"], col2.inputs[1])
    link(col2.outputs["Vector"], b.inputs["Base Color"])
    noise = node("ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 400
    link(tc.outputs["Object"], noise.inputs["Vector"])
    rng = node("ShaderNodeMapRange")
    rng.inputs["To Min"].default_value = 0.46
    rng.inputs["To Max"].default_value = 0.54
    link(noise.outputs["Fac"], rng.inputs["Value"])
    link(rng.outputs["Result"], b.inputs["Roughness"])
    return m


def sweep_mat():
    """A dark slate sweep with a soft pool of light behind the case, a little glossy, so the case stands
    out against the pool and is mirrored faintly in the floor."""
    m = mat("sweep", (0.1, 0.1, 0.1), rough=0.32, spec=0.45)
    nt = m.node_tree
    b = nt.nodes["Principled BSDF"]
    tc = nt.nodes.new("ShaderNodeTexCoord")
    dist = nt.nodes.new("ShaderNodeVectorMath")
    dist.operation = "DISTANCE"
    dist.inputs[1].default_value = (0.0, 0.5, 0.18)
    nt.links.new(tc.outputs["Object"], dist.inputs[0])
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.interpolation = "EASE"
    ramp.color_ramp.elements[0].position = 0.0
    ramp.color_ramp.elements[0].color = (0.16, 0.17, 0.19, 1)
    ramp.color_ramp.elements[1].position = 1.0
    ramp.color_ramp.elements[1].color = (0.008, 0.009, 0.011, 1)
    rng = nt.nodes.new("ShaderNodeMapRange")
    rng.inputs["From Min"].default_value = 0.0
    rng.inputs["From Max"].default_value = 0.65
    nt.links.new(dist.outputs["Value"], rng.inputs["Value"])
    nt.links.new(rng.outputs["Result"], ramp.inputs["Fac"])
    nt.links.new(ramp.outputs["Color"], b.inputs["Base Color"])
    return m


def acrylic_mat(color):
    """Clear cast acrylic: polished, refracting (IOR 1.49), with a faint cyan where the light runs through
    the thickness, as acrylic's edges show."""
    m = mat("acrylic", color, rough=0.02, bevel=0.3, spec=0.5)
    nt = m.node_tree
    b = nt.nodes["Principled BSDF"]
    b.inputs["Transmission Weight"].default_value = 1.0
    b.inputs["IOR"].default_value = 1.49
    vol = nt.nodes.new("ShaderNodeVolumeAbsorption")
    vol.inputs["Color"].default_value = (0.78, 0.95, 0.93, 1)
    vol.inputs["Density"].default_value = 12
    out = [n for n in nt.nodes if n.bl_idname == "ShaderNodeOutputMaterial"][0]
    nt.links.new(vol.outputs["Volume"], out.inputs["Volume"])
    return m


def materials():
    # one colour, or one per part (each part is printed on its own, so a single-colour printer can still
    # print them in different filaments): base, lid, frame
    cols = [srgb(c) for c in COLOR.split(",")]
    cols += [cols[-1]] * (3 - len(cols))
    for part, c in zip(("print", "print_lid", "print_frame"), cols):
        M[part] = acrylic_mat(c) if ACRYLIC else print_mat(c)
    M["pcb"] = mat("pcb", (0.012, 0.11, 0.035), rough=0.45, coat=0.6, bevel=0.1)
    M["vtx_pcb"] = mat("vtx_pcb", (0.008, 0.008, 0.01), rough=0.4, coat=0.7, bevel=0.1)
    M["silk"] = mat("silk", (0.85, 0.85, 0.82), rough=0.7)
    M["shell"] = mat("shell", (0.82, 0.82, 0.84), rough=0.22, metal=1.0, aniso=0.5, bevel=0.2)
    M["chrome"] = mat("chrome", (0.9, 0.9, 0.92), rough=0.12, metal=1.0, bevel=0.1)
    M["gold"] = mat("gold", (0.95, 0.72, 0.36), rough=0.2, metal=1.0)
    M["solder"] = mat("solder", (0.86, 0.87, 0.88), rough=0.15, metal=1.0)
    M["screw"] = mat("screw", (0.03, 0.03, 0.033), rough=0.42, metal=1.0, bevel=0.12)   # black oxide
    M["thread"] = mat("thread", (0.03, 0.03, 0.033), rough=0.45, metal=1.0, thread=0.5)
    M["plastic_black"] = mat("plastic_black", (0.012, 0.012, 0.013), rough=0.45, bevel=0.15)
    M["chip"] = mat("chip", (0.02, 0.02, 0.022), rough=0.6, bevel=0.1)
    M["smd"] = mat("smd", (0.42, 0.36, 0.26), rough=0.45)
    M["conn"] = mat("conn", (0.86, 0.83, 0.74), rough=0.5, bevel=0.15)
    M["fan"] = mat("fan", (0.015, 0.015, 0.017), rough=0.5, bevel=0.3)
    M["label"] = mat("label", (0.02, 0.02, 0.02), rough=0.3, coat=0.5)
    M["usb_blue"] = mat("usb_blue", (0.02, 0.15, 0.6), rough=0.45)
    M["rubber_black"] = mat("rubber_black", (0.018, 0.018, 0.02), rough=0.62, sheen=0.3, bevel=0.3)
    M["rubber_white"] = mat("rubber_white", (0.78, 0.78, 0.77), rough=0.55, sheen=0.3, bevel=0.3)
    M["coax"] = mat("coax", (0.05, 0.05, 0.055), rough=0.5)
    M["shrink"] = mat("shrink", (0.01, 0.01, 0.012), rough=0.35)
    M["wire_red"] = mat("wire_red", (0.55, 0.02, 0.015), rough=0.55, sheen=0.4)
    M["wire_black"] = mat("wire_black", (0.015, 0.015, 0.017), rough=0.55, sheen=0.4)
    M["wire_blue"] = mat("wire_blue", (0.01, 0.08, 0.45), rough=0.55, sheen=0.4)
    M["wire_yellow"] = mat("wire_yellow", (0.7, 0.5, 0.02), rough=0.55, sheen=0.4)
    M["led_red"] = mat("led_red", (1.0, 0.04, 0.02), emit=8)
    M["led_green"] = mat("led_green", (0.1, 1.0, 0.25), emit=6)
    M["led_amber"] = mat("led_amber", (1.0, 0.55, 0.05), emit=4)
    M["led_blue"] = mat("led_blue", (0.1, 0.3, 1.0), emit=10)
    M["flag"] = mat("flag", (0.0, 0.0, 0.0), rough=1.0, spec=0.0)
    M["ink"] = mat("ink", (0.55, 0.56, 0.58), rough=0.6)


# ---- groups: every object hangs under a rig, which the shots move or hide ----
def rig(name):
    if name not in RIGS:
        e = bpy.data.objects.new("rig_" + name, None)
        bpy.context.collection.objects.link(e)
        RIGS[name] = e
    return RIGS[name]


def group(name, obj):
    if name != "cutters":
        obj.parent = rig(name)
    return obj


def pose(mats=None, hide=(), only=None):
    mats = mats or {}
    for name, e in RIGS.items():
        e.matrix_world = mats.get(name, Matrix.Identity(4))
        hidden = name in hide or (only is not None and name not in only) or (only is None and name == "grid")
        for o in e.children:
            o.hide_render = hidden


def lift(dz):
    return Matrix.Translation((0, 0, dz * S))


# ---- geometry helpers (mm in, metres out) ----
def finish(o, m, grp, smooth_angle=None):
    # one slot for the whole object: a boolean cut leaves the original faces on an empty slot otherwise
    o.data.materials.clear()
    o.data.materials.append(m)
    if hasattr(o.data, "polygons"):
        for p in o.data.polygons:
            p.material_index = 0
    if smooth_angle is not None:
        bpy.context.view_layer.objects.active = o
        o.select_set(True)
        try:
            bpy.ops.object.shade_smooth_by_angle(angle=math.radians(smooth_angle))
        except Exception:
            bpy.ops.object.shade_smooth()
        o.select_set(False)
    return group(grp, o)


def box(name, size, at, m, grp, bevel=0.0):
    """A box of size (mm) with its minimum corner at `at` (mm), its edges rounded by `bevel`."""
    bpy.ops.mesh.primitive_cube_add(size=1, location=[(a + s / 2) * S for a, s in zip(at, size)])
    o = bpy.context.active_object
    o.name = name
    o.scale = [s * S for s in size]
    bpy.ops.object.transform_apply(scale=True)
    if bevel:
        mod = o.modifiers.new("bevel", "BEVEL")
        mod.width = bevel * S
        mod.segments = 4
        return finish(o, m, grp, 40)
    return finish(o, m, grp)


def cyl(name, d, h, at, m, grp, axis="Z", verts=48, d2=None):
    """A cylinder (or a cone to d2) of diameter d, length h (mm), from `at` (mm) along +axis."""
    rot = {"Z": (0, 0, 0), "X": (0, math.pi / 2, 0), "Y": (-math.pi / 2, 0, 0)}[axis]
    centre = list(at)
    centre["XYZ".index(axis)] += h / 2
    bpy.ops.mesh.primitive_cone_add(vertices=verts, radius1=d / 2 * S, radius2=(d2 if d2 else d) / 2 * S,
                                    depth=h * S, location=[c * S for c in centre], rotation=rot)
    o = bpy.context.active_object
    o.name = name
    return finish(o, m, grp, 35)


def blob(name, at, size, m, grp):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=24, ring_count=12, radius=1, location=[a * S for a in at])
    o = bpy.context.active_object
    o.name = name
    o.scale = [s / 2 * S for s in size]
    return finish(o, m, grp, 80)


def font():
    if not FONT:
        FONT.append(bpy.data.fonts.load(os.path.abspath(FONT_PATH), check_existing=True))
    return FONT[0]


def text(name, body, size, at, m, grp, rot=(0, 0, 0), align="LEFT", extrude=0.0):
    cu = bpy.data.curves.new(name, "FONT")
    cu.body = body
    cu.font = font()
    cu.size = size * S
    cu.align_x = align
    cu.extrude = extrude * S
    o = bpy.data.objects.new(name, cu)
    bpy.context.collection.objects.link(o)
    o.location = [a * S for a in at]
    o.rotation_euler = rot
    o.data.materials.append(m)
    return group(grp, o)


def cable(name, pts, d, m, grp, start=None, end=None, sleeves=()):
    """A wire through the points (mm), smooth, with its ends leaving along `start` and arriving along
    `end` when given. Sleeves are (from, to, diameter, material) over a fraction of its length:
    heat shrink, a jacket's end."""
    p = [Vector(q) for q in pts]
    n = len(p)
    tang = []
    for i in range(n):
        if i == 0:
            t = Vector(start) if start else p[1] - p[0]
        elif i == n - 1:
            t = Vector(end) if end else p[-1] - p[-2]
        else:
            t = (p[i + 1] - p[i]).normalized() + (p[i] - p[i - 1]).normalized()
        tang.append(t.normalized())

    def curve(cname, dia, mm):
        cu = bpy.data.curves.new(cname, "CURVE")
        cu.dimensions = "3D"
        cu.resolution_u = 24
        cu.bevel_depth = dia / 2 * S
        cu.bevel_resolution = 6
        cu.use_fill_caps = True
        sp = cu.splines.new("BEZIER")
        sp.use_smooth = True
        sp.bezier_points.add(n - 1)
        for i, bp in enumerate(sp.bezier_points):
            bp.co = p[i] * S
            back = (p[i] - p[i - 1]).length / 3 if i > 0 else 1
            fwd = (p[i + 1] - p[i]).length / 3 if i < n - 1 else 1
            bp.handle_left_type = bp.handle_right_type = "FREE"
            bp.handle_left = (p[i] - tang[i] * back) * S
            bp.handle_right = (p[i] + tang[i] * fwd) * S
        cu.materials.append(mm)
        o = bpy.data.objects.new(cname, cu)
        bpy.context.collection.objects.link(o)
        return group(grp, o)

    o = curve(name, d, m)
    for k, (f0, f1, sd, sm) in enumerate(sleeves):
        s = curve(f"{name}_sleeve{k}", sd, sm)
        s.data.bevel_factor_mapping_start = "SPLINE"
        s.data.bevel_factor_mapping_end = "SPLINE"
        s.data.bevel_factor_start = f0
        s.data.bevel_factor_end = f1
    return o


def boolean(target, cutter):
    mod = target.modifiers.new("cut", "BOOLEAN")
    mod.operation = "DIFFERENCE"
    mod.object = cutter
    bpy.context.view_layer.objects.active = target
    bpy.ops.object.modifier_apply(modifier=mod.name)
    bpy.data.objects.remove(cutter)


def screw(name, d, length, at, grp, down=True):
    """A button head cap screw: head on `at` (mm), shaft going down (or up), hex socket, thread."""
    x, y, z = at
    head_d, head_h = d * 1.9, d * 0.55
    cyl(name + "_shaft", d, length, (x, y, z - length if down else z), M["thread"], grp, verts=24)
    bpy.ops.mesh.primitive_uv_sphere_add(segments=40, ring_count=16, radius=1,
                                         location=(x * S, y * S, (z if down else z - head_h) * S))
    head = bpy.context.active_object
    head.name = name + "_head"
    head.scale = (head_d / 2 * S, head_d / 2 * S, head_h * 1.25 * S)
    bpy.ops.object.transform_apply(scale=True)
    floor = cyl("cut", head_d * 2, head_h * 2, (x, y, z - head_h * 2 if down else z), M["screw"], "cutters")
    boolean(head, floor)
    zs = z + head_h * 0.35 if down else z - head_h * 1.4
    sock = cyl("sock", d * 0.75, head_h * 1.1, (x, y, zs), M["screw"], "cutters", verts=6)
    boolean(head, sock)
    finish(head, M["screw"], grp, 35)
    if not down:
        head.rotation_euler = (math.pi, 0, 0)
        head.location.z = (2 * z - head_h) * S
    return head


def load(name, path, m, grp):
    bpy.ops.wm.stl_import(filepath=path, global_scale=S)
    o = bpy.context.selected_objects[0]
    o.name = name
    # the importer puts the mm -> m scale on the object; bake it into the mesh, so texture coordinates
    # (layer lines, grain) are in metres like everything else
    bpy.context.view_layer.objects.active = o
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    o.select_set(False)
    return finish(o, m, grp, 30)


# ---- the parts ----
def build_case():
    load("base", os.path.join(PARTS, "base.stl"), M["print"], "base")
    load("lid", os.path.join(PARTS, "lid_place.stl"), M["print_lid"], "lid")
    load("frame", os.path.join(PARTS, "frame_place.stl"), M["print_frame"], "frame")
    for i, (x, y) in enumerate(BOSSES):
        screw(f"lid_screw{i}", 3, 8, (x, y, Z_TOP + LID_T - 1.65), "lid_screws")
    for i, (dx, dy) in enumerate(((-21, -15), (21, -15), (-21, 15), (21, 15))):
        screw(f"frame_screw{i}", 3, 6, (BAY[0] + dx, BAY[1] + dy, Z_FRAME + 2.5), "frame")


def build_pi():
    g = "pi"
    load("pi_board", os.path.join(PARTS, "pi_board.stl"), M["pcb"], g)
    zt = Z_PTOP
    for h in PI_HOLES:
        cyl(f"pad_{h[0]}_{h[1]}", 6, 0.04, (h[0], h[1], zt), M["gold"], g)
        screw(f"pi_screw_{h[0]}_{h[1]}", 2.5, 6, (h[0], h[1], zt + 0.04), g)
    # the ports, from the Pi 4 drawing
    box("usb_c", (8.9, 7.4, 3.2), (11.2 - 4.45, -1.3, zt), M["shell"], g, bevel=1.2)
    box("usb_c_in", (7.0, 0.6, 1.8), (11.2 - 3.5, -1.5, zt + 0.7), M["chip"], g)
    for i, x in enumerate((26, 39.5)):
        box(f"hdmi{i}", (6.9, 7.5, 2.9), (x - 3.45, -0.8, zt), M["shell"], g, bevel=0.4)
        box(f"hdmi{i}_in", (5.8, 0.6, 1.6), (x - 2.9, -1.0, zt + 0.6), M["chip"], g)
    box("av_base", (7, 12.5, 5.5), (54 - 3.5, 0, zt), M["plastic_black"], g, bevel=0.4)
    cyl("av_jack", 6, 2.5, (54, -2, zt + 3), M["plastic_black"], g, axis="Y")
    for i, y in enumerate((9, 27)):
        box(f"usb{i}", (17.4, 13.1, 15.6), (70.1, y - 6.55, zt), M["shell"], g, bevel=0.5)
        for j in range(2):
            box(f"usb{i}_in{j}", (0.6, 11.6, 5.0), (87.1, y - 5.8, zt + 1.4 + j * 7.8), M["chip"], g)
            box(f"usb{i}_tab{j}", (0.8, 9.6, 1.4), (86.9, y - 4.8, zt + 1.4 + j * 7.8 + 3.1),
                M["usb_blue"] if i == 1 else M["plastic_black"], g)
    box("eth", (21.5, 16, 13.5), (66, 45.75 - 8, zt), M["shell"], g, bevel=0.5)
    box("eth_in", (0.6, 11.8, 9.0), (87.1, 45.75 - 5.9, zt + 1.6), M["chip"], g)
    box("eth_led_g", (0.6, 2, 1.2), (87.1, 45.75 - 7.6, zt + 0.8), M["led_green"], g)
    box("eth_led_y", (0.6, 2, 1.2), (87.1, 45.75 + 5.6, zt + 0.8), M["led_amber"], g)
    # the chips
    box("soc", (15, 15, 1.2), (22, 24, zt), M["chip"], g)
    box("soc_lid", (12, 12, 0.5), (23.5, 25.5, zt + 1.2), M["shell"], g, bevel=0.5)
    box("ram", (10, 15, 1.0), (39.5, 24, zt), M["chip"], g)
    box("wifi", (10, 11, 1.5), (7, 36, zt), M["shell"], g, bevel=0.3)
    box("vl805", (5, 5, 0.9), (57, 20, zt), M["chip"], g)
    box("pmic", (4, 4, 0.8), (8.5, 8.5, zt), M["chip"], g)
    for name, at in (("csi", (44.5, 2, zt)), ("dsi", (2.5, 21, zt))):
        box(name, (3.0, 22, 5.5), at, M["conn"], g, bevel=0.2)
        box(name + "_latch", (1.2, 21, 1.0), (at[0] + 0.9, at[1] + 0.5, zt + 5.5), M["plastic_black"], g)
    box("poe", (5.2, 5.2, 2.5), (56, 44, zt), M["plastic_black"], g)
    for i in range(2):
        for j in range(2):
            box(f"poe_pin{i}{j}", (0.64, 0.64, 8), (57.0 + i * 2.54, 45.0 + j * 2.54, zt + 2.5), M["gold"], g)
    # the GPIO header, 2 x 20
    box("gpio_base", (50.8, 5.08, 2.5), (7.1, 50, zt), M["plastic_black"], g, bevel=0.2)
    for n in range(1, 41):
        x, y = pin(n)
        box(f"pin{n}", (0.64, 0.64, 6), (x - 0.32, y - 0.32, zt + 2.5), M["gold"], g)
    # silkscreen, LEDs, SD card, small parts
    text("silk", "Raspberry Pi 4 Model B", 1.9, (21, 42.6, zt + 0.01), M["silk"], g)
    box("led_pwr", (1.6, 0.8, 0.6), (1.0, 7.0, zt), M["led_red"], g)
    box("led_act", (1.6, 0.8, 0.6), (1.0, 10.0, zt), M["led_green"], g)
    box("sd", (15, 11, 1.0), (-2.4, 24, Z_PCB - 2.2), M["plastic_black"], g, bevel=0.3)
    rnd = random.Random(7)
    for i in range(90):
        x, y = rnd.uniform(5, 80), rnd.uniform(5, 48)
        if 20 < x < 52 and 22 < y < 41 or 6 < x < 18 and 35 < y < 48 or 20 < x < 46 and 42 < y < 45:
            continue
        w, h = rnd.choice(((1.0, 0.5), (1.6, 0.8), (2.0, 1.25), (1.0, 0.5)))
        if rnd.random() < 0.5:
            w, h = h, w
        box(f"smd{i}", (w, h, 0.45), (x, y, zt), rnd.choice((M["chip"], M["smd"], M["smd"])), g)


def build_vtx():
    """A 16x16 whoop VTX: 20 x 20 mm, nothing within 3 mm of a mounting hole."""
    g = "vtx"
    cx, cy = BAY
    x0, y0 = cx - VTX / 2, cy - VTX / 2
    z = Z_VTX
    holes = [(cx + i * 8, cy + j * 8) for i in (-1, 1) for j in (-1, 1)]
    box("vtx_pcb", (VTX, VTX, 1.0), (x0, y0, z), M["vtx_pcb"], g, bevel=0.4)
    for k, (hx, hy) in enumerate(holes):
        cyl(f"vtx_ring{k}", 3.6, 0.04, (hx, hy, z + 1.0), M["gold"], g)
        screw(f"vtx_screw{k}", 2, 6, (hx, hy, z + 1.04), g)
    parts = [   # name, size, corner (offsets from the middle), material
        ("vtx_can", (9, 6.5, 2.4), (-4, 0), M["shell"]),
        ("vtx_chip", (4, 4, 0.9), (-6, -5), M["chip"]),
        ("vtx_mcu", (3, 3, 0.8), (-1, -4.5), M["chip"]),
        ("vtx_button", (3, 3, 1.4), (2.2, -6.2), M["shell"]),
        ("vtx_led", (1.2, 0.8, 0.5), (-1.2, -6.6), M["led_blue"]),
    ]
    for name, size, (dx, dy), m in parts:
        box(name, size, (cx + dx, cy + dy, z + 1), m, g, bevel=0.2 if m is M["shell"] else 0)
    cyl("vtx_button_top", 1.6, 0.6, (cx + 3.7, cy - 4.7, z + 2.4), M["plastic_black"], g)
    cyl("vtx_ufl", 2.6, 1.25, (UFL[0], UFL[1], z + 1), M["gold"], g, verts=24)
    cyl("vtx_ufl_plug", 2.0, 1.4, (UFL[0], UFL[1], z + 2.25), M["chrome"], g, verts=24)
    for name, dx in PADS.items():
        box(f"vtx_pad_{name}", (1.6, 1.8, 0.05), (cx + dx - 0.8, PAD_Y - 0.9, z + 1), M["gold"], g)
        blob(f"vtx_solder_{name}", (cx + dx, PAD_Y, z + 1.25), (2.0, 2.2, 0.9), M["solder"], g)
    rnd = random.Random(3)
    taken = [(cx + dx, cy + dy, cx + dx + s[0], cy + dy + s[1]) for _, s, (dx, dy), _ in parts]
    for i in range(40):
        x, y = rnd.uniform(x0 + 1.5, x0 + VTX - 2.5), rnd.uniform(y0 + 3.2, y0 + VTX - 1.5)
        if any((x - hx) ** 2 + (y - hy) ** 2 < 3.2 ** 2 for hx, hy in holes):
            continue
        if any(a - 0.6 < x < c + 0.4 and b - 0.6 < y < d + 0.4 for a, b, c, d in taken):
            continue
        if (x - UFL[0]) ** 2 + (y - UFL[1]) ** 2 < 2.5 ** 2:
            continue
        taken.append((x, y, x + 1.0, y + 0.5))
        box(f"vtx_smd{i}", (1.0, 0.5, 0.45), (x, y, z + 1), rnd.choice((M["smd"], M["chip"])), g)


def blade(name, r0, r1, a0, span, rise, grp):
    """One fan blade: a twisted strip from the hub to the tip, swept forward, 0.8 mm thick."""
    bm = bmesh.new()
    rows = []
    for i in range(7):
        r = r0 + (r1 - r0) * i / 6
        sweep = 0.3 * (i / 6) ** 1.5
        row = []
        for j in range(7):
            t = j / 6
            a = a0 + sweep + span * t
            zz = Z_FAN + 5 + (0.5 - t) * rise * (1 - 0.3 * i / 6)
            row.append(bm.verts.new(((BAY[0] + r * math.cos(a)) * S, (BAY[1] + r * math.sin(a)) * S, zz * S)))
        rows.append(row)
    for i in range(6):
        for j in range(6):
            bm.faces.new((rows[i][j], rows[i + 1][j], rows[i + 1][j + 1], rows[i][j + 1]))
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    o = bpy.data.objects.new(name, me)
    bpy.context.collection.objects.link(o)
    mod = o.modifiers.new("solid", "SOLIDIFY")
    mod.thickness = 0.8 * S
    mod.offset = 0
    for p in me.polygons:
        p.use_smooth = True
    o.data.materials.append(M["fan"])
    return group(grp, o)


def build_fan():
    g = "fan"
    cx, cy = BAY
    zb = Z_FAN
    bpy.ops.mesh.primitive_cube_add(size=1, location=(cx * S, cy * S, (zb + 5) * S))
    frame = bpy.context.active_object
    frame.name = "fan_frame"
    frame.scale = (40 * S, 40 * S, 10 * S)
    bpy.ops.object.transform_apply(scale=True)
    boolean(frame, cyl("bore", 38, 12, (cx, cy, zb - 1), M["fan"], "cutters", verts=96))
    for i in (-1, 1):
        for j in (-1, 1):
            boolean(frame, cyl("hole", 3.4, 12, (cx + i * 16, cy + j * 16, zb - 1), M["fan"], "cutters", verts=24))
    mod = frame.modifiers.new("bevel", "BEVEL")
    mod.width = 0.6 * S
    mod.segments = 3
    mod.limit_method = "ANGLE"
    finish(frame, M["fan"], g, 30)
    cyl("fan_hub", 19, 8, (cx, cy, zb + 1), M["fan"], g, verts=64)
    cyl("fan_hub_top", 17, 0.2, (cx, cy, zb + 9), M["fan"], g, verts=64)
    cyl("fan_label", 15, 0.1, (cx, cy, zb + 0.9), M["label"], g, verts=64)
    text("fan_text", "DC 5V  0.10A", 1.5, (cx, cy - 0.5, zb + 0.85), M["silk"], g, rot=(math.pi, 0, 0), align="CENTER")
    for k in range(7):
        blade(f"fan_blade{k}", 9.4, 18.6, k * 2 * math.pi / 7, 0.62, 5.5, g)
    for k in range(4):   # the struts on the exhaust side, hub to frame
        a = k * math.pi / 2 + math.pi / 4
        bpy.ops.mesh.primitive_cube_add(size=1, location=((cx + 14.5 * math.cos(a)) * S, (cy + 14.5 * math.sin(a)) * S,
                                                          (zb + 0.9) * S), rotation=(0, 0, a))
        s = bpy.context.active_object
        s.name = f"fan_strut{k}"
        s.scale = (11 * S, 2.0 * S, 1.8 * S)
        finish(s, M["fan"], g)


def dupont(n, grp):
    x, y = pin(n)
    box(f"dupont{n}", (2.5, 2.5, 14), (x - 1.25, y - 1.25, Z_DUPONT), M["plastic_black"], grp, bevel=0.25)


def build_jumpers():
    """Pi to VTX: 5 V (pin 2), SmartAudio through a 1 kOhm resistor under heat shrink (pin 7), ground (pin 9)."""
    g = "cables"
    for n in (2, 7, 9):
        dupont(n, g)
    z0 = Z_DUPONT + 13.4
    pz = PAD_Z + 0.5
    routes = {
        2: ("5v", M["wire_red"], [(8.9, 56.0, z0 + 2.4), (12.5, 59.5, z0 + 0.4), (21, 64.5, 20.0), (34.0, 68.1, 17.2)]),
        7: ("sa", M["wire_blue"], [(16.4, 53.6, z0 + 2.4), (18.5, 57.5, z0 + 1.2), (25, 62.5, 21.0), (36.5, 67.9, 17.3)]),
        9: ("gnd", M["wire_black"], [(18.9, 53.6, z0 + 2.2), (21.2, 57.2, z0 + 0.9), (28, 62.0, 20.8), (39.0, 67.7, 17.3)]),
    }
    for n, (pad, m, mid) in routes.items():
        x, y = pin(n)
        px = BAY[0] + PADS[pad]
        pts = [(x, y, z0), (x, y, z0 + 1.6), *mid, (px, PAD_Y - 2.6, pz + 0.15), (px, PAD_Y, pz)]
        sleeves = [(0.47, 0.63, 2.3, M["shrink"])] if n == 7 else []
        cable(f"jumper{n}", pts, 1.0, m, g, start=(0, 0, 1), end=(0, 1, 0), sleeves=sleeves)


def build_fan_wires(config):
    """The fan's leads to pins 4 (5 V) and 6 (ground), with the lid on (closed) or laid next to the case (beside)."""
    g = "fan_wires"
    for o in list(rig(g).children):
        bpy.data.objects.remove(o, do_unlink=True)
    if config is None:
        return
    for n in (4, 6):
        dupont(n, g)
    z0 = Z_DUPONT + 13.4
    if config == "closed":
        red = [(10.91, 53.81, z0), (10.91, 53.81, z0 + 3.5), (11.4, 54.6, 29.0), (15, 56.5, 30.2),
               (21.5, 58.8, 29.2), (25.2, 60.0, 27.8), (26.0, 61.5, 27.4)]
        black = [(13.45, 53.81, z0), (13.45, 53.81, z0 + 3.3), (13.9, 54.5, 28.4), (17, 56, 29.4),
                 (22.8, 58.4, 28.6), (26.4, 59.9, 27.7), (27.1, 61.5, 27.4)]
        cable("fan_red", red, 1.0, M["wire_red"], g, start=(0, 0, 1), end=(0, 1, 0))
        cable("fan_black", black, 1.0, M["wire_black"], g, start=(0, 0, 1), end=(0, 1, 0))
        return
    # the lid upside down beside the SD-card end: the fan's exit corner lands at x = 2 XP - x, z = 2 ZP - z
    fx = lambda x: 2 * XP - x
    fz = lambda z: 2 * ZP - z
    red = [(fx(26.0), 61.5, fz(27.4)), (fx(26.0), 59.0, fz(27.4) + 0.3), (-46.5, 54.5, 19.5), (-36, 50.5, 33),
           (-20, 50.5, 44), (-4, 52.5, 47.5), (6.5, 53.4, 41), (10.91, 53.81, 31), (10.91, 53.81, z0)]
    black = [(fx(27.1), 61.5, fz(27.4)), (fx(27.1), 59.0, fz(27.4) + 0.3), (-47.8, 54.0, 19.5), (-37.5, 49.6, 33.5),
             (-21, 49.6, 44.8), (-4.5, 51.4, 48.4), (8.5, 52.8, 41.5), (13.45, 53.81, 31), (13.45, 53.81, z0)]
    cable("fan_red", red, 1.0, M["wire_red"], g, start=(0, -1, 0), end=(0, 0, -1))
    cable("fan_black", black, 1.0, M["wire_black"], g, start=(0, -1, 0), end=(0, 0, -1))


def build_av():
    """The TRRS plug in the AV jack; its cable loops back in over the plug, lies on the board, and splits
    into video and ground at the VTX."""
    g = "cables"
    x, z = 54, Z_PTOP + 3
    cyl("av_sleeve", 3.6, 1.8, (x, -3.8, z), M["chrome"], g, axis="Y")
    cyl("av_mold", 6.6, 14, (x, -17.8, z), M["rubber_black"], g, axis="Y", d2=7.4, verts=64)
    for k in range(5):   # grip rings on the overmold
        cyl(f"av_grip{k}", 7.0 + k * 0.1, 0.5, (x, -16.5 + k * 1.2, z), M["rubber_black"], g, axis="Y", verts=64)
    cyl("av_relief", 3.8, 7, (x, -24.8, z), M["rubber_black"], g, axis="Y", d2=5.6, verts=48)
    pts = [(x, -24.0, z), (54, -31, z + 1), (54.5, -36, 18), (55.5, -31.5, 27), (56.5, -18, 32.5), (57, -6, 33),
           (57.5, 0, 31), (60, 8, 24), (62.5, 14, 14), (63.5, 20, 9.2), (63.5, 32, 9.2), (63.5, 44, 9.2),
           (63, 52, 11), (60, 59, 14.5), (56, 62.8, 17), (53.5, 64.5, 17.5)]
    cable("av_cable", pts, 3.6, M["rubber_black"], g, start=(0, -1, 0), end=(-1, 0.6, 0),
          sleeves=[(0.975, 1.0, 4.3, M["shrink"])])
    pz = PAD_Z + 0.45
    vid, gnd = BAY[0] + PADS["vid"], BAY[0] + PADS["gnd2"]
    cable("av_video", [(54.0, 64.0, 17.3), (51.5, 64.8, 17.0), (47.5, 66.5, 16.8), (vid, PAD_Y - 2.5, pz + 0.15),
                       (vid, PAD_Y, pz)], 0.8, M["wire_yellow"], g, end=(0, 1, 0))
    cable("av_ground", [(54.0, 64.5, 17.6), (52.0, 65.6, 17.3), (49.0, 67.6, 16.9), (gnd, PAD_Y - 2.5, pz + 0.15),
                        (gnd, PAD_Y, pz)], 0.8, M["wire_black"], g, end=(0, 1, 0))


def build_power():
    """A USB-C lead in the Pi's power port, lying away over the bench."""
    g = "cables"
    x, z = 11.2, Z_PTOP + 1.6
    box("usbc_plug_metal", (8.3, 5.0, 2.5), (x - 4.15, -5.6, z - 1.25), M["chrome"], g, bevel=1.1)
    box("usbc_mold", (12, 19, 6.4), (x - 6, -24.6, z - 3.2), M["rubber_white"], g, bevel=2.4)
    cyl("usbc_relief", 4.0, 7, (x, -31.4, z), M["rubber_white"], g, axis="Y", d2=5.4)
    pts = [(x, -30.5, z), (x, -40, z - 0.5), (x - 1, -52, 4), (x - 6, -66, 1.9), (x - 20, -86, 1.9),
           (x - 45, -106, 1.9), (x - 80, -122, 1.9), (x - 130, -132, 1.9)]
    cable("usbc_cable", pts, 3.8, M["rubber_white"], g, start=(0, -1, 0))


def build_antenna():
    """A whoop-style whip: the VTX's u.fl pigtail runs under the fan and out through the notch at the
    top of the USB-end wall, and ends in a heat-shrunk dipole."""
    g = "cables"
    ux, uy, uz = UFL[0], UFL[1], Z_VTX + 1 + 2.25 + 0.7
    pts = [(ux + 0.6, uy, uz), (57, 83.5, 19.5), (68, 83, 24), (80, 82, 29.5), (86, 80.8, 33.0), (89, 80.5, 33.4),
           (96, 80.4, 35.5), (104, 80.2, 40.5), (112, 80, 46.5), (119, 79.8, 53)]
    cable("pigtail", pts, 1.13, M["coax"], g, start=(1, 0, 0), sleeves=[(0.62, 1.0, 1.9, M["shrink"])])


def build_grid():
    g = "grid"
    for r, pattern in enumerate(PATTERNS):
        for c, screw_size in enumerate(SCREWS):
            o = load(f"frame_{pattern}_{screw_size}", os.path.join(HERE, "stl", f"frame_{pattern}_{screw_size}.stl"),
                     M["print_frame"], g)
            o.location = (CENTER.x + (c - 1) * 0.062, CENTER.y + (r - 1.5) * 0.05, 0)
        text(f"row{r}", f"{pattern}×{pattern}", 6, (CX - 98, 51 + (r - 1.5) * 50 - 3, 0.02), M["ink"], g, align="RIGHT")
    for c, screw_size in enumerate(SCREWS):
        text(f"col{c}", screw_size, 7, (CX + (c - 1) * 62, 51 - 2.5 * 50 + 10, 0.02), M["ink"], g, align="CENTER")


# ---- the studio ----
def build_stage():
    """A seamless sweep and four softboxes on one rig that turns with the camera."""
    stage = bpy.data.objects.new("stage", None)
    bpy.context.collection.objects.link(stage)
    stage.location = (CENTER.x, CENTER.y, 0)
    bm = bmesh.new()
    prof = [(-1.4 + i * 0.16, 0.0) for i in range(11)]
    r = 0.4
    for k in range(1, 25):
        a = k / 24 * math.pi / 2
        prof.append((0.2 + r * math.sin(a), r - r * math.cos(a)))
    prof.append((0.2 + r, 1.6))
    rows = [[bm.verts.new((x, y, z - 0.00001)) for (y, z) in prof] for x in (-1.6, 1.6)]
    for j in range(len(prof) - 1):
        bm.faces.new((rows[0][j], rows[0][j + 1], rows[1][j + 1], rows[1][j]))
    me = bpy.data.meshes.new("sweep")
    bm.to_mesh(me)
    bm.free()
    for p in me.polygons:
        p.use_smooth = True
    sweep = bpy.data.objects.new("sweep", me)
    bpy.context.collection.objects.link(sweep)
    sweep.data.materials.append(sweep_mat())
    sweep.parent = stage
    # black cards seen only in reflections, so metal and satin plastic show some dark as well as light
    for name, at, rot, size in (("flag_back", (0.0, -1.0, 0.45), (math.radians(70), 0, 0), (1.6, 0.8)),
                                ("flag_side", (0.8, 0.1, 0.3), (0, math.radians(-80), 0), (0.9, 0.7))):
        bpy.ops.mesh.primitive_plane_add(size=1, location=at, rotation=rot)
        f = bpy.context.active_object
        f.name = name
        f.scale = (size[0], size[1], 1)
        f.parent = stage
        f.data.materials.append(M["flag"])
        f.visible_camera = False
        f.visible_shadow = False
        f.visible_diffuse = False

    def softbox(name, at, power, size, size_y, color):
        bpy.ops.object.light_add(type="AREA")
        lamp = bpy.context.active_object
        lamp.name = name
        lamp.parent = stage
        lamp.location = at
        lamp.data.shape = "RECTANGLE"
        lamp.data.size = size
        lamp.data.size_y = size_y
        lamp.data.energy = power
        lamp.data.color = color
        aim(lamp, Vector((0, 0, 0.02)))
    # the camera stands on the stage's -y side
    softbox("key", (-0.34, -0.30, 0.42), 10, 0.5, 0.35, (1.0, 0.95, 0.88))
    softbox("fill", (0.55, -0.25, 0.14), 2, 0.9, 0.6, (0.92, 0.95, 1.0))
    softbox("top", (0.0, 0.05, 0.65), 2.5, 0.7, 0.7, (1.0, 1.0, 1.0))
    # rims in StickTime's menu colours: orange from behind on the left, blue from behind on the right
    softbox("rim_orange", (-0.38, 0.38, 0.22), 9, 0.7, 0.1, (1.0, 0.42, 0.08))
    softbox("rim_blue", (0.38, 0.38, 0.22), 9, 0.7, 0.1, (0.15, 0.4, 1.0))
    return stage


def aim(obj, target):
    d = Vector(target) - obj.location
    obj.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()


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
    scene.cycles.samples = 48 if PREVIEW else (600 if ACRYLIC else 400)
    scene.cycles.use_denoising = True
    if ACRYLIC:   # light goes through several walls and the parts behind them
        scene.cycles.max_bounces = 24
        scene.cycles.transmission_bounces = 16
        scene.cycles.transparent_max_bounces = 16
        scene.cycles.glossy_bounces = 8
    scene.render.resolution_x = 2000
    scene.render.resolution_y = 1400
    scene.render.resolution_percentage = 40 if PREVIEW else 100
    scene.render.image_settings.file_format = "JPEG"   # a tenth of a PNG's size, and the same to the eye
    scene.render.image_settings.quality = 90
    try:
        scene.view_settings.view_transform = "AgX"
        scene.view_settings.look = "AgX - Punchy"   # keeps a bright filament saturated
    except Exception:
        pass
    world = bpy.data.worlds.new("studio")
    scene.world = world
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs[0].default_value = (0.6, 0.62, 0.65, 1)
    bg.inputs[1].default_value = 0.02
    bpy.ops.object.camera_add(location=(0, 0, 1))
    cam = bpy.context.active_object
    cam.name = "camera"
    cam.data.sensor_width = 36
    scene.camera = cam
    return scene, cam


def print_lines(mm_per_px):
    """Lines finer than about 5 pixels beat against the pixel grid into broad stripes (moire), so a
    wide shot draws them a little coarser than the real 0.2 mm; a close-up gets them true to size."""
    for n, k, period in PRINT_LINES:
        p = max(period, 5 * mm_per_px)
        if k is None:   # the bump: ridges as deep as a real layer's, for the pitch drawn
            n.inputs["Distance"].default_value = 0.12 * p * S
        else:
            n.inputs["Scale"].default_value = k / (p * S)


def shot(scene, cam, stage, name, offset, target=None, lens=50, fstop=None):
    if WANTED and name not in WANTED:
        return
    target = CENTER if target is None else target
    dist = Vector(offset).length
    print_lines(dist / S * cam.data.sensor_width / lens / (scene.render.resolution_x * scene.render.resolution_percentage / 100))
    cam.location = target + Vector(offset)
    aim(cam, target)
    cam.data.lens = lens
    cam.data.dof.use_dof = fstop is not None
    if fstop:
        cam.data.dof.aperture_fstop = fstop
        cam.data.dof.focus_distance = (target - cam.location).length
    stage.rotation_euler = (0, 0, math.atan2(offset[1], offset[0]) + math.pi / 2)
    scene.render.filepath = os.path.join(OUT, name + ".jpg")
    bpy.ops.render.render(write_still=True)
    print("rendered", scene.render.filepath)


def main():
    export_pieces()
    scene, cam = setup()
    materials()
    stage = build_stage()
    build_case()
    build_pi()
    build_vtx()
    build_fan()
    build_jumpers()
    build_av()
    build_power()
    build_antenna()
    build_grid()
    rig("fan_wires")

    flip = (Matrix.Translation((XP * S, 0, ZP * S)) @ Matrix.Rotation(math.pi, 4, "Y")
            @ Matrix.Translation((-XP * S, 0, -ZP * S)))
    vtx_c = Vector((BAY[0] * S, BAY[1] * S, (Z_VTX + 1) * S))

    pose()
    build_fan_wires("closed")
    shot(scene, cam, stage, "case", (0.205, 0.215, 0.11), target=CENTER + Vector((0.004, 0, -0.003)), lens=60)
    shot(scene, cam, stage, "front", (0.0, 0.34, 0.07), target=CENTER + Vector((0, 0, -0.003)), lens=65)
    shot(scene, cam, stage, "back", (-0.22, -0.205, 0.115), target=CENTER + Vector((-0.004, 0, -0.003)), lens=60)

    pose({"lid": flip, "fan": flip}, hide=("lid_screws",))
    build_fan_wires("beside")
    # from high over the HDMI side: the Pi, the header's wires, the VTX on its frame, the AV cable, the
    # antenna's pigtail, and the fan on the lid beside the case with its leads, all in view
    shot(scene, cam, stage, "open", (-0.045, -0.165, 0.27), target=Vector((-0.012, 0.054, 0.008)), lens=40)
    shot(scene, cam, stage, "detail", (0.045, 0.07, 0.115), target=vtx_c + Vector((-0.004, -0.004, 0)), lens=55,
         fstop=22)

    # the Pi's ports sit in the walls' openings, so it has to clear the walls (39.4 mm) before it is free
    pose({"pi": lift(44), "frame": lift(30), "vtx": lift(50), "fan": lift(84), "lid": lift(104),
          "lid_screws": lift(132)}, hide=("cables", "fan_wires"))
    build_fan_wires(None)
    shot(scene, cam, stage, "exploded", (0.27, 0.30, 0.25), target=CENTER + Vector((0, 0, 0.05)), lens=50)

    # the assembly steps
    inside = Vector((CX * S, 60 * S, 8 * S))
    pose(only=("base", "pi"))
    shot(scene, cam, stage, "step1", (-0.03, -0.17, 0.24), target=inside, lens=50)
    pose(only=("base", "pi", "frame", "vtx"))
    shot(scene, cam, stage, "step2", (-0.03, -0.17, 0.24), target=inside, lens=50)
    pose({"lid": flip, "fan": lift(30) @ flip}, only=("lid", "fan"))   # the lid on the bench, the fan going on
    shot(scene, cam, stage, "step3", (0.12, -0.17, 0.17), target=Vector(((2 * XP - CX) * S, 51 * S, 0.012)), lens=50)
    pose({"lid": lift(45), "fan": lift(45), "lid_screws": lift(70)}, hide=("fan_wires",))
    shot(scene, cam, stage, "step4", (0.24, 0.27, 0.22), target=CENTER + Vector((0, 0, 0.03)), lens=50)

    pose(only=("grid",))
    shot(scene, cam, stage, "frames", (0.0, -0.28, 0.27), target=Vector((CENTER.x - 0.01, CENTER.y - 0.022, 0)),
         lens=44, fstop=16)


main()
