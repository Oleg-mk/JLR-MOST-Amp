"""Custom footprints of the tube board, written into each variant's jlr_tube.pretty.

Valve_6N28B-V_Flat: 6N28B-V lying on the board (reference book fig. 380: bulb D10.2 x 35, straight part >= 21 from the
base, 9 wire leads D0.4 on a 10-position circle, 36 deg apart, numbered clockwise seen from the base, position 10 cut,
colour mark between 10 and 1). The tube lies with position 10 up. Two rows of tubes with clips share the 30 mm
ledge, so the leads go into two short rows of holes in front of the base: the lower half of the circle into the near
row, the upper half into the far row 2.54 behind it; no lead crosses another. Pitch 2.54, round pads D1.2: 1.34 mm
copper gap in a row and 1.64 between the staggered rows, above the 1.25 mm IPC-2221 B2 needs for 200 V.
The bulb is held by one Keystone 3566 clip (catalogue K75 p.52: 13/32" fuse clip, 11.1 along the axis x 12.3 x 19.0 high,
two flat pins 2.0 x 0.76 into D2.25 holes 10.5 apart along the axis): it fits the 21 mm straight part once, at 6..17 mm
from the base; the soldered leads hold the base end. The clip's holes are part of this footprint (pads without number),
so the clip always sits under the bulb; the clip itself is in the BOM through an off-board symbol.
Neutrik_NC3MAH: drawing ST-NC3MAH rev C (PCB layout, component side), origin on the axis at the rear face of the panel,
the board runs to +Y: pins 2 3 1 at 17.78 (pitch 3.81), ground contact G at 12.7, plastic pegs at (-3.81, 3.81) and
(3.81, 8.89); holes D1.6 for 1, 2 and the pegs, D1.2 for 3 and G; body 19.8 wide, 18.6 behind the panel, 2.7 in front;
XLR axis 12.5 above the board.
"""
import os, uuid

HERE = os.path.dirname(os.path.abspath(__file__))
NS = uuid.UUID("4d3e1a7c-92b5-4f60-8c1d-6e2f0a9b7c31")


def uid(*k):
    return str(uuid.uuid5(NS, "/".join(str(v) for v in k)))


def f(v):
    s = "%.4f" % v
    s = s.rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


class FP:
    def __init__(self, name, descr, tags, attr="through_hole"):
        self.name, self.items = name, []
        self.head = ['(footprint "%s"' % name, '\t(version 20260206)', '\t(generator "jlr_tube_gen")', '\t(layer "F.Cu")',
                     '\t(descr "%s")' % descr, '\t(tags "%s")' % tags]
        self.attr = attr

    def prop(self, key, val, x, y, layer, hide=False):
        self.items.append('\t(property "%s" "%s" (at %s %s 0) (layer "%s")%s (uuid "%s") (effects (font (size 1 1) (thickness 0.15))))'
                          % (key, val, f(x), f(y), layer, " (hide yes)" if hide else "", uid(self.name, "p", key)))

    def line(self, x1, y1, x2, y2, layer, w):
        self.items.append('\t(fp_line (start %s %s) (end %s %s) (stroke (width %s) (type solid)) (layer "%s") (uuid "%s"))'
                          % (f(x1), f(y1), f(x2), f(y2), f(w), layer, uid(self.name, "l", layer, x1, y1, x2, y2)))

    def poly(self, pts, layer, w):
        for (a, b) in zip(pts, pts[1:] + pts[:1]):
            self.line(a[0], a[1], b[0], b[1], layer, w)

    def circle(self, x, y, r, layer, w, fill=False):
        self.items.append('\t(fp_circle (center %s %s) (end %s %s) (stroke (width %s) (type solid)) (fill %s) (layer "%s") (uuid "%s"))'
                          % (f(x), f(y), f(x + r), f(y), f(w), "yes" if fill else "no", layer, uid(self.name, "c", layer, x, y, r)))

    def text(self, t, x, y, layer, size=1.0):
        self.items.append('\t(fp_text user "%s" (at %s %s 0) (layer "%s") (uuid "%s") (effects (font (size %s %s) (thickness %s))))'
                          % (t, f(x), f(y), layer, uid(self.name, "t", t, x, y), f(size), f(size), f(size * 0.15)))

    def pad(self, num, shape, x, y, sx, sy, drill):
        rr = " (roundrect_rratio 0.25)" if shape == "roundrect" else ""
        self.items.append('\t(pad "%s" thru_hole %s (at %s %s) (size %s %s) (drill %s) (layers "*.Cu" "*.Mask")%s (uuid "%s"))'
                          % (num, shape, f(x), f(y), f(sx), f(sy), f(drill), rr, uid(self.name, "pad", num, x, y)))

    def npth(self, x, y, d):
        self.items.append('\t(pad "" np_thru_hole circle (at %s %s) (size %s %s) (drill %s) (layers "*.Cu" "*.Mask") (uuid "%s"))'
                          % (f(x), f(y), f(d), f(d), f(d), uid(self.name, "npth", x, y)))

    def text_out(self):
        return "\n".join(self.head + ['\t(attr %s)' % self.attr] + self.items + [')']) + "\n"


