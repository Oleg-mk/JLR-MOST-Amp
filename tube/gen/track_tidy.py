"""Straighten the tracks of a routed board (run by `route_grid.py <in> <out> tidy`, inside its raster). Tube board copy:
the clearance to another net's copper goes by the classes of both nets.

Each run of track between two fixed points is laid again on its own layer as the path with the fewest turns that keeps
clear of all other copper, within CORR of where it ran. Fixed points: pads, vias, branch points, and points where
another track or a pad touches a run part-way (the track is cut there first, same geometry). Only tracks on the 0.1 mm
grid are touched; the hand-laid HDMI breakout and ground are left alone. KiCad's DRC checks the result; a run it
objects to goes back as it was.
"""
import json, subprocess, re, os

CORR = 1.2                                   # mm either side of the old run
TURN = {0: 0.0, 1: 0.6, 2: 1.2, 3: 4.0}      # mm of length a 45 / 90 / 135 degree turn is worth; no U-turns
D8 = [(1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1)]
EPS = 1e-3


def grid(v):
    """board point -> (i, j) on the routing grid, or None"""
    x, y = (pcbnew.ToMM(v.x) - OX) / G, (pcbnew.ToMM(v.y) - OY) / G
    i, j = round(x), round(y)
    return (i, j) if abs(x - i) < EPS and abs(y - j) < EPS else None


def kpt(n):
    return pcbnew.VECTOR2I(pcbnew.FromMM(OX + n[0] * G), pcbnew.FromMM(OY + n[1] * G))


def on_seg(a, b_, c):
    """grid point of octilinear segment a-b nearest to point c (grid units)"""
    dx, dy = b_[0] - a[0], b_[1] - a[1]
    n = max(abs(dx), abs(dy))
    if n == 0:
        return a
    sx, sy = (dx > 0) - (dx < 0), (dy > 0) - (dy < 0)
    k = ((c[0] - a[0]) * sx + (c[1] - a[1]) * sy) / float(sx * sx + sy * sy)
    k = min(n, max(0, int(round(k))))
    return (a[0] + k * sx, a[1] + k * sy)


def seg_dist(a, b_, c):
    dx, dy = b_[0] - a[0], b_[1] - a[1]
    L2 = dx * dx + dy * dy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((c[0] - a[0]) * dx + (c[1] - a[1]) * dy) / L2))
    return math.hypot(a[0] + t * dx - c[0], a[1] + t * dy - c[1])


def seg_seg(a, b_, c, d):
    """closest points of segments a-b and c-d (grid units): (distance, point on a-b, point on c-d)"""
    best = None
    for p_, (s0, s1) in ((a, (c, d)), (b_, (c, d))):
        dx, dy = s1[0] - s0[0], s1[1] - s0[1]
        L2 = dx * dx + dy * dy
        t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((p_[0] - s0[0]) * dx + (p_[1] - s0[1]) * dy) / L2))
        q_ = (s0[0] + t * dx, s0[1] + t * dy)
        dd = math.hypot(p_[0] - q_[0], p_[1] - q_[1])
        if best is None or dd < best[0]:
            best = (dd, p_, q_)
    for p_, (s0, s1) in ((c, (a, b_)), (d, (a, b_))):
        dx, dy = s1[0] - s0[0], s1[1] - s0[1]
        L2 = dx * dx + dy * dy
        t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((p_[0] - s0[0]) * dx + (p_[1] - s0[1]) * dy) / L2))
        q_ = (s0[0] + t * dx, s0[1] + t * dy)
        dd = math.hypot(p_[0] - q_[0], p_[1] - q_[1])
        if dd < best[0]:
            best = (dd, q_, p_)
    # crossing segments
    r = (b_[0] - a[0], b_[1] - a[1])
    s2 = (d[0] - c[0], d[1] - c[1])
    den = r[0] * s2[1] - r[1] * s2[0]
    if den:
        t = ((c[0] - a[0]) * s2[1] - (c[1] - a[1]) * s2[0]) / den
        u = ((c[0] - a[0]) * r[1] - (c[1] - a[1]) * r[0]) / den
        if 0 <= t <= 1 and 0 <= u <= 1:
            x = (a[0] + t * r[0], a[1] + t * r[1])
            best = (0.0, x, x)
    return best


