"""Import a Specctra session into the board, refill the zones, save under a new name.

python.exe route_import.py <board.kicad_pcb> <session.ses> <out.kicad_pcb>
"""
import sys
import pcbnew

PCB, SES, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
b = pcbnew.LoadBoard(PCB)
ok = pcbnew.ImportSpecctraSES(b, SES)
f = pcbnew.ZONE_FILLER(b)
f.Fill(b.Zones())
pcbnew.SaveBoard(OUT, b)
tr = [t for t in b.GetTracks()]
vias = [t for t in tr if t.GetClass() == "PCB_VIA"]
segs = [t for t in tr if t.GetClass() == "PCB_TRACK"]
bot = [t for t in segs if t.GetLayer() == pcbnew.B_Cu]
print("import:", ok, "segments:", len(segs), "on B.Cu:", len(bot), "vias:", len(vias),
      "B.Cu length mm: %.1f" % sum(pcbnew.ToMM(t.GetLength()) for t in bot))
