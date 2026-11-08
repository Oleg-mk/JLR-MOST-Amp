"""JLR-MOST-Amp board, two-sided assembly.

The front copper is a solid ground foil and carries the bodies of the FOT, TOSLINK, the three RCA jacks and the
HDMI socket. Every other part sits on the back, and the tracks run there.

Back parts are placed in back-view coordinates: the board seen from the back, x mirrored against KiCad's
front view, so pin order reads as on the parts' datasheets. Front parts are placed in front-view coordinates.

Run with KiCad's python:  python.exe gen_pcb.py <netlist.net> <out.kicad_pcb>
Routing is not done here.
"""
import os, re, sys, math
import pcbnew
import netclasses

NET, OUT = sys.argv[1], sys.argv[2]
HW = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KFP = "C:/Users/geniy/AppData/Local/Programs/KiCad/10.0/share/kicad/footprints"
MM = pcbnew.FromMM

# coordinates (mm) start at (X0, Y0); the outline runs from the top of the coax RCA body down to the bottom of the
# HDMI body (the user, 2026-10-15)
X0, Y0, W = 100.0, 100.0, 130.0
TOP, BOT = 1.67, 60.30
H = BOT

# ---------------------------------------------------------------- netlist
s = open(NET, encoding="utf-8").read()
comps = {}
ci, cj = s.index("(components"), s.index("(libparts")
for body in re.split(r"\(comp\s*\(ref ", s[ci:cj])[1:]:
    ref = re.match(r'\s*"([^"]+)"', body).group(1)
    val = re.search(r'\(value "([^"]*)"\)', body).group(1)
    fp = re.search(r'\(footprint "([^"]*)"\)', body).group(1)
    sheet = re.search(r'\(sheetpath\s*\(names "([^"]*)"\)\s*\(tstamps "([^"]*)"\)', body)
    ts = re.search(r'\(tstamps "([^"]+)"', body[body.find(")", body.find("(sheetpath")):]).group(1)
    sfile = re.search(r'\(name "Sheetfile"\)\s*\(value "([^"]*)"\)', body)
    sname = re.search(r'\(name "Sheetname"\)\s*\(value "([^"]*)"\)', body)
    comps[ref] = dict(value=val, fp=fp, path=sheet.group(2) + ts,
                      sheetfile=sfile.group(1) if sfile else "", sheetname=sname.group(1) if sname else "")
nets = {}
i = s.index("(nets")
for blk in re.split(r"\(net\s*\(code", s[i:])[1:]:
    name = re.search(r'\(name "([^"]*)"\)', blk).group(1)
    nets[name] = [(n.group(1), n.group(2)) for n in re.finditer(r'\(node\s*\(ref "([^"]+)"\)\s*\(pin "([^"]+)"\)', blk)]

# ---------------------------------------------------------------- placement
# Front, the foil side (front view, KiCad orientation): connectors that face the enclosure.
FRONT = {
    "U5": (8.86, 6.68, 270),    # TOSLINK, port out of the FOT edge, top
    "J2": (11.8, 24.5, 90),     # FOT, port out of the FOT edge
    "J6": (117.0, 9.22, 90),   # RCA S/PDIF coax, opposite edge, top
    "J4": (117.13, 26.52, 90),  # RCA L
    "J5": (117.15, 42.0, 90),   # RCA R
    "J7": (119.105, 55.341, 270),   # HDMI I2S, below the jacks; body front in line with the RCAs (user, 2026-10-15),
                                    # shield tabs 0.5 off the bottom edge
}

