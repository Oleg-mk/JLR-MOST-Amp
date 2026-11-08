"""Footprint and VRML model of J10, the side-insert USB-C receptacle with 14 DIP contacts, from the seller's drawing
(AliExpress 1005006026498474, "RECOMMENDED P.C.B. LAYOUT", seen from the component side).
    python usbc_side.py
The receptacle stands on its edge and the plug goes in along the board. Origin on the board edge at the centre line,
the body towards -y. Contact tails in two columns 0.75 apart, A on +x and B on -x, rows on a 0.45 grid from 7.00
(A12, B1) to 12.85 (A1, B12) from the edge; holes 0.40, pads 0.60. Shell legs 2.86 apart: the front pair 1.00 wide at
7.05 from the edge, the back pair 1.60 wide at 13.15; slots 0.50 wide and 0.10 longer than the leg, pads 0.20 bigger
all round. Body 14.00 long, mouth 0.05 inside the edge (the side view gives 7.00 from the mouth to the front legs),
shell 8.34 x 2.56 standing 0.73 above the board (9.07 over the seating plane).
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
NAME = "USB_C_Receptacle_14P_SideInsert_THT"
U = 2.54                      # mm per VRML unit in KiCad
EDGE_ROW = 7.00               # A12/B1 row from the board edge
STEP = 0.45
ROWS = {"A": {"A12": 0, "A9": 2, "A7": 5, "A6": 7, "A5": 9, "A4": 11, "A1": 13},
        "B": {"B1": 0, "B4": 2, "B5": 4, "B6": 6, "B7": 8, "B9": 11, "B12": 13}}
COL = {"A": 0.375, "B": -0.375}
HOLE, PAD = 0.40, 0.60
LEG_X = 1.43
LEGS = [(7.05, 1.00), (13.15, 1.60)]          # centre from the edge, leg width
SLOT_W, SLOT_EXTRA, RING = 0.50, 0.10, 0.20
BODY = (0.05, 14.05)                          # mouth and back from the edge
BODY_X = 1.43
CRT_X, CRT_Y = round(LEG_X + SLOT_W / 2 + RING + 0.25, 2), (BODY[0] - 0.25, -(13.15 + (1.60 + SLOT_EXTRA) / 2 + RING + 0.25))
SILK_X = BODY_X + 0.11


def pads():
    out = []
    for col, rows in ROWS.items():
        for name, k in rows.items():
            out.append((name, COL[col], -round(EDGE_ROW + k * STEP, 2)))
    return out


def footprint():
    f = lambda v: ("%.3f" % v).rstrip("0").rstrip(".")
    s = ['(footprint "%s"' % NAME, "\t(version 20260206)", '\t(generator "jlr_most_amp_gen")', '\t(layer "F.Cu")',
         '\t(descr "USB-C receptacle, side insert (stands on its edge, plug along the board), 14 DIP contacts (USB 2.0, '
         'no SBU), 4 shell legs; from the seller\'s drawing, AliExpress 1005006026498474; origin on the board edge, '
         'mouth 0.05 inside it")',
         '\t(tags "USB C Type-C receptacle side insert 90 degree DIP 14P")',
         '\t(property "Reference" "REF**" (at 0 -15.2 0) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))',
         '\t(property "Value" "%s" (at 0 -16.6 0) (layer "F.Fab") (effects (font (size 0.8 0.8) (thickness 0.12))))'
         % NAME, "\t(attr through_hole)"]
    s.append('\t(fp_rect (start %s %s) (end %s %s) (stroke (width 0.1) (type solid)) (fill no) (layer "F.Fab"))'
             % (f(-BODY_X), f(-BODY[0]), f(BODY_X), f(-BODY[1])))
    s.append('\t(fp_line (start %s 0) (end %s 0) (stroke (width 0.1) (type solid)) (layer "F.Fab"))' % (f(-CRT_X), f(CRT_X)))
    s.append('\t(fp_text user "board edge" (at 0 0.6 0) (layer "F.Fab") (effects (font (size 0.5 0.5) (thickness 0.075))))')
    s.append('\t(fp_rect (start %s %s) (end %s %s) (stroke (width 0.05) (type solid)) (fill no) (layer "F.CrtYd"))'
             % (f(-CRT_X), f(-CRT_Y[0]), f(CRT_X), f(CRT_Y[1])))
    # silk along the body sides, 0.2 clear of the leg pads, and across the back between them
    gaps = [(c - (w + SLOT_EXTRA) / 2 - RING - 0.2, c + (w + SLOT_EXTRA) / 2 + RING + 0.2) for c, w in LEGS]
    runs = [(0.5, gaps[0][0]), (gaps[0][1], gaps[1][0])]
    for x in (-SILK_X, SILK_X):
        for a, b in runs:
            s.append('\t(fp_line (start %s %s) (end %s %s) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))'
                     % (f(x), f(-a), f(x), f(-b)))
    bx = LEG_X - SLOT_W / 2 - RING - 0.2
    s.append('\t(fp_line (start %s %s) (end %s %s) (stroke (width 0.12) (type solid)) (layer "F.SilkS"))'
             % (f(-bx), f(-(BODY[1] + 0.11)), f(bx), f(-(BODY[1] + 0.11))))
    for name, x, y in pads():
        s.append('\t(pad "%s" thru_hole circle (at %s %s) (size %s %s) (drill %s) (layers "*.Cu" "*.Mask"))'
                 % (name, f(x), f(y), f(PAD), f(PAD), f(HOLE)))
    for c, w in LEGS:
        hl = w + SLOT_EXTRA
        for x in (-LEG_X, LEG_X):
            s.append('\t(pad "SH" thru_hole oval (at %s %s) (size %s %s) (drill oval %s %s) (layers "*.Cu" "*.Mask"))'
                     % (f(x), f(-c), f(SLOT_W + 2 * RING), f(hl + 2 * RING), f(SLOT_W), f(hl)))
    s += ['\t(model "${KIPRJMOD}/jlr_most_amp.3dshapes/%s.wrl"' % NAME, "\t\t(offset (xyz 0 0 0))",
          "\t\t(scale (xyz 1 1 1))", "\t\t(rotate (xyz 0 0 0))", "\t)", ")"]
    return "\n".join(s) + "\n"


def box(x0, x1, y0, y1, z0, z1):
    """model frame: y = distance from the board edge (footprint -y), z up from the board"""
    pts = [(x, y, z) for z in (z0, z1) for y in (y0, y1) for x in (x0, x1)]
    return pts, [[0, 2, 3, 1], [4, 5, 7, 6], [0, 1, 5, 4], [2, 6, 7, 3], [0, 4, 6, 2], [1, 3, 7, 5]]


def shape(boxes, color):
    pts, faces = [], []
    for b in boxes:
        p, fs = box(*b)
        faces += [[i + len(pts) for i in fc] for fc in fs]
        pts += p
    s = ["Shape {", "  appearance Appearance { material Material {",
         "    diffuseColor %.2f %.2f %.2f specularColor 0.6 0.6 0.6 shininess 0.5 } }" % color,
         "  geometry IndexedFaceSet { solid FALSE coord Coordinate { point ["]
    s += ["    %.4f %.4f %.4f," % (x / U, y / U, z / U) for (x, y, z) in pts]
    s += ["  ] } coordIndex ["] + ["    " + " ".join(str(i) for i in fc) + " -1," for fc in faces] + ["  ] }", "}"]
    return "\n".join(s)


def model():
    z0, z1, t, tail = 0.73, 9.07, 0.25, -1.75
    metal = [(-1.28, 1.28, BODY[0], BODY[1], z0, z1)]
    metal += [(x - 0.125, x + 0.125, c - w / 2, c + w / 2, tail, z0) for c, w in LEGS for x in (-LEG_X, LEG_X)]
    pins = [(x - 0.125, x + 0.125, -y - 0.125, -y + 0.125, tail, 0.0) for _, x, y in pads()]
    mouth = [(-1.28 + t, 1.28 - t, BODY[0] - 0.02, BODY[0] + 0.4, z0 + t, z1 - t)]
    base = [(-1.28, 1.28, EDGE_ROW - 0.7, BODY[1], 0.0, z0)]
    return "\n".join(["#VRML V2.0 utf8", shape(metal, (0.78, 0.78, 0.80)), shape(pins, (0.85, 0.80, 0.60)),
                      shape(mouth, (0.08, 0.08, 0.08)), shape(base, (0.15, 0.15, 0.15))]) + "\n"


if __name__ == "__main__":
    hw = os.path.dirname(HERE)
    open(os.path.join(hw, "jlr_most_amp.pretty", NAME + ".kicad_mod"), "w", encoding="utf-8", newline="\n").write(footprint())
    open(os.path.join(hw, "jlr_most_amp.3dshapes", NAME + ".wrl"), "w", encoding="utf-8", newline="\n").write(model())
    print("written", NAME)
