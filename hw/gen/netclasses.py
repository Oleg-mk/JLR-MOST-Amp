"""Net classes in the project file: pcbnew's SaveBoard of a new board writes only Default.

python netclasses.py            (patches ../jlr_most_amp.kicad_pro)
"""
import json, os

PRO = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "jlr_most_amp.kicad_pro")

# name: (track width, clearance, patterns)
CLASSES = {
    "Power12": (0.8, 0.15, ["*+12V*", "*REM_OUT"]),
    "Supply": (0.5, 0.15, ["*+3V*", "*FOT_RXVCC", "*FOT_TXVCC", "*BUCK_SW"]),
    "Ground": (0.5, 0.15, ["GND"]),
}


def apply(pro=PRO):
    d = json.load(open(pro, encoding="utf-8"))
    ns = d["net_settings"]
    default = next(c for c in ns["classes"] if c["name"] == "Default")
    ns["classes"] = [default]
    ns["netclass_patterns"] = []
    for prio, (name, (w, clr, pats)) in enumerate(CLASSES.items()):
        c = dict(default, name=name, priority=prio, track_width=w, clearance=clr)
        ns["classes"].append(c)
        ns["netclass_patterns"] += [{"netclass": name, "pattern": p} for p in pats]
    with open(pro, "w", encoding="utf-8", newline="\n") as f:
        json.dump(d, f, ensure_ascii=False, indent=2)
        f.write("\n")


if __name__ == "__main__":
    apply()
    print("net classes:", ", ".join(CLASSES))
