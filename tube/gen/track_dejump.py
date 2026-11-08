"""Drop jumps that are no longer needed (run by `route_grid.py <in> <out> dejump`, inside its raster).

A jump is a run of foil-side (B.Cu) track of one net between two terminals: vias of that net or its through-hole pads.
Routing nets one after another leaves jumps over copper that was later moved away. For each jump a path on the track
side (F.Cu) alone is searched between its two terminals, clear of every other net by the class clearances; if one
exists and is not much longer, it replaces the jump, and the vias that only served the jump go. KiCad's DRC checks the
result; a replacement it objects to is put back.
"""
import json, subprocess

TOUCH = pcbnew.FromMM(0.01)
pads_net = {}
for fp in b.GetFootprints():
    for p in fp.Pads():
        pads_net.setdefault(p.GetNetCode(), []).append(p)

foil = [t for t in b.GetTracks() if t.GetClass() == "PCB_TRACK" and t.GetLayer() == FOIL_CU]
vias = [t for t in b.GetTracks() if t.GetClass() == "PCB_VIA"]


def key(v):
    return (int(round(v.x / 1000.0)), int(round(v.y / 1000.0)))   # 1 um


# runs of foil track per net: connected components by shared ends
runs = []
by_net = {}
for t in foil:
    by_net.setdefault(t.GetNetCode(), []).append(t)
for net, ts in by_net.items():
    if net == b.FindNet("GND").GetNetCode():
        continue
    left = list(ts)
    while left:
        comp, stack = [], [left.pop()]
        while stack:
            t = stack.pop()
            comp.append(t)
            ends = {key(t.GetStart()), key(t.GetEnd())}
            for o in list(left):
                if {key(o.GetStart()), key(o.GetEnd())} & ends:
                    left.remove(o)
                    stack.append(o)
        runs.append((net, comp))

done, tried, kept = 0, 0, 0
replaced = []    # (net, new F tracks, removed foil tracks' data, removed vias' data)
for net, comp in runs:
    pts = {}
    for t in comp:
        for q in (t.GetStart(), t.GetEnd()):
            pts[key(q)] = q
    terms = []
    for k_, q in pts.items():
        vv = [v for v in vias if v.GetNetCode() == net and v.HitTest(q, TOUCH)]
        pp = [p for p in pads_net.get(net, []) if p.GetAttribute() == pcbnew.PAD_ATTRIB_PTH and p.HitTest(q)]
        if vv or pp:
            terms.append((q, vv, pp))
    if len(terms) != 2:
        continue
    # a via that also carries other foil copper of the net is a branch: leave such runs
    ok = True
    for q, vv, pp in terms:
        for v in vv:
            others = [o for o in foil if o.GetNetCode() == net and o not in comp and
                      (v.HitTest(o.GetStart(), TOUCH + o.GetWidth() // 2) or v.HitTest(o.GetEnd(), TOUCH + o.GetWidth() // 2))]
            if others:
                ok = False
    if not ok:
        continue
    tried += 1
    full = b.FindNet(net).GetNetname()
    v_id = nid.get(full, 0)
    if v_id <= 0:
        continue
    w = max(pcbnew.ToMM(t.GetWidth()) for t in comp)
    blocked, _ = masks(v_id, w)
    run_len = sum(pcbnew.ToMM(t.GetLength()) for t in comp)

    def cells_at(q, pp):
        if pp:
            return {c for c in pad_cells(next(e for e in pads_of.get(v_id, []) if e[5].m_Uuid.AsString() == pp[0].m_Uuid.AsString())) if c[0] == B}
        i, j = int(round((pcbnew.ToMM(q.x) - OX) / G)), int(round((pcbnew.ToMM(q.y) - OY) / G))
        return {(B, j + dj, i + di) for dj in (-1, 0, 1) for di in (-1, 0, 1)}

    s_cells = {c for c in cells_at(terms[0][0], terms[0][2]) if not blocked[c]}
    t_cells = cells_at(terms[1][0], terms[1][2])
    if not s_cells or not t_cells:
        continue
    q2 = terms[1][0]
    tp = [(int(round((pcbnew.ToMM(q2.y) - OY) / G)), int(round((pcbnew.ToMM(q2.x) - OX) / G)))]
    path, n = astar(s_cells, t_cells, tp, blocked, np.zeros((NY, NX), dtype=bool), False)
    if path is None:
        continue
    plen = 0.0
    prev = None
    for k in path:
        l, r = divmod(k, NN)
        j, i = divmod(r, NX)
        if prev is not None:
            plen += math.hypot(i - prev[0], j - prev[1]) * G
        prev = (i, j)
    if plen > run_len * 2.0 + 3.0:   # a detour is no better than a jump
        kept += 1
        continue
    # the new run on the track side, then the jump and the vias that served only it go
    before = set(id(x) for x in items.get(v_id, []))
    emit(v_id, path, w)
    new = [x for x in items.get(v_id, []) if id(x) not in before]
    gone_t = [(int(t.GetStart().x), int(t.GetStart().y), int(t.GetEnd().x), int(t.GetEnd().y), t.GetWidth(), t.GetLayer())
              for t in comp]     # plain numbers: GetStart() is a live reference into the item
    gone_v = []
    for t in comp:
        b.Delete(t)
    for q, vv, pp in terms:
        for v in vv:
            gone_v.append((int(v.GetPosition().x), int(v.GetPosition().y), v.GetWidth(pcbnew.F_Cu), v.GetDrillValue()))
            b.Delete(v)
            vias.remove(v)
    for t in comp:
        foil.remove(t) if t in foil else None
    replaced.append((v_id, new, gone_t, gone_v))
    done += 1
print("dejump: %d jumps tried, %d moved to the track side, %d left (detour too long)" % (tried, done, kept))

# KiCad's check: a replacement that clashes goes back
pcbnew.SaveBoard(OUT, b)
rpt = OUT + ".drc.json"
subprocess.run([KCLI, "pcb", "drc", "--format", "json", "--severity-error", "--refill-zones", "-o", rpt, OUT], capture_output=True)
bad = set()
for v in json.load(open(rpt, encoding="utf-8"))["violations"]:
    if v["type"] in ("clearance", "shorting_items", "tracks_crossing", "hole_clearance"):
        bad.update(it.get("uuid") for it in v["items"])
back = 0
for v_id, new, gone_t, gone_v in replaced:
    if any(x.m_Uuid.AsString() in bad for x in new):
        ni = b.FindNet(names[v_id])
        for x in new:
            b.Delete(x)
        for sx, sy, ex, ey, w_, lay in gone_t:
            t = pcbnew.PCB_TRACK(b)
            t.SetStart(pcbnew.VECTOR2I(sx, sy))
            t.SetEnd(pcbnew.VECTOR2I(ex, ey))
            t.SetWidth(w_)
            t.SetLayer(lay)
            t.SetNet(ni)
            b.Add(t)
        for px_, py_, d_, dr in gone_v:
            v = pcbnew.PCB_VIA(b)
            v.SetPosition(pcbnew.VECTOR2I(px_, py_))
            v.SetWidth(d_)
            v.SetDrill(dr)
            v.SetNet(ni)
            b.Add(v)
        back += 1
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
pcbnew.SaveBoard(OUT, b)
print("put back after KiCad's check: %d; jumps removed: %d" % (back, done - back))
