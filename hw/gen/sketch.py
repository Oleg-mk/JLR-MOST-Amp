"""Light self-check picture of the placement: courtyards and references (not for the user)."""
import sys
import pcbnew
from PIL import Image, ImageDraw

pcb, png = sys.argv[1], sys.argv[2]
b = pcbnew.LoadBoard(pcb)
k = 8  # px per mm
X0, Y0, W, H = 100.0, 100.0, 130.0, 80.0
im = Image.new("RGB", (int((W + 10) * k), int((H + 10) * k)), "white")
d = ImageDraw.Draw(im)
T = lambda x, y: ((x - X0 + 5) * k, (y - Y0 + 5) * k)
d.rectangle([T(X0, Y0), T(X0 + W, Y0 + H)], outline="black", width=2)
for f in b.GetFootprints():
    poly = f.GetCourtyard(pcbnew.F_CrtYd)
    if poly.OutlineCount():
        bb = poly.BBox()
        x1, y1 = pcbnew.ToMM(bb.GetX()), pcbnew.ToMM(bb.GetY())
        x2, y2 = pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom())
        ref = f.GetReference()
        col = "red" if ref[0] in "UJ" else ("blue" if ref[0] in "CL" else "green")
        d.rectangle([T(x1, y1), T(x2, y2)], outline=col, width=1)
        d.text(T(x1 + 0.2, y1 + 0.1), ref, fill=col)
    for p in f.Pads():
        q = p.GetPosition()
        x, y = pcbnew.ToMM(q.x), pcbnew.ToMM(q.y)
        d.point(T(x, y), fill="black")
im.save(png)