# Back (back view: x, y, rotation counter-clockwise as seen from the back).
BACK = {
    # signal chain along the top: FOT -> OS8104A -> ESP32 -> PCM5122 -> jacks
    "U1": (97.0, 22.0, 180),    # FOT corner (TX, RX) faces J2; the ESP32 lines leave the south and west sides
    "Y1": (92.5, 10.0, 90),     # above XTI/XTO
    "U2": (61.2, 20.3, 90),     # ESP32 lying, antenna end towards the DAC
    "U3": (33.0, 33.0, 0),      # PCM5122, analogue side towards the jacks (SLAS763C fig. 85)
    "U9": (31.5, 42.5, 90),     # AVDD/CPVDD LDO under the DAC, output up
    "R8": (96.091, 29.642, 0),     # SCL pull-up under U1 pins 38 (+3V3) and 39 (user, 2026-10-15)
    "R9": (98.8, 32.662, 90), "R10": (102.1, 30.862, 90),   # as they stood when the user moved R8
    "U4": (18.723, 6.065, 270),    # coax driver beside J6 (user, 2026-10-15, with C41 C42 R34 R35)
    "C41": (19.51, 9.868, 270), "C42": (16.31, 9.88, 90), "R34": (17.91, 9.88, 270), "R35": (18.748, 12.293, 0),
    "SW1": (51.97, 57.53, 90),  # mode switch under the ESP32 inputs, keys out of the bottom edge
    "J3": (36.73, 3.61, 90),    # programming header on the top edge (the user)
    "D20": (62.79, 6.78, 0),    # LED on the top edge (the user)
    # power along the bottom, connector low on the FOT edge (moved up by the user, 2026-10-15)
    "J1": (123.16, 55.88, 90),
    "F1": (117.5, 55.88, 180),  # input fuse right after J1, then 100 nF ceramic (the user, 2026-10-15)
    "C64": (114.2, 53.9, 90),
    "D1": (107.08, 55.68, 0),
    "D2": (105.58, 49.68, 90),
    "L1": (99.08, 55.68, 180),
    "C50": (88.58, 55.68, 180),
    "U10": (74.42, 49.87, 0),
    "R57": (69.67, 48.90, 0), "R58": (69.67, 50.85, 180), "R59": (79.17, 50.85, 180), "C63": (79.47, 48.90, 0),
    "U7": (114.0, 45.0, 90),
    "Q1": (116.5, 37.5, 0),
    "D3": (106.0, 41.0, 0),
    "U8": (76.29, 38.86, 180),  # AP64200Q under the ESP32, DS43577 fig. 34 turned: VIN side east, SW side west
    "L2": (69.29, 41.36, 180),
    "R53": (81.87, 35.96, 0), "R54": (67.52, 36.96, 0), "R55": (70.72, 36.96, 180), "R56": (71.22, 35.23, 180),
    "C54": (64.52, 38.23, 0),
    # OS8104A: crystal, source-port resistors, decoupling
    "C11": (95.2, 10.0, 90), "C12": (89.2, 8.9, 180), "C6": (96.2, 13.6, 90),
    "R16": (98.6, 13.0, 180), "R15": (99.4, 10.6, 90), "R14": (101.0, 10.6, 90), "R13": (104.0, 12.6, 90),
    "C5": (105.3, 22.8, 0), "C7": (105.6, 20.8, 0), "R5": (105.3, 24.6, 0),
    "R1": (107.8, 34.05, 180), "R2": (104.6, 34.05, 180), "R11": (100.6, 30.6, 90), "R17": (92.662, 29.616, 180),   # (user, 2026-10-15)
    # FOT supplies and gain, behind the FOT body
    "R4": (113.6, 21.3, 270), "C3": (116.2, 20.0, 0), "C4": (116.2, 22.1, 0), "FB2": (120.5, 20.0, 180),
    "C1": (116.0, 26.4, 0), "C2": (116.0, 28.2, 0), "FB1": (119.5, 26.4, 180), "R3": (116.0, 31.5, 180),
    # PCM5122 analogue side (SLAS763C fig. 85): charge pump and CPVDD on top, outputs west, AVDD below
    # (the flying cap's lower pad stays above pin 5, so VNEG runs straight under it and OUTL keeps its row)
    "C36": (27.8, 29.6, 270), "C37": (25.0, 30.9, 180), "C31": (27.8, 27.0, 180), "C33": (27.6, 33.9, 180),
    "C30": (24.6, 27.2, 180), "C32": (24.0, 35.8, 180),
    "R32": (21.4, 32.0, 180), "C39": (15.6, 28.2, 270), "R33": (21.4, 33.6, 180), "C40": (15.6, 40.2, 90),
    "C62": (28.4, 41.6, 0), "C61": (28.4, 43.6, 0),
    # digital side: DVDD and LDOO decoupling and the XSMT pull-up above the I2S inputs, MODE2 pull-up below,
    # so the four I2S lines come in through a clear corridor
    "C35": (38.6, 28.0, 0), "C34": (38.9, 26.0, 0), "C38": (38.6, 29.6, 0), "R31": (42.3, 28.6, 180),
    "R30": (40.6, 37.4, 180),
    "R26": (59.3, 6.8, 0),      # LED resistor beside the LED, above the clock taps
    # AP64200Q: input caps across VIN-GND, bootstrap under, inductor at SW, output caps after it
    "C57": (80.89, 39.46, 90), "C55": (83.39, 39.46, 90), "C56": (86.79, 39.46, 90), "C53": (76.29, 42.86, 180),
    "C58": (65.29, 41.36, 90), "C59": (62.89, 41.36, 90),
}
# the LVDS driver and its supply cap keep their place against the HDMI connector (hdmi_breakout.py is drawn for it):
# U6 7.1 mm in from J7 pin 1 and 9.0 mm up, C44 11.6 mm in and 10.0 mm up (back view)
J7X, J7Y = W - FRONT["J7"][0], FRONT["J7"][1]
BACK["U6"] = (round(J7X + 7.1, 3), round(J7Y - 9.0, 3), 270)
BACK["C44"] = (round(J7X + 11.6, 3), round(J7Y - 10.0, 3), 0)

