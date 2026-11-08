"""Courtyard sizes of the footprints the schematic uses (run with KiCad's python)."""
import os, re, sys, glob
import pcbnew

HW = sys.argv[1] if len(sys.argv) > 1 else ".."
KFP = "C:/Users/geniy/AppData/Local/Programs/KiCad/10.0/share/kicad/footprints"

fps = {}
for f in glob.glob(os.path.join(HW, "*.kicad_sch")):
    s = open(f, encoding="utf-8").read()
    for m in re.finditer(r'\(symbol \(lib_id "([^"]+)"\).*?\(instances', s, re.S):
        blk = m.group(0)
        ref = re.search(r'\(property "Reference" "([^"]+)"', blk).group(1)
        if ref.startswith("#"):
            continue
        fp = re.search(r'\(property "Footprint" "([^"]*)"', blk).group(1)
        fps.setdefault(fp, set()).add(ref)

for fp, refs in sorted(fps.items()):
    lib, name = fp.split(":")
    path = os.path.join(HW, lib + ".pretty") if lib == "jlr_most_amp" else os.path.join(KFP, lib + ".pretty")
    m = pcbnew.FootprintLoad(path, name)
    bb = None
    for layer in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
        poly = m.GetCourtyard(layer)
        if poly.OutlineCount():
            b = poly.BBox()
            bb = b if bb is None else bb.Merge(b) or bb
    if bb is None:
        bb = m.GetBoundingBox(False)
    w, h = pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight())
    cx, cy = pcbnew.ToMM(bb.GetCenter().x), pcbnew.ToMM(bb.GetCenter().y)
    print("%-62s %5.1f x %5.1f  c(%5.1f,%5.1f)  n=%d  %s" % (fp, w, h, cx, cy, len(refs), ",".join(sorted(refs))[:60]))