def valve(name, value, L, near, far, a0, da, zone, descr, tags, apart=None):
    """tube lying flat on the foil side of the ledge. The lower half of the lead circle bends into the near row of holes
    (6 mm in front of the base), the upper half into the far row (2.54 further): {pin: y}. Pin 1 is at -Y, the right of
    the base view. Lead k sits at a0 + (k - 1) * da on the base circle, clockwise from the top seen from the base (fab
    drawing). zone = (from, to) marks of the bulb's straight part, measured from the base. apart: {pin: (x, y)} holes
    out of the rows."""
    import math
    R = 5.1                   # bulb radius
    X1, X2 = 6.0, 8.54        # near and far rows of holes
    XC = -11.5                # clip centre: 6..17 mm from the base
    pins = {**{k: (X1, y) for k, y in near.items()}, **{k: (X2, y) for k, y in far.items()}, **(apart or {})}
    fp = FP(name, descr, tags)
    fp.prop("Reference", "REF**", -26.5, -1.0, "F.SilkS")
    fp.prop("Value", value, -26.5, 1.5, "F.Fab")
    for k in sorted(pins):
        x, y = pins[k]
        fp.pad(str(k), "roundrect" if k == 1 else "circle", x, y, 1.2, 1.2, 0.7)
    for dx in (-5.25, 5.25):   # Keystone 3566 pins, no net
        fp.pad("", "circle", XC + dx, 0, 3.5, 3.5, 2.25)
    # fab: bulb, its dome and tip, the clip, the straight-part marks, leads from the base circle (r 3.5) to the holes
    fp.poly([(-(L - 3), -R), (0, -R), (0, R), (-(L - 3), R)], "F.Fab", 0.1)
    fp.line(-(L - 3), -R, -(L - 1.5), -2.0, "F.Fab", 0.1)
    fp.line(-(L - 3), R, -(L - 1.5), 2.0, "F.Fab", 0.1)
    fp.poly([(-(L - 1.5), -2.0), (-L, -1.0), (-L, 1.0), (-(L - 1.5), 2.0)], "F.Fab", 0.1)
    fp.poly([(XC - 5.55, -6.15), (XC + 5.55, -6.15), (XC + 5.55, 6.15), (XC - 5.55, 6.15)], "F.Fab", 0.1)
    fp.text("Keystone 3566", XC, -7.0, "F.Fab", 0.8)
    for x in zone:
        if x:
            fp.line(-x, -R - 0.6, -x, R + 0.6, "F.Fab", 0.05)
            fp.text("%g" % x, -x - 1.2, -R - 1.2, "F.Fab", 0.6)
    for k, (x, y) in pins.items():
        yb = -3.5 * math.sin(math.radians(a0 + (k - 1) * da))     # right of the base view = -Y
        fp.line(0, yb, x - 0.6, y, "F.Fab", 0.1)
    fp.text("mark up", X2 + 4.0, 0, "F.Fab", 0.8)
    # silk: bulb outline on the tube's side, broken by the clip outline; pin 1 dot beyond the far row
    for y in (-R - 0.15, R + 0.15):
        fp.line(-L - 0.15, y, XC - 5.7, y, "F.SilkS", 0.15)
        fp.line(XC + 5.7, y, 0.15, y, "F.SilkS", 0.15)
    fp.line(-L - 0.15, -R - 0.15, -L - 0.15, R + 0.15, "F.SilkS", 0.15)
    fp.line(0.15, -R - 0.15, 0.15, R + 0.15, "F.SilkS", 0.15)
    for y in (-6.3, 6.3):              # clip outline, its sides broken at the clip's pads
        fp.line(XC - 5.7, y, XC + 5.7, y, "F.SilkS", 0.15)
        for x in (XC - 5.7, XC + 5.7):
            fp.line(x, y, x, 2.1 if y > 0 else -2.1, "F.SilkS", 0.15)
    fp.circle(X2 + 1.5, pins[1][1], 0.25, "F.SilkS", 0.15, fill=True)
    # courtyard: bulb with the clip, then the leads and the two rows of holes (each side as far as its pads reach)
    ylo = max(R + 0.25, 0.85 - min(y for _, y in pins.values()))
    yhi = max(R + 0.25, 0.85 + max(y for _, y in pins.values()))
    xe = X2 + 1.9
    fp.poly([(-L - 0.35, -R - 0.25), (XC - 5.8, -R - 0.25), (XC - 5.8, -6.4), (XC + 5.8, -6.4), (XC + 5.8, -R - 0.25),
             (0, -R - 0.25), (0, -ylo), (xe, -ylo), (xe, yhi), (0, yhi), (0, R + 0.25),
             (XC + 5.8, R + 0.25), (XC + 5.8, 6.4), (XC - 5.8, 6.4), (XC - 5.8, R + 0.25), (-L - 0.35, R + 0.25)], "F.CrtYd", 0.05)
    return fp