# Small parts beside one pin (back view): anchor part, anchor pad, distance from the pad, lateral shift,
# pad of the part that faces the anchor.
NEAR = {
    "C8": ("U1", "29", 2.6, 0, "1"), "C9": ("U1", "30", 4.5, 0, "1"), "C10": ("U1", "29", 6.6, 0, "1"),
    "FB3": ("U1", "30", 8.6, 0, "2"), "R6": ("U1", "26", 2.6, 0, "1"), "C13": ("U1", "26", 4.7, 0, "1"),
    "R7": ("U1", "25", 2.6, 0, "1"), "R12": ("U1", "31", 2.6, 0, "2"), "C14": ("U1", "31", 4.7, 0, "1"),
    "C21": ("U2", "2", 2.6, 0, "1"), "C20": ("U2", "2", 4.8, 0, "1"), "R20": ("U2", "3", 2.6, 0, "2"),
    "C22": ("U2", "3", 4.8, 0, "1"), "R21": ("U2", "25", 2.6, 0, "2"),
    "R22": ("U2", "7", 3.2, 0, "2"), "R23": ("U2", "6", 3.2, 0, "2"), "R24": ("U2", "5", 3.2, 0, "2"),
    "R25": ("U2", "4", 3.2, 0, "2"),
    "R27": ("U3", "11", 3.0, 0, "2"), "R28": ("U3", "12", 3.0, 0, "2"),
    "D30": ("U5", "2", 4.0, 0, "1"), "C43": ("U5", "2", 6.5, 0, "1"),
    "C51": ("U7", "1", 2.4, 0, "1"), "C52": ("U7", "6", 2.4, 0, "1"),
    "R50": ("D3", "3", 2.6, 0, "1"), "R51": ("D3", "1", 2.6, 0, "1"), "R52": ("D3", "2", 2.6, 0, "1"),
}
# pads whose outward direction is not their elongation (back view): THT pins at a footprint's origin
OUT_DIR = {"J6": (1, 0), "U5": (-1, 0)}

# ---------------------------------------------------------------- build
board = pcbnew.BOARD()
board.SetCopperLayerCount(2)
ds = board.GetDesignSettings()
ds.m_MinClearance = MM(0.15)
ds.m_TrackMinWidth = MM(0.15)
ds.m_MinThroughDrill = MM(0.3)
ds.m_ViasMinSize = MM(0.6)
nc = ds.m_NetSettings.GetDefaultNetclass()
nc.SetClearance(MM(0.15))
nc.SetTrackWidth(MM(0.25))
nc.SetViaDiameter(MM(0.6))
nc.SetViaDrill(MM(0.3))
netinfo = {}
for name in nets:
    ni = pcbnew.NETINFO_ITEM(board, name)
    board.Add(ni)
    netinfo[name] = ni
