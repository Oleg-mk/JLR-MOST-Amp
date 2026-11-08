"""Take one net off a routed board so it can be laid again with fewer vias.

python.exe lift_net.py <in.kicad_pcb> <out.kicad_pcb> NET[,NET...]

Every foil jump of another net that crosses only the lifted net(s) on the back comes back to the back side on its own
path (when that keeps the clearance), and the lifted net's copper is removed. `route_grid.py <out> <out2> fix` then
lays the lifted net again, with vias priced high.
"""
import sys, math
import pcbnew

IN, OUT, NETS = sys.argv[1], sys.argv[2], sys.argv[3].split(",")
CLR = 0.15
b = pcbnew.LoadBoard(IN)
mm = pcbnew.ToMM
short = lambda n: n.split("/")[-1]
lift = {n for n in b.GetNetsByName().keys() if short(str(n)) in NETS}
lift = {str(n) for n in lift}


def pt(v):
    return mm(v.x), mm(v.y)


def seg_dist(a, c, p, q):
    r = (c[0] - a[0], c[1] - a[1]); s2 = (q[0] - p[0], q[1] - p[1]); den = r[0] * s2[1] - r[1] * s2[0]
    if den:
        t = ((p[0] - a[0]) * s2[1] - (p[1] - a[1]) * s2[0]) / den
        u = ((p[0] - a[0]) * r[1] - (p[1] - a[1]) * r[0]) / den
        if 0 <= t <= 1 and 0 <= u <= 1:
            return 0.0

    def pd(x, s0, s1):
        dx, dy = s1[0] - s0[0], s1[1] - s0[1]; L2 = dx * dx + dy * dy
        t = 0 if L2 == 0 else max(0, min(1, ((x[0] - s0[0]) * dx + (x[1] - s0[1]) * dy) / L2))
        return math.hypot(s0[0] + t * dx - x[0], s0[1] + t * dy - x[1])
    return min(pd(a, p, q), pd(c, p, q), pd(p, a, c), pd(q, a, c))


tracks = [t for t in b.GetTracks() if t.GetClass() == "PCB_TRACK"]
vias = [t for t in b.GetTracks() if t.GetClass() == "PCB_VIA"]
btr = [t for t in tracks if t.GetLayer() == pcbnew.B_Cu]
bpads = [p for f in b.GetFootprints() for p in f.Pads() if p.IsOnLayer(pcbnew.B_Cu)]

# foil jumps: chains of one net's F tracks between two of its vias
jumps = []
by_net = {}
for t in tracks:
    if t.GetLayer() == pcbnew.F_Cu:
        by_net.setdefault(t.GetNetname(), []).append(t)
for net, fs in by_net.items():
    if net in lift:
        continue
    key = lambda v: (v.x, v.y)
    adj = {}
    for t in fs:
        adj.setdefault(key(t.GetStart()), []).append(t)
        adj.setdefault(key(t.GetEnd()), []).append(t)
    vpos = {key(v.GetPosition()): v for v in vias if v.GetNetname() == net}
    used = set()
    for start, ts in adj.items():
        if len(ts) != 1 or start not in vpos or ts[0].m_Uuid.AsString() in used:
            continue
        chain, node, cur = [], start, ts[0]
        while True:
            chain.append(cur)
            used.add(cur.m_Uuid.AsString())
            s_, e_ = key(cur.GetStart()), key(cur.GetEnd())
            node = e_ if s_ == node else s_
            nx = [t for t in adj[node] if t.m_Uuid.AsString() not in used]
            if len(adj[node]) != 2 or not nx:
                break
            cur = nx[0]
        if len(adj[node]) == 1 and node in vpos and node != start:
            jumps.append((net, chain, [vpos[start], vpos[node]]))

moved = 0
for net, chain, jv in jumps:
    crossed = set()
    ok = True
    for f in chain:
        a, c = pt(f.GetStart()), pt(f.GetEnd())
        w = mm(f.GetWidth())
        for t in btr:
            if t.GetNetname() == net:
                continue
            if seg_dist(a, c, pt(t.GetStart()), pt(t.GetEnd())) < (w + mm(t.GetWidth())) / 2 + CLR:
                crossed.add(t.GetNetname())
        n = max(2, int(math.hypot(c[0] - a[0], c[1] - a[1]) / 0.05))
        for p in bpads:
            if p.GetNetname() == net:
                continue
            if any(p.HitTest(pcbnew.VECTOR2I(pcbnew.FromMM(a[0] + (c[0] - a[0]) * k / n), pcbnew.FromMM(a[1] + (c[1] - a[1]) * k / n)),
                             pcbnew.FromMM(w / 2 + CLR)) for k in range(n + 1)):
                if p.GetNetname() not in lift:
                    ok = False
    if not ok or not crossed or not crossed <= lift:
        continue
    for f in chain:
        t = pcbnew.PCB_TRACK(b)
        t.SetStart(f.GetStart())
        t.SetEnd(f.GetEnd())
        t.SetWidth(f.GetWidth())
        t.SetLayer(pcbnew.B_Cu)
        t.SetNet(f.GetNet())
        b.Add(t)
    for it in chain + jv:
        b.Delete(it)
    moved += 1
gone = 0
for t in list(b.GetTracks()):
    if t.GetNetname() in lift:
        b.Delete(t)
        gone += 1
pcbnew.SaveBoard(OUT, b)
print("lifted %s: %d of its tracks/vias removed, %d jumps over it back on the back side" % (
    ",".join(sorted(short(n) for n in lift)), gone, moved))
