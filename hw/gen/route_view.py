"""Picture of a routed board for checking by eye: top tracks red, bottom blue, vias black.

python.exe route_view.py <board.kicad_pcb> <out.png> [back] [net-substring ...]   (named nets drawn thick)
"""
import sys
import pcbnew
from PIL import Image, ImageDraw

pcb, png, hl = sys.argv[1], sys.argv[2], sys.argv[3:]
BACKV = bool(hl) and hl[0] == "back"
if BACKV:
    hl = hl[1:]
b = pcbnew.LoadBoard(pcb)
k = 9  # px per mm
bb = b.GetBoardEdgesBoundingBox()
X0, Y0 = pcbnew.ToMM(bb.GetX()), pcbnew.ToMM(bb.GetY())
W, H = pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight())
im = Image.new("RGB", (int((W + 4) * k), int((H + 4) * k)), "white")
d = ImageDraw.Draw(im)
T = lambda x, y: ((((W - (x - X0)) if BACKV else (x - X0)) + 2) * k, (y - Y0 + 2) * k)
d.rectangle([2 * k, 2 * k, (W + 2) * k, (H + 2) * k], outline="black", width=2)
for f in b.GetFootprints():
    for p in f.Pads():
        r = p.GetBoundingBox()
        a1, a2 = T(pcbnew.ToMM(r.GetX()), pcbnew.ToMM(r.GetY())), T(pcbnew.ToMM(r.GetRight()), pcbnew.ToMM(r.GetBottom()))
        d.rectangle([min(a1[0], a2[0]), a1[1], max(a1[0], a2[0]), a2[1]],
                    fill=(200, 200, 200) if p.GetNetname() != "GND" else (170, 220, 170))
for layer, col, colh in ((pcbnew.F_Cu, (230, 120, 120), (200, 0, 0)), (pcbnew.B_Cu, (110, 140, 255), (0, 0, 200))):
    for t in b.GetTracks():
        if t.GetClass() != "PCB_TRACK" or t.GetLayer() != layer:
            continue
        s, e = t.GetStart(), t.GetEnd()
        thick = any(h in t.GetNetname() for h in hl)
        w = max(1, int(pcbnew.ToMM(t.GetWidth()) * k)) + (3 if thick else 0)
        d.line([T(pcbnew.ToMM(s.x), pcbnew.ToMM(s.y)), T(pcbnew.ToMM(e.x), pcbnew.ToMM(e.y))],
               fill=colh if thick else col, width=w)
for t in b.GetTracks():
    if t.GetClass() == "PCB_VIA":
        x, y = pcbnew.ToMM(t.GetPosition().x), pcbnew.ToMM(t.GetPosition().y)
        r = 0.3 * k
        d.ellipse([x * 0 + T(x, y)[0] - r, T(x, y)[1] - r, T(x, y)[0] + r, T(x, y)[1] + r],
                  outline="black", fill=(0, 150, 0) if t.GetNetname() == "GND" else "black")
for f in b.GetFootprints():
    ref = f.GetReference()
    if ref[0] in "UJYSDQL":
        q = f.GetPosition()
        d.text(T(pcbnew.ToMM(q.x) - 1, pcbnew.ToMM(q.y) - 1), ref, fill=(0, 0, 0))
im.save(png)
print("saved", png)
