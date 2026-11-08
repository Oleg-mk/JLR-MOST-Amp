"""Move parts a little and drag the ends of the tracks that sit on their pads along (no re-routing).
    python.exe nudge.py <board.kicad_pcb> REF:dx:dy[:rot] ...
A track end on a moved pad moves with the pad; the rest of the track stays, so the last segment bends to follow."""
import sys
import pcbnew

PCB = sys.argv[1]
b = pcbnew.LoadBoard(PCB)
fps = {f.GetReference(): f for f in b.GetFootprints()}
tracks = [t for t in b.GetTracks()]
for arg in sys.argv[2:]:
    ref, dx, dy = arg.split(":")[:3]
    f = fps[ref]
    d = pcbnew.VECTOR2I(pcbnew.FromMM(float(dx)), pcbnew.FromMM(float(dy)))
    pads = [(p, p.GetNetCode()) for p in f.Pads()]
    ends = []                        # (track, which end) touching a pad of this part, same net
    for t in tracks:
        if t.GetClass() == "PCB_VIA":
            q = t.GetPosition()
            if any(p.HitTest(q) and t.GetNetCode() == n for p, n in pads):
                ends.append((t, "v"))
            continue
        for which, q in (("s", t.GetStart()), ("e", t.GetEnd())):
            if any(p.IsOnLayer(t.GetLayer()) and p.HitTest(q) and t.GetNetCode() == n for p, n in pads):
                ends.append((t, which))
    f.Move(d)
    for t, which in ends:
        if which == "v":
            t.SetPosition(t.GetPosition() + d)
        elif which == "s":
            t.SetStart(t.GetStart() + d)
        else:
            t.SetEnd(t.GetEnd() + d)
    print("%s moved %s,%s; %d track ends dragged" % (ref, dx, dy, len(ends)))
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(PCB)
