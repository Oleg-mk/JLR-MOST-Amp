"""Tube board (6N28B-V main variant, schematic v0.2): board frame and placement from the netlist. Run with KiCad's python:
    python.exe gen_pcb.py <netlist.net> <out.kicad_pcb>

Frame. The board hangs on standoffs over the back (component side) of the main board hw4, its ground foil (B) facing
the main board. Seen from its own front (F) the main board is mirrored in x: x_tube = 320.08 - x_hw4, y the same. So
the mounting holes H1..H5 land on hw4's, and the two sockets J1 (power/REM) and J3 (signal) land on hw4's pin headers
J9 and J8. The board is hw4's outline (x 99.95..220.13, y 101.62..160.35) plus a 31.27 mm ledge above it (y from
70.35): 120.18 x 90. The four tubes lie on B on the ledge, two rows, tips 1 mm apart; everything else is on F, the
stage parts right over the tubes (the tubes are on the other side).
Routing is not done here.
"""
import json, os, re, sys
import pcbnew

NET, OUT = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(os.path.dirname(os.path.abspath(OUT)), "jlr_tube.pretty")
KFP = "C:/Users/geniy/AppData/Local/Programs/KiCad/10.0/share/kicad/footprints"
HW4 = os.path.join(HERE, "..", "..", "hw4", "jlr_most_amp.kicad_pcb")
MM = pcbnew.FromMM

YT, YM, YB = 70.35, 101.62, 160.35                                      # top of the ledge, main board edge, bottom
XL, XR = 99.95, 220.13
MIR = 320.08                                                        # x_tube = MIR - x_hw4

# ---------------------------------------------------------------- netlist
s = open(NET, encoding="utf-8").read()
comps = {}
ci, cj = s.index("(components"), s.index("(libparts")
for body in re.split(r"\(comp\s*\(ref ", s[ci:cj])[1:]:
    ref = re.match(r'\s*"([^"]+)"', body).group(1)
    val = re.search(r'\(value "([^"]*)"\)', body).group(1)
    fpm = re.search(r'\(footprint "([^"]*)"\)', body)
    sheet = re.search(r'\(sheetpath\s*\(names "([^"]*)"\)\s*\(tstamps "([^"]*)"\)', body)
    ts = re.search(r'\(tstamps "([^"]+)"', body[sheet.end():]).group(1)     # the symbol's own (first unit's) uuid
    sfile = re.search(r'\(name "Sheetfile"\)\s*\(value "([^"]*)"\)', body)
    sname = re.search(r'\(name "Sheetname"\)\s*\(value "([^"]*)"\)', body)
    comps[ref] = dict(value=val, fp=fpm.group(1) if fpm else "", path=sheet.group(2) + ts,
                      sheetfile=sfile.group(1) if sfile else "", sheetname=sname.group(1) if sname else "")
nets = {}
i = s.index("(nets")
for blk in re.split(r"\(net\s*\(code", s[i:])[1:]:
    name = re.search(r'\(name "([^"]*)"\)', blk).group(1)
    nets[name] = [(n.group(1), n.group(2)) for n in re.finditer(r'\(node\s*\(ref "([^"]+)"\)\s*\(pin "([^"]+)"\)', blk)]