pad_net = {}
for name, nodes in nets.items():
    for ref, pin in nodes:
        pad_net[(ref, pin)] = name

fps, native = {}, {}
for ref, c in comps.items():
    lib, name = c["fp"].split(":")
    path = os.path.join(HW, lib + ".pretty") if lib == "jlr_most_amp" else os.path.join(KFP, lib + ".pretty")
    f = pcbnew.FootprintLoad(path, name)
    f.SetFPID(pcbnew.LIB_ID(lib, name))
    f.SetReference(ref)
    f.SetValue(c["value"])
    f.SetPath(pcbnew.KIID_PATH(c["path"]))
    try:
        f.SetSheetfile(c["sheetfile"])
        f.SetSheetname(c["sheetname"])
    except AttributeError:
        pass
    for pad in f.Pads():
        n = pad_net.get((ref, pad.GetNumber()))
        if n:
            pad.SetNet(netinfo[n])
    board.Add(f)
    fps[ref] = f
    o = f.GetPosition()
    native[ref] = [(pcbnew.ToMM(p.GetPosition().x - o.x), pcbnew.ToMM(p.GetPosition().y - o.y)) for p in f.Pads()]


def rot(dx, dy, deg):
    """turn counter-clockwise on screen (y down)"""
    t = math.radians(deg)
    return dx * math.cos(t) + dy * math.sin(t), -dx * math.sin(t) + dy * math.cos(t)


def put_front(ref, x, y, r):
    f = fps[ref]
    f.SetOrientationDegrees(r)
    f.SetPosition(pcbnew.VECTOR2I(MM(X0 + x), MM(Y0 + y)))


def put_back(ref, xb, yb, rb):
    f = fps[ref]
    if not f.IsFlipped():
        f.Flip(f.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
    f.SetPosition(pcbnew.VECTOR2I(MM(X0 + W - xb), MM(Y0 + yb)))
    want = [rot(dx, dy, rb) for dx, dy in native[ref]]
    best = None
    for o in (0, 90, 180, 270):
        f.SetOrientationDegrees(o)
        c = f.GetPosition()
        err = sum(math.hypot(-pcbnew.ToMM(p.GetPosition().x - c.x) - w[0], pcbnew.ToMM(p.GetPosition().y - c.y) - w[1])
                  for p, w in zip(f.Pads(), want))
        if best is None or err < best[0]:
            best = (err, o)
    f.SetOrientationDegrees(best[1])
    if best[0] > 0.01:
        print("orientation mismatch", ref, best)


def bview(pt):
    """board point -> back-view mm"""
    return W - (pcbnew.ToMM(pt.x) - X0), pcbnew.ToMM(pt.y) - Y0


for ref, (x, y, r) in FRONT.items():
    put_front(ref, x, y, r)
for ref, (x, y, r) in BACK.items():
    put_back(ref, x, y, r)


def pad_b(ref, num):
    for pad in fps[ref].Pads():
        if pad.GetNumber() == num:
            return bview(pad.GetPosition()), pad
    raise KeyError((ref, num))


def outward(ref, num):
    """direction away from the part at this pad (back view): along the pad's long side, else from the centre"""
    if ref in OUT_DIR:
        return OUT_DIR[ref]
    (px, py), pad = pad_b(ref, num)
    cx, cy = bview(fps[ref].GetPosition())
    dx, dy = px - cx, py - cy
    bb = pad.GetBoundingBox()
    w, h = bb.GetWidth(), bb.GetHeight()
    if w > 1.2 * h:
        return (1 if dx > 0 else -1, 0)
    if h > 1.2 * w:
        return (0, 1 if dy > 0 else -1)
    if abs(dx) >= abs(dy):
        return (1 if dx > 0 else -1, 0)
    return (0, 1 if dy > 0 else -1)


def crt(ref):
    f = fps[ref]
    return f.GetCourtyard(pcbnew.B_CrtYd if f.IsFlipped() else pcbnew.F_CrtYd)


tht = []  # through-hole pads of every part: obstacles on the back
for ref, f in fps.items():
    for p in f.Pads():
        if p.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH):
            tht.append((ref, p.GetBoundingBox()))
