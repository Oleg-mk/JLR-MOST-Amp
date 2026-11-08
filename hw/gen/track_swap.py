"""Fewer vias by swapping layers (run by `route_grid.py <in> <out> swap`, inside its raster).

Where several nets each jump over one back-side track on the foil, that track goes under them instead: its stretch
across the crossings moves to the foil between two new vias, and each of the jumps comes back to the back side on the
very same path. k jumps (2k vias) become one (2 vias). A swap is made only when every piece keeps its clearance on the
grid; KiCad's DRC then checks the board, and a swap it objects to is put back.
Only signal tracks (0.3 mm or narrower, jumps allowed) are moved to the foil, and a stretch is at most MAX_F long.
"""
import json, subprocess, re

import os
DBG = bool(os.environ.get("SWAP_DEBUG"))
MAX_F = 14.0          # mm of foil a moved stretch may take
GAP = 12.0            # crossings further apart along the track than this are separate groups
EPS = 1e-3


def grid(v):
    x, y = (pcbnew.ToMM(v.x) - OX) / G, (pcbnew.ToMM(v.y) - OY) / G
    i, j = round(x), round(y)
    return (i, j) if abs(x - i) < EPS and abs(y - j) < EPS else None


def kpt(n):
    return pcbnew.VECTOR2I(pcbnew.FromMM(OX + n[0] * G), pcbnew.FromMM(OY + n[1] * G))


def netof(t):
    return nid.get(t.GetNetname(), 0)


def paint_net(net):
    for layer in (B, F):
        m = (owner[layer] == net) & (static[layer] != net)
        owner[layer][m] = 0
    for t in b.GetTracks():
        if netof(t) != net:
            continue
        if t.GetClass() == "PCB_VIA":
            q = t.GetPosition()
            for layer in (B, F):
                paint_seg(layer, pcbnew.ToMM(q.x), pcbnew.ToMM(q.y), pcbnew.ToMM(q.x), pcbnew.ToMM(q.y), VIA_D / 2, net,
                          only_free=True)
        else:
            layer = B if t.GetLayer() == pcbnew.B_Cu else F
            s_, e_ = t.GetStart(), t.GetEnd()
            paint_seg(layer, pcbnew.ToMM(s_.x), pcbnew.ToMM(s_.y), pcbnew.ToMM(e_.x), pcbnew.ToMM(e_.y),
                      pcbnew.ToMM(t.GetWidth()) / 2, net, only_free=True)


for net in sorted({netof(t) for t in b.GetTracks()}):
    paint_net(net)
print("tracks painted, %.1fs" % (time.time() - t0))


def cells_of(pts):
    out = []
    for p_, q_ in zip(pts, pts[1:]):
        n = max(abs(q_[0] - p_[0]), abs(q_[1] - p_[1]))
        sx, sy = (q_[0] > p_[0]) - (q_[0] < p_[0]), (q_[1] > p_[1]) - (q_[1] < p_[1])
        out += [(p_[0] + k * sx, p_[1] + k * sy) for k in range(n + (1 if q_ == pts[-1] else 0))]
    return out


def clear_for(layer, pts, w, nets_ok, extra=None):
    """can a track of width w run along pts on layer, given copper of nets_ok is no obstacle"""
    rt = (w / 2 + CLR) / G + 0.71
    cs = cells_of(pts)
    k = int(math.ceil(rt))
    for (i, j) in cs:
        sub = owner[layer, max(0, j - k):j + k + 1, max(0, i - k):i + k + 1]
        J, I = np.mgrid[max(0, j - k):min(NY, j + k + 1), max(0, i - k):min(NX, i + k + 1)]
        near = (I - i) ** 2 + (J - j) ** 2 <= rt * rt
        bad = near & (sub != 0) & ~np.isin(sub, list(nets_ok))
        if extra is not None:
            bad |= near & extra[layer, max(0, j - k):j + k + 1, max(0, i - k):i + k + 1]
        if bad.any():
            return False
    return True


