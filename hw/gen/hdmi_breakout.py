"""Hand-laid LVDS breakout from U6 to the HDMI connector J7, and U6's supply and ground under its body.

python.exe hdmi_breakout.py <in.kicad_pcb> <out.kicad_pcb>

J7's even pins (BCK, MCLK) are a second row behind the odd ones. The 0.35 mm gaps in a row pass no track, so the far
row is reached only from the pocket between it and the board edge, entered round the rows' west end (the east end,
between pin 18 and the hole, passes one track). DATA runs under the shield tab to pins 1 and 3, LRCK comes straight
down to pins 7 and 9. Round the west end the pairs line up as MCLK, BCK, DATA, LRCK, so U6's channels are 4, 3, 2, 1
in that order. U6's outputs alternate polarity (- + + - - + + -) and J7's pairs do not, so BCK and LRCK cross once:
BCK through two vias west of the connector, LRCK- through a via and the foil to pin 7.

U6's GND pins take vias under its body, and EN joins VCC there, so the input pins keep the north side to themselves.
Coordinates are in the back view (x mirrored, as seen from the SMD side), mm from the board's top-left corner, drawn
for J7 pin 1 at (9.40, 56.00) and shifted with the connector as placed; U6 and C44 must sit at the same offset from
J7 (U6 at (16.5, 47.0) + shift); the script stops if they do not.
"""
import sys
import pcbnew

IN, OUT = sys.argv[1], sys.argv[2]
X0, Y0, W = 100.0, 100.0, 130.0
B, F = pcbnew.B_Cu, pcbnew.F_Cu
W_LVDS = 0.2
VIA_D, VIA_H = 0.6, 0.3

b = pcbnew.LoadBoard(IN)


def raw(ref, num):
    for p in b.FindFootprintByReference(ref).Pads():
        if p.GetNumber() == num:
            q = p.GetPosition()
            return X0 + W - pcbnew.ToMM(q.x), pcbnew.ToMM(q.y) - Y0
    raise KeyError("%s.%s" % (ref, num))


DX, DY = raw("J7", "1")[0] - 9.40, raw("J7", "1")[1] - 56.0   # the connector's shift from the drawn layout


def kpt(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(X0 + W - (x + DX)), pcbnew.FromMM(Y0 + y + DY))


def pad(ref, num):   # in the drawn layout's coordinates
    x, y = raw(ref, num)
    return round(x - DX, 3), round(y - DY, 3)


def net(ref, num):
    for p in b.FindFootprintByReference(ref).Pads():
        if p.GetNumber() == num:
            return p.GetNet()
    raise KeyError("%s.%s" % (ref, num))


def track(ni, layer, pts, w=W_LVDS):
    for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
        t = pcbnew.PCB_TRACK(b)
        t.SetStart(kpt(x1, y1))
        t.SetEnd(kpt(x2, y2))
        t.SetWidth(pcbnew.FromMM(w))
        t.SetLayer(layer)
        t.SetNet(ni)
        b.Add(t)


def via(ni, x, y):
    v = pcbnew.PCB_VIA(b)
    v.SetPosition(kpt(x, y))
    v.SetWidth(pcbnew.FromMM(VIA_D))
    v.SetDrill(pcbnew.FromMM(VIA_H))
    v.SetNet(ni)
    b.Add(v)


# where the layout below expects the pads
EXPECT = {("U6", "9"): (14.225, 49.86), ("U6", "16"): (18.775, 49.86), ("U6", "1"): (18.775, 44.14),
          ("J7", "1"): (9.40, 56.0), ("J7", "2"): (9.98, 57.0), ("J7", "12"): (15.75, 57.0), ("J7", "9"): (14.02, 56.0)}
for (ref, num), (ex, ey) in EXPECT.items():
    x, y = pad(ref, num)
    if abs(x - ex) > 0.02 or abs(y - ey) > 0.02:
        sys.exit("%s.%s is at (%.3f, %.3f), the layout expects (%.3f, %.3f)" % (ref, num, x, y, ex, ey))

u = {n: pad("U6", str(n)) for n in range(1, 17)}
j = {n: pad("J7", str(n)) for n in (1, 3, 4, 6, 7, 9, 10, 12)}


