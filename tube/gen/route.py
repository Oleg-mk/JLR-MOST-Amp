"""Tube board copper, laid by hand block by block (no router). Run with KiCad's python:
    python.exe route.py <board.kicad_pcb> <block> [block ...]

F carries the tracks; B is the ground foil (one GND zone), reached through vias. Each block's copper goes into a group
named after the block, and a re-run of a block replaces only that group, so copper laid or edited elsewhere stays.
Net classes live in the project file: 1.25 mm for the 200 V nets (IPC-2221 B2, 151..300 V), 0.6 mm for the ~100 V
nets of the stage (31..150 V); the default 0.15 mm lets the 0.5 mm pitch of U2 pass.
"""
import json, math, os, sys
import pcbnew

PCB, BLOCKS = sys.argv[1], sys.argv[2:]
PRO = os.path.splitext(PCB)[0] + ".kicad_pro"
MM = pcbnew.FromMM


def strip_blocks(path, names):
    """drop the copper groups of the named blocks from the file text before pcbnew loads it (removing them through
    pcbnew's python crashes KiCad 10)"""
    txt = open(path, encoding="utf-8").read()
    i0 = txt.index("(", 1)                      # first child of (kicad_pcb
    items, depth, start, i = [], 0, None, 0
    for i in range(i0, len(txt)):
        c = txt[i]
        if c == "(":
            if depth == 0:
                start = i
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                items.append((start, i + 1))
            elif depth < 0:
                break
    drop, members = set(), set()
    for a, z in items:
        t = txt[a:z]
        if t.startswith("(group") and any('"cu_%s"' % n in t[:60] for n in names):
            drop.add((a, z))
            m = t.find("(members")
            members |= set(t[m:].split('"')[1::2])
    for a, z in items:
        t = txt[a:z]
        if t.startswith(("(segment", "(via", "(zone", "(arc")):
            u = t.find("(uuid")
            if u >= 0 and t[u:].split('"')[1] in members:
                drop.add((a, z))
    out, last = [], 0
    for a, z in sorted(drop):
        out.append(txt[last:a])
        last = z
    out.append(txt[last:])
    open(path, "w", encoding="utf-8", newline="\n").write("".join(out))
    return len(drop)


if __name__ == "__main__" and BLOCKS:
    print("stripped", strip_blocks(PCB, [n for n in BLOCKS if n != "classes"]))

# ---------------------------------------------------------------- net classes (project file)
CLASSES = {
    # name: (clearance, track, via, via drill, patterns)
    "Default": (0.15, 0.25, 0.6, 0.3, []),
    "Power": (0.15, 0.8, 0.8, 0.4, ["+12V*", "*/+12V_F", "*/+12V_D", "+6V3", "*/HEAT", "*/HB_SW"]),
    "HV": (1.25, 0.4, 0.8, 0.4, ["B+", "B+L", "B+R"]),
    "SW": (1.25, 0.8, 0.8, 0.4, ["*/BP_SW"]),
    "MV": (0.6, 0.25, 0.6, 0.3, ["*/%s_%s" % (n, c) for n in ("A", "B", "D", "X", "G3", "G4") for c in "LR"]
           + ["*/BP_FBA", "*/BP_FBB"]),
}


def netclasses():
    p = json.load(open(PRO, encoding="utf-8"))
    ns = p["net_settings"]
    base = dict(ns["classes"][0])
    ns["classes"], pats = [], []
    for i, (name, (cl, tw, vd, vdr, pp)) in enumerate(CLASSES.items()):
        c = dict(base, name=name, clearance=cl, track_width=tw, via_diameter=vd, via_drill=vdr,
                 priority=2147483647 if name == "Default" else i)
        ns["classes"].append(c)
        pats += [{"netclass": name, "pattern": q} for q in pp]
    ns["netclass_patterns"] = pats
    json.dump(p, open(PRO, "w", encoding="utf-8", newline="\n"), indent=2, ensure_ascii=False)


# ---------------------------------------------------------------- board helpers
board = pcbnew.LoadBoard(PCB)
NETS = {}
for name, ni in board.GetNetsByName().items():
    name = str(name)
    NETS[name] = ni
    NETS.setdefault(name.split("/")[-1], ni)          # short name of a sheet-local net
PADS, FPS, LOCAL = {}, {}, {}
for f in board.GetFootprints():       # all read once: after board edits pcbnew hands back unwrapped SWIG objects
    ref, fx, fy, r0 = f.GetReference(), f.GetPosition().x / 1e6, f.GetPosition().y / 1e6, math.radians(f.GetOrientationDegrees())
    FPS[ref], LOCAL[ref] = f, []
    for p in f.Pads():
        px, py = p.GetPosition().x / 1e6, p.GetPosition().y / 1e6
        PADS.setdefault("%s.%s" % (ref, p.GetNumber()), (px, py))
        dx, dy = px - fx, py - fy       # pad offset in the footprint's own frame
        LOCAL[ref].append((p.GetNumber(), dx * math.cos(r0) - dy * math.sin(r0), dx * math.sin(r0) + dy * math.cos(r0)))