def via_fits(n, nets_ok, extra=None):
    i, j = n
    rt = (VIA_D / 2 + CLR) / G + 0.71
    k = int(math.ceil(rt))
    for layer in (B, F):
        sub = owner[layer, max(0, j - k):j + k + 1, max(0, i - k):i + k + 1]
        J, I = np.mgrid[max(0, j - k):min(NY, j + k + 1), max(0, i - k):min(NX, i + k + 1)]
        near = (I - i) ** 2 + (J - j) ** 2 <= rt * rt
        bad = near & (sub != 0) & ~np.isin(sub, list(nets_ok))
        if extra is not None:
            bad |= near & extra[layer, max(0, j - k):j + k + 1, max(0, i - k):i + k + 1]
        if bad.any():
            return False
    return True


def seg_hit(a, c, p, q, reach):
    """do segments a-c and p-q (grid units) come within reach"""
    r = (c[0] - a[0], c[1] - a[1]); s2 = (q[0] - p[0], q[1] - p[1]); den = r[0] * s2[1] - r[1] * s2[0]
    if den:
        t = ((p[0] - a[0]) * s2[1] - (p[1] - a[1]) * s2[0]) / den
        u = ((p[0] - a[0]) * r[1] - (p[1] - a[1]) * r[0]) / den
        if 0 <= t <= 1 and 0 <= u <= 1:
            return (a[0] + t * r[0], a[1] + t * r[1])

    def pd(pt, s0, s1):
        dx, dy = s1[0] - s0[0], s1[1] - s0[1]; L2 = dx * dx + dy * dy
        t = 0 if L2 == 0 else max(0, min(1, ((pt[0] - s0[0]) * dx + (pt[1] - s0[1]) * dy) / L2))
        return math.hypot(s0[0] + t * dx - pt[0], s0[1] + t * dy - pt[1]), (s0[0] + t * dx, s0[1] + t * dy)
    best = min(pd(a, p, q), pd(c, p, q), (pd(p, a, c)[0], p), (pd(q, a, c)[0], q))
    return best[1] if best[0] < reach else None


# ---- the crossed track: its run through the crossings, as a grid polyline
changes = []          # dict(removed=[geometry...], added=[items])
why = {}


def nope(k):
    why[k] = why.get(k, 0) + 1


def geom(t):   # plain copies: GetStart() and friends are live references into the item
    cp = lambda v: pcbnew.VECTOR2I(int(v.x), int(v.y))
    if t.GetClass() == "PCB_VIA":
        return ("via", cp(t.GetPosition()), netof(t))
    return ("trk", cp(t.GetStart()), cp(t.GetEnd()), t.GetWidth(), t.GetLayer(), netof(t))


def rebuild(gm):
    if gm[0] == "via":
        v = pcbnew.PCB_VIA(b)
        v.SetPosition(gm[1])
        v.SetWidth(pcbnew.FromMM(VIA_D))
        v.SetDrill(pcbnew.FromMM(VIA_H))
        v.SetNet(b.FindNet(names[gm[2]]))
        b.Add(v)
        return v
    t = pcbnew.PCB_TRACK(b)
    t.SetStart(gm[1])
    t.SetEnd(gm[2])
    t.SetWidth(gm[3])
    t.SetLayer(gm[4])
    t.SetNet(b.FindNet(names[gm[5]]))
    b.Add(t)
    return t


def add_trk(net, layer, p_, q_, w):
    return rebuild(("trk", kpt(p_), kpt(q_), w, layer, net))



