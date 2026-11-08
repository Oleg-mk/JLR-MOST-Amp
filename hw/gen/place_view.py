"""Placement picture: the board seen from the back (the SMD and track side) or from the front, with the ratsnest.

python.exe place_view.py <board.kicad_pcb> <out.png> [front]
Back parts: blue courtyards; front parts: orange courtyards; ratsnest: thin lines per net (GND left out).
"""
import sys, math
import pcbnew
from PIL import Image, ImageDraw

pcb, png = sys.argv[1], sys.argv[2]
front = len(sys.argv) > 3 and sys.argv[3] == "front"
b = pcbnew.LoadBoard(pcb)
bb = b.GetBoardEdgesBoundingBox()
X0, Y0 = pcbnew.ToMM(bb.GetX()), pcbnew.ToMM(bb.GetY())
W, H = pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight())
k = 9
im = Image.new("RGB", (int((W + 4) * k), int((H + 4) * k)), "white")
d = ImageDraw.Draw(im)


def T(xmm, ymm):
    x = xmm - X0
    if not front:
        x = W - x
    return ((x + 2) * k, (ymm - Y0 + 2) * k)


d.rectangle([2 * k, 2 * k, (W + 2) * k, (H + 2) * k], outline="black", width=2)
for f in b.GetFootprints():
    back = f.IsFlipped()
    poly = f.GetCourtyard(pcbnew.B_CrtYd if back else pcbnew.F_CrtYd)
    col = (60, 90, 200) if back else (230, 140, 40)
    if poly.OutlineCount():
        r = poly.BBox()
        p1, p2 = T(pcbnew.ToMM(r.GetX()), pcbnew.ToMM(r.GetY())), T(pcbnew.ToMM(r.GetRight()), pcbnew.ToMM(r.GetBottom()))
        d.rectangle([min(p1[0], p2[0]), p1[1], max(p1[0], p2[0]), p2[1]], outline=col, width=1)
    for p in f.Pads():
        r = p.GetBoundingBox()
        p1, p2 = T(pcbnew.ToMM(r.GetX()), pcbnew.ToMM(r.GetY())), T(pcbnew.ToMM(r.GetRight()), pcbnew.ToMM(r.GetBottom()))
        th = p.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH)
        visible = th or (back != front)
        if not visible:
            continue
        fill = (120, 120, 120) if th else ((170, 215, 170) if p.GetNetname() == "GND" else (200, 200, 200))
        d.rectangle([min(p1[0], p2[0]), p1[1], max(p1[0], p2[0]), p2[1]], fill=fill)
    ref = f.GetReference()
    if ref[0] in "UJYSDQL" or ref in ("C50",):
        q = f.GetPosition()
        x, y = T(pcbnew.ToMM(q.x), pcbnew.ToMM(q.y))
        d.text((x - 8, y - 6), ref, fill=(0, 0, 0))

# ratsnest: minimum spanning tree per net
pads = {}
for f in b.GetFootprints():
    for p in f.Pads():
        n = p.GetNetname()
        if n and n != "GND" and not n.startswith("unconnected"):
            q = p.GetPosition()
            pads.setdefault(n, []).append((pcbnew.ToMM(q.x), pcbnew.ToMM(q.y)))
HL = ("BCK", "LRCK", "MCLK", "DOUT", "SPDIF", "MOST_SX3", "SCK", "FSY", "RMCK")
for n, pts in pads.items():
    if len(pts) < 2:
        continue
    base = n.split("/")[-1]
    col = (210, 40, 40) if base in HL else ((40, 150, 40) if base.startswith("+") else (150, 60, 160))
    inn, rest = [pts[0]], pts[1:]
    while rest:
        best = min(((math.hypot(a[0] - c[0], a[1] - c[1]), a, c) for a in inn for c in rest))
        _, a, c = best
        d.line([T(*a), T(*c)], fill=col, width=1)
        inn.append(c)
        rest.remove(c)
d.text((8, 4), "front view (foil side)" if front else "back view (SMD and tracks)", fill=(0, 0, 0))
im.save(png)
print("saved", png)
