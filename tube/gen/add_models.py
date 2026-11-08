"""Attach the standoff hardware models to the H1..H5 footprints of a saved board (its library footprint got them
later than the board was made).
    python.exe add_models.py <board.kicad_pcb> <shapes var, e.g. ${KIPRJMOD}/jlr_tube.3dshapes> <F|B>
F: the holes' footprints are on F (tube board): the standoff hangs under B, washer and screw on F.
B: the footprints are on B (hw4): the standoff stands on B, washer and screw on F."""
import sys, pcbnew

PCB, VAR, SIDE = sys.argv[1], sys.argv[2], sys.argv[3]
ON_B = [("Standoff_M2.5x11_SW5", 0.0, 0), ("Washer_M2.5_DIN433", -1.6, 180), ("Screw_M2.5x6_PanHead", -2.1, 180)]
ON_F = [("Standoff_M2.5x11_SW5", -1.6, 180), ("Washer_M2.5_DIN433", 0.0, 0), ("Screw_M2.5x6_PanHead", 0.5, 0)]
b = pcbnew.LoadBoard(PCB)
n = 0
for f in b.GetFootprints():
    if not f.GetReference().startswith("H"):
        continue
    models = f.Models()
    models.clear()
    for name, z, rx in (ON_B if SIDE == "B" else ON_F):
        m = pcbnew.FP_3DMODEL()
        m.m_Filename = "%s/%s.wrl" % (VAR, name)
        m.m_Offset = pcbnew.VECTOR3D(0, 0, z)
        m.m_Scale = pcbnew.VECTOR3D(1, 1, 1)
        m.m_Rotation = pcbnew.VECTOR3D(rx, 0, 0)
        m.m_Show = True
        models.push_back(m)
    n += 1
b.Save(PCB)
print("models set on %d hole footprints" % n)