def P(key, dx=0.0, dy=0.0):
    """pad centre 'REF.pad', optionally offset"""
    x, y = PADS[key]
    return (x + dx, y + dy)


GROUP, COUNT = None, [0]


def _add(item):
    board.Add(item)
    GROUP.AddItem(item)
    COUNT[0] += 1


def trk(net, w, *pts, layer="F"):
    """a run of track through the points; a point is (x, y) or 'REF.pad'"""
    pts = [P(q) if isinstance(q, str) else q for q in pts]
    for a, b in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pcbnew.VECTOR2I(MM(a[0]), MM(a[1])))
        t.SetEnd(pcbnew.VECTOR2I(MM(b[0]), MM(b[1])))
        t.SetWidth(MM(w))
        t.SetLayer(pcbnew.F_Cu if layer == "F" else pcbnew.B_Cu)
        t.SetNet(NETS[net])
        _add(t)


def via(net, x, y=None, d=0.6, drill=0.3):
    if y is None:
        x, y = P(x) if isinstance(x, str) else x
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
    v.SetWidth(MM(d))
    v.SetDrill(MM(drill))
    v.SetNet(NETS[net])
    _add(v)


def move(ref, x, y, rot=None):
    f = FPS[ref]
    f.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
    if rot is not None:
        f.SetOrientationDegrees(rot)
    r = math.radians(f.GetOrientationDegrees() if rot is None else rot)
    for num, lx, ly in LOCAL[ref]:
        PADS["%s.%s" % (ref, num)] = (x + lx * math.cos(r) + ly * math.sin(r), y - lx * math.sin(r) + ly * math.cos(r))


# ---------------------------------------------------------------- blocks
def b_gnd():
    """the ground foil: one GND zone on B over the whole board"""
    bb = board.GetBoardEdgesBoundingBox()
    z = pcbnew.ZONE(board)
    z.SetLayer(pcbnew.B_Cu)
    z.SetNet(NETS["GND"])
    z.SetZoneName("GND foil")
    ol = z.Outline()
    ol.NewOutline()
    for x, y in ((bb.GetX(), bb.GetY()), (bb.GetRight(), bb.GetY()), (bb.GetRight(), bb.GetBottom()), (bb.GetX(), bb.GetBottom())):
        ol.Append(x, y)
    z.SetLocalClearance(MM(0.25))
    z.SetMinThickness(MM(0.25))
    z.SetThermalReliefGap(MM(0.4))
    z.SetThermalReliefSpokeWidth(MM(0.5))
    z.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
    z.SetAssignedPriority(0)
    _add(z)