# ---------------------------------------------------------------- placement (board coordinates, KiCad front view)
# (x, y, rotation, side). Wall connectors: fronts on the walls (RCA port 0.1 mm in, XLR origin on the edge = rear face
# of the panel, Micro-Fit origin 8.97 in).
TIP = 0.5
XC = 156.04                   # the tubes' tips meet here (4 mm left of the board centre: room for the RCA outputs)
ROW_O, ROW_I = 77.6, 94.4
PLACE = {
    # tubes on B: V1/V2 channel L (V1 gain + concertina in the inner row, V2 followers in the outer), V3/V4 channel R
    "V1": (XC - TIP - 35.0, ROW_I, 180, "B"), "V2": (XC - TIP - 35.0, ROW_O, 180, "B"),
    "V3": (XC + TIP + 35.0, ROW_I, 0, "B"), "V4": (XC + TIP + 35.0, ROW_O, 0, "B"),
    # right wall: RCA outputs on the ledge, XLR on the main part
    "J7": (XR - 13.2, 78.6, 90, "F"), "J8": (XR - 13.2, 94.0, 90, "F"),
    "J5": (XR, 113.5, 270, "F"), "J6": (XR, 137.4, 270, "F"),
    # left wall: REM to the amplifier
    "J2": (XL + 8.97, 155.5, 90, "F"),
    # --- channel L on the ledge, F side. Column A left of the tubes' pads: the 200 V parts (2512 flat, 5.1 mm pitch)
    "R26": (104.4, 72.6, 0, "F"), "R29": (104.4, 78.2, 0, "F"), "R31": (104.4, 83.8, 180, "F"),
    "R25": (104.4, 88.6, 180, "F"), "C30": (104.4, 92.3, 0, "F"), "R34": (104.4, 96.1, 0, "F"),
    # over V2 (the followers): CCS pair by the near-row pins, grid parts, coupling films, the grid reference
    "Q2": (119.1, 72.9, 0, "F"), "Q3": (119.1, 79.9, 0, "F"),
    "R32": (127.5, 72.0, 0, "F"), "R33": (127.5, 74.4, 0, "F"), "R36": (123.4, 76.6, 0, "F"), "R37": (123.4, 78.9, 0, "F"),
    "C32": (131.5, 72.8, 0, "F"), "C33": (131.5, 81.7, 0, "F"),
    "R35": (103.5, 120.5, 0, "F"), "C34": (108.3, 120.5, 0, "F"),
    # over V1 (gain triode and concertina) and the input divider by the grid
    "R27": (119.6, 90.4, 0, "F"), "R30": (119.6, 96.4, 0, "F"), "R28": (123.4, 91.5, 90, "F"), "C31": (129.2, 90.2, 0, "F"),
    "R22": (118.0, 99.6, 0, "F"), "R23": (121.6, 99.6, 0, "F"), "C28": (125.2, 99.6, 0, "F"), "R24": (128.8, 99.6, 0, "F"),
    # --- channel R: mirror of L about x = XC for the over-tube parts; the 200 V column over the tubes' tips
    "R48": (161.5, 72.6, 0, "F"), "R51": (161.5, 78.2, 0, "F"), "R53": (161.5, 83.8, 180, "F"),
    "R47": (161.5, 88.6, 180, "F"), "C43": (161.5, 92.3, 0, "F"), "R56": (161.5, 96.1, 0, "F"),
    "Q4": (192.8, 72.9, 180, "F"), "Q5": (192.8, 79.9, 180, "F"),
    "R54": (180.9, 72.0, 0, "F"), "R55": (180.9, 74.4, 0, "F"), "R58": (185.3, 72.0, 0, "F"), "R59": (185.3, 74.4, 0, "F"),
    "C45": (171.9, 72.8, 180, "F"), "C46": (182.4, 81.7, 180, "F"),
    "R57": (172.0, 112.8, 0, "F"), "C47": (176.2, 112.8, 90, "F"),
    "R49": (192.48, 90.4, 0, "F"), "R52": (192.48, 96.4, 0, "F"), "R50": (188.68, 91.5, 90, "F"), "C44": (182.88, 90.2, 180, "F"),
    "R44": (183.3, 99.6, 0, "F"), "R45": (186.9, 99.6, 0, "F"), "C41": (190.5, 99.6, 0, "F"), "R46": (194.1, 99.6, 0, "F"),
    # --- REM logic on the ledge centre, between the L follower parts and the R 200 V column (DC and slow signals)
    "U5": (149.0, 73.0, 0, "F"), "U3": (154.4, 72.3, 0, "F"),
    "R20": (147.2, 77.2, 0, "F"), "R21": (150.8, 77.2, 0, "F"), "C27": (154.4, 77.2, 0, "F"),
    "C23": (147.2, 79.5, 0, "F"), "C24": (150.8, 79.5, 0, "F"), "C25": (154.4, 79.5, 0, "F"),
    "U4": (148.3, 82.6, 0, "F"), "U6": (153.3, 82.6, 0, "F"),
    "C26": (147.2, 86.3, 0, "F"), "C56": (150.8, 86.3, 0, "F"), "C57": (154.4, 86.3, 0, "F"),
    "R17": (147.2, 88.6, 0, "F"), "R18": (150.8, 88.6, 0, "F"), "R19": (154.4, 88.6, 0, "F"),
    "R66": (147.2, 90.9, 0, "F"), "D4": (151.5, 90.9, 0, "F"),
    # --- main part. B+ filter electrolytics, heater bead and caps at the ledge boundary
    "C29": (105.0, 111.5, 0, "F"), "C42": (160.5, 108.5, 0, "F"),
    "FB1": (111.0, 103.4, 0, "F"), "C54": (105.0, 103.3, 0, "F"), "C55": (197.5, 102.3, 0, "F"),
    # output capacitors: channel L two flat rows left of centre, channel R two upright behind its XLR (see below)
    "C35": (125.0, 114.0, 0, "F"), "C36": (125.0, 125.5, 0, "F"),
    # output networks: L behind XLR J5, R below its capacitors along the bottom
    "R40": (172.0, 105.0, 0, "F"), "C37": (175.6, 105.0, 0, "F"), "R42": (179.2, 105.0, 0, "F"), "C39": (182.8, 105.0, 0, "F"),
    "R43": (186.4, 105.0, 0, "F"), "C40": (172.0, 108.6, 0, "F"), "R38": (176.6, 108.6, 0, "F"),
    "R41": (181.2, 108.6, 0, "F"), "C38": (184.8, 108.6, 0, "F"), "R39": (189.0, 108.6, 90, "F"),
    "R62": (186.0, 150.5, 0, "F"), "C50": (189.6, 150.5, 0, "F"), "R64": (193.2, 150.5, 0, "F"), "C52": (196.8, 150.5, 0, "F"),
    "R65": (200.4, 150.5, 0, "F"), "C53": (204.0, 150.5, 0, "F"),
    "R60": (196.4, 153.9, 0, "F"), "R63": (200.4, 153.9, 0, "F"), "C51": (204.0, 153.9, 0, "F"), "R61": (208.3, 153.9, 0, "F"),
    # input chain left of centre: L1 and C2 under the B+ filter cap, F1/C1/D2 in a row, D1 below
    "L1": (105.0, 127.5, 0, "F"), "C2": (115.4, 127.5, 0, "F"),
    "F1": (104.5, 135.0, 0, "F"), "C1": (110.5, 135.0, 0, "F"), "D2": (116.8, 135.3, 0, "F"), "D1": (106.0, 140.4, 0, "F"),
    # heater buck dividers above the copied U1 block
    "R3": (123.5, 144.3, 0, "F"), "R4": (127.1, 144.3, 0, "F"), "R1": (130.7, 144.3, 0, "F"), "R2": (134.3, 144.3, 0, "F"),
    # B+ boost (LM5022 fig. 31 turned): L3, Q1 with D3 over its tab, the output caps right of D3, the sense resistor
    # under the source; the controller and its parts in the strip above, the B+ divider at the strip's right end
    "L3": (146.5, 150.0, 0, "F"), "Q1": (160.8, 150.0, 90, "F"), "D3": (161.8, 143.2, 180, "F"),
    "C18": (168.5, 145.6, 270, "F"), "C19": (172.5, 145.6, 270, "F"), "C20": (179.5, 155.0, 180, "F"),
    "R8": (155.5, 158.0, 0, "F"), "R9": (152.0, 158.0, 0, "F"), "R10": (166.5, 156.0, 90, "F"),
    "R11": (169.8, 158.3, 0, "F"), "C15": (169.8, 155.6, 0, "F"),
    "U2": (145.0, 140.5, 90, "F"),
    "R5": (149.6, 141.5, 90, "F"), "C14": (151.9, 141.5, 90, "F"), "R7": (140.4, 141.5, 90, "F"), "R6": (138.1, 141.5, 90, "F"),
    "C12": (139.2, 136.3, 0, "F"), "C22": (142.8, 136.3, 0, "F"), "R16": (146.4, 136.3, 0, "F"), "C21": (150.0, 136.3, 0, "F"),
    "C13": (153.6, 136.3, 0, "F"), "R15": (135.6, 136.3, 0, "F"),
    "C17": (124.0, 133.3, 0, "F"), "C16": (128.3, 133.3, 0, "F"),
    "R12": (160.0, 133.3, 0, "F"), "R13": (165.2, 133.3, 0, "F"), "R14": (160.0, 137.6, 0, "F"),
}
# sockets on B, pin for pin under hw4's headers: J9 pins 1..5 at x 207.0 + 2.54 k, y 145.9; J8 pins at x 200.0, y 111.19 + 2.54 k
SOCKETS = {"J1": {str(k + 1): (MIR - (207.0 + 2.54 * k), 145.9) for k in range(5)},
           "J3": {str(k + 1): (MIR - 200.0, 111.19 + 2.54 * k) for k in range(4)}}