def land(n, pts):
    """end a polyline on J7 pin n: straight onto the pad centre along the last segment's column"""
    x, y = j[n]
    return pts + [(x, pts[-1][1]), (x, y)] if abs(pts[-1][0] - x) > 1e-3 else pts + [(x, y)]


# ---- far row: round the west end, down the free strip west of the hole, into the pocket under the far row
# lanes heading west, north to south: MCLK-, MCLK+, BCK+, BCK-; after the BCK cross: MCLK-, MCLK+, BCK-, BCK+;
# down the strip at x 4.3 / 4.7 / 5.1 / 5.5; in the pocket, top to bottom: BCK+ (pin 4), BCK- (6), MCLK+ (10), MCLK- (12)
track(net("U6", "9"), B, land(12, [u[9], (6.5, 49.86), (4.3, 52.06), (4.3, 57.68), (5.38, 58.76), (j[12][0], 58.76)]))
track(net("U6", "10"), B, land(10, [u[10], (14.875, 50.88), (13.9, 50.88), (13.28, 50.26), (6.67, 50.26), (4.7, 52.23),
                                    (4.7, 57.51), (5.59, 58.40), (j[10][0], 58.40)]))
n = net("U6", "11")    # BCK+: from the north lane over to the south one through the foil
track(n, B, [u[11], (15.525, 51.28), (13.7, 51.28), (13.08, 50.66), (8.8, 50.66), (8.6, 50.86)])
via(n, 8.6, 50.86)
track(n, F, [(8.6, 50.86), (7.4, 52.06)])
via(n, 7.4, 52.06)
track(n, B, land(4, [(7.4, 52.06), (6.0, 52.06), (5.5, 52.56), (5.5, 57.18), (6.0, 57.68), (j[4][0], 57.68)]))
track(net("U6", "12"), B, land(6, [u[12], (16.175, 51.68), (13.5, 51.68), (12.88, 51.06), (9.6, 51.06), (9.2, 51.46),
                                   (6.03, 51.46), (5.1, 52.39), (5.1, 57.35), (5.79, 58.04), (j[6][0], 58.04)]))

# ---- near row: DATA under the shield tab, LRCK straight down; parallel 45-degree runs x + y = 68.42 / 68.99 / 69.6 / 70.5
track(net("U6", "13"), B, land(1, [u[13], (16.825, 51.595), (14.2, 54.22), (9.85, 54.22), (j[1][0], 54.22 + 9.85 - j[1][0])]))
track(net("U6", "14"), B, land(3, [u[14], (17.475, 51.515), (14.37, 54.62), (j[3][0], 54.62)]))
track(net("U6", "15"), B, [u[15], (18.125, 51.475), (j[9][0], 69.6 - j[9][0]), j[9]])
n = net("U6", "16")    # LRCK-: under LRCK+ through the foil to pin 7, high enough that the foil still passes
track(n, B, [u[16], (18.775, 51.725), (16.1, 54.4)])   # between it and pins 9 and 11 (pin 11 is ground)
via(n, 16.1, 54.4)
track(n, F, [(16.1, 54.4), (13.4, 54.4), (j[7][0], 54.4 + 13.4 - j[7][0]), j[7]])

# ---- U6 supply and ground under the body: EN (1) to VCC (4), C44 to EN; GND pins 5 and 8 to vias
n = net("U6", "4")
track(n, B, [u[4], (u[4][0], 45.45), (u[1][0], 45.45), u[1]], 0.3)
c1 = pad("C44", "1")
if net("C44", "1").GetNetname() != n.GetNetname():
    sys.exit("C44.1 is not on U6's supply")
track(n, B, [c1, (u[1][0], c1[1] - (c1[0] - u[1][0]))], 0.4)
for k in (5, 8):
    g = net("U6", str(k))
    track(g, B, [u[k], (u[k][0], 46.3)], 0.25)
    via(g, u[k][0], 46.3)

pcbnew.SaveBoard(OUT, b)
print("breakout laid: %d tracks/vias, shifted (%.3f, %.3f)" % (len(b.GetTracks()), DX, DY))