# hw4's U8 block (LM65625-Q1, its datasheet layout) was copied part for part onto U1 by gen_pcb.py: x' = K - x,
# y' = y + DY, hw4's B (the U8 side) onto this F. Its local copper comes the same way. Left out: hw4's FB-to-GND link
# (pin 3 here goes to R1/R2), EN (WAKE there), the +12V/+3V3 feed runs and the vias they used on hw4's F.
HB_K, HB_DY = 306.81, -2.13
HB_NET = {"+12V": "+12V", "+3V3": "+6V3", "BUCK_BST": "HB_BST", "BUCK_SW": "HB_SW", "BUCK_VCC": "HB_VCC", "GND": "GND"}
HB_TRK = [  # hw4 B.Cu: net, width, x1, y1, x2, y2
    ("+12V", 0.40, 124.450, 136.123, 124.450, 137.423), ("+12V", 0.60, 126.400, 136.123, 124.442, 136.123),
    ("+12V", 0.40, 124.600, 140.623, 124.450, 140.773), ("+12V", 0.40, 124.450, 140.773, 124.450, 141.323),
    ("+12V", 0.40, 124.450, 141.323, 125.000, 141.873),
    ("+3V3", 0.50, 130.234, 140.411, 130.234, 141.642), ("+3V3", 0.50, 130.234, 141.642, 129.976, 141.900),
    ("+3V3", 0.50, 130.277, 141.939, 129.976, 142.240), ("+3V3", 0.50, 132.596, 141.939, 130.277, 141.939),
    ("+3V3", 0.50, 129.976, 142.240, 129.976, 141.900), ("+3V3", 0.50, 129.976, 142.240, 129.976, 144.920),
    ("+3V3", 0.50, 132.596, 143.788, 132.596, 144.420), ("+3V3", 0.50, 132.596, 144.420, 132.096, 144.920),
    ("+3V3", 0.50, 132.096, 144.920, 129.976, 144.920), ("+3V3", 0.40, 122.850, 144.920, 122.850, 140.823),
    ("BUCK_BST", 0.25, 124.600, 139.623, 125.134, 139.623), ("BUCK_BST", 0.25, 125.134, 139.623, 125.924, 140.414),
    ("BUCK_SW", 0.50, 124.553, 138.864, 126.250, 138.864), ("BUCK_SW", 0.50, 126.250, 138.864, 127.813, 140.427),
    ("BUCK_VCC", 0.25, 121.650, 138.623, 120.800, 138.623), ("BUCK_VCC", 0.25, 121.650, 138.623, 121.650, 138.823),
    ("BUCK_VCC", 0.25, 121.650, 138.823, 121.950, 139.123), ("BUCK_VCC", 0.25, 121.950, 139.123, 122.200, 139.123),
    ("BUCK_VCC", 0.25, 122.200, 138.623, 121.650, 138.623),
    ("GND", 0.30, 118.389, 138.208, 118.389, 137.658), ("GND", 0.30, 120.375, 140.173, 119.825, 140.173),
    ("GND", 0.30, 122.675, 136.123, 121.990, 136.123), ("GND", 0.30, 123.350, 136.423, 123.350, 137.323),
    ("GND", 0.30, 123.350, 140.623, 123.350, 140.023), ("GND", 0.25, 123.350, 141.623, 123.350, 140.823),
    ("GND", 0.25, 123.850, 142.123, 123.350, 141.623), ("GND", 0.30, 122.450, 138.123, 123.300, 138.123),
    ("GND", 0.30, 128.489, 136.474, 127.939, 136.474), ("GND", 0.30, 131.738, 146.638, 131.188, 146.638),
    ("GND", 0.30, 131.763, 138.938, 131.213, 138.938),
]
HB_VIA = [(118.389, 137.658), (119.825, 140.173), (121.990, 136.123), (123.325, 138.423), (123.325, 139.123),
          (123.325, 139.823), (127.939, 136.474), (131.188, 146.638), (131.213, 138.938)]


def hb(x, y):
    return (HB_K - x, y + HB_DY)


def b_u1():
    """heater buck: hw4's U8 copper mirrored, then the output, FB and EN dividers and FB1 of this board"""
    for net, w, x1, y1, x2, y2 in HB_TRK:
        trk(HB_NET[net], w, hb(x1, y1), hb(x2, y2))
    for x, y in HB_VIA:
        via("GND", *hb(x, y))
    # BIAS (pin 20) to the output along hw4's F run
    trk("+6V3", 0.5, hb(122.850, 144.920), hb(129.976, 144.920))
    # FB divider under the right pin row: FB leaves pin 3 between the pins and C9, down to R1/R2
    move("R1", 181.9, 144.2, 0)
    move("R2", 187.3, 144.2, 0)
    trk("+6V3", 0.3, "R1.1", (P("R1.1")[0], 142.79))
    trk("HB_FB", 0.2, "U1.3", (185.22, P("U1.3")[1]), (185.22, 144.2), "R2.1")
    trk("HB_FB", 0.2, (185.22, 144.2), "R1.2")
    trk("GND", 0.3, "R2.2", (189.6, 144.2))
    via("GND", 189.6, 144.2)
    # EN divider (start 7.9 V on REM_IN) above C3: EN leaves pin 7 to the right over C9 and C5
    move("R3", 193.0, 133.2, 180)
    move("R4", 193.0, 130.9, 0)
    trk("HB_EN", 0.2, "U1.7", (185.3, 134.8), (191.0, 134.8), (191.0, 130.9), "R4.1")
    trk("HB_EN", 0.2, (191.0, 133.2), "R3.2")
    trk("GND", 0.3, "R4.2", (P("R4.2")[0], 129.4))
    via("GND", P("R4.2")[0], 129.4)
    # heater bead left of H5, fed from C11 under the hole's keepout
    move("FB1", 163.4, 142.6, 180)
    trk("+6V3", 0.8, "C11.1", (173.2, 142.6), "FB1.1")


