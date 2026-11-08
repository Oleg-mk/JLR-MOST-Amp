"""DRC of a board by kicad-cli, summed up: violations by type, unconnected items by net.

python drc_summary.py <board.kicad_pcb>
"""
import collections, json, os, re, subprocess, sys, tempfile

KCLI = "C:/Users/geniy/AppData/Local/Programs/KiCad/10.0/bin/kicad-cli.exe"
pcb = sys.argv[1]
rpt = pcb + ".drc_summary.json"   # next to the board: runs side by side do not share it
subprocess.run([KCLI, "pcb", "drc", "--format", "json", "--severity-error", "--severity-warning", "--refill-zones",
                "-o", rpt, pcb], capture_output=True)
d = json.load(open(rpt, encoding="utf-8"))
kinds = collections.Counter(v["type"] for v in d["violations"])
print("violations:", sum(kinds.values()), dict(kinds.most_common()))
for v in d["violations"]:
    if v["type"] in ("clearance", "shorting_items", "tracks_crossing", "hole_clearance", "copper_edge_clearance"):
        print("  %s: %s" % (v["type"], " | ".join(i["description"] for i in v["items"])))
nets = collections.Counter()
for u in d["unconnected_items"]:
    m = re.search(r"\[([^\]]+)\]", u["items"][0]["description"])
    nets[m.group(1).split("/")[-1] if m else "?"] += 1
print("unconnected:", len(d["unconnected_items"]), dict(nets.most_common()))