def valve_6n28b():
    # fig. 380: D10.2 x 35, straight part 21 from the base; 10 positions 36 deg apart, 10 cut and turned up.
    # near row (lower half) 3 4 6 7, far row (upper half) 2 1 9 8, staggered by 1.27; 5 (the screen, at the bottom of the
    # base) bent short, 3 mm in front of the base, out of the near row so B+ can pass between 4 and 6
    return valve("Valve_6N28B-V_Flat", "6N28B-V", 35.0, {3: -5.08, 4: -2.54, 6: 2.54, 7: 5.08},
                 {2: -3.81, 1: -1.27, 9: 1.27, 8: 3.81}, 36.0, 36.0, (0, 21.0),
                 "6N28B-V double triode lying flat, bulb D10.2 x 35 (fig. 380), 9 wire leads D0.4 bent down into two rows: "
                 "near 3 4 6 7, 5 (screen) 3 mm in front of the base, far 2 1 9 8; colour mark (position 10) up; bulb in a Keystone 3566 clip 6..17 mm from the base, axis ~12.5 mm high",
                 "valve tube subminiature 6N28B 6N28B-V", apart={5: (3.0, 0.0)})


def valve_6n16b():
    # outline 6N16B-V: D10.2 x 36 max, not cylindrical within 5 mm of the base; 8 leads 45 deg apart, the mark between
    # 8 and 1 turned up. near row (lower half) 3 4 5 6, far row (upper half) 2 1 8 7
    return valve("Valve_6N16B-V_Flat", "6N16B-V", 36.0, {3: -3.81, 4: -1.27, 5: 1.27, 6: 3.81},
                 {2: -3.81, 1: -1.27, 8: 1.27, 7: 3.81}, 22.5, 45.0, (5.0, 0),
                 "6N16B-V double triode lying flat, bulb D10.2 x 36, 8 wire leads D0.4 bent down into two rows: "
                 "near 3 4 5 6, far 2 1 8 7; mark (between 8 and 1) up; bulb in a Keystone 3566 clip 6..17 mm from the base, axis ~12.5 mm high",
                 "valve tube subminiature 6N16B 6N16B-V")


def neutrik_nc3mah():
    fp = FP("Neutrik_NC3MAH", "Neutrik NC3MAH 3 pole male XLR receptacle, horizontal PCB mount, separate ground contact G "
            "(drawing ST-NC3MAH rev C); origin on the axis at the rear face of the panel, axis 12.5 above the board",
            "XLR male Neutrik NC3MAH")
    fp.prop("Reference", "REF**", 0, 21.0, "F.SilkS")
    fp.prop("Value", "NC3MAH", 0, 9.0, "F.Fab")
    fp.pad("1", "circle", 3.81, 17.78, 2.4, 2.4, 1.6)
    fp.pad("2", "circle", -3.81, 17.78, 2.4, 2.4, 1.6)
    fp.pad("3", "circle", 0, 17.78, 2.0, 2.0, 1.2)
    fp.pad("G", "circle", 0, 12.7, 2.0, 2.0, 1.2)
    fp.npth(-3.81, 3.81, 1.6)
    fp.npth(3.81, 8.89, 1.6)
    fp.poly([(-9.9, -2.7), (9.9, -2.7), (9.9, 18.6), (-9.9, 18.6)], "F.Fab", 0.1)
    fp.line(-12.0, 0, 12.0, 0, "F.Fab", 0.1)
    fp.text("panel rear face", 0, -1.4, "F.Fab", 0.7)
    for x in (-10.05, 10.05):
        fp.line(x, 0.3, x, 19.35, "F.SilkS", 0.15)
    fp.line(-10.05, 19.35, 10.05, 19.35, "F.SilkS", 0.15)
    fp.circle(6.6, 17.78, 0.25, "F.SilkS", 0.15, fill=True)
    fp.poly([(-10.15, -2.95), (10.15, -2.95), (10.15, 19.95), (-10.15, 19.95)], "F.CrtYd", 0.05)
    return fp