def new_track(net, layer, p_, q_, w_):
    t = pcbnew.PCB_TRACK(b)
    t.SetStart(kpt(p_))
    t.SetEnd(kpt(q_))
    t.SetWidth(w_)
    t.SetLayer(LID[layer])
    t.SetNet(b.FindNet(names[net]))
    b.Add(t)
    return t


def paint_net_layer(net, layer):
    """repaint one net's copper on one layer from the board (its movable cells cleared first)"""
    m = (owner[layer] == net) & (static[layer] != net)
    owner[layer][m] = 0
    for t in b.GetTracks():
        if nid.get(t.GetNetname(), 0) != net:
            continue
        if t.GetClass() == "PCB_VIA":
            q = t.GetPosition()
            paint_seg(layer, pcbnew.ToMM(q.x), pcbnew.ToMM(q.y), pcbnew.ToMM(q.x), pcbnew.ToMM(q.y), VIA_D / 2, net,
                      only_free=True)
        elif t.GetLayer() == LID[layer]:
            s_, e_ = t.GetStart(), t.GetEnd()
            paint_seg(layer, pcbnew.ToMM(s_.x), pcbnew.ToMM(s_.y), pcbnew.ToMM(e_.x), pcbnew.ToMM(e_.y),
                      pcbnew.ToMM(t.GetWidth()) / 2, net, only_free=True)


netcodes = sorted({nid.get(t.GetNetname(), 0) for t in b.GetTracks()})
for net in netcodes:
    for layer in (B, F):
        paint_net_layer(net, layer)
print("tracks painted, %.1fs" % (time.time() - t0))


def turn_astar(start, goal, free, oldset):
    """fewest-turn path on one layer within the free mask (window-local grid); node list or None. Steps between two
    cells of the old run are always allowed (it is DRC-clean as it stands)."""
    H, Wd = free.shape
    fb = free.tobytes()

    def h(j, i):
        dx, dy = abs(i - goal[0]), abs(j - goal[1])
        return (max(dx, dy) + (SQ2 - 1) * min(dx, dy)) * G

    sk = (start[1], start[0], 8)
    gsc, par = {sk: 0.0}, {sk: None}
    openl = [(h(start[1], start[0]), 0.0, sk)]
    closed = set()
    while openl:
        f_, g_, k = heapq.heappop(openl)
        if k in closed:
            continue
        closed.add(k)
        j, i, d = k
        if (i, j) == goal:
            path = []
            while k is not None:
                path.append((k[1], k[0]))
                k = par[k]
            return path[::-1]
        if len(closed) > 300000:
            return None
        for nd, (di, dj) in enumerate(D8):
            ii, jj = i + di, j + dj
            if ii < 0 or jj < 0 or ii >= Wd or jj >= H or not fb[jj * Wd + ii]:
                continue
            if di and dj and (not fb[j * Wd + ii] or not fb[jj * Wd + i]) and ((i, j), (ii, jj)) not in oldset:
                continue
            tc = 0.0
            if d != 8:
                tr = min(abs(nd - d), 8 - abs(nd - d))
                if tr > 3:
                    continue
                tc = TURN[tr]
            ng = g_ + (SQ2 if di and dj else 1.0) * G + tc
            kk = (jj, ii, nd)
            if ng < gsc.get(kk, 1e18):
                gsc[kk] = ng
                par[kk] = k
                heapq.heappush(openl, (ng + h(jj, ii), ng, kk))
    return None


def corners(nodes):
    out = [nodes[0]]
    for a_, c_, n_ in zip(nodes, nodes[1:], nodes[2:]):
        d1 = (c_[0] - a_[0], c_[1] - a_[1])
        d2 = (n_[0] - c_[0], n_[1] - c_[1])
        if d1[0] * d2[1] != d1[1] * d2[0] or d1[0] * d2[0] + d1[1] * d2[1] < 0:
            out.append(c_)
    out.append(nodes[-1])
    return out


def length(pts):
    return sum(math.hypot(q_[0] - p_[0], q_[1] - p_[1]) for p_, q_ in zip(pts, pts[1:])) * G


