"""VRML models of the standoff hardware for the mounting-hole footprints of both boards, and the footprint patch that
attaches them.
    python hardware3d.py
Models (mm, written in KiCad's VRML unit of 0.1 inch): hex standoff M2.5 x 11, 5 mm across flats, bored 2.05 (the
thread's minor diameter), standing on z = 0; pan-head screw M2.5 x 6 (DIN 7985: head 5.0 x 2.0) with its head on
z = 0 and the shank along -z; small washer M2.5 (DIN 433: 5.0 x 2.7 x 0.5) on z = 0.
On hw4 the hole footprints are on B: the standoff stands on B, the washer and screw sit on F (rotated 180 about x,
offset by the board's 1.6 mm). On the tube board they are on F: the standoff hangs under B, the washer and screw on F.
"""
import math, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
U = 2.54                      # mm per VRML unit in KiCad


def poly(n, r, z, phase=0.0):
    return [(r * math.cos(phase + 2 * math.pi * k / n), r * math.sin(phase + 2 * math.pi * k / n), z) for k in range(n)]


class Mesh:
    def __init__(self):
        self.pts, self.faces = [], []

    def add(self, pts):
        base = len(self.pts)
        self.pts += pts
        return list(range(base, base + len(pts)))

    def tube(self, outer_bottom, outer_top):
        """side walls between two rings of equal length (indices)"""
        n = len(outer_bottom)
        for k in range(n):
            self.faces.append([outer_bottom[k], outer_bottom[(k + 1) % n], outer_top[(k + 1) % n], outer_top[k]])

    def ring_face(self, outer, inner, flip=False):
        """annulus between an outer and an inner ring of equal length"""
        n = len(outer)
        for k in range(n):
            f = [outer[k], outer[(k + 1) % n], inner[(k + 1) % n], inner[k]]
            self.faces.append(f[::-1] if flip else f)

    def disk(self, ring, centre, flip=False):
        n = len(ring)
        for k in range(n):
            f = [centre, ring[(k + 1) % n], ring[k]]
            self.faces.append(f[::-1] if flip else f)

    def wrl(self, color):
        s = ["#VRML V2.0 utf8", "Shape {", "  appearance Appearance { material Material {",
             "    diffuseColor %.2f %.2f %.2f specularColor 0.6 0.6 0.6 shininess 0.5 } }" % color,
             "  geometry IndexedFaceSet { solid FALSE coord Coordinate { point ["]
        s += ["    %.4f %.4f %.4f," % (x / U, y / U, z / U) for (x, y, z) in self.pts]
        s += ["  ] } coordIndex ["]
        s += ["    " + " ".join(str(i) for i in f) + " -1," for f in self.faces]
        s += ["  ] }", "}"]
        return "\n".join(s) + "\n"


def standoff(L=11.0, af=5.0, bore=2.05):
    m = Mesh()
    R = af / math.sqrt(3)              # hex circumradius
    hb, ht = m.add(poly(6, R, 0, math.pi / 6)), m.add(poly(6, R, L, math.pi / 6))
    bb, bt = m.add(poly(6, bore / 2, 0, math.pi / 6)), m.add(poly(6, bore / 2, L, math.pi / 6))
    m.tube(hb, ht)
    m.ring_face(hb, bb, flip=True)
    m.ring_face(ht, bt)
    m.tube(bt, bb)                     # the bore's wall
    return m


def screw(d=2.5, L=6.0, dk=5.0, k=2.0, n=24):
    m = Mesh()
    r, rk = d / 2, dk / 2
    sh_top, sh_bot = m.add(poly(n, r, 0)), m.add(poly(n, r, -L))
    m.tube(sh_bot, sh_top)
    c = m.add([(0, 0, -L)])[0]
    m.disk(sh_bot, c, flip=True)
    hd_bot = m.add(poly(n, rk, 0))
    hd_mid = m.add(poly(n, rk, k * 0.6))
    hd_top = m.add(poly(n, rk * 0.72, k))
    m.ring_face(hd_bot, sh_top, flip=True)
    m.tube(hd_bot, hd_mid)
    m.tube(hd_mid, hd_top)
    ct = m.add([(0, 0, k)])[0]
    # a cross recess: four slots drawn as a sunken plus (0.6 wide, 2.8 long, 0.5 deep)
    m.disk(hd_top, ct)
    return m


def washer(do=5.0, di=2.7, t=0.5, n=24):
    m = Mesh()
    ob, ot = m.add(poly(n, do / 2, 0)), m.add(poly(n, do / 2, t))
    ib, it = m.add(poly(n, di / 2, 0)), m.add(poly(n, di / 2, t))
    m.tube(ob, ot)
    m.ring_face(ob, ib, flip=True)
    m.ring_face(ot, it)
    m.tube(it, ib)
    return m


MODELS = {"Standoff_M2.5x11_SW5": (standoff(), (0.78, 0.78, 0.80)),
          "Screw_M2.5x6_PanHead": (screw(), (0.70, 0.72, 0.75)),
          "Washer_M2.5_DIN433": (washer(), (0.75, 0.75, 0.78))}
# model entries per footprint side: (file, offset z mm, rotate x deg)
ON_B = [("Standoff_M2.5x11_SW5", 0.0, 0), ("Washer_M2.5_DIN433", -1.6, 180), ("Screw_M2.5x6_PanHead", -2.1, 180)]
ON_F = [("Standoff_M2.5x11_SW5", -1.6, 180), ("Washer_M2.5_DIN433", 0.0, 0), ("Screw_M2.5x6_PanHead", 0.5, 0)]


def attach(mod_path, shapes_var, entries):
    s = open(mod_path, encoding="utf-8").read()
    s = re.sub(r'\n\t\(model "[^"]*"(?:.|\n)*?\n\t\)', "", s)          # old model entries out
    text = ""
    for name, z, rx in entries:
        text += ('\n\t(model "%s/%s.wrl"\n\t\t(offset (xyz 0 0 %g))\n\t\t(scale (xyz 1 1 1))\n\t\t(rotate (xyz %d 0 0))\n\t)'
                 % (shapes_var, name, z, rx))
    i = s.rstrip().rfind(")")
    s = s[:i].rstrip() + text + "\n)\n"
    open(mod_path, "w", encoding="utf-8", newline="\n").write(s)


if __name__ == "__main__":
    targets = [(os.path.join(HERE, "..", "..", "hw4", "jlr_most_amp.3dshapes"),
                os.path.join(HERE, "..", "..", "hw4", "jlr_most_amp.pretty", "MountingHole_2.7mm_M2.5_Standoff.kicad_mod"),
                "${KIPRJMOD}/jlr_most_amp.3dshapes", ON_B)]
    for var in ("6n28b", "6n16b"):
        targets.append((os.path.join(HERE, "..", var, "jlr_tube.3dshapes"),
                        os.path.join(HERE, "..", var, "jlr_tube.pretty", "MountingHole_2.7mm_M2.5_Standoff.kicad_mod"),
                        "${KIPRJMOD}/jlr_tube.3dshapes", ON_F))
    for shapes_dir, mod, var, entries in targets:
        os.makedirs(shapes_dir, exist_ok=True)
        for name, (mesh, color) in MODELS.items():
            open(os.path.join(shapes_dir, name + ".wrl"), "w", encoding="utf-8", newline="\n").write(mesh.wrl(color))
        attach(mod, var, entries)
        print("models in", os.path.relpath(shapes_dir, HERE), "attached to", os.path.basename(mod))
