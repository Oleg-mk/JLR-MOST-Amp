"""Grid router for the two-sided board: tracks on B.Cu (the SMD side), and on the F.Cu ground foil only short jumps
where a crossing cannot be avoided.

python.exe route_grid.py <in.kicad_pcb> <out.kicad_pcb> [fix [REF,REF,...] | tidy | swap]

fix: the board is already routed and the user has moved parts. Copper that KiCad's DRC finds clashing with the moved
parts goes, then every track or via left hanging; all other copper stays as it is, and only the connections now
missing are laid. With REFs, all copper within 2.5 mm of those parts is laid afresh too, so the neighbours can make
room instead of the new connection jumping over them. In fix mode a jump to the foil is priced high: the goal is as
few vias as possible.

tidy: straighten a routed board. Every run of track between two fixed points (a pad, a via, a branch) is laid again
on its own layer as the path with the fewest turns that keeps clear of all other copper, within 1.2 mm of where it
ran. Connections stay as they are and nothing new goes to the foil.

swap: fewer vias. Where several nets each jump over one back-side track, that track goes under them on the foil
instead (track_swap.py).

Nets are routed one by one in a fixed order (ORDER), each as a tree grown from its first pad. A cell is free for a
net when the net's track, at its width, keeps the clearance from all other copper; copper is rasterized from the
real pad shapes, tracks and vias. Cost per mm: 1 on B.Cu, COST_F on F.Cu; a via costs COST_VIA.
"""
import sys, math, heapq, time
import numpy as np
import pcbnew

IN, OUT = sys.argv[1], sys.argv[2]
FIX = len(sys.argv) > 3 and sys.argv[3] == "fix"
AROUND = sys.argv[4].split(",") if FIX and len(sys.argv) > 4 else []
TIDY = len(sys.argv) > 3 and sys.argv[3] == "tidy"
SWAP = len(sys.argv) > 3 and sys.argv[3] == "swap"
KCLI = "C:/Users/geniy/AppData/Local/Programs/KiCad/10.0/bin/kicad-cli.exe"
G = 0.1                     # grid pitch, mm
CLR = 0.15                  # copper clearance
EDGE = 0.5                  # copper to the board edge
HOLE_CLR = 0.25             # copper to a hole edge
VIA_D, VIA_H = 0.6, 0.3
COST_F, COST_VIA = (25.0, 20.0) if FIX else (10.0, 3.0)
import os
if os.environ.get("ROUTE_COST_VIA"):   # a run may price vias differently (a clock bus: no long detours)
    COST_F, COST_VIA = float(os.environ["ROUTE_COST_F"]), float(os.environ["ROUTE_COST_VIA"])
B, F = 0, 1
LID = {B: pcbnew.B_Cu, F: pcbnew.F_Cu}

# net -> (width, jumps on F.Cu allowed)
WIDE = {"+12V": 0.8, "+12V_IN": 0.8, "+12V_F": 0.8, "+12V_D": 0.8, "REM_OUT": 0.8,
        "+3V3": 0.4, "+3V3_AON": 0.4, "+3V0_TOS": 0.3, "+3V3_NICA": 0.3, "FOT_RXVCC": 0.3, "FOT_TXVCC": 0.3,
        "+3V3A": 0.4, "BUCK_SW": 0.6}
NO_JUMP = {"DAC_OUTL", "DAC_OUTR", "LINE_L", "LINE_R", "DAC_CAPP", "DAC_CAPM", "DAC_VNEG", "DAC_LDOO", "+3V3A",
           "XTI", "XTO", "FLT", "FLT_RC", "MOST_RX", "MOST_TX", "FOT_RXD", "FOT_TXD",
           "BUCK_SW", "BUCK_BST", "BUCK_FB", "BUCK_COMP", "BUCK_CC", "BUCK_RT", "COAX_DRV", "COAX_MID", "COAX_OUT"}
ORDER = [
    # short, sensitive
    "XTI", "XTO", "FLT", "FLT_RC", "DAC_CAPP", "DAC_CAPM", "DAC_VNEG", "DAC_LDOO",
    "BUCK_SW", "BUCK_BST", "BUCK_FB", "BUCK_COMP", "BUCK_CC", "BUCK_RT",
    "COAX_DRV", "COAX_MID", "COAX_OUT",
    # the DAC's outputs leave its analogue row before AVDD/CPVDD can wall them in; +3V3A then reaches the pins
    # from below and under the chip
    "DAC_OUTR", "DAC_OUTL", "LINE_L", "LINE_R",
    "+3V3A",
    # FOT lines before anything crowds the OS8104A's FOT corner
    "MOST_TX", "FOT_TXD", "MOST_RX", "FOT_RXD",
    # (the LVDS pairs to the HDMI are laid by hdmi_breakout.py before routing)
    # OS8104A -> ESP32
    "MOST_RST", "SPDIF_SRC", "MOST_SX1", "I2C_SCL", "I2C_SDA", "MOST_INT",
    "SCK", "FSY", "RMCK", "BCK", "LRCK", "MOST_SX3", "MCLK", "DOUT", "SPDIF",
    "FOT_RGAIN", "FOT_STATUS", "MOST_AINT", "MOST_TEST3",
    # control
    "DAC_SDA", "DAC_SCL", "DAC_XSMT", "DAC_MODE2",
    "HOLD", "REM_EN", "REM_IN", "REM_CL", "REM_DELAY", "WAKE", "WAKE_FORCE",
    "MODE0", "MODE1", "MODE2", "MODE3", "LED", "LED_A",
    "ESP_EN", "ESP_TXD0", "ESP_RXD0", "ESP_IO0",
    # supplies
    "+3V3_NICA", "FOT_TXVCC", "FOT_RXVCC", "+3V0_TOS", "+3V3_AON",
    "+3V3", "+12V_IN", "+12V_F", "+12V_D", "+12V", "REM_OUT",
]
# a run may put some nets first, right after the analogue and FOT lines (the I2S bus before what crosses it)
if os.environ.get("ROUTE_FIRST"):
    _first = os.environ["ROUTE_FIRST"].split(",")
    ORDER = [n for n in ORDER if n not in _first]
    ORDER[ORDER.index("FOT_RXD") + 1:ORDER.index("FOT_RXD") + 1] = _first
# the pad each net grows from: the source of a bus, the output of a supply
START = {"MCLK": ("R13", "2"), "BCK": ("R15", "2"), "LRCK": ("R14", "2"), "DOUT": ("U2", "30"), "SPDIF": ("U1", "9"),
         "+3V3": ("L2", "2"), "+12V": ("L1", "2"), "+3V3A": ("U9", "6"), "+3V3_AON": ("U7", "6")}