done = []                                     # (net, layer, width, old corner list, new items)
tried = better = cuts = 0
why = {}
for net in netcodes:
    nm = names.get(net, "").split("/")[-1]
    if net == GND or nm.startswith("I2S_"):
        continue
    if os.environ.get("TIDY_NETS") and nm not in os.environ["TIDY_NETS"].split(","):
        continue   # only the nets a local fix has just laid
    for layer in (B, F):
        segs = []                              # [a, b, width, item, movable]
        for t in b.GetTracks():
            if nid.get(t.GetNetname(), 0) == net and t.GetClass() != "PCB_VIA" and t.GetLayer() == LID[layer]:
                a_, b_ = grid(t.GetStart()), grid(t.GetEnd())
                segs.append([a_, b_, t.GetWidth(), t, a_ is not None and b_ is not None and a_ != b_])
        if not segs:
            continue
        vias = {grid(t.GetPosition()) for t in b.GetTracks() if nid.get(t.GetNetname(), 0) == net and t.GetClass() == "PCB_VIA"}
        vias.discard(None)
        anchors = set(vias)
        for sg in segs:                         # ends of copper left as it is stay put
            if not sg[4]:
                anchors.update(x for x in (sg[0], sg[1]) if x is not None)
        pads = [e[5] for e in pads_of.get(net, []) if layer in e[4]]

        def cut(sg, p_):                       # cut a track in two at a grid point on it, same geometry
            global cuts
            segs.remove(sg)
            b.Delete(sg[3])
            for x, y in ((sg[0], p_), (p_, sg[1])):
                segs.append([x, y, sg[2], new_track(net, layer, x, y, sg[2]), True])
            anchors.add(p_)
            cuts += 1

        # a branch's free end or a via touching a track part-way: cut the track there (corners of the track itself
        # have two tracks on them and are not contacts)
        changed = True
        while changed:
            changed = False
            dg = {}
            for sg in segs:
                for x in (sg[0], sg[1]):
                    if x is not None:
                        dg[x] = dg.get(x, 0) + 1
            contacts = set(vias) | {x for x, d in dg.items() if d == 1}
            for sg in list(segs):
                if not sg[4]:
                    continue
                hw = pcbnew.ToMM(sg[2]) / 2 / G + 0.1
                for c in contacts:
                    if c in (sg[0], sg[1]) or seg_dist(sg[0], sg[1], c) > hw:
                        continue
                    p_ = on_seg(sg[0], sg[1], c)
                    if p_ in (sg[0], sg[1]):
                        anchors.add(p_)
                        continue
                    cut(sg, p_)
                    changed = True
                    break
                if changed:
                    break
            if changed:
                continue
            # two tracks of the net crossing part-way without a shared end: both are cut where they cross
            for x1 in range(len(segs)):
                s1 = segs[x1]
                if s1[0] is None or s1[1] is None:
                    continue
                for x2 in range(x1 + 1, len(segs)):
                    s2 = segs[x2]
                    if s2[0] is None or s2[1] is None or {s1[0], s1[1]} & {s2[0], s2[1]}:
                        continue
                    dd, c1, c2 = seg_seg(s1[0], s1[1], s2[0], s2[1])
                    if dd > 0.0:                 # only tracks whose centre lines cross
                        continue
                    for sg, c in ((s1, c1), (s2, c2)):
                        p_ = on_seg(sg[0], sg[1], c)
                        if p_ in (sg[0], sg[1]) or not sg[4]:
                            anchors.add(p_)
                        else:
                            cut(sg, p_)
                            changed = True
                    if changed:
                        break
                if changed:
                    break
        # a pad touching a track: the track keeps to it (its end inside, or cut at the point nearest the pad centre)
        for p_ in pads:
            pc = ((pcbnew.ToMM(p_.GetPosition().x) - OX) / G, (pcbnew.ToMM(p_.GetPosition().y) - OY) / G)
            for sg in list(segs):
                if not sg[4]:
                    continue
                hwn = sg[2] // 2
                ends = [x for x in (sg[0], sg[1]) if p_.HitTest(kpt(x), hwn)]
                if ends:
                    anchors.update(ends)
                    continue
                n = max(abs(sg[1][0] - sg[0][0]), abs(sg[1][1] - sg[0][1]))
                sx, sy = (sg[1][0] > sg[0][0]) - (sg[1][0] < sg[0][0]), (sg[1][1] > sg[0][1]) - (sg[1][1] < sg[0][1])
                if any(p_.HitTest(kpt((sg[0][0] + k * sx, sg[0][1] + k * sy)), hwn) for k in range(n + 1)):
                    q_ = on_seg(sg[0], sg[1], pc)
                    if q_ in (sg[0], sg[1]):
                        anchors.add(q_)
                    else:
                        cut(sg, q_)
        deg = {}
        for sg in segs:
            for x in (sg[0], sg[1]):
                if x is not None:
                    deg[x] = deg.get(x, 0) + 1
        anchors |= {x for x, d in deg.items() if d != 2}
        at = {}
        for k_, sg in enumerate(segs):
            for x in (sg[0], sg[1]):
                if x is not None:
                    at.setdefault(x, []).append(k_)
        seen, runs = set(), []
        for a0 in list(anchors):
            for k0 in at.get(a0, []):
                if k0 in seen:
                    continue
                run, node, k_, ok = [], a0, k0, True
                while True:
                    seen.add(k_)
                    sg = segs[k_]
                    ok = ok and sg[4]
                    run.append(k_)
                    node = sg[1] if sg[0] == node else sg[0]
                    if node is None or node in anchors:
                        break
                    rest = [x for x in at.get(node, []) if x not in seen]
                    if not rest:
                        break
                    k_ = rest[0]
                if ok and node is not None and node != a0:
                    runs.append((a0, node, run))
                else:
                    why["fixed copper in run" if not ok else "loop/open"] = why.get("fixed copper in run" if not ok else "loop/open", 0) + 1
        if os.environ.get("TIDY_DEBUG") and nm == os.environ.get("TIDY_DEBUG") and layer == B:
            bvp = lambda n_: (round(230 - (OX + n_[0] * G), 2), round(OY + n_[1] * G - 100, 2))
            for sg in segs:
                print("  seg", bvp(sg[0]) if sg[0] else None, bvp(sg[1]) if sg[1] else None, sg[4])
            print("  anchors", sorted(bvp(x) for x in anchors))
            for a0, a1, run in runs:
                print("  run", bvp(a0), "->", bvp(a1), len(run))
        for a0, a1, run in runs:
            ws = {segs[k_][2] for k_ in run}
            if len(ws) != 1:
                why["mixed width"] = why.get("mixed width", 0) + 1
                continue
            w_ = ws.pop()
            pts = [a0]
            for k_ in run:
                sg = segs[k_]
                pts.append(sg[1] if sg[0] == pts[-1] else sg[0])
            old_c = corners(pts)
            if len(old_c) <= 3:                 # one turn or none: as straight as it gets
                why["<=1 turn"] = why.get("<=1 turn", 0) + 1
                continue
            tried += 1
            wmm = pcbnew.ToMM(w_)
            cn = CLASS_OF.get(nm, OT)           # clearance by the classes of both nets (route_grid's CLRT)
            rt = (wmm / 2 + max(CLRT[cn, k_] for k_ in (OT, HV, SW, MV))) / G + 0.71
            m = int(math.ceil(CORR / G + rt)) + 2
            xs, ys = [p_[0] for p_ in pts], [p_[1] for p_ in pts]
            i1, i2 = max(0, min(xs) - m), min(NX - 1, max(xs) + m)
            j1, j2 = max(0, min(ys) - m), min(NY - 1, max(ys) + m)
            sub = owner[layer, j1:j2 + 1, i1:i2 + 1]
            other = (sub != 0) & (sub != net)
            ocl = LUT[sub + 1]
            blk = np.zeros_like(other)
            for k_ in (OT, HV, SW, MV):
                sel = other & (ocl == k_)
                if not sel.any():
                    continue
                for dy, dx in disk((wmm / 2 + CLRT[cn, k_]) / G + 0.71):
                    ys_ = slice(max(0, dy), sel.shape[0] + min(0, dy)); yd = slice(max(0, -dy), sel.shape[0] + min(0, -dy))
                    xs_ = slice(max(0, dx), sel.shape[1] + min(0, dx)); xd = slice(max(0, -dx), sel.shape[1] + min(0, -dx))
                    blk[yd, xd] |= sel[ys_, xs_]
            J, I = np.mgrid[j1:j2 + 1, i1:i2 + 1]
            corr = np.zeros(other.shape, dtype=bool)
            for p_, q_ in zip(pts, pts[1:]):
                dx, dy = q_[0] - p_[0], q_[1] - p_[1]
                L2 = dx * dx + dy * dy
                t_ = np.clip(((I - p_[0]) * dx + (J - p_[1]) * dy) / L2, 0, 1) if L2 else 0
                corr |= ((I - (p_[0] + t_ * dx)) ** 2 + (J - (p_[1] + t_ * dy)) ** 2) <= (CORR / G) ** 2
            free = corr & ~blk
            oldset = set()
            for p_, q_ in zip(pts, pts[1:]):     # the old run itself is always passable (it is DRC-clean)
                n = max(abs(q_[0] - p_[0]), abs(q_[1] - p_[1]))
                sx, sy = (q_[0] > p_[0]) - (q_[0] < p_[0]), (q_[1] > p_[1]) - (q_[1] < p_[1])
                for k in range(n + 1):
                    free[p_[1] + k * sy - j1, p_[0] + k * sx - i1] = True
                    if k:   # the old run's own steps, both ways
                        c0, c1 = (p_[0] + (k - 1) * sx - i1, p_[1] + (k - 1) * sy - j1), (p_[0] + k * sx - i1, p_[1] + k * sy - j1)
                        oldset.add((c0, c1))
                        oldset.add((c1, c0))
            st, gl = (a0[0] - i1, a0[1] - j1), (a1[0] - i1, a1[1] - j1)
            free[st[1], st[0]] = True
            free[gl[1], gl[0]] = True
            path = turn_astar(st, gl, free, oldset)
            if path is None:
                why["no path"] = why.get("no path", 0) + 1
                continue
            new = corners([(x + i1, y + j1) for x, y in path])
            if not (len(new) < len(old_c) and length(new) <= length(old_c) * 1.08 + 0.3):
                why["not better"] = why.get("not better", 0) + 1
                continue
            for k_ in run:
                b.Delete(segs[k_][3])
                segs[k_][3] = None
            newitems = [new_track(net, layer, p_, q_, w_) for p_, q_ in zip(new, new[1:])]
            done.append((net, layer, w_, old_c, newitems))
            better += 1
            paint_net_layer(net, layer)