# heater buck: hw4's U8 block copied pad for pad, mirrored onto this F (x' = HB_K - x, y' = y + HB_DY)
HB_K, HB_DY = 255.9, 12.0
HB_MAP = {"U8": "U1", "L2": "L2", "C55": "C4", "C56": "C5", "C57": "C6", "C60": "C7", "C53": "C8", "C54": "C9",
          "C58": "C10", "C59": "C11"}
# mounting holes: hw4's H1..H5 mirrored
HW4_H = {"H1": (200.711, 105.92), "H2": (201.6, 146.8), "H3": (129.54, 156.05), "H4": (126.45, 107.772), "H5": (150.216, 139.065)}
HOLES = {k: (MIR - x, y) for k, (x, y) in HW4_H.items()}

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
pad_net = {(ref, pin): name for name, nodes in nets.items() for ref, pin in nodes}


def load(lib, name):
    path = LIB if lib == "jlr_tube" else os.path.join(KFP, lib + ".pretty")
    f = pcbnew.FootprintLoad(path, name)
    f.SetFPID(pcbnew.LIB_ID(lib, name))
    return f


fps = {}
for ref, c in comps.items():
    if not c["fp"]:
        continue                       # MP1..MP4: off-board symbols
    lib, name = c["fp"].split(":")
    f = load(lib, name)
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