# cost per mm on the foil side for nets that break out there by design (none at present)
COST_F_NET = {}
COST_VIA_NET = {}
# a run may price some nets' vias moderately (a clock bus: a couple of vias beat a detour round the board)
if os.environ.get("ROUTE_MODERATE"):
    for _n in os.environ["ROUTE_MODERATE"].split(","):
        COST_F_NET[_n], COST_VIA_NET[_n] = 12.0, 6.0

t0 = time.time()
b = pcbnew.LoadBoard(IN)


def free_span(t, ends_ok, ts, pads_net):
    """what of track t must stay when an end is free: (lo, hi) along it from start to end, None if nothing holds it.
    A free end is cut back to the last item of the net touching the track part-way (a branch joined to its middle
    keeps its join)."""
    lay = t.GetLayer()
    TOUCH = pcbnew.FromMM(0.01)
    hw = t.GetWidth() // 2 + TOUCH
    s0, e0 = t.GetStart(), t.GetEnd()
    dx, dy = e0.x - s0.x, e0.y - s0.y
    L2 = float(dx * dx + dy * dy) or 1.0
    us = []
    tu = t.m_Uuid.AsString()
    for o in ts:
        if o.m_Uuid.AsString() == tu:   # (a fresh GetTracks() hands out new Python objects)
            continue
        if o.GetClass() == "PCB_VIA":
            pts_o, wo = [o.GetPosition()], o.GetWidth(lay)
        elif o.GetLayer() == lay:
            pts_o, wo = [o.GetStart(), o.GetEnd()], o.GetWidth()
        else:
            continue
        for e in pts_o:
            if t.HitTest(e, wo // 2 + TOUCH):
                us.append(min(1.0, max(0.0, ((e.x - s0.x) * dx + (e.y - s0.y) * dy) / L2)))
    n_ = max(2, int(math.sqrt(L2) / pcbnew.FromMM(0.05)))
    for p in pads_net:
        if p.IsOnLayer(lay):
            us += [k / n_ for k in range(n_ + 1)
                   if p.HitTest(pcbnew.VECTOR2I(int(s0.x + dx * k / n_), int(s0.y + dy * k / n_)), hw)]
    lo = 0.0 if ends_ok[0] else (min(us) if us else None)
    hi = 1.0 if ends_ok[1] else (max(us) if us else None)
    if lo is None or hi is None or hi - lo < 1e-3:
        return None
    return lo, hi


def apply_span(t, lo, hi):
    s0, e0 = t.GetStart(), t.GetEnd()
    sx, sy, ex, ey = int(s0.x), int(s0.y), int(e0.x), int(e0.y)   # plain numbers: GetStart() is a live reference
    dx, dy = ex - sx, ey - sy
    if os.environ.get("ROUTE_DEBUG"):
        print("   trim %s (%.3f,%.3f)-(%.3f,%.3f) to %.3f..%.3f" % (t.GetNetname().split("/")[-1], 230 - pcbnew.ToMM(s0.x),
              pcbnew.ToMM(s0.y) - 100, 230 - pcbnew.ToMM(e0.x), pcbnew.ToMM(e0.y) - 100, lo, hi))
    t.SetStart(pcbnew.VECTOR2I(int(round(sx + dx * lo)), int(round(sy + dy * lo))))
    t.SetEnd(pcbnew.VECTOR2I(int(round(sx + dx * hi)), int(round(sy + dy * hi))))


def drop_hanging(cuts=()):
    """remove tracks and vias with a free end (or cut a track back to its last join), over and over until none is
    left; returns how many went or were cut back"""
    pads = {}
    for fp in b.GetFootprints():
        for p in fp.Pads():
            pads.setdefault(p.GetNetCode(), []).append(p)
    gnd = b.FindNet("GND").GetNetCode() if b.FindNet("GND") else -1
    TOUCH = pcbnew.FromMM(0.01)
    hang = 0
    while True:
        by_net = {}
        for t in b.GetTracks():
            by_net.setdefault(t.GetNetCode(), []).append(t)
        drop, trims = [], []
        for net, ts in by_net.items():
            for t in ts:
                # copper touching counts, as in KiCad: within the item's own half-width (+0.01 mm)
                if t.GetClass() == "PCB_VIA":
                    q = t.GetPosition()
                    on = {lay for lay in (pcbnew.B_Cu, pcbnew.F_Cu)
                          if any(o is not t and o.GetClass() != "PCB_VIA" and o.GetLayer() == lay and
                                 o.HitTest(q, t.GetWidth(lay) // 2 + TOUCH) for o in ts)
                          or any(p.IsOnLayer(lay) and p.HitTest(q, t.GetWidth(lay) // 2 + TOUCH) for p in pads.get(net, []))}
                    if net == gnd:
                        on.add(pcbnew.F_Cu)   # the foil itself
                    if len(on) < 2:
                        drop.append(t)
                    continue
                lay = t.GetLayer()
                hw = t.GetWidth() // 2 + TOUCH
                ends_ok = []
                for q in (t.GetStart(), t.GetEnd()):
                    ends_ok.append(
                        any(p.IsOnLayer(lay) and p.HitTest(q, hw) for p in pads.get(net, []))
                        or any(o is not t and (o.GetClass() == "PCB_VIA" or o.GetLayer() == lay) and o.HitTest(q, hw)
                               for o in ts)
                        or (net == gnd and lay == pcbnew.F_Cu)
                        or any(abs(q.x - c.x) + abs(q.y - c.y) < TOUCH for c in cuts))
                if all(ends_ok):
                    continue
                span = free_span(t, ends_ok, ts, pads.get(net, []))
                if span is None:
                    drop.append(t)
                else:
                    trims.append((t, span))
        if not drop and not trims:
            break
        for t in drop:
            b.Delete(t)
        for t, (lo, hi) in trims:
            apply_span(t, lo, hi)
        hang += len(drop) + len(trims)
    return hang


def prune():
    """fix mode: drop copper KiCad finds clashing, then whatever hangs loose, until nothing does"""
    import json, subprocess, os, tempfile
    rpt = OUT + ".fix_drc.json"
    subprocess.run([KCLI, "pcb", "drc", "--format", "json", "--severity-error", "--refill-zones", "-o", rpt, IN],
                   capture_output=True)
    bad = set()
    for v in json.load(open(rpt, encoding="utf-8"))["violations"]:
        if v["type"] in ("clearance", "shorting_items", "tracks_crossing", "hole_clearance", "hole_to_hole",
                         "copper_edge_clearance"):
            bad.update(it.get("uuid") for it in v["items"])
    gone = 0
    for t in list(b.GetTracks()):
        if t.m_Uuid.AsString() in bad:
            b.Delete(t)
            gone += 1
    # copper round the named parts goes too; the ends it leaves outside count as connected (the new copper joins them)
    cuts = []
    if AROUND:
        boxes = []
        for ref in AROUND:
            r = b.FindFootprintByReference(ref).GetBoundingBox(False)
            m = pcbnew.FromMM(2.5)
            boxes.append((r.GetX() - m, r.GetY() - m, r.GetRight() + m, r.GetBottom() + m))
        for t in list(b.GetTracks()):
            r = t.GetBoundingBox()
            if any(r.GetX() <= x2 and r.GetRight() >= x1 and r.GetY() <= y2 and r.GetBottom() >= y1 for x1, y1, x2, y2 in boxes):
                if t.GetClass() != "PCB_VIA":
                    cuts += [pcbnew.VECTOR2I(int(t.GetStart().x), int(t.GetStart().y)),
                             pcbnew.VECTOR2I(int(t.GetEnd().x), int(t.GetEnd().y))]
                else:   # a track that ended on this via is cut here too
                    cuts.append(pcbnew.VECTOR2I(int(t.GetPosition().x), int(t.GetPosition().y)))
                b.Delete(t)
                gone += 1
    hang = drop_hanging(cuts)
    print("fix: %d tracks/vias removed (clashing%s), %d left hanging removed" % (gone, " or round " + ",".join(AROUND) if AROUND else "", hang))


if FIX:
    prune()
bb = b.GetBoardEdgesBoundingBox()
OX, OY = pcbnew.ToMM(bb.GetX()) + 0.05, pcbnew.ToMM(bb.GetY()) + 0.05
BW, BH = pcbnew.ToMM(bb.GetWidth()) - 0.1, pcbnew.ToMM(bb.GetHeight()) - 0.1
NX, NY = int(BW / G) + 1, int(BH / G) + 1
NN = NX * NY

nid = {"": 0}
for n in b.GetNetsByName().keys():
    s = str(n)
    if s and s not in nid:
        nid[s] = len(nid)
names = {v: k for k, v in nid.items()}


def short(n):
    return n.split("/")[-1]


short_to_full = {short(k): k for k in nid if k}
GND = nid[short_to_full["GND"]]

owner = np.zeros((2, NY, NX), dtype=np.int32)   # 0 free, >0 net, -1 blocked for all
gx = OX + np.arange(NX) * G
gy = OY + np.arange(NY) * G


def cells(x1, y1, x2, y2):
    i1, i2 = max(0, int(math.floor((x1 - OX) / G))), min(NX - 1, int(math.ceil((x2 - OX) / G)))
    j1, j2 = max(0, int(math.floor((y1 - OY) / G))), min(NY - 1, int(math.ceil((y2 - OY) / G)))
    return i1, i2, j1, j2


def paint_box(layer, x1, y1, x2, y2, val):
    i1, i2, j1, j2 = cells(x1, y1, x2, y2)
    if i2 >= i1 and j2 >= j1:
        X, Y = np.meshgrid(gx[i1:i2 + 1], gy[j1:j2 + 1])
        m = (X >= x1) & (X <= x2) & (Y >= y1) & (Y <= y2)
        owner[layer, j1:j2 + 1, i1:i2 + 1][m] = val


def paint_seg(layer, x1, y1, x2, y2, r, val, only_free=False):
    i1, i2, j1, j2 = cells(min(x1, x2) - r, min(y1, y2) - r, max(x1, x2) + r, max(y1, y2) + r)
    if i2 < i1 or j2 < j1:
        return
    X, Y = np.meshgrid(gx[i1:i2 + 1], gy[j1:j2 + 1])
    dx, dy = x2 - x1, y2 - y1
    L2 = dx * dx + dy * dy
    t = np.zeros_like(X) if L2 == 0 else np.clip(((X - x1) * dx + (Y - y1) * dy) / L2, 0, 1)
    m = (X - (x1 + t * dx)) ** 2 + (Y - (y1 + t * dy)) ** 2 <= r * r
    sub = owner[layer, j1:j2 + 1, i1:i2 + 1]
    if only_free:
        m &= sub == 0
    sub[m] = val


# board edge band
for layer in (B, F):
    k = int(math.ceil((EDGE - CLR) / G))   # the clearance dilation adds CLR
    owner[layer, :k, :] = -1
    owner[layer, -k:, :] = -1
    owner[layer, :, :k] = -1
    owner[layer, :, -k:] = -1

# pads
pads_of = {}      # net id -> list of (footprint ref, pad number, x, y, layers)
for fp in b.GetFootprints():
    for p in fp.Pads():
        if not p.GetNumber() and p.GetAttribute() == pcbnew.PAD_ATTRIB_SMD:
            continue   # unnamed paste / thermal helper inside an exposed pad
        n = p.GetNetname()
        v = nid.get(n, 0) if n and not n.startswith("unconnected") else -1
        attr = p.GetAttribute()
        if attr == pcbnew.PAD_ATTRIB_NPTH:
            layers, v = (B, F), -1
        elif attr == pcbnew.PAD_ATTRIB_PTH:
            layers = (B, F)
        else:
            layers = (B,) if p.IsOnLayer(pcbnew.B_Cu) else ((F,) if p.IsOnLayer(pcbnew.F_Cu) else ())
        q = p.GetPosition()
        px, py = pcbnew.ToMM(q.x), pcbnew.ToMM(q.y)
        for layer in layers:
            r = p.GetBoundingBox()
            x1, y1, x2, y2 = pcbnew.ToMM(r.GetX()), pcbnew.ToMM(r.GetY()), pcbnew.ToMM(r.GetRight()), pcbnew.ToMM(r.GetBottom())
            shape = p.GetShape(LID[layer])
            if attr == pcbnew.PAD_ATTRIB_NPTH:
                hr = pcbnew.ToMM(max(p.GetDrillSize().x, p.GetDrillSize().y)) / 2 + (HOLE_CLR - CLR) + 0.03
                paint_seg(layer, px, py, px, py, hr, -1)
            elif shape == pcbnew.PAD_SHAPE_CIRCLE:
                paint_seg(layer, px, py, px, py, (x2 - x1) / 2, v)
            elif shape == pcbnew.PAD_SHAPE_OVAL:
                w, h = x2 - x1, y2 - y1
                if w >= h:
                    paint_seg(layer, x1 + h / 2, py, x2 - h / 2, py, h / 2, v)
                else:
                    paint_seg(layer, px, y1 + w / 2, px, y2 - w / 2, w / 2, v)
            else:
                paint_box(layer, x1, y1, x2, y2, v)
        if v > 0:
            pads_of.setdefault(v, []).append((fp.GetReference(), p.GetNumber(), px, py, layers, p))

# tracks and vias already on the board (hdmi_breakout.py): fixed copper, never ripped up; the pads they start or end
# on are remembered so the ground fan-out leaves them alone
prewired = set()
pre_nets = set()
for t in ([] if TIDY or SWAP else b.GetTracks()):
    v = nid.get(t.GetNetname(), 0)
    if v <= 0:
        continue
    pre_nets.add(v)
    if t.GetClass() == "PCB_VIA":
        q = t.GetPosition()
        for entry in pads_of.get(v, []):
            if entry[5].HitTest(q):
                prewired.add((entry[0], entry[1]))
        for layer in (B, F):
            paint_seg(layer, pcbnew.ToMM(q.x), pcbnew.ToMM(q.y), pcbnew.ToMM(q.x), pcbnew.ToMM(q.y), VIA_D / 2, v)
        continue
    layer = B if t.GetLayer() == pcbnew.B_Cu else F
    s, e = t.GetStart(), t.GetEnd()
    paint_seg(layer, pcbnew.ToMM(s.x), pcbnew.ToMM(s.y), pcbnew.ToMM(e.x), pcbnew.ToMM(e.y), pcbnew.ToMM(t.GetWidth()) / 2, v)
    for q in (s, e):
        for entry in pads_of.get(v, []):
            if entry[5].HitTest(q):
                prewired.add((entry[0], entry[1]))

# room for the foil's thermal spokes round through-hole ground pads: the zone keeps 0.5 from the pad, needs 0.25 of
# copper and 0.3 to anything else, so other nets' foil-side copper stays 1.05 off the pad (0.9 + the clearance)
for entry in pads_of.get(GND, []):
    p = entry[5]
    if p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH:
        r = p.GetBoundingBox()
        hw = max(pcbnew.ToMM(r.GetWidth()), pcbnew.ToMM(r.GetHeight())) / 2
        paint_seg(F, entry[2], entry[3], entry[2], entry[3], hw + 0.9, GND, only_free=True)

# no tracks under the ESP32 module body on the back (between the pad rows and under the antenna end)
for fp in b.GetFootprints():
    if fp.GetReference() == "U2":
        c = fp.GetPosition()
        cx, cy = pcbnew.ToMM(c.x), pcbnew.ToMM(c.y)
        o = fp.GetOrientationDegrees()
        # native body: x +-9.0, y -16.4..16.3; pads at x +-8.75, inner ends at +-8.3
        pts = []
        for (x, y) in ((-8.2, -16.2), (8.2, -16.2), (8.2, 16.1), (-8.2, 16.1)):
            pts.append((x, y))
        # the module is on the back and turned; map the native box through its pad positions
        pp = {p.GetNumber(): p for p in fp.Pads()}
        p1, p19, p20 = pp["1"].GetPosition(), pp["19"].GetPosition(), pp["20"].GetPosition()
        xs = [pcbnew.ToMM(v.x) for v in (p1, p19, p20)]
        ys = [pcbnew.ToMM(v.y) for v in (p1, p19, p20)]
        # pad rows run along x (module lying): rows at y of p1 and p20
        rowy1, rowy2 = sorted((pcbnew.ToMM(p1.y), pcbnew.ToMM(p20.y)))
        xa, xb_ = sorted((pcbnew.ToMM(p1.x), pcbnew.ToMM(p19.x)))
        body = fp.GetCourtyard(pcbnew.B_CrtYd).BBox()
        bx1, bx2 = pcbnew.ToMM(body.GetX()) + 0.3, pcbnew.ToMM(body.GetRight()) - 0.3
        for layer in (B,):
            i1, i2, j1, j2 = cells(bx1, rowy1 + 0.75, bx2, rowy2 - 0.75)
            sub = owner[layer, j1:j2 + 1, i1:i2 + 1]
            sub[sub == 0] = -1
            # antenna end beyond the pad rows, full module width
            if abs(bx1 - xa) > abs(bx2 - xb_):
                i1, i2, j1, j2 = cells(bx1, rowy1 - 0.6, xa - 1.0, rowy2 + 0.6)
            else:
                i1, i2, j1, j2 = cells(xb_ + 1.0, rowy1 - 0.6, bx2, rowy2 + 0.6)
            sub = owner[layer, j1:j2 + 1, i1:i2 + 1]
            sub[sub == 0] = -1

print("raster %dx%d, pads painted, %d nets prewired, %.1fs" % (NX, NY, len(pre_nets), time.time() - t0))
static = owner.copy()          # pads, edge, keepouts, prewired copper: never ripped up
items = {}                     # net -> board items it has been given


def pre_groups(net):
    """pads of a prewired net joined by that copper: list of (pad entries, cells); a via or a through pad joins layers"""
    m = owner == net
    seen = np.zeros_like(m)
    comp = {}
    for l, j, i in zip(*[a.tolist() for a in np.nonzero(m)]):
        if seen[l, j, i]:
            continue
        cid = len(comp)
        cells_, stack = set(), [(l, j, i)]
        seen[l, j, i] = True
        while stack:
            c = stack.pop()
            cells_.add(c)
            l0, j0, i0 = c
            for l1, j1, i1 in ((l0, j0 + 1, i0), (l0, j0 - 1, i0), (l0, j0, i0 + 1), (l0, j0, i0 - 1), (1 - l0, j0, i0)):
                if 0 <= j1 < NY and 0 <= i1 < NX and m[l1, j1, i1] and not seen[l1, j1, i1]:
                    seen[l1, j1, i1] = True
                    stack.append((l1, j1, i1))
        comp[cid] = cells_
    groups = {}
    for entry in pads_of.get(net, []):
        q = entry[5].GetPosition()
        c = (entry[4][0], int(round((pcbnew.ToMM(q.y) - OY) / G)), int(round((pcbnew.ToMM(q.x) - OX) / G)))
        cid = next((k for k, cs in comp.items() if c in cs), None)
        if cid is not None:
            groups.setdefault(cid, []).append(entry)
    return [(pads, comp[cid]) for cid, pads in groups.items()]


GROUPS = {v: pre_groups(v) for v in pre_nets if v != GND}


def disk(r):
    k = int(math.ceil(r))
    return [(dy, dx) for dy in range(-k, k + 1) for dx in range(-k, k + 1) if dx * dx + dy * dy <= r * r]


def dilate(mask, r):
    out = np.zeros_like(mask)
    for dy, dx in disk(r):
        ys = slice(max(0, dy), NY + min(0, dy)); yd = slice(max(0, -dy), NY + min(0, -dy))
        xs = slice(max(0, dx), NX + min(0, dx)); xd = slice(max(0, -dx), NX + min(0, -dx))
        out[yd, xd] |= mask[ys, xs]
    return out


SQ2 = math.sqrt(2)
DIRS = [(1, 0, 1.0), (-1, 0, 1.0), (0, 1, 1.0), (0, -1, 1.0), (1, 1, SQ2), (1, -1, SQ2), (-1, 1, SQ2), (-1, -1, SQ2)]


def astar(srcs, tgts, tgt_pts, blocked, via_ok, allow_f, cost_f=None, soft=None, penalty=0.0, cost_via=None):
    """srcs, tgts: sets of (layer, iy, ix). Returns the node path or None. soft: cells another net's track
    holds, passable at a penalty (to find which nets to rip up)."""
    bl = [blocked[0].tobytes(), blocked[1].tobytes()]
    sf = [soft[0].tobytes(), soft[1].tobytes()] if soft is not None else None
    vo = via_ok.tobytes()
    tgt = set(l * NN + j * NX + i for (l, j, i) in tgts)

    def h(j, i):
        return min(max(abs(i - ti), abs(j - tj)) + (SQ2 - 1) * min(abs(i - ti), abs(j - tj)) for tj, ti in tgt_pts)

    openl, gsc, par = [], {}, {}
    for (l, j, i) in srcs:
        k = l * NN + j * NX + i
        if bl[l][j * NX + i]:
            continue
        gsc[k] = 0.0
        heapq.heappush(openl, (h(j, i) * G, 0.0, k))
        par[k] = -1
    closed = set()
    n = 0
    while openl:
        f, g, k = heapq.heappop(openl)
        if k in closed:
            continue
        closed.add(k)
        n += 1
        if n > 1500000:
            return None, n
        if k in tgt:
            path = [k]
            while par[k] != -1:
                k = par[k]
                path.append(k)
            return path[::-1], n
        l, r = divmod(k, NN)
        j, i = divmod(r, NX)
        cl = 1.0 if l == B else (cost_f or COST_F)
        for di, dj, st in DIRS:
            ii, jj = i + di, j + dj
            if ii < 0 or jj < 0 or ii >= NX or jj >= NY:
                continue
            rr = jj * NX + ii
            if bl[l][rr]:
                continue
            if di and dj and (bl[l][j * NX + ii] or bl[l][jj * NX + i]):
                continue
            kk = l * NN + rr
            ng = g + st * G * cl + (penalty if sf is not None and sf[l][rr] else 0.0)
            if ng < gsc.get(kk, 1e18):
                gsc[kk] = ng
                par[kk] = k
                heapq.heappush(openl, (ng + h(jj, ii) * G, ng, kk))
        if allow_f and vo[r]:
            l2 = 1 - l
            if not bl[l2][r]:
                kk = l2 * NN + r
                ng = g + (COST_VIA if cost_via is None else cost_via)
                if ng < gsc.get(kk, 1e18):
                    gsc[kk] = ng
                    par[kk] = k
                    heapq.heappush(openl, (ng + h(j, i) * G, ng, kk))
    return None, n


def to_mm(i, j):
    return OX + i * G, OY + j * G


def add_track(net, layer, pts, w):
    ni = b.FindNet(names[net])
    for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(b)
        t.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(x1), pcbnew.FromMM(y1)))
        t.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(x2), pcbnew.FromMM(y2)))
        t.SetWidth(pcbnew.FromMM(w))
        t.SetLayer(LID[layer])
        t.SetNet(ni)
        b.Add(t)
        items.setdefault(net, []).append(t)
        paint_seg(layer, x1, y1, x2, y2, w / 2, net, only_free=True)


def add_via(net, x, y):
    v = pcbnew.PCB_VIA(b)
    v.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
    v.SetWidth(pcbnew.FromMM(VIA_D))
    v.SetDrill(pcbnew.FromMM(VIA_H))
    v.SetNet(b.FindNet(names[net]))
    b.Add(v)
    items.setdefault(net, []).append(v)
    for layer in (B, F):
        paint_seg(layer, x, y, x, y, VIA_D / 2, net, only_free=True)


def emit(net, path, w):
    """node path -> straight segments and vias"""
    runs, cur = [], None
    for k in path:
        l, r = divmod(k, NN)
        j, i = divmod(r, NX)
        if cur is None or cur[0] != l:
            if cur is not None:
                add_via(net, *to_mm(*cur[1][-1]))
            cur = (l, [(i, j)])
            runs.append(cur)
        else:
            cur[1].append((i, j))
    jumps = []
    for l, pts in runs:
        simp = [pts[0]]
        for a, c in zip(pts[1:], pts[2:]):
            p0 = simp[-1]
            if (a[0] - p0[0]) * (c[1] - a[1]) != (a[1] - p0[1]) * (c[0] - a[0]):
                simp.append(a)
        if len(pts) > 1:
            simp.append(pts[-1])
        mm = [to_mm(i, j) for i, j in simp]
        if len(mm) > 1:
            add_track(net, l, mm, w)
        if l == F:
            jumps.append(sum(math.hypot(x2 - x1, y2 - y1) for (x1, y1), (x2, y2) in zip(mm, mm[1:])))
    return jumps


def net_cells(net, layers=(B, F)):
    out = set()
    for l in layers:
        js, iss = np.nonzero(owner[l] == net)
        out.update(zip([l] * len(js), js.tolist(), iss.tolist()))
    return out


def pad_cells(pad_entry):
    ref, num, px, py, layers, p = pad_entry
    r = p.GetBoundingBox()
    i1, i2, j1, j2 = cells(pcbnew.ToMM(r.GetX()), pcbnew.ToMM(r.GetY()), pcbnew.ToMM(r.GetRight()), pcbnew.ToMM(r.GetBottom()))
    net = nid[p.GetNetname()]
    out = set()
    for l in layers:
        sub = owner[l, j1:j2 + 1, i1:i2 + 1]
        js, iss = np.nonzero(sub == net)
        out.update((l, j1 + a, i1 + c) for a, c in zip(js.tolist(), iss.tolist()))
    return out


# ---------------------------------------------------------------- GND: vias from every back SMD ground pad to the foil
EP = {("U2", "39"), ("U7", "7"), ("U9", "7"), ("U8", "9"), ("U10", "9")}
# ground via direction (degrees, board frame, y down: 90 = south, 270 = north) where the outward one would block a
# signal corridor: the DAC's charge pump and AVDD caps keep their vias off the analogue outputs' way
FANOUT_DIR = {("C37", "2"): 270, ("C31", "2"): 270, ("C30", "2"): 270, ("C33", "2"): 90, ("C32", "2"): 90,
              ("U3", "9"): 90, ("U3", "10"): 90}


def gnd_fanout():
    other = (owner[B] != 0) & (owner[B] != GND)
    otherF = (owner[F] != 0) & (owner[F] != GND)
    vrad = (VIA_D / 2 + CLR) / G + 0.71
    trad = (0.15 + CLR) / G + 0.71
    via_ok = ~(dilate(other, vrad) | dilate(otherF, vrad))
    tr_ok = ~dilate(other, trad)
    vias = []
    added, missed = 0, []
    gpads = sorted(pads_of.get(GND, []), key=lambda e: (e[0], e[1]) not in EP)
    for entry in gpads:
        ref, num, px, py, layers, p = entry
        if layers != (B,) or (ref, num) in prewired:
            continue
        r = p.GetBoundingBox()
        w, h = pcbnew.ToMM(r.GetWidth()), pcbnew.ToMM(r.GetHeight())
        ep = (ref, num) in EP
        if ep:   # exposed pad: vias inside, one per sub-pad or a 2x2 set in a big pad
            cands = [(px, py, 0)] if max(w, h) < 2.0 else [(px + fx * (w - 0.8) / 2, py + fy * (h - 0.8) / 2, 0)
                                                            for fx in (-1, 1) for fy in (-1, 1)]
        else:
            fpc = p.GetParentFootprint()
            c = fpc.GetPosition()
            ox, oy = px - pcbnew.ToMM(c.x), py - pcbnew.ToMM(c.y)
            base = math.degrees(math.atan2(oy, ox)) if (ox or oy) else 0.0
            base = FANOUT_DIR.get((ref, num), base)
            body = fpc.GetCourtyard(pcbnew.B_CrtYd if fpc.IsFlipped() else pcbnew.F_CrtYd).BBox()
            bx1, by1 = pcbnew.ToMM(body.GetX()) + 0.2, pcbnew.ToMM(body.GetY()) + 0.2
            bx2, by2 = pcbnew.ToMM(body.GetRight()) - 0.2, pcbnew.ToMM(body.GetBottom()) - 0.2
            cands = []
            for d in (0.9, 1.1, 1.3, 1.6, 2.0, 2.5):
                for a in sorted(range(0, 360, 30), key=lambda a: abs((a - base + 180) % 360 - 180)):
                    vx, vy = px + d * math.cos(math.radians(a)), py + d * math.sin(math.radians(a))
                    if not (bx1 < vx < bx2 and by1 < vy < by2):
                        cands.append((vx, vy, d))
        done = 0
        for (vx, vy, d) in cands:
            i, j = int(round((vx - OX) / G)), int(round((vy - OY) / G))
            if not (0 <= i < NX and 0 <= j < NY):
                continue
            if ep:   # inside the exposed pad: the whole via ring on its copper
                k = int(math.ceil((VIA_D / 2) / G))
                if not (owner[B, j - k:j + k + 1, i - k:i + k + 1] == GND).all():
                    continue
            elif not via_ok[j, i]:
                continue
            if any(math.hypot(vx - ax, vy - ay) < 0.8 for ax, ay in vias):
                continue
            n = max(2, int(d / G))
            if d and not all(tr_ok[int(round((py + (vy - py) * s / n - OY) / G)), int(round((px + (vx - px) * s / n - OX) / G))]
                             for s in range(n + 1)):
                continue
            add_via(GND, vx, vy)
            vias.append((vx, vy))
            if d:
                add_track(GND, B, [(px, py), (vx, vy)], round(min(0.3, 0.9 * min(w, h)), 2))
            added += 1
            done += 1
            if not ep or done >= 4:
                break
        if not done:
            missed.append("%s.%s(%.1fx%.1f)" % (ref, num, w, h))
    # pads with no room for a via: the shortest track on the back to a ground via within 6 mm
    still = []
    if missed:
        other = (owner != 0) & (owner != GND)
        rt = (0.125 + CLR) / G + 0.71
        blocked = np.stack([dilate(other[B], rt), dilate(other[F], rt)])
        nvok = np.zeros((NY, NX), dtype=bool)
        k = int(math.ceil(0.25 / G))
        for entry in [e for e in gpads if "%s.%s" % e[:2] in [m.split("(")[0] for m in missed]]:
            ref, num, px, py, layers, p = entry
            near = [(vx, vy) for vx, vy in vias if math.hypot(vx - px, vy - py) < 6.0]
            tgts = set()
            for vx, vy in near:
                ci, cj = int(round((vx - OX) / G)), int(round((vy - OY) / G))
                tgts.update((B, cj + dj, ci + di) for dj in range(-k, k + 1) for di in range(-k, k + 1))
            tps = [(int(round((vy - OY) / G)), int(round((vx - OX) / G))) for vx, vy in near][:6]
            path = None
            if tgts:
                path, _ = astar(pad_cells(entry), tgts, tps, blocked, nvok, False)
            if path is None:
                still.append("%s.%s" % (ref, num))
                continue
            emit(GND, path, 0.25)
        missed = still
    print("GND vias: %d, no room: %s" % (added, " ".join(missed) or "none"))


if TIDY or SWAP:
    exec(open(__file__.replace("route_grid.py", "track_tidy.py" if TIDY else "track_swap.py"), encoding="utf-8").read())
    sys.exit(0)
gnd_fanout()


# ---------------------------------------------------------------- signal and supply nets
def masks(net, w):
    other = (owner != 0) & (owner != net)
    rt = (w / 2 + CLR) / G + 0.71
    vr = (VIA_D / 2 + CLR) / G + 0.71
    return (np.stack([dilate(other[B], rt), dilate(other[F], rt)]),
            ~(dilate(other[B], vr) | dilate(other[F], vr)))


def route_net(full, quiet=False):
    """route every connection of a net from scratch; returns the pads that failed, the jump lengths, the tree"""
    net = nid[full]
    pads = pads_of.get(net, [])
    sn = short(full)
    w = WIDE.get(sn, 0.25)
    allow_f = sn not in NO_JUMP
    narrow = None
    left = list(pads)
    st = START.get(sn)
    first = next((e for e in left if st and e[0] == st[0] and e[1] == st[1]), left[0])
    left.remove(first)
    tree_pads = [first]
    tree = set(pad_cells(first))

    def absorb(entry):   # pads the prewired copper already joins to this one come into the tree with it
        for gp, gc in GROUPS.get(net, []):
            if entry in gp:
                tree.update(gc)
                for e in gp:
                    if e in left:
                        left.remove(e)
                        tree_pads.append(e)

    def target(entry):   # a pad and the copper already on it: reaching either connects it
        tc = pad_cells(entry)
        for gp, gc in GROUPS.get(net, []):
            if entry in gp:
                tc |= gc
        return tc

    absorb(first)
    jumps, fails, nexp = [], [], 0
    if left:
        blocked, vok = masks(net, w)
    while left:
        best = min(left, key=lambda e: min(math.hypot(e[2] - t[2], e[3] - t[3]) for t in tree_pads))
        left.remove(best)
        tc = target(best)
        if tc & tree:
            tree_pads.append(best)
            tree |= tc
            absorb(best)
            continue
        tp = [(int(round((best[3] - OY) / G)), int(round((best[2] - OX) / G)))]
        extra = sorted({(j, i) for (l, j, i) in tc})   # copper already on the pad guides the search too
        tp += extra[::max(1, len(extra) // 24)]
        path, n = astar(tree, tc, tp, blocked, vok, allow_f, COST_F_NET.get(sn), cost_via=COST_VIA_NET.get(sn))
        nexp += n
        ww = w
        if path is None and w > 0.25:   # neck down to 0.25 mm for this connection
            ww = 0.25
            if narrow is None:
                narrow, _ = masks(net, ww)
            path, n = astar(tree, tc, tp, narrow, vok, allow_f, COST_F_NET.get(sn), cost_via=COST_VIA_NET.get(sn))
            nexp += n
        if path is None:
            fails.append(best)
            continue
        jumps += emit(net, path, ww)
        tree_pads.append(best)
        for k in path:
            l, r = divmod(k, NN)
            tree.add((l, r // NX, r % NX))
        tree |= tc
        absorb(best)
    if not quiet and (nexp or fails or not FIX):
        print("%-12s pads %2d  fails %-24s jumps %s  (%d nodes, %.0fs)" % (
            sn, len(pads), " ".join("%s.%s" % f[:2] for f in fails) or "-",
            " ".join("%.1f" % j for j in jumps) or "-", nexp, time.time() - t0))
        sys.stdout.flush()
    return fails, jumps, tree


def rip(net):
    for it in items.pop(net, []):
        b.Delete(it)
    for l in (B, F):
        m = (owner[l] == net) & (static[l] != net)
        owner[l][m] = 0


def blockers(full, tree, pad):
    """nets whose tracks stand between the routed part of a net and one of its failed pads"""
    net = nid[full]
    sn = short(full)
    w = min(WIDE.get(sn, 0.25), 0.25)
    rt = (w / 2 + CLR) / G + 0.71
    hard_other = (static != 0) & (static != net)
    gnd_dyn = (owner == GND) & (static == 0)
    dyn_other = (owner != 0) & (owner != net) & (static == 0) & ~gnd_dyn
    hard = np.stack([dilate(hard_other[B] | gnd_dyn[B], rt), dilate(hard_other[F] | gnd_dyn[F], rt)])
    soft = np.stack([dilate(dyn_other[B], rt), dilate(dyn_other[F], rt)]) & ~hard
    vr = (VIA_D / 2 + CLR) / G + 0.71
    vok = ~(dilate(hard_other[B] | gnd_dyn[B], vr) | dilate(hard_other[F] | gnd_dyn[F], vr))
    tc = pad_cells(pad)
    tp = [(int(round((pad[3] - OY) / G)), int(round((pad[2] - OX) / G)))]
    path, _ = astar(tree, tc, tp, hard, vok, sn not in NO_JUMP, COST_F_NET.get(sn), soft, penalty=2.0)
    if path is None:
        return None
    k = int(math.ceil(rt))
    found = set()
    for node in path:
        l, r = divmod(node, NN)
        j, i = divmod(r, NX)
        if soft[l, j, i]:
            win = owner[l, max(0, j - k):j + k + 1, max(0, i - k):i + k + 1]
            stc = static[l, max(0, j - k):j + k + 1, max(0, i - k):i + k + 1]
            found.update(int(v) for v in np.unique(win[(win > 0) & (stc == 0)]) if v != net and v != GND)
    return found


report = {}
todo = [short_to_full[n] for n in ORDER if n in short_to_full]
rest = [k for k in nid if k and k not in todo and short(k) != "GND" and nid[k] in pads_of and len(pads_of[nid[k]]) > 1]
order = [f for f in todo + rest if len(pads_of.get(nid[f], [])) > 1]
trees = {}
for full in order:
    fails, jumps, tree = route_net(full)
    report[full] = (fails, jumps)
    trees[full] = tree

# rip up what stands in the way of a failed connection, route that net first, then the ripped nets again
for rnd in range(8):
    failed = [f for f in order if report[f][0]]
    if not failed:
        break
    print("-- round %d: %s" % (rnd + 1, " ".join(short(f) for f in failed)))
    for full in failed:
        if not report[full][0]:
            continue
        found = set()
        for pad in report[full][0]:
            bl = blockers(full, trees[full], pad)
            if bl:
                found |= bl
        if not found:
            print("   %s: nothing to rip up" % short(full))
            continue
        # nets that may not jump to the foil are never ripped up for others: the others jump over them
        victims = [f for f in order if nid[f] in found and short(f) not in NO_JUMP]
        print("   %s: rip up %s" % (short(full), " ".join(short(f) for f in victims)))
        rip(nid[full])
        for v in victims:
            rip(nid[v])
        for f in [full] + victims:
            fails, jumps, tree = route_net(f, quiet=True)
            report[f] = (fails, jumps)
            trees[f] = tree
            if fails:
                print("   still failing: %s %s" % (short(f), " ".join("%s.%s" % x[:2] for x in fails)))
        sys.stdout.flush()

# two vias of one net side by side (two connections changing layer at one spot): keep the first, bridge the other
# to it on both layers with a track inside their joint copper, and drop it
vias = [t for t in b.GetTracks() if t.GetClass() == "PCB_VIA"]
gone, merged = set(), 0
for ia, va in enumerate(vias):
    if ia in gone:
        continue
    pa = va.GetPosition()
    for ib in range(ia + 1, len(vias)):
        vb = vias[ib]
        if ib in gone or vb.GetNetCode() != va.GetNetCode():
            continue
        pb = vb.GetPosition()
        dd = math.hypot(pcbnew.ToMM(pa.x - pb.x), pcbnew.ToMM(pa.y - pb.y))
        if dd < VIA_H + 0.25:   # hole to hole
            w = max(0.15, 2 * math.sqrt(max(0.0, (VIA_D / 2) ** 2 - (dd / 2) ** 2)) - 0.02)
            for layer in (B, F):
                t = pcbnew.PCB_TRACK(b)
                t.SetStart(pb)
                t.SetEnd(pa)
                t.SetWidth(pcbnew.FromMM(w))
                t.SetLayer(LID[layer])
                t.SetNet(va.GetNet())
                b.Add(t)
            b.Delete(vb)
            gone.add(ib)
            merged += 1
print("close vias merged: %d" % merged)
# ---------------------------------------------------------------- KiCad's own check of the result
# the raster can miss a pad corner by a few hundredths of a mm; a net KiCad finds too close is laid again, with the
# copper it came too close to drawn over every cell it touches
import json, subprocess
KCLI = "C:/Users/geniy/AppData/Local/Programs/KiCad/10.0/bin/kicad-cli.exe"
CHECK = ("clearance", "hole_clearance", "tracks_crossing", "shorting_items")


def kicad_violations(types=CHECK, severity="--severity-error"):
    pcbnew.SaveBoard(OUT, b)
    rpt = OUT + ".drc.json"
    subprocess.run([KCLI, "pcb", "drc", "--format", "json", severity, "--refill-zones", "-o", rpt, OUT],
                   capture_output=True)
    return [v for v in json.load(open(rpt, encoding="utf-8"))["violations"] if v["type"] in types]


def fatten(uuid):
    """repaint a pad over every cell it touches, on free cells only"""
    for fp in b.GetFootprints():
        for p in fp.Pads():
            if p.m_Uuid.AsString() != uuid:
                continue
            n = p.GetNetname()
            v = nid.get(n, 0) if n and not n.startswith("unconnected") else -1
            r = p.GetBoundingBox()
            x1, y1 = pcbnew.ToMM(r.GetX()) - G / 2, pcbnew.ToMM(r.GetY()) - G / 2
            x2, y2 = pcbnew.ToMM(r.GetRight()) + G / 2, pcbnew.ToMM(r.GetBottom()) + G / 2
            i1, i2, j1, j2 = cells(x1, y1, x2, y2)
            X, Y = np.meshgrid(gx[i1:i2 + 1], gy[j1:j2 + 1])
            m = (X >= x1) & (X <= x2) & (Y >= y1) & (Y <= y2)
            for l in ([B, F] if p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH else [B]):
                sub, sst = owner[l, j1:j2 + 1, i1:i2 + 1], static[l, j1:j2 + 1, i1:i2 + 1]
                mm_ = m & (sst == 0)
                sub[mm_ & (sub == 0)] = v
                sst[mm_] = v
            return True
    return False


for rnd in range(2):
    viol = kicad_violations()
    print("KiCad check %d: %d clearance violations" % (rnd + 1, len(viol)))
    if not viol:
        break
    own = {it.m_Uuid.AsString(): net for net, its in items.items() for it in its}
    redo = set()
    for v in viol:
        uu = [it.get("uuid") for it in v["items"]]
        hit = [own[u] for u in uu if u in own and own[u] != GND]
        if not hit:
            continue
        redo.add(hit[-1])
        for u in uu:
            if u not in own:
                fatten(u)
    for net in redo:
        full = names[net]
        rip(net)
        fails, jumps, tree = route_net(full)
        report[full] = (fails, jumps)

# stubs with a free end (cut-back branches the new copper did not reuse): a free end carries nothing
if FIX:
    print("hanging stubs removed: %d" % drop_hanging())
for rnd in range(3):   # and whatever KiCad still calls dangling: cut back to the last join, or drop
    viol = kicad_violations(("track_dangling", "via_dangling"), "--severity-all")
    flagged = {}
    for v in viol:
        for it in v["items"]:
            flagged[it.get("uuid")] = it.get("pos", {})
    gone = [t for t in b.GetTracks() if t.m_Uuid.AsString() in flagged]
    if not gone:
        break
    padmap = {}
    for fp in b.GetFootprints():
        for p in fp.Pads():
            padmap.setdefault(p.GetNetCode(), []).append(p)
    n_cut = 0
    for t in gone:
        if t.GetClass() == "PCB_VIA":
            b.Delete(t)
            n_cut += 1
            continue
        pos = flagged[t.m_Uuid.AsString()]
        fx, fy = pcbnew.FromMM(pos.get("x", 0)), pcbnew.FromMM(pos.get("y", 0))
        s0, e0 = t.GetStart(), t.GetEnd()
        free_end = 0 if (s0.x - fx) ** 2 + (s0.y - fy) ** 2 < (e0.x - fx) ** 2 + (e0.y - fy) ** 2 else 1
        ts = [o for o in b.GetTracks() if o.GetNetCode() == t.GetNetCode()]
        span = free_span(t, [free_end != 0, free_end != 1], ts, padmap.get(t.GetNetCode(), []))
        if span is None:
            b.Delete(t)
        else:
            apply_span(t, *span)
        n_cut += 1
    print("dangling stubs cut back or removed: %d" % n_cut)

pcbnew.SaveBoard(OUT, b)
nf = sum(len(r[0]) for r in report.values())
allj = [j for r in report.values() for j in r[1]]
for f in order:
    if report[f][0]:
        print("FAILED %s: %s" % (short(f), " ".join("%s.%s" % x[:2] for x in report[f][0])))
print("done: %d nets, %d connections failed; %d jumps on F.Cu, longest %.1f mm, total %.0f mm; %.0fs" % (
    len(report), nf, len(allj), max(allj) if allj else 0, sum(allj), time.time() - t0))