print("tidy: %d tracks cut at contacts, %d runs tried, %d straightened, %.0fs" % (cuts, tried, better, time.time() - t0))
print("   skipped:", why)

# KiCad's check: a straightened run it objects to goes back
for rnd in range(5):
    pcbnew.SaveBoard(OUT, b)
    rpt = OUT + ".drc.json"
    subprocess.run([KCLI, "pcb", "drc", "--format", "json", "--severity-error", "--refill-zones", "-o", rpt, OUT],
                   capture_output=True)
    rep = json.load(open(rpt, encoding="utf-8"))
    bad = {it.get("uuid") for v in rep["violations"] if v["type"] in ("clearance", "shorting_items", "tracks_crossing",
           "hole_clearance", "copper_edge_clearance") for it in v["items"]}
    torn = set()                                 # nets KiCad finds split: all their straightened runs go back
    for u in rep.get("unconnected_items", []):
        for it in u["items"]:
            m_ = re.search(r"\[([^\]]*)\]", it["description"])
            if m_ and m_.group(1) in nid:
                torn.add(nid[m_.group(1)])
    back = [d_ for d_ in done if d_[0] in torn or any(t.m_Uuid.AsString() in bad for t in d_[4])]
    print("KiCad check %d: %d violations, %d runs put back, %d unconnected" % (
        rnd + 1, len(rep["violations"]), len(back), len(rep.get("unconnected_items", []))))
    if not back:
        break
    for d_ in back:
        net, layer, w_, old_c, newitems = d_
        for t in newitems:
            b.Delete(t)
        d_[4][:] = [new_track(net, layer, p_, q_, w_) for p_, q_ in zip(old_c, old_c[1:])]
        done.remove(d_)
pcbnew.SaveBoard(OUT, b)
print("saved %s: %d runs straightened" % (OUT, len(done)))