for ref, (x, y) in HOLES.items():
    f = load("jlr_tube", "MountingHole_2.7mm_M2.5_Standoff")
    f.SetReference(ref)
    f.SetValue("M2.5")
    board.Add(f)
    f.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
    fps[ref] = f

for ref, (x, y, r, side) in PLACE.items():
    f = fps[ref]
    if side == "B" and not f.IsFlipped():
        f.Flip(f.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
    f.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
    f.SetOrientationDegrees(r)


def match(f, targets):
    """put footprint f so its pads land on targets {pad number: (x, y)}: the rotation (0/90/180/270) whose pad
    pattern fits best, then the centroid on the targets' centroid"""
    best = None
    for r in (0, 90, 180, 270):
        f.SetOrientationDegrees(r)
        f.SetPosition(pcbnew.VECTOR2I(0, 0))
        pp = {p.GetNumber(): (p.GetPosition().x / 1e6, p.GetPosition().y / 1e6) for p in f.Pads() if p.GetNumber() in targets}
        ks = sorted(pp)
        cx, cy = sum(pp[k][0] for k in ks) / len(ks), sum(pp[k][1] for k in ks) / len(ks)
        tx, ty = sum(targets[k][0] for k in ks) / len(ks), sum(targets[k][1] for k in ks) / len(ks)
        err = sum((pp[k][0] - cx - targets[k][0] + tx) ** 2 + (pp[k][1] - cy - targets[k][1] + ty) ** 2 for k in ks)
        if best is None or err < best[0]:
            best = (err, r, tx - cx, ty - cy)
    f.SetOrientationDegrees(best[1])
    f.SetPosition(pcbnew.VECTOR2I(MM(best[2]), MM(best[3])))
    return best[0]


for ref, tg in SOCKETS.items():
    f = fps[ref]
    if not f.IsFlipped():
        f.Flip(f.GetPosition(), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
    e = match(f, tg)
    print(ref, "on B, pad fit error %.4f" % e)

hw4 = pcbnew.LoadBoard(HW4)
for src in hw4.GetFootprints():
    dst = HB_MAP.get(src.GetReference())
    if not dst:
        continue
    seen, tg = set(), {}
    for p in src.Pads():
        n = p.GetNumber()
        if n and n not in seen:          # U8's corner pins come as two pads with one number: take the first
            seen.add(n)
            tg[n] = (HB_K - p.GetPosition().x / 1e6, p.GetPosition().y / 1e6 + HB_DY)
    if dst == "U1":                      # the side pins only: the corner pads would pair up ambiguously
        tg = {n: v for n, v in tg.items() if n not in ("1", "7", "11", "17")}
    match(fps[dst], tg)
    PLACE[dst] = None

# output capacitors of channel R upright: pad 1 (from the follower) on top, pad 2 (to the XLR) at the bottom
for ref, x in (("C48", 181.5), ("C49", 193.0)):
    match(fps[ref], {"1": (x, 118.75), "2": (x, 146.25)})
    PLACE[ref] = None

# parts not placed yet: staged to the right of the board, by sheet
stage_x = {"power.kicad_sch": 240.0, "rem.kicad_sch": 290.0, "tubes.kicad_sch": 330.0}
row = {}
for ref in sorted(fps, key=lambda r: (re.sub(r"\d", "", r), int(re.sub(r"\D", "", r) or 0))):
    if ref in PLACE or ref in HOLES or ref in SOCKETS:
        continue
    sf = comps[ref]["sheetfile"] if ref in comps else "tubes.kicad_sch"
    x0 = stage_x.get(sf, 380.0)
    k = row.get(sf, 0)
    row[sf] = k + 1
    fps[ref].SetPosition(pcbnew.VECTOR2I(MM(x0 + (k % 4) * 11.0), MM(YT + (k // 4) * 9.0)))
    print("staged:", ref)

# outline
for (a, b) in (((XL, YT), (XR, YT)), ((XR, YT), (XR, YB)), ((XR, YB), (XL, YB)), ((XL, YB), (XL, YT))):
    seg = pcbnew.PCB_SHAPE(board)
    seg.SetShape(pcbnew.SHAPE_T_SEGMENT)
    seg.SetLayer(pcbnew.Edge_Cuts)
    seg.SetWidth(MM(0.1))
    seg.SetStart(pcbnew.VECTOR2I(MM(a[0]), MM(a[1])))
    seg.SetEnd(pcbnew.VECTOR2I(MM(b[0]), MM(b[1])))
    board.Add(seg)
# the main board's edge under the ledge, for reference
ln = pcbnew.PCB_SHAPE(board)
ln.SetShape(pcbnew.SHAPE_T_SEGMENT)
ln.SetLayer(pcbnew.Dwgs_User)
ln.SetWidth(MM(0.15))
ln.SetStart(pcbnew.VECTOR2I(MM(XL), MM(YM)))
ln.SetEnd(pcbnew.VECTOR2I(MM(XR), MM(YM)))
board.Add(ln)

board.Save(OUT)
print("saved", OUT, "footprints", len(fps))

# net classes by voltage, into the project file (pcbnew's save writes only Default): the .kicad_dru holds the pair rules
PRO = os.path.splitext(OUT)[0] + ".kicad_pro"
CLASSES = {
    "Default": (0.15, 0.25, 0.6, 0.3, []),
    "Power": (0.15, 0.8, 0.8, 0.4, ["+12V*", "*/+12V_F", "*/+12V_D", "+6V3", "*/HEAT", "*/HB_SW", "*/REM_IN", "*/REM_OUT"]),
    "HV": (1.25, 0.4, 0.8, 0.4, ["B+", "B+L", "B+R"]),
    "SW": (1.25, 0.8, 0.8, 0.4, ["*/BP_SW"]),
    "MV": (0.5, 0.25, 0.6, 0.3, ["*/%s_%s" % (n, c) for n in ("A", "P", "K2", "KJ", "G2", "GH", "GC", "VREF", "OH", "OC") for c in "LR"]
           + ["*/BP_FBA", "*/BP_FBB"]),
}
if os.path.exists(PRO):
    p = json.load(open(PRO, encoding="utf-8"))
    ns = p.setdefault("net_settings", {"classes": [{}], "meta": {"version": 5}})
    base = dict(ns["classes"][0]) if ns.get("classes") else {}
    ns["classes"], pats = [], []
    for i, (name, (cl, tw, vd, vdr, pp)) in enumerate(CLASSES.items()):
        c = dict(base, name=name, clearance=cl, track_width=tw, via_diameter=vd, via_drill=vdr,
                 priority=2147483647 if name == "Default" else i)
        ns["classes"].append(c)
        pats += [{"netclass": name, "pattern": q} for q in pp]
    ns["netclass_patterns"] = pats
    json.dump(p, open(PRO, "w", encoding="utf-8", newline="\n"), indent=2, ensure_ascii=False)
    print("net classes written to", PRO)
