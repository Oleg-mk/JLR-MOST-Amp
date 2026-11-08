"""A GND via beside every top SMD pad on GND, with a short track; clearance-checked.

python.exe gnd_fanout.py <board.kicad_pcb>
"""
import math, sys
import pcbnew

PCB = sys.argv[1]
MM = pcbnew.FromMM
b = pcbnew.LoadBoard(PCB)
gnd = b.FindNet("GND")
VIA_D, VIA_H, CLR, TW = 0.6, 0.3, 0.2, 0.4
TCLR = 0.19

# remove earlier fanout and exposed-pad vias
for t in list(b.GetTracks()):
    if t.GetNetname() == "GND" or t.GetClass() == "PCB_VIA":
        b.Delete(t)
fpd = {f.GetReference(): f for f in b.GetFootprints()}


def add_via(pos):
    v = pcbnew.PCB_VIA(b)
    v.SetPosition(pos)
    v.SetWidth(MM(VIA_D))
    v.SetDrill(MM(VIA_H))
    v.SetNet(gnd)
    v.SetLocked(True)
    b.Add(v)


# exposed pads: vias inside the pad copper
EP = {"U2": "39", "U8": "9", "U7": "7", "U9": "7", "U10": "9"}
for ref, num in EP.items():
    sub = [p for p in fpd[ref].Pads() if p.GetNumber() == num]
    if ref == "U2":
        xs = sorted(set(p.GetPosition().x for p in sub)); ys = sorted(set(p.GetPosition().y for p in sub))
        for p in sub:
            q = p.GetPosition()
            if q.x in (xs[0], xs[-1]) and q.y in (ys[0], ys[-1]):
                add_via(q)
    elif ref == "U8":
        q = sub[0].GetPosition()
        for dx in (-0.6, 0.6):
            for dy in (-0.9, 0.9):
                add_via(q + pcbnew.VECTOR2I(MM(dx), MM(dy)))
    else:
        add_via(sub[0].GetPosition())

obst = []  # (x, y, r, net) circles for pads of other nets and vias
pads = []
for f in b.GetFootprints():
    for p in f.Pads():
        bb = p.GetBoundingBox()
        pads.append((p, f))
        r = 0.5 * math.hypot(pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight()))
        q = p.GetPosition()
        obst.append((pcbnew.ToMM(q.x), pcbnew.ToMM(q.y), r, p.GetNetname(), p))
vias = [(pcbnew.ToMM(t.GetPosition().x), pcbnew.ToMM(t.GetPosition().y)) for t in b.GetTracks() if t.GetClass() == "PCB_VIA"]
keep = [z for z in b.Zones() if z.GetIsRuleArea()]
edge = b.GetBoardEdgesBoundingBox()
ex1, ey1 = pcbnew.ToMM(edge.GetX()) + 0.6, pcbnew.ToMM(edge.GetY()) + 0.6
ex2, ey2 = pcbnew.ToMM(edge.GetRight()) - 0.6, pcbnew.ToMM(edge.GetBottom()) - 0.6


def pad_clear(x, y, rad):
    """True if a circle at (x, y) of radius rad keeps CLR from every non-GND pad"""
    pt = pcbnew.VECTOR2I(MM(x), MM(y))
    for (px, py, r, net, p) in obst:
        if net == "GND" or abs(px - x) > r + rad + CLR + 0.5 or abs(py - y) > r + rad + CLR + 0.5:
            continue
        sh = p.GetEffectivePolygon(pcbnew.F_Cu) if p.IsOnLayer(pcbnew.F_Cu) else p.GetEffectivePolygon(pcbnew.B_Cu)
        d = pcbnew.ToMM(int(math.sqrt(max(0, sh.SquaredDistance(pt, False)))))
        if d < rad + CLR:
            return False
    return True


def ok_via(x, y):
    if not (ex1 < x < ex2 and ey1 < y < ey2):
        return False
    for (vx, vy) in vias:
        if math.hypot(vx - x, vy - y) < VIA_D + 0.25:
            return False
    pt = pcbnew.VECTOR2I(MM(x), MM(y))
    for z in keep:
        if z.GetDoNotAllowVias() and z.Outline().Contains(pt):
            return False
    return pad_clear(x, y, VIA_D / 2)


def ok_track(x1, y1, x2, y2, tw):
    n = max(2, int(math.hypot(x2 - x1, y2 - y1) / 0.1))
    for i in range(n + 1):
        t = i / n
        if not pad_clear(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t, tw / 2 + (TCLR - CLR)):
            return False
    return True


added = 0
failed = []
for p, f in pads:
    if p.GetNetname() != "GND" or p.GetAttribute() != pcbnew.PAD_ATTRIB_SMD or not p.IsOnLayer(pcbnew.F_Cu):
        continue
    if EP.get(f.GetReference()) == p.GetNumber():
        continue
    # exposed pads that already have vias
    q = p.GetPosition()
    px, py = pcbnew.ToMM(q.x), pcbnew.ToMM(q.y)
    if any(math.hypot(vx - px, vy - py) < 2.0 for (vx, vy) in vias) and pcbnew.ToMM(p.GetBoundingBox().GetWidth()) > 0.9 \
            and pcbnew.ToMM(p.GetBoundingBox().GetHeight()) > 0.9:
        continue
    c = f.GetPosition()
    cx, cy = pcbnew.ToMM(c.x), pcbnew.ToMM(c.y)
    bb = p.GetBoundingBox()
    w, h = pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight())
    dx, dy = px - cx, py - cy
    if w > 1.2 * h:
        dirs = [(1 if dx > 0 else -1, 0)]
    elif h > 1.2 * w:
        dirs = [(0, 1 if dy > 0 else -1)]
    else:
        dirs = [((1 if dx > 0 else -1), 0) if abs(dx) >= abs(dy) else (0, (1 if dy > 0 else -1))]
    d0 = dirs[0]
    dirs += [(-d0[1], d0[0]), (d0[1], -d0[0]), (-d0[0], -d0[1])]
    tw = round(min(TW, 0.9 * min(w, h)), 2)
    done = False
    for (ux, uy) in dirs:
        ext = (w / 2 if ux else h / 2)
        for dist in (ext + 0.75, ext + 1.0, ext + 1.3, ext + 1.7, ext + 2.2):
            for lat in (0, 0.4, -0.4, 0.8, -0.8):
                vx, vy = px + ux * dist - uy * lat, py + uy * dist + ux * lat
                if ok_via(vx, vy) and ok_track(px, py, vx, vy, tw):
                    v = pcbnew.PCB_VIA(b)
                    v.SetPosition(pcbnew.VECTOR2I(MM(vx), MM(vy)))
                    v.SetWidth(MM(VIA_D))
                    v.SetDrill(MM(VIA_H))
                    v.SetNet(gnd)
                    b.Add(v)
                    t = pcbnew.PCB_TRACK(b)
                    t.SetStart(q)
                    t.SetEnd(pcbnew.VECTOR2I(MM(vx), MM(vy)))
                    t.SetWidth(MM(tw))
                    t.SetLayer(pcbnew.F_Cu)
                    t.SetNet(gnd)
                    b.Add(t)
                    vias.append((vx, vy))
                    added += 1
                    done = True
                    break
            if done:
                break
        if done:
            break
    if not done:
        failed.append("%s.%s" % (f.GetReference(), p.GetNumber()))

pcbnew.SaveBoard(PCB, b)
print("GND vias added:", added, "no room:", " ".join(failed) if failed else "none")
