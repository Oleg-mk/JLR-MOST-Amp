"""Before routing: no tracks under the ESP32 module, no bottom tracks where the ground must stay whole, Specctra export.

python.exe route_prep.py <board.kicad_pcb> <out.dsn>
"""
import sys
import pcbnew

PCB, DSN = sys.argv[1], sys.argv[2]
MM = pcbnew.FromMM
b = pcbnew.LoadBoard(PCB)
fp = {f.GetReference(): f for f in b.GetFootprints()}
edge = b.GetBoardEdgesBoundingBox()
X0, Y0 = pcbnew.ToMM(edge.GetX()), pcbnew.ToMM(edge.GetY())

# bottom copper stays ground under these (board mm): the DAC's analogue side and output filters, the crystal,
# the OS8104A PLL filter, the buck's switching loop
KEEP_B = {
    "gnd_dac_analog": (99.5, 40.5, 114.5, 60.5),
    "gnd_crystal": (47.0, 30.0, 54.5, 39.5),
    "gnd_pll": (51.0, 19.0, 60.5, 27.0),
    "gnd_buck": (42.0, 52.0, 66.5, 61.0),
}


def rule_area(name, layer, pts):
    z = pcbnew.ZONE(b)
    z.SetIsRuleArea(True)
    z.SetZoneName(name)
    z.SetDoNotAllowTracks(True)
    z.SetDoNotAllowVias(False)
    z.SetDoNotAllowPads(False)
    z.SetDoNotAllowZoneFills(False)
    z.SetDoNotAllowFootprints(False)
    z.SetLayer(layer)
    ol = z.Outline()
    ol.NewOutline()
    for (x, y) in pts:
        ol.Append(MM(x), MM(y))
    b.Add(z)


names = {"no_tracks_under_U2"} | set(KEEP_B)
for z in list(b.Zones()):
    if z.GetIsRuleArea() and z.GetZoneName() in names:
        b.Delete(z)

# no tracks under the ESP32 module body (between its pad rows, below the antenna end)
f = fp["U2"]
cx, cy = pcbnew.ToMM(f.GetPosition().x), pcbnew.ToMM(f.GetPosition().y)
rule_area("no_tracks_under_U2", pcbnew.F_Cu,
          [(cx + x, cy + y) for (x, y) in ((-7.8, -9.3), (7.8, -9.3), (7.8, 15.6), (-7.8, 15.6))])
for name, (x1, y1, x2, y2) in KEEP_B.items():
    rule_area(name, pcbnew.B_Cu, [(X0 + x1, Y0 + y1), (X0 + x2, Y0 + y1), (X0 + x2, Y0 + y2), (X0 + x1, Y0 + y2)])

pcbnew.SaveBoard(PCB, b)
ok = pcbnew.ExportSpecctraDSN(b, DSN)
print("vias:", sum(1 for t in b.GetTracks() if t.GetClass() == "PCB_VIA"), "dsn:", ok)