def one_pass():
    # ---- jumps: a net's foil copper between exactly two of its vias, no branches
    tracks = [t for t in b.GetTracks() if t.GetClass() == "PCB_TRACK"]
    vias_all = [t for t in b.GetTracks() if t.GetClass() == "PCB_VIA"]
    jumps = []
    for net in sorted({netof(t) for t in tracks if t.GetLayer() == pcbnew.F_Cu}):
        fs = [t for t in tracks if netof(t) == net and t.GetLayer() == pcbnew.F_Cu]
        vs = {grid(v.GetPosition()): v for v in vias_all if netof(v) == net}
        adj = {}
        for t in fs:
            a_, c_ = grid(t.GetStart()), grid(t.GetEnd())
            if a_ is None or c_ is None:
                adj = None
                break
            adj.setdefault(a_, []).append((c_, t))
            adj.setdefault(c_, []).append((a_, t))
        if not adj:
            continue
        seen = set()
        for start in adj:
            if start in seen or len(adj[start]) != 1:
                continue
            chain, node, prev, items_ = [start], start, None, []
            seen.add(start)
            while True:
                nxt = [(n_, t) for n_, t in adj[node] if t not in items_]
                if not nxt:
                    break
                n_, t = nxt[0]
                items_.append(t)
                chain.append(n_)
                seen.add(n_)
                node = n_
                if len(adj[node]) != 2:
                    break
            if len(adj[node]) == 1 and start in vs and node in vs and start != node:
                ws = {t.GetWidth() for t in items_}
                if len(ws) == 1:
                    jumps.append(dict(net=net, pts=chain, items=items_, vias=[vs[start], vs[node]], w=ws.pop()))
    print("jumps found: %d" % len(jumps))

    # ---- what each jump crosses on the back
    btr = [t for t in tracks if t.GetLayer() == pcbnew.B_Cu]
    bseg = []
    for t in btr:
        a_, c_ = grid(t.GetStart()), grid(t.GetEnd())
        if a_ is not None and c_ is not None:
            bseg.append((a_, c_, t))
    cand = {}
    for jp in jumps:
        hit = {}
        wj = pcbnew.ToMM(jp["w"])
        for p_, q_ in zip(jp["pts"], jp["pts"][1:]):
            for a_, c_, t in bseg:
                nt = netof(t)
                if nt == jp["net"]:
                    continue
                reach = (wj + pcbnew.ToMM(t.GetWidth())) / 2 / G + CLR / G
                x = seg_hit(p_, q_, a_, c_, reach)
                if x is not None:
                    hit.setdefault(nt, []).append((x, t))
        if len(hit) != 1:
            continue
        xnet, xs = list(hit.items())[0]
        nm = short(names[xnet])
        if xnet == GND or nm in NO_JUMP or WIDE.get(nm, 0.25) > 0.3:
            continue
        # the jump on the back instead: clear of everything but its own net and the crossed one
        if not clear_for(B, jp["pts"], wj, {jp["net"], xnet}):
            continue
        jp["xnet"], jp["xpt"], jp["xtrack"] = xnet, xs[0][0], xs[0][1]
        cand.setdefault(xnet, []).append(jp)
    print("jumps that cross a single back track and could run on the back: %d over %d nets" % (
        sum(len(v) for v in cand.values()), len(cand)))

    swaps = 0
    for xnet, jl in sorted(cand.items(), key=lambda kv: -len(kv[1])):
        if len(jl) < 2:
            continue
        if DBG:
            print("  x", short(names[xnet]), len(jl)); sys.stdout.flush()
        xs = [t for t in b.GetTracks() if t.GetClass() == "PCB_TRACK" and netof(t) == xnet and t.GetLayer() == pcbnew.B_Cu]
        adj = {}
        ok = True
        for t in xs:
            a_, c_ = grid(t.GetStart()), grid(t.GetEnd())
            if a_ is None or c_ is None:
                ok = False
                break
            adj.setdefault(a_, []).append((c_, t))
            adj.setdefault(c_, []).append((a_, t))
        if not ok:
            continue
        xvias = {grid(v.GetPosition()) for v in b.GetTracks() if v.GetClass() == "PCB_VIA" and netof(v) == xnet}
        xpads = [e[5] for e in pads_of.get(xnet, [])]

        def is_anchor(n_):
            return len(adj.get(n_, [])) != 2 or n_ in xvias or any(p.HitTest(kpt(n_)) for p in xpads)

        # walk the run that holds each crossing; group crossings run by run
        runs = {}
        for jp in jl:
            t0_ = jp["xtrack"]
            a_, c_ = grid(t0_.GetStart()), grid(t0_.GetEnd())
            # walk both ways from this segment to anchors
            def walk(node, came):   # (pcbnew hands out a new Python object per access: compare items by uuid)
                pts, its = [node], []
                while not is_anchor(node):
                    nx = [(n_, t) for n_, t in adj[node] if t.m_Uuid.AsString() != came]
                    if not nx:
                        break
                    n_, t = nx[0]
                    pts.append(n_)
                    its.append(t)
                    came, node = t.m_Uuid.AsString(), n_
                return pts, its
            p1, i1 = walk(a_, t0_.m_Uuid.AsString())
            p2, i2 = walk(c_, t0_.m_Uuid.AsString())
            pts = p1[::-1] + p2
            its = i1[::-1] + [t0_] + i2
            key = (pts[0], pts[-1]) if pts[0] <= pts[-1] else (pts[-1], pts[0])
            if key not in runs:
                runs[key] = dict(pts=pts if pts[0] <= pts[-1] else pts[::-1],
                                 its=its if pts[0] <= pts[-1] else its[::-1], jumps=[])
            runs[key]["jumps"].append(jp)
        for rk, rn in runs.items():
            pts = rn["pts"]
            cs = cells_of(pts)
            if len(rn["jumps"]) < 2:
                nope("one crossing per run")
                continue
            # crossing positions along the run (index into its cells)
            def at(x):
                return min(range(len(cs)), key=lambda k: (cs[k][0] - x[0]) ** 2 + (cs[k][1] - x[1]) ** 2)
            js = sorted(rn["jumps"], key=lambda jp: at(jp["xpt"]))
            groups, cur = [], [js[0]]
            for jp in js[1:]:
                if (at(jp["xpt"]) - at(cur[-1]["xpt"])) * G <= GAP:
                    cur.append(jp)
                else:
                    groups.append(cur)
                    cur = [jp]
            groups.append(cur)
            for grp in groups:
                if len(grp) < 2:
                    nope("lone crossing on its stretch")
                    continue
                k1, k2 = at(grp[0]["xpt"]), at(grp[-1]["xpt"])
                jnets = {jp["net"] for jp in grp}
                # what the board looks like with these jumps on the back: their foil gone, their back copies painted
                extra = np.zeros_like(owner, dtype=bool)
                for jp in grp:
                    for (i, j) in cells_of(jp["pts"]):
                        r_ = int(math.ceil(pcbnew.ToMM(jp["w"]) / 2 / G))
                        extra[B, max(0, j - r_):j + r_ + 1, max(0, i - r_):i + r_ + 1] = True
                hide = np.isin(owner[F], list(jnets)) & (static[F] == 0)
                saved_f = owner[F][hide].copy()
                owner[F][hide] = 0
                wx = list({t.GetWidth() for t in rn["its"]})
                if len(wx) != 1:
                    owner[F][hide] = saved_f
                    continue
                wx = wx[0]
                wxm = pcbnew.ToMM(wx)
                gap_c = int(math.ceil((VIA_D / 2 + CLR + 0.3) / G)) + 1
                v1 = v2 = None
                def via_ok_at(k):   # a via clear of other copper, and not on one of the track's own pads
                    return via_fits(cs[k], {xnet}, extra) and not any(
                        p.HitTest(kpt(cs[k]), pcbnew.FromMM(VIA_D / 2 + 0.1)) for p in xpads)
                for k in range(k1 - gap_c, -1, -1):
                    if via_ok_at(k):
                        v1 = k
                        break
                for k in range(k2 + gap_c, len(cs)):
                    if via_ok_at(k):
                        v2 = k
                        break
                if v1 is None or v2 is None or (v2 - v1) * G > MAX_F * 1.5:
                    nope("no room for a via" if v1 is None or v2 is None else "stretch too long")
                    owner[F][hide] = saved_f
                    continue
                fpts = cs[v1:v2 + 1]
                # simplify to corners
                fc = [fpts[0]]
                for a_, c_, n_ in zip(fpts, fpts[1:], fpts[2:]):
                    if (c_[0] - a_[0], c_[1] - a_[1]) != (n_[0] - c_[0], n_[1] - c_[1]):
                        fc.append(c_)
                fc.append(fpts[-1])
                flen = sum(math.hypot(q_[0] - p_[0], q_[1] - p_[1]) for p_, q_ in zip(fc, fc[1:])) * G
                if flen > MAX_F or not clear_for(F, fc, wxm, {xnet}):
                    nope("stretch too long" if flen > MAX_F else "foil not clear")
                    owner[F][hide] = saved_f
                    continue
                owner[F][hide] = saved_f
                # make the swap: jumps to the back
                if DBG:
                    print("    swap", short(names[xnet]), [short(names[n]) for n in jnets]); sys.stdout.flush()
                removed, added = [], []
                for jp in grp:
                    for it in jp["items"] + jp["vias"]:
                        removed.append(geom(it))
                        b.Delete(it)
                    for p_, q_ in zip(jp["pts"], jp["pts"][1:]):
                        added.append(add_trk(jp["net"], pcbnew.B_Cu, p_, q_, jp["w"]))
                # the crossed run: back copper between the vias goes, foil copper and two vias come
                gone_u = set()
                for it in rn["its"]:
                    u = it.m_Uuid.AsString()
                    if u in gone_u:
                        continue
                    gone_u.add(u)
                    removed.append(geom(it))
                    b.Delete(it)
                bc = cs[:v1 + 1]
                ac = cs[v2:]

                def corn(c_l):
                    if len(c_l) < 2:
                        return c_l
                    out = [c_l[0]]
                    for a_, c_, n_ in zip(c_l, c_l[1:], c_l[2:]):
                        if (c_[0] - a_[0], c_[1] - a_[1]) != (n_[0] - c_[0], n_[1] - c_[1]):
                            out.append(c_)
                    out.append(c_l[-1])
                    return out
                for part, layer in ((corn(bc), pcbnew.B_Cu), (fc, pcbnew.F_Cu), (corn(ac), pcbnew.B_Cu)):
                    for p_, q_ in zip(part, part[1:]):
                        added.append(add_trk(xnet, layer, p_, q_, wx))
                for n_ in (cs[v1], cs[v2]):
                    added.append(rebuild(("via", kpt(n_), xnet)))
                changes.append(dict(removed=removed, added=added, nets={xnet} | jnets,
                                    what="%s under %s" % (short(names[xnet]), " ".join(sorted(short(names[n]) for n in jnets)))))
                for n_ in {xnet} | jnets:
                    paint_net(n_)
                swaps += 1
                return swaps        # the board changed: look again from scratch
    return swaps