def transformer_space():
    W, H = 32.0, 32.0
    fp = FP("Transformer_Space_32x32", "Room for a 1:1 10K:10K line transformer up to 32 mm (E25/EI30 class); pads after the part is chosen and measured",
            "transformer placeholder", attr="through_hole")
    fp.prop("Reference", "REF**", 0, -H / 2 - 1.5, "F.SilkS")
    fp.prop("Value", "Transformer_Space_32x32", 0, 0, "F.Fab")
    fp.poly([(-W / 2, -H / 2), (W / 2, -H / 2), (W / 2, H / 2), (-W / 2, H / 2)], "F.Fab", 0.1)
    fp.text("T: pads after measuring", 0, 2.0, "F.Fab", 1.0)
    fp.poly([(-W / 2 - 0.25, -H / 2 - 0.25), (W / 2 + 0.25, -H / 2 - 0.25), (W / 2 + 0.25, H / 2 + 0.25), (-W / 2 - 0.25, H / 2 + 0.25)],
            "F.CrtYd", 0.05)
    return fp


def valve_6n28b_v1():
    # V1 only (the gain tube, left inner row): g1 (4) and the heater's ground end (8) bent short too, 3 mm in front of
    # the base level with 5, so K2_L runs up between the near row and these holes, G1_L leaves to the right and 8 meets
    # the ground parts there without a via
    return valve("Valve_6N28B-V_Flat_458", "6N28B-V", 35.0, {3: -5.08, 6: 2.54, 7: 5.08},
                 {2: -3.81, 1: -1.27, 9: 1.27}, 36.0, 36.0, (0, 21.0),
                 "6N28B-V double triode lying flat, bulb D10.2 x 35 (fig. 380), 9 wire leads D0.4 bent down: near 3 6 7, far 2 1 9, "
                 "4 (g1), 5 (screen) and 8 (heater) 3 mm in front of the base; colour mark (position 10) up; bulb in a Keystone 3566 clip 6..17 mm from the base",
                 "valve tube subminiature 6N28B 6N28B-V", apart={5: (3.0, 0.0), 4: (3.0, -2.54), 8: (3.0, 3.81)})


def valve_6n28b_v4():
    # V4 only (the right follower): every hole where the plain footprint has it, but a2 (6) and k2 (7) trade holes:
    # 7 in 6's place, 6 at the end of the near row, so the two leads cross (10-30)
    return valve("Valve_6N28B-V_Flat_67", "6N28B-V", 35.0, {3: -5.08, 4: -2.54, 7: 2.54, 6: 5.08},
                 {2: -3.81, 1: -1.27, 9: 1.27, 8: 3.81}, 36.0, 36.0, (0, 21.0),
                 "6N28B-V double triode lying flat, bulb D10.2 x 35 (fig. 380), 9 wire leads D0.4 bent down into two rows: "
                 "near 3 4 7 6 (6 and 7 cross), 5 (screen) 3 mm in front of the base, far 2 1 9 8; colour mark (position 10) up; bulb in a Keystone 3566 clip 6..17 mm from the base",
                 "valve tube subminiature 6N28B 6N28B-V", apart={5: (3.0, 0.0)})


LIBS = {"6n28b": [valve_6n28b, valve_6n28b_v1, valve_6n28b_v4, neutrik_nc3mah], "6n16b": [valve_6n16b, neutrik_nc3mah]}   # (transformer_space: unused since variant A)

if __name__ == "__main__":
    for var, makers in LIBS.items():
        d = os.path.join(HERE, "..", var, "jlr_tube.pretty")
        os.makedirs(d, exist_ok=True)
        for mk in makers:
            fp = mk()
            open(os.path.join(d, fp.name + ".kicad_mod"), "w", encoding="utf-8", newline="\n").write(fp.text_out())
            print(var, fp.name)
