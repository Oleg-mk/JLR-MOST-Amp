"""Use of the jump layer of a routed board: per net, and as separate runs (one jump is one short run).

python.exe route_stats.py <board.kicad_pcb> [F|B]   (the jump layer, default F: the ground foil)
"""
import sys
import pcbnew

b = pcbnew.LoadBoard(sys.argv[1])
JL = pcbnew.F_Cu if (len(sys.argv) < 3 or sys.argv[2] == "F") else pcbnew.B_Cu
bot, top, vias = {}, {}, {}
segs = []
for t in b.GetTracks():
    n = t.GetNetname().split("/")[-1]
    if t.GetClass() == "PCB_VIA":
        vias[n] = vias.get(n, 0) + 1
    elif t.GetLayer() == JL:
        bot[n] = bot.get(n, 0) + pcbnew.ToMM(t.GetLength())
        segs.append((n, (t.GetStart().x, t.GetStart().y), (t.GetEnd().x, t.GetEnd().y), pcbnew.ToMM(t.GetLength())))
    else:
        top[n] = top.get(n, 0) + pcbnew.ToMM(t.GetLength())

# bottom runs: segments of one net joined end to end
parent = list(range(len(segs)))


def find(i):
    while parent[i] != i:
        parent[i] = parent[parent[i]]
        i = parent[i]
    return i


ends = {}
for i, (n, a, e, _) in enumerate(segs):
    for p in (a, e):
        k = (n, p)
        if k in ends:
            parent[find(i)] = find(ends[k])
        else:
            ends[k] = i
runs = {}
for i, (n, a, e, ln) in enumerate(segs):
    r = find(i)
    runs.setdefault(r, [n, 0.0])[1] += ln
lens = sorted((ln, n) for n, ln in runs.values())

print("net                 jump mm  track mm   vias   runs")
nruns = {}
for ln, n in lens:
    nruns[n] = nruns.get(n, 0) + 1
for n in sorted(bot, key=lambda k: -bot[k]):
    print("%-18s %8.1f %8.1f %6d %6d" % (n, bot[n], top.get(n, 0), vias.get(n, 0), nruns.get(n, 0)))
print("total on the jump layer %.0f mm in %d nets; signal vias %d" % (sum(bot.values()), len(bot),
      sum(v for k, v in vias.items() if k != "GND")))
edges = (3, 5, 10, 20, 1e9)
cnt = [0] * len(edges)
for ln, _ in lens:
    cnt[next(i for i, e in enumerate(edges) if ln <= e)] += 1
print("jumps: %d; up to 3 mm %d, 3-5 %d, 5-10 %d, 10-20 %d, longer %d" % ((len(lens),) + tuple(cnt)))
print("longest jumps:", ", ".join("%s %.1f" % (n, ln) for ln, n in lens[::-1][:8]))