placed = [r for r in BACK]


def free(ref):
    pa = crt(ref)
    b = pa.BBox()
    if pcbnew.ToMM(b.GetX()) < X0 + 0.3 or pcbnew.ToMM(b.GetY()) < Y0 + TOP + 0.3 or \
            pcbnew.ToMM(b.GetRight()) > X0 + W - 0.3 or pcbnew.ToMM(b.GetBottom()) > Y0 + H - 0.3:
        return False
    for o in placed:
        if o != ref and fps[o].IsFlipped() and crt(o).OutlineCount() and pa.Collide(crt(o)):
            return False
    for o, bb in tht:
        if o != ref and b.Intersects(bb):
            return False
    return True


for ref, (aref, apad, dist, lat, face) in NEAR.items():
    (px, py), _ = pad_b(aref, apad)
    ox, oy = outward(aref, apad)
    # pad 1 points west at rotation 0 (90: south, 180: east, 270: north); the facing pad points back at the anchor
    rb = {(1, 0): 0, (-1, 0): 180, (0, 1): 270, (0, -1): 90}[(ox, oy)]
    rb = rb if face == "1" else (rb + 180) % 360
    cands = sorted((abs(dd) + abs(ll) * 1.2, dist + dd, lat + ll)
                   for dd in [x * 0.5 for x in range(0, 13)] for ll in [x * 0.5 for x in range(-8, 9)])
    for _, d, l in cands:
        put_back(ref, px + ox * d - oy * l, py + oy * d + ox * l, rb)
        if free(ref):
            break
    else:
        put_back(ref, px + ox * dist, py + oy * dist, rb)
        print("no free spot:", ref)
    placed.append(ref)

missing = [r for r in comps if r not in FRONT and r not in BACK and r not in NEAR]
if missing:
    print("NOT PLACED:", " ".join(sorted(missing)))

# outline
rect = pcbnew.PCB_SHAPE(board)
rect.SetShape(pcbnew.SHAPE_T_RECT)
rect.SetStart(pcbnew.VECTOR2I(MM(X0), MM(Y0 + TOP)))
rect.SetEnd(pcbnew.VECTOR2I(MM(X0 + W), MM(Y0 + H)))
rect.SetLayer(pcbnew.Edge_Cuts)
rect.SetWidth(MM(0.1))
board.Add(rect)

# the ground foil on the front
z = pcbnew.ZONE(board)
z.SetLayer(pcbnew.F_Cu)
z.SetNet(netinfo["GND"])
z.SetIsFilled(False)
ol = z.Outline()
ol.NewOutline()
for (x, y) in ((X0, Y0 + TOP), (X0 + W, Y0 + TOP), (X0 + W, Y0 + H), (X0, Y0 + H)):
    ol.Append(MM(x), MM(y))
z.SetLocalClearance(MM(0.3))
z.SetMinThickness(MM(0.25))
z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
board.Add(z)
filler = pcbnew.ZONE_FILLER(board)
filler.Fill(board.Zones())

# courtyard overlaps on each side, back parts over through-hole pads
over = []
refs = sorted(fps)
for i in range(len(refs)):
    a = fps[refs[i]]
    for j in range(i + 1, len(refs)):
        b = fps[refs[j]]
        if a.IsFlipped() != b.IsFlipped():
            continue
        if crt(refs[i]).OutlineCount() and crt(refs[j]).OutlineCount() and crt(refs[i]).Collide(crt(refs[j])):
            over.append("%s/%s" % (refs[i], refs[j]))
for ref in refs:
    if fps[ref].IsFlipped() and fps[ref].GetAttributes() & pcbnew.FP_SMD:
        b = crt(ref).BBox()
        for o, bb in tht:
            if o != ref and b.Intersects(bb):
                over.append("%s/%s-pin" % (ref, o))
                break
print("overlaps:", len(over), " ".join(over))
pcbnew.SaveBoard(OUT, board)
# SaveBoard of a new board leaves only Default in the project beside it
netclasses.apply(os.path.splitext(OUT)[0] + ".kicad_pro")

print("saved", OUT, "footprints:", len(fps), "front:", len(FRONT), "nets:", len(nets))