def b_inp():
    """input: J1 -> F1 -> C1 -> D1 -> D2 -> L1 -> C2 on wide tracks, GND vias by the caps and the TVS; REM_IN from J1.3
    to U3's input and its cap; U3's output to its cap. U3's caps swap places: the input cap by the input pins."""
    move("C23", 116.5, 137.9, 0)
    move("C24", 120.0, 134.6, 0)
    trk("+12V_IN", 1.0, "J1.1", (108.92, 142.9), (114.7, 142.9))
    trk("+12V_F", 0.8, "F1.2", (121.5, 142.55), "C1.1", (124.5, 142.55), (125.5, 141.55))
    trk("GND", 0.4, "C1.2", (123.0, 139.55))
    via("GND", 123.0, 139.55)
    trk("+12V_D", 1.0, "D1.1", (134.6, 138.4), (141.5, 138.4))
    trk("GND", 0.8, "D2.2", (137.3, 144.3))
    trk("GND", 0.8, (136.6, 144.3), (138.0, 144.3))
    via("GND", 136.6, 144.3)
    via("GND", 138.0, 144.3)
    trk("+12V", 1.0, "L1.2", (148.2, 135.75), "C2.1")
    trk("GND", 0.8, "C2.2", (155.4, 144.6))
    trk("GND", 0.8, (154.6, 144.6), (156.2, 144.6))
    via("GND", 154.6, 144.6)
    via("GND", 156.2, 144.6)
    # REM_IN leaves J1.3 up the right side of J1's pin field, so it never meets +12V_IN under it
    trk("REM_IN", 0.3, "J1.3", (113.3, 139.03), (113.3, 137.9), "C23.1")
    trk("REM_IN", 0.3, (113.3, 137.9), (113.3, 135.3), (114.5, 134.6), "U3.2")
    trk("REM_IN", 0.3, (114.5, 134.6), (114.5, 133.95), "U3.1")
    # U3: GND pins to the exposed pad and a via in it; output to its cap
    trk("GND", 0.25, "U3.3", (116.0, 135.25), "U3.4")
    via("GND", 116.0, 134.6)
    trk("+3V3_REM", 0.3, "U3.6", (117.6, 133.95), (118.25, 134.6), "C24.1")
    trk("GND", 0.4, "C24.2", (121.9, 134.6))
    via("GND", 121.9, 134.6)
    trk("GND", 0.4, "C23.2", (117.45, 139.0))
    via("GND", 117.45, 139.0)


def b_bst():
    """boost power stage, LM5022 Fig. 31 mirrored: SW node L3 -> D3 -> Q1 tab short and wide, B+ from D3 over C18/C19
    and round C19 down to C20 (turned so its + pad faces that run), sense Q1 source -> R10, input caps at L3's +12V
    end, gate Q1 <- R8/R9; ground returns through vias by every cap. R12/R13 step 0.6 mm right: B+ to R10's ground
    pad needs 1.25 mm."""
    move("C20", 172.5, 89.0, 180)
    move("R12", 167.6, 95.6, 0)
    move("R13", 172.6, 95.6, 0)
    trk("BP_SW", 1.5, "L3.2", (150.0, 75.5), "D3.2")
    trk("BP_SW", 1.2, "D3.2", (159.5, 80.2))
    trk("B+", 0.6, (164.6, 75.0), (166.2, 74.3))
    trk("B+", 0.6, "C18.1", "C19.1", (174.2, 73.95), (175.1, 74.85), (175.1, 86.4), (173.2, 88.3))
    trk("BP_SNS", 0.8, "Q1.3", "R10.1")
    trk("+12V", 0.8, (148.4, 87.0), (145.9, 88.6))
    trk("+12V", 0.6, (145.52, 90.8), (146.05, 92.0))
    trk("BP_G", 0.4, "R9.1", (158.3, 89.4))
    for ref in ("C18", "C19"):
        x, y = P(ref + ".2")
        trk("GND", 0.8, (x, y), (x, y + 1.45))
        via("GND", x - 0.5, y + 1.45)
        via("GND", x + 0.5, y + 1.45)
    trk("GND", 0.8, "R10.2", (163.3, 96.3))
    via("GND", 163.3, 96.3)
    trk("GND", 0.8, "C16.2", (148.48, 91.3))
    via("GND", 148.48, 91.3)
    trk("GND", 0.4, "C17.2", (148.9, 92.6))
    trk("GND", 0.4, "R9.2", (154.05, 88.1))
    via("GND", 154.05, 88.1)


DEFS = {"gnd": b_gnd, "u1": b_u1, "inp": b_inp, "bst": b_bst}

# ---------------------------------------------------------------- run
CLS = "classes" in BLOCKS                  # written after the save: pcbnew's save rewrites the project file too
BLOCKS = [b for b in BLOCKS if b != "classes"]
for name in BLOCKS:
    GROUP = pcbnew.PCB_GROUP(board)
    GROUP.SetName("cu_" + name)
    board.Add(GROUP)
    COUNT[0] = 0
    DEFS[name]()
    print("block", name, "items", COUNT[0])
filler = pcbnew.ZONE_FILLER(board)
filler.Fill(board.Zones())
board.Save(PCB)
if CLS:
    netclasses()
print("saved", PCB)