npass = total = 0
while npass < 60:
    npass += 1
    why.clear()
    n_ = one_pass()
    total += n_
    print("pass %d: %d swaps" % (npass, n_))
    if not n_:
        break
print("swaps made: %d, %.0fs" % (total, time.time() - t0))
print("   not swapped (last pass):", why)
for c in changes:
    print("   " + c["what"])


# ---- KiCad's check: a swap it objects to goes back
for rnd in range(4):
    pcbnew.SaveBoard(OUT, b)
    rpt = OUT + ".drc.json"
    subprocess.run([KCLI, "pcb", "drc", "--format", "json", "--severity-error", "--refill-zones", "-o", rpt, OUT],
                   capture_output=True)
    rep = json.load(open(rpt, encoding="utf-8"))
    bad = {it.get("uuid") for v in rep["violations"] for it in v["items"]}
    torn = set()
    for u in rep.get("unconnected_items", []):
        for it in u["items"]:
            m_ = re.search(r"\[([^\]]*)\]", it["description"])
            if m_ and m_.group(1) in nid:
                torn.add(nid[m_.group(1)])
    back = [c for c in changes if c["nets"] & torn or any(t.m_Uuid.AsString() in bad for t in c["added"])]
    print("KiCad check %d: %d violations, %d unconnected, %d swaps put back" % (
        rnd + 1, len(rep["violations"]), len(rep.get("unconnected_items", [])), len(back)))
    if not back:
        break
    for c in back:
        for t in c["added"]:
            b.Delete(t)
        for gm in c["removed"]:
            rebuild(gm)
        changes.remove(c)
pcbnew.SaveBoard(OUT, b)
nv = sum(1 for t in b.GetTracks() if t.GetClass() == "PCB_VIA")
print("saved %s: %d swaps kept, %d vias on the board" % (OUT, len(changes), nv))
